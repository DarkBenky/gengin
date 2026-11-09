"""Bake a per-texel emission map down to the engine's 6 x 16 x 16 face grids.

Mirrors CalculateFaceEmissions in object/object.c: six orthographic sweeps over the mesh
bounding box, one value per cell, where the value is the emission sampled at the surface
point the ray hits. Unlike the engine, which reads the per-material scalar, this samples the
emission BMP at the hit UV and multiplies by the per-triangle scalar.
"""

import json
import os
import struct

import binlib

# v2 header: magic, version, fileSize, resolution, components, faceCount, bbMin[3], bbMax[3]
# v1 files (no version or bounds) are still readable
FACE_MAGIC = 0x324D4546
FACE_ONE_MAGIC = 0x32464D46
FACE_MAGIC_V1 = 0x314D4546
FACE_ONE_MAGIC_V1 = 0x31464D46
FACE_VERSION = 2
FACE_HEADER = 48
FACE_HEADER_V1 = 20
FACE_ONE_HEADER_V1 = 16

# (name, uAxis, vAxis, wAxis, side) matching the faces table in CalculateFaceEmissions
FACES = (
    ("front", 0, 1, 2, "min"),
    ("back", 0, 1, 2, "max"),
    ("right", 2, 1, 0, "min"),
    ("left", 2, 1, 0, "max"),
    ("top", 0, 2, 1, "max"),
    ("bottom", 0, 2, 1, "min"),
)


def modelBounds(info, data=None):
    """Axis aligned bounds over every triangle corner."""
    data = data if data is not None else binlib.readFile(info["path"])
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for record in binlib.iterRecords(data, info):
        for corner in range(3):
            base = corner * 4
            for axis in range(3):
                value = record[base + axis]
                if value < lo[axis]:
                    lo[axis] = value
                if value > hi[axis]:
                    hi[axis] = value
    return lo, hi


def faceGrids(data, info, bmp, resolution, onProgress=None):
    """Return ({faceName: [[[r,g,b] * resolution] * resolution]}, usedScalar)."""
    lo, hi = modelBounds(info, data)
    size = info["textureSize"]
    # the spec's semantics are texelEmission * perTriangleEmissionScalar; a model whose scalars
    # are all zero (the usual case for imported geometry) would bake to nothing, so the scalar is
    # only applied when the file actually carries one
    scalarPeak = max((record[18] for record in binlib.iterRecords(data, info)), default=0.0)
    useScalar = scalarPeak > 0.0
    grids = {}
    coverage = {}
    texels = None
    if bmp is not None:
        if bmp["width"] != size or bmp["height"] != size:
            raise binlib.BinError("emission map is {}x{} but the atlas is {}x{}".format(
                bmp["width"], bmp["height"], size, size))
        texels = bmp

    for name, uA, vA, wA, side in FACES:
        uMin, uSize = lo[uA], hi[uA] - lo[uA]
        vMin, vSize = lo[vA], hi[vA] - lo[vA]
        nearestLow = side == "min"

        # depth of the closest surface found so far per cell, plus the sampled color; sweeps that
        # start at the max side look for the largest depth instead of the smallest
        depth = [(float("inf") if nearestLow else float("-inf"))] * (resolution * resolution)
        covered = [False] * (resolution * resolution)
        cells = [[[0.0, 0.0, 0.0] for _ in range(resolution)] for _ in range(resolution)]
        if uSize <= 0.0 or vSize <= 0.0:
            grids[name] = cells
            continue

        for record in binlib.iterRecords(data, info):
            p = [(record[0], record[1], record[2]),
                 (record[4], record[5], record[6]),
                 (record[8], record[9], record[10])]
            us = [corner[uA] for corner in p]
            vs = [corner[vA] for corner in p]
            ws = [corner[wA] for corner in p]

            # cell range the projected triangle can touch
            u0 = (min(us) - uMin) / uSize * resolution
            u1 = (max(us) - uMin) / uSize * resolution
            v0 = (min(vs) - vMin) / vSize * resolution
            v1 = (max(vs) - vMin) / vSize * resolution
            cu0 = max(0, int(u0))
            cu1 = min(resolution - 1, int(u1))
            cv0 = max(0, int(v0))
            cv1 = min(resolution - 1, int(v1))
            if cu1 < cu0 or cv1 < cv0:
                continue

            det = (vs[1] - vs[2]) * (us[0] - us[2]) + (us[2] - us[1]) * (vs[0] - vs[2])
            if det == 0.0:
                continue

            uvs = record[23:29]
            emission = record[18] if useScalar else 1.0
            for cv in range(cv0, cv1 + 1):
                cellV = vMin + (cv + 0.5) / resolution * vSize
                for cu in range(cu0, cu1 + 1):
                    cellU = uMin + (cu + 0.5) / resolution * uSize
                    w1 = ((vs[1] - vs[2]) * (cellU - us[2]) + (us[2] - us[1]) * (cellV - vs[2])) / det
                    if w1 < 0.0 or w1 > 1.0:
                        continue
                    w2 = ((vs[2] - vs[0]) * (cellU - us[2]) + (us[0] - us[2]) * (cellV - vs[2])) / det
                    if w2 < 0.0 or w1 + w2 > 1.0:
                        continue
                    w3 = 1.0 - w1 - w2
                    wDepth = ws[0] * w1 + ws[1] * w2 + ws[2] * w3
                    index = cv * resolution + cu
                    closer = wDepth < depth[index] if nearestLow else wDepth > depth[index]
                    if not closer:
                        continue
                    depth[index] = wDepth
                    covered[index] = True

                    color = [record[19], record[20], record[21]]
                    if texels is not None:
                        u16 = w1 * uvs[0] + w2 * uvs[2] + w3 * uvs[4]
                        v16 = w1 * uvs[1] + w2 * uvs[3] + w3 * uvs[5]
                        tx = min(size - 1, max(0, int(u16 * (size - 1) / 65535.0)))
                        ty = min(size - 1, max(0, int(v16 * (size - 1) / 65535.0)))
                        r, g, b = binlib.bmpPixel(texels, tx, ty)
                        color = [r / 255.0, g / 255.0, b / 255.0]
                    cells[cv][cu] = [color[0] * emission, color[1] * emission, color[2] * emission]
        grids[name] = cells
        coverage[name] = sum(1 for flag in covered if flag)
        if onProgress:
            onProgress(name)
    return grids, useScalar, coverage, (lo, hi)

