"""Image comparison + visual evidence for the gengin optimizer.

Decodes the frame payloads the benchmark already produces (base64 raw BGRA
framebuffer dumps) and ordinary BMP/PNG files, computes per-pair metrics
(MSE, RMSE, PSNR, SSIM, abs-diff stats), and writes full-size side-by-side
composites for PR evidence:

    before | after | amplified diff (x4)

Everything is numpy + stdlib (zlib for PNG). Used by:
  - mcp_server.compare_bench_frames / compare_images (agent-facing tools)
  - main.makeBench(allow_visual_change=True) for the SSIM auto-restore gate
"""

import base64
import json
import os
import struct
import time
import zlib

import numpy as np

SSIM_WINDOW = 11
DIFF_AMPLIFY = 4
PIXEL_DIFF_THRESHOLD = 8

# ---------------------------------------------------------------------------
# decoding
# ---------------------------------------------------------------------------


def frames_from_b64(frame_images, width, height):
    """Decode a bench `frame_images` list into RGBA arrays."""
    out = []
    for b64 in frame_images:
        if not b64:
            continue
        raw = np.frombuffer(base64.b64decode(b64), dtype=np.uint8)
        expected = width * height * 4
        if raw.size != expected:
            raise ValueError(
                f"frame size mismatch: got {raw.size} bytes, expected {expected} "
                f"({width}x{height}x4)"
            )
        out.append(raw.reshape(height, width, 4)[:, :, [2, 1, 0, 3]].copy())
    return out


