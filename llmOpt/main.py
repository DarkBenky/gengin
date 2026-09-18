"""gengin domain library: sandbox build/bench/profile/PR helpers.

Used by mcp_server.py.  Everything operates on PROJECT_DIR (the llmOpt/gengin
sandbox); file editing is the driving harness's job.
"""

import base64
import hashlib
import json
import os
import re
import shutil
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

# Tracked files that are regenerated per checkout; not counted as tree dirt.
_GENERATED_ARTIFACTS = {"compile_commands.json"}


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


def _llmopt_dir():
    return os.path.dirname(os.path.abspath(__file__))


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def generate_inputs_manifest(inputs_dir):
    """Write inputs_dir/manifest.json: name, size, sha256 for every file.

    Used by VM provisioning to record the exact prebuilt inputs a sandbox
    replacement must reproduce. Returns the manifest hash (sha256 of the
    canonical manifest body) for session summaries.
    """
    inputs_dir = os.path.realpath(inputs_dir)
    entries = []
    for dirpath, _dirnames, filenames in os.walk(inputs_dir):
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, inputs_dir)
            if rel == "manifest.json":
                continue
            entries.append({
                "name": rel.replace(os.sep, "/"),
                "size": os.path.getsize(full),
                "sha256": _sha256_file(full),
            })
    entries.sort(key=lambda e: e["name"])
    body = json.dumps(entries, sort_keys=True)
    manifest_hash = hashlib.sha256(body.encode()).hexdigest()
    with open(os.path.join(inputs_dir, "manifest.json"), "w") as f:
        json.dump({"schemaVersion": 1, "files": entries}, f, indent=2, sort_keys=True)
        f.write("\n")
    return manifest_hash


def _sync_inputs(sandbox, inputs_dir):
    """Copy manifest-verified inputs into the sandbox.

    Every required file must exist with a matching size and sha256; a missing
    or mismatched file is a hard failure (no partial copies).
    """
    inputs_dir = os.path.realpath(inputs_dir)
    manifest_path = os.path.join(inputs_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        raise RuntimeError(f"inputs manifest not found: {manifest_path}")
    with open(manifest_path) as f:
        manifest = json.load(f)
    for entry in manifest.get("files", []):
        rel = entry["name"]
        src = os.path.join(inputs_dir, rel)
        dst = os.path.join(sandbox, rel)
        if not os.path.isfile(src):
            raise RuntimeError(f"required input missing: {rel}")
        if os.path.getsize(src) != entry["size"] or _sha256_file(src) != entry["sha256"]:
            raise RuntimeError(f"input hash mismatch: {rel}")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)


def _sync_inputs_legacy(sandbox):
    """Fallback: rsync gitignored build inputs from the parent checkout.

    Used only when no GENGIN_INPUTS_DIR is configured (local development).
    """
    root = _llmopt_dir()
    parent = os.path.dirname(root)
    for rel in ("deps", "assets", ".flamegraph"):
        run(["rsync", "-a", "--ignore-missing-args", f"{parent}/{rel}/", f"{sandbox}/{rel}/"])
    run(["rsync", "-a", "--ignore-missing-args", f"{parent}/default.profdata", f"{sandbox}/default.profdata"])


def _structural_checks(sandbox):
    """Verify expected source and asset files exist in the prepared sandbox."""
    required = [
        "main.c", "Makefile", "render/cpu/ray.c", "object/object.h",
        "util/bench.h", "client/client.h", "deps/minifb/include/MiniFB.h",
    ]
    missing = [rel for rel in required if not os.path.exists(os.path.join(sandbox, rel))]
    if missing:
        raise RuntimeError(f"prepared sandbox missing required files: {missing}")


def _is_ancestor(sha, branch, cwd):
    """Return True if sha is reachable from origin/branch, else False/None."""
    try:
        result = subprocess.run(
            ["git", "merge-base", "--is-ancestor", sha, f"origin/{branch}"],
            capture_output=True, cwd=cwd,
        )
        return result.returncode == 0
    except (subprocess.SubprocessError, OSError):
        return None


