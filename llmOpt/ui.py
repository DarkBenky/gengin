"""
ui.py -- Flask dashboard for the model optimization loop.

Runs in a background thread. The main loop calls ui.push_event() to stream
updates. Open http://localhost:5050 in a browser to watch.
"""

import threading
import time
from flask import Flask, Response, jsonify, render_template_string

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
}
_lock = threading.Lock()

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
  main { display:grid; grid-template-columns:1fr 1fr; gap:0; height:calc(100vh - 49px); }
  section { overflow-y:auto; padding:16px; border-right:1px solid #30363d; }
  section:last-child { border-right:none; }
  h2 { font-size:.85rem; color:#8b949e; text-transform:uppercase;
       letter-spacing:.06em; margin:0 0 8px; }
  pre { margin:0; white-space:pre-wrap; word-break:break-word; font-size:.8rem;
        line-height:1.5; }
  .entry { border:1px solid #21262d; border-radius:6px; padding:8px 10px;
           margin-bottom:8px; }
  .entry .tool { color:#58a6ff; font-weight:bold; }
  .entry .meta { color:#8b949e; font-size:.75rem; }
  #event-log { font-size:.78rem; }
  .ev-ok    { color:#56d364; }
  .ev-err   { color:#f85149; }
  .ev-info  { color:#8b949e; }
  #pr-banner { display:none; background:#0d3555; color:#58a6ff;
               padding:10px 20px; border-bottom:1px solid #30363d; }
</style>
</head>
<body>
<div id="pr-banner"></div>
<header>
  <h1>gengin optimizer</h1>
  <span id="status-badge" class="idle">idle</span>
  <span id="iter-badge" style="color:#8b949e;font-size:.85rem;"></span>
  <span id="token-badge" style="color:#8b949e;font-size:.85rem;margin-left:auto;"></span>
</header>
<main>
  <section>
    <h2>Context entries</h2>
    <div id="context-list"></div>
  </section>
  <section style="display:grid;grid-template-rows:auto 1fr auto;gap:0;padding:0;">
    <div style="padding:16px;border-bottom:1px solid #30363d;">
      <h2>Last model response</h2>
      <pre id="last-response" style="max-height:260px;overflow-y:auto;"></pre>
    </div>
    <div style="padding:16px;border-bottom:1px solid #30363d;overflow-y:auto;">
      <h2>Planner board</h2>
      <pre id="board"></pre>
    </div>
    <div style="padding:16px;overflow-y:auto;max-height:220px;">
      <h2>Event log</h2>
      <pre id="event-log"></pre>
    </div>
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
    document.getElementById('last-response').innerHTML = esc(d.last_response);
    document.getElementById('board').innerHTML = esc(d.board);

    const cl = document.getElementById('context-list');
    cl.innerHTML = d.context.slice().reverse().map(e => {
      const tool = e.tool || '?';
      const out = typeof e.output === 'object' ? JSON.stringify(e.output, null, 2) : String(e.output || '');
      return `<div class="entry"><div class="tool">${esc(tool)}</div>`+
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


def set_last_response(text):
    with _lock:
        _state["last_response"] = text


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


def start(port=5050):
    """Start Flask in a daemon thread. Call once before the main loop."""
    t = threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=port, debug=False, use_reloader=False),
        daemon=True,
    )
    t.start()
    print(f"[ui] dashboard at http://localhost:{port}")
