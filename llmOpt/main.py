# git_pull()
# build(target)                    // make, make bench, make loss, etc.
# run_perf(duration)               // perf record → flamegraph SVG + folded data
# read_file(path)
# list_files(path)
# write_file(path, content)        // model writes proposed patch
# apply_patch(patch)               // git apply
# reset()                          // git checkout -- .
# create_pr(title, body, branch)


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

CONTEXT = []

PROJECT_DIR = "gengin"
BASELINE_RESULTS = None
PINNED_HOTSPOTS = ""  # top hot functions + baseline, survives context trimming

CONTEXT_MAX_TOKENS = 128_000
CONTEXT_COMPRESS_AT = 0.85  # trigger compression when > 85% full

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

    # Fast syntax check on changed .c files — catches 90% of errors
    # in <1 second vs a full 30-second build cycle.
    changed = subprocess.run(
        ["git", "diff", "--name-only"],
        capture_output=True, text=True, cwd=PROJECT_DIR
    )
    changed_c = [f for f in changed.stdout.strip().split("\n") if f.endswith(".c")]
    if changed_c:
        try:
            run(
                ["gcc", "-fsyntax-only", "-I.", "-Iobject", "-I/usr/local/include"]
                + changed_c,
                cwd=PROJECT_DIR,
            )
        except RuntimeError:
            # Syntax error — report and stop early, don't waste time on make
            CONTEXT.append({
                "type": "tool_use",
                "tool": "buildProject",
                "input": None,
                "output": "SYNTAX CHECK FAILED — fix errors above before rebuilding.",
            })
            raise

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

    # Auto-restore on significant visual regression (safety net).
    # Only fires when there IS a baseline to compare against (not the init run).
    if BASELINE_RESULTS is not None:
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
            summary += (
                "\n\n*** AUTO-RESTORED: Significant visual regression detected "
                f"(max MSE={max(mse_values):.1f}). All changes have been reverted. "
                "The previous change likely broke rendering. Read the code more "
                "carefully before re-applying. ***"
            )
            planner.addNote(f"[AUTO-RESTORE] Visual regression (max MSE={max(mse_values):.1f}) — changes reverted.")

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
    global PINNED_HOTSPOTS
    run(["make", "flame"], cwd=PROJECT_DIR)
    data = perfLib.getPerfData(cwd=PROJECT_DIR)
    # Refresh pinned hotspots so the model sees the latest profile
    if data and data.get("hot_functions"):
        top5 = data["hot_functions"][:5]
        PINNED_HOTSPOTS = (
            "Top hotspots (always visible):\n" +
            "\n".join(
                f"  {h['fn']:40s}  incl={h['inclusive_pct']:5.1f}%  excl={h['exclusive_pct']:5.1f}%"
                for h in top5
            )
        )
    CONTEXT.append({
        "type": "tool_use",
        "tool": "makeFlame",
        "input": None,
        "output": data
    })
    return data


BENCH_FUNC_DIR = os.path.join(PROJECT_DIR, "bench")

def createFuncBench(func_name: str, header_code: str, impl_code: str):
    """
    Create bench/<func_name>.h and bench/<func_name>.c so the model can compile
    and time a standalone micro-benchmark with `runFuncBench(func_name)`.

    header_code  - full content of the .h file (guards, typedefs, declarations).
    impl_code    - full content of the .c file; must contain a main() that uses
                   ComputePerformanceMetrics() from tests/timings.h for timing and
                   prints results to stdout.
    """
    os.makedirs(BENCH_FUNC_DIR, exist_ok=True)
    h_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.h")
    c_path = os.path.join(BENCH_FUNC_DIR, f"{func_name}.c")
    with open(h_path, "w") as fh:
        fh.write(header_code)
    with open(c_path, "w") as fh:
        fh.write(impl_code)
    msg = f"Created bench/{func_name}.h and bench/{func_name}.c"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "createFuncBench",
        "input": func_name,
        "output": msg,
    })
    return msg


def runFuncBench(func_name: str):
    """
    Build and run the micro-benchmark for func_name via `make benchFunc <func_name>`.
    Returns the stdout output (timing results and validation).
    """
    try:
        res = run(["make", "benchFunc", func_name], cwd=PROJECT_DIR)
        output = res.stdout
    except RuntimeError as e:
        output = str(e)
    CONTEXT.append({
        "type": "tool_use",
        "tool": "runFuncBench",
        "input": func_name,
        "output": output,
    })
    return output


