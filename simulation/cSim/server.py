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
_latest = None

_HTML = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Training Monitor</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
* { box-sizing:border-box; }
body { background:#111; color:#ccc; font-family:sans-serif; margin:0; padding:8px; }
h1 { margin:2px 0 4px; font-size:1rem; font-weight:500; }
#status { font-size:0.78rem; color:#666; margin-bottom:6px; }
.row { display:flex; gap:6px; }
.col { flex:1; min-width:0; }
</style>
</head>
<body>
<h1>Training Monitor</h1>
<div id="status">Waiting for training data...</div>
<div id="c-epoch" style="height:200px;margin-bottom:6px"></div>
<div id="c-epoch-fan" style="height:220px;margin-bottom:6px"></div>
<div class="row">
  <div class="col">
    <div id="c-total"    style="height:200px"></div>
    <div id="c-distance" style="height:200px;margin-top:6px"></div>
    <div id="c-control"  style="height:200px;margin-top:6px"></div>
  </div>
  <div class="col">
    <div id="c-path" style="height:612px"></div>
  </div>
</div>
<div class="row" style="margin-top:6px">
  <div class="col">
    <div id="c-speed" style="height:200px"></div>
  </div>
  <div class="col">
    <div id="c-altitude" style="height:200px"></div>
  </div>
  <div class="col">
    <div id="c-hist" style="height:200px"></div>
  </div>
</div>
<script>
const BG  = {paper_bgcolor:'#111',plot_bgcolor:'#1a1a2e',font:{color:'#aaa'}};
const AX  = {gridcolor:'#2a2a3e',zerolinecolor:'#333'};
const MAR = {t:28,b:30,l:70,r:10};
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
      // preserve user zoom/pan on 2D axes
      for (const ax of ['xaxis','yaxis','xaxis2','yaxis2']) {
        if (fl[ax] && fl[ax].autorange === false) {
          layout = {...layout, [ax]: {...(layout[ax]||{}), range: fl[ax].range, autorange: false}};
        }
      }
    }
    Plotly.react(id, traces, layout);
  }
}

function bandTraces(d, x) {
  const c = 'rgba(99,110,250,';
  const t = [
    {x,y:d.lo, type:'scatter',mode:'lines',line:{width:0},showlegend:false},
    {x,y:d.hi, type:'scatter',mode:'lines',line:{width:0},fill:'tonexty',
     fillcolor:c+'0.15)',name:'Range',showlegend:true},
  ];
  (d.samples||[]).forEach(s=>
    t.push({x,y:s,type:'scatter',mode:'lines',line:{width:1,color:c+'0.2)'},showlegend:false})
  );
  t.push({x,y:d.avg,type:'scatter',mode:'lines',line:{width:2,color:c+'1)'},name:'Avg'});
  return t;
}

