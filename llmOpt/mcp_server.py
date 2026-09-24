"""gengin-optimizer MCP server (stdio).

Deterministic build/bench/profile/PR tools for the llmOpt/gengin sandbox.
File editing, code search, and terminal work are provided by the driving
harness (Hermes Agent) — this server only exposes what the harness cannot do
itself: sandbox lifecycle, make/flame/bench orchestration, perf annotation,
regression bisection, and clangd semantic queries.
"""

import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

# Ensure the llmOpt directory is on the path so we can import sibling modules
_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))
if _llmOpt_dir not in sys.path:
    sys.path.insert(0, _llmOpt_dir)

from mcp.server.fastmcp import FastMCP

import getFunc as _gf
import main as _main

# All tools operate on llmOpt/gengin/ — a sandboxed copy of the renderer.
_gengin_dir = os.path.join(_llmOpt_dir, "gengin")
os.makedirs(_gengin_dir, exist_ok=True)
_main.PROJECT_DIR = _gengin_dir
_gf.init(base_dir=_gengin_dir)

mcp = FastMCP("gengin-optimizer")

# clangd needs compile_commands.json; generate it for a fresh sandbox.
_cc_json = os.path.join(_gengin_dir, "compile_commands.json")
if not os.path.exists(_cc_json):
    try:
        import contextlib
        import gen_compile_commands as _gcc
        with contextlib.redirect_stdout(sys.stderr):
            _gcc.generate(_gengin_dir)
    except Exception as e:
        print(f"[lsp] compile_commands.json generation skipped: {e}", file=sys.stderr)


# --- LSP helpers ---

def _getLspClient():
    """Lazily get or create the clangd client singleton."""
    import lsp_client as _lsp
    return _lsp.getClient(_gengin_dir)


def _symRange(sym: dict) -> dict:
    loc = sym.get("location", {})
    return sym.get("range") or loc.get("range") or sym.get("selectionRange", {})


def _symName(sym: dict) -> str:
    return sym.get("name", "?")


def _fmtLocation(uri: str, rng: dict) -> str:
    start = rng.get('start', {})
    end = rng.get('end', {})
    sl = start.get('line', 0) + 1
    el = end.get('line', 0) + 1
    fname = uri.replace('file://', '')
    return f"{fname}:{sl}" if sl == el else f"{fname}:{sl}-{el}"


def _fmtSeverity(sev: int) -> str:
    if sev == 1:
        return "ERROR"
    if sev == 2:
        return "WARN"
    if sev == 3:
        return "INFO"
    return f"SEV{sev}"


def _symbolPosition(symbol: str, rel_path: str | None = None) -> tuple[str, int, int] | None:
    """Resolve a symbol to (rel_path, line_0, char_0) via clangd, falling back
    to the getFunc index.  The file is re-indexed first because the harness may
    have edited it since the last index build."""
    funcs = _gf._functions
    structs = _gf._structs
    if rel_path is None:
        if symbol in funcs:
            rel_path = funcs[symbol]['file']
        elif symbol in structs:
            rel_path = structs[symbol]['file']
        else:
            return None

    _gf.refreshFile(rel_path)

    try:
        syms = _getLspClient().documentSymbol(rel_path)
        if syms:
            for s in syms:
                if _symName(s) == symbol:
                    start = _symRange(s).get("start", {})
                    return (rel_path, start.get("line", 0), start.get("character", 0))
    except Exception:
        pass

    funcs = _gf._functions
    structs = _gf._structs
    if symbol in funcs:
        info = funcs[symbol]
        col = info.get('sig', '').find(symbol)
        return (info['file'], info['start'] - 1, max(col, 0))
    if symbol in structs:
        info = structs[symbol]
        col = info.get('full', '').find(symbol)
        return (info['file'], info['start'] - 1, max(col, 0))
    return None


# ===================================================================
# Build & profiling
# ===================================================================

def _targetSha():
    """The commit the sandbox must be pinned to.

    In supervised sessions the supervisor sets GENGIN_TARGET_SHA so the agent
    can never rebase onto a later main commit. For manual use, fall back to
    the sandbox's current HEAD.
    """
    sha = os.environ.get("GENGIN_TARGET_SHA", "")
    if re.fullmatch(r"[0-9a-f]{40}", sha):
        return sha
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, cwd=_gengin_dir,
        ).stdout.strip()
        if re.fullmatch(r"[0-9a-f]{40}", head):
            return head
    except (subprocess.SubprocessError, OSError):
        pass
    raise RuntimeError(
        "GENGIN_TARGET_SHA is not set and the sandbox has no HEAD; "
        "cannot determine the target commit"
    )


