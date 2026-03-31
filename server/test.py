import socket
import time

PORT = 8080
ADDRESS = "localhost"

def send(sock, message):
    print("Sending message to server:", message)
    payload = message.encode()
    sock.sendall(len(payload).to_bytes(4, "little") + payload)
    response_length_data = sock.recv(4)
    if not response_length_data:
        print("No response from server.")
        return None
    response_length = int.from_bytes(response_length_data, "little")
    response_data = sock.recv(response_length)
    response = response_data.decode()
    return response

if __name__ == "__main__":
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((ADDRESS, PORT))
    res = send(sock, "Hello, Server!")
    print("Response from server:", res)
    sock.close()

    # benchmark connection
    responseTime = []
    for i in range(1024):
        start = time.time()

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((ADDRESS, PORT))
        res = send(sock, "Hello, Server!")
        sock.close()

        responseTime.append(time.time() - start)
        
    print("Average response time (ms):", sum(responseTime) / len(responseTime) * 1000)
    print("Max response time (ms):", max(responseTime) * 1000)
    print("Min response time (ms):", min(responseTime) * 1000)
    print("Median response time (ms):", sorted(responseTime)[len(responseTime) // 2] * 1000)
    print("99th percentile response time (ms):", sorted(responseTime)[int(len(responseTime) * 0.99)] * 1000)
