import subprocess
import os
from collections import defaultdict

FLAMEGRAPH_DIR = ".flamegraph"
PERF_DATA = "perf.data"
FOLDED_FILE = "perf_folded.txt"

TOP_FUNCTIONS = 25
TOP_PATHS = 20


def _run(cmd, **kwargs):
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)}\n{result.stderr}")
    return result.stdout


def recordPerf(duration: int = 10, cwd: str = "."):
    """perf record for `duration` seconds, saves perf.data in cwd."""
    _run(["make", "flame_record", f"PERF_DURATION={duration}"], cwd=cwd)


def _ensureFlameGraph(cwd: str):
    fg = os.path.join(cwd, FLAMEGRAPH_DIR)
    if not os.path.isdir(fg):
        _run(["git", "clone", "--depth=1",
              "https://github.com/brendangregg/FlameGraph", fg])


def _getFoldedStacks(cwd: str) -> str:
    """Run perf script | stackcollapse-perf.pl, return folded text."""
    _ensureFlameGraph(cwd)
    collapse = os.path.join(cwd, FLAMEGRAPH_DIR, "stackcollapse-perf.pl")
    perf_data = os.path.join(cwd, PERF_DATA)

    script = subprocess.run(
        ["sudo", "perf", "script", "-f", "-i", PERF_DATA],
        capture_output=True, text=True, cwd=cwd
    )
    if not script.stdout:
        raise RuntimeError(
            f"perf script produced no output.\n"
            f"returncode={script.returncode}\nstderr={script.stderr[:500]}"
        )

    collapse_proc = subprocess.run(
        ["perl", os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl")],
        input=script.stdout, capture_output=True, text=True, cwd=cwd
    )
    if not collapse_proc.stdout:
        raise RuntimeError(
            f"stackcollapse-perf.pl produced no output.\n"
            f"returncode={collapse_proc.returncode}\nstderr={collapse_proc.stderr[:500]}\n"
            f"perf script lines={script.stdout.count(chr(10))}"
        )
    folded = collapse_proc.stdout

    with open(os.path.join(cwd, FOLDED_FILE), "w") as f:
        f.write(folded)

    return folded


def _parseFolded(folded: str):
    """
    Parse folded stack format into:
      - inclusive/exclusive sample counts per function
      - raw paths with counts
    """
    inclusive = defaultdict(int)
    exclusive = defaultdict(int)
    paths = []
    total = 0

    for line in folded.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.rsplit(" ", 1)
        if len(parts) != 2:
            continue
        stack_str, count_str = parts
        try:
            count = int(count_str)
        except ValueError:
            continue

        frames = stack_str.split(";")
        total += count
        paths.append((frames, count))

        seen = set()
        for fn in frames:
            if fn not in seen:
                inclusive[fn] += count
                seen.add(fn)
        if frames:
            exclusive[frames[-1]] += count

    return inclusive, exclusive, paths, total


def _hotFunctions(inclusive, exclusive, total, n=TOP_FUNCTIONS):
    fns = sorted(inclusive.keys(), key=lambda f: inclusive[f], reverse=True)[:n]
    return [
        {
            "fn": f,
            "inclusive_pct": round(inclusive[f] / total * 100, 1),
            "exclusive_pct": round(exclusive.get(f, 0) / total * 100, 1),
        }
        for f in fns
    ]


def _hotPaths(paths, total, n=TOP_PATHS):
    paths_sorted = sorted(paths, key=lambda x: x[1], reverse=True)[:n]
    return [
        {
            "stack": frames,
            "pct": round(count / total * 100, 1),
            "samples": count,
        }
        for frames, count in paths_sorted
    ]


def getPerfData(cwd: str = ".") -> dict:
    """
    Produce LLM-friendly perf summary from existing perf.data in cwd.
    Returns dict with hot_functions and hot_paths.
    """
    perf_data = os.path.join(cwd, PERF_DATA)
    if not os.path.exists(perf_data):
        raise FileNotFoundError(f"No perf.data found in {cwd}. Run make flame first.")

    folded = _getFoldedStacks(cwd)
    inclusive, exclusive, paths, total = _parseFolded(folded)

    if total == 0:
        raise RuntimeError("No samples found in perf data.")

    return {
        "total_samples": total,
        "hot_functions": _hotFunctions(inclusive, exclusive, total),
        "hot_paths": _hotPaths(paths, total),
    }
