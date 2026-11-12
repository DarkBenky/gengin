"""Stdlib only .bin and BMP helpers for the model editor MCP tools.

This is a second implementation of the same format the page reads, kept small so the
MCP server has no dependency beyond Python and so the editor can be checked from the
outside (size equation, cell containment, BMP layout) without a browser.
"""

import hashlib
import struct

HEADER_SIZE = 20
LEGACY_HEADER_SIZE = 16
RECORD_SIZE = 108
OBSOLETE_RECORD_SIZE = 92
FLAG_TEXTURED = 1
FLAG_CULL_BACKFACES = 2
MIN_TEXTURE_SIZE = 256
MAX_TEXTURE_SIZE = 16384
LEGACY_TEXTURE_SIZE = 4096
BYTES_PER_TEXEL = 9
MAX_GRID = 64

# <3fI3fI3fI3fIfff3fI6HI> is the 108 byte record: positions, normal, three floats,
# color, six u16 UVs, one zero pad. '<' means no alignment padding is inserted.
RECORD = struct.Struct("<3fI3fI3fI3fIfff3fI6HI")
assert RECORD.size == RECORD_SIZE

# pad byte offsets inside a record, all of which the writer must leave at zero
PAD_OFFSETS = (12, 28, 44, 60, 88, 104)

BMP_FILE_HEADER = 14
BMP_INFO_HEADER = 40


class BinError(Exception):
    pass


def formatCount(value):
    return "{:,}".format(value)


def formatBytes(value):
    if value < 1024:
        return "{:.0f} B".format(value)
    if value < 1024 * 1024:
        return "{:.1f} KiB".format(value / 1024.0)
    return "{:.1f} MiB".format(value / (1024.0 * 1024.0))


def expectedCurrentSize(triangleCount, flags, textureSize):
    atlas = textureSize * textureSize * BYTES_PER_TEXEL if flags & FLAG_TEXTURED else 0
    return HEADER_SIZE + triangleCount * RECORD_SIZE + atlas


def expectedLegacySize(triangleCount, flags):
    atlas = LEGACY_TEXTURE_SIZE * LEGACY_TEXTURE_SIZE * BYTES_PER_TEXEL if flags & FLAG_TEXTURED else 0
    return LEGACY_HEADER_SIZE + triangleCount * RECORD_SIZE + atlas


def resolveLayout(fileSize, recordSize, triangleCount, flags, sizeField):
    """Mirror resolveLayout in binio.js (and LoadObj in the engine)."""
    textured = flags & FLAG_TEXTURED
    if recordSize == OBSOLETE_RECORD_SIZE:
        return {
            "ok": False,
            "error": "old/unsupported .bin: triangle records are {} bytes, only the 108 byte "
                     "layout is supported".format(recordSize),
        }
    if recordSize != RECORD_SIZE:
        return {
            "ok": False,
            "error": "not a gengin .bin: record size {} is neither the current 108 nor the "
                     "obsolete 92 byte layout".format(recordSize),
        }
    if MIN_TEXTURE_SIZE <= sizeField <= MAX_TEXTURE_SIZE:
        expected = expectedCurrentSize(triangleCount, flags, sizeField)
        if expected == fileSize:
            return {"ok": True, "headerSize": HEADER_SIZE, "textureSize": sizeField, "legacy": False}
        if textured:
            return {
                "ok": False,
                "error": "truncated or corrupt .bin: header says {} bytes but {} triangles plus "
                         "a {} atlas need {}".format(formatBytes(fileSize), formatCount(triangleCount),
                                                     sizeField, formatBytes(expected)),
            }
    legacyExpected = expectedLegacySize(triangleCount, flags)
    if legacyExpected == fileSize:
        return {"ok": True, "headerSize": LEGACY_HEADER_SIZE, "textureSize": LEGACY_TEXTURE_SIZE, "legacy": True}
    return {
        "ok": False,
        "error": "truncated or corrupt .bin: header says {} bytes but {} triangles need {} in the "
                 "legacy layout".format(formatBytes(fileSize), formatCount(triangleCount),
                                        formatBytes(legacyExpected)),
    }


def readFile(path):
    with open(path, "rb") as handle:
        return handle.read()


