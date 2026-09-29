#!/usr/bin/env python3
"""Convert a PyTorch checkpoint into the kgen-weights JSON format and make sure the OpenCL
kernels it needs exist (auto-runs generateKernel.py for any missing conv/pool/dense/softmax,
pixel shuffle or bilinear upscale).

SR checkpoints (head / blocks.N.convM / tail) take the SR path automatically (--arch sr forces
it): the JSON lists the convs in compute order and a C glue header (SRNet_Init / SRNet_LoadWeights /
SRNet_Forward / SRNet_Run; block adds via the kernels' accumulate flag, plus the global bilinear
skip) is written to --out-c (default srnet.h).

Layers are auto-detected by shape: every "*.weight" tensor that is 4D [N,C,F,F] is a conv
(permuted to the generator's [N,F,F,C]), every 2D [OUT,IN] tensor is a dense. A matching
"*.bias" is picked up when present. Max-pool layers carry no weights, so the pool placement
is inferred by finding the spots where halving the spatial size makes the conv stack flatten
to the first dense's input size. The first dense's input columns are permuted from torch's
channel-first (NCHW) flatten to the channel-last (NHWC) flatten the C pipeline produces.

Usage:
  python3 torchToJson.py model.pt [out.json]     # default out: weights.json in the cwd
  python3 torchToJson.py model.pt --list         # print parameter names and exit
  python3 torchToJson.py model.pt --input 28x28  # input image size for conv shape inference
  python3 torchToJson.py model.pt --arch sr      # force the SR path (weights JSON + srnet.h glue)
  python3 torchToJson.py model.pt --skip-gen     # don't generate missing kernels
"""

import argparse
import pickle
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent
GENERATOR = ROOT / "generateKernel.py"
KERNEL_FILE = ROOT / "ccnKernel2d.cl"


def loadStateDict(path):
    if path.endswith(".safetensors"):
        from safetensors.torch import load_file
        return load_file(path)

    import torch
    try:
        obj = torch.load(path, map_location="cpu")
    except pickle.UnpicklingError:
        # since torch 2.6 weights_only=True (the safe default) rejects full-module pickles
        print("torchToJson: note: falling back to weights_only=False (trusted checkpoint assumed)", file=sys.stderr)
        try:
            obj = torch.load(path, map_location="cpu", weights_only=False)
        except (AttributeError, ModuleNotFoundError) as err:
            raise SystemExit(f"torchToJson: cannot unpickle the module ({err}); run from a directory where its "
                             "class is importable, or export a plain state_dict")
    if hasattr(obj, "state_dict"):
        return dict(obj.state_dict())
    if isinstance(obj, dict):
        for key in ("state_dict", "model", "model_state_dict"):
            inner = obj.get(key)
            if isinstance(inner, dict) and inner:
                return inner
        return obj
    raise SystemExit(f"torchToJson: no state dict found in {path}")


def toFloatArray(tensor):
    if hasattr(tensor, "detach"):
        tensor = tensor.detach().cpu().float().numpy()
    return np.asarray(tensor, dtype=np.float32)


def writeFloatArray(f, values):
    # mirrors Kgwt_WriteFloatArray in weightsJson.h
    f.write("[")
    for i, v in enumerate(values):
        if i:
            f.write(",")
        if i % 16 == 0:
            f.write("\n      ")
        f.write("%.9g" % float(v))
    f.write("\n    ]")


def writeJson(outPath, entries):
    with open(outPath, "w") as f:
        f.write("{\n  \"format\": \"kgen-weights\",\n  \"version\": 1,\n  \"layers\": [\n")
        for i, (spec, w, b) in enumerate(entries):
            f.write(f"    {{\"name\": \"{spec['name']}\", \"type\": \"{spec['type']}\"")
            for key, value in spec["shape"]:
                f.write(f", \"{key}\": {value}")
            f.write(", \"weights\": ")
            writeFloatArray(f, w)
            if b is not None:
                f.write(", \"bias\": ")
                writeFloatArray(f, b)
            f.write("},\n" if i + 1 < len(entries) else "}\n")
        f.write("  ]\n}\n")
    print(f"wrote {outPath}")


