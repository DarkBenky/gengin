## DEPRECATED — This module is part of the legacy custom agent orchestration layer.
## It is only used when running the old standalone loop via main.py directly.
## For the new MCP-server workflow, use mcp_server.py with an external agent
## harness (OpenCode) which manages its own planning and task tracking.
"""
planner.py -- persistent task and note board for the model's agentic loop.

State is kept in a JSON file (default: planner_state.json next to this module)
so it survives across multiple script invocations.

Task statuses: "todo" | "in_progress" | "done" | "blocked" | "converged"
Convergence: when further optimization of a target yields diminishing returns,
mark it "converged" rather than "done" so the model knows to move on.

Hierarchical tasks:
  Tasks can have parent_id pointing to another task, forming a tree.
  Subtasks are created via addSubtask(parent_id, text).
  When all subtasks of a parent are done/converged, the parent auto-completes.

Attempt tracking:
  Each task tracks attempts: what was tried, the result, and lessons learned.
  This survives context compression and feeds the refinement loop.
"""

import json
import os
import time

_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "planner_state.json")

_state = None

CONVERGENCE_MAX_ATTEMPTS = 5        # attempts before considering convergence
CONVERGENCE_IMPROVEMENT_THRESHOLD = 0.01  # 1% improvement to reset stall counter


def _load():
    global _state
    if _state is not None:
        return
    if os.path.exists(_STATE_FILE):
        with open(_STATE_FILE) as fh:
            _state = json.load(fh)
    else:
        _state = {"tasks": [], "notes": [], "refinement_log": []}

    # migrate old task format
    for t in _state.get("tasks", []):
        t.setdefault("parent_id", None)
        t.setdefault("attempts", [])
        t.setdefault("convergence_stall_count", 0)
        t.setdefault("best_result", None)
        t.setdefault("created_at", time.time())
    _state.setdefault("refinement_log", [])


def _save():
    with open(_STATE_FILE, "w") as fh:
        json.dump(_state, fh, indent=2)


def _next_id(items):
    if not items:
        return 1
    return max(item["id"] for item in items) + 1


# --- Tasks ---

def addTask(text, context=None, parent_id=None):
    _load()
    task = {
        "id": _next_id(_state["tasks"]),
        "text": text,
        "status": "todo",
        "parent_id": parent_id,
        "attempts": [],
        "convergence_stall_count": 0,
        "best_result": None,
        "created_at": time.time(),
    }
    _state["tasks"].append(task)
    _save()
    msg = f"added task #{task['id']}: {text}"
    if parent_id:
        msg += f" (subtask of #{parent_id})"
    if context is not None:
        context.append({"type": "tool_use", "tool": "addTask", "input": text, "output": msg})
    return task["id"]


def addSubtask(parent_id, text, context=None):
    """Create a child task under parent_id. Returns the new task id."""
    _load()
    # verify parent exists
    parent = _getTask(parent_id)
    if not parent:
        msg = f"parent task #{parent_id} not found"
        if context is not None:
            context.append({"type": "tool_use", "tool": "addSubtask",
                           "input": {"parent_id": parent_id, "text": text}, "output": msg})
        return None
    return addTask(text, context=context, parent_id=parent_id)


def listTasks(returnString=False, context=None, show_subtasks=True):
    _load()
    if not _state["tasks"]:
        output = "(no tasks)"
    else:
        lines = []
        # sort: top-level first, then subtasks indented under their parents
        top_level = [t for t in _state["tasks"] if t.get("parent_id") is None]
        top_level.sort(key=lambda t: t["id"])

        def _render(task, indent=0):
            marker = {"todo": "[ ]", "in_progress": "[~]", "done": "[x]",
                      "blocked": "[!]", "converged": "[c]"}.get(task["status"], "[ ]")
            prefix = "  " * indent
            stall = ""
            if task.get("convergence_stall_count", 0) >= 3:
                stall = f" (stalled x{task['convergence_stall_count']})"
            attempt_count = len(task.get("attempts", []))
            attempt_str = f" [{attempt_count} attempts]" if attempt_count > 0 else ""
            lines.append(f"{prefix}#{task['id']:>3}  {marker}  {task['text']}{stall}{attempt_str}")

        for t in top_level:
            _render(t)
            if show_subtasks:
                children = [c for c in _state["tasks"] if c.get("parent_id") == t["id"]]
                children.sort(key=lambda c: c["id"])
                for child in children:
                    _render(child, indent=1)
        output = "Tasks:\n" + "\n".join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "listTasks", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def markTaskDone(task_id, context=None):
    result = _setTaskStatus(task_id, "done", "markTaskDone", context)
    if result:
        _checkParentCompletion(task_id)
    return result


