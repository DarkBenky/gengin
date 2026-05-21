# TODO: Implement model optimization pipeline
# git_pull()
# build(target)                    // make, make bench, make loss, etc.
# run_perf(duration)               // perf record → flamegraph SVG + folded data
# read_file(path)
# list_files(path)
# write_file(path, content)        // model writes proposed patch
# apply_patch(patch)               // git apply
# reset()                          // git checkout -- .
# create_pr(title, body, branch)

# TODO: defines skills and tools for the model to use, e.g. git, perf, flamegraph, etc.
# TODO: add boundaries so model can execute commands outside of the directory

import subprocess
from pprint import pprint
import json

SYSTEM_PROMPT = "TODO: create prompt for model"

CONTEXT = []

PROJECT_DIR = "gengin"

def run(cmd, **kwargs):
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)

    print(f"[{' '.join(cmd)}]")
    print(result.stdout)

    if result.stderr:
        print("ERR:", result.stderr)

    if result.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)}")
    
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
    res = run(["grep", "-r", "TODO", path], cwd=PROJECT_DIR)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "getTodos",
        "input": path,
        "output": res.stdout
    })
    return res.stdout

def buildProject():
    res = run(["make"], cwd=PROJECT_DIR)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "buildProject",
        "input": None,
        "output": res.stdout
    })
    return res.stdout

def makeBench():
    res = run(["make", "bench"], cwd=PROJECT_DIR)
    with open(f"{PROJECT_DIR}/bench_results.json", "r") as f:
        bench_results = f.read()
    bench_results = json.loads(bench_results)
    del bench_results["frame_images"]
    data = json.loads(bench_results)
    record = {
        "type": "tool_use",
        "tool": "makeBench",
        "input": None,
        "output": res.stdout,
        "bench_results": str(bench_results)
    }
    CONTEXT.append(record)
    return res.stdout, data

BASELINE_RESULTS = None

if __name__ == "__main__":
    git_pull_project()
    getTree()
    getTodos()
    buildProject()
    _ , BASELINE_RESULTS = makeBench()
    pprint(CONTEXT)