@mcp.tool()
def git_pull_project() -> str:
    """Re-prepare the llmOpt/gengin sandbox at the pinned target commit
    (GENGIN_TARGET_SHA, or the sandbox HEAD for manual use), sync
    manifest-verified inputs, and re-index source files. The replacement is
    atomic: a failed preparation leaves the previous sandbox intact."""
    repo_url = os.environ.get("GENGIN_REPO_URL", "git@github.com:DarkBenky/gengin.git")
    branch = os.environ.get("GENGIN_TARGET_BRANCH", "main")
    target = _targetSha()
    inputs_dir = os.environ.get("GENGIN_INPUTS_DIR", "")
    old_cwd = os.getcwd()
    try:
        os.chdir(_llmOpt_dir)
        _main.PROJECT_DIR = "gengin"  # relative to llmOpt/
        _main.git_pull_project(
            repo_url, branch, target,
            inputs_dir=inputs_dir or None,
            session_id=os.environ.get("GENGIN_SESSION_ID", "manual"),
        )
        _main.PROJECT_DIR = _gengin_dir
    finally:
        os.chdir(old_cwd)
    _gf.init(base_dir=_gengin_dir)
    _main.BASELINE_RESULTS = None
    _main._clearEditStack()
    return f"Project prepared at {target} in llmOpt/gengin/ and indexed."


@mcp.tool()
def build_project() -> str:
    """Run `make clean && make` in the sandbox, after a fast `gcc -fsyntax-only`
    pass over changed .c files.  Raises on compilation failure.  Records an
    edit snapshot for bisect_regression()."""
    _main._captureEditSnapshot("edits before build_project")
    return _main.buildProject()


@mcp.tool()
def make_bench(allow_visual_change: bool = False) -> str:
    """Run `make bench` 5 times (median-aggregated) and compare against the
    baseline.  Returns JSON with `summary` (comparison; includes an
    auto-restore notice when a visual regression was detected), `bench_results`
    (scalar metrics), `visual` (per-frame SSIM/MSE when allow_visual_change)
    and `raw_stdout_preview`.  The first call establishes the baseline.

    Exact mode (default) auto-restores all sandbox changes on a significant
    MSE regression.  Set allow_visual_change=True only for a deliberate
    algorithm variation: the gate becomes SSIM (auto-restore below 0.85), the
    change is kept when frames stay similar, and the metrics feed the visual
    PR evidence.  Records an edit snapshot for bisect_regression()."""
    _SESSION_STATE["bench_calls"] += 1
    _main._captureEditSnapshot("edits before make_bench")
    result = _main.makeBench(allow_visual_change=allow_visual_change)
    results = {k: v for k, v in result["results"].items()
               if k not in ("frame_images", "frame_hashes")}
    payload = {
        "summary": result["summary"],
        "bench_results": results,
        "raw_stdout_preview": (result["stdout"] or "")[:2000],
    }
    if result.get("visual"):
        payload["visual"] = result["visual"]
    return json.dumps(payload, indent=2)


@mcp.tool()
def compare_bench_frames(label: str) -> str:
    """Compare the current bench frames against the pinned clean baseline and
    write visual evidence: full-size `before | after | diff(x4)` PNG
    composites plus metrics.json (MSE, RMSE, PSNR, SSIM, abs-diff stats)
    under screenshots/visual/<label>/ in the sandbox.

    Use after make_bench(allow_visual_change=true) for a visual PR, then pass
    the composite paths to create_pr(compareImagePaths=[...]).  Returns JSON
    with `out_dir` (sandbox-relative) and the metrics summary; `min_ssim` is
    the PR gate (>= 0.95)."""
    summary, out_dir = _main.compareBenchFrames(label)
    rel = os.path.relpath(out_dir, _main.PROJECT_DIR)
    per_frame = [
        {k: f[k] for k in ("index", "ssim", "mse", "psnr",
                           "pct_pixels_gt8", "composite")}
        for f in summary.get("per_frame", [])
    ]
    return json.dumps({
        "out_dir": rel,
        "min_ssim": summary["min_ssim"],
        "max_mse": summary["max_mse"],
        "frames": summary["frames"],
        "per_frame": per_frame,
    }, indent=2)