def git_pull_project(repo_url, branch, target_sha, inputs_dir=None, session_id=None):
    """Prepare an exact-SHA sandbox and atomically replace the current one.

    Clones the branch into a temporary sibling, checks out target_sha detached,
    verifies HEAD matches exactly, syncs manifest-verified inputs, generates
    compile_commands.json, runs structural checks, then swaps the prepared
    sandbox into place. On any failure the temporary directory is removed and
    the previous sandbox is preserved.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", target_sha):
        raise RuntimeError(f"invalid target sha: {target_sha!r}")

    root = _llmopt_dir()
    sandbox = os.path.realpath(os.path.join(root, "gengin"))
    # The sandbox must be exactly the expected child of llmOpt.
    if os.path.dirname(sandbox) != os.path.realpath(root):
        raise RuntimeError(f"refusing to touch unexpected sandbox path: {sandbox}")

    tag = session_id or "manual"
    prepare = os.path.realpath(os.path.join(root, f"gengin.prepare-{tag}"))
    backup = os.path.realpath(os.path.join(root, f"gengin.backup-{tag}"))
    for path in (prepare, backup):
        if os.path.exists(path):
            shutil.rmtree(path)

    try:
        run(["git", "clone", "--branch", branch, "--no-checkout", repo_url, prepare])
        # Ensure the target commit is present (it may not be the branch tip).
        probe = subprocess.run(
            ["git", "cat-file", "-e", f"{target_sha}^{{commit}}"],
            capture_output=True, cwd=prepare,
        )
        if probe.returncode != 0:
            run(["git", "fetch", "origin", target_sha], cwd=prepare)
        run(["git", "checkout", "--detach", target_sha], cwd=prepare)

        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=prepare
        ).stdout.strip()
        if head != target_sha:
            raise RuntimeError(f"checkout mismatch: HEAD={head} expected={target_sha}")

        descendant = _is_ancestor(target_sha, branch, prepare)

        if inputs_dir:
            _sync_inputs(prepare, inputs_dir)
        else:
            _sync_inputs_legacy(prepare)

        _structural_checks(prepare)

        # Atomic swap: move current sandbox aside, move prepared into place.
        if os.path.exists(sandbox):
            os.rename(sandbox, backup)
        try:
            os.rename(prepare, sandbox)
        except BaseException:
            if os.path.exists(backup):
                os.rename(backup, sandbox)
            raise
        shutil.rmtree(backup, ignore_errors=True)

        # Generate compile_commands.json against the FINAL path so clangd gets
        # usable absolute paths (the committed copy points at another checkout).
        try:
            import gen_compile_commands
            gen_compile_commands.generate(sandbox)
        except Exception as exc:  # non-fatal: clangd degrades, build does not
            print(f"[gen_compile_commands] {exc}", file=sys.stderr)

        return {"targetSha": target_sha, "descendantOfBranch": descendant}
    except BaseException:
        shutil.rmtree(prepare, ignore_errors=True)
        if os.path.exists(backup) and not os.path.exists(sandbox):
            os.rename(backup, sandbox)
        raise


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
        _head, _dirty = _projectGitHead()
        if _dirty:
            raise RuntimeError(
                "No valid clean baseline is available and the working tree is "
                "dirty, so this run cannot be used as a baseline. Run make_bench "
                "on a clean checkout first (or let the supervisor prepare the "
                "clean baseline before agent edits)."
            )
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


def _openclFingerprint():
    try:
        out = subprocess.run(["clinfo", "-l"], capture_output=True, text=True, timeout=10).stdout
        return hashlib.sha256(out.encode()).hexdigest() if out.strip() else "none"
    except Exception:
        return "unknown"


def _mesaRenderer():
    try:
        out = subprocess.run(["glxinfo", "-B"], capture_output=True, text=True, timeout=10).stdout
        for line in out.splitlines():
            if "OpenGL renderer" in line:
                return line.split(":", 1)[1].strip()
        return "unknown"
    except Exception:
        return "unknown"


def environmentFingerprint():
    """Hash of the environment inputs that determine benchmark results.

    A cached baseline is only reusable when this fingerprint matches, so a VM
    resize, compiler upgrade, OpenCL device change, input-manifest change, or
    Makefile/flag change invalidates it. Best-effort: an unavailable probe
    records a stable sentinel rather than failing.
    """
    parts = {}
    head, _dirty = _projectGitHead()
    parts["git_sha"] = head or "unknown"

    inputs_dir = os.environ.get("GENGIN_INPUTS_DIR", "")
    manifest = os.path.join(inputs_dir, "manifest.json") if inputs_dir else ""
    if manifest and os.path.exists(manifest):
        try:
            with open(manifest) as f:
                parts["input_manifest"] = hashlib.sha256(
                    json.dumps(json.load(f), sort_keys=True).encode()).hexdigest()
        except (OSError, json.JSONDecodeError):
            parts["input_manifest"] = "unreadable"
    else:
        parts["input_manifest"] = "none"

    makefile = os.path.join(PROJECT_DIR, "Makefile")
    if os.path.exists(makefile):
        with open(makefile, "rb") as f:
            parts["makefile"] = hashlib.sha256(f.read()).hexdigest()
    else:
        parts["makefile"] = "none"

    try:
        parts["compiler"] = subprocess.run(
            ["clang", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        parts["compiler"] = "unknown"

    try:
        with open("/proc/cpuinfo") as f:
            cpuinfo = f.read()
        model = next((l.split(":", 1)[1].strip()
                      for l in cpuinfo.splitlines() if l.startswith("model name")), "unknown")
        parts["cpu"] = f"{model}|{os.cpu_count()}"
    except Exception:
        parts["cpu"] = "unknown"

    try:
        parts["kernel"] = os.uname().release
    except Exception:
        parts["kernel"] = "unknown"
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") as f:
            parts["governor"] = f.read().strip()
    except Exception:
        parts["governor"] = "unknown"

    parts["opencl"] = _openclFingerprint()
    parts["mesa"] = _mesaRenderer()
    parts["bench"] = f"runs={BENCH_RUNS}|duration={os.environ.get('BENCH_DURATION', '10.0')}"

    return hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()


def _projectGitHead():
    """Return (head_hash, is_dirty) for PROJECT_DIR, or (None, True) on failure.

    Expected generated artifacts (compile_commands.json is regenerated per
    sandbox with machine-specific absolute paths) do not count as dirt; agent
    source edits still do.
    """
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
        changed = [line for line in dirty.stdout.splitlines()
                   if line[3:].strip() not in _GENERATED_ARTIFACTS]
        return head.stdout.strip(), bool(changed)
    except Exception:
        return None, True


def _loadBaselineCache():
    """Return the cached clean-HEAD baseline if SHA and fingerprint match.

    A dirty working tree does NOT block loading: the baseline was captured on
    clean HEAD, so it stays valid after the agent edits the tree. Only a SHA
    or environment-fingerprint mismatch invalidates it.
    """
    if not os.path.exists(BASELINE_CACHE_FILE):
        return None
    head, _dirty = _projectGitHead()
    if head is None:
        return None
    try:
        with open(BASELINE_CACHE_FILE) as fh:
            cache = json.load(fh)
    except (json.JSONDecodeError, OSError):
        return None
    if cache.get("head") != head:
        return None
    if cache.get("fingerprint") and cache["fingerprint"] != environmentFingerprint():
        return None
    return cache.get("baseline")


def _saveBaselineCache(baseline):
    """Persist the baseline keyed by clean HEAD + environment fingerprint.

    Only called from a clean tree; writes atomically.
    """
    head, dirty = _projectGitHead()
    if head is None or dirty:
        return  # don't cache a baseline taken against a dirty tree
    tmp = BASELINE_CACHE_FILE + f".tmp.{os.getpid()}"
    try:
        with open(tmp, "w") as fh:
            json.dump({"head": head, "fingerprint": environmentFingerprint(),
                       "baseline": baseline}, fh)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, BASELINE_CACHE_FILE)
    except OSError as e:
        print(f"[baseline-cache] save failed: {e}", file=sys.stderr)
        try:
            os.unlink(tmp)
        except OSError:
            pass


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

def _githubRepo():
    """Return (owner, repo) parsed from the sandbox origin remote URL."""
    res = subprocess.run(
        ["git", "remote", "get-url", "origin"],
        capture_output=True, text=True, cwd=PROJECT_DIR,
    )
    remote = res.stdout.strip()
    m = re.search(r'[:/]([^/]+/[^/]+?)(?:\.git)?$', remote)
    if not m:
        raise RuntimeError(f"Cannot parse repo from remote URL: {remote}")
    owner, _, repo = m.group(1).partition("/")
    return owner, repo


def _githubHeaders():
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        raise RuntimeError("GITHUB_TOKEN not set in environment or .env")
    return {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "Content-Type": "application/json",
        "X-GitHub-Api-Version": "2022-11-28",
    }


def _github_find_pr(branch):
    """Return the open PR URL for branch, or None."""
    import urllib.request, json as _json
    owner, repo = _githubRepo()
    url = (f"https://api.github.com/repos/{owner}/{repo}/pulls"
           f"?head={owner}:{branch}&state=open")
    req = urllib.request.Request(url, headers=_githubHeaders())
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = _json.loads(r.read())
        return data[0]["html_url"] if data else None
    except Exception:
        return None


def _github_create_pr(title, body, head, base="main"):
    import urllib.request, json as _json, urllib.error
    owner, repo = _githubRepo()
    payload = _json.dumps({"title": title, "body": body, "head": head, "base": base}).encode()
    req = urllib.request.Request(
        f"https://api.github.com/repos/{owner}/{repo}/pulls",
        data=payload,
        headers=_githubHeaders(),
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = _json.loads(r.read())
        return data["html_url"]
    except urllib.error.HTTPError as e:
        if e.code == 422:  # validation failed — likely an existing PR for this head
            existing = _github_find_pr(head)
            if existing:
                return existing
        raise


_TARGET_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_BRANCH_RE = re.compile(r"^llmopt/[0-9a-f]{8}/[A-Za-z0-9._-]+$")
_FORBIDDEN_STAGING_RE = re.compile(
    r"(^|/)(\.env|\.hermes|state|logs|run)(/|$)"
    r"|(^|/)baseline_cache\.json$"
    r"|(^|/)codebase_context\.md$"
    r"|\.perf\.data$|\.profdata$|\.profraw$"
    r"|^build/|^bench/results/|^\.flamegraph/"
    r"|(^|/)(flamegraph|callgraph|icicle)\.svg$"
)
_SECRET_RE = re.compile(r"sk-or-v1-[0-9a-fA-F]{32,}|ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{22,}")


def _sandboxHead():
    res = subprocess.run(["git", "rev-parse", "HEAD"],
                         capture_output=True, text=True, cwd=PROJECT_DIR)
    return res.stdout.strip() if res.returncode == 0 else ""


def _branchExists(branch):
    res = subprocess.run(["git", "rev-parse", "--verify", "--quiet", branch],
                         capture_output=True, text=True, cwd=PROJECT_DIR)
    return res.returncode == 0


def _remoteBranchSha(branch):
    res = subprocess.run(["git", "ls-remote", "origin", f"refs/heads/{branch}"],
                         capture_output=True, text=True, cwd=PROJECT_DIR, timeout=120)
    line = res.stdout.strip()
    return line.split()[0] if line else None


def createPR(title, body, branch="", commit_msg=None):
    """Commit sandbox changes, push one focused branch, open a PR.

    Guards: target-SHA ancestry, forbidden staging paths, secret patterns,
    empty diff, and idempotent branch/PR reuse. Never force-pushes. Returns
    the PR URL.
    """
    commit_msg = commit_msg or title
    target = os.environ.get("GENGIN_TARGET_SHA", "")
    session_id = os.environ.get("GENGIN_SESSION_ID", "")

    head = _sandboxHead()
    if not head:
        raise RuntimeError("cannot determine sandbox HEAD")

    if target and _TARGET_SHA_RE.fullmatch(target):
        rc = subprocess.run(["git", "merge-base", "--is-ancestor", target, head],
                            capture_output=True, cwd=PROJECT_DIR)
        if rc.returncode != 0:
            raise RuntimeError(
                f"sandbox HEAD {head[:12]} does not descend from target "
                f"{target[:12]}; refusing to create a PR")

    if not branch:
        if not (target and session_id):
            raise RuntimeError(
                "branch name required outside supervised sessions "
                "(set GENGIN_TARGET_SHA and GENGIN_SESSION_ID)")
        branch = f"llmopt/{target[:8]}/{session_id.rsplit('-', 1)[-1]}"
    if not _BRANCH_RE.fullmatch(branch):
        raise RuntimeError(f"branch must match llmopt/<8-hex-sha>/<id>: got {branch!r}")

    # Switch to (or create) the branch before staging so staged changes carry over.
    if _branchExists(branch):
        run(["git", "checkout", branch], cwd=PROJECT_DIR)
    else:
        run(["git", "checkout", "-b", branch], cwd=PROJECT_DIR)

    run(["git", "add", "-A"], cwd=PROJECT_DIR)
    cached = subprocess.run(["git", "diff", "--cached", "--name-only"],
                            capture_output=True, text=True, cwd=PROJECT_DIR).stdout.split()
    forbidden = [f for f in cached if _FORBIDDEN_STAGING_RE.search(f)]
    if forbidden:
        run(["git", "reset", "-q", "--"] + forbidden, cwd=PROJECT_DIR)
        print(f"[createPR] excluded from staging: {forbidden}", file=sys.stderr)
    staged = [f for f in cached if f not in forbidden]
    if not staged:
        # Retry path: the branch may already carry the commit and an open PR.
        existing = _github_find_pr(branch)
        if existing:
            print(f"PR already exists: {existing}", file=sys.stderr)
            return existing
        raise RuntimeError("empty source diff; refusing to create an empty commit or PR")

    diff_text = subprocess.run(["git", "diff", "--cached", "-U0"],
                               capture_output=True, text=True, cwd=PROJECT_DIR).stdout
    if _SECRET_RE.search(diff_text):
        raise RuntimeError("staged diff contains a secret-looking token; refusing to commit")

    run(["git", "commit", "-m", commit_msg], cwd=PROJECT_DIR)

    # Push without force; tolerate an existing remote branch with identical content.
    push = subprocess.run(["git", "push", "-u", "origin", branch],
                          capture_output=True, text=True, cwd=PROJECT_DIR)
    if push.returncode != 0:
        remote_sha = _remoteBranchSha(branch)
        local_sha = _sandboxHead()
        if remote_sha and remote_sha == local_sha:
            print("[createPR] remote branch already up to date", file=sys.stderr)
        else:
            raise RuntimeError(
                f"push failed and remote branch differs (no force-push allowed):\n"
                f"{push.stderr}")

    url = _github_create_pr(title, body, head=branch)
    print(f"PR created: {url}", file=sys.stderr)
    return url
