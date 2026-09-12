"""gengin domain library: sandbox build/bench/profile/PR helpers.

Used by mcp_server.py.  Everything operates on PROJECT_DIR (the llmOpt/gengin
sandbox); file editing is the driving harness's job.
"""

import base64
import json
import os
import re
import subprocess
import sys

import perf as perfLib
import getFunc as gf


def _load_env(path):
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, _, v = line.partition("=")
                    os.environ.setdefault(k.strip(), v.strip())
    except FileNotFoundError:
        pass


_load_env(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

# MiniFB benches open an X window even in bench mode.  The Hermes MCP child
# gets DISPLAY from .hermes/config.yaml; this covers direct python use.
os.environ.setdefault("DISPLAY", ":2")

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None


def run(cmd, **kwargs):
    """Run a command, echo its output to stderr, raise RuntimeError on failure.

    Never writes to stdout: that channel carries the MCP stdio protocol.
    """
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)

    print(f"[{' '.join(cmd)}]", file=sys.stderr)
    print(result.stdout, file=sys.stderr)
    if result.stderr:
        print("ERR:", result.stderr, file=sys.stderr)

    if result.returncode != 0:
        output = (result.stdout or "") + (result.stderr or "")
        raise RuntimeError(f"Command failed: {' '.join(cmd)}\n{output}")

    return result


def git_pull_project():
    """Clone the gengin repo into the sandbox and sync gitignored build inputs.

    Destructive: removes the existing sandbox first.  The vendored deps
    (deps/minifb, deps/cute_headers, deps/fsr1) are gitlink placeholders in a
    fresh clone, so the parent checkout is the only source of the headers and
    the prebuilt MiniFB static lib the Makefile links against.
    """
    root = os.path.dirname(os.path.abspath(__file__))
    parent = os.path.dirname(root)
    sandbox = os.path.join(root, "gengin")

    run(["rm", "-rf", sandbox])
    run(["git", "clone", "git@github.com:DarkBenky/gengin.git", sandbox])
    # copy gitignored build inputs and assets from the parent checkout
    for rel in ("deps", "assets", ".flamegraph"):
        run(["rsync", "-a", "--ignore-missing-args", f"{parent}/{rel}/", f"{sandbox}/{rel}/"])
    run(["rsync", "-a", "--ignore-missing-args", f"{parent}/default.profdata", f"{sandbox}/default.profdata"])


def buildProject():
    """make clean + make, with a fast syntax-only pass over changed .c files first."""
    run(["make", "clean"], cwd=PROJECT_DIR)

    changed = subprocess.run(
        ["git", "diff", "--name-only"],
        capture_output=True, text=True, cwd=PROJECT_DIR,
    )
    changed_c = [f for f in changed.stdout.strip().split("\n") if f.endswith(".c")]
    if changed_c:
        try:
            run(
                ["gcc", "-fsyntax-only", "-I.", "-Iobject", "-I/usr/local/include"]
                + changed_c,
                cwd=PROJECT_DIR,
            )
        except RuntimeError as e:
            raise RuntimeError(f"Syntax check failed — fix errors before rebuilding.\n{e}")

    return run(["make"], cwd=PROJECT_DIR).stdout


METRIC_NOISE_PCT = 1.0  # run-to-run noise floor; deltas below this are not signal