@mcp.tool()
def compare_images(before: str, after: str, label: str) -> str:
    """Compare two image files (BMP or PNG; sandbox-relative or absolute
    paths) and write a `before | after | diff(x4)` composite plus metrics.json
    under screenshots/visual/<label>/.

    Same metrics shape as compare_bench_frames.  Use for ad-hoc visual checks
    (e.g. tests/img screenshots); pass the composite to
    create_pr(compareImagePaths=[...]) when opening a visual PR."""
    summary, out_dir = _main.compareImages(before, after, label)
    rel = os.path.relpath(out_dir, _main.PROJECT_DIR)
    return json.dumps({"out_dir": rel, "summary": summary}, indent=2)


@mcp.tool()
def make_flame() -> dict:
    """Run `make flame` (perf record + flamegraph) and return total_samples,
    hot_functions (top 25) and hot_paths (top 20).  Prerequisite for
    hot_annotate_func / hot_annotate_file."""
    return _main.makeFlame()


@mcp.tool()
def create_pr(title: str, body: str, imageOutputChange: bool,
              branch: str = "", commit_msg: str = "",
              compareImagePaths: list[str] | None = None) -> str:
    """Commit sandbox changes, push one focused branch, and open a GitHub PR
    via the REST API (requires GITHUB_TOKEN).

    imageOutputChange is required: False for exact-match optimizations
    (screenshots/ is never staged); True for a deliberate visual change —
    then compareImagePaths must list the composite PNGs written by
    compare_bench_frames / compare_images (each needs a sibling metrics.json
    with min SSIM >= 0.95), the title is prefixed with "[visual] " and a
    Visual evidence section is appended to the body.

    Guards reject empty diffs, forbidden staged paths (logs/state/secrets),
    and non-descendant bases.  In supervised sessions the branch is derived
    automatically (llmopt/<short-sha>/<session-id>); pass it explicitly only
    to reuse an existing branch or in a manual session, e.g.
    "llmopt/<8-hex-sha>/<topic>" (7-40 hex accepted).  On credential errors
    (401/403) report `blocked`; do not hunt for other credentials.  Returns
    the PR URL."""
    return _main.createPR(title, body, branch, commit_msg or title,
                          image_output_change=imageOutputChange,
                          compare_image_paths=compareImagePaths)


@mcp.tool()
def flight_bench(steps: int = 0, capture_baseline: bool = False) -> str:
    """Run the flight-controller suite (deterministic, ~20 s) and compare it
    against the pinned baseline.  Metrics per tier - static, drift, weave, step,
    jink (moving targets that change direction and speed) - plus an aggregate:
    miss (closest-approach distance), hit rate against the hit radius, control
    effort, and median controller cost in microseconds per step.  The first
    call on a clean controller captures the baseline; pass capture_baseline=True
    to re-capture it.  `steps` overrides the rollout length (a non-default
    length has no baseline and only reports absolute numbers)."""
    return json.dumps(_main.flightBench(steps=steps, capture_baseline=capture_baseline),
                      indent=2)


@mcp.tool()
def flight_scenarios() -> str:
    """List the flight suite: settings, tier list, scenario ids, and whether a
    baseline exists for the current HEAD and suite hash."""
    return json.dumps(_main.flightScenarios(), indent=2)


@mcp.tool()
def flight_trace(scenario: str, steps: int = 0, label: str = "") -> str:
    """Run ONE flight scenario (id like "weave:2" from flight_scenarios) and
    write its trajectory to bench/results/flight_trace_<label>.csv (inside the
    sandbox, never staged), returning the metric block plus the first and last
    rows.  Use it to see how a controller change actually flies."""
    return json.dumps(_main.flightTrace(scenario, steps=steps, label=label), indent=2)


_SESSION_STATUSES = ("pr_created", "no_change", "blocked", "failed")

# A no_change that never ran a frame bench is the failure mode the proxy coach
# cannot reach (the give-up wording only appears after the report).  Refuse it
# twice, then accept, so a stubborn model cannot loop forever.
_NO_CHANGE_GUARD = os.environ.get("GENGIN_NO_CHANGE_GUARD", "1") != "0"
_NO_CHANGE_REFUSAL_CAP = int(os.environ.get("GENGIN_NO_CHANGE_GUARD_MAX", "2") or 2)
_SESSION_STATE = {"bench_calls": 0, "no_change_refusals": 0}


