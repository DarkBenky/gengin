import pygame
import socket
import time
import random
import json
import threading
import queue

SERVER_HOST = '127.0.0.1'
SERVER_PORT = 5555

class Player:
    def __init__(self, x, y, imagePath):
        self.id = random.randint(1000, 9999)  # Generate a random ID for the player
        self.x = x
        self.y = y
        self.velX = 0
        self.velY = 0
        self.lastSyncTime = time.time()
        self.playerTexture = pygame.Surface((50, 50))
        # Load the player texture from the specified image path
        try:
            self.playerTexture = pygame.image.load(imagePath).convert_alpha()
            self.playerTexturePath = imagePath
        except Exception as e:
            print(f"Unable to load player texture: {e}")
            self.playerTexture.fill((255, 0, 0))
            self.playerTexturePath = None

    def draw(self, screen):
        screen.blit(self.playerTexture, (self.x, self.y))

    def drawAt(self, screen, x, y):
        screen.blit(self.playerTexture, (x, y))

    def update(self, xVel, yVel, dt=1/30):
        # frame-rate independent physics; velocity in px/s
        friction = 0.98 ** (dt * 60)
        self.velX = self.velX * friction + xVel * 3000.0 * (1.0 - friction)
        self.velY = self.velY * friction + yVel * 3000.0 * (1.0 - friction)
        self.x += self.velX * dt
        self.y += self.velY * dt
        if self.x < 0:
            self.x = -self.x
            self.velX = abs(self.velX)
        if self.y < 0:
            self.y = -self.y
            self.velY = abs(self.velY)
        if self.x > 800 - self.playerTexture.get_width():
            self.x = 2 * (800 - self.playerTexture.get_width()) - self.x
            self.velX = -abs(self.velX)
        if self.y > 600 - self.playerTexture.get_height():
            self.y = 2 * (600 - self.playerTexture.get_height()) - self.y
            self.velY = -abs(self.velY)

    def sendPosition(self, sock):
        packet = {
            'id': self.id,
            'x': self.x,
            'y': self.y,
            'velX': self.velX,
            'velY': self.velY,
            'time': time.time(),
            'texturePath': self.playerTexturePath
        }

        positionData = str(packet)
        try:
            sock.sendall(positionData.encode())
        except socket.error as e:
            print(f"Error sending position data: {e}")


class LocalState:
    def __init__(self):
        self.players = {}

    def updatePlayer(self, playerData):
        playerId = playerData['id']
        if playerId not in self.players:
            newPlayer = Player(playerData['x'], playerData['y'], playerData['texturePath'])
            newPlayer.lastSyncTime = time.time()
            self.players[playerId] = newPlayer
        else:
            existingPlayer = self.players[playerId]
            existingPlayer.x = playerData['x']
            existingPlayer.y = playerData['y']
            existingPlayer.velX = playerData['velX']
            existingPlayer.velY = playerData['velY']
            existingPlayer.lastSyncTime = time.time()


class NetStats:
    def __init__(self):
        self.ping = 0.0
        self.tps = 0.0
        self._ping_sent = None
        self._response_count = 0
        self._tps_start = time.time()

    def sent(self):
        self._ping_sent = time.time()

    def received(self):
        now = time.time()
        if self._ping_sent is not None:
            self.ping = (now - self._ping_sent) * 1000
            self._ping_sent = None
        self._response_count += 1
        elapsed = now - self._tps_start
        if elapsed >= 1.0:
            self.tps = self._response_count / elapsed
            self._response_count = 0
            self._tps_start = now


def connectToServer():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((SERVER_HOST, SERVER_PORT))
        print(f"Connected to server at {SERVER_HOST}:{SERVER_PORT}")
        return sock
    except Exception as e:
        print(f"Could not connect to server: {e}")
        return None


def receiveThread(sock, recv_queue, local_id, stats):
    buffer = ''
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                break
            buffer += data.decode()
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                if not line.strip():
                    continue
                msg = json.loads(line)
                stats.received()
                for playerData in msg['players']:
                    if playerData['id'] != local_id:
                        recv_queue.put(playerData)
        except Exception as e:
            print(f"Receive error: {e}")
            break


def sendUpdate(sock, player):
    msg = {
        'type': 'update',
        'id': player.id,
        'x': player.x,
        'y': player.y,
        'velX': player.velX,
        'velY': player.velY,
        'time': time.time(),
        'texturePath': player.playerTexturePath,
    }
    try:
        sock.sendall((json.dumps(msg) + '\n').encode())
    except Exception as e:
        print(f"Send error: {e}")


def requestState(sock, stats):
    stats.sent()
    msg = {'type': 'get_state', 'time': time.time()}
    try:
        sock.sendall((json.dumps(msg) + '\n').encode())
    except Exception as e:
        print(f"Request error: {e}")


def getKeyPress():
    keys = pygame.key.get_pressed()
    xVel = 0
    yVel = 0
    if keys[pygame.K_w]:
        yVel = -1
    if keys[pygame.K_s]:
        yVel = 1
    if keys[pygame.K_a]:
        xVel = -1
    if keys[pygame.K_d]:
        xVel = 1
    return xVel, yVel


if __name__ == "__main__":
    pygame.init()
    screen = pygame.display.set_mode((800, 600))
    pygame.display.set_caption("Exploration Game")

    lState = LocalState()
    player = Player(100, 100, "player_texture.png")
    lState.players[player.id] = player

    stats = NetStats()
    recv_queue = queue.Queue()
    sock = connectToServer()
    if sock:
        t = threading.Thread(target=receiveThread, args=(sock, recv_queue, player.id, stats), daemon=True)
        t.start()

    font = pygame.font.SysFont('monospace', 16)
    frame = 0
    running = True
    fpsClock = pygame.time.Clock()
    while running:
        dt = fpsClock.tick(30) / 1000.0

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        xVel, yVel = getKeyPress()
        player.update(xVel, yVel, dt)

        while not recv_queue.empty():
            lState.updatePlayer(recv_queue.get_nowait())

        if sock and frame % 3 == 0:  # 10 Hz at 30 fps
            sendUpdate(sock, player)
            requestState(sock, stats)

        screen.fill((0, 0, 0))

        now = time.time()
        for p in lState.players.values():
            if p is player:
                p.draw(screen)
            else:
                dt_ext = min(now - p.lastSyncTime, 0.5)
                p.drawAt(screen, p.x + p.velX * dt_ext, p.y + p.velY * dt_ext)

        if sock:
            label = font.render(f'Ping: {stats.ping:.1f}ms  TPS: {stats.tps:.1f}', True, (0, 220, 0))
            screen.blit(label, (8, 8))

        pygame.display.flip()
        frame += 1

    if sock:
        sock.close()
    pygame.quit()