def _read_bmp(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"BM":
        raise ValueError(f"not a BMP: {path}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError(f"unsupported BMP header size {dib_size}: {path}")
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if compression != 0 or bpp not in (24, 32):
        raise ValueError(f"unsupported BMP (bpp={bpp}, compression={compression}): {path}")
    top_down = height < 0
    height = abs(height)
    stride = ((width * bpp + 31) // 32) * 4
    rows = np.frombuffer(
        data, dtype=np.uint8, count=stride * height, offset=pixel_offset
    ).reshape(height, stride)
    pixels = rows[:, : width * (bpp // 8)].reshape(height, width, bpp // 8)
    if not top_down:
        pixels = pixels[::-1]
    rgba = np.empty((height, width, 4), dtype=np.uint8)
    rgba[:, :, 0] = pixels[:, :, 2]
    rgba[:, :, 1] = pixels[:, :, 1]
    rgba[:, :, 2] = pixels[:, :, 0]
    rgba[:, :, 3] = pixels[:, :, 3] if bpp == 32 else 255
    return rgba


def _read_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos = 8
    idat = bytearray()
    width = height = depth = color_type = interlace = 0
    while pos + 8 <= len(data):
        length, tag = struct.unpack_from(">I4s", data, pos)
        payload = data[pos + 8: pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if depth != 8 or interlace != 0:
        raise ValueError(f"unsupported PNG (depth={depth}, interlace={interlace}): {path}")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"unsupported PNG color type {color_type}: {path}")
    raw = np.frombuffer(zlib.decompress(bytes(idat)), dtype=np.uint8)
    stride = width * channels
    raw = raw.reshape(height, stride + 1)
    filters = raw[:, 0]
    rows = raw[:, 1:].astype(np.uint8)
    out = np.empty((height, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    for y in range(height):
        f = filters[y]
        line = rows[y].copy()
        if f == 1:
            # Sub: per-channel prefix sum modulo 256 (vectorized).
            line = (np.cumsum(line.reshape(width, channels), axis=0,
                              dtype=np.uint32) % 256).astype(np.uint8).reshape(-1)
        elif f == 2:
            line = (line.astype(np.int16) + prev).astype(np.uint8)
        elif f == 3:
            for x in range(stride):
                left = int(line[x - channels]) if x >= channels else 0
                line[x] = (int(line[x]) + ((left + int(prev[x])) >> 1)) & 0xFF
        elif f == 4:
            line = _unfilter_paeth(line, prev, channels)
        out[y] = line
        prev = line
    pixels = out.reshape(height, width, channels)
    if channels == 1:
        rgba = np.repeat(pixels, 4, axis=2)
    elif channels == 2:
        rgba = np.concatenate([np.repeat(pixels[:, :, :1], 3, axis=2), pixels[:, :, 1:]], axis=2)
    elif channels == 3:
        rgba = np.concatenate([pixels, np.full((height, width, 1), 255, np.uint8)], axis=2)
    else:
        rgba = pixels
    return np.ascontiguousarray(rgba)


def _unfilter_paeth(line, prev, channels):
    out = np.empty_like(line)
    for x in range(len(line)):
        a = int(out[x - channels]) if x >= channels else 0
        b = int(prev[x])
        c = int(prev[x - channels]) if x >= channels else 0
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        out[x] = (int(line[x]) + pred) & 0xFF
    return out


def load_image(path):
    """BMP or PNG -> (h, w, 4) uint8 RGBA array."""
    with open(path, "rb") as fh:
        magic = fh.read(8)
    if magic[:2] == b"BM":
        return _read_bmp(path)
    if magic[:8] == b"\x89PNG\r\n\x1a\n":
        return _read_png(path)
    raise ValueError(f"unsupported image format: {path}")


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------


def _luma(rgba):
    rgb = rgba[:, :, :3].astype(np.float64)
    return 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]


def _box_filter(img, radius):
    h, w = img.shape
    k = 2 * radius + 1
    padded = np.pad(img, ((radius, radius), (radius, radius)), mode="edge")
    integral = np.zeros((padded.shape[0] + 1, padded.shape[1] + 1))
    integral[1:, 1:] = padded.cumsum(0).cumsum(1)
    ys = np.arange(h)[:, None]
    xs = np.arange(w)[None, :]
    total = (
        integral[ys + k, xs + k]
        - integral[ys, xs + k]
        - integral[ys + k, xs]
        + integral[ys, xs]
    )
    return total / (k * k)


def ssim(a, b):
    """Mean SSIM on luma (11x11 box window); inputs are RGBA uint8 arrays."""
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    x, y = _luma(a), _luma(b)
    radius = SSIM_WINDOW // 2
    mu_x, mu_y = _box_filter(x, radius), _box_filter(y, radius)
    xx = _box_filter(x * x, radius) - mu_x * mu_x
    yy = _box_filter(y * y, radius) - mu_y * mu_y
    xy = _box_filter(x * y, radius) - mu_x * mu_y
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    numerator = (2 * mu_x * mu_y + c1) * (2 * xy + c2)
    denominator = (mu_x ** 2 + mu_y ** 2 + c1) * (xx + yy + c2)
    return float(np.mean(numerator / denominator))


def metrics(a, b):
    """Per-pair metrics for two (h, w, 4) uint8 arrays."""
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    # RGB only: alpha carries no visual signal in the renderer and would make
    # BMP (opaque) vs RGBA comparisons noisy.
    diff = a[:, :, :3].astype(np.int16) - b[:, :, :3].astype(np.int16)
    abs_diff = np.abs(diff)
    mse = float(np.mean(abs_diff.astype(np.float64) ** 2))
    per_pixel = abs_diff.max(axis=2)
    result = {
        "width": int(a.shape[1]),
        "height": int(a.shape[0]),
        "mse": mse,
        "rmse": float(np.sqrt(mse)),
        "psnr": float("inf") if mse == 0.0 else float(10 * np.log10(255.0 ** 2 / mse)),
        "ssim": ssim(a, b),
        "mean_abs_diff": float(abs_diff.mean()),
        "max_abs_diff": int(abs_diff.max()),
        "pct_pixels_gt8": float((per_pixel > PIXEL_DIFF_THRESHOLD).mean() * 100.0),
    }
    if not np.isfinite(result["psnr"]):
        result["psnr"] = None
    return result


# ---------------------------------------------------------------------------
# PNG writer + composites
# ---------------------------------------------------------------------------


def write_png(path, rgba):
    height, width, _ = rgba.shape

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(height))
    body = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as fh:
        fh.write(body)


def composite(before, after):
    """before | after | amplified abs-diff, side by side."""
    diff = np.abs(
        after.astype(np.int16) - before.astype(np.int16)
    ).astype(np.uint8)
    diff = np.clip(diff.astype(np.uint16) * DIFF_AMPLIFY, 0, 255).astype(np.uint8)
    diff[:, :, 3] = 255
    return np.concatenate([before, after, diff], axis=1)


# ---------------------------------------------------------------------------
# high-level entry points
# ---------------------------------------------------------------------------


def compare_frames(baseline_frames, current_frames, out_dir, label,
                   context=None, hashes_equal=None):
    """Compare two bench frame lists; write composites + metrics.json.

    baseline_frames / current_frames: lists of (h, w, 4) RGBA arrays.
    hashes_equal: optional list of booleans (frame_hashes informational only).
    Returns the metrics summary dict.
    """
    if len(baseline_frames) != len(current_frames):
        raise ValueError(
            f"frame count mismatch: baseline {len(baseline_frames)} vs "
            f"current {len(current_frames)}"
        )
    if not baseline_frames:
        raise ValueError("no frames to compare")

    os.makedirs(out_dir, exist_ok=True)
    per_frame = []
    for index, (before, after) in enumerate(zip(baseline_frames, current_frames)):
        entry = metrics(before, after)
        entry["index"] = index
        if hashes_equal is not None and index < len(hashes_equal):
            entry["hashes_equal"] = bool(hashes_equal[index])
        name = f"frame_{index:02d}.png"
        write_png(os.path.join(out_dir, name), composite(before, after))
        entry["composite"] = name
        per_frame.append(entry)

    summary = {
        "label": label,
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "frames": len(per_frame),
        "min_ssim": min(f["ssim"] for f in per_frame),
        "max_mse": max(f["mse"] for f in per_frame),
        "mean_ssim": float(np.mean([f["ssim"] for f in per_frame])),
        "mean_psnr": float(np.mean([f["psnr"] for f in per_frame if f["psnr"] is not None]))
        if any(f["psnr"] is not None for f in per_frame) else None,
        "per_frame": per_frame,
        "context": context or {},
    }
    with open(os.path.join(out_dir, "metrics.json"), "w") as fh:
        json.dump(summary, fh, indent=2)
        fh.write("\n")
    return summary


def compare_files(before_path, after_path, out_dir, label, context=None):
    """Compare two image files (BMP/PNG); write a composite + metrics.json."""
    before = load_image(before_path)
    after = load_image(after_path)
    return compare_frames([before], [after], out_dir, label, context=context)