@mcp.tool()
def report_session_result(status: str, summary: str, pr_url: str = "") -> str:
    """Report the final session outcome to the supervisor. Call exactly once
    before exit. status must be one of: pr_created, no_change, blocked, failed.
    pr_url is required for pr_created and must be a GitHub pull URL."""
    if status not in _SESSION_STATUSES:
        return f"error: status must be one of {list(_SESSION_STATUSES)}, got {status!r}"
    if status == "pr_created" and not pr_url.startswith("https://github.com/"):
        return "error: pr_created requires a https://github.com/... pull URL"
    if status != "pr_created" and pr_url:
        return "error: pr_url is only valid with status=pr_created"

    if (status == "no_change" and _NO_CHANGE_GUARD
            and _SESSION_STATE["bench_calls"] == 0
            and _SESSION_STATE["no_change_refusals"] < _NO_CHANGE_REFUSAL_CAP):
        _SESSION_STATE["no_change_refusals"] += 1
        return (
            "error: no_change refused - this session never ran make_bench. Pick the "
            "best candidate you measured (>= 1% in run_func_bench), apply it with "
            "patch, and validate with make_bench: the frame bench (real scene, "
            "image_mse, frame hashes) is the validation a micro-bench cannot give, "
            "and applying then reverting is the normal loop. Or work the next "
            "untried row of the ## Node map in codebase_context.md. Refusal %d/%d."
            % (_SESSION_STATE["no_change_refusals"], _NO_CHANGE_REFUSAL_CAP)
        )

    result_path = os.environ.get("GENGIN_SESSION_RESULT_PATH", "")
    if not result_path:
        return "error: GENGIN_SESSION_RESULT_PATH is not set (not a supervised session)"
    payload = {
        "schemaVersion": 1,
        "status": status,
        "summary": summary[:4000],
        "prUrl": pr_url,
        "targetSha": os.environ.get("GENGIN_TARGET_SHA", ""),
        "reportedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    tmp = result_path + f".tmp.{os.getpid()}"
    with open(tmp, "w") as fh:
        json.dump(payload, fh, indent=2)
        fh.write("\n")
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, result_path)
    return f"session result recorded: {status}"


@mcp.tool()
def bisect_regression() -> str:
    """Find which recorded edit caused a benchmark regression by replaying the
    edit snapshots (recorded before build_project / make_bench) and
    re-benchmarking each.  Restores the sandbox afterwards.
    Requires a baseline (make_bench ran) and at least one edit snapshot."""
    return _main.bisectRegression()


# ===================================================================
# Micro-benchmark sandbox
# ===================================================================

@mcp.tool()
def create_func_bench(func_name: str, header_code: str, impl_code: str) -> str:
    """Create bench/<func_name>.h and bench/<func_name>.c for standalone
    micro-benchmarking.  header_code should hold the original function plus
    optimized variants; impl_code must implement main() that times each variant
    with clock_gettime, prints ns/call, and validates variants against the
    original."""
    return _main.createFuncBench(func_name, header_code, impl_code)


@mcp.tool()
def run_func_bench(func_name: str) -> str:
    """Build bench/<func_name>.c and run the binary.  Returns stdout with
    timing and validation output."""
    return _main.runFuncBench(func_name)


@mcp.tool()
def run_perf_stat(func_name: str) -> str:
    """Run `perf stat` on the bench binary (cache-misses, cycles, instructions,
    branches, branch-misses) and return parsed counters with IPC, cache-miss
    rate, branch-miss rate, and interpretation guidance.  Call after
    run_func_bench."""
    return _main.runPerfStat(func_name)


@mcp.tool()
def delete_func_bench(func_name: str) -> str:
    """Remove bench/<func_name>.h, bench/<func_name>.c and the compiled binary."""
    return _main.deleteFuncBench(func_name)


# ===================================================================
# Perf hotspot annotation
# ===================================================================

@mcp.tool()
def hot_annotate_func(func_name: str, threshold: float = 0.5) -> str:
    """Return func_name's source annotated with /* HOT X.X% */ markers on lines
    consuming >= threshold% of perf samples.  Requires perf.data from a previous
    make_flame() run."""
    return _gf.hotAnnotateFunc(func_name, threshold=threshold)


@mcp.tool()
def hot_annotate_file(rel_path: str, threshold: float = 0.5) -> str:
    """Return an entire source file annotated with per-line perf hotness merged
    across all its functions.  Requires perf.data from a previous make_flame()
    run."""
    return _gf.hotAnnotateFile(rel_path, threshold=threshold)