def naturalKey(key):
    head = key.split(".")[0]
    return (0, int(head), key) if head.isdigit() else (1, 0, key)


def detectLayers(state):
    """classify every "*.weight" tensor as conv (4D) or dense (2D), in model order"""
    keys = sorted((k for k in state if k.endswith(".weight")), key=naturalKey)
    if not keys:
        raise SystemExit("torchToJson: no '*.weight' tensors in the checkpoint")
    layers = []
    for key in keys:
        t = toFloatArray(state[key])
        prefix = key[: -len(".weight")]
        b = toFloatArray(state[prefix + ".bias"]) if prefix + ".bias" in state else None
        if t.ndim == 4:
            n, c, fh, fw = t.shape
            if fh != fw or fh % 2 != 1:
                raise SystemExit(f"torchToJson: {key}: filter {fh}x{fw} unsupported (square, odd size only)")
            layers.append({"type": "conv", "key": key, "w": t, "b": b, "n": n, "c": c, "f": fh})
        elif t.ndim == 2:
            outF, inF = t.shape
            layers.append({"type": "dense", "key": key, "w": t, "b": b, "in": inF, "out": outF})
        else:
            raise SystemExit(f"torchToJson: {key}: shape {t.shape} is neither conv (4D) nor dense (2D)")
    return layers


def kernelName(kind, **kw):
    if kind == "conv":
        name = f"cnn2dFilter_w{kw['w']}_h{kw['h']}_c{kw['c']}_f{kw['f']}"
        return name + (f"_n{kw['n']}" if kw["n"] > 1 else "")
    if kind == "pool":
        return f"cnn2dMaxPool_w{kw['w']}_h{kw['h']}_c{kw['c']}_p{kw['p']}"
    if kind == "dense":
        return f"cnn2dDense_i{kw['in']}_o{kw['out']}"
    if kind == "pixelshuffle":
        return f"cnn2dPixelShuffle_w{kw['w']}_h{kw['h']}_c{kw['c']}_r{kw['r']}"
    if kind == "bilinear":
        return f"cnn2dBilinear_w{kw['w']}_h{kw['h']}_c{kw['c']}_r{kw['r']}"
    return f"cnn2dSoftmax_n{kw['n']}"


def generatorArgs(kind, **kw):
    if kind == "conv":
        return ["conv", kw["w"], kw["h"], kw["c"], kw["f"], kw["n"]]
    if kind == "pool":
        return ["pool", kw["w"], kw["h"], kw["c"], kw["p"]]
    if kind == "dense":
        return ["dense", kw["in"], kw["out"]]
    if kind == "pixelshuffle":
        return ["pixelshuffle", kw["w"], kw["h"], kw["c"], kw["r"]]
    if kind == "bilinear":
        return ["bilinear", kw["w"], kw["h"], kw["c"], kw["r"]]
    return ["softmax", kw["n"]]


