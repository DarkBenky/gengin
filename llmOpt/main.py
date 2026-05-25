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

import os
import re
import subprocess
from datetime import datetime

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

MAX_CONTEXT_TOKENS    = 128_000   # hard context window limit
SUMMARIZE_THRESHOLD   = 0.80      # summarize when at 80% of limit
CRITICAL_THRESHOLD    = 0.95      # emergency drop when at 95% after summarization
KEEP_RECENT           = 8         # number of most-recent entries to never summarize
NOTES_FILE            = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model_notes.md")

CONTEXT = []
NOTES = ""

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None


def _count_tokens(obj) -> int:
    """Estimate token count: character length / 4 is a reasonable approximation."""
    if isinstance(obj, str):
        return len(obj) // 4
    return len(json.dumps(obj, ensure_ascii=False)) // 4


def _print_token_usage(tokens: int, max_tokens: int = MAX_CONTEXT_TOKENS, label: str = "Tokens"):
    pct = (tokens / max_tokens * 100) if max_tokens else 0.0
    print(f"{label}: {tokens} / {max_tokens} ({pct:.1f}%)")

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


def loadNotes() -> str:
    """Load persistent notes from disk. Returns empty string if file doesn't exist."""
    global NOTES
    try:
        with open(NOTES_FILE, "r", encoding="utf-8") as f:
            NOTES = f.read().strip()
    except FileNotFoundError:
        NOTES = ""
    return NOTES


def saveNotes(content: str):
    """Persist notes to disk."""
    global NOTES
    NOTES = (content or "").strip()
    with open(NOTES_FILE, "w", encoding="utf-8") as f:
        f.write(NOTES)


def addNote(note: str = None, text: str = None, context=None):
    """Append a timestamped note. Exposed as a tool to the model."""
    note_text = note if note is not None else text
    if note_text is None:
        raise TypeError("addNote() missing required argument: 'note'")

    note_text = str(note_text).strip()
    if not note_text:
        raise ValueError("note must not be empty")

    timestamp = datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC")
    entry = f"- [{timestamp}] {note_text}"
    current = NOTES.rstrip()
    updated = f"{current}\n{entry}" if current else entry
    saveNotes(updated)

    if context is not None:
        context.append({
            "type": "tool_use",
            "tool": "addNote",
            "input": note_text,
            "output": entry
        })
    return entry


def getNotes(context=None) -> str:
    """Return current notes content. Exposed as a tool to the model."""
    if context is not None:
        context.append({
            "type": "tool_use",
            "tool": "getNotes",
            "input": None,
            "output": NOTES
        })
    return NOTES


def updateNotes(content: str, context=None):
    """Replace all notes with new content (for model to rewrite/compress its own notes). Exposed as a tool."""
    saveNotes(content)
    if context is not None:
        context.append({
            "type": "tool_use",
            "tool": "updateNotes",
            "input": None,
            "output": NOTES
        })
    return NOTES


def _registerPersistentNotesApiHelp():
    if not hasattr(gf, "_API"):
        return

    section_name = "Persistent Notes  [main.py]"
    for section, _entries in gf._API:
        if section == section_name:
            return

    gf._API.append((section_name, [
        ("addNote(note)",
         f"Append a timestamped note to {NOTES_FILE}. Also accepts addNote(text=...)."),
        ("getNotes()",
         f"Return the full persistent note contents from {NOTES_FILE}."),
        ("updateNotes(content)",
         f"Replace the persistent notes stored in {NOTES_FILE}."),
    ]))


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
    tokenCount = _count_tokens(xml)
    return xml, tokenCount