def _benchSummary(current, baseline):
    if baseline is None:
        return "No baseline to compare against."
    metrics = ["frames", "avg_ms", "median_ms", "p99_ms"]
    lines = ["Benchmark comparison vs baseline:"]
    improved = 0
    regressed = 0
    for m in metrics:
        b = baseline.get(m)
        c = current.get(m)
        if b is None or c is None:
            continue
        # for frame count higher is better; for latency lower is better
        if m == "frames":
            delta = (c - b) / b * 100
        else:
            delta = (b - c) / b * 100  # positive = improvement
        if abs(delta) <= METRIC_NOISE_PCT:
            arrow = "unchanged (within noise)"
        elif delta > 0:
            arrow = "IMPROVED"
            improved += 1
        else:
            arrow = "REGRESSED"
            regressed += 1
        lines.append(f"  {m:12s}  baseline={b}  now={c}  ({delta:+.1f}%)  [{arrow}]")

    base_hashes = baseline.get("frame_hashes") or []
    curr_hashes = current.get("frame_hashes") or []
    total = max(len(base_hashes), len(curr_hashes))
    if total == 0:
        lines.append("  frame_hashes  no hash data available")
    else:
        changed = sum(1 for b, c in zip(base_hashes, curr_hashes) if b != c)
        changed += abs(len(base_hashes) - len(curr_hashes))
        if changed == 0:
            lines.append(f"  frame_hashes  all {total} frame(s) match baseline (informational only)")
        else:
            lines.append(f"  frame_hashes  {changed}/{total} frame(s) differ (informational only -- use MSE below)")

    base_imgs = baseline.get("frame_images") or []
    curr_imgs = current.get("frame_images") or []
    mse_values = []
    for b64_b, b64_c in zip(base_imgs, curr_imgs):
        if not b64_b or not b64_c:
            continue
        try:
            raw_b = base64.b64decode(b64_b)
            raw_c = base64.b64decode(b64_c)
            n = min(len(raw_b), len(raw_c))
            if n == 0:
                continue
            # sample every 64th byte to keep this fast without numpy
            step = 64
            mse = sum((raw_b[i] - raw_c[i]) ** 2 for i in range(0, n, step)) / (n // step)
            mse_values.append(mse)
        except Exception:
            pass
    if mse_values:
        avg_mse = sum(mse_values) / len(mse_values)
        max_mse = max(mse_values)
        if avg_mse < 2.5:
            verdict = "VISUALLY IDENTICAL"
        elif avg_mse < 10.0:
            verdict = "MINOR DIFFERENCE (acceptable)"
        elif avg_mse < 100.0:
            verdict = "NOTICEABLE DIFFERENCE"
        else:
            verdict = "SIGNIFICANT VISUAL CHANGE"
        lines.append(f"  image_mse     avg={avg_mse:.2f}  max={max_mse:.2f}  [{verdict}]")
    else:
        lines.append("  image_mse     no image data available")

    if improved > regressed:
        lines.append("=> OVERALL: PERFORMANCE IMPROVED")
    elif regressed > improved:
        lines.append("=> OVERALL: PERFORMANCE REGRESSED -- revert the change (git checkout -- .)")
    else:
        lines.append("=> OVERALL: no significant change")
    return "\n".join(lines)


BENCH_RUNS = 5  # number of bench runs to median-aggregate


def _median(values):
    s = sorted(v for v in values if v is not None)
    if not s:
        return None
    mid = len(s) // 2
    return s[mid] if len(s) & 1 else (s[mid - 1] + s[mid]) * 0.5


def makeBench():
    """Run `make bench` BENCH_RUNS times, median-aggregate, compare vs baseline.

    Establishes the baseline on the first run.  Auto-restores the sandbox when
    a significant visual regression is detected (max MSE >= 50 or avg >= 30),
    so a change that broke rendering is never left applied.

    Returns {summary: str, results: dict, stdout: str}.
    """
    global BASELINE_RESULTS

    scalar_keys = ["frames", "avg_ms", "median_ms", "p99_ms"]
    runs = []
    last_stdout = ""
    for _ in range(BENCH_RUNS):
        run(["make", "clean"], cwd=PROJECT_DIR)
        res = run(["make", "bench"], cwd=PROJECT_DIR)
        last_stdout = res.stdout
        with open(f"{PROJECT_DIR}/bench/results/bench_results.json") as f:
            runs.append(json.load(f))

    # median over scalar metrics; keep visual data from the middle run
    mid_run = runs[len(runs) // 2]
    bench_results_raw = mid_run.copy()
    for k in scalar_keys:
        vals = [r.get(k) for r in runs if r.get(k) is not None]
        if vals:
            bench_results_raw[k] = _median(vals)

    if BASELINE_RESULTS is None:
        BASELINE_RESULTS = _loadBaselineCache()

    if BASELINE_RESULTS is None:
        BASELINE_RESULTS = bench_results_raw
        _saveBaselineCache(bench_results_raw)
        summary = ("Baseline established from this run — the next make_bench "
                   "call will compare against it.")
    else:
        summary = _benchSummary(bench_results_raw, BASELINE_RESULTS)

        base_imgs = BASELINE_RESULTS.get("frame_images") or []
        curr_imgs = bench_results_raw.get("frame_images") or []
        mse_values = []
        for b64_b, b64_c in zip(base_imgs, curr_imgs):
            if not b64_b or not b64_c:
                continue
            try:
                raw_b = base64.b64decode(b64_b)
                raw_c = base64.b64decode(b64_c)
                n = min(len(raw_b), len(raw_c))
                if n == 0:
                    continue
                step = 64
                mse = sum((raw_b[i] - raw_c[i]) ** 2 for i in range(0, n, step)) / (n // step)
                mse_values.append(mse)
            except Exception:
                pass
        if mse_values and (max(mse_values) >= 50.0 or (sum(mse_values) / len(mse_values)) >= 30.0):
            gf.restoreAll()
            _clearEditStack()
            summary += (
                "\n\n*** AUTO-RESTORED: Significant visual regression detected "
                f"(max MSE={max(mse_values):.1f}). All changes have been reverted. "
                "The previous change likely broke rendering. Read the code more "
                "carefully before re-applying. ***"
            )

    return {"summary": summary, "results": bench_results_raw, "stdout": last_stdout}


def makeFlame():
    """Run make flame and return parsed perf data (hot_functions / hot_paths)."""
    run(["make", "flame"], cwd=PROJECT_DIR)
    return perfLib.getPerfData(cwd=PROJECT_DIR)


# ---------------------------------------------------------------------------
# baseline cache — skip the cold-start bench when project code is unchanged
# ---------------------------------------------------------------------------

BASELINE_CACHE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline_cache.json")


def _projectGitHead():
    """Return (head_hash, is_dirty) for PROJECT_DIR, or (None, True) on failure."""
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, cwd=PROJECT_DIR,
        )
        if head.returncode != 0:
            return None, True
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no",
             "--ignore-submodules=dirty"],
            capture_output=True, text=True, cwd=PROJECT_DIR,
        )
        return head.stdout.strip(), bool(dirty.stdout.strip())
    except Exception:
        return None, True


