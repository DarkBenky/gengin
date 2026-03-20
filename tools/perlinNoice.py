from noise import pnoise3
import ctypes
import numpy as np
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

def generate3DCloud(width, height, depth, scale, iterations, contrast=3.0):
    cloud = [[[0 for _ in range(depth)] for _ in range(height)] for _ in range(width)]
    for x in range(width):
        for y in range(height):
            for z in range(depth):
                cloud[x][y][z] = pnoise3(x * scale, y * scale, z * scale)
    # apply higher octaves noise for more detail
    for i in range(1, iterations):
        frequency = 2 ** i
        amplitude = 0.5 ** i
        for x in range(width):
            for y in range(height):
                for z in range(depth):
                    cloud[x][y][z] += amplitude * pnoise3(x * scale * frequency, y * scale * frequency, z * scale * frequency)
    # normalize to [0,1] then apply power curve to thin density
    max_value = max(max(max(row) for row in plane) for plane in cloud)
    min_value = min(min(min(row) for row in plane) for plane in cloud)
    for x in range(width):
        for y in range(height):
            for z in range(depth):
                n = (cloud[x][y][z] - min_value) / (max_value - min_value)
                cloud[x][y][z] = n ** contrast
    
    return cloud

def visualizeCloud(cloud, threshold=0.5):
    data = np.array(cloud)
    xs, ys, zs = np.where(data > threshold)
    densities = data[xs, ys, zs]

    w, h, d = data.shape
    fig = plt.figure(figsize=(10, 8))
    ax = fig.add_subplot(111, projection='3d')
    scatter = ax.scatter(xs, zs, ys, c=densities, cmap='Blues', alpha=0.3, s=1, vmin=threshold, vmax=1.0)
    ax.set_box_aspect([w, d, h])
    fig.colorbar(scatter, ax=ax, label='Density')
    ax.set_title(f'Cloud (threshold={threshold})')
    ax.set_xlabel('X')
    ax.set_ylabel('Z')
    ax.set_zlabel('Y (height)')
    plt.tight_layout()
    plt.show()


def saveObject(cloud, filename, threshold=0.5):
    with open(filename, "wb") as f:
        width = len(cloud)
        height = len(cloud[0])
        depth = len(cloud[0][0])
        f.write(ctypes.c_int(width))
        f.write(ctypes.c_int(height))
        f.write(ctypes.c_int(depth))
        for x in range(width):
            for y in range(height):
                for z in range(depth):
                    if cloud[x][y][z] > threshold:
                        f.write(ctypes.c_float(cloud[x][y][z]))
                    else:
                        f.write(ctypes.c_float(0.0))

def loadObject(filename):
    with open(filename, "rb") as f:
        width = ctypes.c_int.from_buffer_copy(f.read(4)).value
        height = ctypes.c_int.from_buffer_copy(f.read(4)).value
        depth = ctypes.c_int.from_buffer_copy(f.read(4)).value
        cloud = [[[0.0 for _ in range(depth)] for _ in range(height)] for _ in range(width)]
        for x in range(width):
            for y in range(height):
                for z in range(depth):
                    cloud[x][y][z] = ctypes.c_float.from_buffer_copy(f.read(4)).value
    return cloud

if __name__ == "__main__":
    x = 128
    y = 16
    z = 128
    scale = 0.08
    iterations = 4
    cloud = generate3DCloud(x, y, z, scale, iterations, contrast=1.85)
    saveObject(cloud, "../assets/models/cloud.bin", threshold=0.45)
    # visualizeCloud(cloud, threshold=0.45)