def deleteFuncBench(func_name: str):
    """Remove bench/<func_name>.h, bench/<func_name>.c and the compiled binary."""
    removed = []
    for ext in (".h", ".c", ""):
        path = os.path.join(BENCH_FUNC_DIR, f"{func_name}{ext}")
        if os.path.exists(path):
            os.remove(path)
            removed.append(f"bench/{func_name}{ext}")
    msg = f"Removed: {', '.join(removed)}" if removed else f"No files found for bench/{func_name}"
    CONTEXT.append({
        "type": "tool_use",
        "tool": "deleteFuncBench",
        "input": func_name,
        "output": msg,
    })
    return msg


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
    # JSON serialization underestimates vs the XML we actually send.
    # XML tags and escaping add ~25-30% overhead over raw JSON.
    json_len = len(json.dumps(obj))
    return int(json_len * 1.3) // 4


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

    # Persist high-level insights to codebase_context.md before trimming
    syncPlannerToCodebaseContext()

    board = planner.showBoard(returnString=True)
    keep = max(8, len(CONTEXT) * 2 // 3)
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

    # Checkpoint: persist planner insights to codebase_context.md
    syncPlannerToCodebaseContext()

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

CODEBASE_CONTEXT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "codebase_context.md")
PLANNER_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "planner_state.json")


def syncPlannerToCodebaseContext():
    """
    Ask the model to distill the planner's accumulated notes and tasks into
    high-level insights, then inject them into codebase_context.md so
    knowledge survives across runs. Only architectural findings, confirmed
    wins, and remaining priorities are kept — rejected approaches and transient
    debugging notes are filtered out.

    Can be called by the model as a tool, or automatically at checkpoints.
    """
    import datetime

    # Load planner state
    if not os.path.exists(PLANNER_STATE_FILE):
        return "No planner state file found — nothing to sync."

    with open(PLANNER_STATE_FILE) as fh:
        state = json.load(fh)

    notes = state.get("notes", [])
    tasks = state.get("tasks", [])

    if not notes and not tasks:
        return "Planner has no notes or tasks — nothing to sync."

    # Build a compact representation for the model to summarize
    notes_text = "\n".join(f"  [{n['id']}] {n['text']}" for n in notes)
    tasks_text = "\n".join(
        f"  [{t['status']}] #{t['id']} {t['text']}" for t in tasks
    )

    summarize_prompt = (
        "You are reviewing the accumulated notes and tasks from a C engine "
        "performance optimization session. Your job is to distill this into "
        "HIGH-LEVEL insights suitable for injecting into a codebase knowledge "
        "base (codebase_context.md).\n\n"
        "FILTERING RULES:\n"
        "- INCLUDE: architectural findings, confirmed performance wins with "
        "measured numbers, remaining high-priority hotspots, lessons about what "
        "techniques work or don't work for this specific codebase.\n"
        "- EXCLUDE: exact code snippets, rejected implementations, transient "
        "debugging notes, tool call logs, temporary measurements.\n"
        "- Be concise. Each insight should be 1-2 sentences.\n\n"
        "PLANNER NOTES:\n" + (notes_text or "(none)") + "\n\n"
        "PLANNER TASKS:\n" + (tasks_text or "(none)") + "\n\n"
        "Reply with ONLY a valid JSON object (no prose, no markdown fences):\n"
        "{\n"
        '  "summary": "1-2 sentence overview of what was accomplished this session",\n'
        '  "confirmed_wins": ["function: technique -> measured improvement"],\n'
        '  "architectural_insights": ["important findings about this codebase"],\n'
        '  "remaining_hotspots": ["functions still needing optimization, in priority order"],\n'
        '  "techniques_to_try": ["approaches worth attempting next"],\n'
        '  "techniques_to_avoid": ["approaches that did not work and why"]\n'
        "}"
    )

    try:
        raw = model.getResponse(summarize_prompt, model="deepseek/deepseek-v4-flash", provider="deepinfra/fp4")
        raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()
        m = re.search(r'\{.*\}', raw, re.DOTALL)
        if not m:
            return f"Model response had no JSON: {raw[:200]}"
        data = json.loads(m.group(0))
    except Exception as e:
        return f"Summarization failed: {e}"

    # Build the section to inject
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    lines = [
        f"\n\n---\n\n## Session Insights ({timestamp})\n",
    ]
    if data.get("summary"):
        lines.append(f"**Summary**: {data['summary']}\n")

    sections = [
        ("Confirmed Wins", "confirmed_wins"),
        ("Architectural Insights", "architectural_insights"),
        ("Remaining Hotspots", "remaining_hotspots"),
        ("Techniques to Try", "techniques_to_try"),
        ("Techniques to Avoid", "techniques_to_avoid"),
    ]
    for heading, key in sections:
        items = data.get(key, [])
        if items:
            lines.append(f"### {heading}")
            for item in items:
                lines.append(f"  - {item}")
            lines.append("")

    section_text = "\n".join(lines)

    # Load existing codebase context
    if os.path.exists(CODEBASE_CONTEXT_FILE):
        with open(CODEBASE_CONTEXT_FILE) as fh:
            existing = fh.read()
    else:
        existing = ""

    # Replace old Session Insights section if present, otherwise append
    marker = "\n\n---\n\n## Session Insights"
    if marker in existing:
        # Keep everything before the first session insights marker
        existing = existing[:existing.index(marker)].rstrip()

    with open(CODEBASE_CONTEXT_FILE, "w") as fh:
        fh.write(existing.rstrip() + section_text)

    # Clear the notes that have been distilled (keep tasks)
    state["notes"] = []
    with open(PLANNER_STATE_FILE, "w") as fh:
        json.dump(state, fh, indent=2)

    return f"Synced {len(notes)} note(s) and {len(tasks)} task(s) into {CODEBASE_CONTEXT_FILE}. Planner notes cleared."