# ===================================================================
# LSP / clangd (read-only semantic queries)
# ===================================================================

@mcp.tool()
def lsp_definition(symbol: str, rel_path: str) -> str:
    """Go to the AST-exact definition of a symbol using clangd."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    defs = _getLspClient().definition(f, line, char)
    if defs is None:
        return "LSP: clangd not available or request failed."
    if not defs:
        return f"No definition found for '{symbol}'."
    return "\n".join([f"Definition of '{symbol}':"] +
                     [f"  {_fmtLocation(d['uri'], d['range'])}" for d in defs])


@mcp.tool()
def lsp_references(symbol: str, rel_path: str) -> str:
    """Find all semantic references to a symbol using clangd (more accurate
    than text search).  Returns file:line for every reference."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    refs = _getLspClient().references(f, line, char)
    if refs is None:
        return "LSP: clangd not available or request failed."
    if not refs:
        return f"No references found for '{symbol}'."
    return "\n".join([f"{len(refs)} reference(s) to '{symbol}':"] +
                     [f"  {_fmtLocation(r['uri'], r['range'])}" for r in refs])


@mcp.tool()
def lsp_call_hierarchy(symbol: str, rel_path: str, direction: str = "incoming") -> str:
    """Show the call hierarchy for a function: direction='incoming' lists who
    calls it, 'outgoing' lists what it calls.  Understand the blast radius
    before editing hot-path code."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    if direction not in ("incoming", "outgoing"):
        return f"direction must be 'incoming' or 'outgoing', got {direction!r}."
    f, line, char = pos
    calls = _getLspClient().callHierarchy(f, line, char, direction)
    if calls is None:
        return f"Call hierarchy not available for '{symbol}'."
    if not calls:
        label = "callers" if direction == "incoming" else "callees"
        return f"No {label} found for '{symbol}'."
    label = f"Callers of '{symbol}'" if direction == "incoming" else f"Functions called by '{symbol}'"
    lines = [f"{label} ({len(calls)}):"]
    for c in calls:
        node = c.get("from", {}) if direction == "incoming" else c.get("to", {})
        lines.append(f"  {node.get('name', '?')}  ({_fmtLocation(node.get('uri', ''), node.get('range', {}))})")
    return "\n".join(lines)


@mcp.tool()
def lsp_diagnostics(rel_path: str) -> str:
    """Compiler warnings/errors for a file via clangd publishDiagnostics
    (~1s vs ~30s for a full build).  Call before build_project."""
    diags = _getLspClient().diagnostics(rel_path)
    if diags is None:
        return "LSP: clangd not available."
    if not diags:
        return f"No diagnostics for {rel_path} — file is clean."
    lines = [f"Diagnostics for {rel_path} ({len(diags)} issue(s)):"]
    for d in sorted(diags, key=lambda x: (x.get('range', {}).get('start', {}).get('line', 0), x.get('severity', 4))):
        sev = _fmtSeverity(d.get("severity", 4))
        line = d.get("range", {}).get("start", {}).get("line", 0) + 1
        lines.append(f"  {rel_path}:{line}: [{sev}] {d.get('message', '')}")
    return "\n".join(lines)


@mcp.tool()
def lsp_diagnostics_all() -> str:
    """Diagnostics for every file clangd has parsed so far."""
    all_diags = _getLspClient().diagnosticsAll()
    if all_diags is None:
        return "No diagnostics — no files parsed yet."
    lines = [f"Diagnostics across {len(all_diags)} file(s):"]
    for fname, diags in sorted(all_diags.items()):
        fname_short = fname.replace('file://', '')
        for d in sorted(diags, key=lambda x: x.get('range', {}).get('start', {}).get('line', 0)):
            sev = _fmtSeverity(d.get("severity", 4))
            line = d.get("range", {}).get("start", {}).get("line", 0) + 1
            lines.append(f"  {fname_short}:{line}: [{sev}] {d.get('message', '')}")
    return "\n".join(lines)


if __name__ == "__main__":
    print(f"gengin-optimizer MCP server (stdio) — sandbox: {_gengin_dir}", file=sys.stderr)
    print(f"Indexed {len(_gf._functions)} functions in {len(_gf._sources)} source files.", file=sys.stderr)
    print("17 domain tools; file editing/navigation is provided by the harness.", file=sys.stderr)
    print("Configure: llmOpt/scripts/setup-hermes.sh — run: llmOpt/scripts/gengin-opt.sh", file=sys.stderr)
    mcp.run()