def analyze(state, inputWH):
    """returns (json layers with names+shape metadata, kernel requests in compute order)"""
    detected = detectLayers(state)
    convs = [l for l in detected if l["type"] == "conv"]
    denses = [l for l in detected if l["type"] == "dense"]
    if not convs:
        raise SystemExit("torchToJson: no conv layers found")
    if not denses:
        raise SystemExit("torchToJson: no dense layers found")

    w, h = inputWH
    k = len(convs)
    valid = []
    for mask in range(1 << k):
        ww, hh, ch = w, h, convs[0]["c"]
        ok = True
        for i, layer in enumerate(convs):
            if layer["c"] != ch:
                ok = False
                break
            ch = layer["n"]
            if mask & (1 << i):
                if ww % 2 or hh % 2:
                    ok = False
                    break
                ww //= 2
                hh //= 2
        if ok and ww * hh * ch == denses[0]["in"]:
            valid.append(mask)
    if not valid:
        raise SystemExit(
            f"torchToJson: no pool placement flattens the conv stack to the first dense input "
            f"({denses[0]['in']}); check --input WxH (currently {w}x{h})")
    allPools = (1 << k) - 1
    mask = allPools if allPools in valid else valid[0]

    ops = []
    ww, hh, ch = w, h, convs[0]["c"]
    for i, layer in enumerate(convs):
        name = f"conv{i + 1}"
        spec = {"name": name, "type": "conv", "key": layer["key"],
                "shape": [("w", ww), ("h", hh), ("c", ch), ("f", layer["f"]), ("n", layer["n"])]}
        ops.append({"kind": "conv", "spec": spec, "tensor": layer["w"], "bias": layer["b"]})
        ch = layer["n"]
        if mask & (1 << i):
            ops.append({"kind": "pool",
                        "spec": {"name": f"pool{i + 1}", "type": "pool",
                                 "shape": [("w", ww), ("h", hh), ("c", ch), ("p", 2)]}})
            ww //= 2
            hh //= 2

    flat = ww * hh * ch
    for i, layer in enumerate(denses):
        name = f"dense{i + 1}"
        if i == 0 and layer["in"] != flat:
            raise SystemExit(f"torchToJson: {layer['key']} expects {layer['in']} inputs, "
                             f"the conv stack flattens to {flat}")
        if i > 0 and layer["in"] != denses[i - 1]["out"]:
            raise SystemExit(f"torchToJson: {layer['key']} expects {layer['in']} inputs, "
                             f"previous dense outputs {denses[i - 1]['out']}")
        spec = {"name": name, "type": "dense", "key": layer["key"],
                "shape": [("in", layer["in"]), ("out", layer["out"])]}
        op = {"kind": "dense", "spec": spec, "tensor": layer["w"], "bias": layer["b"]}
        if i == 0:
            # torch flattens feature maps channel-first (NCHW), the C pipeline flattens them
            # channel-last (NHWC), so the first dense's input columns need a spatial permute
            op["flatten"] = {"c": ch, "h": hh, "w": ww}
        ops.append(op)
    return ops


def ensureKernelsFor(wanted, skip):
    text = KERNEL_FILE.read_text()
    for kind, spec in wanted:
        name = kernelName(kind, **spec)
        if f"kernel void {name}(" in text:
            continue
        if skip:
            print(f"  missing kernel {name} (generation skipped)")
            continue
        cmd = [sys.executable, str(GENERATOR)] + [str(a) for a in generatorArgs(kind, **spec)]
        try:
            subprocess.run(cmd, check=True, capture_output=True)
        except subprocess.CalledProcessError as err:
            detail = err.stderr.decode().strip() if err.stderr else "no stderr"
            raise SystemExit(f"torchToJson: generateKernel.py failed for {name}: {detail}")

        if f"kernel void {name}(" in KERNEL_FILE.read_text():
            print(f"  generated kernel {name}")
        else:
            raise SystemExit(f"torchToJson: generateKernel.py did not produce {name}")


def ensureKernels(ops, skip):
    wanted = []
    for op in ops:
        spec = dict(op["spec"]["shape"])
        wanted.append((op["kind"], spec))
    lastOut = dict(ops[-1]["spec"]["shape"])["out"]
    wanted.append(("softmax", {"n": lastOut}))
    ensureKernelsFor(wanted, skip)


