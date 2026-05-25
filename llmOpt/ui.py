"""
ui.py -- Flask dashboard for the model optimization loop.

Runs in a background thread. The main loop calls ui.push_event() to stream
updates. Open http://localhost:5050 in a browser to watch.
"""

import json
import queue
import threading
import time
from flask import Flask, Response, jsonify, render_template_string, request

app = Flask(__name__)

_state = {
    "status": "idle",        # idle | running | waiting_model | done
    "iteration": 0,
    "token_count": 0,
    "last_response": "",
    "context": [],           # copy of CONTEXT list
    "board": "",             # planner.showBoard() text
    "events": [],            # SSE event log
    "pr_url": "",
    "diff": "",
    "nudge_pending": "",     # set by UI button, consumed by main loop
    "steer_pending": "",     # set by UI steer input, consumed by main loop
    "stream_buffer": "",     # accumulates tokens during generation
}
_lock = threading.Lock()
_subscribers: list = []
_subs_lock = threading.Lock()

_HTML = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>gengin optimizer</title>
<style>
  body { font-family: monospace; background:#0d1117; color:#c9d1d9; margin:0; }
  header { background:#161b22; padding:12px 20px; border-bottom:1px solid #30363d;
           display:flex; align-items:center; gap:16px; }
  h1 { margin:0; font-size:1.1rem; color:#58a6ff; }
  #status-badge { padding:2px 10px; border-radius:12px; font-size:.85rem; }
  .idle    { background:#30363d; }
  .running { background:#1a4a1a; color:#56d364; }
  .waiting_model { background:#3d2a00; color:#e3b341; }
  .done    { background:#0d3555; color:#58a6ff; }
  main { display:grid; grid-template-columns:1fr 1fr 1fr; gap:0; height:calc(100vh - 49px); }
  section { overflow-y:auto; padding:16px; border-right:1px solid #30363d; }
  section:last-child { border-right:none; }
  h2 { font-size:.85rem; color:#8b949e; text-transform:uppercase;
       letter-spacing:.06em; margin:0 0 8px; }
  pre { margin:0; white-space:pre-wrap; word-break:break-word; font-size:.8rem;
        line-height:1.5; }
  .entry { border:1px solid #21262d; border-radius:6px; padding:8px 10px;
           margin-bottom:8px; }
  .entry-err { border-color:#f85149; background:#2a0a0a; }
  .entry .tool { color:#58a6ff; font-weight:bold; }
  .entry-err .tool { color:#f85149; }
  .entry .meta { color:#8b949e; font-size:.75rem; }
  #event-log { font-size:.78rem; }
  .ev-ok    { color:#56d364; }
  .ev-err   { color:#f85149; }
  .ev-info  { color:#8b949e; }
  #pr-banner { display:none; background:#0d3555; color:#58a6ff;
               padding:10px 20px; border-bottom:1px solid #30363d; }
  .diff-add  { color:#56d364; }
  .diff-del  { color:#f85149; }
  .diff-hunk { color:#79c0ff; }
  .diff-file { color:#e3b341; font-weight:bold; }
</style>
</head>
<body>
<div id="pr-banner"></div>
<header>
  <h1>gengin optimizer</h1>
  <span id="status-badge" class="idle">idle</span>
  <span id="iter-badge" style="color:#8b949e;font-size:.85rem;"></span>
  <span id="token-badge" style="color:#8b949e;font-size:.85rem;margin-left:auto;"></span>
  <button id="nudge-btn" onclick="sendNudge()"
    style="margin-left:12px;padding:3px 12px;border-radius:8px;border:1px solid #f85149;
           background:#2a0a0a;color:#f85149;cursor:pointer;font-family:monospace;font-size:.82rem;">
    nudge (unstuck)
  </button>
</header>
<main>
  <section>
    <h2>Context entries</h2>
    <div id="context-list"></div>
  </section>
  <section style="display:grid;grid-template-rows:auto 1fr auto auto;gap:0;padding:0;">
    <div style="padding:16px;border-bottom:1px solid #30363d;">
      <h2>Last model response</h2>
      <pre id="last-response" style="max-height:260px;overflow-y:auto;"></pre>
    </div>
    <div style="padding:16px;border-bottom:1px solid #30363d;overflow-y:auto;">
      <h2>Planner board</h2>
      <pre id="board"></pre>
    </div>
    <div style="padding:16px;overflow-y:auto;max-height:180px;border-bottom:1px solid #30363d;">
      <h2>Event log</h2>
      <pre id="event-log"></pre>
    </div>
    <div style="padding:12px 16px;">
      <h2>Steer model</h2>
      <textarea id="steer-input" rows="2" placeholder="System instruction to prepend to next request..."
        style="width:100%;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;
               border-radius:4px;padding:6px;font-family:monospace;font-size:.78rem;
               box-sizing:border-box;resize:vertical;"></textarea>
      <button id="steer-btn" onclick="sendSteer()"
        style="margin-top:5px;padding:3px 12px;border-radius:8px;border:1px solid #58a6ff;
               background:#0d3555;color:#58a6ff;cursor:pointer;font-family:monospace;font-size:.82rem;">
        Steer
      </button>
    </div>
  </section>
  <section>
    <h2>Code changes (git diff)</h2>
    <pre id="diff-view" style="font-size:.75rem;line-height:1.45;"></pre>
  </section>
</main>
<script>
function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

function refresh() {
  fetch('/api/state').then(r=>r.json()).then(d => {
    const badge = document.getElementById('status-badge');
    badge.textContent = d.status;
    badge.className = d.status;

    document.getElementById('iter-badge').textContent = d.iteration ? `iteration ${d.iteration}` : '';
    document.getElementById('token-badge').textContent = d.token_count ? `~${d.token_count.toLocaleString()} tokens` : '';
    document.getElementById('board').innerHTML = esc(d.board);

    const renderDiff = text => {
      if (!text || text === '(no changes)') return '<span class="ev-info">(no changes)</span>';
      return text.split('\\n').map(line => {
        if (line.startsWith('+++') || line.startsWith('---')) return `<span class="diff-file">${esc(line)}</span>`;
        if (line.startsWith('+')) return `<span class="diff-add">${esc(line)}</span>`;
        if (line.startsWith('-')) return `<span class="diff-del">${esc(line)}</span>`;
        if (line.startsWith('@@')) return `<span class="diff-hunk">${esc(line)}</span>`;
        return esc(line);
      }).join('\\n');
    };
    document.getElementById('diff-view').innerHTML = renderDiff(d.diff);

    const cl = document.getElementById('context-list');
    cl.innerHTML = d.context.slice().reverse().map(e => {
      const _typeLabel = {'model_response':'model','planning':'planning','tool_use':'tool','tool_error':'error','context_summary':'summary','intervention':'nudge'};
      const tool = e.tool || _typeLabel[e.type] || e.type || '?';
      const raw = e.output !== undefined ? e.output : (e.error !== undefined ? '[ERROR] ' + e.error : '');
      const out = typeof raw === 'object' ? JSON.stringify(raw, null, 2) : String(raw || '');
      const isErr = e.error !== undefined && e.output === undefined;
      return `<div class="entry${isErr ? ' entry-err' : ''}"><div class="tool">${esc(tool)}</div>`+
             `<div class="meta">${esc(e.input !== undefined ? JSON.stringify(e.input) : '')}</div>`+
             `<pre>${esc(out.slice(0,1200))}${out.length>1200?'\\n[...]':''}</pre></div>`;
    }).join('');

    const el = document.getElementById('event-log');
    el.innerHTML = d.events.slice().reverse().slice(0,60).map(ev => {
      const cls = ev.startsWith('[ok]')?'ev-ok':ev.startsWith('[err]')?'ev-err':'ev-info';
      return `<span class="${cls}">${esc(ev)}</span>`;
    }).join('\\n');

    if (d.pr_url) {
      const b = document.getElementById('pr-banner');
      b.style.display = 'block';
      b.innerHTML = `PR created: <a href="${esc(d.pr_url)}" style="color:#58a6ff;">${esc(d.pr_url)}</a>`;
    }
  });
}

// poll every 1.5 s — simple, no SSE complexity needed
setInterval(refresh, 1500);
refresh();

function sendNudge() {
  const msg = prompt('Nudge message (leave blank for default):',
    'Write your notes NOW: call addNote() with what you have observed and why it is slow, then addTask() with your plan. Then implement the change with searchReplace() or applyChange(). Do not just read more code.');
  if (msg === null) return;
  fetch('/api/nudge', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({message: msg || ''})});
  document.getElementById('nudge-btn').textContent = 'nudge sent!';
  setTimeout(() => document.getElementById('nudge-btn').textContent = 'nudge (unstuck)', 2000);
}

function sendSteer() {
  const msg = document.getElementById('steer-input').value.trim();
  if (!msg) return;
  fetch('/api/steer', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({message: msg})});
  document.getElementById('steer-input').value = '';
  const btn = document.getElementById('steer-btn');
  btn.textContent = 'sent!';
  setTimeout(() => btn.textContent = 'Steer', 2000);
}

const _lr = document.getElementById('last-response');
const _es = new EventSource('/api/token-stream');
_es.onmessage = function(e) {
  try {
    const m = JSON.parse(e.data);
    if (m.t === 'i') { _lr.textContent = m.v; }
    else if (m.t === 'r') { _lr.textContent = ''; }
    else if (m.t === 'c') { _lr.textContent += m.v; _lr.parentElement.scrollTop = _lr.parentElement.scrollHeight; }
    else if (m.t === 'f') { _lr.textContent = m.v; }
  } catch(_) {}
};
</script>
</body>
</html>"""


@app.route("/")
def index():
    return render_template_string(_HTML)


@app.route("/api/state")
def api_state():
    with _lock:
        return jsonify(dict(_state))


@app.route("/api/token-stream")
def token_stream():
    q = queue.Queue()
    with _lock:
        init_text = _state["last_response"]
    q.put(json.dumps({"t": "i", "v": init_text}))
    with _subs_lock:
        _subscribers.append(q)

    def generate():
        try:
            while True:
                try:
                    item = q.get(timeout=15)
                    yield f"data: {item}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        finally:
            with _subs_lock:
                if q in _subscribers:
                    _subscribers.remove(q)

    return Response(
        generate(),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/api/steer", methods=["POST"])
def api_steer():
    data = request.get_json(silent=True) or {}
    msg = (data.get("message") or "").strip()
    if msg:
        with _lock:
            _state["steer_pending"] = msg
        _log(f"[info] steer queued by user")
    return jsonify({"ok": True})


@app.route("/api/nudge", methods=["POST"])
def api_nudge():
    data = request.get_json(silent=True) or {}
    msg = (data.get("message") or "").strip()
    if not msg:
        msg = ("Write your notes NOW before doing anything else: "
               "call addNote() with what you have observed and why the hotspot is slow, "
               "then addTask() for each change you plan. "
               "Notes survive context compression — anything not written to the board will be forgotten. "
               "After noting, implement the top task with searchReplace() or applyChange().")
    with _lock:
        _state["nudge_pending"] = msg
    _log(f"[info] nudge queued by user")
    return jsonify({"ok": True})


# --- Public API called by main loop ---

def set_status(status):
    with _lock:
        _state["status"] = status
    _log(f"[info] status -> {status}")


def set_iteration(n):
    with _lock:
        _state["iteration"] = n


def set_token_count(n):
    with _lock:
        _state["token_count"] = n


def _broadcast(payload: dict):
    msg = json.dumps(payload)
    with _subs_lock:
        for q in _subscribers[:]:
            try:
                q.put_nowait(msg)
            except Exception:
                pass


def set_last_response(text):
    with _lock:
        _state["last_response"] = text
    _broadcast({"t": "f", "v": text})


def sync_context(context_list):
    """Call after every executor run to mirror CONTEXT into the UI state."""
    with _lock:
        _state["context"] = list(context_list)


def sync_board(board_text):
    with _lock:
        _state["board"] = board_text


def set_pr_url(url):
    with _lock:
        _state["pr_url"] = url
        _state["status"] = "done"
    _log(f"[ok] PR created: {url}")


def log_tool_result(tool, error):
    if error:
        _log(f"[err] {tool}: {error}")
    else:
        _log(f"[ok] {tool}")


def _log(msg):
    ts = time.strftime("%H:%M:%S")
    with _lock:
        _state["events"].append(f"{ts}  {msg}")
        if len(_state["events"]) > 200:
            _state["events"] = _state["events"][-200:]


def sync_diff(text):
    with _lock:
        _state["diff"] = text or ""


def pop_nudge():
    """Return and clear any pending nudge message. Returns empty string if none."""
    with _lock:
        msg = _state["nudge_pending"]
        _state["nudge_pending"] = ""
    return msg


def pop_steer() -> str:
    """Return and clear any pending steer message."""
    with _lock:
        msg = _state["steer_pending"]
        _state["steer_pending"] = ""
    return msg


def push_token(delta: str):
    with _lock:
        _state["stream_buffer"] += delta
    _broadcast({"t": "c", "v": delta})


def clear_stream():
    with _lock:
        _state["stream_buffer"] = ""
    _broadcast({"t": "r"})


def start(port=5051):
    """Start Flask in a daemon thread. Call once before the main loop."""
    t = threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=port, debug=False, use_reloader=False),
        daemon=True,
    )
    t.start()
    print(f"[ui] dashboard at http://localhost:{port}")
