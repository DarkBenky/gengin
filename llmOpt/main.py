# git_pull()
# build(target)                    // make, make bench, make loss, etc.
# run_perf(duration)               // perf record → flamegraph SVG + folded data
# read_file(path)
# list_files(path)
# write_file(path, content)        // model writes proposed patch
# apply_patch(patch)               // git apply
# reset()                          // git checkout -- .
# create_pr(title, body, branch)


import base64
import os
import re
import subprocess

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
from pprint import pprint
import json
import perf as perfLib
import getFunc as gf
import planner
import executor
import modelSelector as model
import sys
import ui
import random
import refine

CONTEXT = []

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None
PINNED_HOTSPOTS = ""  # top hot functions + baseline, survives context trimming

CONTEXT_MAX_TOKENS = 128_000
CONTEXT_COMPRESS_AT = 0.85  # trigger compression when > 85% full

def run(cmd, **kwargs):
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)

    print(f"[{' '.join(cmd)}]")
    print(result.stdout)

    if result.stderr:
        print("ERR:", result.stderr)

    if result.returncode != 0:
        output = (result.stdout or "") + (result.stderr or "")
        raise RuntimeError(f"Command failed: {' '.join(cmd)}\n{output}")
    
    print(f"Command succeeded: {' '.join(cmd)}")

    return result

def git_pull_project():
    run(["rm", "-rf", "gengin"])
    run(["git", "clone", "git@github.com:DarkBenky/gengin.git"])
    run(["sudo", "apt", "update", "-y"])
    run(["sudo", "apt", "install", "tree", "-y"])
    # copy gitignored assets and profdata from parent repo
    run(["rsync", "-a", "../assets/", "gengin/assets/"])
    run(["cp", "../default.profdata", "gengin/default.profdata"])
    # copy flamegraph tools if available in parent
    run(["rsync", "-a", "--ignore-missing-args", "../.flamegraph/", "gengin/.flamegraph/"])

def getTree(path: str = PROJECT_DIR):
    res = run(["tree", "--charset=ascii"], cwd=path)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getTree",
        "input": path,
        "output": res.stdout
    })
    return res.stdout

def getTodos(path: str = "."):
    result = subprocess.run(
        ["grep", "-r", "--binary-files=without-match", "--exclude-dir=.git", "--exclude=perf.data", "TODO", path],
        capture_output=True, text=True, cwd=PROJECT_DIR
    )
    if result.returncode == 2:
        raise RuntimeError(f"grep failed: {result.stderr}")
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getTodos",
        "input": path,
        "output": result.stdout
    })
    return result.stdout

def buildProject():
    _res = run(["make", "clean"], cwd=PROJECT_DIR)

    # Fast syntax check on changed .c files — catches 90% of errors
    # in <1 second vs a full 30-second build cycle.
    changed = subprocess.run(
        ["git", "diff", "--name-only"],
        capture_output=True, text=True, cwd=PROJECT_DIR
    )
    changed_c = [f for f in changed.stdout.strip().split("\n") if f.endswith(".c")]
    if changed_c:
        try:
            run(
                ["gcc", "-fsyntax-only", "-I.", "-Iobject", "-I/usr/local/include"]
                + changed_c,
                cwd=PROJECT_DIR,
            )
        except RuntimeError:
            # Syntax error — report and stop early, don't waste time on make
            CONTEXT.append({
                "type": "tool_use",
                "tool": "buildProject",
                "input": None,
                "output": "SYNTAX CHECK FAILED — fix errors above before rebuilding.",
            })
            raise

        # Auto-review the diff with the reviewer model BEFORE the ~30s build,
        # but only once per unique diff so we never pay for a duplicate review.
        review = _autoReviewBeforeBuild()
        if review:
            print(f"[buildProject] auto-review: {review.splitlines()[0]}")

    res = run(["make"], cwd=PROJECT_DIR)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "buildProject",
        "input": None,
        "output": _res.stdout + res.stdout 
    })
    return res.stdout

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
        higher_is_better = (m == "frames")
        if higher_is_better:
            delta = (c - b) / b * 100
            better = delta > 0
        else:
            delta = (b - c) / b * 100  # positive = improvement
            better = delta > 0
        arrow = "IMPROVED" if better else ("REGRESSED" if delta < 0 else "unchanged")
        if better:
            improved += 1
        elif delta < 0:
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

    # MSE-based image similarity — primary visual correctness metric
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
            # sample every 64 bytes to keep this fast without numpy
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
        lines.append("=> OVERALL: PERFORMANCE REGRESSED -- consider restoreAll()")
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
    scalar_keys = ["frames", "avg_ms", "median_ms", "p99_ms"]
    runs = []
    last_stdout = ""
    for _ in range(BENCH_RUNS):
        _res = run(["make", "clean"], cwd=PROJECT_DIR)
        res = run(["make", "bench"], cwd=PROJECT_DIR)
        last_stdout = res.stdout
        with open(f"{PROJECT_DIR}/bench_results.json", "r") as f:
            runs.append(json.load(f))

    # median over scalar metrics; keep visual data from the middle run
    mid_run = runs[len(runs) // 2]
    bench_results_raw = mid_run.copy()
    for k in scalar_keys:
        vals = [r.get(k) for r in runs if r.get(k) is not None]
        if vals:
            bench_results_raw[k] = _median(vals)

    bench_results = {k: v for k, v in bench_results_raw.items()
                     if k not in ("frame_images", "frame_hashes")}
    summary = _benchSummary(bench_results_raw, BASELINE_RESULTS)

    # Auto-restore on significant visual regression (safety net).
    # Only fires when there IS a baseline to compare against (not the init run).
    if BASELINE_RESULTS is not None:
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
            summary += (
                "\n\n*** AUTO-RESTORED: Significant visual regression detected "
                f"(max MSE={max(mse_values):.1f}). All changes have been reverted. "
                "The previous change likely broke rendering. Read the code more "
                "carefully before re-applying. ***"
            )
            planner.addNote(f"[AUTO-RESTORE] Visual regression (max MSE={max(mse_values):.1f}) — changes reverted.")

    record = {
        "type": "tool_use",
        "tool": "makeBench",
        "input": None,
        "output": summary,
        "bench_results": bench_results
    }
    CONTEXT.append(record)
    return last_stdout, bench_results_raw

def makeFlame():
    global PINNED_HOTSPOTS
    run(["make", "flame"], cwd=PROJECT_DIR)
    data = perfLib.getPerfData(cwd=PROJECT_DIR)
    # Refresh pinned hotspots so the model sees the latest profile
    if data and data.get("hot_functions"):
        top5 = data["hot_functions"][:5]
        PINNED_HOTSPOTS = (
            "Top hotspots (always visible):\n" +
            "\n".join(
                f"  {h['fn']:40s}  incl={h['inclusive_pct']:5.1f}%  excl={h['exclusive_pct']:5.1f}%"
                for h in top5
            )
        )
        # Auto-initialize or refresh refinement state for current top hotspots
        for h in top5:
            refine.initRefinement(h["fn"], hotspotData=h)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "makeFlame",
        "input": None,
        "output": data
    })
    return data


# ---------------------------------------------------------------------------
# baseline cache — skip the cold-start bench+flame when project code is unchanged
# ---------------------------------------------------------------------------

BASELINE_CACHE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline_cache.json")


def _projectGitHead():
    """Return (head_hash, is_dirty) for PROJECT_DIR, or (None, True) on failure."""
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, cwd=PROJECT_DIR
        )
        if head.returncode != 0:
            return None, True
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            capture_output=True, text=True, cwd=PROJECT_DIR
        )
        return head.stdout.strip(), bool(dirty.stdout.strip())
    except Exception:
        return None, True


def _loadBaselineCache():
    """Return cached (baseline, flame) if the cache matches the current clean HEAD."""
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
    return cache.get("baseline"), cache.get("flame")


def _saveBaselineCache(baseline, flame):
    """Persist the baseline + flame keyed by the current clean HEAD."""
    head, dirty = _projectGitHead()
    if head is None or dirty:
        return  # don't cache a baseline taken against a dirty tree
    try:
        with open(BASELINE_CACHE_FILE, "w") as fh:
            json.dump({"head": head, "baseline": baseline, "flame": flame}, fh)
    except OSError as e:
        print(f"[baseline-cache] save failed: {e}")


def _applyFlameHotspots(flame):
    """Pin top-5 hotspots and seed refinement state from a flame dict. Returns the header string."""
    global PINNED_HOTSPOTS
    if not (flame and flame.get("hot_functions")):
        return PINNED_HOTSPOTS
    top5 = flame["hot_functions"][:5]
    PINNED_HOTSPOTS = (
        "Top hotspots (always visible):\n" +
        "\n".join(
            f"  {h['fn']:40s}  incl={h['inclusive_pct']:5.1f}%  excl={h['exclusive_pct']:5.1f}%"
            for h in top5
        )
    )
    for h in top5:
        refine.initRefinement(h["fn"], hotspotData=h)
    return PINNED_HOTSPOTS


BENCH_FUNC_DIR = os.path.join(PROJECT_DIR, "bench")

def createFuncBench(func_name: str, header_code: str, impl_code: str):
    """
    Create bench/<func_name>.h and bench/<func_name>.c so the model can compile
    and time a standalone micro-benchmark with `runFuncBench(func_name)`.

    header_code  - full content of the .h file (guards, typedefs, declarations).
    impl_code    - full content of the .c file; must contain a main() that uses
                   ComputePerformanceMetrics() from tests/timings.h for timing and
                   prints results to stdout.
    """
    os.makedirs(BENCH_FUNC_DIR, exist_ok=True)
    h_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.h")
    c_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.c")
    with open(h_path, "w") as fh:
        fh.write(header_code)
    with open(c_path, "w") as fh:
        fh.write(impl_code)
    msg = f"Created bench/{func_name}.h and bench/{func_name}.c"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "createFuncBench",
        "input": func_name,
        "output": msg,
    })
    return msg


def runFuncBench(func_name: str):
    """
    Build and run the micro-benchmark for func_name via `make benchFunc <func_name>`.
    Returns the stdout output (timing results and validation).
    """
    try:
        res = run(["make", "benchFunc", func_name], cwd=PROJECT_DIR)
        output = res.stdout
    except RuntimeError as e:
        output = str(e)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "runFuncBench",
        "input": func_name,
        "output": output,
    })
    return output


def deleteFuncBench(func_name: str):
    """Remove bench/<func_name>.h, bench/<func_name>.c and the compiled binary."""
    removed = []
    for ext in (".h", ".c", ""):
        path = os.path.join(BENCH_FUNC_DIR, f"{func_name}{ext}")
        if os.path.exists(path):
            os.remove(path)
            removed.append(f"bench/{func_name}{ext}")
    msg = f"Removed: {', '.join(removed)}" if removed else f"No files found for bench/{func_name}"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "deleteFuncBench",
        "input": func_name,
        "output": msg,
    })
    return msg


