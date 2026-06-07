import numpy as np
from noise import pnoise3
import matplotlib.pyplot as plt
from scipy.ndimage import distance_transform_edt

_pnoise3v = np.vectorize(pnoise3)

def generate2DTexture(width, height, scale, iterations, contrast=3.0):
    xs = np.arange(width, dtype=float) * scale
    ys = np.arange(height, dtype=float) * scale
    X, Y = np.meshgrid(xs, ys, indexing='ij')
    texture = _pnoise3v(X, Y, 0.0)
    for i in range(1, iterations):
        f = 2 ** i
        texture += (0.5 ** i) * _pnoise3v(X * f, Y * f, 0.0)
    texture = (texture - texture.min()) / (texture.max() - texture.min())
    return texture ** contrast

def generate3DTexture(width, height, depth, scale, iterations, contrast=3.0):
    xs = np.arange(width, dtype=float) * scale
    ys = np.arange(height, dtype=float) * scale
    zs = np.arange(depth, dtype=float) * scale
    X, Y, Z = np.meshgrid(xs, ys, zs, indexing='ij')
    texture = _pnoise3v(X, Y, Z)
    for i in range(1, iterations):
        f = 2 ** i
        texture += (0.5 ** i) * _pnoise3v(X * f, Y * f, Z * f)
    texture = (texture - texture.min()) / (texture.max() - texture.min())
    return texture ** contrast

def calculateSDF(texture, inside_threshold=0.5):
    inside = texture > inside_threshold
    dist_out = distance_transform_edt(inside)
    dist_in  = distance_transform_edt(~inside)
    return dist_in - dist_out

def visualize(texture, sdf):
    fig, axes = plt.subplots(1, 2, figsize=(10, 5))
    im0 = axes[0].imshow(texture, cmap='gray')
    fig.colorbar(im0, ax=axes[0], label='Noise Value')
    axes[0].set_title('Perlin Noise Texture')
    im1 = axes[1].imshow(sdf, cmap='RdBu')
    fig.colorbar(im1, ax=axes[1], label='Signed Distance')
    axes[1].set_title('SDF')
    plt.tight_layout()
    return fig

def visualize3D(texture, sdf):
    mx, my, mz = texture.shape[0]//2, texture.shape[1]//2, texture.shape[2]//2
    slices = [
        (texture[:, :, mz], sdf[:, :, mz], 'XY (z=mid)'),
        (texture[:, my, :], sdf[:, my, :], 'XZ (y=mid)'),
        (texture[mx, :, :], sdf[mx, :, :], 'YZ (x=mid)'),
    ]
    fig, axes = plt.subplots(3, 2, figsize=(10, 12))
    for i, (st, ss, label) in enumerate(slices):
        im0 = axes[i][0].imshow(st, cmap='gray')
        fig.colorbar(im0, ax=axes[i][0], label='Noise Value')
        axes[i][0].set_title(f'Texture {label}')
        im1 = axes[i][1].imshow(ss, cmap='RdBu')
        fig.colorbar(im1, ax=axes[i][1], label='Signed Distance')
        axes[i][1].set_title(f'SDF {label}')
    plt.tight_layout()
    return fig

if __name__ == "__main__":
    scale = 0.005
    iterations = 8
    contrast = 3.0
    treshold = 0.5

    texture2d = generate2DTexture(512, 512, scale, iterations, contrast=contrast)
    sdf2d = calculateSDF(texture2d, inside_threshold=treshold)
    visualize(texture2d, sdf2d)

    texture3d = generate3DTexture(255, 255, 255, scale, iterations, contrast=contrast)
    sdf3d = calculateSDF(texture3d, inside_threshold=treshold)
    visualize3D(texture3d, sdf3d)

    plt.show()
