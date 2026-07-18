"""
research_agent.py -- Read-only sub-agent for codebase exploration.

Launches an agentic loop with a separately configurable model (default DeepSeek
V4 Flash) that has access ONLY to read-only and diagnostic tools.  The agent
cannot modify source code, create commits, open PRs, or launch reviews.

Used by the MCP server's research_agent() tool.  The main optimization agent
calls this when it needs to deeply understand a subsystem before implementing
changes.

Usage:
    import research_agent as ra
    findings = ra.runResearch("Trace the full call chain of Trace() through all callees")
"""

import json
import os
import re
import sys

# Ensure llmOpt is on the path for sibling imports.
_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))
if _llmOpt_dir not in sys.path:
    sys.path.insert(0, _llmOpt_dir)

import getFunc as _gf
import main as _main
import model_config as _mc
import modelSelector as _model

# ----- JSON tool-call parsing (same pattern as executor.py) ------------------
_CMD_RE = re.compile(r'```json\s*(\{.*?\})\s*```', re.DOTALL)

MAX_ITERATIONS = 15

# ----- System prompt for the research agent ----------------------------------

RESEARCH_SYSTEM_PROMPT = """\
You are a thorough codebase research agent. Your job is to explore and understand
a C codebase (a real-time CPU ray tracer) and report your findings. You have
access to READ-ONLY tools — you CANNOT modify any files, create commits, or
change state.

WORKFLOW:
1. Start by reading relevant files and tracing call chains.
2. Use exploration tools to understand data structures, function relationships,
   and performance characteristics.
3. If profiling data is available, use it to identify hotspots.
4. Check the codebase context (accumulated knowledge from prior sessions).
5. When you have a solid understanding, produce a structured final report.

RULES:
- Be thorough. Read source files, trace callers/callees, check struct definitions.
- Prefer breadth-first exploration: understand the big picture before diving deep.
- Cite specific file paths and line numbers in your findings.
- If a tool returns an error or empty result, try a different approach.
- Do NOT guess. If you cannot determine something, say so.

FINAL REPORT FORMAT:
When you are done researching, produce a final message (with NO tool calls) that
contains:

## Research Findings: <topic>

### Key Files
- path/to/file.c — what it does and why it matters

### Key Functions
- FunctionName() in file.c:line — purpose, callers, callees, performance notes

### Data Structures
- StructName in file.h — fields and their roles

### Call Graph Summary
Describe the main call chain(s) from entry point to leaf functions.

### Performance Observations
Any hotspots, memory patterns, or optimization opportunities noted.

### Recommendations
What the main agent should focus on when optimizing this area.

Reply with your final report as plain text (no JSON, no tool calls).
"""

# ----- Tool definitions ------------------------------------------------------

def _resolveProjectDir():
    """Determine the gengin project directory for tool calls."""
    # Use the same directory that mcp_server.py configured in _main.PROJECT_DIR.
    if hasattr(_main, 'PROJECT_DIR') and _main.PROJECT_DIR:
        return _main.PROJECT_DIR
    # Fallback: llmOpt/gengin/
    candidate = os.path.join(_llmOpt_dir, "gengin")
    if os.path.isdir(candidate):
        return candidate
    return os.getcwd()


