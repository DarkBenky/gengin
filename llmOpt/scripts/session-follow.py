#!/usr/bin/env python3
"""Live view of the newest supervised session.

`hermes -z` prints only the final answer, so Hermes sessions are followed
through the session's state.db (turns, tool calls and results land there as
they happen).  dsh prints a running transcript, which the supervisor tees into
logs/sessions/<id>.log, so dsh sessions are followed through that log.
The console pane already shows the merged supervisor stream; this pane is the
agent-only transcript.

    llmOpt/scripts/session-follow.py [checkout] [--interval SECONDS] [--ticks N]

Read-only and stdlib only; SQLite WAL allows this to run next to a live
session.  Ctrl-C stops it.
"""

import argparse
import glob
import json
import os
import sqlite3
import sys
import time

IDLE_RESET_SECONDS = 600   # an older session is a finished one: wait for a new one
HEARTBEAT_SECONDS = 30
CONTEXT_ROWS = 8           # rows of history printed when attaching mid-session
PREVIEW = 200
DSH_TAIL_LINES = 25        # log lines shown when attaching to a dsh session


def default_checkout():
    here = os.path.abspath(__file__)
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def session_id(path):
    return path.split(os.sep + "run" + os.sep)[1].split(os.sep)[0]


def newest_session(checkout):
    """(kind, sid, path) for the most recently active supervised session."""
    run_glob = os.path.join(checkout, "llmOpt", "run", "*")
    candidates = []
    for path in glob.glob(os.path.join(run_glob, "hermes", "state.db")):
        candidates.append((os.path.getmtime(path), "hermes", path))
    for path in glob.glob(os.path.join(run_glob, "dsh", "sessions", "**", "session*.zstd"),
                          recursive=True):
        candidates.append((os.path.getmtime(path), "dsh", path))
    # A just-prepared dsh session may not have written its session store yet.
    for path in glob.glob(os.path.join(run_glob, "dsh")):
        candidates.append((os.path.getmtime(path), "dsh", path))
    if not candidates:
        return None
    _mtime, kind, path = max(candidates)
    return kind, session_id(path), path


def session_finished(checkout, sid):
    """The supervisor writes logs/sessions/<id>.json when a session ends."""
    return os.path.exists(os.path.join(checkout, "llmOpt", "logs", "sessions", sid + ".json"))


def preview(text, limit=PREVIEW):
    if not text:
        return ""
    return " ".join(str(text).split())[:limit]


def tool_calls(raw):
    """(name, arguments) for every call in a stored tool_calls column."""
    if not raw:
        return []
    try:
        items = json.loads(raw if raw.lstrip().startswith("[") else "[" + raw + "]")
    except ValueError:
        return []
    out = []
    for item in items:
        if not isinstance(item, dict):
            continue
        fn = item.get("function") if isinstance(item.get("function"), dict) else {}
        out.append((fn.get("name") or item.get("name") or "?", fn.get("arguments") or ""))
    return out


def last_activity(con):
    row = con.execute("select max(timestamp) from messages").fetchone()
    return float(row[0]) if row and row[0] else 0.0


def usage_line(con):
    row = con.execute(
        "select coalesce(sum(api_call_count), 0), coalesce(sum(input_tokens), 0),"
        " coalesce(sum(output_tokens), 0) from session_model_usage").fetchone()
    model = con.execute("select model from session_model_usage limit 1").fetchone()
    calls, tokens_in, tokens_out = (int(value or 0) for value in row)
    return "%d api calls, %d in / %d out tokens, model %s" % (
        calls, tokens_in, tokens_out, model[0] if model and model[0] else "?")


def print_new(con, last_id):
    rows = con.execute(
        "select id, role, tool_name, content, tool_calls, timestamp from messages"
        " where id > ? order by id", (last_id,)).fetchall()
    for row_id, role, tool_name, content, calls, stamp in rows:
        when = time.strftime("%H:%M:%S", time.localtime(float(stamp))) if stamp else "--:--:--"
        if role == "assistant":
            if content and content.strip():
                print("[%s] agent: %s" % (when, preview(content)), flush=True)
            for name, args in tool_calls(calls):
                print("[%s]   -> %s %s" % (when, name, preview(args, 120)), flush=True)
        elif role == "tool":
            print("[%s]   <- %s: %s" % (when, tool_name or "?", preview(content, 140)), flush=True)
        elif role == "user":
            print("[%s] (prompt sent)" % when, flush=True)
    return rows[-1][0] if rows else last_id


