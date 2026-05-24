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
import perf as perfLib
import getFunc as gf
import planner
import executor

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
        bench_results = json.load(f)
        bench_results_raw = bench_results.copy()
    bench_results.pop("frame_images", None)
    record = {
        "type": "tool_use",
        "tool": "makeBench",
        "input": None,
        "output": res.stdout,
        "bench_results": bench_results
    }
    CONTEXT.append(record)
    return res.stdout, bench_results_raw

def makeFlame():
    run(["make", "flame"], cwd=PROJECT_DIR)
    data = perfLib.getPerfData(cwd=PROJECT_DIR)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "makeFlame",
        "input": None,
        "output": data
    })
    return data

BASELINE_RESULTS = None

def convertContextToXml():
    xml = "<context>\n"
    for entry in CONTEXT:
        xml += "  <entry>\n"
        for key, value in entry.items():
            xml += f"    <{key}>{value}</{key}>\n"
        xml += "  </entry>\n"
    xml += "</context>"
    tokenCount = len(xml.split() * 4)  # rough estimate: 1 word ~ 4 tokens rather overestimating to be safe
    return xml, tokenCount

def removeStaffFromContext(maxTokens=128_000, currentTokens=0):
    newContext = []
    for entry in reversed(CONTEXT):
        entryTokens = len(json.dumps(entry).split()) * 4  # rough estimate
        if currentTokens + entryTokens > maxTokens:
            break
        newContext.append(entry)
        currentTokens += entryTokens
    newContext = list(reversed(newContext))
    CONTEXT[:] = newContext
    return newContext, currentTokens

def createPR(title, body, branch, commit_msg=None):
    """Commit staged changes, push to branch, open a GitHub PR via gh CLI."""
    commit_msg = commit_msg or title

    run(["git", "checkout", "-b", branch], cwd=PROJECT_DIR)
    run(["git", "add", "-A"], cwd=PROJECT_DIR)
    run(["git", "commit", "-m", commit_msg], cwd=PROJECT_DIR)
    run(["git", "push", "-u", "origin", branch], cwd=PROJECT_DIR)

    res = run(
        ["gh", "pr", "create", "--title", title, "--body", body, "--head", branch],
        cwd=PROJECT_DIR,
    )
    url = res.stdout.strip()
    CONTEXT.append({
        "type": "tool_use",
        "tool": "createPR",
        "input": {"title": title, "branch": branch},
        "output": url,
    })
    return url

if __name__ == "__main__":
    # git_pull_project()
    gf.init()
    planner.resetBoard()

    getTree()
    getTodos()
    buildProject()
    _ , BASELINE_RESULTS = makeBench()
    flame = makeFlame()
    gf.listFunctions(context=CONTEXT)
    gf.apiHelp(context=CONTEXT)
    pprint(CONTEXT)
    
    # TODO: main loop
    import sys
    TOOL_MAP = executor.buildToolMap(gf, planner, sys.modules[__name__])