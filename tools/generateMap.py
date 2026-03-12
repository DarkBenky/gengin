import ctypes
from math import sqrt
import os
import struct
import numpy as np
from scipy.ndimage import gaussian_filter
from noise import pnoise2
import matplotlib.pyplot as plt

NUM_OF_TRIANGLES = 75_000
GRID_SIZE = int(sqrt(NUM_OF_TRIANGLES / 2)) + 1
WIDTH = GRID_SIZE
HEIGHT = GRID_SIZE

SEED             = 42
COLOR_BRIGHTNESS = 1.85  # multiply all biome colors by this (1.0 = unchanged)

# terrain generation params
BASE_SCALE   = 0.0175   # base frequency — lower = larger continent features
OCTAVES      = 16       # detail layers
PERSISTENCE  = 0.5     # amplitude falloff per octave
LACUNARITY   = 2.0     # frequency growth per octave
HEIGHT_POWER = 4.25     # exponent > 1 flattens lowlands, sharpens peaks
SEA_LEVEL    = -0.75     # values below this are flat water

# hydraulic erosion params
EROSION_DROPS      = 25_000  # number of water droplets
EROSION_RADIUS     = 3        # brush radius for deposit/erode
EROSION_INERTIA    = 0.025     # how much droplet keeps its direction (0=always downhill)
SEDIMENT_CAPACITY  = 4.0      # max sediment a droplet carries relative to speed*slope
ERODE_SPEED        = 0.3
DEPOSIT_SPEED      = 0.3
EVAPORATE_SPEED    = 0.01
GRAVITY            = 8.0
MAX_STEPS          = 64       # max steps per droplet before evaporation

class Triangle(ctypes.Structure):
    _fields_ = [
        ("v1x", ctypes.c_float),
        ("v1y", ctypes.c_float),
        ("v1z", ctypes.c_float),
        ("v1w", ctypes.c_float),

        ("v2x", ctypes.c_float),
        ("v2y", ctypes.c_float),
        ("v2z", ctypes.c_float),
        ("v2w", ctypes.c_float),

        ("v3x", ctypes.c_float),
        ("v3y", ctypes.c_float),
        ("v3z", ctypes.c_float),
        ("v3w", ctypes.c_float),

        ("normalx", ctypes.c_float),
        ("normaly", ctypes.c_float),
        ("normalz", ctypes.c_float),
        ("normalw", ctypes.c_float),

        ("roughness", ctypes.c_float),
        ("metallic", ctypes.c_float),
        ("emission", ctypes.c_float),

        ("colorR", ctypes.c_float),
        ("colorG", ctypes.c_float),
        ("colorB", ctypes.c_float),
        ("colorW", ctypes.c_float),
    ]

def _bilinear_sample(map, x, y):
    """Sample height and gradient at a float (x, y) using bilinear interpolation."""
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, map.shape[1] - 1), min(y0 + 1, map.shape[0] - 1)
    u, v = x - x0, y - y0

    h00 = map[y0, x0]
    h10 = map[y0, x1]
    h01 = map[y1, x0]
    h11 = map[y1, x1]

    height = h00*(1-u)*(1-v) + h10*u*(1-v) + h01*(1-u)*v + h11*u*v
    gx = (h10 - h00)*(1-v) + (h11 - h01)*v  # slope in x
    gy = (h01 - h00)*(1-u) + (h11 - h10)*u  # slope in y
    return height, gx, gy


def _build_brush(radius):
    """Precompute offsets and weights for a circular deposit/erode brush."""
    offsets, weights = [], []
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            d = (dx*dx + dy*dy) ** 0.5
            if d < radius:
                w = 1.0 - d / radius
                offsets.append((dx, dy))
                weights.append(w)
    total = sum(weights)
    weights = [w / total for w in weights]
    return offsets, weights


