import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ML_BENCH = os.path.join(ROOT, "llmOpt", "ml_bench.py")


def main():
    args = sys.argv[1:] or ["--suite", "all"]
    try:
        return subprocess.call([sys.executable, ML_BENCH] + args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