# ---------------------------------------------------------------------------
# perf stat integration — gives the pre-mortem real cache/memory data
# ---------------------------------------------------------------------------

def runPerfStat(func_name: str):
    """
    Run `perf stat` on a micro-benchmark binary to collect hardware counter data:
    cache-misses, cycles, instructions, branches, branch-misses.

    Call this AFTER runFuncBench() to see whether your optimization traded
    instructions for cache misses — the #1 cause of micro-bench wins that
    regress in the multi-threaded renderer.

    Returns a dict with parsed counters, and injects a human-readable summary
    into the context.
    """
    bench_bin = os.path.join(PROJECT_DIR, "bench", func_name)
    if not os.path.exists(bench_bin):
        msg = f"Bench binary not found: bench/{func_name}. Run runFuncBench() first."
        CONTEXT.append({"type": "tool_use", "tool": "runPerfStat",
                        "input": func_name, "output": msg})
        return msg

    events = "cache-misses,cycles,instructions,branches,branch-misses"
    try:
        result = subprocess.run(
            ["perf", "stat", "-e", events, bench_bin],
            capture_output=True, text=True, cwd=PROJECT_DIR, timeout=60
        )
        # perf stat writes to stderr
        raw = result.stderr.strip()
        if not raw:
            raw = result.stdout.strip()
    except subprocess.TimeoutExpired:
        msg = f"perf stat timed out on bench/{func_name}"
        CONTEXT.append({"type": "tool_use", "tool": "runPerfStat",
                        "input": func_name, "output": msg})
        return msg
    except FileNotFoundError:
        msg = "perf not found — install linux-tools package"
        CONTEXT.append({"type": "tool_use", "tool": "runPerfStat",
                        "input": func_name, "output": msg})
        return msg

    # Parse perf stat output
    parsed = {}
    import re as _re
    for line in raw.splitlines():
        # Lines look like: "  1,234,567      cache-misses"
        m = _re.match(r'\s*([\d,]+)\s+(\S+)', line)
        if m:
            value = int(m.group(1).replace(',', ''))
            name = m.group(2)
            parsed[name] = value

    if not parsed:
        msg = f"perf stat produced no parseable counters:\n{raw[:500]}"
        CONTEXT.append({"type": "tool_use", "tool": "runPerfStat",
                        "input": func_name, "output": msg})
        return msg

    # Compute derived metrics
    cycles = parsed.get("cycles", 1)
    instructions = parsed.get("instructions", 1)
    cache_misses = parsed.get("cache-misses", 0)
    branches = parsed.get("branches", 0)
    branch_misses = parsed.get("branch-misses", 0)

    ipc = instructions / cycles if cycles > 0 else 0
    cache_miss_pct = (cache_misses / instructions * 100) if instructions > 0 else 0
    branch_miss_pct = (branch_misses / branches * 100) if branches > 0 else 0

    summary_lines = [
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
    ]
    output = "\n".join(summary_lines)

    CONTEXT.append({
        "type": "tool_use",
        "tool": "runPerfStat",
        "input": func_name,
        "output": output,
        "perf_counters": parsed,
    })
    return output


# ---------------------------------------------------------------------------
# automatic bisection on regression
# ---------------------------------------------------------------------------

# Stack of (description, patch_content) captured after each successful edit
_edit_stack = []

def _captureEditSnapshot(description=""):
    """Capture the current git diff as a named snapshot on the edit stack."""
    result = subprocess.run(
        ["git", "diff", "HEAD"],
        capture_output=True, text=True, cwd=PROJECT_DIR
    )
    diff = result.stdout.strip()
    if not diff:
        return  # no changes to capture
    if _edit_stack and _edit_stack[-1][1] == diff:
        return  # duplicate — same diff as last capture
    _edit_stack.append((description, diff))


def _clearEditStack():
    _edit_stack.clear()


def bisectRegression():
    """
    When makeBench shows a regression, try to identify which specific edit
    caused it by applying patches incrementally and re-benchmarking.

    Strategy:
      - If 1 edit was made: that's the culprit, report it directly.
      - If 2-4 edits: apply one at a time, test each.
      - If 5+ edits: binary search (apply first half, test, narrow down).

    After identifying the culprit, restores all changes and reports findings.
    Call this instead of restoreAll() when you want precise feedback.
    """
    if not _edit_stack:
        return "No edit snapshots available. Edits may have been made before tracking started."

    n = len(_edit_stack)
    if n == 1:
        desc, _ = _edit_stack[0]
        msg = (f"Only 1 edit was made: '{desc}'. This is the cause of the regression. "
               "Restore it and try a different approach.")
        planner.addNote(f"[BISECT] Single edit '{desc}' caused regression")
        _clearEditStack()
        return msg

    # Save current state so we can return to it
    subprocess.run(["git", "stash", "push", "-m", "bisect-pre-revert"],
                   capture_output=True, text=True, cwd=PROJECT_DIR)

    # Revert to HEAD
    gf.restoreAll()

    culprit = None
    tested = []

    if n <= 4:
        # Linear search: apply one at a time
        for i, (desc, patch) in enumerate(_edit_stack):
            # Apply this single patch
            proc = subprocess.run(
                ["git", "apply"],
                input=patch, capture_output=True, text=True, cwd=PROJECT_DIR
            )
            if proc.returncode != 0:
                tested.append(f"  [{i+1}] '{desc}' — could not apply in isolation (depends on other edits)")
                subprocess.run(["git", "checkout", "--", "."],
                               capture_output=True, cwd=PROJECT_DIR)
                continue

            # Quick build check first
            build_proc = subprocess.run(
                ["make"], capture_output=True, text=True, cwd=PROJECT_DIR
            )
            if build_proc.returncode != 0:
                tested.append(f"  [{i+1}] '{desc}' — BUILD FAILED (this edit alone breaks compilation)")
                culprit = (i, desc, "build failure")
                break

            # Run bench
            try:
                _, bench_raw = makeBench()
                bench_summary = _benchSummary(bench_raw, BASELINE_RESULTS)
                if "REGRESSED" in bench_summary:
                    tested.append(f"  [{i+1}] '{desc}' — REGRESSION CONFIRMED")
                    culprit = (i, desc, "performance regression")
                    break
                else:
                    tested.append(f"  [{i+1}] '{desc}' — OK (no regression)")
            except Exception as e:
                tested.append(f"  [{i+1}] '{desc}' — BENCH FAILED: {e}")
                culprit = (i, desc, f"bench error: {e}")
                break

            # Revert before testing next
            subprocess.run(["git", "checkout", "--", "."],
                           capture_output=True, cwd=PROJECT_DIR)
    else:
        # Binary search for 5+ edits
        mid = n // 2
        first_half = _edit_stack[:mid]
        second_half = _edit_stack[mid:]

        # Test first half
        for _, patch in first_half:
            subprocess.run(["git", "apply"], input=patch, capture_output=True,
                           text=True, cwd=PROJECT_DIR)
        try:
            _, bench_raw = makeBench()
            bench_summary = _benchSummary(bench_raw, BASELINE_RESULTS)
            if "REGRESSED" in bench_summary:
                tested.append(f"  First {mid} edits — REGRESSION (culprit in this group)")
                # Would recurse here, but keep simple: report the group
                culprit_desc = f"some edit in the first {mid} edits"
            else:
                tested.append(f"  First {mid} edits — OK")
                # Culprit in second half
                gf.restoreAll()
                for _, patch in second_half:
                    subprocess.run(["git", "apply"], input=patch, capture_output=True,
                                   text=True, cwd=PROJECT_DIR)
                _, bench_raw = makeBench()
                bench_summary = _benchSummary(bench_raw, BASELINE_RESULTS)
                if "REGRESSED" in bench_summary:
                    culprit_desc = f"some edit in the last {n - mid} edits"
                else:
                    culprit_desc = "interaction between edits (neither half alone regresses)"
        except Exception as e:
            culprit_desc = f"bench error during bisection: {e}"

        gf.restoreAll()
        culprit = (None, culprit_desc, "binary search result")

    # Build report
    lines = [f"BISECTION RESULT ({n} edit(s) tested):"]
    lines.extend(tested)
    if culprit:
        idx, desc, reason = culprit
        if idx is not None:
            lines.append(f"\nCULPRIT: Edit [{idx+1}] '{desc}' — {reason}.")
        else:
            lines.append(f"\nCULPRIT: {desc} — {reason}.")
        lines.append("Restore this edit and try a different approach.")
        planner.addNote(f"[BISECT] Culprit: {desc} — {reason}")
    else:
        lines.append("\nNo single edit caused regression — may be an interaction effect.")
        lines.append("Try applying fewer edits at once, or test combinations.")

    # Restore original state from stash
    pop = subprocess.run(
        ["git", "stash", "pop"],
        capture_output=True, text=True, cwd=PROJECT_DIR
    )

    output = "\n".join(lines)
    _clearEditStack()
    CONTEXT.append({"type": "tool_use", "tool": "bisectRegression",
                    "input": f"{n} edits", "output": output})
    return output


# ---------------------------------------------------------------------------
# reviewer model pass — second pair of eyes before build
# ---------------------------------------------------------------------------

REVIEWER_MODEL = "deepseek/deepseek-v4-flash"
REVIEWER_PROVIDER = "baidu/fp8"

REVIEWER_PROMPT = """\
You are a senior C code reviewer. Review the following git diff for correctness \
bugs. Focus ONLY on bugs that would cause:

1. COMPILE ERRORS: missing semicolons, undeclared variables, type mismatches, \
   missing #includes, broken macro expansions.
2. RUNTIME CRASHES: null pointer dereference, use-after-free, buffer overflow, \
   stack overflow (large VLAs), division by zero.
3. LOGIC ERRORS: off-by-one, inverted conditions, missing edge cases, \
   incorrect operator precedence, accidental assignment in condition (= vs ==).
4. PERFORMANCE REGRESSIONS: accidental O(n^2) loops, repeated malloc/free in \
   hot path, VLAs on stack in threaded code (32 threads x VLA = stack explosion), \
   cache line false sharing between threads.
5. CORRECTNESS: changed function behavior (side effects removed, return value \
   semantics changed, const violations), float precision issues.

IGNORE: style issues, naming, comments, whitespace. Only flag bugs that would \
cause wrong output, crashes, or significant performance problems.

Reply with ONLY a valid JSON object (no markdown fences, no prose):
{
  "verdict": "PASS" or "FAIL",
  "issues": [
    {
      "severity": "CRITICAL" | "HIGH" | "MEDIUM" | "LOW",
      "file": "path/to/file.c",
      "line_hint": "approximate line or context",
      "description": "what is wrong",
      "fix_suggestion": "how to fix it"
    }
  ],
  "summary": "1-sentence summary if issues found, or empty string if PASS"
}

If no issues found: {"verdict": "PASS", "issues": [], "summary": ""}
"""