def _loadBaselineCache():
    """Return the cached baseline if it matches the current clean HEAD."""
    if not os.path.exists(BASELINE_CACHE_FILE):
        return None
    head, dirty = _projectGitHead()
    if head is None or dirty:
        return None  # uncommitted changes — cache cannot be trusted
    try:
        with open(BASELINE_CACHE_FILE) as fh:
            cache = json.load(fh)
    except (json.JSONDecodeError, OSError):
        return None
    if cache.get("head") != head:
        return None
    return cache.get("baseline")


def _saveBaselineCache(baseline):
    """Persist the baseline keyed by the current clean HEAD."""
    head, dirty = _projectGitHead()
    if head is None or dirty:
        return  # don't cache a baseline taken against a dirty tree
    try:
        with open(BASELINE_CACHE_FILE, "w") as fh:
            json.dump({"head": head, "baseline": baseline}, fh)
    except OSError as e:
        print(f"[baseline-cache] save failed: {e}", file=sys.stderr)


BENCH_FUNC_DIR = os.path.join(PROJECT_DIR, "bench")


def createFuncBench(func_name: str, header_code: str, impl_code: str):
    """Write bench/<func_name>.h and bench/<func_name>.c for a micro-benchmark.

    header_code holds the original function plus optimized variants;
    impl_code must implement main() that times every variant with
    clock_gettime, prints ns/call, and validates variants against the original.
    """
    os.makedirs(BENCH_FUNC_DIR, exist_ok=True)
    h_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.h")
    c_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.c")
    with open(h_path, "w") as fh:
        fh.write(header_code)
    with open(c_path, "w") as fh:
        fh.write(impl_code)
    return f"Created bench/{func_name}.h and bench/{func_name}.c"


