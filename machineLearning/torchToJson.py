#!/usr/bin/env python3
"""Convert a PyTorch checkpoint into the kgen-weights JSON format and make sure the OpenCL
kernels it needs exist (auto-runs generateKernel.py for any missing conv/pool/dense/softmax).

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
  python3 torchToJson.py model.pt --skip-gen     # don't generate missing kernels
"""

import argparse
import pickle
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
    return f"cnn2dSoftmax_n{kw['n']}"


def generatorArgs(kind, **kw):
    if kind == "conv":
        return ["conv", kw["w"], kw["h"], kw["c"], kw["f"], kw["n"]]
    if kind == "pool":
        return ["pool", kw["w"], kw["h"], kw["c"], kw["p"]]
    if kind == "dense":
        return ["dense", kw["in"], kw["out"]]
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


def ensureKernels(ops, skip):
    text = KERNEL_FILE.read_text()
    wanted = []
    for op in ops:
        spec = dict(op["spec"]["shape"])
        wanted.append((op["kind"], spec))
    lastOut = dict(ops[-1]["spec"]["shape"])["out"]
    wanted.append(("softmax", {"n": lastOut}))
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
    ap.add_argument("--skip-gen", action="store_true", help="don't auto-generate missing kernels")
    args = ap.parse_args()

    state = loadStateDict(args.checkpoint)
    if args.list:
        for key in sorted(state, key=naturalKey):
            shape = tuple(state[key].shape) if hasattr(state[key], "shape") else "?"
            print(f"{key}: {shape}")
        return
    convert(state, args.out, args.input, args.skip_gen)


if __name__ == "__main__":
    main()
