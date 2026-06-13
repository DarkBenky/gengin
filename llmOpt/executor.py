"""
executor.py -- safe model command parser and dispatcher.

Model responses must contain one or more ```json blocks, each a valid JSON object
with "tool" (string) and "args" (object, may be omitted) fields:

    ```json
    {"tool": "showContext", "args": {"func": "renderFrame", "depth": 2}}
    ```

    ```json
    {"tool": "getDiff", "args": {}}
    ```

Multiple blocks in one response are executed in order.
Only tools registered in the tool map can be called.
The regex captures from the outer { to the outer } via backtracking against the
closing fence, so nested objects/arrays in args are handled correctly.
"""

import inspect
import json
import re

_CMD_RE = re.compile(r'```json\s*(\{.*?\})\s*```', re.DOTALL)

# Argument keys that are treated as file paths and get traversal-checked.
_PATH_ARGS = {"rel_path", "file", "path"}


class ExecutorError(Exception):
    pass


def _sig_hint(fn):
    """Return a human-readable call signature for error messages."""
    try:
        sig = inspect.signature(fn)
        params = [
            p for p in sig.parameters.values()
            if p.name not in ("context",)
        ]
        parts = []
        for p in params:
            if p.default is inspect.Parameter.empty:
                parts.append(p.name)
            else:
                parts.append(f"{p.name}={p.default!r}")
        return f"{fn.__name__}({', '.join(parts)})"
    except Exception:
        return fn.__name__


def _unknown_tool_hint(tool, tool_map):
    import difflib
    close = difflib.get_close_matches(tool, tool_map.keys(), n=3, cutoff=0.5)
    hint = f"Unknown tool: {tool!r}."
    if close:
        hint += f" Did you mean: {', '.join(close)}?"
    hint += " Call apiHelp() to list all available tools."
    return hint



def _checkPath(value):
    """Reject anything that tries to escape the project directory."""
    if not isinstance(value, str):
        return
    normalized = value.replace("\\", "/")
    if ".." in normalized.split("/"):
        raise ExecutorError(f"path traversal rejected: {value!r}")


def extractCommands(text):
    """Return list of raw dicts parsed from all ```json blocks in text."""
    cmds = []
    for m in _CMD_RE.finditer(text):
        try:
            cmds.append(json.loads(m.group(1)))
        except json.JSONDecodeError as e:
            raise ExecutorError(f"invalid JSON in command block: {e}\n{m.group(1)}")
    return cmds


def validateCommand(cmd, tool_map):
    """
    Raise ExecutorError if the command is malformed or disallowed.
    Returns (tool_name, args_dict).
    """
    if not isinstance(cmd, dict):
        raise ExecutorError(f"command must be a JSON object, got: {type(cmd).__name__}")

    tool = cmd.get("tool")
    if not tool:
        raise ExecutorError("command missing 'tool' field")
    if not isinstance(tool, str):
        raise ExecutorError(f"'tool' must be a string, got: {type(tool).__name__}")
    if tool not in tool_map:
        raise ExecutorError(_unknown_tool_hint(tool, tool_map))

    args = cmd.get("args", {})
    if not isinstance(args, dict):
        raise ExecutorError(f"'args' must be a JSON object, got: {type(args).__name__}")

    # path traversal check on known path arguments
    for key, value in args.items():
        if key in _PATH_ARGS:
            _checkPath(value)

    return tool, args


def executeCommand(cmd, tool_map, context=None):
    """
    Parse, validate and execute a single command dict.
    Returns {"tool": name, "result": ..., "error": None|str}.
    """
    try:
        tool, args = validateCommand(cmd, tool_map)
    except ExecutorError as e:
        msg = str(e)
        if context is not None:
            context.append({"type": "tool_error", "tool": cmd.get("tool", "?"), "error": msg})
        return {"tool": cmd.get("tool", "?"), "result": None, "error": msg}

    fn = tool_map[tool]
    try:
        sig = inspect.signature(fn)
        if context is not None and "context" in sig.parameters:
            result = fn(**args, context=context)
        else:
            result = fn(**args)
        return {"tool": tool, "result": result, "error": None}
    except TypeError as e:
        hint = f"wrong arguments for {tool!r}: {e}. Expected signature: {_sig_hint(fn)}"
        if context is not None:
            context.append({"type": "tool_error", "tool": tool, "error": hint})
        return {"tool": tool, "result": None, "error": hint}
    except Exception as e:
        msg = f"{type(e).__name__}: {e}"
        if context is not None:
            context.append({"type": "tool_error", "tool": tool, "error": msg})
        return {"tool": tool, "result": None, "error": msg}


