import socket
import struct
import threading
import json
import numpy as np
import plotly.graph_objects as go
from http.server import HTTPServer, BaseHTTPRequestHandler

HOST = "127.0.0.1"
TCP_PORT = 5174
HTTP_PORT = 8081

_lock = threading.Lock()
_epoch_losses = []
_epoch_quantiles = []  # per epoch: [p10, p25, p50, p75, p90]
_backprop_losses = []
_max_angles = []  # per epoch: current MaxDivergenceDegrees
_latest = None

_HTML = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Training Monitor</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: #0b0b12;
  color: #b0b0c8;
  font-family: 'Segoe UI', system-ui, sans-serif;
  padding: 14px 16px;
  min-height: 100vh;
}

/* ---- header ---- */
.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 14px;
  padding-bottom: 10px;
  border-bottom: 1px solid #181824;
}
.header-left { display: flex; align-items: center; gap: 12px; }
.header h1 {
  font-size: 0.82rem;
  font-weight: 600;
  letter-spacing: 0.14em;
  text-transform: uppercase;
  color: #6670cc;
}
.pulse {
  width: 8px; height: 8px;
  border-radius: 50%;
  background: #2a2a3a;
  flex-shrink: 0;
  transition: background 0.4s, box-shadow 0.4s;
}
.pulse.live { background: #00e676; box-shadow: 0 0 0 3px rgba(0,230,118,0.18); animation: blink 2s ease-in-out infinite; }
@keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.5} }
.header-time { font-size: 0.72rem; color: #3a3a55; font-variant-numeric: tabular-nums; }

/* ---- stat cards ---- */
.stats {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 8px;
  margin-bottom: 12px;
}
.stat {
  background: #0f0f1c;
  border: 1px solid #181828;
  border-radius: 7px;
  padding: 10px 14px;
}
.stat .lbl {
  font-size: 0.6rem;
  text-transform: uppercase;
  letter-spacing: 0.12em;
  color: #383858;
  margin-bottom: 5px;
}
.stat .val {
  font-size: 1.25rem;
  font-weight: 600;
  color: #9090b8;
  font-variant-numeric: tabular-nums;
  letter-spacing: -0.01em;
}
.stat .val.hi { color: #6670cc; }
.stat .val.lo { color: #00c864; }
.stat .val.bp { color: #e06060; }

/* ---- chart card ---- */
.card {
  background: #0f0f1c;
  border: 1px solid #181828;
  border-radius: 7px;
  overflow: hidden;
}

/* ---- layout grids ---- */
.g2   { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 8px; }
.g3   { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 8px; }
.gmain {
  display: grid;
  grid-template-columns: 1fr 400px;
  gap: 8px;
  margin-bottom: 8px;
}
.gmain-left { display: flex; flex-direction: column; gap: 8px; }
</style>
</head>
<body>

<div class="header">
  <div class="header-left">
    <div class="pulse" id="pulse"></div>
    <h1>Training Monitor</h1>
  </div>
  <span class="header-time" id="htime">—</span>
</div>

<div class="stats">
  <div class="stat"><div class="lbl">Epoch</div><div class="val hi" id="s-epoch">—</div></div>
  <div class="stat"><div class="lbl">Avg Loss</div><div class="val" id="s-avg">—</div></div>
  <div class="stat"><div class="lbl">Min Loss</div><div class="val lo" id="s-min">—</div></div>
  <div class="stat"><div class="lbl">Max Loss</div><div class="val" id="s-max">—</div></div>
  <div class="stat"><div class="lbl">Backprop Loss</div><div class="val bp" id="s-bp">—</div></div>
  <div class="stat"><div class="lbl">Max Angle</div><div class="val" id="s-angle">—</div></div>
</div>

<div class="g2">
  <div class="card"><div id="c-epoch"     style="height:190px"></div></div>
  <div class="card"><div id="c-epoch-fan" style="height:190px"></div></div>
</div>

<div class="card" style="margin-bottom:8px">
  <div id="c-backprop" style="height:140px"></div>
</div>

<div class="card" style="margin-bottom:8px">
  <div id="c-angle" style="height:120px"></div>
</div>

<div class="gmain">
  <div class="gmain-left">
    <div class="g3" style="margin-bottom:0">
      <div class="card"><div id="c-total"    style="height:175px"></div></div>
      <div class="card"><div id="c-distance" style="height:175px"></div></div>
      <div class="card"><div id="c-control"  style="height:175px"></div></div>
    </div>
    <div class="g3" style="margin-bottom:0">
      <div class="card"><div id="c-speed"    style="height:175px"></div></div>
      <div class="card"><div id="c-altitude" style="height:175px"></div></div>
      <div class="card"><div id="c-hist"     style="height:175px"></div></div>
    </div>
  </div>
  <div class="card">
    <div id="c-path" style="height:366px"></div>
  </div>
</div>

<div class="g3" style="margin-bottom:0">
  <div class="card"><div id="c-path-top"   style="height:280px"></div></div>
  <div class="card"><div id="c-path-side"  style="height:280px"></div></div>
  <div class="card"><div id="c-path-front" style="height:280px"></div></div>
</div>

<div class="g3" style="margin-top:8px;margin-bottom:0">
  <div class="card"><div id="c-aileron"   style="height:175px"></div></div>
  <div class="card"><div id="c-elevator"  style="height:175px"></div></div>
  <div class="card"><div id="c-rudder"    style="height:175px"></div></div>
</div>

<script>
const BG  = {paper_bgcolor:'#0f0f1c', plot_bgcolor:'#0b0b16', font:{color:'#606080', size:10}};
const AX  = {gridcolor:'#14142a', zerolinecolor:'#1c1c30', tickfont:{size:9, color:'#505070'}};
const MAR = {t:26, b:24, l:48, r:8};
const SMMAR = {t:26, b:24, l:44, r:6};
let ready = {};

function react(id, traces, layout) {
  const el = document.getElementById(id);
  if (!ready[id]) {
    Plotly.newPlot(id, traces, layout, {responsive:true, displayModeBar:false});
    ready[id] = true;
  } else {
    const fl = el && el._fullLayout;
    if (fl) {
      if (fl.scene) {
        layout = {...layout, scene: {...(layout.scene||{}), camera: fl.scene.camera}};
      }
      for (const ax of ['xaxis','yaxis','xaxis2','yaxis2']) {
        if (fl[ax] && fl[ax].autorange === false)
          layout = {...layout, [ax]: {...(layout[ax]||{}), range: fl[ax].range, autorange: false}};
      }
    }
    Plotly.react(id, traces, layout);
  }
}

function bandTraces(d, x) {
  const c = 'rgba(99,110,250,';
  const t = [
    {x, y:d.lo, type:'scatter', mode:'lines', line:{width:0}, showlegend:false},
    {x, y:d.hi, type:'scatter', mode:'lines', line:{width:0}, fill:'tonexty',
     fillcolor:c+'0.12)', name:'Range', showlegend:false},
  ];
  (d.samples||[]).forEach(s =>
    t.push({x, y:s, type:'scatter', mode:'lines', line:{width:1, color:c+'0.13)'}, showlegend:false})
  );
  t.push({x, y:d.avg, type:'scatter', mode:'lines', line:{width:2, color:c+'0.9)'}, name:'Avg', showlegend:false});
  return t;
}

function titleLayout(text) {
  return {text, font:{color:'#6060a0', size:11}, x:0.01, xanchor:'left'};
}

function set(id, v) { const e=document.getElementById(id); if(e) e.textContent=v; }

async function update() {
  let r; try { r = await fetch('/data'); } catch(e){ return; }
  if (!r.ok) return;
  const d = await r.json();

  if (d.epoch_losses && d.epoch_losses.length) {
    const el = d.epoch_losses;
    const x  = el.map((_,i) => i);
    const lo = el.map(v => v[0]), avg = el.map(v => v[1]), hi = el.map(v => v[2]);
    const last = el.length - 1;

    document.getElementById('pulse').className = 'pulse live';
    set('htime', `updated ${new Date().toLocaleTimeString()}`);
    set('s-epoch', el.length);
    set('s-avg',   avg[last].toFixed(4));
    set('s-min',   lo[last].toFixed(4));
    set('s-max',   hi[last].toFixed(4));

    react('c-epoch',
      [{x, y:lo, type:'scatter', mode:'lines', line:{width:0}, showlegend:false},
       {x, y:hi, type:'scatter', mode:'lines', line:{width:0}, fill:'tonexty',
        fillcolor:'rgba(99,110,250,0.14)', name:'Min-Max', showlegend:false},
       {x, y:avg, type:'scatter', mode:'lines', line:{width:2, color:'rgb(99,110,250)'}, name:'Avg', showlegend:false}],
      {...BG, margin:MAR,
       xaxis:{...AX, title:{text:'Epoch', font:{size:9}}},
       yaxis:{...AX, title:{text:'Loss', font:{size:9}}},
       title:titleLayout('Epoch Loss')});
  }

  if (d.backprop_losses && d.backprop_losses.length) {
    const bl = d.backprop_losses;
    const x  = bl.map((_,i) => i);
    set('s-bp', bl[bl.length-1].toFixed(4));
    react('c-backprop',
      [{x, y:bl, type:'scatter', mode:'lines', line:{width:2, color:'rgba(220,80,80,0.85)'}, name:'Backprop', showlegend:false}],
      {...BG, margin:{t:22, b:22, l:52, r:8},
       xaxis:{...AX, title:{text:'Epoch', font:{size:9}}},
       yaxis:{...AX, title:{text:'Step Delta Dist', font:{size:9}}},
       title:titleLayout('Backprop Loss')});
  }

  if (d.max_angles && d.max_angles.length) {
    const ma = d.max_angles;
    const x  = ma.map((_,i) => i);
    set('s-angle', ma[ma.length-1].toFixed(1) + '°');
    react('c-angle',
      [{x, y:ma, type:'scatter', mode:'lines', line:{width:2, color:'rgba(80,200,180,0.85)'}, showlegend:false}],
      {...BG, margin:{t:22, b:22, l:44, r:8},
       xaxis:{...AX, title:{text:'Epoch', font:{size:9}}},
       yaxis:{...AX, title:{text:'Degrees', font:{size:9}}, range:[0,270]},
       title:titleLayout('Spawn Angle (curriculum)')});
  }

  if (d.epoch_quantiles && d.epoch_quantiles.length) {
    const eq = d.epoch_quantiles;
    const x  = eq.map((_,i) => i);
    const p10=eq.map(v=>v[0]), p25=eq.map(v=>v[1]), p50=eq.map(v=>v[2]),
          p75=eq.map(v=>v[3]), p90=eq.map(v=>v[4]);
    const c = 'rgba(99,110,250,';
    react('c-epoch-fan', [
      {x, y:p10, type:'scatter', mode:'lines', line:{width:0}, showlegend:false},
      {x, y:p90, type:'scatter', mode:'lines', line:{width:0}, fill:'tonexty', fillcolor:c+'0.08)', name:'p10-p90'},
      {x, y:p25, type:'scatter', mode:'lines', line:{width:0}, showlegend:false},
      {x, y:p75, type:'scatter', mode:'lines', line:{width:0}, fill:'tonexty', fillcolor:c+'0.20)', name:'p25-p75'},
      {x, y:p50, type:'scatter', mode:'lines', line:{width:2, color:c+'0.9)'}, name:'Median'},
    ], {...BG, margin:MAR,
       xaxis:{...AX, title:{text:'Epoch', font:{size:9}}},
       yaxis:{...AX, title:{text:'Loss', font:{size:9}}},
       legend:{bgcolor:'rgba(0,0,0,0)', font:{size:9, color:'#505070'}, x:1, xanchor:'right', y:1},
       title:titleLayout('Loss Quantiles')});
  }

  if (d.latest) {
    const lt = d.latest, x = lt.step_x;

    [['c-total','total','Total Loss'],
     ['c-distance','distance','Distance'],
     ['c-control','control','Control Effort']].forEach(([id, key, title]) => {
      react(id, bandTraces(lt[key], x),
        {...BG, margin:SMMAR, showlegend:false,
         xaxis:{...AX, title:{text:'Step', font:{size:9}}}, yaxis:{...AX},
         title:titleLayout(title)});
    });

    if (lt.speed)
      react('c-speed', bandTraces(lt.speed, lt.speed_x),
        {...BG, margin:SMMAR, showlegend:false,
         xaxis:{...AX, title:{text:'Step', font:{size:9}}},
         yaxis:{...AX, title:{text:'m/s', font:{size:9}}},
         title:titleLayout('Speed (m/s)')});

    if (lt.altitude)
      react('c-altitude', bandTraces(lt.altitude, x),
        {...BG, margin:SMMAR, showlegend:false,
         xaxis:{...AX, title:{text:'Step', font:{size:9}}},
         yaxis:{...AX, title:{text:'Altitude (Y)', font:{size:9}}},
         title:titleLayout('Altitude')});

    if (lt.loss_hist) {
      const h = lt.loss_hist;
      react('c-hist',
        [{x:h.x, y:h.y, type:'bar', marker:{color:'rgba(99,110,250,0.6)'}, showlegend:false}],
        {...BG, margin:SMMAR, bargap:0.05, showlegend:false,
         xaxis:{...AX, title:{text:'Final Loss', font:{size:9}}},
         yaxis:{...AX, title:{text:'Count', font:{size:9}}},
         title:titleLayout('Loss Histogram')});
    }

    // 3D path
    const pt = [];
    (lt.sampled_paths||[]).forEach(p =>
      pt.push({x:p.map(v=>v[0]), y:p.map(v=>v[2]), z:p.map(v=>v[1]),
               type:'scatter3d', mode:'lines',
               line:{width:1, color:'rgba(99,110,250,0.10)'}, showlegend:false}));
    if (lt.mean_path) {
      const mp = lt.mean_path;
      pt.push({x:mp.map(v=>v[0]), y:mp.map(v=>v[2]), z:mp.map(v=>v[1]),
               type:'scatter3d', mode:'lines',
               line:{width:4, color:'rgba(99,110,250,1)'}, name:'Mean'});
    }
    if (lt.start)
      pt.push({x:[lt.start[0]], y:[lt.start[2]], z:[lt.start[1]],
               type:'scatter3d', mode:'markers',
               marker:{size:6, color:'#00e676'}, name:'Start'});
    if (lt.target) {
      const tx=lt.target[0], ty=lt.target[2], tz=lt.target[1];
      pt.push({x:[tx], y:[ty], z:[tz],
               type:'scatter3d', mode:'markers+text',
               marker:{size:14, color:'#ff1744', opacity:0.9, line:{color:'#fff', width:2}},
               text:['TARGET'], textposition:'top center',
               textfont:{color:'#ff4444', size:12, family:'monospace'},
               name:'Target'});
      const r=120, steps=64;
      for (const [ax,ay,az] of [[1,1,0],[1,0,1],[0,1,1]]) {
        const cx=[],cy=[],cz=[];
        for (let i=0;i<=steps;i++){
          const a=2*Math.PI*i/steps;
          cx.push(tx + ax*r*Math.cos(a));
          cy.push(ty + ay*r*Math.sin(a) + az*r*Math.cos(a));
          cz.push(tz + az*r*Math.sin(a));
        }
        pt.push({x:cx, y:cy, z:cz, type:'scatter3d', mode:'lines',
                 line:{width:1, color:'rgba(255,68,68,0.35)'}, showlegend:false});
      }
    }
    react('c-path', pt, {
      paper_bgcolor:'#0f0f1c', font:{color:'#505070', size:10},
      margin:{t:26, b:8, l:8, r:8},
      title:titleLayout('Flight Paths'),
      showlegend:true,
      legend:{bgcolor:'rgba(0,0,0,0)', font:{size:9, color:'#505070'}, x:0, y:0},
      scene:{
        bgcolor:'#0b0b16',
        xaxis:{gridcolor:'#14142a', color:'#303050', title:{text:'X', font:{size:9}}},
        yaxis:{gridcolor:'#14142a', color:'#303050', title:{text:'Z', font:{size:9}}},
        zaxis:{gridcolor:'#14142a', color:'#303050', title:{text:'Y (alt)', font:{size:9}}}
      }
    });

    // 2D projections — stable camera, won't reset on update
    const path2d = (getX, getY, paths, mean, start, target) => {
      const traces = [];
      (paths||[]).forEach(p =>
        traces.push({x:p.map(getX), y:p.map(getY), type:'scatter', mode:'lines',
                     line:{width:1, color:'rgba(99,110,250,0.12)'}, showlegend:false}));
      if (mean)
        traces.push({x:mean.map(getX), y:mean.map(getY), type:'scatter', mode:'lines',
                     line:{width:2, color:'rgba(99,110,250,0.9)'}, name:'Mean', showlegend:false});
      if (start)
        traces.push({x:[getX(start)], y:[getY(start)], type:'scatter', mode:'markers',
                     marker:{size:8, color:'#00e676', symbol:'circle'}, name:'Start', showlegend:false});
      if (target)
        traces.push({x:[getX(target)], y:[getY(target)], type:'scatter', mode:'markers',
                     marker:{size:10, color:'#ff1744', symbol:'x', line:{width:2}}, name:'Target', showlegend:false});
      return traces;
    };

    const sp = lt.sampled_paths, mp = lt.mean_path;
    const st = lt.start, tg = lt.target;
    const PMAR = {t:26, b:34, l:44, r:10};

    react('c-path-top',
      path2d(v=>v[0], v=>v[2], sp, mp, st, tg),
      {...BG, margin:PMAR, showlegend:false,
       xaxis:{...AX, title:{text:'X', font:{size:9}}, scaleanchor:'y', scaleratio:1},
       yaxis:{...AX, title:{text:'Z (forward)', font:{size:9}}},
       title:titleLayout('Top (X-Z)')});

    react('c-path-side',
      path2d(v=>v[2], v=>v[1], sp, mp, st, tg),
      {...BG, margin:PMAR, showlegend:false,
       xaxis:{...AX, title:{text:'Z (forward)', font:{size:9}}},
       yaxis:{...AX, title:{text:'Y (altitude)', font:{size:9}}},
       title:titleLayout('Side (Z-Y)')});

    react('c-path-front',
      path2d(v=>v[0], v=>v[1], sp, mp, st, tg),
      {...BG, margin:PMAR, showlegend:false,
       xaxis:{...AX, title:{text:'X', font:{size:9}}, scaleanchor:'y', scaleratio:1},
       yaxis:{...AX, title:{text:'Y (altitude)', font:{size:9}}},
       title:titleLayout('Front (X-Y)')});

    // control surface inputs
    const ctrlYlabels = {aileron:'-1..1', elevator:'-1..1', rudder:'-1..1'};
    const ctrlTitles  = {aileron:'Aileron', elevator:'Elevator', rudder:'Rudder'};
    for (const key of ['aileron','elevator','rudder']) {
      if (!lt[key]) continue;
      react('c-'+key, bandTraces(lt[key], lt.step_x),
        {...BG, margin:SMMAR, showlegend:false,
         xaxis:{...AX, title:{text:'Step', font:{size:9}}},
         yaxis:{...AX, title:{text:ctrlYlabels[key], font:{size:9}}},
         title:titleLayout(ctrlTitles[key])});
    }
  }
}

setInterval(update, 1000);
update();
</script>
</body>
</html>"""

def recv_all(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf.extend(chunk)
    return bytes(buf)

def _compact(paths, totalLoss, distanceToTarget, controlEffortLoss, outputs, n, start=None, target=None, losses=None):
    m = paths.shape[0]
    step = max(1, n // 256)
    idx = np.arange(0, n, step)

    mean_path = paths.mean(axis=0)[idx].tolist()
    si = np.random.choice(m, min(150, m), replace=False)
    sampled_paths = paths[si][:, idx, :].tolist()

    def band(data):
        d = data[:, idx]
        s = np.random.choice(m, min(30, m), replace=False)
        return {"lo": d.min(axis=0).tolist(), "hi": d.max(axis=0).tolist(),
                "avg": d.mean(axis=0).tolist(), "samples": d[s].tolist()}

    # speed in m/s: distance between consecutive sampled positions / time per sample
    dt = step / 24.0  # seconds per idx-step (simulation runs at 24 Hz)
    pts = paths[:, idx, :]  # [m, len(idx), 3]
    diffs = np.diff(pts, axis=1)  # [m, len(idx)-1, 3]
    spd = np.linalg.norm(diffs, axis=2) / dt  # m/s
    speed_band = {"lo": spd.min(axis=0).tolist(), "hi": spd.max(axis=0).tolist(),
                  "avg": spd.mean(axis=0).tolist()}

    # altitude: y component of position (index 1 = C's Y = altitude)
    alt = paths[:, idx, 1]  # [m, len(idx)]
    alt_band = {"lo": alt.min(axis=0).tolist(), "hi": alt.max(axis=0).tolist(),
                "avg": alt.mean(axis=0).tolist()}

    # final loss distribution histogram
    loss_hist = None
    if losses is not None:
        counts, edges = np.histogram(losses, bins=30)
        loss_hist = {"x": ((edges[:-1] + edges[1:]) / 2).tolist(), "y": counts.tolist()}

    # per-control surface bands: outputs shape [m, n, 3] -> Aileron, Elevator, Rudder
    ctrl_aileron  = band(outputs[:, :, 0])
    ctrl_elevator = band(outputs[:, :, 1])
    ctrl_rudder   = band(outputs[:, :, 2])

    return {"step_x": idx.tolist(), "mean_path": mean_path,
            "sampled_paths": sampled_paths,
            "start": start, "target": target,
            "total": band(totalLoss), "distance": band(distanceToTarget),
            "control": band(controlEffortLoss),
            "speed": speed_band, "speed_x": idx[1:].tolist(),
            "altitude": alt_band, "loss_hist": loss_hist,
            "aileron": ctrl_aileron, "elevator": ctrl_elevator, "rudder": ctrl_rudder}


def parse_training_stats(data):
    global _latest
    m, n = struct.unpack_from("<II", data, 0)
    start = struct.unpack_from("<3f", data, 8)
    target = struct.unpack_from("<3f", data, 20)
    backprop_loss = struct.unpack_from("<f", data, 32)[0]
    max_angle_deg = struct.unpack_from("<f", data, 36)[0]
    off = 40  # 2*uint32 + 2*float3 + 2*float

    losses = np.frombuffer(data, dtype="<f4", count=m, offset=off).copy()
    off += m * 4

    paths = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4)[:, :, :3].copy()
    off += m * n * 16

    epoch_losses = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4).copy()
    off += m * n * 16

    # ModelOutput: Aileron, Elevator, Rudder (3 floats each, packed tightly)
    outputs = np.frombuffer(data, dtype="<f4", count=m * n * 3, offset=off).reshape(m, n, 3).copy()

    compact = _compact(paths, epoch_losses[:, :, 0], epoch_losses[:, :, 1], epoch_losses[:, :, 2], outputs, n,
                       list(start), list(target), losses)

    with _lock:
        _epoch_losses.append((float(losses.min()), float(losses.mean()), float(losses.max())))
        qs = np.percentile(losses, [10, 25, 50, 75, 90])
        _epoch_quantiles.append([float(q) for q in qs])
        _backprop_losses.append(float(backprop_loss))
        _max_angles.append(float(max_angle_deg))
        _latest = compact

def handle(conn, addr):
    try:
        raw_total = recv_all(conn, 4)
        total_size = struct.unpack("<I", raw_total)[0]
        payload = recv_all(conn, total_size)

        msg_type = payload[0]
        data = payload[1:]

        if msg_type == 1:  # POST
            parse_training_stats(data)
        else:
            print(f"GET from {addr}, {len(data)} bytes")

        conn.sendall(struct.pack("<I", 0))
    except Exception as e:
        print(f"error from {addr}: {e}")
    finally:
        conn.close()

def _tcp_loop():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, TCP_PORT))
        srv.listen()
        print(f"TCP listener on {HOST}:{TCP_PORT}")
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()

def visualizePath(paths, start=None, target=None):
    # paths: [m][n][3]
    m, n, _ = paths.shape
    fig = go.Figure(layout=dict(template="plotly_dark"))
    for i in range(m):
        fig.add_trace(go.Scatter3d(
            x=paths[i, :, 0], y=paths[i, :, 1], z=paths[i, :, 2],
            mode='lines',
            line=dict(width=1, color='rgba(99,110,250,0.04)'),
            showlegend=False
        ))
    mean_path = paths.mean(axis=0)
    fig.add_trace(go.Scatter3d(
        x=mean_path[:, 0], y=mean_path[:, 1], z=mean_path[:, 2],
        mode='lines',
        line=dict(width=4, color='rgba(99,110,250,1.0)'),
        name='Mean'
    ))
    if start is not None:
        fig.add_trace(go.Scatter3d(
            x=[start[0]], y=[start[1]], z=[start[2]],
            mode='markers', marker=dict(size=5, color='green'), name='Start'
        ))
    if target is not None:
        fig.add_trace(go.Scatter3d(
            x=[target[0]], y=[target[1]], z=[target[2]],
            mode='markers', marker=dict(size=5, color='red'), name='Target'
        ))
    return fig

def visualizeLosses(totalLoss, distanceToTarget, controlEffortLoss):
    # totalLoss, distanceToTarget, controlEffortLoss: [m][n]
    from plotly.subplots import make_subplots
    m, n = totalLoss.shape
    # cap individual lines for browser performance
    sample_idx = np.random.choice(m, min(m, 50), replace=False) if m > 50 else np.arange(m)
    fig = make_subplots(rows=3, cols=1, shared_xaxes=True,
                        subplot_titles=("Total Loss", "Distance to Target", "Control Effort Loss"))
    x = np.arange(n)
    for row, data in enumerate([totalLoss, distanceToTarget, controlEffortLoss], 1):
        lo, hi, avg = data.min(axis=0), data.max(axis=0), data.mean(axis=0)
        # min-max band
        fig.add_trace(go.Scatter(x=x, y=lo, mode='lines', line=dict(width=0), showlegend=False), row=row, col=1)
        fig.add_trace(go.Scatter(x=x, y=hi, mode='lines', line=dict(width=0),
                                 fill='tonexty', fillcolor='rgba(99,110,250,0.15)',
                                 name='Range', showlegend=(row == 1)), row=row, col=1)
        # faint sampled individual runs
        for i in sample_idx:
            fig.add_trace(go.Scatter(x=x, y=data[i], mode='lines',
                                     line=dict(width=1, color='rgba(99,110,250,0.2)'),
                                     showlegend=False), row=row, col=1)
        # average
        fig.add_trace(go.Scatter(x=x, y=avg, mode='lines', line=dict(width=2),
                                 name='Avg', showlegend=(row == 1)), row=row, col=1)
    fig.update_layout(template="plotly_dark")
    return fig

def visualizeEpochLosses(epoch_losses):
    arr = np.array(epoch_losses)
    x = np.arange(len(arr))
    fig = go.Figure(layout=dict(template="plotly_dark"))
    fig.add_trace(go.Scatter(x=x, y=arr[:, 0], mode='lines', line=dict(width=0), showlegend=False))
    fig.add_trace(go.Scatter(x=x, y=arr[:, 2], mode='lines', line=dict(width=0),
                             fill='tonexty', fillcolor='rgba(99,110,250,0.2)', name='Min-Max range'))
    fig.add_trace(go.Scatter(x=x, y=arr[:, 1], mode='lines', line=dict(width=2), name='Avg'))
    fig.update_layout(xaxis_title='Epoch', yaxis_title='Loss')
    return fig

class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/':
            body = _HTML.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/data':
            with _lock:
                payload = json.dumps({'epoch_losses': list(_epoch_losses), 'epoch_quantiles': list(_epoch_quantiles), 'backprop_losses': list(_backprop_losses), 'max_angles': list(_max_angles), 'latest': _latest}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *_):
        pass


def main():
    threading.Thread(target=_tcp_loop, daemon=True).start()
    print(f"Dashboard: http://localhost:{HTTP_PORT}")
    HTTPServer(('', HTTP_PORT), _Handler).serve_forever()


if __name__ == "__main__":
    main()