def hydraulic_erosion(map):
    rng = np.random.default_rng(SEED)
    H, W = map.shape
    brush_offsets, brush_weights = _build_brush(EROSION_RADIUS)

    for _ in range(EROSION_DROPS):
        px = rng.uniform(EROSION_RADIUS, W - 1 - EROSION_RADIUS)
        py = rng.uniform(EROSION_RADIUS, H - 1 - EROSION_RADIUS)
        dx, dy = 0.0, 0.0
        speed = 1.0
        water = 1.0
        sediment = 0.0

        for _ in range(MAX_STEPS):
            height, gx, gy = _bilinear_sample(map, px, py)

            # update direction: blend inertia with gradient
            dx = dx * EROSION_INERTIA - gx * (1 - EROSION_INERTIA)
            dy = dy * EROSION_INERTIA - gy * (1 - EROSION_INERTIA)

            length = (dx*dx + dy*dy) ** 0.5
            if length < 1e-6:
                dx = rng.uniform(-1, 1)
                dy = rng.uniform(-1, 1)
                length = (dx*dx + dy*dy) ** 0.5
            dx /= length
            dy /= length

            nx, ny = px + dx, py + dy
            if nx < 0 or nx >= W - 1 or ny < 0 or ny >= H - 1:
                break

            new_height, _, _ = _bilinear_sample(map, nx, ny)
            delta_h = new_height - height

            # sediment capacity depends on speed, water, and downslope steepness
            capacity = max(-delta_h * speed * water * SEDIMENT_CAPACITY, 0.01)

            if sediment > capacity or delta_h > 0:
                # deposit: either overfull or going uphill
                deposit = (delta_h > 0) and (min(delta_h, sediment)) or ((sediment - capacity) * DEPOSIT_SPEED)
                deposit = max(deposit, 0.0)
                sediment -= deposit
                ix, iy = int(px), int(py)
                for (ox, oy), w in zip(brush_offsets, brush_weights):
                    bx, by = ix + ox, iy + oy
                    if 0 <= bx < W and 0 <= by < H:
                        map[by, bx] += deposit * w
            else:
                # erode
                erode = min((capacity - sediment) * ERODE_SPEED, -delta_h)
                erode = max(erode, 0.0)
                sediment += erode
                ix, iy = int(px), int(py)
                for (ox, oy), w in zip(brush_offsets, brush_weights):
                    bx, by = ix + ox, iy + oy
                    if 0 <= bx < W and 0 <= by < H:
                        map[by, bx] -= erode * w

            speed = max((max(speed*speed + delta_h * GRAVITY, 0.0)) ** 0.5, 0.0)
            water *= (1 - EVAPORATE_SPEED)
            if water < 0.01:
                break
            if nx != nx or ny != ny:  # NaN check
                break
            px, py = nx, ny

    return map


def generate_map():
    Map = np.zeros((HEIGHT, WIDTH), dtype=np.float32)

    for y in range(HEIGHT):
        for x in range(WIDTH):
            # fBm: accumulate octaves with falling amplitude and rising frequency
            value = 0.0
            amplitude = 1.0
            frequency = 1.0
            max_val = 0.0
            for _ in range(OCTAVES):
                value    += pnoise2(x * BASE_SCALE * frequency,
                                    y * BASE_SCALE * frequency,
                                    base=SEED) * amplitude
                max_val  += amplitude
                amplitude *= PERSISTENCE
                frequency *= LACUNARITY
            value /= max_val             # normalize to [-1, 1]
            value  = (value + 1.0) * 0.5 # remap to [0, 1]

            # redistribute: flatten sea floor, sharpen mountains
            if value < SEA_LEVEL:
                value = SEA_LEVEL * 0.3  # flat shallow sea
            else:
                value = pow((value - SEA_LEVEL) / (1.0 - SEA_LEVEL), HEIGHT_POWER)

            Map[y][x] = value

    print(f"Running hydraulic erosion ({EROSION_DROPS} drops)...")
    hydraulic_erosion(Map)
    # re-normalize after erosion
    Map -= Map.min()
    Map /= Map.max()

    # smooth water areas so the flat surface is perfectly even
    water_mask = Map < WATER_H
    smoothed = gaussian_filter(Map, sigma=4)
    Map = np.where(water_mask, smoothed, Map)
    Map = np.clip(Map, 0.0, 1.0)

    # build RGB preview using the same material colors as the mesh
    preview = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            color, _, _, _ = _height_to_material(float(Map[y, x]))
            preview[y, x] = color
    preview = np.clip(preview, 0.0, 1.0)

    plt.imshow(preview)
    plt.title('Generated Terrain')
    plt.axis('off')
    plt.show()
    return Map

WORLD_SIZE_X  = 200.0   # world-space width of the terrain
WORLD_SIZE_Z  = 200.0   # world-space depth
HEIGHT_SCALE  = 20.0    # max height in world units
BASE_Y        = -1.25   # y offset (matches floor level in scene)
CENTER_X      = 0.0
CENTER_Z      = 35.0    # roughly center of the plane grid
WATER_H       = 0.05    # normalized height threshold below which terrain is flat water


def _cross_normal(v1, v2, v3):
    ax, ay, az = v2[0]-v1[0], v2[1]-v1[1], v2[2]-v1[2]
    bx, by, bz = v3[0]-v1[0], v3[1]-v1[1], v3[2]-v1[2]
    nx = ay*bz - az*by
    ny = az*bx - ax*bz
    nz = ax*by - ay*bx
    length = (nx*nx + ny*ny + nz*nz) ** 0.5
    if length < 1e-8:
        return (0.0, 1.0, 0.0)
    return (nx/length, ny/length, nz/length)


