import socket
import struct
import numpy as np
import numpy as np
import plotly.graph_objects as go

HOST = "127.0.0.1"
PORT = 5173


# trainingStats layout:
#   uint32 modelCount
#   uint32 iterationCount
#   float[modelCount]                    losses
#   float3[modelCount * iterationCount]  paths       (4 floats each, w unused)
#   float3[modelCount * iterationCount]  epochLosses (x=totalLoss, y=distanceToTarget, z=controlEffortLoss, w unused)

epochLosses = [] # [iterations][3] = [minForEpoch, avgForEpoch, maxForEpoch]

def recv_all(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf.extend(chunk)
    return bytes(buf)

def parse_training_stats(data):
    m, n = struct.unpack_from("<II", data, 0)
    off = 8

    losses = np.frombuffer(data, dtype="<f4", count=m, offset=off).copy()
    off += m * 4

    paths = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4)[:, :, :3].copy()
    off += m * n * 16

    epoch_losses = np.frombuffer(data, dtype="<f4", count=m * n * 4, offset=off).reshape(m, n, 4).copy()

    minEpochLoss = epoch_losses[:, :, 0].min()
    averageEpochLoss = epoch_losses[:, :, 0].mean()
    maxEpochLoss = epoch_losses[:, :, 0].max()
    epochLosses.append((minEpochLoss, averageEpochLoss, maxEpochLoss))

    return {
        "modelCount":        m,
        "iterationCount":    n,
        "losses":            losses,                    # [m]
        "paths":             paths,                     # [m][n][3]
        "totalLoss":         epoch_losses[:, :, 0],     # [m][n]
        "distanceToTarget":  epoch_losses[:, :, 1],     # [m][n]
        "controlEffortLoss": epoch_losses[:, :, 2],     # [m][n]
    }

def handle(conn, addr):
    try:
        raw_total = recv_all(conn, 4)
        total_size = struct.unpack("<I", raw_total)[0]
        payload = recv_all(conn, total_size)

        msg_type = payload[0]
        data = payload[1:]

        if msg_type == 1:  # POST
            stats = parse_training_stats(data)
            print(stats)
        else:
            print(f"GET from {addr}, {len(data)} bytes")

        # ack: send uint32(0) = empty response
        conn.sendall(struct.pack("<I", 0))
    except Exception as e:
        print(f"error from {addr}: {e}")
    finally:
        conn.close()

def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, PORT))
        srv.listen()
        print(f"listening on {HOST}:{PORT}")
        while True:
            conn, addr = srv.accept()
            handle(conn, addr)

def visualizePath(paths, start, target):
    # paths: [m][n][3]
    m, n, _ = paths.shape
    fig = go.Figure(layout=dict(template="plotly_dark"))
    for i in range(m):
        fig.add_trace(go.Scatter3d(
            x=paths[i, :, 0],
            y=paths[i, :, 1],
            z=paths[i, :, 2],
            mode='lines',
            name=f'Model {i}'
        ))
    fig.add_trace(go.Scatter3d(
        x=[start[0]],
        y=[start[1]],
        z=[start[2]],
        mode='markers',
        marker=dict(size=5, color='green'),
        name='Start'
    ))
    fig.add_trace(go.Scatter3d(
        x=[target[0]],
        y=[target[1]],
        z=[target[2]],
        mode='markers',
        marker=dict(size=5, color='red'),
        name='Target'
    ))
    return fig

def visualizeLosses(totalLoss, distanceToTarget, controlEffortLoss):
    # totalLoss, distanceToTarget, controlEffortLoss: [m][n]
    from plotly.subplots import make_subplots
    m, n = totalLoss.shape
    fig = make_subplots(rows=3, cols=1, shared_xaxes=True,
                        subplot_titles=("Total Loss", "Distance to Target", "Control Effort Loss"))
    x = np.arange(n)
    for i in range(m):
        fig.add_trace(go.Scatter(x=x, y=totalLoss[i],         mode='lines', name=f'Model {i}',                    legendgroup=f'{i}'),              row=1, col=1)
        fig.add_trace(go.Scatter(x=x, y=distanceToTarget[i],  mode='lines', name=f'Model {i}',                    legendgroup=f'{i}', showlegend=False), row=2, col=1)
        fig.add_trace(go.Scatter(x=x, y=controlEffortLoss[i], mode='lines', name=f'Model {i}',                    legendgroup=f'{i}', showlegend=False), row=3, col=1)
    fig.update_layout(template="plotly_dark")
    return fig

def visualizeEpochLosses(epochLosses):
    # epochLosses: [iterations][3] = [minForEpoch, avgForEpoch, maxForEpoch]
    epochLosses = np.array(epochLosses)  # [iterations][3]
    x = np.arange(len(epochLosses))
    fig = go.Figure(layout=dict(template="plotly_dark"))
    # bottom of band (min)
    fig.add_trace(go.Scatter(
        x=x, y=epochLosses[:, 0],
        mode='lines', line=dict(width=0),
        name='Min', showlegend=False
    ))
    # top of band (max), filled down to previous trace
    fig.add_trace(go.Scatter(
        x=x, y=epochLosses[:, 2],
        mode='lines', line=dict(width=0),
        fill='tonexty', fillcolor='rgba(99,110,250,0.2)',
        name='Min-Max range'
    ))
    # average line on top
    fig.add_trace(go.Scatter(
        x=x, y=epochLosses[:, 1],
        mode='lines', line=dict(width=2),
        name='Avg'
    ))
    fig.update_layout(xaxis_title='Epoch', yaxis_title='Loss')
    return fig

if __name__ == "__main__":
    visualizePath(np.random.rand(3, 10, 3), [0, 0, 0], [1, 1, 1]).show(renderer="browser")
    visualizeLosses(np.random.rand(3, 10), np.random.rand(3, 10), np.random.rand(3, 10)).show(renderer="browser")
    visualizeEpochLosses([(0.5, 0.7, 0.9), (0.4, 0.6, 0.8), (0.3, 0.5, 0.7)]).show(renderer="browser")