async function update() {
  let r; try { r=await fetch('/data'); } catch(e){return;}
  if (!r.ok) return;
  const d = await r.json();

  if (d.epoch_losses && d.epoch_losses.length) {
    const el=d.epoch_losses, x=el.map((_,i)=>i);
    const lo=el.map(v=>v[0]), avg=el.map(v=>v[1]), hi=el.map(v=>v[2]);
    const last=el.length-1;
    document.getElementById('status').textContent =
      `Epoch ${el.length}  |  avg: ${avg[last].toFixed(3)}  |  min: ${lo[last].toFixed(3)}`;
    react('c-epoch',
      [{x,y:lo,type:'scatter',mode:'lines',line:{width:0},showlegend:false},
       {x,y:hi,type:'scatter',mode:'lines',line:{width:0},fill:'tonexty',
        fillcolor:'rgba(99,110,250,0.2)',name:'Min-Max'},
       {x,y:avg,type:'scatter',mode:'lines',line:{width:2,color:'rgb(99,110,250)'},name:'Avg'}],
      {...BG,margin:MAR,xaxis:{...AX,title:'Epoch'},yaxis:{...AX,title:'Loss'},
       title:{text:'Epoch Loss',font:{color:'#bbb',size:12}}});
  }

  if (d.epoch_quantiles && d.epoch_quantiles.length) {
    const eq=d.epoch_quantiles, x=eq.map((_,i)=>i);
    const p10=eq.map(v=>v[0]), p25=eq.map(v=>v[1]), p50=eq.map(v=>v[2]),
          p75=eq.map(v=>v[3]), p90=eq.map(v=>v[4]);
    const c = 'rgba(99,110,250,';
    react('c-epoch-fan', [
      {x,y:p10,type:'scatter',mode:'lines',line:{width:0},showlegend:false},
      {x,y:p90,type:'scatter',mode:'lines',line:{width:0},fill:'tonexty',
       fillcolor:c+'0.10)',name:'p10-p90',showlegend:true},
      {x,y:p25,type:'scatter',mode:'lines',line:{width:0},showlegend:false},
      {x,y:p75,type:'scatter',mode:'lines',line:{width:0},fill:'tonexty',
       fillcolor:c+'0.25)',name:'p25-p75',showlegend:true},
      {x,y:p50,type:'scatter',mode:'lines',line:{width:2,color:c+'1)'},name:'Median'},
    ], {...BG,margin:MAR,
       xaxis:{...AX,title:'Epoch'},yaxis:{...AX,title:'Loss'},
       title:{text:'Loss Distribution Over Time',font:{color:'#bbb',size:12}}});
  }

  if (d.latest) {
    const lt=d.latest, x=lt.step_x;
    [['c-total','total','Total Loss'],
     ['c-distance','distance','Distance to Target'],
     ['c-control','control','Control Effort']].forEach(([id,key,title],i) => {
      react(id, bandTraces(lt[key],x),
        {...BG,margin:MAR,showlegend:i===0,
         xaxis:{...AX,title:'Step'},yaxis:{...AX},
         title:{text:title,font:{color:'#bbb',size:12}}});
    });

    if (lt.speed)
      react('c-speed', bandTraces(lt.speed, lt.speed_x),
        {...BG,margin:MAR,showlegend:false,
         xaxis:{...AX,title:'Step'},yaxis:{...AX,title:'Speed (units/step)'},
         title:{text:'Speed',font:{color:'#bbb',size:12}}});

    if (lt.altitude)
      react('c-altitude', bandTraces(lt.altitude, x),
        {...BG,margin:MAR,showlegend:false,
         xaxis:{...AX,title:'Step'},yaxis:{...AX,title:'Altitude (Y)'},
         title:{text:'Altitude',font:{color:'#bbb',size:12}}});

    if (lt.loss_hist) {
      const h=lt.loss_hist;
      react('c-hist',
        [{x:h.x,y:h.y,type:'bar',marker:{color:'rgba(99,110,250,0.7)'},name:'Sims'}],
        {...BG,margin:MAR,bargap:0.05,showlegend:false,
         xaxis:{...AX,title:'Final Loss'},yaxis:{...AX,title:'Count'},
         title:{text:'Loss Distribution',font:{color:'#bbb',size:12}}});
    }

    const pt = [];
    (lt.sampled_paths||[]).forEach(p=>
      pt.push({x:p.map(v=>v[0]),y:p.map(v=>v[2]),z:p.map(v=>v[1]),
               type:'scatter3d',mode:'lines',
               line:{width:1,color:'rgba(99,110,250,0.15)'},showlegend:false}));
    if (lt.mean_path) {
      const mp=lt.mean_path;
      pt.push({x:mp.map(v=>v[0]),y:mp.map(v=>v[2]),z:mp.map(v=>v[1]),
               type:'scatter3d',mode:'lines',
               line:{width:4,color:'rgba(99,110,250,1)'},name:'Mean'});
    }
    if (lt.start) {
      pt.push({x:[lt.start[0]],y:[lt.start[2]],z:[lt.start[1]],
               type:'scatter3d',mode:'markers',
               marker:{size:8,color:'#00e676',symbol:'circle'},name:'Start'});
    }
    if (lt.target) {
      const tx=lt.target[0], ty=lt.target[2], tz=lt.target[1];
      // large glowing sphere
      pt.push({x:[tx],y:[ty],z:[tz],
               type:'scatter3d',mode:'markers+text',
               marker:{size:20,color:'#ff1744',opacity:0.9,
                       line:{color:'#ffffff',width:3}},
               text:['TARGET'],textposition:'top center',
               textfont:{color:'#ff4444',size:16,family:'monospace'},
               name:'Target'});
      // cross-hair ring — 3 circles on XY/XZ/YZ planes
      const r=120, steps=64;
      for (const [ax,ay,az] of [[1,1,0],[1,0,1],[0,1,1]]) {
        const cx=[],cy=[],cz=[];
        for (let i=0;i<=steps;i++){
          const a=2*Math.PI*i/steps;
          cx.push(tx + ax*r*Math.cos(a));
          cy.push(ty + ay*r*Math.sin(a) + az*r*Math.cos(a));
          cz.push(tz + az*r*Math.sin(a));
        }
        pt.push({x:cx,y:cy,z:cz,type:'scatter3d',mode:'lines',
                 line:{width:2,color:'rgba(255,68,68,0.5)'},showlegend:false});
      }
    }
    react('c-path', pt, {
      paper_bgcolor:'#111',font:{color:'#aaa'},margin:{t:28,b:10,l:10,r:10},
      title:{text:'Paths',font:{color:'#bbb',size:12}},
      scene:{bgcolor:'#1a1a2e',
             xaxis:{gridcolor:'#2a2a3e',color:'#666',title:'X'},
             yaxis:{gridcolor:'#2a2a3e',color:'#666',title:'Z (forward)'},
             zaxis:{gridcolor:'#2a2a3e',color:'#666',title:'Y (altitude)'}}
    });
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

def _compact(paths, totalLoss, distanceToTarget, controlEffortLoss, n, start=None, target=None, losses=None):
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

    # speed: euclidean distance between consecutive sampled positions
    pts = paths[:, idx, :]  # [m, len(idx), 3]
    diffs = np.diff(pts, axis=1)  # [m, len(idx)-1, 3]
    spd = np.linalg.norm(diffs, axis=2)  # [m, len(idx)-1]
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

    return {"step_x": idx.tolist(), "mean_path": mean_path,
            "sampled_paths": sampled_paths,
            "start": start, "target": target,
            "total": band(totalLoss), "distance": band(distanceToTarget),
            "control": band(controlEffortLoss),
            "speed": speed_band, "speed_x": idx[1:].tolist(),
            "altitude": alt_band, "loss_hist": loss_hist}


def parse_training_stats(data):
    global _latest
    m, n = struct.unpack_from("<II", data, 0)
    start = struct.unpack_from("<3f", data, 8)
    target = struct.unpack_from("<3f", data, 20)
    off = 32  # 2*uint32 + 2*float3 (3 floats each)

    losses = np.frombuffer(data, dtype="<f4", count=m, offset=off).copy()
    off += m * 4

    paths = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4)[:, :, :3].copy()
    off += m * n * 16

    epoch_losses = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4).copy()

    compact = _compact(paths, epoch_losses[:, :, 0], epoch_losses[:, :, 1], epoch_losses[:, :, 2], n,
                       list(start), list(target), losses)

    with _lock:
        _epoch_losses.append((float(losses.min()), float(losses.mean()), float(losses.max())))
        qs = np.percentile(losses, [10, 25, 50, 75, 90])
        _epoch_quantiles.append([float(q) for q in qs])
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
                payload = json.dumps({'epoch_losses': list(_epoch_losses), 'epoch_quantiles': list(_epoch_quantiles), 'latest': _latest}).encode()
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