def runFuncBench(func_name: str):
    """Build bench/<func_name>.c and run the resulting binary.

    The Makefile's `benchFunc <name>` form also treats the bare <name> as a test
    goal, which errors after the bench has already run — so build and run the
    binary directly instead.
    """
    binary = os.path.join("build", "bench", func_name)
    try:
        run(["make", binary], cwd=PROJECT_DIR)
        return run([binary], cwd=PROJECT_DIR).stdout
    except RuntimeError as e:
        return str(e)


def deleteFuncBench(func_name: str):
    """Remove bench/<func_name>.h, bench/<func_name>.c and the compiled binary."""
    removed = []
    for path in (
        os.path.join(BENCH_FUNC_DIR, f"{func_name}.h"),
        os.path.join(BENCH_FUNC_DIR, f"{func_name}.c"),
        os.path.join(PROJECT_DIR, "build", "bench", func_name),
    ):
        if os.path.exists(path):
            os.remove(path)
            removed.append(os.path.relpath(path, PROJECT_DIR))
    return f"Removed: {', '.join(removed)}" if removed else f"No files found for bench/{func_name}"


def runPerfStat(func_name: str):
    """Run `perf stat` on a bench binary and return parsed counters + guidance.

    Call after runFuncBench() to see whether an optimization traded
    instructions for cache misses — the usual cause of a micro-bench win that
    regresses in the multi-threaded renderer.
    """
    bench_bin = os.path.join(PROJECT_DIR, "build", "bench", func_name)
    if not os.path.exists(bench_bin):
        return f"Bench binary not found: build/bench/{func_name}. Run run_func_bench() first."

    events = "cache-misses,cycles,instructions,branches,branch-misses"
    try:
        result = subprocess.run(
            ["perf", "stat", "-e", events, bench_bin],
            capture_output=True, text=True, cwd=PROJECT_DIR, timeout=60,
        )
        # perf stat writes to stderr
        raw = result.stderr.strip()
        if not raw:
            raw = result.stdout.strip()
    except subprocess.TimeoutExpired:
        return f"perf stat timed out on bench/{func_name}"
    except FileNotFoundError:
        return "perf not found — install linux-tools package"

    parsed = {}
    for line in raw.splitlines():
        # lines look like: "  1,234,567      cache-misses"
        m = re.match(r"\s*([\d,]+)\s+(\S+)", line)
        if m:
            parsed[m.group(2)] = int(m.group(1).replace(",", ""))

    if not parsed:
        hint = ""
        if "permission" in raw.lower() or "paranoid" in raw.lower() or "perfmon" in raw.lower():
            hint = ("\n(hint: perf is blocked by kernel.perf_event_paranoid; run "
                    "llmOpt/scripts/enable-perf.sh once (needs sudo) to enable counters)")
        return f"perf stat produced no parseable counters:\n{raw[:500]}{hint}"

    cycles = parsed.get("cycles", 1)
    instructions = parsed.get("instructions", 1)
    cache_misses = parsed.get("cache-misses", 0)
    branches = parsed.get("branches", 0)
    branch_misses = parsed.get("branch-misses", 0)

    ipc = instructions / cycles if cycles > 0 else 0
    cache_miss_pct = (cache_misses / instructions * 100) if instructions > 0 else 0
    branch_miss_pct = (branch_misses / branches * 100) if branches > 0 else 0

    return "\n".join([
        f"perf stat results for bench/{func_name}:",
        f"  {instructions:>12,}  instructions",
        f"  {cycles:>12,}  cycles  (IPC = {ipc:.2f})",
        f"  {cache_misses:>12,}  cache-misses  ({cache_miss_pct:.2f}% of instructions)",
        f"  {branches:>12,}  branches",
        f"  {branch_misses:>12,}  branch-misses  ({branch_miss_pct:.2f}% mispredicted)",
        "",
        "INTERPRETATION GUIDE:",
        f"  IPC: {ipc:.2f} — {'GOOD (>1.5)' if ipc > 1.5 else 'OK (0.7-1.5)' if ipc > 0.7 else 'LOW (<0.7) — likely memory-bound or branch-heavy'}",
        f"  Cache-miss rate: {cache_miss_pct:.2f}% — {'HIGH (>1%) — memory pressure is significant' if cache_miss_pct > 1.0 else 'LOW (<1%) — cache-friendly'}",
        f"  Branch-miss rate: {branch_miss_pct:.2f}% — {'HIGH (>5%) — unpredictable branches' if branch_miss_pct > 5.0 else 'LOW (<5%) — predictable'}",
        "",
        "If comparing two runs: increased instructions with LOWER IPC = compiler bloated code.",
        "Decreased instructions with HIGHER cache-miss rate = you saved CPU but hurt memory.",
        "This is the EXACT tradeoff that causes micro-bench wins to regress in the real renderer.",
    ])


