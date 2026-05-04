import socket
import struct
import numpy as np

HOST = "127.0.0.1"
PORT = 5173


# trainingStats layout:
#   uint32 modelCount
#   uint32 iterationCount
#   float[modelCount]                    losses
#   float3[modelCount * iterationCount]  paths       (4 floats each, w unused)
#   float3[modelCount * iterationCount]  epochLosses (x=totalLoss, y=distanceToTarget, z=controlEffortLoss, w unused)

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

if __name__ == "__main__":
    main()