RESEARCH_MODEL = "deepseek/deepseek-v4-flash"
RESEARCH_PROVIDER = "baidu/fp8"
MIN_RESEARCH_CONTEXT_ENTRIES = 5

# Tools that must be excluded from the research-phase tool map (read-only mode).
EDITING_TOOLS = frozenset({
    "searchReplace", "searchReplaceMulti", "applyChange", "replaceLines",
    "insertLines", "deleteLines", "applyPatch", "restoreAll", "restoreFile",
    "restoreFunction",
})

RESEARCH_SYSTEM_PROMPT = """\
You are an expert C software engineer performing a thorough codebase research pass \
before an optimization session begins. Your goal is to build a comprehensive, \
persistent knowledge base about this engine's source code so future optimization \
iterations start with full context.

== YOUR TASK ==
Explore the codebase systematically and document:
1. The overall architecture and key subsystems (files, modules, their responsibilities).
2. Every performance-critical function: its location, what it does, why it is likely slow \
(algorithmic complexity, memory access patterns, branching, synchronization, GPU/CPU \
boundary crossings, etc.).
3. Important data structures and how they flow between hot functions.
4. Known TODOs, existing comments about performance, and obvious low-hanging fruit.
5. The call graph from the main render/compute loop down to leaf hot functions.

== HOW TO CALL TOOLS ==
You MUST call tools by emitting fenced JSON blocks. Every response that wants to \
take action MUST contain at least one tool block:

```json
{"tool": "listFunctions", "args": {}}
```

```json
{"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
```

```json
{"tool": "saveCodebaseContext", "args": {"report": "# Codebase Architecture\\n## Key Files and Modules\\n..."}}
```

Do NOT just write prose about what you plan to do — actually emit the JSON tool \
blocks to call the tools. Your response can contain prose + tool blocks together.

== WORKFLOW ==
1. Start with listFunctions() to get a complete function inventory.
2. For key files, call readSourceFile(rel_path) or showContext(func, depth=2).
3. If profiling data is available, call hotAnnotateFile(rel_path) or hotAnnotateFunc(func).
4. Call grepSource(pattern) to find TODOs, FIXME, or performance comments.
5. Call getCallers(func) to trace the call graph for hot functions.
6. After gathering enough information, call saveCodebaseContext(report) with your \
full structured Markdown report. This MUST be the last tool call — it ends the phase.

== saveCodebaseContext(report) ==
Call this with a single "report" arg containing your full Markdown report:
```json
{"tool": "saveCodebaseContext", "args": {"report": "# Codebase Architecture\\n\\n## Key Files and Modules\\n- render/render.c: main render loop...\\n\\n## Critical Data Structures\\n...\\n\\n# Performance-Critical Functions\\n\\n## renderFrame (render/render.c)\\n- What it does: ...\\n- Why it is slow: ...\\n- Callers: ...\\n\\n# Known TODOs and Low-Hanging Fruit\\n...\\n\\n# Recommended Optimization Order\\n1. ...\\n2. ..."}}
```

== CONSTRAINTS ==
- Do NOT apply any code changes. Only use read/exploration tools.
- Call apiHelp() if you are unsure which tools are available.
- When your research is complete, call saveCodebaseContext(report) EXACTLY ONCE.
- If you don't yet have enough information, call more exploration tools.
- The iteration count tells you how many turns remain — plan accordingly.
"""

RESEARCH_MAX_ITERATIONS = 30  # guard against runaway research loops

RESEARCH_CONTEXT: list = []


def saveCodebaseContext(report: str, context=None):
    """Write the model's codebase research report to a persistent Markdown file."""
    with open(CODEBASE_CONTEXT_FILE, "w") as fh:
        fh.write(report)
    msg = f"Codebase context saved to {CODEBASE_CONTEXT_FILE} ({len(report)} chars)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "saveCodebaseContext",
                        "input": f"{len(report)} chars", "output": msg})
    print(f"[research] {msg}")
    return msg


def loadCodebaseContext() -> str | None:
    """Load the persisted codebase research report, or return None if absent."""
    if not os.path.exists(CODEBASE_CONTEXT_FILE):
        return None
    with open(CODEBASE_CONTEXT_FILE) as fh:
        return fh.read()