def markTaskInProgress(task_id, context=None):
    return _setTaskStatus(task_id, "in_progress", "markTaskInProgress", context)


def markTaskTodo(task_id, context=None):
    return _setTaskStatus(task_id, "todo", "markTaskTodo", context)


def markTaskBlocked(task_id, reason="", context=None):
    """Mark a task as blocked (e.g., waiting on another change or external factor)."""
    _load()
    task = _getTask(task_id)
    if task:
        task["status"] = "blocked"
        if reason:
            task.setdefault("block_reason", reason)
        _save()
        msg = f"task #{task_id} -> blocked"
        if reason:
            msg += f": {reason}"
    else:
        msg = f"task #{task_id} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "markTaskBlocked",
                       "input": {"id": task_id, "reason": reason}, "output": msg})
    return bool(task)


def markTaskConverged(task_id, context=None):
    """Mark as converged — further optimization yields diminishing returns."""
    return _setTaskStatus(task_id, "converged", "markTaskConverged", context)


def _setTaskStatus(task_id, status, tool_name, context):
    _load()
    task = _getTask(task_id)
    if task:
        task["status"] = status
        _save()
        msg = f"task #{task_id} -> {status}"
        if context is not None:
            context.append({"type": "tool_use", "tool": tool_name,
                           "input": {"id": task_id, "status": status}, "output": msg})
        return True
    msg = f"task #{task_id} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": tool_name,
                       "input": {"id": task_id, "status": status}, "output": msg})
    return False


def _checkParentCompletion(child_id):
    """If all siblings are done/converged, auto-complete the parent."""
    _load()
    child = _getTask(child_id)
    if not child or not child.get("parent_id"):
        return
    parent = _getTask(child["parent_id"])
    if not parent:
        return
    siblings = [t for t in _state["tasks"]
                if t.get("parent_id") == parent["id"] and t["id"] != child["id"]]
    all_done = all(s["status"] in ("done", "converged") for s in siblings)
    if all_done and parent["status"] not in ("done", "converged"):
        parent["status"] = "done"
        _save()


def removeTask(task_id, context=None):
    _load()
    before = len(_state["tasks"])
    _state["tasks"] = [t for t in _state["tasks"] if t["id"] != task_id]
    _save()
    success = len(_state["tasks"]) < before
    msg = f"removed task #{task_id}" if success else f"task #{task_id} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "removeTask", "input": task_id, "output": msg})
    return success


def clearDoneTasks(context=None):
    _load()
    removed = sum(1 for t in _state["tasks"] if t["status"] == "done")
    _state["tasks"] = [t for t in _state["tasks"] if t["status"] != "done"]
    _save()
    msg = f"cleared {removed} done task(s)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "clearDoneTasks", "input": None, "output": msg})
    return removed


def clearAllTasks(context=None):
    _load()
    count = len(_state["tasks"])
    _state["tasks"] = []
    _save()
    msg = f"cleared all {count} task(s)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "clearAllTasks", "input": None, "output": msg})


# --- Attempt tracking (for refinement loop) ---

def recordAttempt(task_id, approach, success, result_summary="", context=None):
    """Record an optimization attempt against a task. Feeds the refinement loop."""
    _load()
    task = _getTask(task_id)
    if not task:
        msg = f"task #{task_id} not found"
        if context is not None:
            context.append({"type": "tool_use", "tool": "recordAttempt",
                           "input": {"task_id": task_id, "approach": approach},
                           "output": msg})
        return False

    attempt = {
        "timestamp": time.time(),
        "approach": approach,
        "success": success,
        "result_summary": result_summary,
    }
    task.setdefault("attempts", []).append(attempt)
    task["convergence_stall_count"] = task.get("convergence_stall_count", 0)

    if success:
        task["convergence_stall_count"] = 0
        task["best_result"] = result_summary
    else:
        task["convergence_stall_count"] += 1

    # Auto-converge if too many failed attempts
    if task["convergence_stall_count"] >= CONVERGENCE_MAX_ATTEMPTS:
        task["status"] = "converged"

    _save()
    msg = (f"recorded {'successful' if success else 'failed'} attempt on task #{task_id}: "
           f"{approach[:80]} (stall={task['convergence_stall_count']})")
    if task["status"] == "converged":
        msg += " — task auto-converged"
    if context is not None:
        context.append({"type": "tool_use", "tool": "recordAttempt",
                       "input": {"task_id": task_id, "approach": approach, "success": success},
                       "output": msg})
    return True


