import socket

PORT = 8080
ADDRESS = "localhost"

def send(socket, message):
    socket.sendall(message.encode())
    response = socket.recv(1024).decode()
    return response

if __name__ == "__main__":

    socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    socket.connect((ADDRESS, PORT))

    res = send(socket, "Hello, Server!")
    print("Response from server:", res)