def _lerp(a, b, t):
    return a + (b - a) * t

def _lerp3(a, b, t):
    return (_lerp(a[0], b[0], t), _lerp(a[1], b[1], t), _lerp(a[2], b[2], t))

def _smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)

def _height_to_material(h):
    """Smooth biome transitions using blended zones."""
    # biome anchor points: (height, color, roughness, metallic)
    WATER = ((0.18, 0.52, 0.78), 0.10, 0.15)
    SAND  = ((0.92, 0.82, 0.52), 0.95, 0.01)
    GRASS = ((0.28, 0.72, 0.18), 0.87, 0.01)
    DIRT  = ((0.62, 0.52, 0.36), 0.83, 0.02)
    ROCK  = ((0.68, 0.65, 0.60), 0.80, 0.05)
    SNOW  = ((0.95, 0.97, 1.00), 0.25, 0.0)

    # blend zones: (start, end, from_biome, to_biome)
    zones = [
        (0.00, 0.08, WATER, WATER),
        (0.08, 0.14, WATER, SAND),
        (0.14, 0.22, SAND,  GRASS),
        (0.22, 0.50, GRASS, GRASS),
        (0.50, 0.62, GRASS, DIRT),
        (0.62, 0.74, DIRT,  ROCK),
        (0.74, 0.88, ROCK,  SNOW),
        (0.88, 1.00, SNOW,  SNOW),
    ]

    for start, end, a, b in zones:
        if h <= end:
            t = _smoothstep(start, end, h)
            color = _lerp3(a[0], b[0], t)
            color = tuple(min(c * COLOR_BRIGHTNESS, 1.0) for c in color)
            roughness = _lerp(a[1], b[1], t)
            metallic  = _lerp(a[2], b[2], t)
            return color, roughness, metallic, 0.0

    snow = tuple(min(c * COLOR_BRIGHTNESS, 1.0) for c in SNOW[0])
    return snow, SNOW[1], SNOW[2], 0.0


def save_map_to_bin(filename, Map):
    H, W = Map.shape
    cell_x = WORLD_SIZE_X / (W - 1)
    cell_z = WORLD_SIZE_Z / (H - 1)

    TRI_STRUCT_SIZE = 5*16 + 3*4   # 92 bytes — matches Go writeFile
    num_tris = (W - 1) * (H - 1) * 2
    file_size = 8 + num_tris * TRI_STRUCT_SIZE

    def pack_tri(v1, v2, v3, color, roughness, metallic, emission):
        n = _cross_normal(v1, v2, v3)
        return struct.pack('<23f',
            v1[0], v1[1], v1[2], 0.0,
            v2[0], v2[1], v2[2], 0.0,
            v3[0], v3[1], v3[2], 0.0,
            n[0],  n[1],  n[2],  0.0,
            roughness, metallic, emission,
            color[0], color[1], color[2], 0.0,
        )

    with open(filename, 'wb') as f:
        f.write(struct.pack('<II', file_size, TRI_STRUCT_SIZE))
        for y in range(H - 1):
            for x in range(W - 1):
                wx = CENTER_X - WORLD_SIZE_X * 0.5 + x * cell_x
                wz = CENTER_Z - WORLD_SIZE_Z * 0.5 + y * cell_z

                h00 = float(Map[y,   x  ])
                h10 = float(Map[y,   x+1])
                h01 = float(Map[y+1, x  ])
                h11 = float(Map[y+1, x+1])

                # water is clamped flat at BASE_Y
                def wy(h):
                    return BASE_Y if h < WATER_H else BASE_Y + h * HEIGHT_SCALE

                p00 = (wx,          wy(h00), wz)
                p10 = (wx + cell_x, wy(h10), wz)
                p01 = (wx,          wy(h01), wz + cell_z)
                p11 = (wx + cell_x, wy(h11), wz + cell_z)

                avg_h = (h00 + h10 + h01 + h11) * 0.25
                color, roughness, metallic, emission = _height_to_material(avg_h)

                f.write(pack_tri(p00, p10, p01, color, roughness, metallic, emission))
                f.write(pack_tri(p10, p11, p01, color, roughness, metallic, emission))

    print(f"Saved {num_tris} triangles ({file_size} bytes) to {filename}")

if __name__ == "__main__":
    Map = generate_map()
    out = os.path.join(os.path.dirname(__file__), "..", "assets", "models", "map.bin")
    save_map_to_bin(out, Map)
    print("map generation complete")
