#!/usr/bin/env python3
"""Reconstruct the make_bench verdict from bench_results.json writes.

The dsh MCP client times out before the harness finishes its 5 bench runs, so
the tool result is lost while the server-side work completes.  This watcher
snapshots each run the harness writes and replays main.py's own aggregation and
summary on them, against the same pinned baseline cache.
"""
import json
import os
import sys
import time

LLMOPT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, LLMOPT)
import main as H  # noqa: E402

RESULTS = os.path.join(H.PROJECT_DIR, "bench", "results", "bench_results.json")
SCALAR = ["frames", "avg_ms", "median_ms", "p99_ms"]


def load_json(path):
    for _ in range(20):
        try:
            with open(path) as fh:
                return json.load(fh)
        except (json.JSONDecodeError, ValueError):
            time.sleep(0.25)
    return None


def watch(runs_wanted, timeout_s):
    start = time.time()
    runs = []
    last_key = None
    while len(runs) < runs_wanted and time.time() - start < timeout_s:
        try:
            st = os.stat(RESULTS)
        except FileNotFoundError:
            time.sleep(0.5)
            continue
        if st.st_mtime < start - 2:
            time.sleep(0.4)
            continue
        # wait for the 41 MB write to settle before parsing
        stable = False
        for _ in range(30):
            time.sleep(0.4)
            try:
                st2 = os.stat(RESULTS)
            except FileNotFoundError:
                break
            if abs(st2.st_mtime - st.st_mtime) < 1e-6 and st2.st_size == st.st_size:
                stable = True
                break
            st = st2
        if not stable:
            continue
        key = (round(st.st_mtime, 3), st.st_size)
        if key == last_key:
            continue
        last_key = key
        data = load_json(RESULTS)
        if data is None:
            continue
        runs.append(data)
        print("run %d: frames=%s avg=%.3f median=%.3f p99=%.3f" % (
            len(runs), data.get("frames"), data.get("avg_ms") or 0,
            data.get("median_ms") or 0, data.get("p99_ms") or 0), flush=True)
        time.sleep(1.0)
    return runs


def aggregate(runs):
    mid = runs[len(runs) // 2]
    agg = dict(mid)
    for k in SCALAR:
        vals = [r.get(k) for r in runs if r.get(k) is not None]
        if vals:
            agg[k] = H._median(vals)
    return agg


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "run"
    runs_wanted = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    timeout_s = float(sys.argv[3]) if len(sys.argv) > 3 else 900.0
    with open(H.BASELINE_CACHE_FILE) as fh:
        cache = json.load(fh)
    baseline = cache["baseline"]
    print("baseline head=%s avg=%.3f median=%.3f p99=%.3f frames=%s" % (
        cache["head"][:8], baseline["avg_ms"], baseline["median_ms"],
        baseline["p99_ms"], baseline["frames"]), flush=True)
    runs = watch(runs_wanted, timeout_s)
    if not runs:
        print("NO RUNS CAPTURED (%s)" % label, flush=True)
        return 1
    agg = aggregate(runs)
    print("\n=== bench_gate [%s] %d runs ===" % (label, len(runs)))
    print(H._benchSummary(agg, baseline))
    return 0


if __name__ == "__main__":
    sys.exit(main())