def summarise(grids, resolution, useScalar=True, coverage=None):
    lit = 0
    peak = 0.0
    for cells in grids.values():
        for row in cells:
            for cell in row:
                value = max(cell)
                if value > 0.0:
                    lit += 1
                if value > peak:
                    peak = value
    summary = {"resolution": resolution, "cellsPerFace": resolution * resolution,
               "faces": len(grids), "litCells": lit, "peak": round(peak, 4),
               "usedTriangleScalar": useScalar}
    if coverage is not None:
        summary["coveredCells"] = sum(coverage.values())
        summary["coveredPerFace"] = coverage
    return summary


def writeGrids(path, grids):
    with open(path, "w") as handle:
        json.dump(grids, handle)
    return os.path.getsize(path)


def faceHeader(magic, fileSize, resolution, faceCount, bounds):
    lo, hi = bounds
    return struct.pack("<6I6f", magic, FACE_VERSION, fileSize, resolution, 3, faceCount,
                       lo[0], lo[1], lo[2], hi[0], hi[1], hi[2])


def writeFaceBin(path, grids, resolution, bounds):
    """All six faces in one file: header, then face, row (top first), cell RGB as float32."""
    faceCount = len(grids)
    body = bytearray()
    for cells in grids.values():
        for row in cells:
            for cell in row:
                body += struct.pack("<3f", cell[0], cell[1], cell[2])
    total = FACE_HEADER + len(body)
    with open(path, "wb") as handle:
        handle.write(faceHeader(FACE_MAGIC, total, resolution, faceCount, bounds))
        handle.write(body)
    return total


def writeFaceBins(outDir, grids, resolution, bounds):
    """One binary per face; the payload is exactly EmissionMap.emissionMap."""
    os.makedirs(outDir, exist_ok=True)
    paths = []
    for name, cells in grids.items():
        body = bytearray()
        for row in cells:
            for cell in row:
                body += struct.pack("<3f", cell[0], cell[1], cell[2])
        total = FACE_HEADER + len(body)
        path = os.path.join(outDir, name + ".bin")
        with open(path, "wb") as handle:
            handle.write(faceHeader(FACE_ONE_MAGIC, total, resolution, 1, bounds))
            handle.write(body)
        paths.append(path)
    return paths


def readFaceBin(path):
    """Read a face map file, single face or bundle, v1 or v2, and return (header, floats)."""
    data = binlib.readFile(path)
    if len(data) < FACE_ONE_HEADER_V1:
        raise binlib.BinError("not a face map file: only {} bytes".format(len(data)))
    magic = struct.unpack_from("<I", data, 0)[0]
    bounds = None
    if magic in (FACE_ONE_MAGIC, FACE_MAGIC):
        _, version, total, resolution, components, faceCount = struct.unpack_from("<6I", data, 0)
        lo = struct.unpack_from("<3f", data, 24)
        hi = struct.unpack_from("<3f", data, 36)
        bounds = (list(lo), list(hi))
        headerSize = FACE_HEADER
        if version != FACE_VERSION:
            raise binlib.BinError("unsupported face map version {}".format(version))
    elif magic == FACE_ONE_MAGIC_V1:
        _, total, resolution, components = struct.unpack_from("<4I", data, 0)
        faceCount = 1
        headerSize = FACE_ONE_HEADER_V1
    elif magic == FACE_MAGIC_V1:
        _, total, resolution, faceCount, components = struct.unpack_from("<5I", data, 0)
        headerSize = FACE_HEADER_V1
    else:
        raise binlib.BinError("not a face map file: bad magic 0x{:08x}".format(magic))
    if components != 3:
        raise binlib.BinError("unsupported component count {}".format(components))
    expected = headerSize + faceCount * resolution * resolution * components * 4
    if total != expected or len(data) != expected:
        raise binlib.BinError("truncated or corrupt face map: header says {} bytes, expected {}".format(
            total, expected))
    count = faceCount * resolution * resolution * components
    values = struct.unpack_from("<{}f".format(count), data, headerSize)
    header = {"resolution": resolution, "faceCount": faceCount, "components": components,
              "fileSize": total, "singleFace": magic in (FACE_ONE_MAGIC, FACE_ONE_MAGIC_V1),
              "version": FACE_VERSION if bounds is not None else 1,
              "bounds": {"min": bounds[0], "max": bounds[1]} if bounds else None}
    return header, values