def runCodebaseResearch():
    """
    Run an agentic research loop where the model explores the codebase and
    produces a persistent codebase_context.md knowledge base file.
    """
    global RESEARCH_CONTEXT
    RESEARCH_CONTEXT = []

    # Seed with exploration data
    getTree()
    gf.listFunctions(context=RESEARCH_CONTEXT)
    gf.apiHelp(context=RESEARCH_CONTEXT)

    # Exploration-only tool map (no editing, no build/bench)
    research_tool_map = executor.buildToolMap(gf, planner)
    research_tool_map["saveCodebaseContext"] = lambda report, context=None: saveCodebaseContext(report, context=context)
    # Remove editing/destructive tools so the research pass is read-only
    for _tool in EDITING_TOOLS:
        research_tool_map.pop(_tool, None)

    print("[research] Starting codebase research phase …")
    last_response_was_prose_only = 0
    for iteration in range(1, RESEARCH_MAX_ITERATIONS + 1):
        remaining = RESEARCH_MAX_ITERATIONS - iteration
        # Hard-trim research context if it grows too large (keep most recent 2/3)
        if _estimateTokens(RESEARCH_CONTEXT) > CONTEXT_MAX_TOKENS * CONTEXT_COMPRESS_AT:
            keep = max(MIN_RESEARCH_CONTEXT_ENTRIES + 5, len(RESEARCH_CONTEXT) * 3 // 4)
            del RESEARCH_CONTEXT[:-keep]
            print(f"[research] trimmed context, kept last {keep} entries")

        # Build XML from RESEARCH_CONTEXT using the same escaper as the main loop
        def _esc(v):
            return str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        xml_research = "<context>\n"
        for entry in RESEARCH_CONTEXT:
            xml_research += "  <entry>\n"
            for k, v in entry.items():
                xml_research += f"    <{k}>{_esc(v)}</{k}>\n"
            xml_research += "  </entry>\n"
        xml_research += "</context>"

        # Build urgency hint
        urgency = ""
        if remaining <= 0:
            urgency = (
                "\n\n*** THIS IS YOUR LAST ITERATION. You MUST call saveCodebaseContext(report) "
                "NOW with whatever information you have gathered. Do NOT call any other tools. "
                "Failure to call saveCodebaseContext() means ALL your research is lost. ***"
            )
        elif remaining <= 3:
            urgency = (
                f"\n\n*** Only {remaining} iterations remaining. You MUST call "
                "saveCodebaseContext(report) very soon. If you already have enough information "
                "about the codebase, call saveCodebaseContext() now. ***"
            )
        elif remaining <= 7:
            urgency = (
                f"\n\n({remaining} iterations remaining. Make sure you are gathering enough "
                "information to write a complete report before you run out of turns.)"
            )

        # Nudge if model produced prose-only responses multiple times in a row
        prose_nudge = ""
        if last_response_was_prose_only >= 2:
            prose_nudge = (
                "\n\n*** Your last responses had NO tool calls. You MUST emit JSON tool blocks "
                "to call tools. Example: ```json\n{\"tool\": \"readSourceFile\", \"args\": {\"rel_path\": \"render/render.c\"}}\n```\n"
                "Do not just describe what you would do — actually call the tools. ***"
            )

        prompt = (
            RESEARCH_SYSTEM_PROMPT
            + f"\n\nIteration {iteration}/{RESEARCH_MAX_ITERATIONS} ({remaining} remaining)"
            + urgency
            + prose_nudge
            + "\n\nContext:\n" + xml_research
        )
        print(f"[research] iteration {iteration}/{RESEARCH_MAX_ITERATIONS} (context ~{len(xml_research)//4} tokens, {remaining} remaining)")

        try:
            response = model.getResponse(prompt, model=RESEARCH_MODEL, provider=RESEARCH_PROVIDER)
        except Exception as e:
            print(f"[research] model error: {e}, retrying …")
            continue

        if response is None:
            print("[research] model returned None, retrying …")
            continue

        response_clean = re.sub(r"<think>.*?</think>", "", response, flags=re.DOTALL).strip()
        RESEARCH_CONTEXT.append({
            "type": "model_response",
            "iteration": iteration,
            "output": response_clean,
        })
        print(f"[research] model responded ({len(response_clean)} chars)")

        results = executor.executeAll(response, research_tool_map, context=RESEARCH_CONTEXT)
        if results:
            last_response_was_prose_only = 0
        else:
            last_response_was_prose_only += 1
            print(f"[research] no tool calls in response ({last_response_was_prose_only} consecutive prose-only responses)")

        # Stop as soon as saveCodebaseContext was successfully called
        for r in results:
            if r["tool"] == "saveCodebaseContext" and r["error"] is None:
                print("[research] Research phase complete.")
                return

    print(f"[research] WARNING: reached max iterations ({RESEARCH_MAX_ITERATIONS}) without "
          "saveCodebaseContext being called. Partial results (if any) may exist.")


PLAN_PROMPT = """\
You are an expert C software engineer reviewing your own optimization session. \
Your ONLY task right now is to think deeply and produce a structured plan. \
Do NOT call any tools. Do NOT write any code. Do NOT emit any JSON tool blocks.

Review everything in the context — flame graph data, bench results, code you have \
already read, notes on the board, completed changes, regressions — and answer:

1. What have I already tried and what were the results? (be specific: function names, \
   measured numbers from bench results)
2. What are the current top hotspots that are NOT yet addressed?
3. What is the single most impactful function I should optimize next, and why?
4. What optimization strategies should I try for that function? (e.g. SIMD, loop \
   unrolling, data layout changes, algorithmic improvements, branchless techniques)
5. What do I need to read first to write a correct micro-benchmark for this function?
6. Are there any risks or constraints I should keep in mind?
7. Is there anything I have been stubbornly repeating that is not working? Be honest.

REMEMBER: The workflow is ISOLATION-FIRST. Your next step after planning should be:
  read function code -> createFuncBench with original + optimized variants -> \
  runFuncBench -> prove speedup -> only then apply to main code.

After your analysis, call addNote() with a summary of this plan (use [PLAN] prefix), \
then call addTask() for each concrete next step. Then resume normal tool-calling \
with the SYSTEM_PROMPT workflow.

Reply in plain prose. Be specific: name functions, file paths, and measured numbers \
from the context where relevant.
"""

SYSTEM_PROMPT = """\
You are an expert C software engineer. Your job is to analyse a renderer/engine \
codebase and iteratively improve its performance. You work in an ISOLATION-FIRST loop:

  profile -> identify hotspot -> read function code -> write optimized variant \
in bench/ folder -> benchmark in isolation -> validate correctness -> \
only then apply proven optimization to main codebase -> build -> bench -> PR

The KEY PRINCIPLE is: NEVER optimize the main codebase directly. Always first write \
and prove your optimization in the bench/ micro-benchmark sandbox, similar to how \
tests/rayAABB_inv.h contains multiple versions (V1 original, V2, V3, V4 AVX2, etc.) \
with timing and correctness validation. Only after proving a speedup in isolation \
should you apply the change to the main code.

The session header shows your current iteration, iterations since last successful \
change, and whether you have uncommitted modifications. Use this to gauge progress.

== CALLING TOOLS ==
To call a tool, emit one or more fenced JSON blocks anywhere in your response.
Every block must be valid JSON with "tool" (string) and "args" (object) fields:

```json
{"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
```

```json
{"tool": "createFuncBench", "args": {"func_name": "myFunc", "header_code": "...", "impl_code": "..."}}
```

Multiple blocks in one response are executed in order. Omitting "args" is also \
accepted and treated as an empty object. Every tool result is appended to the \
context so you can see it in the next turn. Use apiHelp() (already in context) to \
list all available tools and their exact argument names.

== WORKFLOW (follow this order strictly) ==
1. Read the flame graph and bench results already in the context.
2. Use showBoard() to review your current task list and notes.
3. Pick the top hotspot function. Use hotAnnotateFunc(func) to see exactly which \
lines are hot, or showContext(func, depth=2) / showSrcPair(path) to read the code.
4. BEFORE writing any code, record your findings: \
call addNote() with the hotspot name and why it is slow, \
call addTask() for each concrete optimization you plan to try.
5. **CREATE A MICRO-BENCHMARK** (this is MANDATORY for every optimization): \
call createFuncBench(func_name, header_code, impl_code) where:
   - header_code contains the original function + your optimized variant declarations
   - impl_code contains a main() that:
     a. Generates random test data (large sample size, e.g. millions of calls)
     b. Runs a warm-up pass
     c. Times the ORIGINAL function (baseline)
     d. Times your OPTIMIZED variant(s)
     e. Validates correctness (compare outputs of original vs optimized)
     f. Prints timing results and speedup ratio
   Follow the pattern in tests/rayAABB_inv.h and tests/rayAABB_inv.c as a reference: \
multiple versions (V1=original, V2=optimized, V3=SIMD, etc.) all benchmarked \
and validated against the original.
6. Call runFuncBench(func_name) to compile and run the benchmark.
7. Record results with addNote(). If the optimization shows a measurable speedup \
AND passes correctness validation, proceed to step 8. If not, try a different \
approach (go back to step 5 with a new variant) or move to a different hotspot.
8. **ONLY AFTER PROVING SPEEDUP**: Apply the proven optimization to the main \
codebase using editing tools:\
\n   - searchReplace(rel_path, old_text, new_text) — preferred, no line numbers needed\
\n   - searchReplaceMulti(rel_path, [{"old":...,"new":...},...]) — multiple edits in one call\
\n   - applyChange(func_name, new_definition) — replace a whole named function
9. Call buildProject(). If it fails, read the error, fix the code, rebuild.
10. Call makeBench() and compare against the baseline. If performance regressed, \
call restoreAll() and add a note explaining what went wrong.
11. When a change is solid (measurable improvement, no visual regression), call \
createPR() with a clear title and body, then call markTaskDone().
12. Call deleteFuncBench(func_name) to clean up, then continue with the next hotspot.

== NOTE-TAKING RULES ==
- ALWAYS call addNote() / addTask() BEFORE reading code or applying changes.
- After every bench result, call addNote() with the measured numbers.
- When you discover something about the codebase, call addNote() immediately.
- Notes and tasks survive context compression — they are your long-term memory.
  Anything not written to the board WILL be forgotten when context is trimmed.
- Use showBoard() at the start of each iteration to recall your plan.

== MICRO-BENCHMARK PATTERN (MANDATORY) ==
Look at tests/rayAABB_inv.h and tests/rayAABB_inv.c for the gold standard example:
- The .h file contains the ORIGINAL function + multiple optimized variants (V2, V3, V4, V5, V6, V7)
- The .c file generates random test data, times every variant, compares results for correctness
- Each variant explores a different optimization strategy (pass-by-value, FMA, SSE, AVX2, SoA layout)
- Results print ns/call for each variant so you can see exactly which is fastest

Your bench files should follow this SAME pattern:
1. createFuncBench(func_name, header_code, impl_code)
   Creates bench/<func_name>.h and bench/<func_name>.c.
   - header_code: #ifndef guard, the ORIGINAL function copied verbatim, then your \
optimized variant(s) with different names (e.g. funcV2, funcV3, funcV4_sse, etc.)
   - impl_code: full .c file with main() that:
     a. Allocates large arrays of random test data (millions of samples).
     b. Runs warm-up passes for all variants.
     c. Times each variant using clock_gettime(CLOCK_MONOTONIC).
     d. Prints ns/call (or ms/call) for each variant.
     e. Validates ALL optimized variants produce the same output as the original.
     f. Reports mismatches if any.
   The file may #include project headers by path if needed (e.g. "../render/render.h", \
"../object/format.h"), but must NOT depend on OpenCL or minifb.

2. runFuncBench(func_name)
   Compiles bench/<func_name>.c (linked with tests/timings.c and -lm) and runs it.
   Returns the full stdout output with your timing and correctness results.

3. deleteFuncBench(func_name)
   Removes the bench files and binary when you are done.

== WHEN YOU MAY SKIP THE SANDBOX ==
Only skip the micro-benchmark when:
- The change requires OpenCL, minifb, or other project-specific infrastructure \
that cannot be isolated.
- The change is purely structural (data layout change that affects the whole pipeline).
In these rare cases, apply directly and use makeBench() to validate.

== ANTI-PATTERNS: Things you MUST NOT do ==
1. DO NOT apply optimizations directly to the main codebase without first proving \
   them in a micro-benchmark. This is the #1 rule.
2. DO NOT read the same source file more than twice without making a change.
   If you keep re-reading, you are stuck — call addNote() and move on.
3. DO NOT call buildProject() + makeBench() without having changed any code.
4. DO NOT apply a change, then immediately restoreAll() and re-apply the same change.
   If a change regresses, document why and try a different approach.
5. DO NOT propose changes without first reading the relevant source code.
6. DO NOT ignore build errors. If buildProject() fails, fix the error immediately.
7. DO NOT keep calling the same exploration tool over and over.

== CONSTRAINTS ==
- Only call tools that are listed in apiHelp(). Anything else will be rejected.
- File paths must be relative to the project root. Never use ".." to escape it.
- Do not break the public API (function signatures visible in headers) without \
explicit instruction.
- Always build and bench after applying a proven optimization to main code.
- If you are unsure whether a change is safe, benchmark it in isolation first.
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
{codebase_context_section}"""

# TODO: add api where model can execute some code in a sandbox

def _buildSystemPrompt():
    """Return (prompt_str, codebase_ctx_or_None) with the codebase context section injected."""
    ctx = loadCodebaseContext()
    if ctx:
        section = (
            "\n\n== CODEBASE KNOWLEDGE BASE ==\n"
            "The following pre-built research report describes this codebase's "
            "architecture and known performance hotspots. Use it as your primary "
            "reference before reading source files.\n\n"
            + ctx
        )
    else:
        section = ""
    return SYSTEM_PROMPT.replace("{codebase_context_section}", section), ctx


if __name__ == "__main__":
    import argparse
    _parser = argparse.ArgumentParser(description="llmOpt agentic optimization loop")
    _parser.add_argument(
        "--research", action="store_true",
        help="Run the codebase research phase and save codebase_context.md, then exit."
    )
    _args = _parser.parse_args()

    ui.start()
    git_pull_project()
    gf.init()

    if _args.research:
        print("[main] Running codebase research phase …")
        runCodebaseResearch()
        print(f"[main] Research complete. Context saved to {CODEBASE_CONTEXT_FILE}")
        sys.exit(0)

    # Sync any leftover planner notes from the previous run into
    # codebase_context.md before wiping the board for a fresh session.
    syncPlannerToCodebaseContext()

    planner.resetBoard()

    getTree()
    # getTodos()
    buildProject()
    _ , BASELINE_RESULTS = makeBench()
    flame = makeFlame()

    # Pin top 5 hotspots so the model always knows what to target,
    # even after context compression trims the raw flame data.
    if flame and flame.get("hot_functions"):
        top5 = flame["hot_functions"][:5]
        PINNED_HOTSPOTS = (
            "Top hotspots (always visible):\n" +
            "\n".join(
                f"  {h['fn']:40s}  incl={h['inclusive_pct']:5.1f}%  excl={h['exclusive_pct']:5.1f}%"
                for h in top5
            )
        )

    gf.listFunctions(context=CONTEXT)
    gf.apiHelp(context=CONTEXT)

    _SYSTEM_PROMPT, codebase_ctx = _buildSystemPrompt()
    if codebase_ctx:
        print(f"[main] Loaded codebase context ({len(codebase_ctx)} chars) from {CODEBASE_CONTEXT_FILE}")
        CONTEXT.insert(0, {
            "type": "codebase_knowledge",
            "tool": "loadCodebaseContext",
            "output": codebase_ctx,
        })
    else:
        print(f"[main] No codebase_context.md found. Run with --research to generate it.")

    TOOL_MAP = executor.buildToolMap(gf, planner, sys.modules[__name__])
    iteration = 0
    _recent_calls = []          # (tool, args_str) tuples for loop detection
    _recent_tool_names = []     # tool names only, for broader pattern detection
    _consecutive_prose_only = 0 # count of responses with no tool calls
    _last_pr_iteration = 0      # iteration when last PR was created
    _last_build_bench_iteration = 0  # iteration when build+bench last succeeded
    _plan_at_iteration = random.randint(5, 8)  # next planning iteration

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
            _recent_tool_names.clear()
            print(f"[nudge] {nudge}")

        xmlContext, tokenCount = convertContextToXml()
        ui.set_token_count(tokenCount)
        ui.sync_context(CONTEXT)
        board = planner.showBoard(returnString=True)
        ui.sync_board(board)

        # Build session state header
        iters_since_pr = iteration - _last_pr_iteration if _last_pr_iteration else iteration
        iters_since_bench = iteration - _last_build_bench_iteration if _last_build_bench_iteration else iteration
        diff = gf.getDiff(returnString=True, code_only=True)
        has_changes = bool(diff and diff.strip())
        session_header = (
            f"== SESSION STATE ==\n"
            f"Total iterations: {iteration}\n"
            f"Iterations since last PR: {iters_since_pr}\n"
            f"Iterations since last successful build+bench: {iters_since_bench}\n"
            f"Uncommitted changes: {'YES -- call getDiff() to review' if has_changes else 'none'}\n"
            + (PINNED_HOTSPOTS + "\n" if PINNED_HOTSPOTS else "")
        )

        # Staleness warning
        staleness_warning = ""
        if iters_since_bench > 15:
            staleness_warning = (
                f"\n*** WARNING: {iters_since_bench} iterations without a successful "
                "build+bench cycle. You may be stuck in analysis paralysis. "
                "If you have a change ready, apply it now with searchReplace() and call buildProject(). "
                "If not, pick the simplest possible improvement and try it. ***"
            )
        elif iters_since_bench > 8:
            staleness_warning = (
                f"\n(Note: {iters_since_bench} iterations since last build+bench. "
                "Consider making a concrete change soon.)"
            )

        # Prose-only nudge
        prose_nudge = ""
        if _consecutive_prose_only >= 2:
            prose_nudge = (
                "\n*** Your last responses had NO tool calls. You MUST emit JSON tool blocks "
                "to take action. Example:\n"
                "```json\n{\"tool\": \"showContext\", \"args\": {\"func\": \"renderFrame\", \"depth\": 2}}\n```\n"
                "Do not just describe what you would do -- actually call the tools. ***"
            )

        # Decide whether this is a planning iteration
        is_plan_iteration = (iteration >= _plan_at_iteration and iters_since_pr >= 5)
        if is_plan_iteration:
            _plan_at_iteration = iteration + random.randint(5, 8)

        if is_plan_iteration:
            prompt = (
                PLAN_PROMPT
                + "\n\n" + session_header
                + "\n\n== CURRENT BOARD ==\n" + board
                + "\n\nContext:\n" + xmlContext
            )
            print(f"[plan] Iteration {iteration}: injecting planning step (next plan at {_plan_at_iteration})")
        else:
            prompt = (
                _SYSTEM_PROMPT
                + "\n\n" + session_header
                + staleness_warning
                + prose_nudge
                + "\n== CURRENT BOARD ==\n" + board
                + "\n\nContext:\n" + xmlContext
            )
            print(f"Iteration {iteration}: token count ~{tokenCount}")

        ui.set_status("waiting_model")
        steer = ui.pop_steer()
        _cleared = [False]
        def _on_token(delta):
            if not _cleared[0]:
                ui.clear_stream()
                _cleared[0] = True
            ui.push_token(delta)

        try:
            if is_plan_iteration:
                response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="baidu/fp8")
            else:
                response = model.getResponse(prompt, model="deepseek/deepseek-v4-flash", provider="baidu/fp8")
        except Exception as e:
            CONTEXT.append({
                "type": "model_response",
                "iteration": iteration,
                "output": f"Error getting model response: {e} try again in the next iteration."
            })
            continue

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

        results = executor.executeAll(response, TOOL_MAP, context=CONTEXT)

        # Track tool usage patterns
        for r in results:
            ui.log_tool_result(r["tool"], r["error"])
            tool_name = r["tool"]
            _recent_tool_names.append(tool_name)
            args_str = str(sorted((r.get("args") or {}).items()) if isinstance(r.get("args"), dict) else "")
            _recent_calls.append((tool_name, args_str))
            # Track build+bench success
            if tool_name == "makeBench" and r["error"] is None:
                _last_build_bench_iteration = iteration
            if tool_name == "buildProject" and r["error"] is None:
                pass  # bench must also succeed to count
            if tool_name == "createPR" and r["error"] is None:
                _last_pr_iteration = iteration
                _plan_at_iteration = iteration + 3  # plan soon after a PR

        _recent_calls = _recent_calls[-12:]
        _recent_tool_names = _recent_tool_names[-20:]

        # Track prose-only responses
        if results:
            _consecutive_prose_only = 0
        else:
            _consecutive_prose_only += 1
            if _consecutive_prose_only >= 3:
                print(f"[stall] {_consecutive_prose_only} consecutive prose-only responses")

        # --- LOOP DETECTION ---

        # Pattern 1: same tool+args 4 times in a row
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
            _recent_tool_names.clear()
            print(f"[loop-detect] injected nudge for repeated '{loop_tool}'")

        # Pattern 2: readSourceFile or showContext called 5+ times in last 8 without any edit
        if len(_recent_tool_names) >= 8:
            recent_8 = _recent_tool_names[-8:]
            read_count = sum(1 for t in recent_8 if t in ("readSourceFile", "showContext", "showSrc", "showSrcPair"))
            edit_count = sum(1 for t in recent_8 if t in ("searchReplace", "searchReplaceMulti", "applyChange",
                                                           "insertLines", "deleteLines", "applyPatch"))
            if read_count >= 5 and edit_count == 0:
                nudge = (
                    "[AUTO-NUDGE] You have read source files 5+ times in recent iterations "
                    "without making any changes. You are over-researching. "
                    "Pick ONE hotspot function and call createFuncBench() to write a "
                    "micro-benchmark with the original + your optimized variant. "
                    "Prove the optimization works, then apply to main code."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                _recent_tool_names.clear()
                print("[loop-detect] injected nudge for over-reading without editing")

        # Pattern 3: buildProject or makeBench called 4+ times without edits in between
        if len(_recent_tool_names) >= 6:
            recent_6 = _recent_tool_names[-6:]
            build_bench_count = sum(1 for t in recent_6 if t in ("buildProject", "makeBench"))
            edit_count_6 = sum(1 for t in recent_6 if t in ("searchReplace", "searchReplaceMulti", "applyChange",
                                                              "insertLines", "deleteLines", "applyPatch"))
            if build_bench_count >= 4 and edit_count_6 == 0:
                nudge = (
                    "[AUTO-NUDGE] You have called buildProject/makeBench multiple times "
                    "without applying any code changes. This wastes time. "
                    "If you are trying to understand the baseline, look at the bench results "
                    "already in context. If you want to make a change, use searchReplace()."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                _recent_tool_names.clear()
                print("[loop-detect] injected nudge for build/bench without edits")

        # Pattern 4: editing main code without first using createFuncBench/runFuncBench
        if len(_recent_tool_names) >= 3:
            recent_all = _recent_tool_names[-10:]
            edit_tools = EDITING_TOOLS
            bench_tools = ("createFuncBench", "runFuncBench")
            has_edit = any(t in edit_tools for t in recent_all[-3:])
            has_bench = any(t in bench_tools for t in recent_all[-10:])
            if has_edit and not has_bench:
                nudge = (
                    "[AUTO-NUDGE] You are editing the main codebase WITHOUT first proving "
                    "your optimization in a micro-benchmark. The workflow is ISOLATION-FIRST:\n"
                    "1. Read the hot function code\n"
                    "2. createFuncBench() with original + optimized variants\n"
                    "3. runFuncBench() to prove speedup and correctness\n"
                    "4. Only THEN apply to main code with searchReplace()\n"
                    "If you already ran a bench and it was trimmed from context, call addNote() "
                    "to record the result and proceed. Otherwise, use createFuncBench() first."
                )
                CONTEXT.append({"type": "intervention", "tool": "nudge", "output": nudge})
                print("[loop-detect] injected nudge for editing without bench-first")

        ui.sync_context(CONTEXT)
        ui.sync_diff(gf.getDiff(returnString=True, code_only=True))

        removeDoublesFromContext()