def read_log_tail(path, lines=DSH_TAIL_LINES):
    """(tail_text, size) of the session log, best effort."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return "", 0
    text = data.decode("utf-8", "replace")
    return "\n".join(text.splitlines()[-lines:]), len(data)


def drain_log(state):
    """Print new complete lines from the followed session log.

    state is {"path", "pos", "buf"}; partial lines stay buffered.  Returns
    True when new output arrived.
    """
    try:
        size = os.path.getsize(state["path"])
    except OSError:
        return False
    if size <= state["pos"]:
        return False
    try:
        with open(state["path"], "rb") as fh:
            fh.seek(state["pos"])
            chunk = fh.read()
            state["pos"] = fh.tell()
    except OSError:
        return False
    state["buf"] += chunk.decode("utf-8", "replace")
    while "\n" in state["buf"]:
        line, state["buf"] = state["buf"].split("\n", 1)
        print(line, flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("checkout", nargs="?", default=default_checkout())
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--ticks", type=int, default=0, help="stop after N polls (0 = forever)")
    parser.add_argument("--idle-reset", type=float, default=IDLE_RESET_SECONDS,
                        help="treat a session quiet for this many seconds as finished")
    args = parser.parse_args()

    print("[follow] watching %s/llmOpt/run/ (hermes state.db / dsh session log)"
          % args.checkout, flush=True)
    current = None
    last_id = 0                # hermes: highest message id printed
    log_state = None           # dsh: {"path", "pos", "buf"} of the session log
    last_event = time.time()
    heartbeat = time.time() - HEARTBEAT_SECONDS   # report state on the first poll
    finished = set()
    ticks = 0

    while True:
        ticks += 1
        newest = newest_session(args.checkout)
        if newest is None:
            if time.time() - heartbeat >= HEARTBEAT_SECONDS:
                print("[follow] waiting for the first session ...", flush=True)
                heartbeat = time.time()
        else:
            kind, sid, path = newest
            fresh = (kind, sid) != current
            if fresh:
                current = (kind, sid)
                last_id = 0
                log_state = None
                if sid not in finished:
                    print("[follow] session %s (%s)" % (sid, kind), flush=True)

            if kind == "hermes":
                try:
                    con = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
                    if fresh:
                        newest_row = con.execute("select max(id) from messages").fetchone()
                        last_id = max(0, int(newest_row[0] or 0) - CONTEXT_ROWS)
                    activity = last_activity(con)
                    if sid in finished:
                        pass
                    elif session_finished(args.checkout, sid) or (
                            last_id and time.time() - last_event > args.idle_reset):
                        finished.add(sid)
                        print("[follow] session %s finished; waiting for the next one" % sid,
                              flush=True)
                        heartbeat = time.time()
                    elif last_id == 0 and activity and time.time() - activity > args.idle_reset:
                        if time.time() - heartbeat >= HEARTBEAT_SECONDS:
                            print("[follow] last session ended %d min ago, waiting for a new one"
                                  % ((time.time() - activity) / 60), flush=True)
                            heartbeat = time.time()
                    else:
                        new_id = print_new(con, last_id)
                        if new_id != last_id:
                            last_id, last_event, heartbeat = new_id, time.time(), time.time()
                        elif time.time() - heartbeat >= HEARTBEAT_SECONDS:
                            print("[follow]   ... %s | last event %d s ago"
                                  % (usage_line(con), time.time() - last_event), flush=True)
                            heartbeat = time.time()
                except sqlite3.Error as exc:
                    if time.time() - heartbeat >= HEARTBEAT_SECONDS:
                        print("[follow] state.db not readable yet: %s" % exc, flush=True)
                        heartbeat = time.time()
            else:
                # dsh: follow the supervisor's tee of the agent's stdout/stderr.
                if log_state is None:
                    log_path = os.path.join(args.checkout, "llmOpt", "logs", "sessions",
                                            sid + ".log")
                    tail, size = read_log_tail(log_path)
                    log_state = {"path": log_path, "pos": size, "buf": ""}
                    if tail:
                        print("[follow] --- last output ---", flush=True)
                        print(tail, flush=True)
                if sid in finished:
                    pass
                elif session_finished(args.checkout, sid) or (
                        log_state["pos"] and time.time() - last_event > args.idle_reset):
                    drain_log(log_state)
                    if log_state["buf"]:
                        print(log_state["buf"], flush=True)
                        log_state["buf"] = ""
                    finished.add(sid)
                    print("[follow] session %s finished; waiting for the next one" % sid,
                          flush=True)
                    heartbeat = time.time()
                elif drain_log(log_state):
                    last_event = heartbeat = time.time()
                elif time.time() - heartbeat >= HEARTBEAT_SECONDS:
                    print("[follow]   ... session log %d bytes, last output %d s ago"
                          % (log_state["pos"], time.time() - last_event), flush=True)
                    heartbeat = time.time()

        if args.ticks and ticks >= args.ticks:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
