import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ML_BENCH = os.path.join(ROOT, "llmOpt", "ml_bench.py")
STAT_COLUMNS = ("minMs", "medianMs", "meanMs", "p99Ms", "maxMs")


def printStats(doc):
    settings = doc["settings"]
    print("device: %s | %s | reps %d | warmup %d | generator %s"
          % (settings["device"], settings["platform"], settings["reps"],
             settings["warmup"], doc["generatorHash"]))
    print("%-38s" % "config" + "".join("%11s" % column for column in STAT_COLUMNS))
    for row in doc["rows"]:
        flag = "" if row.get("ok", True) else "  FAIL"
        print("%-38s" % row["id"] + "".join("%11.6f" % row.get(column, 0.0)
                                             for column in STAT_COLUMNS) + flag)


def main():
    args = sys.argv[1:] or ["--suite", "all"]
    passthrough = any(arg in ("--json", "--list", "--help", "-h") for arg in args)
    if not passthrough:
        args = args + ["--json"]
    try:
        proc = subprocess.run([sys.executable, ML_BENCH] + args,
                              capture_output=True, text=True)
    except KeyboardInterrupt:
        return 130
    sys.stderr.write(proc.stderr)
    if passthrough:
        sys.stdout.write(proc.stdout)
        return proc.returncode
    start = proc.stdout.find("{")
    doc = None
    if start >= 0:
        try:
            doc = json.loads(proc.stdout[start:])
        except ValueError:
            doc = None
    if not isinstance(doc, dict) or not isinstance(doc.get("rows"), list):
        sys.stdout.write(proc.stdout)
        return proc.returncode
    printStats(doc)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