def getTaskAttempts(task_id):
    """Return list of attempts for a task, most recent first."""
    _load()
    task = _getTask(task_id)
    if not task:
        return []
    return list(reversed(task.get("attempts", [])))


def getRecentFailures(limit=5):
    """Return recently failed attempts across all tasks for the refinement prompt."""
    _load()
    failures = []
    for t in _state["tasks"]:
        for a in t.get("attempts", []):
            if not a.get("success"):
                failures.append({
                    "task_id": t["id"],
                    "task_text": t["text"],
                    "approach": a["approach"],
                    "result": a.get("result_summary", ""),
                    "timestamp": a.get("timestamp", 0),
                })
    failures.sort(key=lambda x: x["timestamp"], reverse=True)
    return failures[:limit]


def getConvergenceReport():
    """Return a summary of which tasks have converged and why."""
    _load()
    converged = [t for t in _state["tasks"] if t["status"] == "converged"]
    stalled = [t for t in _state["tasks"]
               if t["status"] != "converged" and t.get("convergence_stall_count", 0) >= 3]
    lines = []
    if converged:
        lines.append("Converged (diminishing returns):")
        for t in converged:
            attempts = t.get("attempts", [])
            failed = sum(1 for a in attempts if not a["success"])
            lines.append(f"  #{t['id']} {t['text']} ({failed} failed attempts)")
    if stalled:
        lines.append("Stalling (may converge soon):")
        for t in stalled:
            lines.append(f"  #{t['id']} {t['text']} (stall={t['convergence_stall_count']})")
    return "\n".join(lines) if (converged or stalled) else "(no convergence data)"


def _getTask(task_id):
    for t in _state["tasks"]:
        if t["id"] == task_id:
            return t
    return None


# --- Notes ---

def addNote(text, context=None):
    _load()
    note = {"id": _next_id(_state["notes"]), "text": text}
    _state["notes"].append(note)
    _save()
    msg = f"added note #{note['id']}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "addNote", "input": text, "output": msg})
    return note["id"]


def listNotes(returnString=False, context=None):
    _load()
    if not _state["notes"]:
        output = "(no notes)"
    else:
        lines = [f"  #{n['id']:>3}  {n['text']}" for n in _state["notes"]]
        output = "Notes:\n" + "\n".join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "listNotes", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def removeNote(note_id, context=None):
    _load()
    before = len(_state["notes"])
    _state["notes"] = [n for n in _state["notes"] if n["id"] != note_id]
    _save()
    success = len(_state["notes"]) < before
    msg = f"removed note #{note_id}" if success else f"note #{note_id} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "removeNote", "input": note_id, "output": msg})
    return success


def clearNotes(context=None):
    _load()
    count = len(_state["notes"])
    _state["notes"] = []
    _save()
    msg = f"cleared {count} note(s)"
    if context is not None:
        context.append({"type": "tool_use", "tool": "clearNotes", "input": None, "output": msg})


# --- Refinement log ---

def logRefinementStep(func_name, depth, action, detail="", context=None):
    """Log a step in the recursive refinement process for debugging."""
    _load()
    entry = {
        "timestamp": time.time(),
        "func_name": func_name,
        "depth": depth,
        "action": action,
        "detail": detail,
    }
    _state.setdefault("refinement_log", []).append(entry)
    if len(_state["refinement_log"]) > 200:
        _state["refinement_log"] = _state["refinement_log"][-200:]
    _save()
    if context is not None:
        context.append({"type": "tool_use", "tool": "logRefinementStep",
                       "input": {"func_name": func_name, "depth": depth, "action": action},
                       "output": "logged"})


def getRefinementLog(tail=20):
    """Return the last N refinement log entries."""
    _load()
    entries = _state.get("refinement_log", [])[-tail:]
    if not entries:
        return "(no refinement log)"
    lines = []
    for e in entries:
        indent = "  " * min(e["depth"], 5)
        lines.append(f"{indent}[d={e['depth']}] {e['action']}: {e['func_name']} — {e['detail'][:100]}")
    return "\n".join(lines)


# --- Full board ---

def showBoard(returnString=False, context=None):
    tasks = listTasks(returnString=True)
    notes = listNotes(returnString=True)
    convergence = getConvergenceReport()
    output = tasks + "\n\n" + notes
    if convergence and convergence != "(no convergence data)":
        output += "\n\nConvergence:\n" + convergence
    if context is not None:
        context.append({"type": "tool_use", "tool": "showBoard", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def resetBoard(context=None):
    global _state
    _state = {"tasks": [], "notes": [], "refinement_log": []}
    _save()
    if context is not None:
        context.append({"type": "tool_use", "tool": "resetBoard", "input": None, "output": "board cleared"})