def ensureKernelsSR(sr, skip):
    head, tail = sr["head"], sr["tail"]
    w, h, scale, c = sr["w"], sr["h"], sr["scale"], head["n"]
    wanted = [
        ("conv", {"w": w, "h": h, "c": head["c"], "f": head["f"], "n": head["n"]}),
        ("conv", {"w": w, "h": h, "c": c, "f": sr["blocks"][0][0]["f"], "n": c}),
        ("conv", {"w": w, "h": h, "c": c, "f": tail["f"], "n": tail["n"]}),
        ("pixelshuffle", {"w": w, "h": h, "c": 3, "r": scale}),
        ("bilinear", {"w": w, "h": h, "c": 3, "r": scale}),
    ]
    ensureKernelsFor(wanted, skip)


def convert(state, outPath, inputWH, skipGen):
    ops = analyze(state, inputWH)
    ensureKernels(ops, skipGen)

    entries = []
    for op in ops:
        if op["kind"] == "pool":
            continue
        spec = op["spec"]
        w = op["tensor"]
        torchShape = tuple(w.shape)
        if op["kind"] == "conv":
            w = w.transpose(0, 2, 3, 1)  # torch [N,C,F,F] -> generator [N,F,F,C]
        if op["kind"] == "dense" and op.get("flatten"):
            f = op["flatten"]
            w = w.reshape(w.shape[0], f["c"], f["h"], f["w"]).transpose(0, 2, 3, 1).reshape(w.shape[0], -1)
        w = w.reshape(-1)
        b = None if op["bias"] is None else op["bias"].reshape(-1)

        s = dict(spec["shape"])
        want = s["n"] * s["f"] * s["f"] * s["c"] if op["kind"] == "conv" else s["out"] * s["in"]
        if w.size != want:
            raise SystemExit(f"torchToJson: {spec['name']}: {spec['key']} holds {w.size} floats, expected {want}")

        entries.append((spec, w, b))
        shape = ", ".join(f"{k2}={v}" for k2, v in spec["shape"])
        print(f"  {spec['name']:6s} {spec['key']} {torchShape} -> {shape}  {w.size} floats, bias {0 if b is None else b.size}")

    writeJson(outPath, entries)


def convCPrefix(w, h, c, f, n):
    base = f"KGen_w{w}_h{h}_c{c}_f{f}"
    return base if n == 1 else f"{base}_n{n}"


def convMacroTag(w, h, c, f, n):
    base = f"W{w}_H{h}_C{c}_F{f}"
    return base if n == 1 else f"{base}_N{n}"


def detectSR(state):
    if "head.weight" not in state or "tail.weight" not in state:
        return False
    return any(re.fullmatch(r"blocks\.\d+\.conv\d+\.weight", key) for key in state)


def convLayerFromKey(state, key):
    t = toFloatArray(state[key])
    if t.ndim != 4:
        raise SystemExit(f"torchToJson: {key}: expected a 4D conv tensor, got {t.shape}")
    n, c, fh, fw = t.shape
    if fh != fw or fh % 2 != 1:
        raise SystemExit(f"torchToJson: {key}: filter {fh}x{fw} unsupported (square, odd size only)")
    prefix = key[: -len(".weight")]
    b = toFloatArray(state[prefix + ".bias"]) if prefix + ".bias" in state else None
    return {"type": "conv", "key": key, "w": t, "b": b, "n": n, "c": c, "f": fh}


