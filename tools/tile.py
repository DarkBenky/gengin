import sys
import os
import numpy as np
from PIL import Image

DEFAULT_DST = "../assets/tiles/"

def usage(error=False):
    print(
        "usage: tile.py [-h] <from_path> [dst_path] <tile_size | tile_w tile_h>\n"
        "\n"
        "  from_path   source image (png, jpg, ...)\n"
        "  dst_path    output directory  (default: ../assets/tiles/)\n"
        "  tile_size   single value used for both width and height\n"
        "  tile_w/h    explicit width then height\n"
        "\n"
        "output: one .bin per tile, row-major R G B 0 uint8 per pixel"
    )
    sys.exit(1 if error else 0)

if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or "-h" in args:
        usage()
    if len(args) < 2:
        usage(error=True)

    from_path = args[0]
    args = args[1:]

    dst = DEFAULT_DST
    if args and not args[0].lstrip("-").isdigit():
        dst = args[0]
        args = args[1:]

    if len(args) == 1:
        tile_w = tile_h = int(args[0])
    elif len(args) == 2:
        tile_w, tile_h = int(args[0]), int(args[1])
    else:
        usage()

    name = os.path.splitext(os.path.basename(from_path))[0]
    dst = os.path.join(dst, name)
    os.makedirs(dst, exist_ok=True)

    img = Image.open(from_path).convert("RGB")
    img_data = np.array(img)  # shape: (H, W, 3)

    tile_count_x = img.width  // tile_w
    tile_count_y = img.height // tile_h

    pad = np.zeros((tile_h, tile_w, 1), dtype=np.uint8)

    for ty in range(tile_count_y):
        for tx in range(tile_count_x):
            tile_rgb = img_data[
                ty * tile_h:(ty + 1) * tile_h,
                tx * tile_w:(tx + 1) * tile_w
            ]
            # pack as R G B 0 — flat array of uint8
            tile_rgba = np.concatenate([tile_rgb, pad], axis=2)
            out_path = os.path.join(dst, f"tile_{ty}_{tx}.bin")
            tile_rgba.astype(np.uint8).tofile(out_path)
