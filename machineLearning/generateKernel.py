#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path

KERNEL_FILE = Path(__file__).resolve().parent / "ccnKernel2d.cl"
C_HEADER_FILE = Path(__file__).resolve().parent / "kernelGen.h"
CORE_END_MARKER = "#endif // KERNELGEN_H"

# Preamble written by --fresh: lets a bench generate a complete, self-contained
# header/kernel pair into scratch paths instead of appending to the tracked ones.
# Keep in sync with the tracked ccnKernel2d.cl / kernelGen.h preamble.
SKELETON_CL = """enum ActivationFunction {
    ReLU,
    Sigmoid,
    Tanh,
    None,
};
"""

SKELETON_H = """#ifndef KERNELGEN_H
#define KERNELGEN_H

#include "../render/gpu/format.h" // CL_Context, CL_Pipeline, CL_Buffer, CL_SetArg*, CL_Dispatch2D

typedef enum { KGEN_RELU = 0, KGEN_SIGMOID = 1, KGEN_TANH = 2, KGEN_NONE = 3 } KGenActivation; // mirrors the enum in ccnKernel2d.cl

typedef struct {
	CL_Pipeline pip;
	CL_Buffer weights;    // N*F*F*C floats
	CL_Buffer inputBuf;   // scratch for the host-array _Run path
	CL_Buffer outputBuf;
	float *hostWeights;   // owned CPU copy, kept in sync by SetWeights/Mutate
	CL_Buffer bias;       // N floats (one per filter)
	int width;
	int height;
	int channels;
	int filterSize;
	size_t localX;
	size_t localY;
} KGenConvLayer;

typedef struct {
	CL_Pipeline pip;
	CL_Buffer inputBuf;
	CL_Buffer outputBuf;
	int width;
	int height;
	int channels;
	int poolSize;
	int outWidth;
	int outHeight;
	size_t localX;
	size_t localY;
} KGenPoolLayer;

typedef struct {
	CL_Pipeline pip;
	CL_Buffer weights;    // OUT*IN floats
	CL_Buffer bias;       // OUT floats
	CL_Buffer inputBuf;
	CL_Buffer outputBuf;
	float *hostWeights;
	int inputFloats;
	int outputFloats;
	size_t local;
} KGenDenseLayer;

typedef struct {
	CL_Pipeline pip;
	CL_Buffer inputBuf;
	CL_Buffer outputBuf;
	int count;
} KGenSoftmaxLayer;

// PixelShuffle has its own block (see generateCPixelShuffle): it is emitted with
// the kernels so the same text can be appended to an existing header, guarded so
// the struct is defined exactly once either way.
#ifndef KGEN_PIXELSHUFFLE_LAYER_DEFINED
#define KGEN_PIXELSHUFFLE_LAYER_DEFINED
typedef struct {
	CL_Pipeline pip;
	CL_Buffer inputBuf;
	CL_Buffer outputBuf;
	int width;        // input width (output width = width * upscale)
	int height;       // input height (output height = height * upscale)
	int channels;     // output channels (input channels = channels * upscale^2)
	int upscale;
	int outWidth;
	int outHeight;
	size_t localX;
	size_t localY;
} KGenPixelShuffleLayer;
#endif

#ifndef KGEN_BILINEAR_LAYER_DEFINED
#define KGEN_BILINEAR_LAYER_DEFINED
typedef struct {
	CL_Pipeline pip;
	CL_Buffer inputBuf;
	CL_Buffer outputBuf;
	int width;        // input width (output width = width * upscale)
	int height;       // input height (output height = height * upscale)
	int channels;     // channels (same in and out)
	int upscale;
	int outWidth;
	int outHeight;
	size_t localX;
	size_t localY;
} KGenBilinearLayer;
#endif

static inline void KGen_MutateWeights(float *weights, int count, float amount) {
	for (int i = 0; i < count; i++)
		weights[i] += amount * (2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f);
}
"""

_QUIET = False


def announce(message):
    if not _QUIET:
        print(message)


def writeSkeleton(cl_path, hdr_path):
    """Write the shared preamble into scratch output paths (see --fresh)."""
    for path in (cl_path, hdr_path):
        parent = path.parent
        if parent and not parent.exists():
            parent.mkdir(parents=True, exist_ok=True)
    cl_path.write_text(SKELETON_CL)
    hdr_path.write_text(SKELETON_H + "\n" + CORE_END_MARKER + "\n")


def kernelName(width, height, channels, filterSize, filters=1):
    base = f"cnn2dFilter_w{width}_h{height}_c{channels}_f{filterSize}"
    return base if filters == 1 else f"{base}_n{filters}"


def offsetTerm(var, delta):
    if delta == 0:
        return var
    op = "+" if delta > 0 else "-"
    return f"({var} {op} {abs(delta)})"


def tapGuard(dy, dx, width, height):
    conds = []
    if dy < 0:
        conds.append(f"y >= {-dy}")
    elif dy > 0:
        conds.append(f"y < {height - dy}")
    if dx < 0:
        conds.append(f"x >= {-dx}")
    elif dx > 0:
        conds.append(f"x < {width - dx}")
    return " && ".join(conds)


# Conv generation-time plan.  Historically the emitted kernel was exactly one
# work item per output pixel, which leaves the GPU almost idle on the small
# feature maps this suite is full of (14x14x32 = 196 threads on an 82-SM part).
# The plan therefore has two knobs, both resolved at generation time:
#   * `perItem` filters are accumulated in registers per work item, so a tap's
#     input value is loaded once and reused across all of them instead of being
#     re-read (and re-guarded) once per filter;
#   * when the image alone cannot fill the device, `groups` filter blocks are
#     spread over global_id(0) (`x + width * group`), multiplying the thread
#     count by `groups`.
CONV_SPLIT_TARGET = 16384   # work items we would like in flight
CONV_MIN_ITEMS = 32768      # work items below which a conv is latency bound
CONV_MIN_PER_ITEM = 4       # never block fewer filters than this
CONV_MAX_PER_ITEM = 16      # register budget for the accumulator array
CONV_L1_FLOATS = 32768      # filter block (floats) that still caches in the L1
CONV_BLOCK_COLUMNS = 4      # output columns a work item may accumulate
CONV_BLOCK_BUDGET = 16      # accumulators per work item (filters x columns)
CONV_MAX_UNROLL_QUADS = 16  # channel quads emitted inline before the body loops


def convBlockX(width, channels, filterSize, filters, perItem):
    """Output columns per work item for the register-blocked conv body, or 1.

    A work item streams `perItem` filters x k*k x channels weights and spends
    every load on one output, so the weight loads per output stay at `k*k *
    channels` however the filters are blocked.  Accumulating `blockX` adjacent
    columns amortizes one weight load (and the input load beside it) over
    `blockX` outputs, which at a fixed accumulator budget is the even split
    between the two.

    Only the scalar body gets the columns: it keeps one register per
    accumulator, where the float4 body already spends four on each of its
    `perItem` accumulators and spills when the columns are added.
    """
    if filterSize != 3 or (channels % 4) == 0:
        return 1
    if filters < 2 * CONV_MIN_PER_ITEM or perItem < 2 * CONV_MIN_PER_ITEM:
        return 1
    blockX = min(CONV_BLOCK_COLUMNS, perItem)
    while blockX > 1 and width % blockX:
        blockX //= 2
    return blockX