# Cache of the last reviewed diff so the same change is never reviewed twice,
# whether the review was triggered manually (reviewChanges) or automatically
# (buildProject pre-build gate).
_last_reviewed_diff_hash = None
_last_review_output = None


def _reviewDiff(diff: str) -> str:
    """Run the reviewer model on a diff string and return a human-readable verdict."""
    diff_truncated = diff[:12000]
    if len(diff) > 12000:
        diff_truncated += f"\n... [{len(diff) - 12000} more bytes truncated]"

    prompt = REVIEWER_PROMPT + "\n\nDIFF TO REVIEW:\n```diff\n" + diff_truncated + "\n```"
    try:
        raw = model.getResponse(prompt, model=REVIEWER_MODEL, provider=REVIEWER_PROVIDER)
        raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()
        m = re.search(r'\{.*\}', raw, re.DOTALL)
        if not m:
            return f"Reviewer model returned no valid JSON. Raw response:\n{raw[:500]}"
        data = json.loads(m.group(0))
        verdict = data.get("verdict", "?")
        issues = data.get("issues", [])
        summary = data.get("summary", "")
        if verdict == "PASS" or not issues:
            return "REVIEW: PASS — no bugs detected."
        lines = [f"REVIEW: {verdict} — {len(issues)} issue(s) found:"]
        for issue in issues:
            sev = issue.get("severity", "?")
            file = issue.get("file", "?")
            desc = issue.get("description", "?")
            fix = issue.get("fix_suggestion", "")
            lines.append(f"  [{sev}] {file}: {desc}")
            if fix:
                lines.append(f"        Fix: {fix}")
        if summary:
            lines.append(f"\nSummary: {summary}")
        return "\n".join(lines)
    except Exception as e:
        return f"Reviewer model error: {e}"


def reviewChanges():
    """
    Send the current git diff to a fast reviewer model for a bug check.
    Call this AFTER applying edits but BEFORE buildProject().

    The reviewer checks for: compile errors, null derefs, off-by-one,
    VLA stack explosions in threaded code, cache false sharing, etc.

    Returns the review verdict and injects issues into the context.
    """
    global _last_reviewed_diff_hash, _last_review_output
    diff = gf.getDiff(returnString=True)
    if not diff or diff == "(no changes)":
        return "(no changes to review)"

    import hashlib
    diff_hash = hashlib.md5(diff.encode()).hexdigest()
    if diff_hash == _last_reviewed_diff_hash and _last_review_output is not None:
        return _last_review_output + "\n(cached — diff unchanged since last review)"

    output = _reviewDiff(diff)
    _last_reviewed_diff_hash = diff_hash
    _last_review_output = output

    CONTEXT.append({"type": "tool_use", "tool": "reviewChanges",
                    "input": f"{len(diff)} byte diff", "output": output})
    return output


def _autoReviewBeforeBuild():
    """
    Run the reviewer model on the current diff before an expensive build, unless
    that exact diff was already reviewed. Returns the verdict string, or "" if
    there is nothing new to review. Cheap insurance against burning a 30s build
    cycle on a diff with an obvious bug.
    """
    global _last_reviewed_diff_hash, _last_review_output
    diff = gf.getDiff(returnString=True)
    if not diff or diff == "(no changes)":
        return ""
    import hashlib
    diff_hash = hashlib.md5(diff.encode()).hexdigest()
    if diff_hash == _last_reviewed_diff_hash:
        return ""  # already reviewed this exact diff

    output = _reviewDiff(diff)
    _last_reviewed_diff_hash = diff_hash
    _last_review_output = output
    CONTEXT.append({"type": "tool_use", "tool": "reviewChanges",
                    "input": f"auto pre-build, {len(diff)} byte diff", "output": output})
    return output


def convertContextToXml():
    def _esc(v):
        return str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    xml = "<context>\n"
    for entry in CONTEXT:
        xml += "  <entry>\n"
        for key, value in entry.items():
            xml += f"    <{key}>{_esc(value)}</{key}>\n"
        xml += "  </entry>\n"
    xml += "</context>"
    tokenCount = len(xml) // 4
    return xml, tokenCount


def _estimateTokens(obj) -> int:
    # JSON serialization underestimates vs the XML we actually send.
    # XML tags and escaping add ~25-30% overhead over raw JSON.
    json_len = len(json.dumps(obj))
    return int(json_len * 1.3) // 4


def compressContext():
    """
    Ask the model to summarise the current context, persist key findings and
    pending tasks to the planner board, then trim the in-memory context so
    nothing important is lost to truncation.

    Also preserves refinement state and recent benchmark results as structured
    notes so the model doesn't lose track of what strategies were tried.
    """
    if not CONTEXT:
        return

    # Persist refinement state to planner notes before compression
    refinementSummary = refine.getGlobalRefinementSummary()
    if refinementSummary:
        planner.addNote(f"[REFINEMENT_STATE]\n{refinementSummary}")

    # Persist calibration guidance
    calGuidance = refine.getCalibrationGuidance()
    if calGuidance:
        planner.addNote(f"[CALIBRATION]\n{calGuidance}")

    # Persist active convergence data
    convReport = planner.getConvergenceReport()
    if convReport and convReport != "(no convergence data)":
        planner.addNote(f"[CONVERGENCE]\n{convReport}")

    xmlContext, _ = convertContextToXml()
    summarize_prompt = (
        "You are reviewing your own working context from a C engine performance "
        "optimization session. Before this context is trimmed, extract and preserve "
        "the most critical information.\n"
        "<context_to_summarize>\n" + xmlContext + "\n</context_to_summarize>\n\n"
        "CRITICAL: Preserve anything that would help avoid repeating failed approaches.\n"
        "Include specific function names, strategies tried, and measured numbers.\n"
        "Reply with ONLY a valid JSON object (no prose, no markdown fences):\n"
        "{\n"
        '  "summary": "2-3 sentence overview of progress so far",\n'
        '  "key_findings": ["important hotspot or perf finding with numbers"],\n'
        '  "completed_changes": ["change applied + measured result"],\n'
        '  "failed_approaches": ["what was tried and WHY it failed — MOST IMPORTANT"],\n'
        '  "pending_tasks": ["still needs to be investigated or tried"],\n'
        '  "critical_notes": ["baselines, regressions, constraints to remember"],\n'
        '  "lessons_learned": ["patterns discovered about this specific codebase"]\n'
        "}"
    )

    try:
        raw = model.getResponseQwen3_6(summarize_prompt, mode="instruct")
        raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()
        m = re.search(r'\{.*\}', raw, re.DOTALL)
        if not m:
            print(f"[compressContext] no JSON found in model response: {raw[:300]}")
        if m:
            data = json.loads(m.group(0))
            if data.get("summary"):
                planner.addNote(f"[SUMMARY] {data['summary']}")
            for item in data.get("key_findings", []):
                planner.addNote(f"[HOTSPOT] {item}")
            for item in data.get("completed_changes", []):
                planner.addNote(f"[DONE] {item}")
            for item in data.get("failed_approaches", []):
                planner.addNote(f"[FAILED] {item}")
            for item in data.get("pending_tasks", []):
                planner.addTask(item)
            for item in data.get("critical_notes", []):
                planner.addNote(f"[CRITICAL] {item}")
            for item in data.get("lessons_learned", []):
                planner.addNote(f"[LESSON] {item}")
            print(f"[compressContext] saved {len(data.get('pending_tasks', []))} tasks and "
                  f"{len(data.get('key_findings', [])) + len(data.get('completed_changes', [])) + len(data.get('failed_approaches', [])) + len(data.get('critical_notes', [])) + len(data.get('lessons_learned', []))} notes to planner")
    except Exception as e:
        print(f"[compressContext] summarization failed: {e}")

    # Persist high-level insights to codebase_context.md before trimming
    syncPlannerToCodebaseContext()

    board = planner.showBoard(returnString=True)
    # Preserve the last few entries that contain benchmark results or flame data
    preserved = []
    for entry in reversed(CONTEXT):
        if entry.get("tool") in ("makeBench", "makeFlame", "getRefinementState",
                                  "getUntriedStrategies", "showBoard"):
            preserved.append(entry)
        if len(preserved) >= 3:
            break

    # Preserve the codebase-knowledge pointer so the model always remembers it can
    # re-read the knowledge base via getCodebaseContext() after compression.
    knowledge_entry = next(
        (e for e in CONTEXT if e.get("type") == "codebase_knowledge"), None
    )

    keep = max(8, len(CONTEXT) * 2 // 3)
    del CONTEXT[:-keep]
    # Re-insert preserved entries at the front (most recent first)
    for entry in reversed(preserved):
        CONTEXT.insert(1, entry)
    if knowledge_entry is not None and knowledge_entry not in CONTEXT:
        CONTEXT.insert(1, knowledge_entry)
    CONTEXT.insert(0, {
        "type": "context_summary",
        "tool": "contextSummary",
        "output": (
            f"[Context compressed. Key information saved to planner.]\n{board}\n\n"
            f"PINNED_HOTSPOTS:\n{PINNED_HOTSPOTS if PINNED_HOTSPOTS else '(none)'}"
        ),
    })


def removeStaffFromContext(maxTokens=CONTEXT_MAX_TOKENS):
    # The real prompt is system_prompt + session_header + board + xmlContext. The
    # system prompt embeds the codebase knowledge base, which grows as insights
    # accumulate. Reserve room for it so CONTEXT is trimmed against the budget that
    # actually remains, not the full window.
    try:
        prompt_tokens = len(getSystemPrompt()) // 4
    except Exception:
        prompt_tokens = 0
    reserve = prompt_tokens + 4_000  # +headroom for board, session header, response
    effective = max(maxTokens // 4, maxTokens - reserve)

    if _estimateTokens(CONTEXT) > effective * CONTEXT_COMPRESS_AT:
        compressContext()
    # Hard trim if still over limit after compression. Never drop the codebase
    # knowledge pointer — it is the model's gateway back to the knowledge base.
    idx = 0
    while _estimateTokens(CONTEXT) > effective and len(CONTEXT) > 1:
        if idx >= len(CONTEXT):
            break
        if CONTEXT[idx].get("type") == "codebase_knowledge":
            idx += 1
            continue
        removed = CONTEXT.pop(idx)
        print(f"[trim] dropped entry type={removed.get('type')} tool={removed.get('tool')}")

def _github_create_pr(title, body, head, base="main"):
    import urllib.request, json as _json, re as _re
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        raise RuntimeError("GITHUB_TOKEN not set in environment or .env")
    res = subprocess.run(
        ["git", "remote", "get-url", "origin"],
        capture_output=True, text=True, cwd=PROJECT_DIR
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
    commit_msg = commit_msg or title

    run(["git", "checkout", "-b", branch], cwd=PROJECT_DIR)
    run(["git", "add", "-A"], cwd=PROJECT_DIR)
    run(["git", "commit", "-m", commit_msg], cwd=PROJECT_DIR)
    run(["git", "push", "-u", "origin", branch], cwd=PROJECT_DIR)

    url = _github_create_pr(title, body, head=branch)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "createPR",
        "input": {"title": title, "branch": branch},
        "output": url,
    })
    ui.set_pr_url(url)
    print(f"PR created: {url}")

    # Checkpoint: persist planner insights to codebase_context.md
    syncPlannerToCodebaseContext()

def removeDoublesFromContext():
    # keep newest entry for each (type, tool, input) key
    seen = set()
    new_context = []
    for entry in reversed(CONTEXT):
        key = (entry.get("type"), entry.get("tool"), str(entry.get("input")))
        if key not in seen:
            seen.add(key)
            new_context.append(entry)
    CONTEXT[:] = list(reversed(new_context))

CODEBASE_CONTEXT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "codebase_context.md")
PLANNER_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "planner_state.json")
KNOWLEDGE_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "knowledge_state.json")

