#!/usr/bin/env python3
"""Live view of the newest supervised session.

`hermes -z` prints only the final answer, so the console pane sits silent for
hours while the agent works.  Everything the agent does (turns, tool calls,
tool results) lands in the session's state.db as it happens, so this tails that
database instead.

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

IDLE_RESET_SECONDS = 600   # an older DB is a finished session: wait for a new one
HEARTBEAT_SECONDS = 30
PREVIEW = 200


def default_checkout():
    here = os.path.abspath(__file__)
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def newest_db(checkout):
    paths = glob.glob(os.path.join(checkout, "llmOpt", "run", "*", "hermes", "state.db"))
    if not paths:
        return None
    return max(paths, key=os.path.getmtime)


def session_id(path):
    return path.split(os.sep + "run" + os.sep)[1].split(os.sep)[0]


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("checkout", nargs="?", default=default_checkout())
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--ticks", type=int, default=0, help="stop after N polls (0 = forever)")
    args = parser.parse_args()

    print("[follow] watching %s/llmOpt/run/*/hermes/state.db" % args.checkout, flush=True)
    current = None
    last_id = 0
    last_event = time.time()
    heartbeat = time.time() - HEARTBEAT_SECONDS   # report state on the first poll
    ticks = 0

    while True:
        ticks += 1
        path = newest_db(args.checkout)
        if path is None:
            if time.time() - heartbeat >= HEARTBEAT_SECONDS:
                print("[follow] waiting for the first session ...", flush=True)
                heartbeat = time.time()
        else:
            if path != current:
                current, last_id = path, 0
                print("[follow] session %s" % session_id(path), flush=True)
            try:
                con = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
                activity = last_activity(con)
                if last_id == 0 and activity and time.time() - activity > IDLE_RESET_SECONDS:
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
        if args.ticks and ticks >= args.ticks:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
