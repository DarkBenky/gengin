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
import modelSelector as model
import sys
import ui

SYSTEM_PROMPT = "TODO: create prompt for model"

CONTEXT = []

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None

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
            lines.append(f"  frame_hashes  all {total} frame(s) match baseline [OK]")
        else:
            lines.append(f"  frame_hashes  {changed}/{total} frame(s) changed vs baseline [OUTPUT CHANGED]")

    if improved > regressed:
        lines.append("=> OVERALL: PERFORMANCE IMPROVED")
    elif regressed > improved:
        lines.append("=> OVERALL: PERFORMANCE REGRESSED -- consider restoreAll()")
    else:
        lines.append("=> OVERALL: no significant change")
    return "\n".join(lines)


def makeBench():
    res = run(["make", "bench"], cwd=PROJECT_DIR)
    with open(f"{PROJECT_DIR}/bench_results.json", "r") as f:
        bench_results = json.load(f)
        bench_results_raw = bench_results.copy()
    bench_results.pop("frame_images", None)
    bench_results.pop("frame_hashes", None)
    summary = _benchSummary(bench_results_raw, BASELINE_RESULTS)
    record = {
        "type": "tool_use",
        "tool": "makeBench",
        "input": None,
        "output": summary,
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
    tokenCount = len(xml.split()) * 4
    return xml, tokenCount

def removeStaffFromContext(maxTokens=128_000):
    currentTokens = json.dumps(CONTEXT).count(" ") * 4  # rough estimate
    while currentTokens > maxTokens and CONTEXT:
        removed = CONTEXT.pop(0)
        removedTokens = json.dumps(removed).count(" ") * 4
        currentTokens -= removedTokens
        print(f"Removed {removedTokens} tokens from context. Current token count: {currentTokens}. Removed entry: {removed}")

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
    ui.set_pr_url(url)
    print(f"PR created: {url}")
    sys.exit(0)

SYSTEM_PROMPT = """\
You are an expert C software engineer. Your job is to analyse a renderer/engine \
codebase and iteratively improve its performance. You work in a loop:

  profile -> identify hotspot -> read code -> propose change \
-> apply -> build -> bench -> compare -> repeat

== CALLING TOOLS ==
To call a tool, emit one or more fenced JSON blocks anywhere in your response.
Every block must be valid JSON with "tool" (string) and "args" (object) fields:

```json
{"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
```

```json
{"tool": "replaceLines", "args": {"rel_path": "render/render.c", "start": 42, "end": 55, "new_text": "int foo() {\n    return 1;\n}"}}
```

```json
{"tool": "getDiff", "args": {}}
```

Multiple blocks in one response are executed in order. Omitting "args" is also \
accepted and treated as an empty object. Every tool result is appended to the \
context so you can see it in the next turn. Use apiHelp() (already in context) to \
list all available tools and their exact argument names.

== WORKFLOW ==
1. Read the flame graph and bench results already in the context.
2. Use listFunctions() to get an overview of the codebase.
3. Pick the top hotspot. Use showContext(func, depth=2) or showSrcPair(path) to \
read the relevant code.
4. Form a hypothesis. Record it with addNote(). Break the work into tasks with \
applyChange() applies changes to whole functions. searchReplace() applies changes to any text in a file — \
prefer searchReplace() for partial changes within a function. For multiple \
independent regions in one file, use applyPatch().
5. Apply your change with searchReplace() or applyChange(). Multiple independent \
regions can be batched with applyPatch().
6. Call buildProject(). If it fails, read the error, fix the code, rebuild.
7. Call makeBench() and compare against the baseline in context. If performance \
regressed, call restoreAll() and try a different approach.
8. When a change is solid, call createPR() with a clear title and body explaining \
what you changed, why, and the measured speedup.
9. Mark completed tasks with markTaskDone() and continue with the next hotspot.

== CONSTRAINTS ==
- Only call tools that are listed in apiHelp(). Anything else will be rejected.
- File paths must be relative to the project root. Never use ".." to escape it.
- Do not break the public API (function signatures visible in headers) without \
explicit instruction.
- Always build and bench after every change before moving on.
- If you are unsure whether a change is safe, read more code first.
- Keep changes focused and minimal — one logical improvement per PR.
"""


if __name__ == "__main__":
    ui.start()
    git_pull_project()
    gf.init()
    planner.resetBoard()

    getTree()
    # getTodos()
    buildProject()
    _ , BASELINE_RESULTS = makeBench()
    flame = makeFlame()
    gf.listFunctions(context=CONTEXT)
    gf.apiHelp(context=CONTEXT)

    TOOL_MAP = executor.buildToolMap(gf, planner, sys.modules[__name__])
    iteration = 0
    while True:
        iteration += 1
        ui.set_iteration(iteration)
        ui.set_status("running")
        removeStaffFromContext(512_000)
        xmlContext, tokenCount = convertContextToXml()
        ui.set_token_count(tokenCount)
        ui.sync_context(CONTEXT)
        ui.sync_board(planner.showBoard(returnString=True))
        prompt = SYSTEM_PROMPT + "\n\n" + "Context:\n" + xmlContext
        print(f"Prompt token count: {tokenCount}")
        ui.set_status("waiting_model")
        # response = model.getResponseOllama(prompt, model="gemma4:e4b")
        if iteration % 8 == 0:
            # use cheaper model every 8 iterations to save costs, since not every iteration needs a super detailed response
            response = model.getResponse(prompt, model="deepseek/deepseek-v4-pro", provider="deepseek") # Input 1M / 0.43$ Output 1M / 0.87$
        else:
            response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="deepinfra/fp4") # Input 1M / 0.10$ Output 1M / 0.20$
        ui.set_last_response(response)
        print("Model response:", response)
        ui.set_status("running")
        results = executor.executeAll(response, TOOL_MAP, context=CONTEXT)
        for r in results:
            ui.log_tool_result(r["tool"], r["error"])
        ui.sync_context(CONTEXT)
        ui.sync_board(planner.showBoard(returnString=True))
        ui.sync_diff(gf.getDiff(returnString=True, code_only=True))