# Insight categories accumulated across sessions, in render order.
_KNOWLEDGE_CATEGORIES = [
    ("confirmed_wins", "Confirmed Wins"),
    ("architectural_insights", "Architectural Insights"),
    ("remaining_hotspots", "Remaining Hotspots"),
    ("techniques_to_try", "Techniques to Try"),
    ("techniques_to_avoid", "Techniques to Avoid"),
]
_KNOWLEDGE_CAP = 25  # max insights kept per category (most recent retained)


def _normInsight(text: str) -> str:
    """Normalize an insight for dedup: lowercase, collapse whitespace, strip punctuation tails."""
    return re.sub(r"\s+", " ", str(text).strip().lower()).rstrip(".;,")


def _loadKnowledgeState() -> dict:
    if os.path.exists(KNOWLEDGE_STATE_FILE):
        try:
            with open(KNOWLEDGE_STATE_FILE) as fh:
                return json.load(fh)
        except (json.JSONDecodeError, OSError):
            pass
    return {key: [] for key, _ in _KNOWLEDGE_CATEGORIES}


def _mergeKnowledge(state: dict, data: dict) -> int:
    """Merge a session's distilled insights into the accumulated store.
    Dedupes case-insensitively (newest wins) and caps each category. Returns count added."""
    added = 0
    for key, _ in _KNOWLEDGE_CATEGORIES:
        existing = state.setdefault(key, [])
        seen = {_normInsight(x) for x in existing}
        for item in data.get(key, []):
            norm = _normInsight(item)
            if not norm or norm in seen:
                continue
            existing.append(str(item).strip())
            seen.add(norm)
            added += 1
        # keep the most recent _KNOWLEDGE_CAP entries
        if len(existing) > _KNOWLEDGE_CAP:
            del existing[:-_KNOWLEDGE_CAP]
    return added


def _renderKnowledgeSection(state: dict, summary: str, timestamp: str) -> str:
    lines = [f"\n\n---\n\n## Accumulated Insights (updated {timestamp})\n"]
    if summary:
        lines.append(f"**Latest session**: {summary}\n")
    for key, heading in _KNOWLEDGE_CATEGORIES:
        items = state.get(key, [])
        if items:
            lines.append(f"### {heading}")
            for item in items:
                lines.append(f"  - {item}")
            lines.append("")
    return "\n".join(lines)


def syncPlannerToCodebaseContext():
    """
    Ask the model to distill the planner's accumulated notes and tasks into
    high-level insights, then inject them into codebase_context.md so
    knowledge survives across runs. Only architectural findings, confirmed
    wins, and remaining priorities are kept — rejected approaches and transient
    debugging notes are filtered out.

    Can be called by the model as a tool, or automatically at checkpoints.
    """
    import datetime

    # Load planner state
    if not os.path.exists(PLANNER_STATE_FILE):
        return "No planner state file found — nothing to sync."

    with open(PLANNER_STATE_FILE) as fh:
        state = json.load(fh)

    notes = state.get("notes", [])
    tasks = state.get("tasks", [])

    if not notes and not tasks:
        return "Planner has no notes or tasks — nothing to sync."

    # Build a compact representation for the model to summarize
    notes_text = "\n".join(f"  [{n['id']}] {n['text']}" for n in notes)
    tasks_text = "\n".join(
        f"  [{t['status']}] #{t['id']} {t['text']}" for t in tasks
    )

    summarize_prompt = (
        "You are reviewing the accumulated notes and tasks from a C engine "
        "performance optimization session. Your job is to distill this into "
        "HIGH-LEVEL insights suitable for injecting into a codebase knowledge "
        "base (codebase_context.md).\n\n"
        "FILTERING RULES:\n"
        "- INCLUDE: architectural findings, confirmed performance wins with "
        "measured numbers, remaining high-priority hotspots, lessons about what "
        "techniques work or don't work for this specific codebase.\n"
        "- EXCLUDE: exact code snippets, rejected implementations, transient "
        "debugging notes, tool call logs, temporary measurements.\n"
        "- Be concise. Each insight should be 1-2 sentences.\n\n"
        "PLANNER NOTES:\n" + (notes_text or "(none)") + "\n\n"
        "PLANNER TASKS:\n" + (tasks_text or "(none)") + "\n\n"
        "Reply with ONLY a valid JSON object (no prose, no markdown fences):\n"
        "{\n"
        '  "summary": "1-2 sentence overview of what was accomplished this session",\n'
        '  "confirmed_wins": ["function: technique -> measured improvement"],\n'
        '  "architectural_insights": ["important findings about this codebase"],\n'
        '  "remaining_hotspots": ["functions still needing optimization, in priority order"],\n'
        '  "techniques_to_try": ["approaches worth attempting next"],\n'
        '  "techniques_to_avoid": ["approaches that did not work and why"]\n'
        "}"
    )

    try:
        raw = model.getResponse(summarize_prompt, model="deepseek/deepseek-v4-flash", provider="deepinfra/fp4")
        raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()
        m = re.search(r'\{.*\}', raw, re.DOTALL)
        if not m:
            return f"Model response had no JSON: {raw[:200]}"
        data = json.loads(m.group(0))
    except Exception as e:
        return f"Summarization failed: {e}"

    # Merge this session's insights into the persistent, deduplicated knowledge store
    # so findings accumulate across runs instead of overwriting each other.
    knowledge = _loadKnowledgeState()
    added = _mergeKnowledge(knowledge, data)
    with open(KNOWLEDGE_STATE_FILE, "w") as fh:
        json.dump(knowledge, fh, indent=2)

    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    section_text = _renderKnowledgeSection(knowledge, data.get("summary", ""), timestamp)

    # Load existing codebase context
    if os.path.exists(CODEBASE_CONTEXT_FILE):
        with open(CODEBASE_CONTEXT_FILE) as fh:
            existing = fh.read()
    else:
        existing = ""

    # Replace the rendered insights block if present, otherwise append. The
    # canonical store is knowledge_state.json; the markdown is just a view of it.
    for marker in ("\n\n---\n\n## Accumulated Insights", "\n\n---\n\n## Session Insights"):
        if marker in existing:
            existing = existing[:existing.index(marker)].rstrip()
            break

    with open(CODEBASE_CONTEXT_FILE, "w") as fh:
        fh.write(existing.rstrip() + section_text)

    # Clear the notes that have been distilled (keep tasks)
    state["notes"] = []
    with open(PLANNER_STATE_FILE, "w") as fh:
        json.dump(state, fh, indent=2)

    return (f"Synced {len(notes)} note(s) and {len(tasks)} task(s): {added} new insight(s) "
            f"merged into the knowledge base ({KNOWLEDGE_STATE_FILE}). Planner notes cleared.")

RESEARCH_MODEL = "deepseek/deepseek-v4-flash"
RESEARCH_PROVIDER = "baidu/fp8"
MIN_RESEARCH_CONTEXT_ENTRIES = 5

# Tools that must be excluded from the research-phase tool map (read-only mode).
EDITING_TOOLS = frozenset({
    "searchReplace", "searchReplaceMulti", "applyChange", "replaceLines",
    "insertLines", "deleteLines", "applyPatch", "restoreAll", "restoreFile",
    "restoreFunction",
})

RESEARCH_SYSTEM_PROMPT = """\
You are an expert C software engineer performing a thorough codebase research pass \
before an optimization session begins. Your goal is to build a comprehensive, \
persistent knowledge base about this engine's source code so future optimization \
iterations start with full context.

== YOUR TASK ==
Explore the codebase systematically and document:
1. The overall architecture and key subsystems (files, modules, their responsibilities).
2. Every performance-critical function: its location, what it does, why it is likely slow \
(algorithmic complexity, memory access patterns, branching, synchronization, GPU/CPU \
boundary crossings, etc.).
3. Important data structures and how they flow between hot functions.
4. Known TODOs, existing comments about performance, and obvious low-hanging fruit.
5. The call graph from the main render/compute loop down to leaf hot functions.

== HOW TO CALL TOOLS ==
You MUST call tools by emitting fenced JSON blocks. Every response that wants to \
take action MUST contain at least one tool block:

```json
{"tool": "listFunctions", "args": {}}
```

```json
{"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
```

```json
{"tool": "saveCodebaseContext", "args": {"report": "# Codebase Architecture\\n## Key Files and Modules\\n..."}}
```

Do NOT just write prose about what you plan to do — actually emit the JSON tool \
blocks to call the tools. Your response can contain prose + tool blocks together.

== WORKFLOW ==
1. Start with listFunctions() to get a complete function inventory.
2. For key files, call readSourceFile(rel_path) or showContext(func, depth=2).
3. If profiling data is available, call hotAnnotateFile(rel_path) or hotAnnotateFunc(func).
4. Call grepSource(pattern) to find TODOs, FIXME, or performance comments.
5. Call getCallers(func) to trace the call graph for hot functions.
6. After gathering enough information, call saveCodebaseContext(report) with your \
full structured Markdown report. This MUST be the last tool call — it ends the phase.

== saveCodebaseContext(report) ==
Call this with a single "report" arg containing your full Markdown report:
```json
{"tool": "saveCodebaseContext", "args": {"report": "# Codebase Architecture\\n\\n## Key Files and Modules\\n- render/render.c: main render loop...\\n\\n## Critical Data Structures\\n...\\n\\n# Performance-Critical Functions\\n\\n## renderFrame (render/render.c)\\n- What it does: ...\\n- Why it is slow: ...\\n- Callers: ...\\n\\n# Known TODOs and Low-Hanging Fruit\\n...\\n\\n# Recommended Optimization Order\\n1. ...\\n2. ..."}}
```

== CONSTRAINTS ==
- Do NOT apply any code changes. Only use read/exploration tools.
- Call apiHelp() if you are unsure which tools are available.
- When your research is complete, call saveCodebaseContext(report) EXACTLY ONCE.
- If you don't yet have enough information, call more exploration tools.
- The iteration count tells you how many turns remain — plan accordingly.
"""

