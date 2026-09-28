#!/usr/bin/env python3
"""PyTorch oracle for the generated OpenCL kernels.

    reference.py --configs build/mlbench/configs.json --data build/mlbench/data

For every config in the list it writes float32 binaries into <data>/<id>/:

    in.bin    layer input            [inFloats]
    w.bin     weights (conv/dense)   [out][k][k][in] / [out][in]
    b.bin     bias                   [out]
    prev.bin  only when accumulate=1 [outFloats]
    ref.bin   expected output        [outFloats]

Everything is seeded per config, so the C bench compares against exactly the same
numbers the model would see on any machine.  Chains are evaluated layer by layer
(the bench runs the same sequence through the generated forwards).
"""
import argparse
import json
import os
import sys

import torch
import torch.nn.functional as F


def activation(t, kind):
    if kind == 1:
        return torch.sigmoid(t)
    if kind == 2:
        return torch.tanh(t)
    if kind == 3:
        return t
    return torch.clamp(t, min=0.0)


def conv_forward(x, w, b, params, act):
    """x: [H,W,C] weights: [N,K,K,C] (generator layout) -> [H,W,N]"""
    size = params["filterSize"]
    pad = size // 2
    x_t = x.permute(2, 0, 1).unsqueeze(0)                    # [1,C,H,W]
    w_t = w.permute(0, 3, 1, 2).contiguous()                 # [N,C,K,K]
    y = F.conv2d(x_t, w_t, b, padding=pad)
    return activation(y.squeeze(0).permute(1, 2, 0), act)     # [H,W,N]


def pool_forward(x, params):
    size = params["poolSize"]
    x_t = x.permute(2, 0, 1).unsqueeze(0)
    y = F.max_pool2d(x_t, size)
    return y.squeeze(0).permute(1, 2, 0)


def dense_forward(x, w, b, act):
    return activation(w @ x + b, act)


def softmax_forward(x):
    return torch.softmax(x, dim=0)


def run_layer(layer, x, weights, bias):
    """Returns the layer output as a flat float32 tensor (accumulate is added by the caller)."""
    kind = layer["kind"]
    params = layer.get("params") or {}
    act = layer.get("activation", 0)
    if kind == "conv":
        return conv_forward(x.view(params["height"], params["width"], params["channels"]),
                            weights, bias, params, act).reshape(-1)
    if kind == "pool":
        return pool_forward(x.view(params["height"], params["width"], params["channels"]),
                            params).reshape(-1)
    if kind == "dense":
        return dense_forward(x, weights, bias, act)
    if kind == "softmax":
        return softmax_forward(x)
    raise ValueError(f"unknown layer kind {kind!r}")


def write_bin(path, tensor):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(tensor.detach().to(torch.float32).contiguous().numpy().tobytes())


def make_input(kind, params, gen):
    """Random input for a layer, scaled by params['inputScale'] when present."""
    scale = float(params.get("inputScale", 1.0))
    if kind == "dense":
        count = params["inputFloats"]
    elif kind == "softmax":
        count = params["count"]
    else:
        count = params["width"] * params["height"] * params["channels"]
    return torch.randn(count, generator=gen, dtype=torch.float32) * scale


def make_weights(kind, params, gen):
    """(weights, bias) in the generator's layout, or (None, None) when weightless."""
    if kind == "conv":
        n, k, c = params["filters"], params["filterSize"], params["channels"]
        return (torch.randn(n, k, k, c, generator=gen, dtype=torch.float32) * 0.2,
                torch.randn(n, generator=gen, dtype=torch.float32) * 0.1)
    if kind == "dense":
        return (torch.randn(params["outputFloats"], params["inputFloats"], generator=gen,
                            dtype=torch.float32) * 0.1,
                torch.randn(params["outputFloats"], generator=gen, dtype=torch.float32) * 0.1)
    return None, None


def build_config(cfg, data_root):
    """Write input/weights/prev/reference binaries for one config."""
    gen = torch.Generator(device="cpu").manual_seed(int(cfg.get("seed", 1)))
    layers = cfg.get("layers") or [dict(cfg)]
    single = len(layers) == 1
    out_dir = os.path.join(data_root, cfg["id"].replace(":", "_"))
    os.makedirs(out_dir, exist_ok=True)

    state = None
    flat_out = None
    first_in_floats = 0
    for index, layer in enumerate(layers):
        kind = layer["kind"]
        params = layer.get("params") or {}
        weights, bias = make_weights(kind, params, gen)
        if index == 0:
            state = make_input(kind, params, gen)
            first_in_floats = int(state.numel())
            write_bin(os.path.join(out_dir, "in.bin"), state)
        if weights is not None:
            suffix = "" if single else str(index)
            write_bin(os.path.join(out_dir, f"w{suffix}.bin"), weights)
            write_bin(os.path.join(out_dir, f"b{suffix}.bin"), bias)

        out = run_layer(layer, state, weights, bias)
        if index + 1 < len(layers):
            state = out
            continue
        if cfg.get("accumulate"):
            prev = torch.randn(out.numel(), generator=gen, dtype=torch.float32)
            write_bin(os.path.join(out_dir, "prev.bin"), prev)
            out = prev + out
        write_bin(os.path.join(out_dir, "ref.bin"), out)
        flat_out = out

    return {"id": cfg["id"], "layers": len(layers),
            "inFloats": first_in_floats, "outFloats": int(flat_out.numel()),
            "dir": out_dir}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--configs", required=True)
    parser.add_argument("--data", required=True)
    args = parser.parse_args()

    with open(args.configs) as fh:
        specs = json.load(fh)
    torch.manual_seed(0)
    summary = [build_config(cfg, args.data) for cfg in specs]
    print(json.dumps({"written": len(summary), "configs": summary}, indent=2))


if __name__ == "__main__":
    sys.exit(main())
