"""
planner.py -- persistent task and note board for the model's agentic loop.

State is kept in a JSON file (default: planner_state.json next to this module)
so it survives across multiple script invocations.

Task statuses: "todo" | "in_progress" | "done"
"""

import json
import os

_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "planner_state.json")

_state = None


def _load():
    global _state
    if _state is not None:
        return
    if os.path.exists(_STATE_FILE):
        with open(_STATE_FILE) as fh:
            _state = json.load(fh)
    else:
        _state = {"tasks": [], "notes": []}


def _save():
    with open(_STATE_FILE, "w") as fh:
        json.dump(_state, fh, indent=2)


# --- Tasks ---

def addTask(text, context=None):
    _load()
    task = {"id": len(_state["tasks"]) + 1, "text": text, "status": "todo"}
    _state["tasks"].append(task)
    _save()
    msg = f"added task #{task['id']}: {text}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "addTask", "input": text, "output": msg})
    return task["id"]


def listTasks(returnString=False, context=None):
    _load()
    if not _state["tasks"]:
        output = "(no tasks)"
    else:
        lines = []
        for t in _state["tasks"]:
            marker = {"todo": "[ ]", "in_progress": "[~]", "done": "[x]"}.get(t["status"], "[ ]")
            lines.append(f"  #{t['id']:>3}  {marker}  {t['text']}")
        output = "Tasks:\n" + "\n".join(lines)
    if context is not None:
        context.append({"type": "tool_use", "tool": "listTasks", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def markTaskDone(task_id, context=None):
    return _setTaskStatus(task_id, "done", context)


def markTaskInProgress(task_id, context=None):
    return _setTaskStatus(task_id, "in_progress", context)


def markTaskTodo(task_id, context=None):
    return _setTaskStatus(task_id, "todo", context)


def _setTaskStatus(task_id, status, context):
    _load()
    for t in _state["tasks"]:
        if t["id"] == task_id:
            t["status"] = status
            _save()
            msg = f"task #{task_id} -> {status}"
            if context is not None:
                context.append({"type": "tool_use", "tool": "_setTaskStatus", "input": {"id": task_id, "status": status}, "output": msg})
            return True
    msg = f"task #{task_id} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "_setTaskStatus", "input": {"id": task_id, "status": status}, "output": msg})
    return False


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


# --- Notes ---

def addNote(text, context=None):
    _load()
    note = {"id": len(_state["notes"]) + 1, "text": text}
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


# --- Full board ---

def showBoard(returnString=False, context=None):
    tasks = listTasks(returnString=True)
    notes = listNotes(returnString=True)
    output = tasks + "\n\n" + notes
    if context is not None:
        context.append({"type": "tool_use", "tool": "showBoard", "input": None, "output": output})
    if returnString:
        return output
    print(output)


def resetBoard(context=None):
    global _state
    _state = {"tasks": [], "notes": []}
    _save()
    if context is not None:
        context.append({"type": "tool_use", "tool": "resetBoard", "input": None, "output": "board cleared"})