RESEARCH_MAX_ITERATIONS = 30  # guard against runaway research loops

RESEARCH_CONTEXT: list = []


def saveCodebaseContext(report: str, context=None):
    """Write the model's codebase research report to a persistent Markdown file."""
    with open(CODEBASE_CONTEXT_FILE, "w") as fh:
        fh.write(report)
    msg = f"Codebase context saved to {CODEBASE_CONTEXT_FILE} ({len(report)} chars)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "saveCodebaseContext",
                        "input": f"{len(report)} chars", "output": msg})
    print(f"[research] {msg}")
    return msg


def loadCodebaseContext() -> str | None:
    """Load the persisted codebase research report, or return None if absent."""
    if not os.path.exists(CODEBASE_CONTEXT_FILE):
        return None
    with open(CODEBASE_CONTEXT_FILE) as fh:
        return fh.read()


def _extractSection(markdown: str, heading_query: str) -> str | None:
    """Return the markdown block whose '#'/'##'/'###' heading contains heading_query
    (case-insensitive), up to the next heading of the same-or-higher level."""
    q = heading_query.strip().lower()
    lines = markdown.splitlines()
    start = None
    start_level = 0
    for i, line in enumerate(lines):
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m and q in m.group(2).strip().lower():
            start = i
            start_level = len(m.group(1))
            break
    if start is None:
        return None
    end = len(lines)
    for j in range(start + 1, len(lines)):
        m = re.match(r'^(#{1,6})\s+', lines[j])
        if m and len(m.group(1)) <= start_level:
            end = j
            break
    return "\n".join(lines[start:end]).strip()


def getCodebaseContext(section: str = None, context=None):
    """
    Return the persisted codebase knowledge base (codebase_context.md).

    The full report is always present in the system prompt, but it can be trimmed
    from the working context during compression — call this to re-read it on demand.
    Pass `section` (a heading substring, e.g. "Performance-Critical Functions" or
    "Accumulated Insights") to fetch only that part instead of the whole document.
    """
    ctx = loadCodebaseContext()
    if not ctx:
        output = "No codebase_context.md found. Run the research phase (--research) first."
    elif section:
        block = _extractSection(ctx, section)
        output = block if block else (
            f"No section matching '{section}'. Call getCodebaseContext() without args "
            "to read the whole report."
        )
    else:
        output = ctx
    if context is not None:
        context.append({"type": "tool_use", "tool": "getCodebaseContext",
                        "input": section, "output": output})
    return output