def _makeToolMap():
    """Build the read-only tool map as {name: (callable, description)}.

    Each callable takes a single dict (the JSON "args" object) and returns a
    string result.  This keeps dispatch simple and consistent.
    """
    proj = _resolveProjectDir()

    tools = {}

    # -- Exploration (getFunc.py) --

    tools["show_context"] = (
        lambda a: _gf.showContext(
            a["func"], depth=a.get("depth", 1), returnString=True, context=None,
        ),
        "Show a function, its callees, and the structs it uses. "
        "Args: func (str), depth (int, default 1).",
    )

    tools["get_definition"] = (
        lambda a: _gf.getDefinition(a["name"], returnString=True, context=None),
        "Print the full source definition of a function or struct. "
        "Args: name (str).",
    )

    tools["get_callers"] = (
        lambda a: _gf.getCallers(a["func"], returnString=True, context=None),
        "List all functions that call the given function. "
        "Args: func (str).",
    )

    tools["grep_source"] = (
        lambda a: _gf.grepSource(
            a["pattern"],
            a.get("rel_path"),
            a.get("ignore_case", False),
            returnString=True, context=None,
        ),
        "Search for a regex pattern across .c/.h/.cl files. "
        "Args: pattern (str), rel_path (str, optional), ignore_case (bool, default false).",
    )

    tools["find_symbol"] = (
        lambda a: _gf.findSymbol(a["name"], returnString=True, context=None),
        "Word-boundary grep for an identifier across the codebase. "
        "Args: name (str).",
    )

    tools["list_dir"] = (
        lambda a: _gf.listDir(a.get("rel_path", "."), returnString=True, context=None),
        "List files and directories under a path. "
        "Args: rel_path (str, default '.').",
    )

    tools["read_lines"] = (
        lambda a: _gf.readLines(
            a["rel_path"], a["start"], a["end"],
            returnString=True, context=None,
        ),
        "Read a specific line range from a file with line numbers. "
        "Args: rel_path (str), start (int), end (int).",
    )

    tools["read_source_file"] = (
        lambda a: _gf.readSourceFile(a["rel_path"], returnString=True, context=None),
        "Read the raw content of a source file. "
        "Args: rel_path (str).",
    )

    tools["list_functions"] = (
        lambda a: _gf.listFunctions(returnString=True, context=None),
        "List all indexed functions in the project, sorted by file. "
        "Args: (none).",
    )

    tools["show_src"] = (
        lambda a: _gf.showSrc(a["rel_path"], returnString=True, context=None),
        "Show a source file with line number prefixes. "
        "Args: rel_path (str).",
    )

    tools["show_src_pair"] = (
        lambda a: _gf.showSrcPair(a["rel_path"], returnString=True, context=None),
        "Show a .c file with its companion .h file. "
        "Args: rel_path (str) — path to the .c file.",
    )

    # -- Project-level tools (main.py) --

    tools["get_tree"] = (
        lambda a: _main.getTree(a.get("path", proj)),
        "Return an ASCII directory tree of the project. "
        "Args: path (str, default: project root).",
    )

    tools["get_todos"] = (
        lambda a: _main.getTodos(a.get("path", ".")),
        "Grep all TODO comments in the project. "
        "Args: path (str, default '.').",
    )

    # -- Profiling (main.py) --

    tools["make_flame"] = (
        lambda a: json.dumps(_main.makeFlame(), indent=2),
        "Run perf record + flamegraph, return top-25 hotspots and top-20 call paths. "
        "Args: (none).",
    )

    tools["hot_annotate_func"] = (
        lambda a: _gf.hotAnnotateFunc(
            a["func_name"],
            a.get("threshold", 0.5),
            context=None,
        ),
        "Annotate a function with per-line perf sample percentages. "
        "Args: func_name (str), threshold (float, default 0.5).",
    )

    tools["hot_annotate_file"] = (
        lambda a: _gf.hotAnnotateFile(
            a["rel_path"],
            a.get("threshold", 0.5),
            context=None,
        ),
        "Annotate an entire file with per-line perf sample percentages. "
        "Args: rel_path (str), threshold (float, default 0.5).",
    )

    # -- Micro-benchmarks (main.py) --

    tools["run_func_bench"] = (
        lambda a: _main.runFuncBench(a["func_name"]),
        "Build and run a micro-benchmark binary. The benchmark must already "
        "exist (created by the main agent). "
        "Args: func_name (str).",
    )

    tools["run_perf_stat"] = (
        lambda a: _main.runPerfStat(a["func_name"]),
        "Run perf stat on a micro-benchmark binary to collect hardware counters "
        "(IPC, cache-miss rate, branch-miss rate). "
        "Args: func_name (str).",
    )

    # -- Knowledge base (main.py) --

    tools["get_codebase_context"] = (
        lambda a: _main.getCodebaseContext(a.get("section", "")),
        "Read the persisted codebase knowledge base (accumulated insights from "
        "prior optimization sessions). "
        "Args: section (str, optional — filter by section name).",
    )

    # -- LSP / clangd tools --

    tools["lsp_diagnostics"] = (
        lambda a: _lspDiagnostics(a["rel_path"]),
        "Get compiler warnings/errors for a file via clangd (fast, ~1s). "
        "Args: rel_path (str).",
    )

    tools["lsp_symbols"] = (
        lambda a: _lspSymbols(a["rel_path"]),
        "List all document symbols (functions, structs, etc.) in a file via clangd. "
        "Args: rel_path (str).",
    )

    tools["lsp_hover"] = (
        lambda a: _lspHover(a["symbol"], a["rel_path"]),
        "Get type information and documentation for a symbol via clangd. "
        "Args: symbol (str), rel_path (str).",
    )

    return tools