def executeAll(text, tool_map, context=None):
    """
    Extract all ```json commands from model text, execute each in order.
    Returns list of result dicts. Prints a summary line per command.
    """
    try:
        cmds = extractCommands(text)
    except ExecutorError as e:
        print(f"[executor] parse error: {e}")
        return [{"tool": "?", "result": None, "error": str(e)}]

    if not cmds:
        print("[executor] no commands found in response")
        return []

    results = []
    for cmd in cmds:
        r = executeCommand(cmd, tool_map, context=context)
        status = "ok" if r["error"] is None else f"ERROR: {r['error']}"
        print(f"[executor] {r['tool']} -> {status}")
        results.append(r)

    return results


def buildToolMap(gf_module, planner_module, main_module=None, refine_module=None):
    """
    Assemble the whitelist tool map from available modules.
    Pass main_module to include build/git/PR functions.
    Pass refine_module to include refinement loop tools.
    """
    gf = gf_module
    pl = planner_module

    tools = {
        # --- exploration ---
        "showContext":         gf.showContext,
        "showContextWithMeta": gf.showContextWithMeta,
        "getDefinition":       gf.getDefinition,
        "getCallers":          gf.getCallers,
        "showSrc":             gf.showSrc,
        "showSrcPair":         gf.showSrcPair,
        "listFunctions":       gf.listFunctions,
        "readSourceFile":      gf.readSourceFile,
        "hotAnnotateFunc":     gf.hotAnnotateFunc,
        "hotAnnotateFile":     gf.hotAnnotateFile,
        "grepSource":          gf.grepSource,
        "findSymbol":          gf.findSymbol,
        "listDir":             gf.listDir,
        "readLines":           gf.readLines,
        "runCommand":          gf.runCommand,
        "apiHelp":             gf.apiHelp,
        # --- applying changes ---
        "searchReplace":       gf.searchReplace,
        "searchReplaceMulti":  gf.searchReplaceMulti,
        "previewChange":       gf.previewChange,
        "applyChange":         gf.applyChange,
        "replaceLines":        gf.replaceLines,
        "insertLines":         gf.insertLines,
        "deleteLines":         gf.deleteLines,
        "applyPatch":          gf.applyPatch,
        # --- git / restore ---
        "getDiff":             gf.getDiff,
        "restoreAll":          gf.restoreAll,
        "restoreFile":         gf.restoreFile,
        "restoreFunction":     gf.restoreFunction,
        # --- planner ---
        "addTask":             pl.addTask,
        "addSubtask":          pl.addSubtask,
        "listTasks":           pl.listTasks,
        "markTaskDone":        pl.markTaskDone,
        "markTaskInProgress":  pl.markTaskInProgress,
        "markTaskTodo":        pl.markTaskTodo,
        "markTaskBlocked":     pl.markTaskBlocked,
        "markTaskConverged":   pl.markTaskConverged,
        "removeTask":          pl.removeTask,
        "clearDoneTasks":      pl.clearDoneTasks,
        "clearAllTasks":       pl.clearAllTasks,
        "addNote":             pl.addNote,
        "listNotes":           pl.listNotes,
        "removeNote":          pl.removeNote,
        "clearNotes":          pl.clearNotes,
        "showBoard":           pl.showBoard,
        "recordAttempt":       pl.recordAttempt,
        "getConvergenceReport": pl.getConvergenceReport,
    }

    if refine_module is not None:
        rf = refine_module
        tools.update({
            "initRefinement":         rf.toolRecordRefinementAttempt,
            "recordRefinementAttempt": rf.toolRecordRefinementAttempt,
            "convergeFunction":       rf.toolConvergeFunction,
            "avoidStrategy":          rf.toolAvoidStrategy,
            "getRefinementState":     rf.toolGetRefinementState,
            "getUntriedStrategies":   rf.toolGetUntriedStrategies,
            "addSubComponent":        rf.toolAddSubComponent,
        })

    if main_module is not None:
        m = main_module
        tools.update({
            "buildProject":    m.buildProject,
            "makeBench":       m.makeBench,
            "makeFlame":       m.makeFlame,
            "getTree":         m.getTree,
            "getTodos":        m.getTodos,
            "createPR":        m.createPR,
            "createFuncBench": m.createFuncBench,
            "runFuncBench":    m.runFuncBench,
            "runPerfStat":     m.runPerfStat,
            "deleteFuncBench": m.deleteFuncBench,
            "reviewChanges":   m.reviewChanges,
            "bisectRegression": m.bisectRegression,
            "syncPlannerToCodebaseContext": m.syncPlannerToCodebaseContext,
        })

    return tools