def convPlan(width, height, channels, filterSize, filters):
    """(groups, perItem, blockX, vectorized, channelQuads) for one conv config."""
    vectorized = (channels % 4) == 0
    quads = channels // 4
    if filters <= 1:
        return 1, 1, 1, vectorized, quads
    want = max(1, CONV_SPLIT_TARGET // max(1, width * height))
    want = min(want, filters)
    perItem = -(-filters // want)                      # ceil
    perItem = min(perItem, CONV_MAX_PER_ITEM)
    perItem = max(CONV_MIN_PER_ITEM, perItem)
    perItem = min(perItem, filters)
    blockX = convBlockX(width, channels, filterSize, filters, perItem)
    if blockX > 1:
        perItem = max(1, CONV_BLOCK_BUDGET // blockX)  # columns share the same budget
    groups = -(-filters // perItem)                    # ceil
    if blockX == 1 and groups == 1 and perItem < CONV_MAX_PER_ITEM and width * height < CONV_MIN_ITEMS:
        # A single filter group on an image below the latency-bound item count
        # leaves the device half idle; the filter block is well inside the
        # register budget, so splitting it buys warps instead of costing reuse.
        perItem = min(filters, CONV_MIN_PER_ITEM)
        groups = -(-filters // perItem)
    return groups, perItem, blockX, vectorized, quads


def generateKernel(width, height, channels, filterSize, filters=1):
    if min(width, height, channels, filterSize, filters) < 1:
        raise ValueError("width, height, channels, filterSize and filters must be >= 1")

    pad = filterSize // 2
    rowStride = width * channels
    planeStride = filterSize * channels
    filterStride = filterSize * filterSize * channels
    name = kernelName(width, height, channels, filterSize, filters)
    groups, perItem, blockX, vec, quads = convPlan(width, height, channels, filterSize, filters)
    split = groups > 1
    wholeBlock = perItem == filters          # one block holds every filter
    looped = (not split) and (not wholeBlock)
    guarded = (not wholeBlock) and (filters % perItem) != 0
    ind = "    "

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (kernelGen v2, do not edit by hand)")
    out.append(f"// width={width} height={height} channels={channels} filterSize={filterSize} filters={filters}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {width}x{height}x{filters} (output channels = {filters}) | weights [{filters}][{filterSize}][{filterSize}][{channels}] | bias [{filters}]")
    plan = f"// plan: {perItem} filter(s) in registers"
    if blockX > 1:
        plan += f", {blockX} output columns per work item"
    if split:
        plan += f", {groups} filter groups over global_id(0)"
    plan += ", float4 channel loads" if vec else ", scalar channel loads"
    out.append(plan)
    out.append("// params: input, filterWeights, bias, output, activation, accumulate")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global const float* filterWeights,")
    out.append("    __global const float* bias,")
    out.append("    __global float* output,")
    out.append("    const int activation,")
    out.append("    const int accumulate)")
    out.append("{")
    if split:
        out.append("    const int g0 = get_global_id(0);")
        if blockX > 1:
            out.append(f"    const int x = (g0 % {width // blockX}) * {blockX};")
            out.append(f"    const int n0 = (g0 / {width // blockX}) * {perItem};")
        else:
            out.append(f"    const int x = g0 % {width};")
            out.append(f"    const int n0 = (g0 / {width}) * {perItem};")
        out.append("    const int y = get_global_id(1);")
        out.append(f"    if (x >= {width} || y >= {height} || n0 >= {filters}) return;")
    else:
        out.append("    const int x = get_global_id(0);")
        out.append("    const int y = get_global_id(1);")
        out.append(f"    if (x >= {width} || y >= {height}) return;")
    out.append("")

    body = ind
    if looped:
        out.append(f"    for (int n0 = 0; n0 < {filters}; n0 += {perItem})")
        out.append("    {")
        body = "        "

    def nTerm(b):
        return str(b) if wholeBlock else f"(n0 + {b})"

    def wIndex(b, offset):
        return (str(b * filterStride + offset) if wholeBlock
                else f"{nTerm(b)} * {filterStride} + {offset}")

    def accName(p, b):
        return f"a{b}" if blockX == 1 else f"a{p}_{b}"

    for p in range(blockX):
        for b in range(perItem):
            name = accName(p, b)
            out.append(f"{body}float4 {name} = (float4)(0.0f);" if vec
                       else f"{body}float {name} = 0.0f;")
    out.append("")

    for j in range(filterSize):
        dy = j - pad
        for i in range(filterSize):
            dx = i - pad
            yTerm = offsetTerm("y", dy)
            filterBase = j * planeStride + i * channels

            out.append(f"{body}// tap ({j}, {i})")
            for p in range(blockX):
                # column p owns output x + p, so this tap lands on x + p + dx
                col = p + dx
                base = f"base_{j}_{i}" if blockX == 1 else f"base_{j}_{i}_{p}"
                guard = tapGuard(dy, col, width, height)
                xTerm = offsetTerm("x", col)
                inner = body
                if guard:
                    out.append(f"{body}if ({guard})")
                if guard or blockX > 1:
                    # sibling column blocks need their own scope for v{q}
                    out.append(f"{body}{{")
                    inner = body + "    "
                out.append(f"{inner}const int {base} = {yTerm} * {rowStride} + {xTerm} * {channels};")
                if vec:
                    if quads > CONV_MAX_UNROLL_QUADS:
                        # Wide channels: a rolled channel loop keeps the body a few
                        # hundred bytes instead of one straight-line block per quad.
                        out.append(f"{inner}for (int q = 0; q < {quads}; q++)")
                        out.append(f"{inner}{{")
                        inner = inner + "    "
                        out.append(f"{inner}const float4 v = *(__global const float4*)(input + {base} + 4 * q);")
                        for b in range(perItem):
                            out.append(f"{inner}{accName(p, b)} += v * *(__global const float4*)(filterWeights + {wIndex(b, filterBase)} + 4 * q);")
                        out.append(f"{inner}}}")
                    else:
                        for q in range(quads):
                            off = f"{base}" if q == 0 else f"{base} + {4 * q}"
                            out.append(f"{inner}const float4 v{q} = *(__global const float4*)(input + {off});")
                            for b in range(perItem):
                                out.append(f"{inner}{accName(p, b)} += v{q} * *(__global const float4*)(filterWeights + {wIndex(b, filterBase + 4 * q)});")
                else:
                    for k in range(channels):
                        inIndex = f"[{base}]" if k == 0 else f"[{base} + {k}]"
                        out.append(f"{inner}const float v{k} = input{inIndex};")
                        for b in range(perItem):
                            out.append(f"{inner}{accName(p, b)} += v{k} * filterWeights[{wIndex(b, filterBase + k)}];")
                if guard or blockX > 1:
                    out.append(f"{body}}}")
                out.append("")

    for p in range(blockX):
        storeX = "x" if p == 0 else f"x + {p}"
        for b in range(perItem):
            if guarded:
                out.append(f"{body}if (n0 + {b} < {filters})")
            out.append(f"{body}{{")
            inner = body + "    "
            name = accName(p, b)
            acc = f"({name}.x + {name}.y) + ({name}.z + {name}.w)" if vec else name
            out.append(f"{inner}float sum = {acc} + bias[{nTerm(b)}];")
            out.append("")
            out.append(f"{inner}float value = sum;")
            out.append(f"{inner}switch (activation)")
            out.append(f"{inner}{{")
            out.append(f"{inner}    case ReLU:    value = max(sum, 0.0f); break;")
            out.append(f"{inner}    case Sigmoid: value = 1.0f / (1.0f + exp(-sum)); break;")
            out.append(f"{inner}    case Tanh:    value = tanh(sum); break;")
            out.append(f"{inner}    case None:    value = sum; break;")
            out.append(f"{inner}}}")
            out.append("")
            out.append(f"{inner}const int idx = (y * {width} + {storeX}) * {filters} + {nTerm(b)};")
            out.append(f"{inner}output[idx] = accumulate ? output[idx] + value : value;")
            out.append(f"{body}}}")
            out.append("")

    if looped:
        out.append("    }")
    out.append("}")
    return "\n".join(out) + "\n"


def cFunctionPrefix(width, height, channels, filterSize, filters=1):
    base = f"KGen_w{width}_h{height}_c{channels}_f{filterSize}"
    return base if filters == 1 else f"{base}_n{filters}"


def roundUp(value, multiple):
    return ((value + multiple - 1) // multiple) * multiple


def nextPow2(value):
    p = 1
    while p < value:
        p *= 2
    return p


def generateC(width, height, channels, filterSize, filters=1):
    baseTag = f"W{width}_H{height}_C{channels}_F{filterSize}"
    tag = baseTag if filters == 1 else f"{baseTag}_N{filters}"
    pref = cFunctionPrefix(width, height, channels, filterSize, filters)
    name = kernelName(width, height, channels, filterSize, filters)
    groups, perItem, blockX, _vec, _quads = convPlan(width, height, channels, filterSize, filters)
    # One work item per (output column block, filter group): the kernel's
    # global_id(0) carries `x + (width / blockX) * group` (see generateKernel),
    # so the grid is `groups` times wider than the image.
    gx = roundUp((width // blockX) * groups, 16)
    gy = roundUp(height, 16)
    # A filter block that no longer fits the L1 misses on every weight load
    # anyway, so halving the group costs it nothing: three half-height groups
    # fit per SM instead of one, and the tail drains in smaller steps.  A block
    # that does fit keeps the full group - more resident blocks would evict it.
    localY = 8 if perItem * filterSize * filterSize * channels >= CONV_L1_FLOATS else 16
    initBound = (6.0 / (filterSize * filterSize * channels)) ** 0.5

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   width={width} height={height} channels={channels} filterSize={filterSize} filters={filters}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {width}x{height}x{filters} (output channels = {filters}) | weights [{filters}][{filterSize}][{filterSize}][{channels}] | bias [{filters}]")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"#define KGEN_{tag}_IN_FLOATS     ({width} * {height} * {channels})")
    out.append(f"#define KGEN_{tag}_OUT_FLOATS    ({width} * {height} * {filters})")
    out.append(f"#define KGEN_{tag}_WEIGHT_FLOATS ({filters} * {filterSize} * {filterSize} * {channels})")
    out.append(f"#define KGEN_{tag}_BIAS_FLOATS   ({filters})")
    out.append("")
    out.append("// Init: builds the kernel; weights NULL = random (kaiming-uniform), bias NULL = zeros; allocates the buffers")
    out.append(f"static inline KGenConvLayer {pref}_Init(CL_Context *ctx, const char *clPath, const float *weights, const float *bias) {{")
    out.append("\tKGenConvLayer layer = {0};")
    out.append(f"\tlayer.width = {width};")
    out.append(f"\tlayer.height = {height};")
    out.append(f"\tlayer.channels = {channels};")
    out.append(f"\tlayer.filterSize = {filterSize};")
    out.append("\tlayer.localX = 16;")
    out.append(f"\tlayer.localY = {localY};")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.hostWeights = malloc(KGEN_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\tif (layer.hostWeights == NULL) {")
    out.append('\t\tprintf("[KGen] weights allocation failed\\n");')
    out.append("\t\tCL_Pipeline_Destroy(&layer.pip);")
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("\tif (weights != NULL) {")
    out.append(f"\t\tmemcpy(layer.hostWeights, weights, KGEN_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\t} else {")
    out.append(f"\t\tfor (int i = 0; i < KGEN_{tag}_WEIGHT_FLOATS; i++)")
    out.append(f"\t\t\tlayer.hostWeights[i] = {initBound:.6f}f * (2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f);")
    out.append("\t}")
    out.append(f"\tlayer.weights = CL_Buffer_CreateFromData(ctx, KGEN_{tag}_WEIGHT_FLOATS * sizeof(float), (void *)layer.hostWeights, CL_MEM_READ_ONLY);")
    out.append("")
    out.append("\tif (bias != NULL) {")
    out.append(f"\t\tlayer.bias = CL_Buffer_CreateFromData(ctx, KGEN_{tag}_BIAS_FLOATS * sizeof(float), (void *)bias, CL_MEM_READ_ONLY);")
    out.append("\t} else {")
    out.append(f"\t\tfloat *zeroBias = calloc(KGEN_{tag}_BIAS_FLOATS, sizeof(float));")
    out.append(f"\t\tlayer.bias = CL_Buffer_CreateFromData(ctx, KGEN_{tag}_BIAS_FLOATS * sizeof(float), (void *)zeroBias, CL_MEM_READ_ONLY);")
    out.append("\t\tfree(zeroBias);")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGEN_{tag}_IN_FLOATS * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGEN_{tag}_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// SetWeights: uploads weights (and bias when not NULL)")
    out.append(f"static inline void {pref}_SetWeights(CL_Context *ctx, KGenConvLayer *layer, const float *weights, const float *bias) {{")
    out.append(f"\tmemcpy(layer->hostWeights, weights, KGEN_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->weights, (void *)layer->hostWeights, KGEN_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\tif (bias != NULL)")
    out.append(f"\t\tCL_Buffer_Write(ctx, &layer->bias, (void *)bias, KGEN_{tag}_BIAS_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (zero-copy chaining)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenConvLayer *layer, CL_Buffer *input, CL_Buffer *output, int activation, int accumulate) {{")
    out.append(f"\tif (input->size < KGEN_{tag}_IN_FLOATS * sizeof(float) || output->size < KGEN_{tag}_OUT_FLOATS * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, &layer->weights);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 2, &layer->bias);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 3, output);")
    out.append("\tCL_SetArgInt(&layer->pip, 4, activation);")
    out.append("\tCL_SetArgInt(&layer->pip, 5, accumulate);")
    out.append(f"\tCL_Dispatch2D(ctx, &layer->pip, {gx}, {gy}, layer->localX, layer->localY);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenConvLayer *layer, const float *input, float *output, int activation, int accumulate) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGEN_{tag}_IN_FLOATS * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf, activation, accumulate);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGEN_{tag}_OUT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Mutate: hostWeights += amount * U[-1, 1], then re-uploads")
    out.append(f"static inline void {pref}_Mutate(CL_Context *ctx, KGenConvLayer *layer, float amount) {{")
    out.append(f"\tKGen_MutateWeights(layer->hostWeights, KGEN_{tag}_WEIGHT_FLOATS, amount);")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->weights, (void *)layer->hostWeights, KGEN_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases hostWeights, GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenConvLayer *layer) {{")
    out.append("\tfree(layer->hostWeights);")
    out.append("\tCL_Buffer_Destroy(&layer->weights);")
    out.append("\tCL_Buffer_Destroy(&layer->bias);")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def poolKernelName(width, height, channels, poolSize):
    return f"cnn2dMaxPool_w{width}_h{height}_c{channels}_p{poolSize}"


def poolCPrefix(width, height, channels, poolSize):
    return f"KGenPool_w{width}_h{height}_c{channels}_p{poolSize}"


def poolPlan(width, height, channels, poolSize):
    """(groups, vectorized, outWidth, outHeight) for one pool config.

    Max pooling only ever compares, so vectorising it changes nothing
    numerically.  With channels % 4 == 0 the emitted kernel works on float4
    channel quads and spreads `channels / 4` channel groups over global_id(0);
    the historical one-thread-per-output-pixel mapping leaves a 14x14 output
    map with 196 live threads.
    """
    outW = width // poolSize
    outH = height // poolSize
    vectorized = (channels % 4) == 0
    groups = channels // 4 if vectorized else 1
    return groups, vectorized, outW, outH


def generateKernelPool(width, height, channels, poolSize):
    if min(width, height, channels, poolSize) < 1:
        raise ValueError("width, height, channels and poolSize must be >= 1")
    if width % poolSize != 0 or height % poolSize != 0:
        raise ValueError("width and height must be divisible by poolSize")

    outW = width // poolSize
    outH = height // poolSize
    name = poolKernelName(width, height, channels, poolSize)
    groups, vec, _, _ = poolPlan(width, height, channels, poolSize)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// width={width} height={height} channels={channels} poolSize={poolSize} -> {outW}x{outH}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {outW}x{outH}x{channels} (max pool {poolSize}x{poolSize}, per channel)")
    out.append("// plan: float4 channel quads, %d channel groups over global_id(0)" % groups
               if vec else "// plan: scalar channels")
    out.append("// params: input, output")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global float* output)")
    out.append("{")
    if vec:
        out.append("    const int g0 = get_global_id(0);")
        out.append(f"    const int ox = g0 / {groups};")
        out.append(f"    const int cq = g0 % {groups};")
        out.append("    const int oy = get_global_id(1);")
        out.append(f"    if (ox >= {outW} || oy >= {outH}) return;")
        out.append("")
        out.append(f"    const int base = ((oy * {poolSize}) * {width} + (ox * {poolSize})) * {channels} + cq * 4;")
        out.append("")
        out.append("    // one float4 covers 4 adjacent channels: max is exact, so the")
        out.append("    // vectorised reduction is bit-identical to the scalar one")
        out.append("    float4 m = (float4)(-INFINITY);")
        for j in range(poolSize):
            for i in range(poolSize):
                offset = j * width * channels + i * channels
                index = "input + base" if offset == 0 else f"input + base + {offset}"
                out.append(f"    m = max(m, *(__global const float4*)({index}));")
        out.append("")
        out.append(f"    *(__global float4*)(output + (oy * {outW} + ox) * {channels} + cq * 4) = m;")
        out.append("}")
        return "\n".join(out) + "\n"

    out.append("    const int ox = get_global_id(0);")
    out.append("    const int oy = get_global_id(1);")
    out.append(f"    if (ox >= {outW} || oy >= {outH}) return;")
    out.append("")
    out.append(f"    const int base = ((oy * {poolSize}) * {width} + (ox * {poolSize})) * {channels};")
    out.append("")

    for c in range(channels):
        out.append(f"    // channel {c}")
        out.append(f"    float m{c} = -INFINITY;")
        for j in range(poolSize):
            for i in range(poolSize):
                offset = j * width * channels + i * channels + c
                index = "input[base]" if offset == 0 else f"input[base + {offset}]"
                out.append(f"    m{c} = max(m{c}, {index});")
        out.append(f"    output[(oy * {outW} + ox) * {channels} + {c}] = m{c};")
        out.append("")

    out.append("}")
    return "\n".join(out) + "\n"


def generateCPool(width, height, channels, poolSize):
    tag = f"W{width}_H{height}_C{channels}_P{poolSize}"
    pref = poolCPrefix(width, height, channels, poolSize)
    name = poolKernelName(width, height, channels, poolSize)
    outW = width // poolSize
    outH = height // poolSize
    groups, _vec, _ow, _oh = poolPlan(width, height, channels, poolSize)
    # global_id(0) carries the channel group (see generateKernelPool), so the
    # grid is `groups` times wider than the output map.
    gx = roundUp(outW * groups, 16)
    gy = roundUp(outH, 16)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   width={width} height={height} channels={channels} poolSize={poolSize}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {outW}x{outH}x{channels} (max pool {poolSize}x{poolSize}, per channel)")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"#define KGENPOOL_{tag}_IN_FLOATS  ({width} * {height} * {channels})")
    out.append(f"#define KGENPOOL_{tag}_OUT_FLOATS ({outW} * {outH} * {channels})")
    out.append("")
    out.append("// Init: builds the kernel and the scratch buffers (no weights)")
    out.append(f"static inline KGenPoolLayer {pref}_Init(CL_Context *ctx, const char *clPath) {{")
    out.append("\tKGenPoolLayer layer = {0};")
    out.append(f"\tlayer.width = {width};")
    out.append(f"\tlayer.height = {height};")
    out.append(f"\tlayer.channels = {channels};")
    out.append(f"\tlayer.poolSize = {poolSize};")
    out.append(f"\tlayer.outWidth = {outW};")
    out.append(f"\tlayer.outHeight = {outH};")
    out.append("\tlayer.localX = 16;")
    out.append("\tlayer.localY = 16;")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGENPOOL_{tag}_IN_FLOATS * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGENPOOL_{tag}_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (zero-copy chaining)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenPoolLayer *layer, CL_Buffer *input, CL_Buffer *output) {{")
    out.append(f"\tif (input->size < KGENPOOL_{tag}_IN_FLOATS * sizeof(float) || output->size < KGENPOOL_{tag}_OUT_FLOATS * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, output);")
    out.append(f"\tCL_Dispatch2D(ctx, &layer->pip, {gx}, {gy}, layer->localX, layer->localY);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenPoolLayer *layer, const float *input, float *output) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGENPOOL_{tag}_IN_FLOATS * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGENPOOL_{tag}_OUT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenPoolLayer *layer) {{")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def denseKernelName(inputFloats, outputFloats):
    return f"cnn2dDense_i{inputFloats}_o{outputFloats}"


def denseCPrefix(inputFloats, outputFloats):
    return f"KGenDense_i{inputFloats}_o{outputFloats}"


def generateKernelDense(inputFloats, outputFloats):
    if min(inputFloats, outputFloats) < 1:
        raise ValueError("inputFloats and outputFloats must be >= 1")

    name = denseKernelName(inputFloats, outputFloats)
    # Every thread streams its own weight row, so the kernel is load-latency
    # bound long before it is FLOP bound: one float4 load per four MACs (and
    # four independent accumulator lanes) is worth several times a scalar chain.
    vectorized = (inputFloats % 4) == 0

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// inputFloats={inputFloats} outputFloats={outputFloats}")
    out.append(f"// dims: in {inputFloats} -> out {outputFloats} | weights [{outputFloats}][{inputFloats}] | bias [{outputFloats}]")
    out.append("// plan: float4 input/weight loads, 4 accumulator lanes" if vectorized
               else "// plan: scalar loads")
    out.append("// params: input, weights, bias, output, activation, accumulate")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global const float* weights,")
    out.append("    __global const float* bias,")
    out.append("    __global float* output,")
    out.append("    const int activation,")
    out.append("    const int accumulate)")
    out.append("{")
    out.append("    const int o = get_global_id(0);")
    out.append(f"    if (o >= {outputFloats}) return;")
    out.append("")
    if vectorized:
        out.append(f"    const int base = o * {inputFloats};")
        out.append("    float4 a = (float4)(0.0f);")
        out.append(f"    for (int i = 0; i < {inputFloats // 4}; i++)")
        out.append("        a += *(__global const float4*)(input + 4 * i) * *(__global const float4*)(weights + base + 4 * i);")
        out.append("")
        out.append("    float sum = (a.x + a.y) + (a.z + a.w);")
    else:
        out.append("    float sum = 0.0f;")
        out.append(f"    for (int i = 0; i < {inputFloats}; i++)")
        out.append(f"        sum += input[i] * weights[o * {inputFloats} + i];")
    out.append("    sum += bias[o];")
    out.append("")
    out.append("    float value = sum;")
    out.append("    switch (activation)")
    out.append("    {")
    out.append("        case ReLU:    value = max(sum, 0.0f); break;")
    out.append("        case Sigmoid: value = 1.0f / (1.0f + exp(-sum)); break;")
    out.append("        case Tanh:    value = tanh(sum); break;")
    out.append("        case None:    value = sum; break;")
    out.append("    }")
    out.append("")
    out.append("    output[o] = accumulate ? output[o] + value : value;")
    out.append("}")
    return "\n".join(out) + "\n"


def generateCDense(inputFloats, outputFloats):
    tag = f"I{inputFloats}_O{outputFloats}"
    pref = denseCPrefix(inputFloats, outputFloats)
    name = denseKernelName(inputFloats, outputFloats)
    localSize = min(64, nextPow2(outputFloats))
    gOut = roundUp(outputFloats, localSize)
    initBound = (6.0 / inputFloats) ** 0.5

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   inputFloats={inputFloats} outputFloats={outputFloats}")
    out.append(f"// dims: in {inputFloats} -> out {outputFloats} | weights [{outputFloats}][{inputFloats}] | bias [{outputFloats}]")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"#define KGENDENSE_{tag}_IN_FLOATS     ({inputFloats})")
    out.append(f"#define KGENDENSE_{tag}_OUT_FLOATS    ({outputFloats})")
    out.append(f"#define KGENDENSE_{tag}_WEIGHT_FLOATS ({outputFloats} * {inputFloats})")
    out.append(f"#define KGENDENSE_{tag}_BIAS_FLOATS   ({outputFloats})")
    out.append("")
    out.append("// Init: builds the kernel; weights NULL = random (kaiming-uniform), bias NULL = zeros; allocates the buffers")
    out.append(f"static inline KGenDenseLayer {pref}_Init(CL_Context *ctx, const char *clPath, const float *weights, const float *bias) {{")
    out.append("\tKGenDenseLayer layer = {0};")
    out.append(f"\tlayer.inputFloats = {inputFloats};")
    out.append(f"\tlayer.outputFloats = {outputFloats};")
    out.append(f"\tlayer.local = {localSize};")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.hostWeights = malloc(KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\tif (layer.hostWeights == NULL) {")
    out.append('\t\tprintf("[KGen] weights allocation failed\\n");')
    out.append("\t\tCL_Pipeline_Destroy(&layer.pip);")
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("\tif (weights != NULL) {")
    out.append(f"\t\tmemcpy(layer.hostWeights, weights, KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\t} else {")
    out.append(f"\t\tfor (int i = 0; i < KGENDENSE_{tag}_WEIGHT_FLOATS; i++)")
    out.append(f"\t\t\tlayer.hostWeights[i] = {initBound:.6f}f * (2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f);")
    out.append("\t}")
    out.append(f"\tlayer.weights = CL_Buffer_CreateFromData(ctx, KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float), (void *)layer.hostWeights, CL_MEM_READ_ONLY);")
    out.append("")
    out.append("\tif (bias != NULL) {")
    out.append(f"\t\tlayer.bias = CL_Buffer_CreateFromData(ctx, KGENDENSE_{tag}_BIAS_FLOATS * sizeof(float), (void *)bias, CL_MEM_READ_ONLY);")
    out.append("\t} else {")
    out.append(f"\t\tfloat *zeroBias = calloc(KGENDENSE_{tag}_BIAS_FLOATS, sizeof(float));")
    out.append(f"\t\tlayer.bias = CL_Buffer_CreateFromData(ctx, KGENDENSE_{tag}_BIAS_FLOATS * sizeof(float), (void *)zeroBias, CL_MEM_READ_ONLY);")
    out.append("\t\tfree(zeroBias);")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGENDENSE_{tag}_IN_FLOATS * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGENDENSE_{tag}_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// SetWeights: uploads weights (and bias when not NULL)")
    out.append(f"static inline void {pref}_SetWeights(CL_Context *ctx, KGenDenseLayer *layer, const float *weights, const float *bias) {{")
    out.append(f"\tmemcpy(layer->hostWeights, weights, KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->weights, (void *)layer->hostWeights, KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("\tif (bias != NULL)")
    out.append(f"\t\tCL_Buffer_Write(ctx, &layer->bias, (void *)bias, KGENDENSE_{tag}_BIAS_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (zero-copy chaining)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenDenseLayer *layer, CL_Buffer *input, CL_Buffer *output, int activation, int accumulate) {{")
    out.append(f"\tif (input->size < KGENDENSE_{tag}_IN_FLOATS * sizeof(float) || output->size < KGENDENSE_{tag}_OUT_FLOATS * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, &layer->weights);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 2, &layer->bias);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 3, output);")
    out.append("\tCL_SetArgInt(&layer->pip, 4, activation);")
    out.append("\tCL_SetArgInt(&layer->pip, 5, accumulate);")
    out.append(f"\tCL_Dispatch1D(ctx, &layer->pip, {gOut}, layer->local);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenDenseLayer *layer, const float *input, float *output, int activation, int accumulate) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGENDENSE_{tag}_IN_FLOATS * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf, activation, accumulate);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGENDENSE_{tag}_OUT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Mutate: hostWeights += amount * U[-1, 1], then re-uploads")
    out.append(f"static inline void {pref}_Mutate(CL_Context *ctx, KGenDenseLayer *layer, float amount) {{")
    out.append(f"\tKGen_MutateWeights(layer->hostWeights, KGENDENSE_{tag}_WEIGHT_FLOATS, amount);")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->weights, (void *)layer->hostWeights, KGENDENSE_{tag}_WEIGHT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases hostWeights, GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenDenseLayer *layer) {{")
    out.append("\tfree(layer->hostWeights);")
    out.append("\tCL_Buffer_Destroy(&layer->weights);")
    out.append("\tCL_Buffer_Destroy(&layer->bias);")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def softmaxKernelName(count):
    return f"cnn2dSoftmax_n{count}"


def softmaxCPrefix(count):
    return f"KGenSoftmax_n{count}"


def generateKernelSoftmax(count):
    if count < 1:
        raise ValueError("count must be >= 1")

    name = softmaxKernelName(count)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// count={count} (softmax over the whole vector)")
    out.append(f"// dims: in {count} -> out {count} (probability distribution, sums to 1)")
    out.append("// params: input, output")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global float* output)")
    out.append("{")
    out.append("    if (get_global_id(0) != 0) return;")
    out.append("")
    out.append("    float m = -INFINITY;")
    out.append(f"    for (int i = 0; i < {count}; i++)")
    out.append("        m = max(m, input[i]);")
    out.append("")
    out.append("    float s = 0.0f;")
    out.append(f"    for (int i = 0; i < {count}; i++)")
    out.append("        s += exp(input[i] - m);")
    out.append("")
    out.append(f"    for (int i = 0; i < {count}; i++)")
    out.append("        output[i] = exp(input[i] - m) / s;")
    out.append("}")
    return "\n".join(out) + "\n"


def generateCSoftmax(count):
    tag = f"N{count}"
    pref = softmaxCPrefix(count)
    name = softmaxKernelName(count)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   count={count}")
    out.append(f"// dims: in {count} -> out {count} (softmax, sums to 1)")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"#define KGENSOFTMAX_{tag}_COUNT ({count})")
    out.append("")
    out.append("// Init: builds the kernel and the scratch buffers (no weights)")
    out.append(f"static inline KGenSoftmaxLayer {pref}_Init(CL_Context *ctx, const char *clPath) {{")
    out.append("\tKGenSoftmaxLayer layer = {0};")
    out.append(f"\tlayer.count = {count};")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGENSOFTMAX_{tag}_COUNT * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGENSOFTMAX_{tag}_COUNT * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (single work-item)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenSoftmaxLayer *layer, CL_Buffer *input, CL_Buffer *output) {{")
    out.append(f"\tif (input->size < KGENSOFTMAX_{tag}_COUNT * sizeof(float) || output->size < KGENSOFTMAX_{tag}_COUNT * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, output);")
    out.append("\tCL_Dispatch1D(ctx, &layer->pip, 1, 1);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenSoftmaxLayer *layer, const float *input, float *output) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGENSOFTMAX_{tag}_COUNT * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGENSOFTMAX_{tag}_COUNT * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenSoftmaxLayer *layer) {{")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def pixelShuffleKernelName(width, height, channels, upscale):
    return f"cnn2dPixelShuffle_w{width}_h{height}_c{channels}_r{upscale}"


def pixelShuffleCPrefix(width, height, channels, upscale):
    return f"KGenPixelShuffle_w{width}_h{height}_c{channels}_r{upscale}"


def generateKernelPixelShuffle(width, height, channels, upscale):
    """torch.nn.PixelShuffle(upscale): (H,W,C*r^2) -> (H*r,W*r,C), channels-last.

    The r*r sub-images packed in the channel dimension become an r-times denser
    spatial grid: out[y*r + i][x*r + j][c] = in[y][x][c*r*r + i*r + j].
    """
    if min(width, height, channels, upscale) < 1:
        raise ValueError("width, height, channels and upscale must be >= 1")

    outW = width * upscale
    outH = height * upscale
    inChannels = channels * upscale * upscale
    name = pixelShuffleKernelName(width, height, channels, upscale)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// width={width} height={height} channels={channels} upscale={upscale} -> {outW}x{outH}")
    out.append(f"// dims: in {width}x{height}x{inChannels} -> out {outW}x{outH}x{channels} | pixel shuffle x{upscale}, channels-last")
    out.append(f"// out[y*{upscale} + i][x*{upscale} + j][c] = in[y][x][c*{upscale * upscale} + i*{upscale} + j]")
    out.append("// params: input, output")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global float* output)")
    out.append("{")
    out.append("    const int x = get_global_id(0);")
    out.append("    const int y = get_global_id(1);")
    out.append(f"    if (x >= {outW} || y >= {outH}) return;")
    out.append("")
    out.append(f"    const int sub = (y % {upscale}) * {upscale} + (x % {upscale});")
    out.append(f"    const int src = ((y / {upscale}) * {width} + (x / {upscale})) * {inChannels} + sub;")
    out.append(f"    const int dst = (y * {outW} + x) * {channels};")
    out.append(f"    for (int c = 0; c < {channels}; c++)")
    out.append(f"        output[dst + c] = input[src + c * {upscale * upscale}];")
    out.append("}")
    return "\n".join(out) + "\n"


def generateCPixelShuffle(width, height, channels, upscale):
    tag = f"W{width}_H{height}_C{channels}_R{upscale}"
    pref = pixelShuffleCPrefix(width, height, channels, upscale)
    name = pixelShuffleKernelName(width, height, channels, upscale)
    outW = width * upscale
    outH = height * upscale
    inChannels = channels * upscale * upscale
    gx = roundUp(outW, 16)
    gy = roundUp(outH, 16)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   width={width} height={height} channels={channels} upscale={upscale}")
    out.append(f"// dims: in {width}x{height}x{inChannels} -> out {outW}x{outH}x{channels} (pixel shuffle x{upscale}, channels-last)")
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Guarded: the struct is also in the skeleton preamble, and this block may be")
    out.append("// appended to a header that already has it.")
    out.append("#ifndef KGEN_PIXELSHUFFLE_LAYER_DEFINED")
    out.append("#define KGEN_PIXELSHUFFLE_LAYER_DEFINED")
    out.append("typedef struct {")
    out.append("\tCL_Pipeline pip;")
    out.append("\tCL_Buffer inputBuf;")
    out.append("\tCL_Buffer outputBuf;")
    out.append("\tint width;        // input width (output width = width * upscale)")
    out.append("\tint height;       // input height (output height = height * upscale)")
    out.append("\tint channels;     // output channels (input channels = channels * upscale^2)")
    out.append("\tint upscale;")
    out.append("\tint outWidth;")
    out.append("\tint outHeight;")
    out.append("\tsize_t localX;")
    out.append("\tsize_t localY;")
    out.append("} KGenPixelShuffleLayer;")
    out.append("#endif")
    out.append("")
    out.append(f"#define KGENPIXELSHUFFLE_{tag}_IN_FLOATS  ({width} * {height} * {inChannels})")
    out.append(f"#define KGENPIXELSHUFFLE_{tag}_OUT_FLOATS ({outW} * {outH} * {channels})")
    out.append("")
    out.append("// Init: builds the kernel and the scratch buffers (no weights)")
    out.append(f"static inline KGenPixelShuffleLayer {pref}_Init(CL_Context *ctx, const char *clPath) {{")
    out.append("\tKGenPixelShuffleLayer layer = {0};")
    out.append(f"\tlayer.width = {width};")
    out.append(f"\tlayer.height = {height};")
    out.append(f"\tlayer.channels = {channels};")
    out.append(f"\tlayer.upscale = {upscale};")
    out.append(f"\tlayer.outWidth = {outW};")
    out.append(f"\tlayer.outHeight = {outH};")
    out.append("\tlayer.localX = 16;")
    out.append("\tlayer.localY = 16;")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGENPIXELSHUFFLE_{tag}_IN_FLOATS * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGENPIXELSHUFFLE_{tag}_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (zero-copy chaining)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenPixelShuffleLayer *layer, CL_Buffer *input, CL_Buffer *output) {{")
    out.append(f"\tif (input->size < KGENPIXELSHUFFLE_{tag}_IN_FLOATS * sizeof(float) || output->size < KGENPIXELSHUFFLE_{tag}_OUT_FLOATS * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, output);")
    out.append(f"\tCL_Dispatch2D(ctx, &layer->pip, {gx}, {gy}, layer->localX, layer->localY);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenPixelShuffleLayer *layer, const float *input, float *output) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGENPIXELSHUFFLE_{tag}_IN_FLOATS * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGENPIXELSHUFFLE_{tag}_OUT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenPixelShuffleLayer *layer) {{")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def bilinearKernelName(width, height, channels, upscale):
    return f"cnn2dBilinear_w{width}_h{height}_c{channels}_r{upscale}"


def bilinearCPrefix(width, height, channels, upscale):
    return f"KGenBilinear_w{width}_h{height}_c{channels}_r{upscale}"


def generateKernelBilinear(width, height, channels, upscale):
    """Bilinear upscale x`upscale` for a (H,W,C) channels-last tensor, align_corners=false.

    Matches torch.nn.functional.interpolate(mode="bilinear", align_corners=False):
    src = max((dst + 0.5) / r - 0.5, 0) with the far tap clamped to the last pixel.
    """
    if min(width, height, channels, upscale) < 1:
        raise ValueError("width, height, channels and upscale must be >= 1")

    outW = width * upscale
    outH = height * upscale
    name = bilinearKernelName(width, height, channels, upscale)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {name} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// width={width} height={height} channels={channels} upscale={upscale} -> {outW}x{outH}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {outW}x{outH}x{channels} (bilinear x{upscale}, align_corners=false, channels-last)")
    out.append("// params: input, output, accumulate")
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"__kernel void {name}(")
    out.append("    __global const float* input,")
    out.append("    __global float* output,")
    out.append("    const int accumulate)")
    out.append("{")
    out.append("    const int x = get_global_id(0);")
    out.append("    const int y = get_global_id(1);")
    out.append(f"    if (x >= {outW} || y >= {outH}) return;")
    out.append("")
    out.append(f"    const float sx = max((x + 0.5f) / {upscale} - 0.5f, 0.0f);")
    out.append(f"    const float sy = max((y + 0.5f) / {upscale} - 0.5f, 0.0f);")
    out.append("    const int x0 = (int)sx;")
    out.append("    const int y0 = (int)sy;")
    out.append(f"    const int x1 = (x0 + 1 < {width}) ? (x0 + 1) : x0;")
    out.append(f"    const int y1 = (y0 + 1 < {height}) ? (y0 + 1) : y0;")
    out.append("    const float tx = sx - (float)x0;")
    out.append("    const float ty = sy - (float)y0;")
    out.append("")
    out.append(f"    const int row0 = y0 * {width} * {channels};")
    out.append(f"    const int row1 = y1 * {width} * {channels};")
    out.append(f"    const int col0 = x0 * {channels};")
    out.append(f"    const int col1 = x1 * {channels};")
    out.append(f"    const int dst = (y * {outW} + x) * {channels};")
    out.append(f"    for (int c = 0; c < {channels}; c++)")
    out.append("    {")
    out.append("        const float v00 = input[row0 + col0 + c];")
    out.append("        const float v01 = input[row0 + col1 + c];")
    out.append("        const float v10 = input[row1 + col0 + c];")
    out.append("        const float v11 = input[row1 + col1 + c];")
    out.append("        const float top = v00 + (v01 - v00) * tx;")
    out.append("        const float bottom = v10 + (v11 - v10) * tx;")
    out.append("        const float value = top + (bottom - top) * ty;")
    out.append("        output[dst + c] = accumulate ? output[dst + c] + value : value;")
    out.append("    }")
    out.append("}")
    return "\n".join(out) + "\n"


def generateCBilinear(width, height, channels, upscale):
    tag = f"W{width}_H{height}_C{channels}_R{upscale}"
    pref = bilinearCPrefix(width, height, channels, upscale)
    name = bilinearKernelName(width, height, channels, upscale)
    outW = width * upscale
    outH = height * upscale
    gx = roundUp(outW, 16)
    gy = roundUp(outH, 16)

    out = []
    out.append("// ---------------------------------------------------------------------------")
    out.append(f"// {pref} -- generated by generateKernel.py (do not edit by hand)")
    out.append(f"// kernel: {name}   width={width} height={height} channels={channels} upscale={upscale}")
    out.append(f"// dims: in {width}x{height}x{channels} -> out {outW}x{outH}x{channels} (bilinear x{upscale}, align_corners=false, channels-last)")
    out.append("// ---------------------------------------------------------------------------")
    out.append("// Guarded: the struct is also in the skeleton preamble, and this block may be")
    out.append("// appended to a header that already has it.")
    out.append("#ifndef KGEN_BILINEAR_LAYER_DEFINED")
    out.append("#define KGEN_BILINEAR_LAYER_DEFINED")
    out.append("typedef struct {")
    out.append("\tCL_Pipeline pip;")
    out.append("\tCL_Buffer inputBuf;")
    out.append("\tCL_Buffer outputBuf;")
    out.append("\tint width;        // input width (output width = width * upscale)")
    out.append("\tint height;       // input height (output height = height * upscale)")
    out.append("\tint channels;     // channels (same in and out)")
    out.append("\tint upscale;")
    out.append("\tint outWidth;")
    out.append("\tint outHeight;")
    out.append("\tsize_t localX;")
    out.append("\tsize_t localY;")
    out.append("} KGenBilinearLayer;")
    out.append("#endif")
    out.append("")
    out.append(f"#define KGENBILINEAR_{tag}_IN_FLOATS  ({width} * {height} * {channels})")
    out.append(f"#define KGENBILINEAR_{tag}_OUT_FLOATS ({outW} * {outH} * {channels})")
    out.append("")
    out.append("// Init: builds the kernel and the scratch buffers (no weights)")
    out.append(f"static inline KGenBilinearLayer {pref}_Init(CL_Context *ctx, const char *clPath) {{")
    out.append("\tKGenBilinearLayer layer = {0};")
    out.append(f"\tlayer.width = {width};")
    out.append(f"\tlayer.height = {height};")
    out.append(f"\tlayer.channels = {channels};")
    out.append(f"\tlayer.upscale = {upscale};")
    out.append(f"\tlayer.outWidth = {outW};")
    out.append(f"\tlayer.outHeight = {outH};")
    out.append("\tlayer.localX = 16;")
    out.append("\tlayer.localY = 16;")
    out.append("")
    out.append(f'\tlayer.pip = CL_Pipeline_FromFile(ctx, clPath, "{name}", NULL);')
    out.append("\tif (layer.pip.kernel == NULL) {")
    out.append(f'\t\tprintf("[KGen] failed to build kernel {name} from %s\\n", clPath);')
    out.append("\t\treturn layer;")
    out.append("\t}")
    out.append("")
    out.append(f"\tlayer.inputBuf = CL_Buffer_Create(ctx, KGENBILINEAR_{tag}_IN_FLOATS * sizeof(float), CL_MEM_READ_ONLY);")
    out.append(f"\tlayer.outputBuf = CL_Buffer_Create(ctx, KGENBILINEAR_{tag}_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    out.append("\treturn layer;")
    out.append("}")
    out.append("")
    out.append("// Forward: dispatches on CL buffers (zero-copy chaining; accumulate adds into the existing output)")
    out.append(f"static inline void {pref}_Forward(CL_Context *ctx, KGenBilinearLayer *layer, CL_Buffer *input, CL_Buffer *output, int accumulate) {{")
    out.append(f"\tif (input->size < KGENBILINEAR_{tag}_IN_FLOATS * sizeof(float) || output->size < KGENBILINEAR_{tag}_OUT_FLOATS * sizeof(float)) {{")
    out.append(f'\t\tprintf("[KGen] buffer too small for {name}\\n");')
    out.append("\t\treturn;")
    out.append("\t}")
    out.append("\tCL_SetArgBuffer(&layer->pip, 0, input);")
    out.append("\tCL_SetArgBuffer(&layer->pip, 1, output);")
    out.append("\tCL_SetArgInt(&layer->pip, 2, accumulate);")
    out.append(f"\tCL_Dispatch2D(ctx, &layer->pip, {gx}, {gy}, layer->localX, layer->localY);")
    out.append("}")
    out.append("")
    out.append("// Run: host input -> scratch buffers -> Forward -> host output")
    out.append(f"static inline void {pref}_Run(CL_Context *ctx, KGenBilinearLayer *layer, const float *input, float *output, int accumulate) {{")
    out.append("\tif (input == NULL || output == NULL) return;")
    out.append(f"\tCL_Buffer_Write(ctx, &layer->inputBuf, (void *)input, KGENBILINEAR_{tag}_IN_FLOATS * sizeof(float));")
    out.append(f"\t{pref}_Forward(ctx, layer, &layer->inputBuf, &layer->outputBuf, accumulate);")
    out.append(f"\tCL_Buffer_Read(ctx, &layer->outputBuf, output, KGENBILINEAR_{tag}_OUT_FLOATS * sizeof(float));")
    out.append("}")
    out.append("")
    out.append("// Destroy: releases GPU buffers and the pipeline")
    out.append(f"static inline void {pref}_Destroy(KGenBilinearLayer *layer) {{")
    out.append("\tCL_Buffer_Destroy(&layer->inputBuf);")
    out.append("\tCL_Buffer_Destroy(&layer->outputBuf);")
    out.append("\tCL_Pipeline_Destroy(&layer->pip);")
    out.append("}")
    return "\n".join(out) + "\n"


def appendC(path, anchor, source):
    if not path.exists():
        raise FileNotFoundError(f"C header not found: {path}")
    text = path.read_text()
    if re.search(rf"\b{re.escape(anchor)}\b", text):
        return False
    idx = text.rfind(CORE_END_MARKER)
    if idx == -1:
        raise ValueError(f"marker '{CORE_END_MARKER}' not found in {path}")
    path.write_text(text[:idx] + "\n" + source + "\n" + text[idx:])
    return True


def appendKernel(path, name, source):
    if not path.exists():
        raise FileNotFoundError(f"kernel file not found: {path}")
    text = path.read_text()
    if re.search(rf"\b{re.escape(name)}\b", text):
        return False
    with path.open("a") as fh:
        if text and not text.endswith("\n"):
            fh.write("\n")
        fh.write("\n" + source)
    return True


def promptInt(label, minimum=1, default=None):
    prompt = f"{label} [{default}]: " if default is not None else f"{label}: "
    while True:
        raw = input(prompt).strip()
        if raw == "" and default is not None:
            return default
        try:
            value = int(raw)
        except ValueError:
            print("please enter an integer")
            continue
        if value < minimum:
            print(f"must be >= {minimum}")
            continue
        return value


def interactive():
    print("KernelGen interactive mode (press Ctrl+C to cancel)")
    mode = ""
    while mode not in ("conv", "pool", "dense", "softmax", "pixelshuffle", "bilinear"):
        mode = input("layer type (conv/pool/dense/softmax/pixelshuffle/bilinear): ").strip().lower()
    if mode == "dense":
        inputFloats = promptInt("inputFloats")
        outputFloats = promptInt("outputFloats")
        return mode, (inputFloats, outputFloats)
    if mode == "softmax":
        return mode, (promptInt("count"),)
    if mode in ("pixelshuffle", "bilinear"):
        width = promptInt("width")
        height = promptInt("height")
        channels = promptInt("channels (output; input = channels * upscale^2)" if mode == "pixelshuffle" else "channels")
        return mode, (width, height, channels, promptInt("upscale", default=2))
    width = promptInt("width")
    height = promptInt("height")
    channels = promptInt("channels")
    if mode == "conv":
        return mode, (width, height, channels, promptInt("filterSize"), promptInt("filters", default=1))
    return mode, (width, height, channels, promptInt("poolSize"))


def applyOutputOverrides(args):
    """Point KERNEL_FILE / C_HEADER_FILE at the requested scratch paths."""
    global KERNEL_FILE, C_HEADER_FILE
    if args.out_cl:
        KERNEL_FILE = Path(args.out_cl).resolve()
    if args.out_hdr:
        C_HEADER_FILE = Path(args.out_hdr).resolve()
    if args.fresh:
        writeSkeleton(KERNEL_FILE, C_HEADER_FILE)


def emitConv(width, height, channels, filterSize, filters=1):
    name = kernelName(width, height, channels, filterSize, filters)
    kernelAdded = appendKernel(KERNEL_FILE, name, generateKernel(width, height, channels, filterSize, filters))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = cFunctionPrefix(width, height, channels, filterSize, filters)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init", generateC(width, height, channels, filterSize, filters))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "conv", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": width * height * channels, "outFloats": width * height * filters,
            "params": {"width": width, "height": height, "channels": channels,
                       "filterSize": filterSize, "filters": filters}}


def emitPool(width, height, channels, poolSize):
    name = poolKernelName(width, height, channels, poolSize)
    kernelAdded = appendKernel(KERNEL_FILE, name, generateKernelPool(width, height, channels, poolSize))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = poolCPrefix(width, height, channels, poolSize)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init", generateCPool(width, height, channels, poolSize))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "pool", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": width * height * channels,
            "outFloats": (width // poolSize) * (height // poolSize) * channels,
            "params": {"width": width, "height": height, "channels": channels,
                       "poolSize": poolSize}}


def emitDense(inputFloats, outputFloats):
    name = denseKernelName(inputFloats, outputFloats)
    kernelAdded = appendKernel(KERNEL_FILE, name, generateKernelDense(inputFloats, outputFloats))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = denseCPrefix(inputFloats, outputFloats)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init", generateCDense(inputFloats, outputFloats))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "dense", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": inputFloats, "outFloats": outputFloats,
            "params": {"inputFloats": inputFloats, "outputFloats": outputFloats}}


def emitSoftmax(count):
    name = softmaxKernelName(count)
    kernelAdded = appendKernel(KERNEL_FILE, name, generateKernelSoftmax(count))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = softmaxCPrefix(count)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init", generateCSoftmax(count))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "softmax", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": count, "outFloats": count, "params": {"count": count}}


def emitPixelShuffle(width, height, channels, upscale):
    """torch.nn.PixelShuffle(upscale) for a (H,W,C*r^2) channels-last tensor."""
    name = pixelShuffleKernelName(width, height, channels, upscale)
    kernelAdded = appendKernel(KERNEL_FILE, name,
                               generateKernelPixelShuffle(width, height, channels, upscale))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = pixelShuffleCPrefix(width, height, channels, upscale)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init",
                     generateCPixelShuffle(width, height, channels, upscale))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "shuffle", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": width * height * channels * upscale * upscale,
            "outFloats": (width * upscale) * (height * upscale) * channels,
            "params": {"width": width, "height": height, "channels": channels,
                       "upscale": upscale}}


def emitBilinear(width, height, channels, upscale):
    """Align-corners-free bilinear upscale for a (H,W,C) channels-last tensor."""
    name = bilinearKernelName(width, height, channels, upscale)
    kernelAdded = appendKernel(KERNEL_FILE, name,
                               generateKernelBilinear(width, height, channels, upscale))
    announce(f"appended {name} to {KERNEL_FILE}" if kernelAdded
             else f"{name} already exists in {KERNEL_FILE}, nothing to do")

    pref = bilinearCPrefix(width, height, channels, upscale)
    cAdded = appendC(C_HEADER_FILE, f"{pref}_Init",
                     generateCBilinear(width, height, channels, upscale))
    announce(f"appended C helpers for {name} to {C_HEADER_FILE}" if cAdded
             else f"C helpers for {name} already exist in {C_HEADER_FILE}, nothing to do")
    return {"kind": "bilinear", "kernel": name, "kernelAdded": kernelAdded, "cAdded": cAdded,
            "inFloats": width * height * channels,
            "outFloats": (width * upscale) * (height * upscale) * channels,
            "params": {"width": width, "height": height, "channels": channels,
                       "upscale": upscale}}


def addIoArgs(parser):
    """Scratch-output options shared by every subcommand.

    --out-cl/--out-hdr redirect generation (default: the tracked files next to
    this script); --fresh rewrites the shared preamble first, so a bench can
    measure exactly this generator's output instead of a stale tracked file;
    --print-json replaces the human log with one machine-readable object.
    """
    parser.add_argument("--out-cl", metavar="PATH", default=None,
                        help="write kernels to PATH instead of ./ccnKernel2d.cl")
    parser.add_argument("--out-hdr", metavar="PATH", default=None,
                        help="write C helpers to PATH instead of ./kernelGen.h")
    parser.add_argument("--fresh", action="store_true",
                        help="rewrite the shared preamble in both outputs first")
    parser.add_argument("--print-json", action="store_true",
                        help="print one JSON object instead of the human log")


def main():
    global _QUIET
    argv = sys.argv[1:]
    if argv and argv[0].lstrip("+-").isdigit():
        argv = ["conv"] + argv

    if not argv:
        mode, params = interactive()
        emit = None
        args = None
    else:
        parser = argparse.ArgumentParser(usage="%(prog)s conv|pool|dense|softmax|pixelshuffle|bilinear ...")
        sub = parser.add_subparsers(dest="mode", required=True)

        p = sub.add_parser("conv", usage="%(prog)s conv width height channels filterSize [filters]")
        p.add_argument("width", type=int)
        p.add_argument("height", type=int)
        p.add_argument("channels", type=int)
        p.add_argument("filterSize", type=int)
        p.add_argument("filters", type=int, nargs="?", default=1)
        addIoArgs(p)

        p = sub.add_parser("pool", usage="%(prog)s pool width height channels poolSize")
        p.add_argument("width", type=int)
        p.add_argument("height", type=int)
        p.add_argument("channels", type=int)
        p.add_argument("poolSize", type=int)
        addIoArgs(p)

        p = sub.add_parser("dense", usage="%(prog)s dense inputFloats outputFloats")
        p.add_argument("inputFloats", type=int)
        p.add_argument("outputFloats", type=int)
        addIoArgs(p)

        p = sub.add_parser("softmax", usage="%(prog)s softmax count")
        p.add_argument("count", type=int)
        addIoArgs(p)

        p = sub.add_parser("pixelshuffle",
                           usage="%(prog)s pixelshuffle width height channels upscale")
        p.add_argument("width", type=int, help="input width")
        p.add_argument("height", type=int, help="input height")
        p.add_argument("channels", type=int, help="output channels (input = channels * upscale^2)")
        p.add_argument("upscale", type=int, nargs="?", default=2, help="r of torch.nn.PixelShuffle(r)")
        addIoArgs(p)

        p = sub.add_parser("bilinear",
                           usage="%(prog)s bilinear width height channels upscale")
        p.add_argument("width", type=int, help="input width")
        p.add_argument("height", type=int, help="input height")
        p.add_argument("channels", type=int, help="channels (same in and out)")
        p.add_argument("upscale", type=int, nargs="?", default=2, help="upscale factor")
        addIoArgs(p)

        args = parser.parse_args(argv)
        if args.mode == "conv":
            params = (args.width, args.height, args.channels, args.filterSize, args.filters)
        elif args.mode == "pool":
            params = (args.width, args.height, args.channels, args.poolSize)
        elif args.mode == "dense":
            params = (args.inputFloats, args.outputFloats)
        elif args.mode in ("pixelshuffle", "bilinear"):
            params = (args.width, args.height, args.channels, args.upscale)
        else:
            params = (args.count,)
        mode = args.mode

    if args is not None:
        _QUIET = bool(args.print_json)
        applyOutputOverrides(args)

    if mode == "conv":
        info = emitConv(*params)
    elif mode == "pool":
        info = emitPool(*params)
    elif mode == "dense":
        info = emitDense(*params)
    elif mode == "pixelshuffle":
        info = emitPixelShuffle(*params)
    elif mode == "bilinear":
        info = emitBilinear(*params)
    else:
        info = emitSoftmax(*params)

    if args is not None and args.print_json:
        info["mode"] = mode
        info["cl"] = str(KERNEL_FILE)
        info["hdr"] = str(C_HEADER_FILE)
        info["fresh"] = bool(args.fresh)
        print(json.dumps(info))


if __name__ == "__main__":
    main()