# ----- LSP helpers (minimal, no symbol-position mapping needed) ---------------

_lsp_client = None

def _getLspClient():
    global _lsp_client
    if _lsp_client is None:
        import lsp_client as _lsp
        _lsp_client = _lsp.getClient(_resolveProjectDir())
    return _lsp_client


def _lspDiagnostics(rel_path: str) -> str:
    try:
        diags = _getLspClient().diagnostics(rel_path)
        if not diags:
            return f"No diagnostics for {rel_path}."
        lines = []
        for d in diags:
            sev = {1: "ERROR", 2: "WARN", 3: "INFO", 4: "HINT"}.get(d.get("severity", 0), "?")
            msg = d.get("message", "")
            rng = d.get("range", {})
            start = rng.get("start", {})
            line = start.get("line", 0) + 1
            lines.append(f"  [{sev}] line {line}: {msg}")
        return f"Diagnostics for {rel_path}:\n" + "\n".join(lines)
    except Exception as e:
        return f"LSP diagnostics error: {e}"


def _lspSymbols(rel_path: str) -> str:
    try:
        syms = _getLspClient().documentSymbol(rel_path)
        if not syms:
            return f"No symbols found in {rel_path}."
        lines = [f"Symbols in {rel_path}:"]
        for s in syms:
            name = s.get("name", "?")
            kind = s.get("kind", "?")
            rng = s.get("range", {})
            start = rng.get("start", {})
            line = start.get("line", 0) + 1
            lines.append(f"  {name} (kind={kind}) at line {line}")
        return "\n".join(lines)
    except Exception as e:
        return f"LSP symbols error: {e}"


def _lspHover(symbol: str, rel_path: str) -> str:
    try:
        # Use getFunc index to find line/char position for the symbol.
        funcs = getattr(_gf, '_functions', {})
        structs = getattr(_gf, '_structs', {})
        line0 = 0
        char0 = 0
        if symbol in funcs:
            info = funcs[symbol]
            line0 = info['start'] - 1
            char0 = info.get('sig', '').find(symbol)
            if char0 < 0:
                char0 = 0
        elif symbol in structs:
            info = structs[symbol]
            line0 = info['start'] - 1
            char0 = info.get('full', '').find(symbol)
            if char0 < 0:
                char0 = 0
        else:
            return f"Symbol {symbol!r} not found in index for {rel_path}."

        # LSP hover requires a file:// URI.  Construct absolute path from the
        # project directory + relative path, mirroring mcp_server.py's approach.
        proj_dir = _resolveProjectDir()
        file_uri = f"file://{os.path.join(proj_dir, rel_path)}"

        result = _getLspClient()._request("textDocument/hover", {
            "textDocument": {"uri": file_uri},
            "position": {"line": line0, "character": char0},
        })
        if not result:
            return f"No hover info for {symbol} in {rel_path}."
        contents = result.get("contents", {})
        if isinstance(contents, dict):
            return contents.get("value", str(contents))
        if isinstance(contents, list):
            return "\n".join(
                c.get("value", str(c)) if isinstance(c, dict) else str(c)
                for c in contents
            )
        return str(contents)
    except Exception as e:
        return f"LSP hover error: {e}"


# ----- Tool description for the system prompt ---------------------------------