def runCodebaseResearch():
    """
    Run an agentic research loop where the model explores the codebase and
    produces a persistent codebase_context.md knowledge base file.
    """
    global RESEARCH_CONTEXT
    RESEARCH_CONTEXT = []

    # Seed with exploration data
    getTree()
    gf.listFunctions(context=RESEARCH_CONTEXT)
    gf.apiHelp(context=RESEARCH_CONTEXT)

    # Exploration-only tool map (no editing, no build/bench)
    research_tool_map = executor.buildToolMap(gf, planner)
    research_tool_map["saveCodebaseContext"] = lambda report, context=None: saveCodebaseContext(report, context=context)
    # Remove editing/destructive tools so the research pass is read-only
    for _tool in EDITING_TOOLS:
        research_tool_map.pop(_tool, None)

    print("[research] Starting codebase research phase …")
    last_response_was_prose_only = 0
    for iteration in range(1, RESEARCH_MAX_ITERATIONS + 1):
        remaining = RESEARCH_MAX_ITERATIONS - iteration
        # Hard-trim research context if it grows too large (keep most recent 2/3)
        if _estimateTokens(RESEARCH_CONTEXT) > CONTEXT_MAX_TOKENS * CONTEXT_COMPRESS_AT:
            keep = max(MIN_RESEARCH_CONTEXT_ENTRIES + 5, len(RESEARCH_CONTEXT) * 3 // 4)
            del RESEARCH_CONTEXT[:-keep]
            print(f"[research] trimmed context, kept last {keep} entries")

        # Build XML from RESEARCH_CONTEXT using the same escaper as the main loop
        def _esc(v):
            return str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        xml_research = "<context>\n"
        for entry in RESEARCH_CONTEXT:
            xml_research += "  <entry>\n"
            for k, v in entry.items():
                xml_research += f"    <{k}>{_esc(v)}</{k}>\n"
            xml_research += "  </entry>\n"
        xml_research += "</context>"

        # Build urgency hint
        urgency = ""
        if remaining <= 0:
            urgency = (
                "\n\n*** THIS IS YOUR LAST ITERATION. You MUST call saveCodebaseContext(report) "
                "NOW with whatever information you have gathered. Do NOT call any other tools. "
                "Failure to call saveCodebaseContext() means ALL your research is lost. ***"
            )
        elif remaining <= 3:
            urgency = (
                f"\n\n*** Only {remaining} iterations remaining. You MUST call "
                "saveCodebaseContext(report) very soon. If you already have enough information "
                "about the codebase, call saveCodebaseContext() now. ***"
            )
        elif remaining <= 7:
            urgency = (
                f"\n\n({remaining} iterations remaining. Make sure you are gathering enough "
                "information to write a complete report before you run out of turns.)"
            )

        # Nudge if model produced prose-only responses multiple times in a row
        prose_nudge = ""
        if last_response_was_prose_only >= 2:
            prose_nudge = (
                "\n\n*** Your last responses had NO tool calls. You MUST emit JSON tool blocks "
                "to call tools. Example: ```json\n{\"tool\": \"readSourceFile\", \"args\": {\"rel_path\": \"render/render.c\"}}\n```\n"
                "Do not just describe what you would do — actually call the tools. ***"
            )

        prompt = (
            RESEARCH_SYSTEM_PROMPT
            + f"\n\nIteration {iteration}/{RESEARCH_MAX_ITERATIONS} ({remaining} remaining)"
            + urgency
            + prose_nudge
            + "\n\nContext:\n" + xml_research
        )
        print(f"[research] iteration {iteration}/{RESEARCH_MAX_ITERATIONS} (context ~{len(xml_research)//4} tokens, {remaining} remaining)")

        try:
            response = model.getResponse(prompt, model=RESEARCH_MODEL, provider=RESEARCH_PROVIDER)
        except Exception as e:
            print(f"[research] model error: {e}, retrying …")
            continue

        if response is None:
            print("[research] model returned None, retrying …")
            continue

        response_clean = re.sub(r"<think>.*?</think>", "", response, flags=re.DOTALL).strip()
        RESEARCH_CONTEXT.append({
            "type": "model_response",
            "iteration": iteration,
            "output": response_clean,
        })
        print(f"[research] model responded ({len(response_clean)} chars)")

        results = executor.executeAll(response, research_tool_map, context=RESEARCH_CONTEXT)
        if results:
            last_response_was_prose_only = 0
        else:
            last_response_was_prose_only += 1
            print(f"[research] no tool calls in response ({last_response_was_prose_only} consecutive prose-only responses)")

        # Stop as soon as saveCodebaseContext was successfully called
        for r in results:
            if r["tool"] == "saveCodebaseContext" and r["error"] is None:
                print("[research] Research phase complete.")
                return

    print(f"[research] WARNING: reached max iterations ({RESEARCH_MAX_ITERATIONS}) without "
          "saveCodebaseContext being called. Partial results (if any) may exist.")


PLAN_PROMPT = """\
You are an expert C software engineer reviewing your own optimization session. \
Your ONLY task right now is to think deeply and produce a structured plan. \
Do NOT call any tools. Do NOT write any code. Do NOT emit any JSON tool blocks.

Review everything in the context — flame graph data, bench results, code you have \
already read, notes on the board, completed changes, regressions, and the \
REFINEMENT STATE section showing what strategies have been tried on each function.

Fill out EVERY section below. Be specific with function names, file paths, and \
measured numbers from the context.

== SITUATION REVIEW ==
1. Functions already optimized (with measured results):
   [List each function, what was tried, and the bench result. Be honest about regressions.]

2. Current top 3 hotspots from flame graph (NOT yet successfully optimized):
   [Function name — inclusive% — why it's hot]

3. Recent failures and what was learned from each:
   [For each failed attempt: what approach, why it failed, what you learned.]

== NEXT TARGET ==
4. Single function to optimize next:
   [Name]

5. Why this function (use numbers from context):
   [Justify with hotspot data, potential impact, and whether strategies remain untried.]

== STRATEGY ==
6. Optimization strategy (pick ONE from this taxonomy):
   algorithmic / memory_layout / simd / threading / branchless / precompute /
   loop_transform / data_reuse / instruction_level
   [Your choice]

7. Specific approach:
   [What exact change will you make? Be concrete.]

8. Expected improvement:
   Micro-bench: [X%]   makeBench: [Y%]

== PRE-MORTEM (answer honestly) ==
9. Why might this optimization regress in the multi-threaded renderer?
   Consider: cache pressure, memory bandwidth, TLB contention, working set size,
   compiler already doing this at -O3 -ffast-math, micro-bench not representative.

10. What evidence would convince you this approach is WRONG?
    [What bench result would make you abandon this strategy?]

11. Is there an alternative strategy you should try FIRST?
    [Sometimes a simpler approach works better. Think before committing.]

== EXECUTION ==
12. Concrete next steps (in order):
    1. [Read what source file/function?]
    2. [Create micro-benchmark for what function?]
    3. [What variant(s) will you compare?]
    4. [After proving speedup, apply to which file?]

REMEMBER: The workflow is ISOLATION-FIRST.
  read function code -> createFuncBench -> runFuncBench -> prove speedup ->
  ONLY THEN apply to main code -> buildProject -> makeBench.

After your analysis, call addNote() with "[PLAN] <function>: <strategy> — <approach>" \
and addTask() for each concrete step. If all strategies for a function are exhausted, \
call convergeFunction() and pick a different hotspot.

Reply in plain prose filling out all sections above. Be brutally honest in the \
pre-mortem — recognizing failure modes BEFORE coding is the most valuable skill.
"""

SYSTEM_PROMPT = """\
You are an expert C software engineer. Your job is to analyse a renderer/engine \
codebase and iteratively improve its performance. You work in an ISOLATION-FIRST loop:

  profile -> identify hotspot -> read function code -> write optimized variant \
in bench/ folder -> benchmark in isolation -> validate correctness -> \
PRE-MORTEM check -> only then apply proven optimization to main codebase -> \
build -> bench -> PR

The KEY PRINCIPLE is: NEVER optimize the main codebase directly. Always first write \
and prove your optimization in the bench/ micro-benchmark sandbox, similar to how \
tests/rayAABB_inv.h contains multiple versions (V1 original, V2, V3, V4 AVX2, etc.) \
with timing and correctness validation. Only after proving a speedup in isolation \
should you apply the change to the main code.

== CRITICAL: THE PRE-MORTEM RULE ==
Before applying ANY micro-benchmark win to the main codebase, you MUST do a \
pre-mortem assessment. Ask yourself:
  "This micro-benchmark is single-threaded and has a tiny working set. The real \
   renderer is multi-threaded (32 threads) with heavy cache and memory pressure. \
   Will this optimization survive the transition?"
Common reasons micro-bench wins regress in the real renderer:
  - Increased working set size (more cache eviction)
  - Extra indirection (more cache misses per thread)
  - Changed alignment (TLB effects across threads)
  - Added instructions that the compiler cannot hoist/schedule across thread boundaries
  - The micro-benchmark's input data pattern doesn't match real workload
If you cannot explain WHY the optimization will survive multi-threaded conditions, \
do NOT apply it. Instead, try a different strategy or converge.

The session header shows your current iteration, iterations since last successful \
change, and whether you have uncommitted modifications. Use this to gauge progress.
The REFINEMENT STATE section shows what strategies have been tried on each function —
use getRefinementState() and getUntriedStrategies() to avoid repeating failures.

== CALLING TOOLS ==
To call a tool, emit one or more fenced JSON blocks anywhere in your response.
Every block must be valid JSON with "tool" (string) and "args" (object) fields:

```json
{"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
```

```json
{"tool": "createFuncBench", "args": {"func_name": "myFunc", "header_code": "...", "impl_code": "..."}}
```

Multiple blocks in one response are executed in order. Omitting "args" is also \
accepted and treated as an empty object. Every tool result is appended to the \
context so you can see it in the next turn. Use apiHelp() (already in context) to \
list all available tools and their exact argument names.

== WORKFLOW (follow this order strictly) ==
1. Read the flame graph and bench results already in the context.
2. Use showBoard() to review your current task list and notes.
3. Pick the top hotspot function. Use hotAnnotateFunc(func) to see exactly which \
lines are hot, or showContext(func, depth=2) / showSrcPair(path) to read the code.
4. BEFORE writing any code, record your findings: \
call addNote() with the hotspot name and why it is slow, \
call addTask() for each concrete optimization you plan to try.
5. **CREATE A MICRO-BENCHMARK** (this is MANDATORY for every optimization): \
call createFuncBench(func_name, header_code, impl_code) where:
   - header_code contains the original function + your optimized variant declarations
   - impl_code contains a main() that:
     a. Generates random test data (large sample size, e.g. millions of calls)
     b. Runs a warm-up pass
     c. Times the ORIGINAL function (baseline)
     d. Times your OPTIMIZED variant(s)
     e. Validates correctness (compare outputs of original vs optimized)
     f. Prints timing results and speedup ratio
   Follow the pattern in tests/rayAABB_inv.h and tests/rayAABB_inv.c as a reference: \
multiple versions (V1=original, V2=optimized, V3=SIMD, etc.) all benchmarked \
and validated against the original.
6. Call runFuncBench(func_name) to compile and run the benchmark.
7. Record results with addNote(). If the optimization shows a measurable speedup \
AND passes correctness validation, proceed to step 8. If not, try a different \
approach (go back to step 5 with a new variant) or move to a different hotspot.
8. **ONLY AFTER PROVING SPEEDUP**: Apply the proven optimization to the main \
codebase using editing tools:\
\n   - searchReplace(rel_path, old_text, new_text) — preferred, no line numbers needed\
\n   - searchReplaceMulti(rel_path, [{"old":...,"new":...},...]) — multiple edits in one call\
\n   - applyChange(func_name, new_definition) — replace a whole named function
9. Call buildProject(). If it fails, read the error, fix the code, rebuild.
10. Call makeBench() and compare against the baseline. If performance regressed, \
call restoreAll() and add a note explaining what went wrong.
11. When a change is solid (measurable improvement, no visual regression), call \
createPR() with a clear title and body, then call markTaskDone().
12. Call deleteFuncBench(func_name) to clean up, then continue with the next hotspot.

== NOTE-TAKING RULES ==
- ALWAYS call addNote() / addTask() BEFORE reading code or applying changes.
- After every bench result, call addNote() with the measured numbers.
- When you discover something about the codebase, call addNote() immediately.
- Notes and tasks survive context compression — they are your long-term memory.
  Anything not written to the board WILL be forgotten when context is trimmed.
- Use showBoard() at the start of each iteration to recall your plan.

== MICRO-BENCHMARK PATTERN (MANDATORY) ==
Look at tests/rayAABB_inv.h and tests/rayAABB_inv.c for the gold standard example:
- The .h file contains the ORIGINAL function + multiple optimized variants (V2, V3, V4, V5, V6, V7)
- The .c file generates random test data, times every variant, compares results for correctness
- Each variant explores a different optimization strategy (pass-by-value, FMA, SSE, AVX2, SoA layout)
- Results print ns/call for each variant so you can see exactly which is fastest

Your bench files should follow this SAME pattern:
1. createFuncBench(func_name, header_code, impl_code)
   Creates bench/<func_name>.h and bench/<func_name>.c.
   - header_code: #ifndef guard, the ORIGINAL function copied verbatim, then your \
optimized variant(s) with different names (e.g. funcV2, funcV3, funcV4_sse, etc.)
   - impl_code: full .c file with main() that:
     a. Allocates large arrays of random test data (millions of samples).
     b. Runs warm-up passes for all variants.
     c. Times each variant using clock_gettime(CLOCK_MONOTONIC).
     d. Prints ns/call (or ms/call) for each variant.
     e. Validates ALL optimized variants produce the same output as the original.
     f. Reports mismatches if any.
   The file may #include project headers by path if needed (e.g. "../render/render.h", \
"../object/format.h"), but must NOT depend on OpenCL or minifb.

2. runFuncBench(func_name)
   Compiles bench/<func_name>.c (linked with tests/timings.c and -lm) and runs it.
   Returns the full stdout output with your timing and correctness results.

3. deleteFuncBench(func_name)
   Removes the bench files and binary when you are done.

== WHEN YOU MAY SKIP THE SANDBOX ==
Only skip the micro-benchmark when:
- The change requires OpenCL, minifb, or other project-specific infrastructure \
that cannot be isolated.
- The change is purely structural (data layout change that affects the whole pipeline).
In these rare cases, apply directly and use makeBench() to validate.

== ANTI-PATTERNS: Things you MUST NOT do ==
1. DO NOT apply optimizations directly to the main codebase without first proving \
   them in a micro-benchmark. This is the #1 rule.
2. DO NOT read the same source file more than twice without making a change.
   If you keep re-reading, you are stuck — call addNote() and move on.
3. DO NOT call buildProject() + makeBench() without having changed any code.
4. DO NOT apply a change, then immediately restoreAll() and re-apply the same change.
   If a change regresses, document why and try a different approach.
5. DO NOT propose changes without first reading the relevant source code.
6. DO NOT ignore build errors. If buildProject() fails, fix the error immediately.
7. DO NOT keep calling the same exploration tool over and over.

== CONSTRAINTS ==
- Only call tools that are listed in apiHelp(). Anything else will be rejected.
- File paths must be relative to the project root. Never use ".." to escape it.
- Do not break the public API (function signatures visible in headers) without \
explicit instruction.
- Always build and bench after applying a proven optimization to main code.
- If you are unsure whether a change is safe, benchmark it in isolation first.
- Keep changes focused and minimal — one logical improvement per PR.

== VISUAL CORRECTNESS ==
makeBench() reports two visual metrics:
  image_mse     — MSE between current and baseline pixel data. THIS IS THE PRIMARY \
correctness metric. avg_mse < 1.0 = visually identical; < 10.0 = acceptable minor \
difference (floating-point reordering, precision changes); < 100.0 = noticeable but \
may be acceptable; >= 100.0 = significant change, investigate before submitting.
  frame_hashes  — exact 32-bit hash match. INFORMATIONAL ONLY. Hashes will differ \
for any floating-point reordering, SIMD shuffles, or precision changes even when the \
image is visually identical. Do NOT treat a hash mismatch as a correctness failure. \
Use image_mse to judge correctness.
{codebase_context_section}"""

# TODO: add api where model can execute some code in a sandbox

def _buildSystemPrompt():
    """Return (prompt_str, codebase_ctx_or_None) with the codebase context section injected."""
    ctx = loadCodebaseContext()
    if ctx:
        section = (
            "\n\n== CODEBASE KNOWLEDGE BASE ==\n"
            "The following pre-built research report describes this codebase's "
            "architecture and known performance hotspots. Use it as your primary "
            "reference before reading source files.\n\n"
            + ctx
        )
    else:
        section = ""
    return SYSTEM_PROMPT.replace("{codebase_context_section}", section), ctx


_SYSTEM_PROMPT_CACHE = None
_SYSTEM_PROMPT_MTIME = None


def getSystemPrompt() -> str:
    """
    Return the system prompt with the codebase knowledge base embedded, rebuilding
    it whenever codebase_context.md changes on disk. This lets insights written by
    syncPlannerToCodebaseContext() mid-session become visible to the model on the
    very next iteration instead of only after a process restart.
    """
    global _SYSTEM_PROMPT_CACHE, _SYSTEM_PROMPT_MTIME
    try:
        mtime = os.path.getmtime(CODEBASE_CONTEXT_FILE)
    except OSError:
        mtime = None
    if _SYSTEM_PROMPT_CACHE is None or mtime != _SYSTEM_PROMPT_MTIME:
        _SYSTEM_PROMPT_CACHE, _ = _buildSystemPrompt()
        _SYSTEM_PROMPT_MTIME = mtime
        if mtime is not None:
            print("[main] Rebuilt system prompt from updated codebase_context.md")
    return _SYSTEM_PROMPT_CACHE


# Track session-wide patterns for adaptive guidance
_session_micro_bench_wins = 0     # micro-benchmarks that showed speedup
_session_real_regressions = 0     # those same changes that regressed in makeBench
_session_restore_count = 0        # total restoreAll calls
_session_last_target_func = ""    # last function the model worked on
_session_same_func_iterations = 0 # consecutive iterations on same function


def _buildAdaptiveGuidance(recentToolNames, iteration, lastBenchIteration):
    """
    Build adaptive guidance that gets stronger as problematic patterns persist.
    Returns a string to inject into the system prompt, or empty string.
    """
    global _session_micro_bench_wins, _session_real_regressions
    global _session_restore_count, _session_last_target_func, _session_same_func_iterations

    parts = []

    # Pattern A: micro-bench wins that regress in real workload
    # This is the most critical pattern — it means the model doesn't understand
    # the multi-threaded cache/memory dynamics of this codebase.
    if _session_micro_bench_wins >= 2 and _session_real_regressions >= 2:
        ratio = _session_real_regressions / max(_session_micro_bench_wins, 1)
        if ratio >= 0.5:
            parts.append(
                "== ADAPTIVE GUIDANCE: MICRO-BENCH UNRELIABILITY ==\n"
                f"You have had {_session_micro_bench_wins} micro-benchmark wins but "
                f"{_session_real_regressions} regressed in the real renderer. "
                "This codebase's single-threaded micro-benchmarks DO NOT predict "
                "multi-threaded performance. Before applying ANY micro-bench win:\n"
                "1. Explain WHY the change will survive 32-thread cache pressure.\n"
                "2. If you cannot explain it, do NOT apply — try a different strategy.\n"
                "3. Consider algorithmic changes (which scale) over micro-optimizations "
                "(which the compiler already does).\n"
                "4. Prefer strategies that REDUCE working set or IMPROVE locality.\n"
                "The codebase_context.md documents this pattern in detail — re-read "
                "the 'Techniques to Avoid' section."
            )

    # Pattern B: excessive restores without convergence
    restoreRecent = sum(1 for t in recentToolNames[-10:] if t == "restoreAll")
    if restoreRecent >= 3:
        _session_restore_count += restoreRecent
        parts.append(
            "== ADAPTIVE GUIDANCE: CONVERGENCE NEEDED ==\n"
            "You are repeatedly restoring changes. This means your approaches are "
            "not working. Instead of trying yet another variant:\n"
            "1. Call getRefinementState() on your current target function.\n"
            "2. Call getUntriedStrategies() to see what remains.\n"
            "3. If only low-impact strategies remain (instruction_level, branchless, "
            "loop_transform), call convergeFunction() and MOVE ON.\n"
            "4. If high-impact strategies remain untried, pick ONE and commit to it.\n"
            "STOP RESTORING AND RETRYING THE SAME APPROACH."
        )

    # Pattern C: stuck on same function too long
    itersSinceBench = iteration - lastBenchIteration if lastBenchIteration else iteration
    if itersSinceBench > 20:
        parts.append(
            "== ADAPTIVE GUIDANCE: MOVE ON ==\n"
            f"It has been {itersSinceBench} iterations since your last successful "
            "build+bench. You are deep in analysis paralysis. "
            "Call convergeFunction() on your current target and switch to a "
            "DIFFERENT hotspot from the PINNED_HOTSPOTS list. A fresh function "
            "may have easier wins than the one you are stuck on."
        )

    # Pattern D: using low-impact strategies when high-impact remain
    # Check if model is trying instruction_level/loop_transform/branchless
    # on functions where algorithmic/memory_layout haven't been tried
    lowImpactRecent = sum(1 for t in recentToolNames[-8:]
                          if t in ("createFuncBench", "runFuncBench"))
    if lowImpactRecent >= 3:
        # Check if any active refinement targets have untried high-impact strategies
        activeStates = refine.getAllRefinementStates()
        for name, rs in activeStates.items():
            if rs.get("converged"):
                continue
            tried = set(rs.get("strategies_tried", []))
            avoided = {a["strategy"] for a in rs.get("strategies_avoided", [])}
            highUntried = [s for s in refine.HIGH_IMPACT_STRATEGIES
                          if s not in tried and s not in avoided]
            if highUntried:
                parts.append(
                    f"== ADAPTIVE GUIDANCE: TRY HIGH-IMPACT FIRST ==\n"
                    f"Function '{name}' still has untried high-impact strategies: "
                    f"{', '.join(highUntried)}. These are likely to yield larger "
                    f"gains than micro-optimizations. Try algorithmic or memory "
                    f"layout changes before instruction-level tweaks."
                )
                break  # only mention once

    return "\n\n".join(parts) if parts else ""


if __name__ == "__main__":
    import argparse
    _parser = argparse.ArgumentParser(description="llmOpt agentic optimization loop")
    _parser.add_argument(
        "--research", action="store_true",
        help="Run the codebase research phase and save codebase_context.md, then exit."
    )
    _args = _parser.parse_args()

    ui.start()
    git_pull_project()
    gf.init()

    if _args.research:
        print("[main] Running codebase research phase …")
        runCodebaseResearch()
        print(f"[main] Research complete. Context saved to {CODEBASE_CONTEXT_FILE}")
        sys.exit(0)

    # Sync any leftover planner notes from the previous run into
    # codebase_context.md before wiping the board for a fresh session.
    syncPlannerToCodebaseContext()

    planner.resetBoard()

    getTree()
    # getTodos()
    buildProject()

    # Reuse a cached baseline + flame when the project HEAD is unchanged and the
    # tree is clean — avoids the costly 5x bench + perf flame cold start on restart.
    _cached = _loadBaselineCache()
    if _cached is not None:
        BASELINE_RESULTS, flame = _cached
        print("[main] Reused cached baseline + flame for current HEAD (skipped cold-start bench).")
        # Replay the bench/flame context entries the model expects to see.
        CONTEXT.append({
            "type": "tool_use", "tool": "makeBench", "input": None,
            "output": _benchSummary(BASELINE_RESULTS, None),
            "bench_results": {k: v for k, v in BASELINE_RESULTS.items()
                              if k not in ("frame_images", "frame_hashes")},
        })
        CONTEXT.append({
            "type": "tool_use", "tool": "makeFlame", "input": None, "output": flame,
        })
    else:
        _ , BASELINE_RESULTS = makeBench()
        flame = makeFlame()
        _saveBaselineCache(BASELINE_RESULTS, flame)

    # Pin top 5 hotspots so the model always knows what to target,
    # even after context compression trims the raw flame data.
    _applyFlameHotspots(flame)

    gf.listFunctions(context=CONTEXT)
    gf.apiHelp(context=CONTEXT)

    _SYSTEM_PROMPT, codebase_ctx = _buildSystemPrompt()
    if codebase_ctx:
        print(f"[main] Loaded codebase context ({len(codebase_ctx)} chars) from {CODEBASE_CONTEXT_FILE}")
        # The full report lives in the system prompt every turn; keep only a compact
        # pointer in the working context to avoid duplicating ~hundreds of lines of
        # tokens. The model can call getCodebaseContext(section=...) to re-read it.
        CONTEXT.insert(0, {
            "type": "codebase_knowledge",
            "tool": "loadCodebaseContext",
            "output": (
                "Codebase knowledge base is loaded in the system prompt (architecture, "
                "hotspots, TODOs, accumulated insights). Call getCodebaseContext() to re-read "
                "the full report, or getCodebaseContext('Performance-Critical Functions') / "
                "getCodebaseContext('Accumulated Insights') for a specific section."
            ),
        })
    else:
        print(f"[main] No codebase_context.md found. Run with --research to generate it.")

    TOOL_MAP = executor.buildToolMap(gf, planner, sys.modules[__name__], refine_module=refine)
    iteration = 0
    _recent_calls = []          # (tool, args_str) tuples for loop detection
    _recent_tool_names = []     # tool names only, for broader pattern detection
    _consecutive_prose_only = 0 # count of responses with no tool calls
    _last_pr_iteration = 0      # iteration when last PR was created
    _last_build_bench_iteration = 0  # iteration when build+bench last succeeded
    _plan_at_iteration = random.randint(5, 8)  # next planning iteration

    while True:
        iteration += 1
        ui.set_iteration(iteration)
        ui.set_status("running")
        removeStaffFromContext(CONTEXT_MAX_TOKENS)

        # inject nudge if UI button was pressed or loop detected
        nudge = ui.pop_nudge()
        if nudge:
            CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
            _recent_calls.clear()
            _recent_tool_names.clear()
            print(f"[nudge] {nudge}")

        xmlContext, tokenCount = convertContextToXml()
        ui.set_token_count(tokenCount)
        ui.sync_context(CONTEXT)
        board = planner.showBoard(returnString=True)
        ui.sync_board(board)

        # Build session state header
        iters_since_pr = iteration - _last_pr_iteration if _last_pr_iteration else iteration
        iters_since_bench = iteration - _last_build_bench_iteration if _last_build_bench_iteration else iteration
        diff = gf.getDiff(returnString=True, code_only=True)
        has_changes = bool(diff and diff.strip())

        # Refinement state summary
        refinementSummary = refine.getGlobalRefinementSummary()
        calibrationGuidance = refine.getCalibrationGuidance()
        refinementSection = ""
        if refinementSummary or calibrationGuidance:
            refinementSection = "\n== REFINEMENT STATE ==\n"
            if refinementSummary:
                refinementSection += refinementSummary + "\n"
            if calibrationGuidance:
                refinementSection += calibrationGuidance + "\n"

        session_header = (
            f"== SESSION STATE ==\n"
            f"Total iterations: {iteration}\n"
            f"Iterations since last PR: {iters_since_pr}\n"
            f"Iterations since last successful build+bench: {iters_since_bench}\n"
            f"Uncommitted changes: {'YES -- call getDiff() to review' if has_changes else 'none'}\n"
            + refinementSection
            + (PINNED_HOTSPOTS + "\n" if PINNED_HOTSPOTS else "")
        )

        # Staleness warning
        staleness_warning = ""
        if iters_since_bench > 15:
            staleness_warning = (
                f"\n*** WARNING: {iters_since_bench} iterations without a successful "
                "build+bench cycle. You may be stuck in analysis paralysis. "
                "If you have a change ready, apply it now with searchReplace() and call buildProject(). "
                "If not, pick the simplest possible improvement and try it. ***"
            )
        elif iters_since_bench > 8:
            staleness_warning = (
                f"\n(Note: {iters_since_bench} iterations since last build+bench. "
                "Consider making a concrete change soon.)"
            )

        # Adaptive guidance based on session patterns
        adaptive_guidance = _buildAdaptiveGuidance(
            _recent_tool_names, iteration, _last_build_bench_iteration
        )

        # Prose-only nudge
        prose_nudge = ""
        if _consecutive_prose_only >= 2:
            prose_nudge = (
                "\n*** Your last responses had NO tool calls. You MUST emit JSON tool blocks "
                "to take action. Example:\n"
                "```json\n{\"tool\": \"showContext\", \"args\": {\"func\": \"renderFrame\", \"depth\": 2}}\n```\n"
                "Do not just describe what you would do -- actually call the tools. ***"
            )

        # Decide whether this is a planning iteration
        is_plan_iteration = (iteration >= _plan_at_iteration and iters_since_pr >= 5)
        if is_plan_iteration:
            _plan_at_iteration = iteration + random.randint(5, 8)

        if is_plan_iteration:
            prompt = (
                PLAN_PROMPT
                + "\n\n" + session_header
                + "\n\n== CURRENT BOARD ==\n" + board
                + "\n\nContext:\n" + xmlContext
            )
            print(f"[plan] Iteration {iteration}: injecting planning step (next plan at {_plan_at_iteration})")
        else:
            prompt = (
                getSystemPrompt()
                + "\n\n" + session_header
                + staleness_warning
                + ("\n\n" + adaptive_guidance if adaptive_guidance else "")
                + prose_nudge
                + "\n== CURRENT BOARD ==\n" + board
                + "\n\nContext:\n" + xmlContext
            )
            print(f"Iteration {iteration}: token count ~{tokenCount}")

        ui.set_status("waiting_model")
        steer = ui.pop_steer()
        _cleared = [False]
        def _on_token(delta):
            if not _cleared[0]:
                ui.clear_stream()
                _cleared[0] = True
            ui.push_token(delta)

        try:
            if is_plan_iteration:
                response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="baidu/fp8")
            else:
                response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="baidu/fp8")
        except Exception as e:
            CONTEXT.append({
                "type": "model_response",
                "iteration": iteration,
                "output": f"Error getting model response: {e} try again in the next iteration."
            })
            continue

        ui.set_last_response(response)
        print("Model response:", response)
        if response is None:
            CONTEXT.append({
                "type": "model_response",
                "iteration": iteration,
                "output": "Error: model returned no response, try again in the next iteration."
            })
            continue

        response_for_context = re.sub(r"<think>.*?</think>", "", response, flags=re.DOTALL).strip()
        entry_type = "planning" if is_plan_iteration else "model_response"
        CONTEXT.append({
            "type": entry_type,
            "iteration": iteration,
            "output": response_for_context
        })
        ui.set_status("running")

        results = executor.executeAll(response, TOOL_MAP, context=CONTEXT)

        # Track tool usage patterns
        _pending_micro_bench_win = False  # set when runFuncBench succeeds
        for r in results:
            ui.log_tool_result(r["tool"], r["error"])
            tool_name = r["tool"]
            _recent_tool_names.append(tool_name)
            args_str = str(sorted((r.get("args") or {}).items()) if isinstance(r.get("args"), dict) else "")
            _recent_calls.append((tool_name, args_str))
            # Track build+bench success
            if tool_name == "makeBench" and r["error"] is None:
                _last_build_bench_iteration = iteration
                # Check the bench output for regression vs baseline
                output_str = str(r.get("result", ""))
                if "REGRESSED" in output_str or "PERFORMANCE REGRESSED" in output_str:
                    _session_real_regressions += 1
            if tool_name == "buildProject" and r["error"] is None:
                pass  # bench must also succeed to count
            if tool_name == "createPR" and r["error"] is None:
                _last_pr_iteration = iteration
                _plan_at_iteration = iteration + 3  # plan soon after a PR
            # Track micro-bench wins
            if tool_name == "runFuncBench" and r["error"] is None:
                output_str = str(r.get("result", ""))
                # Look for speedup indicators in the output
                if any(phrase in output_str.lower() for phrase in
                       ("speedup", "faster", "improvement", "ns/call", "ms/call")):
                    _session_micro_bench_wins += 1
            # Track restores
            if tool_name == "restoreAll":
                _session_restore_count += 1
                _clearEditStack()

            # Capture edit snapshots for bisection
            if tool_name in ("searchReplace", "searchReplaceMulti", "applyChange",
                             "replaceLines", "insertLines", "deleteLines", "applyPatch"):
                if r["error"] is None:
                    desc = str(r.get("tool", "edit")) + ": " + str(r.get("result", ""))[:100]
                    _captureEditSnapshot(desc)

            # Clear edit stack on successful PR (changes committed)
            if tool_name == "createPR" and r["error"] is None:
                _clearEditStack()

        _recent_calls = _recent_calls[-12:]
        _recent_tool_names = _recent_tool_names[-20:]

        # Track prose-only responses
        if results:
            _consecutive_prose_only = 0
        else:
            _consecutive_prose_only += 1
            if _consecutive_prose_only >= 3:
                print(f"[stall] {_consecutive_prose_only} consecutive prose-only responses")

        # --- LOOP DETECTION ---

        # Pattern 1: same tool+args 4 times in a row
        if len(_recent_calls) >= 4 and len(set(_recent_calls[-4:])) == 1:
            loop_tool = _recent_calls[-1][0]
            nudge = (
                f"[AUTO-NUDGE] You have called '{loop_tool}' with the same arguments "
                f"4 times in a row. You are stuck in a loop. "
                f"Step 1: call addNote() to record what you have learned so far and WHY you think it is slow. "
                f"Step 2: call addTask() with the concrete change you plan to make. "
                f"Step 3: apply the change with searchReplace() or applyChange(). "
                f"Do NOT call {loop_tool} again until steps 1-3 are done."
            )
            CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
            _recent_calls.clear()
            _recent_tool_names.clear()
            print(f"[loop-detect] injected nudge for repeated '{loop_tool}'")

        # Pattern 2: readSourceFile or showContext called 5+ times in last 8 without any edit
        if len(_recent_tool_names) >= 8:
            recent_8 = _recent_tool_names[-8:]
            read_count = sum(1 for t in recent_8 if t in ("readSourceFile", "showContext", "showSrc", "showSrcPair"))
            edit_count = sum(1 for t in recent_8 if t in ("searchReplace", "searchReplaceMulti", "applyChange",
                                                           "insertLines", "deleteLines", "applyPatch"))
            if read_count >= 5 and edit_count == 0:
                nudge = (
                    "[AUTO-NUDGE] You have read source files 5+ times in recent iterations "
                    "without making any changes. You are over-researching. "
                    "Pick ONE hotspot function and call createFuncBench() to write a "
                    "micro-benchmark with the original + your optimized variant. "
                    "Prove the optimization works, then apply to main code."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                _recent_tool_names.clear()
                print("[loop-detect] injected nudge for over-reading without editing")

        # Pattern 3: buildProject or makeBench called 4+ times without edits in between
        if len(_recent_tool_names) >= 6:
            recent_6 = _recent_tool_names[-6:]
            build_bench_count = sum(1 for t in recent_6 if t in ("buildProject", "makeBench"))
            edit_count_6 = sum(1 for t in recent_6 if t in ("searchReplace", "searchReplaceMulti", "applyChange",
                                                              "insertLines", "deleteLines", "applyPatch"))
            if build_bench_count >= 4 and edit_count_6 == 0:
                nudge = (
                    "[AUTO-NUDGE] You have called buildProject/makeBench multiple times "
                    "without applying any code changes. This wastes time. "
                    "If you are trying to understand the baseline, look at the bench results "
                    "already in context. If you want to make a change, use searchReplace()."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                _recent_tool_names.clear()
                print("[loop-detect] injected nudge for build/bench without edits")

        # Pattern 4: editing main code without first using createFuncBench/runFuncBench
        if len(_recent_tool_names) >= 3:
            recent_all = _recent_tool_names[-10:]
            edit_tools = EDITING_TOOLS
            bench_tools = ("createFuncBench", "runFuncBench")
            has_edit = any(t in edit_tools for t in recent_all[-3:])
            has_bench = any(t in bench_tools for t in recent_all[-10:])
            if has_edit and not has_bench:
                nudge = (
                    "[AUTO-NUDGE] You are editing the main codebase WITHOUT first proving "
                    "your optimization in a micro-benchmark. The workflow is ISOLATION-FIRST:\n"
                    "1. Read the hot function code\n"
                    "2. createFuncBench() with original + optimized variants\n"
                    "3. runFuncBench() to prove speedup and correctness\n"
                    "4. Only THEN apply to main code with searchReplace()\n"
                    "If you already ran a bench and it was trimmed from context, call addNote() "
                    "to record the result and proceed. Otherwise, use createFuncBench() first."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                print("[loop-detect] injected nudge for editing without bench-first")

        # Pattern 5: refinement stagnation — same function, repeated failures, not converging
        if _recent_tool_names and len(_recent_tool_names) >= 8:
            recent_8 = _recent_tool_names[-8:]
            # Count exploration + bench tools that suggest working on same function
            explore_bench = sum(1 for t in recent_8
                               if t in ("showContext", "showSrc", "showSrcPair",
                                        "createFuncBench", "runFuncBench", "getRefinementState"))
            restore_count = sum(1 for t in recent_8 if t == "restoreAll")
            converge_count = sum(1 for t in recent_8 if t == "convergeFunction")
            if explore_bench >= 5 and restore_count >= 2 and converge_count == 0:
                # Model keeps trying and restoring on same function without converging
                # Find active refinement targets that are stalling
                activeStates = refine.getAllRefinementStates()
                stalling = []
                for name, rs in activeStates.items():
                    if not rs.get("converged"):
                        attempts = len(rs.get("attempts", []))
                        failures = sum(1 for a in rs.get("attempts", []) if not a.get("success"))
                        if failures >= 3:
                            stalling.append(f"{name} ({failures}/{attempts} failed)")
                if stalling:
                    nudge = (
                        "[AUTO-NUDGE] You have been working on the same function(s) with "
                        "repeated failures and restores. Use the refinement tools to break "
                        "the cycle:\n"
                        f"Stalling functions: {', '.join(stalling)}\n"
                        "1. Call getRefinementState() to review what has been tried.\n"
                        "2. Call getUntriedStrategies() to see what approaches remain.\n"
                        "3. If no promising strategies remain, call convergeFunction() to "
                        "move on — further optimization yields diminishing returns.\n"
                        "4. If strategies remain, pick the highest-impact untried one "
                        "and create a micro-benchmark with createFuncBench().\n"
                        "DO NOT restoreAll() and retry the same approach."
                    )
                    CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                    print("[loop-detect] injected nudge for refinement stagnation")

        ui.sync_context(CONTEXT)
        ui.sync_diff(gf.getDiff(returnString=True, code_only=True))

        removeDoublesFromContext()