def manageContext(max_tokens: int = MAX_CONTEXT_TOKENS):
    """
    1. Count current tokens accurately.
    2. If below SUMMARIZE_THRESHOLD * max_tokens → do nothing.
    3. If above threshold:
       a. Keep the last KEEP_RECENT entries untouched.
       b. Send the older entries to the model asking for a dense summary.
       c. Replace those entries with a single {"type": "context_summary", "output": <summary>} entry.
       d. Re-count. If still above CRITICAL_THRESHOLD * max_tokens, drop oldest entries one by one.
    4. Print/log the before/after token counts.
    """
    before_tokens = _count_tokens(CONTEXT)
    _print_token_usage(before_tokens, max_tokens, label="Context tokens before management")

    if before_tokens < max_tokens * SUMMARIZE_THRESHOLD:
        _print_token_usage(before_tokens, max_tokens, label="Context tokens after management")
        return before_tokens

    if len(CONTEXT) > KEEP_RECENT:
        older_entries = CONTEXT[:-KEEP_RECENT]
        recent_entries = CONTEXT[-KEEP_RECENT:]
        summarize_prompt = """You are a context compressor. Summarize the following optimization session context into a dense, information-preserving paragraph. Include: hotspots identified, changes attempted (success/failure), current benchmark deltas, and open tasks. Be concise.

Optimization session context:
""" + json.dumps(older_entries, ensure_ascii=False, indent=2)
        try:
            summary = model.getResponseQwen3_6(summarize_prompt, mode="instruct")
            CONTEXT[:] = [{
                "type": "context_summary",
                "output": summary
            }] + recent_entries
            print(f"Summarized {len(older_entries)} older context entries into one context_summary entry.")
        except Exception as e:
            print(f"Context summarization failed: {e}")
    else:
        print("Context above summarize threshold, but there are no older entries to summarize.")

    current_tokens = _count_tokens(CONTEXT)
    while current_tokens > max_tokens * CRITICAL_THRESHOLD and CONTEXT:
        drop_index = 0
        if CONTEXT[0].get("type") == "context_summary" and len(CONTEXT) > 1:
            drop_index = 1
        removed = CONTEXT.pop(drop_index)
        current_tokens = _count_tokens(CONTEXT)
        print(f"Emergency-dropped oldest context entry ({removed.get('type', 'unknown')}).")

    _print_token_usage(current_tokens, max_tokens, label="Context tokens after management")
    return current_tokens


NOTES = loadNotes()

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
regressed, call restoreAll() and try a different approach. After calling \
makeBench(), call addNote() to record the result.
8. When a change is solid, call createPR() with a clear title and body explaining \
what you changed, why, and the measured speedup.
9. Mark completed tasks with markTaskDone() and continue with the next hotspot.

== PERSISTENT NOTES ==
You have a personal notepad that survives across iterations. Use it aggressively.
Available note tools:
  addNote(note)           — append a timestamped note (observations, hypotheses, decisions)
  getNotes()              — read all your current notes
  updateNotes(content)    — rewrite/compress your notes (do this when they get long)

Use addNote() at least once per iteration to record:
  - What hotspot you are focusing on and why
  - What change you applied and your hypothesis
  - The outcome (benchmark result, build error, etc.)
  - Any insight for future iterations

Your notes are prepended to every prompt, so keep them dense and useful.
When the context is summarized, your notes are the primary way to preserve detailed reasoning.

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
    _registerPersistentNotesApiHelp()
    gf.apiHelp(context=CONTEXT)

    TOOL_MAP = executor.buildToolMap(gf, planner, sys.modules[__name__])
    TOOL_MAP.update({
        "addNote": addNote,
        "getNotes": getNotes,
        "updateNotes": updateNotes,
    })
    iteration = 0
    while True:
        iteration += 1
        ui.set_iteration(iteration)
        ui.set_status("running")
        manageContext()
        xmlContext, tokenCount = convertContextToXml()
        ui.sync_context(CONTEXT)
        ui.sync_board(planner.showBoard(returnString=True))
        notes_content = loadNotes()
        notes_section = f"\n\n== YOUR NOTES ==\n{notes_content}\n" if notes_content else ""
        prompt = SYSTEM_PROMPT + notes_section + "\n\nContext:\n" + xmlContext
        prompt_token_count = _count_tokens(prompt)
        ui.set_token_count(prompt_token_count)
        _print_token_usage(prompt_token_count, MAX_CONTEXT_TOKENS)
        ui.set_status("waiting_model")
        response = model.getResponseQwen3_6(prompt, mode="coding")
        # response = model.getResponseOllama(prompt, model="gemma4:e4b")
        # response = model.getResponse(prompt, model="tencent/hy3-preview", provider="siliconflow")
        # response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="deepinfra/fp4")
        ui.set_last_response(response)
        print("Model response:", response)
        response_for_context = re.sub(r"<think>.*?</think>", "", response, flags=re.DOTALL).strip()
        CONTEXT.append({
            "type": "model_response",
            "iteration": iteration,
            "output": response_for_context
        })
        ui.set_status("running")
        results = executor.executeAll(response, TOOL_MAP, context=CONTEXT)
        for r in results:
            ui.log_tool_result(r["tool"], r["error"])
        ui.sync_context(CONTEXT)
        ui.sync_board(planner.showBoard(returnString=True))
        ui.sync_diff(gf.getDiff(returnString=True, code_only=True))