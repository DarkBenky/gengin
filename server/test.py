import socket
import struct
import time

PORT = 8080
ADDRESS = "localhost"

# Object layout (matches C struct, little-endian):
#   uint32  Id
#   uint32  TimeStamp
#   float3  Position  (x, y, z, w_pad)
#   float3  Rotation  (x, y, z, w_pad)
#   float3  Scale     (x, y, z, w_pad)
OBJECT_FMT = "<II" + "ffff" * 3  # 56 bytes
OBJECT_SIZE = struct.calcsize(OBJECT_FMT)

def pack_object(id, timestamp, pos, rot, scale):
    return struct.pack(OBJECT_FMT,
        id, timestamp,
        pos[0], pos[1], pos[2], 0.0,
        rot[0], rot[1], rot[2], 0.0,
        scale[0], scale[1], scale[2], 0.0,
    )

def unpack_objects(data):
    count = len(data) // OBJECT_SIZE
    objects = []
    for i in range(count):
        chunk = data[i * OBJECT_SIZE:(i + 1) * OBJECT_SIZE]
        fields = struct.unpack(OBJECT_FMT, chunk)
        objects.append({
            "id": fields[0], "ts": fields[1],
            "pos": fields[2:5], "rot": fields[6:9], "scale": fields[10:13],
        })
    return objects

def send_raw(sock, type_byte, payload=b""):
    data = bytes([type_byte]) + payload
    sock.sendall(len(data).to_bytes(4, "little") + data)
    size = int.from_bytes(sock.recv(4), "little")
    return sock.recv(size)

def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((ADDRESS, PORT))
    return s

if __name__ == "__main__":
    # POST: register two objects
    objects_payload = (2).to_bytes(4, "little")
    objects_payload += pack_object(1, 0, (1.0, 2.0, 3.0), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))
    objects_payload += pack_object(2, 0, (4.0, 5.0, 6.0), (0.0, 0.1, 0.0), (2.0, 2.0, 2.0))

    sock = connect()
    resp = send_raw(sock, 1, objects_payload)  # 1 = POST
    sock.close()
    print("POST response:", resp.decode(errors="replace"))

    # GET: fetch interpolated state
    sock = connect()
    resp = send_raw(sock, 0)  # 0 = GET
    sock.close()
    objs = unpack_objects(resp)
    print(f"GET returned {len(objs)} object(s):")
    for o in objs:
        print(f"  id={o['id']} pos={o['pos']}")

    time.sleep(1.0)

    # update object 1
    objects_payload = (1).to_bytes(4, "little")
    objects_payload += pack_object(1, 0, (7.0, 8.0, 9.0), (0.0, 0.2, 0.0), (1.5, 1.5, 1.5))
    sock = connect()
    resp = send_raw(sock, 1, objects_payload)  # 1 = POST
    sock.close()
    print("POST response:", resp.decode(errors="replace"))

    # GET: fetch interpolated state
    sock = connect()
    resp = send_raw(sock, 0)  # 0 = GET
    sock.close()
    objs = unpack_objects(resp)
    print(f"GET returned {len(objs)} object(s):")
    for o in objs:
        print(f"  id={o['id']} pos={o['pos']}")

    # wait a bit and GET again to see interpolation
    time.sleep(1.0)
    sock = connect()
    resp = send_raw(sock, 0)  # 0 = GET
    sock.close()
    objs = unpack_objects(resp)
    print(f"GET after 1s returned {len(objs)} object(s):")
    for o in objs:
        print(f"  id={o['id']} pos={o['pos']}")

    # wait 5s to check if objects are removed after timeout
    time.sleep(5.0)
    sock = connect()
    resp = send_raw(sock, 0)  # 0 = GET
    sock.close()
    objs = unpack_objects(resp)
    print(f"GET after 5s returned {len(objs)} object(s):")
    for o in objs:
        print(f"  id={o['id']} pos={o['pos']}")

    # wait 5s to check if objects are removed after timeout
    time.sleep(5.0)
    sock = connect()
    resp = send_raw(sock, 0)  # 0 = GET
    sock.close()
    objs = unpack_objects(resp)
    print(f"GET after 5s returned {len(objs)} object(s):")
    for o in objs:
        print(f"  id={o['id']} pos={o['pos']}")

    # Benchmark
    times = []
    for _ in range(1024):
        t0 = time.time()
        s = connect()
        send_raw(s, 0)
        s.close()
        times.append(time.time() - t0)

    times.sort()
    n = len(times)
    print(f"\nBenchmark ({n} GET requests):")
    print(f"  avg    {sum(times)/n*1000:.2f} ms")
    print(f"  min    {times[0]*1000:.2f} ms")
    print(f"  max    {times[-1]*1000:.2f} ms")
    print(f"  median {times[n//2]*1000:.2f} ms")
    print(f"  p99    {times[int(n*0.99)]*1000:.2f} ms")

