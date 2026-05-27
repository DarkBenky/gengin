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

SYSTEM_PROMPT = "TODO: create prompt for model"

CONTEXT = []

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None

CONTEXT_MAX_TOKENS = 48_000
CONTEXT_COMPRESS_AT = 0.75  # trigger compression when > 75% full

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
    tokenCount = len(xml) // 4
    return xml, tokenCount

def _estimateTokens(obj) -> int:
    return len(json.dumps(obj)) // 4


def compressContext():
    """
    Ask the model to summarise the current context, persist key findings and
    pending tasks to the planner board, then trim the in-memory context so
    nothing important is lost to truncation.
    """
    if not CONTEXT:
        return

    xmlContext, _ = convertContextToXml()
    summarize_prompt = (
        "You are reviewing your own working context from a C engine performance "
        "optimization session. Before this context is trimmed, extract and preserve "
        "the most critical information.\n"
        "<context_to_summarize>\n" + xmlContext + "\n</context_to_summarize>\n\n"
        "Reply with ONLY a valid JSON object (no prose, no markdown fences):\n"
        "{\n"
        '  "summary": "2-3 sentence overview of progress so far",\n'
        '  "key_findings": ["important hotspot or perf finding"],\n'
        '  "completed_changes": ["change applied + measured result"],\n'
        '  "pending_tasks": ["still needs to be investigated or tried"],\n'
        '  "critical_notes": ["baselines, regressions, constraints to remember"]\n'
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
            for item in data.get("pending_tasks", []):
                planner.addTask(item)
            for item in data.get("critical_notes", []):
                planner.addNote(f"[CRITICAL] {item}")
            print(f"[compressContext] saved {len(data.get('pending_tasks', []))} tasks and "
                  f"{len(data.get('key_findings', [])) + len(data.get('completed_changes', [])) + len(data.get('critical_notes', []))} notes to planner")
    except Exception as e:
        print(f"[compressContext] summarization failed: {e}")

    board = planner.showBoard(returnString=True)
    keep = max(5, len(CONTEXT) // 3)
    del CONTEXT[:-keep]
    CONTEXT.insert(0, {
        "type": "context_summary",
        "tool": "contextSummary",
        "output": f"[Context compressed. Key information saved to planner.]\n{board}",
    })


def removeStaffFromContext(maxTokens=CONTEXT_MAX_TOKENS):
    if _estimateTokens(CONTEXT) > maxTokens * CONTEXT_COMPRESS_AT:
        compressContext()
    # Hard trim if still over limit after compression
    while _estimateTokens(CONTEXT) > maxTokens and len(CONTEXT) > 1:
        removed = CONTEXT.pop(0)
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

PLAN_PROMPT = """\
You are an expert C software engineer reviewing your own optimization session. \
Your ONLY task right now is to think deeply and produce a structured plan. \
Do NOT call any tools. Do NOT write any code. Do NOT emit any JSON tool blocks.

Review everything in the context — flame graph data, bench results, code you have \
already read, notes on the board, completed changes, regressions — and answer:

1. What have I already tried and what were the results?
2. What are the current top hotspots that are NOT yet addressed?
3. What is the single most impactful change I should try next, and why?
4. What do I need to read or profile first before I can safely make that change?
5. Are there any risks or constraints I should keep in mind?

Reply in plain prose. Be specific: name functions, file paths, and measured numbers \
from the context where relevant. This plan will be injected into the context so your \
future self can act on it directly.
"""

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
3. Pick the top hotspot. Use hotAnnotateFunc(func) to see exactly which lines are \
hot, or showContext(func, depth=2) / showSrcPair(path) to read the relevant code.
4. BEFORE writing any code, record your findings: \
call addNote() with the hotspot name and why it is slow, \
call addTask() for each concrete change you plan to make. \
This is mandatory — do not skip it.
5. Apply your change with one of the editing tools:\
\n   - searchReplace(rel_path, old_text, new_text)  — preferred, no line numbers needed\
\n   - searchReplaceMulti(rel_path, [{"old":...,"new":...},...])  — multiple edits in one call\
\n   - insertLines(rel_path, after_line, new_text)  — add code without touching existing lines (after_line=0 to prepend)\
\n   - deleteLines(rel_path, start, end)  — remove lines cleanly\
\n   - applyChange(func_name, new_definition)  — replace a whole named function\
\n   Multiple independent \
regions can be batched with applyPatch().
6. Call buildProject(). If it fails, read the error, fix the code, rebuild. \
Record the error summary with addNote().
7. Call makeBench() and compare against the baseline in context. \
Record the result with addNote(). If performance regressed, call restoreAll() \
and add a note explaining what failed.
8. When a change is solid, call createPR() with a clear title and body, \
then call markTaskDone() for the completed tasks.
9. Call showBoard() to review remaining tasks and continue with the next hotspot.

== NOTE-TAKING RULES ==
- ALWAYS call addNote() / addTask() BEFORE reading code or applying changes.
- After every bench result, call addNote() with the measured numbers.
- When you discover something about the codebase, call addNote() immediately.
- Notes and tasks survive context compression — they are your long-term memory.
  Anything not written to the board WILL be forgotten when context is trimmed.
- Use showBoard() at the start of each iteration to recall your plan.

== CONSTRAINTS ==
- Only call tools that are listed in apiHelp(). Anything else will be rejected.
- File paths must be relative to the project root. Never use ".." to escape it.
- Do not break the public API (function signatures visible in headers) without \
explicit instruction.
- Always build and bench after every change before moving on.
- If you are unsure whether a change is safe, read more code first.
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
"""

# TODO: add api where model can execute some code in a sandbox

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
    _recent_calls = []  # (tool, args_str) tuples for loop detection
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
            print(f"[nudge] {nudge}")
        xmlContext, tokenCount = convertContextToXml()
        ui.set_token_count(tokenCount)
        ui.sync_context(CONTEXT)
        board = planner.showBoard(returnString=True)
        ui.sync_board(board)
        prompt = SYSTEM_PROMPT + "\n\n== CURRENT BOARD ==\n" + board + "\n\n" + "Context:\n" + xmlContext
        print(f"Prompt token count: {tokenCount}")
        ui.set_status("waiting_model")
        steer = ui.pop_steer()
        is_plan_iteration = False
        _cleared = [False]
        def _on_token(delta):
            if not _cleared[0]:
                ui.clear_stream()
                _cleared[0] = True
            ui.push_token(delta)
        try:
            # if random.random() > 0.85:
            #     response = model.getResponse(prompt, model="deepseek/deepseek-v4-pro", provider="deepseek")
            # elif random.random() < 0.35:
            response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="deepinfra/fp4")
            # elif random.random() < 0.1:
            #     is_plan_iteration = True
            #     plan_prompt = PLAN_PROMPT + "\n\nContext:\n" + xmlContext
            #     response = model.getResponseQwen3_6(plan_prompt, mode="general")
            # else:
            # mode = random.choices(["coding", "general", "instruct"], weights=[0.5, 0.2, 0.3])[0]
            # response = model.getResponseQwen3_6(prompt, mode=mode, on_token=_on_token, system_prompt=steer or None)
        except Exception as e:
            CONTEXT.append({
                "type": "model_response",
                "iteration": iteration,
                "output": f"Error getting model response: {e} try again in the next iteration."
            })
            continue
        # response = model.getResponseOllama(prompt, model="gemma4:e4b")
        # response = model.getResponse(prompt, model="tencent/hy3-preview", provider="siliconflow")
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
        if is_plan_iteration:
            ui.sync_context(CONTEXT)
            ui.sync_diff(gf.getDiff(returnString=True, code_only=True))
            continue
        results = executor.executeAll(response, TOOL_MAP, context=CONTEXT)
        for r in results:
            ui.log_tool_result(r["tool"], r["error"])

        # loop detection: same tool+args 4 times in a row
        for r in results:
            sig = (r["tool"], str(sorted((r.get("args") or {}).items()) if isinstance(r.get("args"), dict) else ""))
            _recent_calls.append(sig)
        _recent_calls = _recent_calls[-12:]
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
            print(f"[loop-detect] injected nudge for repeated '{loop_tool}'")

        ui.sync_context(CONTEXT)
        ui.sync_diff(gf.getDiff(returnString=True, code_only=True))

        removeDoublesFromContext()