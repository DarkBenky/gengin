import signal
import socket
import struct
import threading

HOST = '127.0.0.1'
PORT = 6789

try:
    import wandb
    wandb.init(project="gengin-flight-control")
    HAS_WANDB = True
except ImportError:
    print("wandb not installed, logging to stdout only", flush=True)
    HAS_WANDB = False

last_gen = -1
wandb_lock = threading.Lock()


def new_run():
    global last_gen
    if HAS_WANDB:
        wandb.finish()
        wandb.init(project="gengin-flight-control")
    last_gen = -1
    print("--- new wandb run started ---", flush=True)


def log_stats(gen, avg_loss, best_loss):
    global last_gen
    with wandb_lock:
        if gen <= last_gen:
            new_run()
        last_gen = gen
        print(f"gen={gen:5d}  avg_loss={avg_loss:.4f}  best_loss={best_loss:.4f}", flush=True)
        if HAS_WANDB:
            wandb.log({"avg_loss": avg_loss, "best_loss": best_loss}, step=gen)


def recv_all(conn, n):
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def handle(conn):
    with conn:
        try:
            hdr = recv_all(conn, 4)
            total_size = struct.unpack('<I', hdr)[0]
            if total_size < 1:
                return
            recv_all(conn, 1)  # type byte (ignored)
            data = recv_all(conn, total_size - 1)
            # payload: int32 gen, float avg_loss, float best_loss
            gen, avg_loss, best_loss = struct.unpack('<iff', data)
            log_stats(gen, avg_loss, best_loss)
        except Exception as e:
            print(f"handle error: {e}", flush=True)
        finally:
            try:
                conn.sendall(struct.pack('<I', 0))
            except Exception:
                pass


server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT))
server.listen(256)
print(f"wandb logger listening on {HOST}:{PORT}", flush=True)


def shutdown(sig, frame):
    print("shutting down", flush=True)
    if HAS_WANDB:
        wandb.finish()
    server.close()


signal.signal(signal.SIGINT, shutdown)
signal.signal(signal.SIGTERM, shutdown)

while True:
    try:
        conn, _ = server.accept()
    except OSError:
        break
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