def _buildToolDescriptions(tool_map):
    """Generate a concise tool reference for the system prompt."""
    lines = ["## Available Tools (READ-ONLY)\n"]
    for name, (_, desc) in sorted(tool_map.items()):
        lines.append(f"- **{name}**: {desc}")
    lines.append(
        "\n## How to Call Tools\n"
        "Respond with a JSON code block for each tool call:\n"
        "```json\n"
        '{"tool": "tool_name", "args": {"arg1": "value", "arg2": 42}}\n'
        "```\n"
        "You can call multiple tools in one response — each in its own "
        "```json block.  Tool results will be appended and you can continue.\n"
        "\nWhen you are finished, respond with your final report (plain text, "
        "no JSON, no tool calls)."
    )
    return "\n".join(lines)


# ----- Main agent loop --------------------------------------------------------

def runResearch(research_prompt: str) -> str:
    """Launch the research agent with the given prompt and return its findings.

    The agent uses the RESEARCH_MODEL from model_config (independently
    configurable, default DeepSeek V4 Flash).  It has access to a curated
    set of read-only tools and runs for up to MAX_ITERATIONS tool-calling
    rounds before returning whatever it has found.
    """
    tool_map = _makeToolMap()
    tool_descriptions = _buildToolDescriptions(tool_map)

    model_id = _mc.getResearchModel()
    provider = _mc.getResearchProvider()

    conversation = [
        {"role": "system", "content": RESEARCH_SYSTEM_PROMPT + "\n\n" + tool_descriptions},
        {"role": "user", "content": research_prompt},
    ]

    for iteration in range(1, MAX_ITERATIONS + 1):
        # Build the full prompt from the conversation.
        prompt_parts = []
        for msg in conversation:
            role = msg["role"].upper()
            prompt_parts.append(f"[{role}]\n{msg['content']}\n")
        full_prompt = "\n".join(prompt_parts)

        try:
            raw = _model.getResponse(
                full_prompt,
                model=model_id,
                provider=provider,
                reasoning_effort=None,  # no thinking needed for research
            )
        except Exception as e:
            return f"Research agent LLM call failed (iteration {iteration}): {e}"

        # Strip any leaked thinking tags.
        raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()

        # Handle empty or whitespace-only responses.
        if not raw:
            if iteration < MAX_ITERATIONS:
                conversation.append({"role": "assistant", "content": "(empty response)"})
                continue
            return "RESEARCH AGENT: Model returned an empty response. The research could not be completed."

        # Parse tool calls from the response.
        cmds = _CMD_RE.findall(raw)
        if not cmds:
            # No tool calls — this is the final answer.
            return raw

        # Execute each tool call and collect results.
        tool_results = []
        for cmd_str in cmds:
            try:
                cmd = json.loads(cmd_str)
            except json.JSONDecodeError:
                tool_results.append(f"Invalid JSON in tool call: {cmd_str[:200]}")
                continue

            if not isinstance(cmd, dict):
                tool_results.append(
                    f"Tool call must be a JSON object, got {type(cmd).__name__}: {cmd_str[:200]}"
                )
                continue

            tool_name = cmd.get("tool", "")
            args = cmd.get("args", {})

            if tool_name not in tool_map:
                tool_results.append(
                    f"Unknown tool: {tool_name!r}. Available: {', '.join(sorted(tool_map.keys()))}"
                )
                continue

            fn, _desc = tool_map[tool_name]
            try:
                result = fn(args)
                tool_results.append(f"[{tool_name} result]\n{result}")
            except Exception as e:
                tool_results.append(f"[{tool_name} ERROR]\n{e}")

        # Append the assistant's response and tool results to the conversation.
        conversation.append({"role": "assistant", "content": raw})
        conversation.append({
            "role": "tool_results",
            "content": "\n\n".join(tool_results),
        })

        # Prevent unbounded conversation growth — keep only the last 20 messages.
        if len(conversation) > 22:  # system + user + 10 pairs
            conversation = [conversation[0], conversation[1]] + conversation[-20:]

    # Max iterations reached — return the last response with a warning.
    return (
        "RESEARCH AGENT: Maximum iterations reached. Here is the last response:\n\n"
        + raw
        + "\n\nNOTE: Research was truncated. Consider narrowing your research prompt."
    )