def sha256File(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def loadBin(path):
    """Return (data, info). Raises BinError with a user facing message on bad input."""
    data = readFile(path)
    if len(data) < HEADER_SIZE:
        raise BinError("not a model file: only {} bytes".format(len(data)))

    fileSize, recordSize, triangleCount, flags, sizeField = struct.unpack_from("<5I", data, 0)
    layout = resolveLayout(fileSize, recordSize, triangleCount, flags, sizeField)
    if not layout["ok"]:
        raise BinError(layout["error"])
    if len(data) != fileSize:
        raise BinError("truncated or corrupt .bin: file is {} bytes but the header declares {}".format(
            formatBytes(len(data)), formatBytes(fileSize)))

    textured = bool(flags & FLAG_TEXTURED)
    atlasBytes = layout["textureSize"] * layout["textureSize"] * BYTES_PER_TEXEL if textured else 0
    triangleBytes = triangleCount * RECORD_SIZE
    if layout["headerSize"] + atlasBytes + triangleBytes != len(data):
        raise BinError("truncated or corrupt .bin: data blocks do not fill the file")

    info = {
        "path": path,
        "fileSize": len(data),
        "declaredSize": fileSize,
        "recordSize": recordSize,
        "triangleCount": triangleCount,
        "flags": flags,
        "textured": textured,
        "cullBackfaces": bool(flags & FLAG_CULL_BACKFACES),
        "textureSize": layout["textureSize"] if textured else 0,
        "headerSize": layout["headerSize"],
        "legacy": layout["legacy"],
        "atlasOffset": layout["headerSize"],
        "atlasBytes": atlasBytes,
        "triangleOffset": layout["headerSize"] + atlasBytes,
        "sizeEquation": layout["headerSize"] + triangleCount * RECORD_SIZE + atlasBytes,
    }
    return data, info


def iterRecords(data, info):
    offset = info["triangleOffset"]
    for index in range(info["triangleCount"]):
        yield RECORD.unpack_from(data, offset + index * RECORD_SIZE)


def uvToTexel(uv, textureSize):
    return int(uv * (textureSize - 1) / 65535)


def scanModel(data, info):
    """Single pass over the records; everything the verify tools report comes from here."""
    stats = {
        "uvMin": [65535, 65535],
        "uvMax": [0, 0],
        "nonFinite": 0,
        "emissionMin": float("inf"),
        "emissionMax": float("-inf"),
        "roughnessMin": float("inf"),
        "roughnessMax": float("-inf"),
        "metallicMin": float("inf"),
        "metallicMax": float("-inf"),
        "nonzeroPad": 0,
        "zeroNormal": 0,
    }
    boxes = []
    for v1x, v1y, v1z, _p1, v2x, v2y, v2z, _p2, v3x, v3y, v3z, _p3, nx, ny, nz, _pn, rough, metal, emission, \
            cr, cg, cb, _pc, u1, v1, u2, v2, u3, v3, _pad in iterRecords(data, info):
        values = (v1x, v1y, v1z, v2x, v2y, v2z, v3x, v3y, v3z, nx, ny, nz, emission, cr, cg, cb)
        if not all(v == v and abs(v) != float("inf") for v in values):
            stats["nonFinite"] += 1
        if nx == 0.0 and ny == 0.0 and nz == 0.0:
            stats["zeroNormal"] += 1
        stats["emissionMin"] = min(stats["emissionMin"], emission)
        stats["emissionMax"] = max(stats["emissionMax"], emission)
        stats["roughnessMin"] = min(stats["roughnessMin"], rough)
        stats["roughnessMax"] = max(stats["roughnessMax"], rough)
        stats["metallicMin"] = min(stats["metallicMin"], metal)
        stats["metallicMax"] = max(stats["metallicMax"], metal)
        for u, v in ((u1, v1), (u2, v2), (u3, v3)):
            stats["uvMin"][0] = min(stats["uvMin"][0], u)
            stats["uvMin"][1] = min(stats["uvMin"][1], v)
            stats["uvMax"][0] = max(stats["uvMax"][0], u)
            stats["uvMax"][1] = max(stats["uvMax"][1], v)
        boxes.append((u1, v1, u2, v2, u3, v3))
    for offset in PAD_OFFSETS:
        start = info["triangleOffset"] + offset
        end = info["triangleOffset"] + info["triangleCount"] * RECORD_SIZE
        chunk = data[start:end:RECORD_SIZE]
        stats["nonzeroPad"] += sum(1 for byte in chunk if byte != 0)
    if info["triangleCount"] == 0:
        stats["emissionMin"] = stats["emissionMax"] = 0.0
        stats["roughnessMin"] = stats["roughnessMax"] = 0.0
        stats["metallicMin"] = stats["metallicMax"] = 0.0
    stats["boxes"] = boxes
    return stats


def cellOfUv(u, v, textureSize, grid):
    cell = textureSize // grid
    return uvToTexel(u, textureSize) // cell, uvToTexel(v, textureSize) // cell


def checkCells(stats, textureSize, grid):
    """Every triangle's texel box must land inside exactly one atlas cell."""
    if textureSize % grid != 0:
        raise BinError("grid {} does not divide atlas size {}".format(grid, textureSize))
    cell = textureSize // grid
    perCell = {}
    outside = 0
    straddling = 0
    for box in stats["boxes"]:
        xs = [uvToTexel(box[0], textureSize), uvToTexel(box[2], textureSize), uvToTexel(box[4], textureSize)]
        ys = [uvToTexel(box[1], textureSize), uvToTexel(box[3], textureSize), uvToTexel(box[5], textureSize)]
        cx0, cx1 = min(xs) // cell, max(xs) // cell
        cy0, cy1 = min(ys) // cell, max(ys) // cell
        if cx0 != cx1 or cy0 != cy1:
            straddling += 1
        for cx, cy in ((cx0, cy0),):
            perCell[(cx, cy)] = perCell.get((cx, cy), 0) + 1
        if cx0 < 0 or cy0 < 0 or cx1 >= grid or cy1 >= grid:
            outside += 1
    return {"grid": grid, "cell": cell, "straddling": straddling, "outside": outside,
            "occupiedCells": len(perCell), "perCell": perCell}


def atlasColor(data, info, texel):
    offset = info["atlasOffset"] + texel * 4
    return data[offset], data[offset + 1], data[offset + 2], data[offset + 3]


def atlasCellStats(data, info, grid):
    """Per cell color variety; a cell the importer never overlaid stays one flat color."""
    textureSize = info["textureSize"]
    if textureSize % grid != 0:
        raise BinError("grid {} does not divide atlas size {}".format(grid, textureSize))
    cell = textureSize // grid
    cells = []
    for cy in range(grid):
        for cx in range(grid):
            counts = {}
            step = max(1, cell // 32)
            sampled = 0
            for y in range(0, cell, step):
                rowBase = info["atlasOffset"] + ((cy * cell + y) * textureSize + cx * cell) * 4
                for x in range(0, cell, step):
                    offset = rowBase + x * 4
                    key = data[offset:offset + 3]
                    counts[key] = counts.get(key, 0) + 1
                    sampled += 1
            topColor, topCount = max(counts.items(), key=lambda item: item[1])
            cells.append({
                "cx": cx, "cy": cy,
                "distinctColors": len(counts),
                "flatFraction": round(topCount / float(sampled), 4),
                "topColor": list(topColor),
                "alphaAtCenter": data[info["atlasOffset"] + (((cy * cell + cell // 2) * textureSize) + cx * cell + cell // 2) * 4 + 3]
                if info["textured"] else None,
            })
    return {"grid": grid, "cell": cell, "cells": cells}


def writeBmp24(path, width, height, rgb):
    """14 byte file header + 40 byte info header, rows bottom up, BGR, 4 byte padded."""
    if len(rgb) != width * height * 3:
        raise BinError("pixel buffer is {} bytes, expected {}".format(len(rgb), width * height * 3))
    stride = (width * 3 + 3) & ~3
    pixelBytes = stride * height
    fileSize = BMP_FILE_HEADER + BMP_INFO_HEADER + pixelBytes
    out = bytearray(fileSize)
    struct.pack_into("<2sIHHI", out, 0, b"BM", fileSize, 0, 0, BMP_FILE_HEADER + BMP_INFO_HEADER)
    struct.pack_into("<IiiHHIIiiII", out, BMP_FILE_HEADER, BMP_INFO_HEADER, width, height, 1, 24, 0,
                     pixelBytes, 2835, 2835, 0, 0)
    base = BMP_FILE_HEADER + BMP_INFO_HEADER
    rowBytes = width * 3
    for y in range(height):
        src = (height - 1 - y) * rowBytes
        dst = base + y * stride
        for x in range(0, rowBytes, 3):
            out[dst + x] = rgb[src + x + 2]
            out[dst + x + 1] = rgb[src + x + 1]
            out[dst + x + 2] = rgb[src + x]
    with open(path, "wb") as handle:
        handle.write(out)
    return fileSize


def readBmp(path):
    data = readFile(path)
    if len(data) < BMP_FILE_HEADER + BMP_INFO_HEADER or data[0:2] != b"BM":
        raise BinError("not a BMP: missing BM signature")
    fileSize = struct.unpack_from("<I", data, 2)[0]
    pixelOffset = struct.unpack_from("<I", data, 10)[0]
    infoSize, width, height, planes, bits, compression = struct.unpack_from("<IiiHHI", data, BMP_FILE_HEADER)
    if infoSize < BMP_INFO_HEADER:
        raise BinError("unsupported BMP: info header is {} bytes".format(infoSize))
    if compression != 0:
        raise BinError("unsupported BMP: compression {} (only uncompressed is supported)".format(compression))
    if bits not in (24, 32):
        raise BinError("unsupported BMP: {} bits per pixel (only 24 and 32)".format(bits))
    if planes != 1:
        raise BinError("unsupported BMP: {} planes".format(planes))
    if width <= 0 or height == 0:
        raise BinError("unsupported BMP: size {}x{}".format(width, height))
    topDown = height < 0
    height = abs(height)
    stride = ((width * bits // 8) + 3) & ~3
    if pixelOffset + stride * (height - 1) + width * bits // 8 > len(data):
        raise BinError("truncated BMP: {:.1f} MiB of pixel data declared".format(
            stride * height / (1024.0 * 1024.0)))
    return {"path": path, "fileSize": len(data), "declaredSize": fileSize, "width": width, "height": height,
            "bitsPerPixel": bits, "stride": stride, "topDown": topDown, "pixelOffset": pixelOffset,
            "data": data}


def bmpPixel(bmp, x, y):
    bits = bmp["bitsPerPixel"]
    row = y if bmp["topDown"] else bmp["height"] - 1 - y
    offset = bmp["pixelOffset"] + row * bmp["stride"] + x * (bits // 8)
    data = bmp["data"]
    return data[offset + 2], data[offset + 1], data[offset]