# ---------------------------------------------------------------------------
# regression bisection
# ---------------------------------------------------------------------------

# stack of (description, git-diff patch) snapshots captured before each
# build/bench validation cycle
_edit_stack = []


def _captureEditSnapshot(description=""):
    """Record the current sandbox diff as a named snapshot on the edit stack."""
    result = subprocess.run(
        ["git", "diff", "HEAD"],
        capture_output=True, text=True, cwd=PROJECT_DIR,
    )
    diff = result.stdout.strip()
    if not diff:
        return
    if _edit_stack and _edit_stack[-1][1] == diff:
        return  # same state as the previous snapshot
    _edit_stack.append((description, diff))


def _clearEditStack():
    _edit_stack.clear()


def bisectRegression():
    """Find which recorded edit caused a benchmark regression.

    Replays the edit snapshots (captured before build_project / make_bench),
    re-benchmarking after each, then restores the sandbox to its pre-bisect
    state.  Linear search for <= 4 edits, binary split otherwise.

    Call after make_bench reports a regression and before reverting anything.
    """
    if not _edit_stack:
        return "No edit snapshots available. Edits may have been made before tracking started."
    if BASELINE_RESULTS is None:
        return "No baseline available — run make_bench first, then bisect after a regression."

    n = len(_edit_stack)
    if n == 1:
        desc, _ = _edit_stack[0]
        _clearEditStack()
        return (f"Only 1 edit was made: '{desc}'. This is the cause of the regression. "
                "Revert it and try a different approach.")

    # Save current state so we can return to it afterwards
    subprocess.run(["git", "stash", "push", "-m", "bisect-pre-revert"],
                   capture_output=True, text=True, cwd=PROJECT_DIR)
    gf.restoreAll()

    culprit = None
    tested = []

    if n <= 4:
        for i, (desc, patch) in enumerate(_edit_stack):
            proc = subprocess.run(
                ["git", "apply"],
                input=patch, capture_output=True, text=True, cwd=PROJECT_DIR,
            )
            if proc.returncode != 0:
                tested.append(f"  [{i+1}] '{desc}' — could not apply in isolation (depends on other edits)")
                subprocess.run(["git", "checkout", "--", "."],
                               capture_output=True, cwd=PROJECT_DIR)
                continue

            build_proc = subprocess.run(
                ["make"], capture_output=True, text=True, cwd=PROJECT_DIR,
            )
            if build_proc.returncode != 0:
                tested.append(f"  [{i+1}] '{desc}' — BUILD FAILED (this edit alone breaks compilation)")
                culprit = (i, desc, "build failure")
                break

            try:
                bench_summary = makeBench()["summary"]
                if "REGRESSED" in bench_summary:
                    tested.append(f"  [{i+1}] '{desc}' — REGRESSION CONFIRMED")
                    culprit = (i, desc, "performance regression")
                    break
                tested.append(f"  [{i+1}] '{desc}' — OK (no regression)")
            except Exception as e:
                tested.append(f"  [{i+1}] '{desc}' — BENCH FAILED: {e}")
                culprit = (i, desc, f"bench error: {e}")
                break

            subprocess.run(["git", "checkout", "--", "."],
                           capture_output=True, cwd=PROJECT_DIR)
    else:
        mid = n // 2
        first_half = _edit_stack[:mid]
        second_half = _edit_stack[mid:]
        culprit_desc = "unknown"

        for _, patch in first_half:
            subprocess.run(["git", "apply"], input=patch, capture_output=True,
                           text=True, cwd=PROJECT_DIR)
        try:
            if "REGRESSED" in makeBench()["summary"]:
                tested.append(f"  First {mid} edits — REGRESSION (culprit in this group)")
                culprit_desc = f"some edit in the first {mid} edits"
            else:
                tested.append(f"  First {mid} edits — OK")
                gf.restoreAll()
                for _, patch in second_half:
                    subprocess.run(["git", "apply"], input=patch, capture_output=True,
                                   text=True, cwd=PROJECT_DIR)
                if "REGRESSED" in makeBench()["summary"]:
                    culprit_desc = f"some edit in the last {n - mid} edits"
                else:
                    culprit_desc = "interaction between edits (neither half alone regresses)"
        except Exception as e:
            culprit_desc = f"bench error during bisection: {e}"

        gf.restoreAll()
        culprit = (None, culprit_desc, "binary search result")

    lines = [f"BISECTION RESULT ({n} edit(s) tested):"]
    lines.extend(tested)
    if culprit:
        idx, desc, reason = culprit
        if idx is not None:
            lines.append(f"\nCULPRIT: Edit [{idx+1}] '{desc}' — {reason}.")
        else:
            lines.append(f"\nCULPRIT: {desc} — {reason}.")
        lines.append("Revert this edit and try a different approach.")
    else:
        lines.append("\nNo single edit caused regression — may be an interaction effect.")
        lines.append("Try applying fewer edits at once, or test combinations.")

    # Restore the pre-bisect state from the stash
    subprocess.run(["git", "checkout", "--", "."], capture_output=True, cwd=PROJECT_DIR)
    subprocess.run(["git", "stash", "pop"], capture_output=True, text=True, cwd=PROJECT_DIR)

    _clearEditStack()
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# git / GitHub
# ---------------------------------------------------------------------------

