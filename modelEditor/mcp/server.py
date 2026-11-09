"""gengin modelEditor MCP server (stdio).

Lets a model inspect and validate gengin .bin models and emission BMPs without a
browser: header/layout checks, the size equation, atlas cell containment, legacy to
current conversion, atlas export and BMP readback. Pure stdlib except the MCP SDK.

Run: python3 modelEditor/mcp/server.py
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    print("mcp package missing: pip install 'mcp>=1.27,<2' (llmOpt/requirements-mcp.txt)", file=sys.stderr)
    raise

import binlib
import emissionfaces


mcp = FastMCP("gengin-model-editor")


def fail(error):
    return {"ok": False, "error": str(error)}


def unknownGridError(textureSize):
    return "model has no texture block, grid checks need a textured model (atlas {})".format(textureSize)


@mcp.tool()
def bin_info(path: str) -> dict:
    """Read a .bin header: layout (current or legacy), triangle count, flags, atlas size,
    the size equation result and the SHA-256. Returns ok=false with a message for corrupt,
    truncated or old record layouts instead of raising."""
    try:
        data, info = binlib.loadBin(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    return {
        "ok": True,
        "path": path,
        "layout": "legacy" if info["legacy"] else "current",
        "headerSize": info["headerSize"],
        "recordSize": info["recordSize"],
        "triangleCount": info["triangleCount"],
        "flags": info["flags"],
        "textured": info["textured"],
        "cullBackfaces": info["cullBackfaces"],
        "textureSize": info["textureSize"],
        "fileSize": info["fileSize"],
        "sizeEquation": info["sizeEquation"],
        "sha256": binlib.sha256File(path),
    }


@mcp.tool()
def bin_verify(path: str, grid: int = 0) -> dict:
    """Full pass over every triangle record: non finite values, zero normals, non zero
    padding, roughness/metallic/emission ranges, UV range in 0..65535, and when grid > 0
    that each triangle stays inside one atlas cell and how many cells are occupied."""
    try:
        data, info = binlib.loadBin(path)
        stats = binlib.scanModel(data, info)
    except (binlib.BinError, OSError) as error:
        return fail(error)

    result = {
        "ok": True,
        "path": path,
        "triangleCount": info["triangleCount"],
        "textureSize": info["textureSize"],
        "uvMin": stats["uvMin"],
        "uvMax": stats["uvMax"],
        "nonFiniteValues": stats["nonFinite"],
        "zeroNormals": stats["zeroNormal"],
        "nonZeroPadBytes": stats["nonzeroPad"],
        "roughnessRange": [stats["roughnessMin"], stats["roughnessMax"]],
        "metallicRange": [stats["metallicMin"], stats["metallicMax"]],
        "emissionRange": [stats["emissionMin"], stats["emissionMax"]],
    }
    if info["triangleCount"] == 0:
        result["ok"] = False
        result["error"] = "model has no triangles"
        return result
    if grid > 0:
        if not info["textured"]:
            result["ok"] = False
            result["error"] = unknownGridError(info["textureSize"])
            return result
        try:
            cells = binlib.checkCells(stats, info["textureSize"], grid)
        except binlib.BinError as error:
            return fail(error)
        result["cells"] = {
            "grid": cells["grid"],
            "cell": cells["cell"],
            "trianglesStraddlingCells": cells["straddling"],
            "trianglesOutsideAtlas": cells["outside"],
            "occupiedCells": cells["occupiedCells"],
        }
    return result


@mcp.tool()
def bin_atlas_stats(path: str, grid: int = 0) -> dict:
    """Sample the color map per atlas cell. A cell the importer never filled stays a single
    flat color, so flatFraction near 1.0 with distinctColors 1 flags an empty cell. grid 0
    treats the whole atlas as one cell."""
    try:
        data, info = binlib.loadBin(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if not info["textured"]:
        return fail(unknownGridError(info["textureSize"]))
    grid = grid or 1
    try:
        stats = binlib.atlasCellStats(data, info, grid)
    except binlib.BinError as error:
        return fail(error)
    empty = [cell for cell in stats["cells"] if cell["distinctColors"] == 1]
    return {
        "ok": True,
        "path": path,
        "textureSize": info["textureSize"],
        "grid": stats["grid"],
        "cell": stats["cell"],
        "uniformCells": len(empty),
        "cells": stats["cells"],
    }


@mcp.tool()
def bin_convert(in_path: str, out_path: str) -> dict:
    """Rewrite any supported .bin in the current 20 byte header layout without touching the
    atlas or triangle bytes (legacy files gain the 4 byte header field and a corrected
    fileSize). Refuses to overwrite the input."""
    try:
        data, info = binlib.loadBin(in_path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if os.path.abspath(in_path) == os.path.abspath(out_path):
        return fail("refusing to overwrite the input file")
    flags = info["flags"]
    fileSize = binlib.expectedCurrentSize(info["triangleCount"], flags, info["textureSize"])
    header = struct.pack("<5I", fileSize, binlib.RECORD_SIZE, info["triangleCount"], flags, info["textureSize"])
    with open(out_path, "wb") as handle:
        handle.write(header)
        handle.write(data[info["atlasOffset"]:])
    try:
        _, written = binlib.loadBin(out_path)
    except (binlib.BinError, OSError) as error:
        return fail("output failed verification: {}".format(error))
    return {
        "ok": True,
        "inPath": in_path,
        "outPath": out_path,
        "fromLayout": "legacy" if info["legacy"] else "current",
        "triangleCount": written["triangleCount"],
        "textureSize": written["textureSize"],
        "fileSize": written["fileSize"],
        "sha256": binlib.sha256File(out_path),
    }


@mcp.tool()
def bin_atlas_export(path: str, out_path: str) -> dict:
    """Write the color map of a textured model as a 24 bit BMP (alpha dropped) so the atlas
    can be inspected with an image viewer or compared with the source textures."""
    try:
        data, info = binlib.loadBin(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if not info["textured"]:
        return fail(unknownGridError(info["textureSize"]))
    size = info["textureSize"]
    rgb = bytearray(size * size * 3)
    base = info["atlasOffset"]
    for texel in range(size * size):
        rgb[texel * 3:texel * 3 + 3] = data[base + texel * 4:base + texel * 4 + 3]
    binlib.writeBmp24(out_path, size, size, rgb)
    return {"ok": True, "path": path, "outPath": out_path, "width": size, "height": size,
            "fileSize": os.path.getsize(out_path)}


@mcp.tool()
def bmp_info(path: str) -> dict:
    """BMP header details for an emission map: dimensions, bits per pixel, compression,
    row stride, top down flag and file size."""
    try:
        bmp = binlib.readBmp(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    return {"ok": True, "path": path, "width": bmp["width"], "height": bmp["height"],
            "bitsPerPixel": bmp["bitsPerPixel"], "stride": bmp["stride"], "topDown": bmp["topDown"],
            "fileSize": bmp["fileSize"], "declaredSize": bmp["declaredSize"],
            "sha256": binlib.sha256File(path)}


@mcp.tool()
def bmp_sample(path: str, points: list) -> dict:
    """Read RGB values at the given [[x, y], ...] texel positions (origin top left)."""
    try:
        bmp = binlib.readBmp(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    samples = []
    for point in points:
        x, y = int(point[0]), int(point[1])
        if x < 0 or y < 0 or x >= bmp["width"] or y >= bmp["height"]:
            samples.append({"x": x, "y": y, "error": "outside the image"})
            continue
        r, g, b = binlib.bmpPixel(bmp, x, y)
        samples.append({"x": x, "y": y, "rgb": [r, g, b]})
    return {"ok": True, "path": path, "width": bmp["width"], "height": bmp["height"], "samples": samples}


@mcp.tool()
def bmp_compare(path_a: str, path_b: str) -> dict:
    """Compare two BMPs pixel by pixel; reports dimensions, differing pixel count and the
    first few differences."""
    try:
        a = binlib.readBmp(path_a)
        b = binlib.readBmp(path_b)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if a["width"] != b["width"] or a["height"] != b["height"]:
        return {"ok": True, "sameSize": False, "a": [a["width"], a["height"]], "b": [b["width"], b["height"]]}
    differences = []
    differing = 0
    for y in range(a["height"]):
        for x in range(a["width"]):
            if binlib.bmpPixel(a, x, y) != binlib.bmpPixel(b, x, y):
                differing += 1
                if len(differences) < 5:
                    differences.append({"x": x, "y": y, "a": list(binlib.bmpPixel(a, x, y)),
                                        "b": list(binlib.bmpPixel(b, x, y))})
    return {"ok": True, "sameSize": True, "width": a["width"], "height": a["height"],
            "differingPixels": differing, "firstDifferences": differences}


@mcp.tool()
def emission_blank(atlas_size: int, out_path: str) -> dict:
    """Write an all black emission BMP of atlas_size x atlas_size, the template a painted
    emission map starts from (black means no emission)."""
    if atlas_size < binlib.MIN_TEXTURE_SIZE or atlas_size > binlib.MAX_TEXTURE_SIZE:
        return fail("atlas size must be between {} and {}".format(binlib.MIN_TEXTURE_SIZE,
                                                                  binlib.MAX_TEXTURE_SIZE))
    rgb = bytearray(atlas_size * atlas_size * 3)
    binlib.writeBmp24(out_path, atlas_size, atlas_size, rgb)
    return {"ok": True, "outPath": out_path, "width": atlas_size, "height": atlas_size,
            "fileSize": os.path.getsize(out_path)}


@mcp.tool()
def bin_emission_check(bin_path: str, bmp_path: str) -> dict:
    """Check that an emission BMP matches the model atlas so the existing UVs sample it
    directly, and report how much of it is non black."""
    try:
        data, info = binlib.loadBin(bin_path)
        bmp = binlib.readBmp(bmp_path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if not info["textured"]:
        return fail(unknownGridError(info["textureSize"]))
    matches = bmp["width"] == info["textureSize"] and bmp["height"] == info["textureSize"]
    lit = 0
    step = max(1, info["textureSize"] // 256)
    sampled = 0
    for y in range(0, bmp["height"], step):
        for x in range(0, bmp["width"], step):
            sampled += 1
            if binlib.bmpPixel(bmp, x, y) != (0, 0, 0):
                lit += 1
    return {"ok": True, "matchesAtlas": matches, "atlasSize": info["textureSize"],
            "bmpSize": [bmp["width"], bmp["height"]], "sampledTexels": sampled,
            "nonBlackFraction": round(lit / float(sampled), 4) if sampled else 0.0}


@mcp.tool()
def emission_face_maps(bin_path: str, bmp_path: str = "", resolution: int = 16, out_path: str = "",
                       out_dir: str = "", out_bin: str = "") -> dict:
    """Bake the per-texel emission side file into the six face grids the engine keeps in
    EmissionMap (object.h): front/back/right/left/top/bottom, each resolution x resolution float3
    values, same orthographic sweep as CalculateFaceEmissions. Each cell samples the emission BMP
    at the hit UV times the per-triangle emission scalar; a zero cell means nothing was hit.
    out_dir writes one binary per face (front.bin, back.bin, ...) whose payload is exactly
    EmissionMap.emissionMap; out_bin writes all six in one file; out_path writes JSON."""
    try:
        data, info = binlib.loadBin(bin_path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    if resolution < 1 or resolution > 256:
        return fail("resolution must be between 1 and 256")
    bmp = None
    if bmp_path:
        try:
            bmp = binlib.readBmp(bmp_path)
        except (binlib.BinError, OSError) as error:
            return fail(error)
    elif info["textured"]:
        return fail("pass bmp_path to sample a per-texel emission map, this model has an atlas")
    try:
        grids, useScalar, coverage, bounds = emissionfaces.faceGrids(data, info, bmp, resolution)
    except binlib.BinError as error:
        return fail(error)
    summary = emissionfaces.summarise(grids, resolution, useScalar, coverage)
    summary.update({"ok": True, "path": bin_path, "bmpPath": bmp_path or None,
                    "bytes": 6 * resolution * resolution * 12,
                    "bounds": {"min": bounds[0], "max": bounds[1]}})
    if out_dir:
        paths = emissionfaces.writeFaceBins(out_dir, grids, resolution, bounds)
        summary["binFiles"] = paths
        summary["binBytesEach"] = emissionfaces.FACE_HEADER + resolution * resolution * 12
    if out_path:
        summary["outPath"] = out_path
        summary["outBytes"] = emissionfaces.writeGrids(out_path, grids)
    if out_bin:
        summary["outBin"] = out_bin
        summary["outBinBytes"] = emissionfaces.writeFaceBin(out_bin, grids, resolution, bounds)
        try:
            header, values = emissionfaces.readFaceBin(out_bin)
            summary["outBinVerified"] = (header["fileSize"] == summary["outBinBytes"] and
                                         len(values) == 6 * resolution * resolution * 3)
        except binlib.BinError as error:
            summary["outBinVerified"] = False
            summary["outBinError"] = str(error)
    front = grids["front"]
    summary["frontSample"] = front[resolution // 2][resolution // 2]
    return summary


@mcp.tool()
def emission_faces_info(path: str) -> dict:
    """Read a face map .bin written by emission_face_maps: header fields, non zero cell count per
    face and the peak value, without loading the whole model."""
    try:
        header, values = emissionfaces.readFaceBin(path)
    except (binlib.BinError, OSError) as error:
        return fail(error)
    resolution = header["resolution"]
    cells = resolution * resolution
    litPerFace = []
    peak = 0.0
    for face in range(header["faceCount"]):
        base = face * cells * 3
        lit = 0
        for cell in range(cells):
            r, g, b = values[base + cell * 3:base + cell * 3 + 3]
            if max(r, g, b) > 0.0:
                lit += 1
            peak = max(peak, r, g, b)
        litPerFace.append(lit)
    return {"ok": True, "path": path, "resolution": resolution,
            "faceCount": header["faceCount"], "components": header["components"],
            "fileSize": header["fileSize"], "version": header["version"],
            "bounds": header["bounds"], "litPerFace": litPerFace,
            "peak": round(peak, 4)}


if __name__ == "__main__":
    mcp.run()
