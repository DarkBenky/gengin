import socket
import threading
import json
import time

HOST = '127.0.0.1'
PORT = 5555

players = {}
players_lock = threading.Lock()


def interpolate(player, at_time):
    dt = at_time - player['time']
    return {
        'id': player['id'],
        'x': player['x'] + player['velX'] * dt,
        'y': player['y'] + player['velY'] * dt,
        'velX': player['velX'],
        'velY': player['velY'],
        'texturePath': player['texturePath'],
    }


def handle_client(conn, addr):
    print(f"Connected: {addr}")
    client_id = None
    buffer = ''
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buffer += data.decode()
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue

                if msg['type'] == 'update':
                    client_id = msg['id']
                    with players_lock:
                        players[client_id] = {
                            'id': msg['id'],
                            'x': msg['x'],
                            'y': msg['y'],
                            'velX': msg['velX'],
                            'velY': msg['velY'],
                            'time': msg['time'],
                            'texturePath': msg['texturePath'],
                        }

                elif msg['type'] == 'get_state':
                    req_time = msg['time']
                    with players_lock:
                        state = [interpolate(p, req_time) for p in players.values()]
                    response = json.dumps({'players': state}) + '\n'
                    conn.sendall(response.encode())

    except Exception as e:
        print(f"Client {addr} error: {e}")
    finally:
        if client_id is not None:
            with players_lock:
                players.pop(client_id, None)
            print(f"Player {client_id} disconnected")
        conn.close()


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen()
        print(f"Server listening on {HOST}:{PORT}")
        while True:
            conn, addr = s.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()


if __name__ == '__main__':
    main()