def _github_create_pr(title, body, head, base="main"):
    import urllib.request, json as _json, re as _re
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        raise RuntimeError("GITHUB_TOKEN not set in environment or .env")
    res = subprocess.run(
        ["git", "remote", "get-url", "origin"],
        capture_output=True, text=True, cwd=PROJECT_DIR,
    )
    remote = res.stdout.strip()
    m = _re.search(r'[:/]([^/]+/[^/]+?)(?:\.git)?$', remote)
    if not m:
        raise RuntimeError(f"Cannot parse repo from remote URL: {remote}")
    repo = m.group(1)
    payload = _json.dumps({"title": title, "body": body, "head": head, "base": base}).encode()
    req = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/pulls",
        data=payload,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(req) as r:
        data = _json.loads(r.read())
    return data["html_url"]


def createPR(title, body, branch, commit_msg=None):
    """Commit sandbox changes, push a branch, open a PR.  Returns the PR URL."""
    commit_msg = commit_msg or title
    run(["git", "checkout", "-b", branch], cwd=PROJECT_DIR)
    run(["git", "add", "-A"], cwd=PROJECT_DIR)
    run(["git", "commit", "-m", commit_msg], cwd=PROJECT_DIR)
    run(["git", "push", "-u", "origin", branch], cwd=PROJECT_DIR)
    url = _github_create_pr(title, body, head=branch)
    print(f"PR created: {url}", file=sys.stderr)
    return url