def analyzeSR(state, inputWH):
    """head conv / residual blocks (conv1-relu-conv2 + add) / tail conv + pixel shuffle + bilinear skip"""
    head = convLayerFromKey(state, "head.weight")
    tail = convLayerFromKey(state, "tail.weight")
    if head["c"] != 3:
        raise SystemExit(f"torchToJson: SR: head takes {head['c']} channels, expected RGB (3)")

    ids = []
    for key in state:
        m = re.fullmatch(r"blocks\.(\d+)\.conv\d+\.weight", key)
        if m:
            ids.append(int(m.group(1)))
    ids = sorted(set(ids))
    if not ids or ids != list(range(ids[-1] + 1)):
        raise SystemExit("torchToJson: SR: block indices must be contiguous from 0")

    blocks = []
    for i in ids:
        pair = []
        for j in (1, 2):
            layer = convLayerFromKey(state, f"blocks.{i}.conv{j}.weight")
            if layer["c"] != head["n"] or layer["n"] != head["n"] or layer["f"] != head["f"]:
                raise SystemExit(f"torchToJson: SR: {layer['key']} does not match the head's {head['n']} channels")
            pair.append(layer)
        blocks.append(tuple(pair))

    if tail["c"] != head["n"] or tail["f"] != head["f"] or tail["n"] % 3 != 0:
        raise SystemExit("torchToJson: SR: tail does not match the head (channels / filter size / 3*r^2 outputs)")
    scale = int(round((tail["n"] // 3) ** 0.5))
    if scale * scale * 3 != tail["n"]:
        raise SystemExit(f"torchToJson: SR: tail outputs {tail['n']} channels; expected 3*r^2")

    w, h = inputWH
    return {"head": head, "tail": tail, "blocks": blocks, "scale": scale, "w": w, "h": h}


def writeGlue(path, checkpoint, sr):
    path = Path(path)
    w, h, scale = sr["w"], sr["h"], sr["scale"]
    c, f = sr["head"]["n"], sr["head"]["f"]
    hrW, hrH = w * scale, h * scale
    blocks = len(sr["blocks"])
    guard = re.sub(r"\W+", "_", path.stem.upper()).strip("_") + "_H"

    headPref = convCPrefix(w, h, sr["head"]["c"], f, c)
    headTag = convMacroTag(w, h, sr["head"]["c"], f, c)
    blockPref = convCPrefix(w, h, c, f, c)
    blockTag = convMacroTag(w, h, c, f, c)
    tailPref = convCPrefix(w, h, c, sr["tail"]["f"], sr["tail"]["n"])
    tailTag = convMacroTag(w, h, c, sr["tail"]["f"], sr["tail"]["n"])
    shufflePref = f"KGenPixelShuffle_w{w}_h{h}_c{3}_r{scale}"
    bilinearPref = f"KGenBilinear_w{w}_h{h}_c{3}_r{scale}"

    def specLine(name, cin, nout, tag):
        return (f"\t{{\"{name}\", \"conv\", {{\"w\", \"h\", \"c\", \"f\", \"n\"}}, "
                f"{{{w}, {h}, {cin}, {f}, {nout}}}, 5, KGEN_{tag}_WEIGHT_FLOATS, KGEN_{tag}_BIAS_FLOATS}},")

    lines = []
    lines.append("// ---------------------------------------------------------------------------")
    lines.append(f"// {path.name} -- generated by torchToJson.py from {Path(checkpoint).name} (do not edit by hand)")
    lines.append(f"// SRNet: {w}x{h}x3 -> {hrW}x{hrH}x3 | head->{c} channels | {blocks} residual blocks (conv-relu-conv + add)")
    lines.append(f"// pipeline: head-relu -> blocks -> tail -> pixel shuffle x{scale} -> +(bilinear skip x{scale})")
    lines.append("// ---------------------------------------------------------------------------")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append('#include "kernelGen.h"')
    lines.append('#include "weightsJson.h"')
    lines.append("")
    lines.append(f"#define SRNET_LR_WIDTH     {w}")
    lines.append(f"#define SRNET_LR_HEIGHT    {h}")
    lines.append(f"#define SRNET_HR_WIDTH     {hrW}")
    lines.append(f"#define SRNET_HR_HEIGHT    {hrH}")
    lines.append(f"#define SRNET_CHANNELS     {c}")
    lines.append(f"#define SRNET_BLOCKS       {blocks}")
    lines.append(f"#define SRNET_SCALE        {scale}")
    lines.append("#define SRNET_IN_FLOATS    (SRNET_LR_WIDTH * SRNET_LR_HEIGHT * 3)")
    lines.append("#define SRNET_STAGE_CHANNELS (SRNET_CHANNELS > 3 * SRNET_SCALE * SRNET_SCALE ? SRNET_CHANNELS : 3 * SRNET_SCALE * SRNET_SCALE)")
    lines.append("#define SRNET_STAGE_FLOATS   (SRNET_LR_WIDTH * SRNET_LR_HEIGHT * SRNET_STAGE_CHANNELS)")
    lines.append("#define SRNET_OUT_FLOATS   (SRNET_HR_WIDTH * SRNET_HR_HEIGHT * 3)")
    lines.append("#define SRNET_LAYERS       (2 + 2 * SRNET_BLOCKS)")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("\tKGenConvLayer head;")
    lines.append("\tKGenConvLayer conv1[SRNET_BLOCKS];")
    lines.append("\tKGenConvLayer conv2[SRNET_BLOCKS];")
    lines.append("\tKGenConvLayer tail;")
    lines.append("\tKGenPixelShuffleLayer shuffle;")
    lines.append("\tKGenBilinearLayer skip;")
    lines.append("\tCL_Buffer stageA;")
    lines.append("\tCL_Buffer stageB;")
    lines.append("} SRNet;")
    lines.append("")
    lines.append("static const KgwtLayerSpec SRNET_SPECS[SRNET_LAYERS] = {")
    lines.append(specLine("head", sr["head"]["c"], c, headTag))
    for i in range(blocks):
        lines.append(specLine(f"block{i}.conv1", c, c, blockTag))
        lines.append(specLine(f"block{i}.conv2", c, c, blockTag))
    lines.append(specLine("tail", c, sr["tail"]["n"], tailTag))
    lines.append("};")
    lines.append("")
    lines.append("// Init: builds every kernel and allocates the ping-pong stage buffers")
    lines.append("static inline SRNet SRNet_Init(CL_Context *ctx, const char *clPath) {")
    lines.append("\tSRNet net = {0};")
    lines.append(f"\tnet.head = {headPref}_Init(ctx, clPath, NULL, NULL);")
    lines.append("\tfor (int i = 0; i < SRNET_BLOCKS; i++) {")
    lines.append(f"\t\tnet.conv1[i] = {blockPref}_Init(ctx, clPath, NULL, NULL);")
    lines.append(f"\t\tnet.conv2[i] = {blockPref}_Init(ctx, clPath, NULL, NULL);")
    lines.append("\t}")
    lines.append(f"\tnet.tail = {tailPref}_Init(ctx, clPath, NULL, NULL);")
    lines.append(f"\tnet.shuffle = {shufflePref}_Init(ctx, clPath);")
    lines.append(f"\tnet.skip = {bilinearPref}_Init(ctx, clPath);")
    lines.append("\tnet.stageA = CL_Buffer_Create(ctx, SRNET_STAGE_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    lines.append("\tnet.stageB = CL_Buffer_Create(ctx, SRNET_STAGE_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    lines.append("\treturn net;")
    lines.append("}")
    lines.append("")
    lines.append("// LoadWeights: fills hostWeights from a kgen-weights JSON, then uploads")
    lines.append("static inline int SRNet_LoadWeights(CL_Context *ctx, SRNet *net, const char *path) {")
    lines.append("\tstatic float biasBuf[SRNET_LAYERS][SRNET_CHANNELS];")
    lines.append("\tfloat *weights[SRNET_LAYERS];")
    lines.append("\tfloat *biases[SRNET_LAYERS];")
    lines.append("\tint n = 0;")
    lines.append("\tweights[n] = net->head.hostWeights; biases[n] = biasBuf[n]; n++;")
    lines.append("\tfor (int i = 0; i < SRNET_BLOCKS; i++) {")
    lines.append("\t\tweights[n] = net->conv1[i].hostWeights; biases[n] = biasBuf[n]; n++;")
    lines.append("\t\tweights[n] = net->conv2[i].hostWeights; biases[n] = biasBuf[n]; n++;")
    lines.append("\t}")
    lines.append("\tweights[n] = net->tail.hostWeights; biases[n] = biasBuf[n];")
    lines.append("\tif (!Kgwt_LoadJson(path, SRNET_LAYERS, SRNET_SPECS, weights, biases)) return 0;")
    lines.append(f"\t{headPref}_SetWeights(ctx, &net->head, net->head.hostWeights, biasBuf[0]);")
    lines.append("\tfor (int i = 0; i < SRNET_BLOCKS; i++) {")
    lines.append(f"\t\t{blockPref}_SetWeights(ctx, &net->conv1[i], net->conv1[i].hostWeights, biasBuf[1 + 2 * i]);")
    lines.append(f"\t\t{blockPref}_SetWeights(ctx, &net->conv2[i], net->conv2[i].hostWeights, biasBuf[2 + 2 * i]);")
    lines.append("\t}")
    lines.append(f"\t{tailPref}_SetWeights(ctx, &net->tail, net->tail.hostWeights, biasBuf[SRNET_LAYERS - 1]);")
    lines.append("\treturn 1;")
    lines.append("}")
    lines.append("")
    lines.append("// Forward: stageA holds the block input, conv2 accumulates into it (kernel accumulate = x + conv2(...))")
    lines.append("static inline void SRNet_Forward(CL_Context *ctx, SRNet *net, CL_Buffer *lr, CL_Buffer *hr) {")
    lines.append(f"\t{headPref}_Forward(ctx, &net->head, lr, &net->stageA, KGEN_RELU, 0);")
    lines.append("\tfor (int i = 0; i < SRNET_BLOCKS; i++) {")
    lines.append(f"\t\t{blockPref}_Forward(ctx, &net->conv1[i], &net->stageA, &net->stageB, KGEN_RELU, 0);")
    lines.append(f"\t\t{blockPref}_Forward(ctx, &net->conv2[i], &net->stageB, &net->stageA, KGEN_NONE, 1);")
    lines.append("\t}")
    lines.append(f"\t{tailPref}_Forward(ctx, &net->tail, &net->stageA, &net->stageB, KGEN_NONE, 0);")
    lines.append(f"\t{shufflePref}_Forward(ctx, &net->shuffle, &net->stageB, hr);")
    lines.append(f"\t{bilinearPref}_Forward(ctx, &net->skip, lr, hr, 1);")
    lines.append("}")
    lines.append("")
    lines.append("// Run: host lr -> GPU -> host hr (per-call staging buffers)")
    lines.append("static inline void SRNet_Run(CL_Context *ctx, SRNet *net, const float *lr, float *hr) {")
    lines.append("\tCL_Buffer lrBuf = CL_Buffer_CreateFromData(ctx, SRNET_IN_FLOATS * sizeof(float), (void *)lr, CL_MEM_READ_ONLY);")
    lines.append("\tCL_Buffer hrBuf = CL_Buffer_Create(ctx, SRNET_OUT_FLOATS * sizeof(float), CL_MEM_READ_WRITE);")
    lines.append("\tSRNet_Forward(ctx, net, &lrBuf, &hrBuf);")
    lines.append("\tCL_Buffer_Read(ctx, &hrBuf, hr, SRNET_OUT_FLOATS * sizeof(float));")
    lines.append("\tCL_Buffer_Destroy(&lrBuf);")
    lines.append("\tCL_Buffer_Destroy(&hrBuf);")
    lines.append("}")
    lines.append("")
    lines.append("// Destroy: releases every layer and the stage buffers")
    lines.append("static inline void SRNet_Destroy(SRNet *net) {")
    lines.append(f"\t{headPref}_Destroy(&net->head);")
    lines.append("\tfor (int i = 0; i < SRNET_BLOCKS; i++) {")
    lines.append(f"\t\t{blockPref}_Destroy(&net->conv1[i]);")
    lines.append(f"\t\t{blockPref}_Destroy(&net->conv2[i]);")
    lines.append("\t}")
    lines.append(f"\t{tailPref}_Destroy(&net->tail);")
    lines.append(f"\t{shufflePref}_Destroy(&net->shuffle);")
    lines.append(f"\t{bilinearPref}_Destroy(&net->skip);")
    lines.append("\tCL_Buffer_Destroy(&net->stageA);")
    lines.append("\tCL_Buffer_Destroy(&net->stageB);")
    lines.append("}")
    lines.append("")
    lines.append(f"#endif // {guard}")
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {path}")


def convertSR(state, checkpoint, outPath, inputWH, skipGen, outCPath):
    sr = analyzeSR(state, inputWH)
    ensureKernelsSR(sr, skipGen)
    print(f"  SR: {sr['w']}x{sr['h']}x3 -> {sr['w'] * sr['scale']}x{sr['h'] * sr['scale']}x3, "
          f"{len(sr['blocks'])} blocks, {sr['head']['n']} channels, x{sr['scale']}")

    entries = []

    def add(name, layer):
        spec = {"name": name, "type": "conv", "key": layer["key"],
                "shape": [("w", sr["w"]), ("h", sr["h"]), ("c", layer["c"]),
                          ("f", layer["f"]), ("n", layer["n"])]}
        tensor = layer["w"].transpose(0, 2, 3, 1).reshape(-1)
        want = layer["n"] * layer["f"] * layer["f"] * layer["c"]
        if tensor.size != want:
            raise SystemExit(f"torchToJson: {layer['key']} holds {tensor.size} floats, expected {want}")
        bias = None if layer["b"] is None else layer["b"].reshape(-1)
        shape = ", ".join(f"{k2}={v}" for k2, v in spec["shape"])
        print(f"  {name:14s} {layer['key']} {tuple(layer['w'].shape)} -> {shape}  {tensor.size} floats, bias {0 if bias is None else bias.size}")
        entries.append((spec, tensor, bias))

    add("head", sr["head"])
    for i, (convA, convB) in enumerate(sr["blocks"]):
        add(f"block{i}.conv1", convA)
        add(f"block{i}.conv2", convB)
    add("tail", sr["tail"])
    writeJson(outPath, entries)
    writeGlue(outCPath, checkpoint, sr)


def parseInput(text):
    parts = text.lower().replace(",", "x").split("x")
    if len(parts) == 1:
        parts = [parts[0], parts[0]]
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("expected WxH (e.g. 28x28)")
    return int(parts[0]), int(parts[1])


def main():
    ap = argparse.ArgumentParser(description="Convert a torch checkpoint to kgen-weights JSON")
    ap.add_argument("checkpoint")
    ap.add_argument("out", nargs="?", default="weights.json")
    ap.add_argument("--list", action="store_true", help="print parameter names and exit")
    ap.add_argument("--input", type=parseInput, default=(28, 28), metavar="WxH",
                    help="input image size used to infer conv/pool shapes")
    ap.add_argument("--arch", choices=("auto", "chain", "sr"), default="auto",
                    help="model layout; 'auto' picks the SR path for head/blocks/tail checkpoints")
    ap.add_argument("--out-c", metavar="PATH", default="srnet.h",
                    help="C glue header written for SR checkpoints (default: srnet.h)")
    ap.add_argument("--skip-gen", action="store_true", help="don't auto-generate missing kernels")
    args = ap.parse_args()

    state = loadStateDict(args.checkpoint)
    if args.list:
        for key in sorted(state, key=naturalKey):
            shape = tuple(state[key].shape) if hasattr(state[key], "shape") else "?"
            print(f"{key}: {shape}")
        return
    if args.arch == "sr" or (args.arch == "auto" and detectSR(state)):
        convertSR(state, args.checkpoint, args.out, args.input, args.skip_gen, args.out_c)
    else:
        convert(state, args.out, args.input, args.skip_gen)


if __name__ == "__main__":
    main()
