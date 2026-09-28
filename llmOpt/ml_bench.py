#!/usr/bin/env python3
"""Benchmark harness for the generated CNN layer kernels (machineLearning).

    python3 llmOpt/ml_bench.py --suite core [--generator PATH] [--capture]
    python3 llmOpt/ml_bench.py --suite smoke --kind conv --reps 5
    python3 llmOpt/ml_bench.py --list

The generator under test (default `machineLearning/generateKernel.py`) is run once per
config into scratch files under `machineLearning/` (dot-prefixed, gitignored), a PyTorch
reference is written for the same seeds, a small shim is generated that binds each
config to the generated helpers, and `machineLearning/bench/kernelBench.c` is compiled
against it.  The result is per-config timing plus a correctness gate against PyTorch.

Everything the model cannot be trusted to keep honest - the config list, the seeds, the
tolerances, the reference, the bench source - is hashed into the suite key, so editing
the test invalidates the baseline instead of moving the numbers.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import time

LLMOPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(LLMOPT_DIR)

GENERATOR_DEFAULT = "machineLearning/generateKernel.py"
BENCH_DIR = "machineLearning/bench"
WORK_DIR = os.path.join(PROJECT_DIR, "build", "mlbench")
GEN_H = "machineLearning/.benchGen.h"
GEN_CL = "machineLearning/.benchGen.cl"
GEN_SHIM = "machineLearning/.benchShim.c"
BASELINE_FILE = os.path.join(LLMOPT_DIR, "ml_baseline.json")

REPS_DEFAULT = 20
WARMUP_DEFAULT = 2
ABS_TOL_DEFAULT = 1e-4
REL_TOL_DEFAULT = 1e-3
SUITE_VERSION = "mlbench-v1"

# The bench and the oracle are fixed inputs: changing either invalidates a baseline.
SUITE_INPUTS = (BENCH_DIR + "/kernelBench.c", BENCH_DIR + "/kernelBench.h",
                BENCH_DIR + "/reference.py")


def rel(path):
    return os.path.join(PROJECT_DIR, path)


def run(cmd, cwd=None, timeout=600, check=True):
    result = subprocess.run(cmd, cwd=cwd or PROJECT_DIR, capture_output=True, text=True,
                            timeout=timeout)
    if check and result.returncode != 0:
        raise RuntimeError("%s failed (%s):\n%s" % (" ".join(cmd), result.returncode,
                                                   (result.stderr or result.stdout)[-1500:]))
    return result


# ---------------------------------------------------------------------------
# config model
# ---------------------------------------------------------------------------

ACTIVATIONS = {"relu": 0, "sigmoid": 1, "tanh": 2, "none": 3}


def conv(width, height, channels, filterSize, filters=1, act="relu", acc=0, seed=None,
         absTol=None, relTol=None, inputScale=None):
    params = {"width": width, "height": height, "channels": channels,
              "filterSize": filterSize, "filters": filters}
    if inputScale is not None:
        params["inputScale"] = inputScale
    return {"kind": "conv", "params": params, "activation": ACTIVATIONS[act],
            "accumulate": acc, "seed": seed, "absTol": absTol, "relTol": relTol}


def pool(width, height, channels, poolSize, seed=None):
    return {"kind": "pool", "params": {"width": width, "height": height,
                                       "channels": channels, "poolSize": poolSize},
            "activation": ACTIVATIONS["none"], "accumulate": 0, "seed": seed}


def dense(inputFloats, outputFloats, act="relu", acc=0, seed=None, absTol=None,
          relTol=None, inputScale=None):
    params = {"inputFloats": inputFloats, "outputFloats": outputFloats}
    if inputScale is not None:
        params["inputScale"] = inputScale
    return {"kind": "dense", "params": params, "activation": ACTIVATIONS[act],
            "accumulate": acc, "seed": seed, "absTol": absTol, "relTol": relTol}


def softmax(count, seed=None, absTol=None, relTol=None, inputScale=None):
    params = {"count": count}
    if inputScale is not None:
        params["inputScale"] = inputScale
    return {"kind": "softmax", "params": params, "activation": ACTIVATIONS["none"],
            "accumulate": 0, "seed": seed, "absTol": absTol, "relTol": relTol}


def chain(*layers, seed=None, acc=0):
    return {"kind": "chain", "layers": list(layers), "accumulate": acc, "seed": seed}


def config_id(spec):
    kind = spec["kind"]
    params = spec.get("params") or {}
    if kind == "conv":
        parts = ["w%d" % params["width"], "h%d" % params["height"], "c%d" % params["channels"],
                 "k%d" % params["filterSize"], "n%d" % params["filters"]]
    elif kind == "pool":
        parts = ["w%d" % params["width"], "h%d" % params["height"],
                 "c%d" % params["channels"], "p%d" % params["poolSize"]]
    elif kind == "dense":
        parts = ["i%d" % params["inputFloats"], "o%d" % params["outputFloats"]]
    elif kind == "softmax":
        parts = ["n%d" % params["count"]]
    else:
        parts = ["-".join(_layer_tag(layer) for layer in spec.get("layers") or [])]
    act = spec.get("activation", 0)
    if act and kind in ("conv", "dense"):
        parts.append(["relu", "sigmoid", "tanh", "none"][act])
    if spec.get("accumulate"):
        parts.append("acc")
    if (spec.get("params") or {}).get("inputScale"):
        parts.append("s%g" % spec["params"]["inputScale"])
    return "%s:%s" % (kind, "_".join(parts))


def _layer_tag(layer):
    """Compact per-layer tag, so chain ids stay unique and readable."""
    kind = layer["kind"]
    params = layer["params"]
    if kind == "conv":
        return "c%dx%dx%dk%dn%d" % (params["width"], params["height"], params["channels"],
                                    params["filterSize"], params["filters"])
    if kind == "pool":
        return "p%dx%dx%dq%d" % (params["width"], params["height"], params["channels"],
                                 params["poolSize"])
    if kind == "dense":
        return "d%d>%d" % (params["inputFloats"], params["outputFloats"])
    return "s%d" % params["count"]


def layer_output_floats(spec):
    """Output element count for one layer (used for chain validation)."""
    kind = spec["kind"]
    params = spec["params"]
    if kind == "conv":
        return params["width"] * params["height"] * params["filters"]
    if kind == "pool":
        return ((params["width"] // params["poolSize"]) *
                (params["height"] // params["poolSize"]) * params["channels"])
    if kind == "dense":
        return params["outputFloats"]
    if kind == "softmax":
        return params["count"]
    raise ValueError("no output size for kind %r" % kind)


def layer_input_floats(spec):
    kind = spec["kind"]
    params = spec["params"]
    if kind == "dense":
        return params["inputFloats"]
    if kind == "softmax":
        return params["count"]
    return params["width"] * params["height"] * params["channels"]


def config_floats(spec):
    """(inFloats, outFloats) of the whole config, validating chain wiring."""
    if spec["kind"] != "chain":
        return layer_input_floats(spec), layer_output_floats(spec)
    layers = spec["layers"]
    produced = layer_input_floats(layers[0])
    for index, layer in enumerate(layers):
        want = layer_input_floats(layer)
        if want != produced:
            raise ValueError("chain layer %d wants %d inputs but the previous layer "
                             "produces %d" % (index, want, produced))
        produced = layer_output_floats(layer)
    return layer_input_floats(layers[0]), produced


def config_work(spec):
    """(flops, bytes) per call, for GFLOP/s and GB/s."""
    if spec["kind"] == "chain":
        total = 0.0
        for layer in spec["layers"]:
            total += config_work(layer)[0]
        head = layer_input_floats(spec["layers"][0])
        tail = layer_output_floats(spec["layers"][-1])
        weights = 0
        for layer in spec["layers"]:
            if layer["kind"] == "conv":
                p = layer["params"]
                weights += p["filters"] * p["filterSize"] * p["filterSize"] * p["channels"] + p["filters"]
            elif layer["kind"] == "dense":
                p = layer["params"]
                weights += p["outputFloats"] * p["inputFloats"] + p["outputFloats"]
        return total, 4.0 * (head + tail + weights)

    kind = spec["kind"]
    params = spec["params"]
    if kind == "conv":
        w, h, c = params["width"], params["height"], params["channels"]
        k, n = params["filterSize"], params["filters"]
        flops = 2.0 * k * k * c * w * h * n
        bytes_ = 4.0 * (w * h * c + n * k * k * c + n + w * h * n)
    elif kind == "pool":
        w, h, c, p = (params["width"], params["height"], params["channels"],
                      params["poolSize"])
        out = (w // p) * (h // p) * c
        flops = float((p * p - 1) * out)
        bytes_ = 4.0 * (w * h * c + out)
    elif kind == "dense":
        i, o = params["inputFloats"], params["outputFloats"]
        flops = 2.0 * i * o
        bytes_ = 4.0 * (i + o * i + o + o)
    else:
        n = params["count"]
        flops = 4.0 * n
        bytes_ = 4.0 * (2 * n)
    return flops, bytes_


# ---------------------------------------------------------------------------
# suites
# ---------------------------------------------------------------------------

SUITES = {
    "smoke": [
        conv(28, 28, 1, 3, 16, act="relu"),
        pool(28, 28, 16, 2),
        dense(1568, 128, act="relu"),
        softmax(10),
    ],
    "core": [
        conv(28, 28, 1, 3, 16, act="relu"),
        conv(28, 28, 1, 3, 16, act="none"),
        conv(28, 28, 1, 3, 16, act="sigmoid"),
        conv(28, 28, 1, 3, 16, act="tanh"),
        conv(28, 28, 4, 3, 8, act="relu"),
        conv(28, 28, 8, 3, 16, act="relu"),
        conv(28, 28, 3, 5, 4, act="relu"),
        conv(64, 64, 8, 3, 16, act="relu"),
        conv(32, 64, 16, 3, 32, act="relu"),
        conv(28, 28, 1, 3, 4, act="relu"),
        conv(28, 28, 1, 3, 16, act="relu", acc=1),
        conv(14, 14, 16, 3, 32, act="relu"),
        conv(16, 16, 6, 7, 4, act="relu"),
        conv(8, 8, 32, 1, 8, act="relu"),
        pool(28, 28, 16, 2),
        pool(28, 28, 8, 4),
        pool(54, 54, 3, 3),
        pool(14, 14, 32, 2),
        dense(1568, 128, act="relu"),
        dense(128, 10, act="none"),
        dense(4096, 512, act="relu"),
        dense(64, 4096, act="relu"),
        softmax(10),
        softmax(100),
    ],
    "stress": [
        conv(512, 512, 8, 9, 1, act="none"),
        conv(256, 256, 32, 5, 16, act="relu"),
        conv(64, 64, 64, 3, 64, act="relu"),
        conv(128, 128, 16, 3, 32, act="relu"),
        conv(512, 512, 16, 3, 8, act="relu"),
        conv(128, 32, 64, 3, 16, act="relu"),
        pool(512, 512, 64, 4),
        dense(4096, 4096, act="relu"),
        softmax(10000, absTol=1e-3, relTol=1e-3),
    ],
    "edges": [
        conv(5, 5, 1, 9, 1, act="none"),
        conv(27, 29, 3, 3, 4, act="relu"),
        conv(33, 65, 6, 5, 2, act="relu"),
        conv(1, 1, 1, 1, 1, act="none"),
        conv(28, 28, 1, 3, 16, act="relu", acc=1),
        conv(28, 28, 1, 1, 1, act="none"),
        pool(4, 4, 1, 4),
        pool(30, 30, 2, 3),
        dense(1, 8, act="none"),
        softmax(1000, inputScale=100.0, absTol=1e-3, relTol=1e-3),
    ],
    "chain": [
        chain(conv(28, 28, 1, 3, 16, act="relu"),
              pool(28, 28, 16, 2),
              conv(14, 14, 16, 3, 32, act="relu"),
              pool(14, 14, 32, 2),
              dense(1568, 128, act="relu"),
              dense(128, 10, act="none"),
              softmax(10)),
        chain(conv(32, 32, 3, 3, 16, act="relu"),
              pool(32, 32, 16, 2),
              conv(16, 16, 16, 5, 8, act="relu"),
              dense(2048, 10, act="none"),
              softmax(10)),
        chain(conv(64, 64, 8, 3, 32, act="relu"),
              pool(64, 64, 32, 2),
              dense(32768, 128, act="relu"),
              dense(128, 10, act="none"),
              softmax(10)),
    ],
}
SUITES["all"] = SUITES["core"] + SUITES["stress"] + SUITES["edges"] + SUITES["chain"]


def select_configs(suite="core", configs=None, kind=None):
    """Resolve a suite name / explicit id list / kind filter into configs with ids."""
    if configs:
        pool_ = {config_id(spec): spec for items in SUITES.values() for spec in items}
        chosen = []
        for name in configs:
            if name not in pool_:
                raise ValueError("unknown config %r (see --list)" % name)
            chosen.append((name, pool_[name]))
    else:
        items = SUITES.get(suite)
        if items is None:
            raise ValueError("unknown suite %r (have %s)" % (suite, ", ".join(sorted(SUITES))))
        chosen = []
        seen = set()
        for index, spec in enumerate(items):
            cid = config_id(spec)
            if cid in seen:
                raise ValueError("duplicate config id %r in suite %s" % (cid, suite))
            seen.add(cid)
            chosen.append((cid, spec))
    if kind:
        chosen = [(cid, spec) for cid, spec in chosen
                  if (spec["kind"] == kind or
                      (spec["kind"] == "chain" and any(l["kind"] == kind
                                                       for l in spec["layers"])))]
    resolved = []
    for index, (cid, spec) in enumerate(chosen):
        item = dict(spec)
        item["id"] = cid
        item["seed"] = spec.get("seed") or (1000 + index * 7)
        item["absTol"] = spec.get("absTol") or ABS_TOL_DEFAULT
        item["relTol"] = spec.get("relTol") or REL_TOL_DEFAULT
        resolved.append(item)
    return resolved


# ---------------------------------------------------------------------------
# generation
# ---------------------------------------------------------------------------

def _helper_names(spec):
    """(prefix, weightMacro, biasMacro) exactly as generateKernel.py names them."""
    kind = spec["kind"]
    params = spec["params"]
    if kind == "conv":
        tag = "W%d_H%d_C%d_F%d" % (params["width"], params["height"], params["channels"],
                                   params["filterSize"])
        prefix = "KGen_w%d_h%d_c%d_f%d" % (params["width"], params["height"],
                                           params["channels"], params["filterSize"])
        if params["filters"] > 1:
            tag += "_N%d" % params["filters"]
            prefix += "_n%d" % params["filters"]
        return prefix, "KGEN_%s_WEIGHT_FLOATS" % tag, "KGEN_%s_BIAS_FLOATS" % tag
    if kind == "pool":
        return ("KGenPool_w%d_h%d_c%d_p%d" % (params["width"], params["height"],
                                              params["channels"], params["poolSize"]),
                None, None)
    if kind == "dense":
        tag = "I%d_O%d" % (params["inputFloats"], params["outputFloats"])
        return ("KGenDense_i%d_o%d" % (params["inputFloats"], params["outputFloats"]),
                "KGENDENSE_%s_WEIGHT_FLOATS" % tag, "KGENDENSE_%s_BIAS_FLOATS" % tag)
    tag = "N%d" % params["count"]
    return "KGenSoftmax_n%d" % params["count"], None, None


def _generator_args(spec):
    kind = spec["kind"]
    params = spec["params"]
    if kind == "conv":
        args = ["conv", str(params["width"]), str(params["height"]),
                str(params["channels"]), str(params["filterSize"]), str(params["filters"])]
    elif kind == "pool":
        args = ["pool", str(params["width"]), str(params["height"]),
                str(params["channels"]), str(params["poolSize"])]
    elif kind == "dense":
        args = ["dense", str(params["inputFloats"]), str(params["outputFloats"])]
    else:
        args = ["softmax", str(params["count"])]
    return args


def generate_kernels(generator, configs):
    """Run the generator once per layer, writing scratch header/kernel files."""
    if not os.path.exists(rel(generator)):
        raise RuntimeError("generator not found: %s" % generator)
    layers = []
    for spec in configs:
        layers.extend(spec["layers"] if spec["kind"] == "chain" else [spec])
    written = 0
    for index, layer in enumerate(layers):
        cmd = [sys.executable, rel(generator)] + _generator_args(layer) + [
            "--out-cl", rel(GEN_CL), "--out-hdr", rel(GEN_H), "--print-json"]
        if index == 0:
            cmd.append("--fresh")
        result = run(cmd, timeout=600)
        info = json.loads(result.stdout.strip().splitlines()[-1])
        written += 1 if info.get("kernelAdded") else 0
    return {"layers": len(layers), "regenerated": written}


def write_shim(configs):
    """Emit the C glue that binds each config id to the generated helpers."""
    lines = [
        "// generated by llmOpt/ml_bench.py - binds config ids to the generated helpers",
        '#include "bench/kernelBench.h"',
        '#include ".benchGen.h"',
        "",
    ]
    entries, inits, runs, destroys = [], [], [], []
    for index, spec in enumerate(configs):
        layers = spec["layers"] if spec["kind"] == "chain" else [spec]
        in_floats, out_floats = config_floats(spec)
        if in_floats <= 0 or out_floats <= 0:
            raise ValueError("config %s has empty buffers" % spec["id"])
        states, mids = [], []
        for li, layer in enumerate(layers):
            prefix, weight_macro, bias_macro = _helper_names(layer)
            struct = {"conv": "KGenConvLayer", "pool": "KGenPoolLayer",
                      "dense": "KGenDenseLayer", "softmax": "KGenSoftmaxLayer"}[layer["kind"]]
            var = "s%d_%d" % (index, li)
            lines.append("static %s %s;" % (struct, var))
            states.append((layer, prefix, weight_macro, bias_macro, var))
            if li + 1 < len(layers):
                mid = "m%d_%d" % (index, li)
                lines.append("static CL_Buffer %s;" % mid)
                mids.append((mid, layer_output_floats(layer)))
        lines.append("")

        lines.append("static int init_%d(CL_Context *ctx, const char *clPath, const MlEntry *e) {" % index)
        for li, (layer, prefix, weight_macro, bias_macro, var) in enumerate(states):
            suffix = "" if len(states) == 1 else str(li)
            if weight_macro:
                lines.append("    static float w%d_%d[%s];" % (index, li, weight_macro))
                lines.append("    static float b%d_%d[%s];" % (index, li, bias_macro))
                lines.append('    MlLoadFloats(e->dataDir, "w%s.bin", w%d_%d, %s);'
                             % (suffix, index, li, weight_macro))
                lines.append('    MlLoadFloats(e->dataDir, "b%s.bin", b%d_%d, %s);'
                             % (suffix, index, li, bias_macro))
                lines.append("    %s = %s_Init(ctx, clPath, w%d_%d, b%d_%d);"
                             % (var, prefix, index, li, index, li))
            else:
                lines.append("    %s = %s_Init(ctx, clPath);" % (var, prefix))
            lines.append("    if (%s.pip.kernel == NULL) return 1;" % var)
        for mid, mid_floats in mids:
            lines.append("    %s = CL_Buffer_Create(ctx, %d * sizeof(float), CL_MEM_READ_WRITE);"
                         % (mid, mid_floats))
            lines.append("    if (%s.buf == NULL) return 1;" % mid)
        lines.append("    return 0;")
        lines.append("}")
        lines.append("")

        lines.append("static int run_%d(CL_Context *ctx, const MlEntry *e, CL_Buffer *in, CL_Buffer *out) {" % index)
        lines.append("    (void)ctx;")
        sources = ["in"] + ["&%s" % mid for mid, _ in mids]
        for li, (layer, prefix, weight_macro, bias_macro, var) in enumerate(states):
            last = li + 1 == len(states)
            src = sources[li]
            dst = "out" if last else "&m%d_%d" % (index, li)
            if layer["kind"] in ("conv", "dense"):
                activation = layer.get("activation", 0)
                accumulate = 1 if (last and spec.get("accumulate")) else 0
                lines.append("    %s_Forward(ctx, &%s, %s, %s, %d, %d);"
                             % (prefix, var, src, dst, activation, accumulate))
            else:
                lines.append("    %s_Forward(ctx, &%s, %s, %s);" % (prefix, var, src, dst))
        lines.append("    return 0;")
        lines.append("}")
        lines.append("")

        lines.append("static void destroy_%d(void) {" % index)
        for li, (layer, prefix, weight_macro, bias_macro, var) in enumerate(states):
            lines.append("    %s_Destroy(&%s);" % (prefix, var))
        for mid, _ in mids:
            lines.append("    CL_Buffer_Destroy(&%s);" % mid)
        lines.append("}")
        lines.append("")

        entries.append('    {"%s", "%s", "%s", %d, %d, %d, %d, 0, %gf, %gf, ""}'
                       % (spec["id"], _shape_text(spec), spec["kind"], in_floats, out_floats,
                          spec.get("activation", 0), 1 if spec.get("accumulate") else 0,
                          spec["absTol"], spec["relTol"]))
        inits.append("init_%d" % index)
        runs.append("run_%d" % index)
        destroys.append("destroy_%d" % index)

    lines.append("MlEntry kMlEntries[] = {")
    for position, entry in enumerate(entries):
        lines.append(entry + ("," if position + 1 < len(entries) else ""))
    lines.append("};")
    lines.append("const int kMlEntryCount = %d;" % len(configs))
    lines.append("const MlInitFn kMlInit[] = {%s};" % ", ".join(inits))
    lines.append("const MlRunFn kMlRun[] = {%s};" % ", ".join(runs))
    lines.append("const MlDestroyFn kMlDestroy[] = {%s};" % ", ".join(destroys))
    lines.append("")

    os.makedirs(os.path.dirname(rel(GEN_SHIM)), exist_ok=True)
    with open(rel(GEN_SHIM), "w") as fh:
        fh.write("\n".join(lines))
    return len(configs)


def _shape_text(spec):
    if spec["kind"] == "chain":
        # No spaces: the C bench parses the manifest with sscanf("%s").
        return "->".join(config_id(l) for l in spec["layers"])
    return config_id(spec).split(":", 1)[1]


def write_manifest(configs, data_root):
    os.makedirs(WORK_DIR, exist_ok=True)
    path = os.path.join(WORK_DIR, "manifest.tsv")
    with open(path, "w") as fh:
        fh.write("# id\tkind\tshape\tinFloats\toutFloats\tflops\tbytes\tactivation\t"
                 "accumulate\treps\tabsTol\trelTol\tdataDir\n")
        for spec in configs:
            in_floats, out_floats = config_floats(spec)
            flops, bytes_ = config_work(spec)
            data_dir = os.path.join(data_root, spec["id"].replace(":", "_"))
            fh.write("%s\t%s\t%s\t%d\t%d\t%.6f\t%.6f\t%d\t%d\t0\t%g\t%g\t%s\n"
                     % (spec["id"], spec["kind"], _shape_text(spec), in_floats, out_floats,
                        flops, bytes_, spec.get("activation", 0),
                        1 if spec.get("accumulate") else 0, spec["absTol"], spec["relTol"],
                        data_dir))
    return path


# ---------------------------------------------------------------------------
# build + run
# ---------------------------------------------------------------------------

def compile_bench():
    binary = os.path.join(WORK_DIR, "kernelBench")
    os.makedirs(WORK_DIR, exist_ok=True)
    cmd = ["/usr/bin/clang", "-O2", "-Wall", "-march=native",
           "-I", rel("machineLearning"), "-I", PROJECT_DIR,
           rel(BENCH_DIR + "/kernelBench.c"), rel(GEN_SHIM), rel("render/gpu/format.c"),
           "-o", binary,
           "-Wl,--disable-new-dtags", "-Wl,-rpath,/usr/lib/x86_64-linux-gnu",
           "--ld-path=/usr/bin/ld", "-L/usr/lib/x86_64-linux-gnu", "-lOpenCL", "-lm"]
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=PROJECT_DIR, timeout=600)
    if result.returncode != 0:
        raise RuntimeError("bench build failed:\n%s" % (result.stderr or result.stdout)[-2000:])
    return binary, (result.stderr or "").strip()


def run_bench(binary, manifest, cl_path, reps, warmup):
    result_path = os.path.join(WORK_DIR, "result.json")
    if os.path.exists(result_path):
        os.unlink(result_path)  # never analyse a previous run's report
    cmd = [binary, "--manifest", manifest, "--cl", cl_path,
           "--reps", str(reps), "--warmup", str(warmup), "--json", result_path]
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=PROJECT_DIR, timeout=3600)
    stderr = (result.stderr or "").strip()
    if not os.path.exists(result_path) or os.path.getsize(result_path) == 0:
        raise RuntimeError("bench produced no result (exit %s):\n%s"
                           % (result.returncode, (stderr or result.stdout)[-2000:]))
    with open(result_path) as fh:
        raw = fh.read()
    # CL_Pipeline_FromFile logs to stdout before the report starts.
    start = raw.find("{")
    if start < 0:
        raise RuntimeError("bench result has no JSON: %s" % raw[-500:])
    doc = json.loads(raw[start:])
    doc["exitCode"] = result.returncode
    doc["stderr"] = stderr[-4000:]
    return doc


# ---------------------------------------------------------------------------
# suite identity + baseline
# ---------------------------------------------------------------------------

def _file_hash(path):
    try:
        with open(path, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()[:16]
    except OSError:
        return "missing"


def suite_hash(configs):
    digest = hashlib.sha256()
    digest.update(SUITE_VERSION.encode())
    digest.update(json.dumps([[c["id"], c["seed"], c["absTol"], c["relTol"]]
                              for c in configs], sort_keys=True).encode())
    for path in SUITE_INPUTS:
        digest.update(_file_hash(rel(path)).encode())
    return digest.hexdigest()[:8]


def generator_hash(generator):
    return _file_hash(rel(generator))


def _baseline_read():
    if not os.path.exists(BASELINE_FILE):
        return {}
    try:
        with open(BASELINE_FILE) as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return {}


def load_baseline(suite_key, doc=None):
    """Pinned baseline for this suite key, or None when absent or from another device."""
    entry = (_baseline_read().get("suites") or {}).get(suite_key) or {}
    if not entry.get("configs"):
        return None
    if doc is not None:
        want = entry.get("settings") or {}
        got = doc.get("settings") or {}
        for field in ("device", "platform"):
            if want.get(field) and want.get(field) != got.get(field):
                return None
    return entry


def save_baseline(suite_key, generator, doc):
    cache = _baseline_read()
    suites = cache.get("suites")
    if not isinstance(suites, dict):
        suites = {}
    cache["version"] = 1
    cache["suites"] = suites
    suites[suite_key] = {
        "generator": generator,
        "generatorHash": generator_hash(generator),
        "capturedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "configs": doc["configs"],
        "settings": doc["settings"],
        "aggregate": doc["aggregate"],
    }
    tmp = BASELINE_FILE + ".tmp.%d" % os.getpid()
    with open(tmp, "w") as fh:
        json.dump(cache, fh, indent=2, sort_keys=True)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, BASELINE_FILE)


def _family(kind):
    return kind if kind in ("conv", "pool", "dense", "softmax", "chain") else "other"


# ---------------------------------------------------------------------------
# verdict
# ---------------------------------------------------------------------------

def _spread(row):
    median = row.get("medianMs") or 0.0
    if median <= 0.0:
        return 0.0
    return (row.get("p90Ms", 0.0) - row.get("p10Ms", 0.0)) / median


def summarize(doc, baseline, generator, previous_hash=None, captured=False):
    """Return (human text, verdict code)."""
    rows = doc["configs"]
    failed = [r for r in rows if not r["ok"]]
    worst_diff = max((r["maxAbs"] for r in rows), default=0.0)
    lines = []
    if baseline is None:
        lines.append("No baseline for this suite and device - this run captured it."
                     if captured else
                     "No baseline for this suite and device, and none was captured "
                     "(the generator or the tracked kernels are modified).")
        lines.append("  configs: %d, passed the torch gate: %d, worst |diff| %.3e"
                     % (len(rows), len(rows) - len(failed), worst_diff))
        return "\n".join(lines), ("correctness_failure" if failed else
                                  "baseline_captured" if captured else "no_baseline")

    base = {r["id"]: r for r in baseline.get("configs") or []}
    deltas = {}
    bands = {}
    for row in rows:
        old = base.get(row["id"])
        if old and old.get("medianMs") and row.get("medianMs"):
            deltas[row["id"]] = (old["medianMs"] - row["medianMs"]) / old["medianMs"] * 100.0
            spread = max(_spread(row), _spread(old))
            bands[row["id"]] = max(3.0, 200.0 * spread)
    families = {}
    for row in rows:
        if row["id"] in deltas:
            families.setdefault(_family(row["kind"]), []).append(1.0 + deltas[row["id"]] / 100.0)
    fam_gain = {name: (math.prod(values) ** (1.0 / len(values)) - 1.0) * 100.0
                for name, values in families.items() if values}

    lines.append("bench vs baseline (device %s, baseline device %s)"
                 % (doc.get("settings", {}).get("device", "?"),
                    baseline.get("settings", {}).get("device", "?")))
    if previous_hash and previous_hash != generator_hash(generator):
        lines.append("  generator %s -> %s" % (previous_hash, generator_hash(generator)))
    lines.append("  configs: %d, passed the torch gate: %d, worst |diff| %.3e"
                 % (len(rows), len(rows) - len(failed), worst_diff))
    if fam_gain:
        lines.append("  family geomean: " + ", ".join(
            "%s %+.1f%%" % (name, gain) for name, gain in sorted(fam_gain.items())))
    if deltas:
        best = [kv for kv in sorted(deltas.items(), key=lambda kv: -kv[1])
                if kv[1] >= bands.get(kv[0], 3.0)][:4]
        slow = [kv for kv in sorted(deltas.items(), key=lambda kv: kv[1])
                if kv[1] <= -bands.get(kv[0], 3.0)][:4]
        if best:
            lines.append("  fastest movers: " + ", ".join("%s %+.1f%%" % kv for kv in best))
        if slow:
            lines.append("  slowest movers: " + ", ".join("%s %+.1f%%" % kv for kv in slow))
        if not best and not slow:
            lines.append("  every config within its noise band of the baseline")
    if failed:
        first = failed[0]
        lines.append("  CORRECTNESS: %d config(s) failed, first: %s (maxAbs %.3e, firstBad %d)"
                     % (len(failed), first["id"], first["maxAbs"], first["firstBad"]))

    hurt = [name for name, gain in fam_gain.items() if gain < -5.0]
    worst_cfg = min((deltas[k] for k in deltas if deltas[k] <= -bands.get(k, 3.0)), default=0.0)
    improved_fams = [name for name, gain in fam_gain.items() if gain > 3.0]
    if failed:
        verdict, text = "correctness_failure", "CORRECTNESS FAILURE - fix the math before reading speed"
    elif worst_cfg < -10.0:
        worst_id = min(deltas, key=lambda k: deltas[k])
        verdict, text = "regressed", "REGRESSED - %s lost %.1f%%" % (worst_id, -worst_cfg)
    elif hurt:
        verdict, text = "regressed", "REGRESSED - family %s lost more than 5%%" % ",".join(hurt)
    elif len(improved_fams) >= 2:
        verdict, text = "improved", "IMPROVED - %s faster (geomean %+.1f%%)" % (", ".join(sorted(improved_fams)), min(fam_gain.values()) if fam_gain else 0.0)
    else:
        verdict, text = "same", "no significant change"
    lines.append("=> OVERALL: %s" % text)
    return "\n".join(lines), verdict


# ---------------------------------------------------------------------------
# public entry points
# ---------------------------------------------------------------------------

def runBench(generator=GENERATOR_DEFAULT, suite="core", configs=None, kind=None,
             reps=REPS_DEFAULT, warmup=WARMUP_DEFAULT, capture_baseline=False):
    """Run the layer suite for one generator and compare against the pinned baseline."""
    configs = select_configs(suite=suite, configs=configs, kind=kind)
    if not configs:
        raise RuntimeError("no configs selected for suite %r kind %r" % (suite, kind))
    data_root = os.path.join(WORK_DIR, "data", suite)
    os.makedirs(data_root, exist_ok=True)

    gen_info = generate_kernels(generator, configs)
    shim_entries = write_shim(configs)
    manifest = write_manifest(configs, data_root)
    configs_path = os.path.join(WORK_DIR, "configs.json")
    with open(configs_path, "w") as fh:
        json.dump([{k: v for k, v in spec.items() if k not in ("absTol", "relTol")}
                   for spec in configs], fh, indent=1)

    ref = run([sys.executable, rel(BENCH_DIR + "/reference.py"),
               "--configs", configs_path, "--data", data_root], timeout=1800)
    reference = json.loads(ref.stdout[ref.stdout.index("{"):])

    binary, build_warnings = compile_bench()
    key = suite_hash(configs)
    doc = run_bench(binary, manifest, rel(GEN_CL), reps, warmup)

    baseline = load_baseline(key, doc)
    previous_hash = (baseline or {}).get("generatorHash")
    captured = False
    if capture_baseline or (baseline is None and configs is None and not kind):
        if clean_for_capture(generator):
            save_baseline(key, generator, doc)
            captured = True
            baseline = None  # a fresh capture is a new reference point, not a comparison

    summary, verdict = summarize(doc, baseline, generator, previous_hash=previous_hash,
                                 captured=captured)
    return {
        "summary": summary,
        "verdict": verdict,
        "suite": key,
        "configs": len(configs),
        "generator": generator,
        "generatorHash": generator_hash(generator),
        "regenerated": gen_info,
        "reference": {"written": reference.get("written")},
        "buildWarnings": build_warnings[-500:],
        "settings": doc.get("settings"),
        "rows": doc["configs"],
        "aggregate": doc["aggregate"],
        "capturedBaseline": captured,
        "baseline": None if baseline is None else {
            "device": (baseline.get("settings") or {}).get("device"),
            "geomeanMs": (baseline.get("aggregate") or {}).get("geomeanMs")},
        "workDir": WORK_DIR,
    }


def clean_for_capture(generator):
    """True when the generator and the tracked artifacts are unmodified."""
    paths = [generator, "machineLearning/ccnKernel2d.cl", "machineLearning/kernelGen.h"]
    result = subprocess.run(["git", "status", "--porcelain", "--"] + paths,
                            capture_output=True, text=True, cwd=PROJECT_DIR)
    return not result.stdout.strip()


def listConfigs():
    """Suite inventory: ids, shapes and whether a baseline exists per suite."""
    out = {}
    for name, items in SUITES.items():
        if name == "all":
            continue
        out[name] = [{"id": config_id(spec), "kind": spec["kind"],
                      "shape": _shape_text(spec)} for spec in items]
    keys = {}
    for name in out:
        key = suite_hash(select_configs(suite=name))
        keys[name] = {"suiteKey": key,
                      "baseline": load_baseline(key, GENERATOR_DEFAULT) is not None}
    return {"suites": out, "keys": keys, "defaults": {
        "reps": REPS_DEFAULT, "warmup": WARMUP_DEFAULT,
        "absTol": ABS_TOL_DEFAULT, "relTol": REL_TOL_DEFAULT},
        "generator": GENERATOR_DEFAULT, "workDir": WORK_DIR}


def traceConfig(config, generator=GENERATOR_DEFAULT, reps=0):
    """Run ONE config with more reps and return the full row plus the device info."""
    doc = runBench(generator=generator, suite="trace", configs=[config],
                   reps=reps or 50)
    row = doc["rows"][0] if doc["rows"] else None
    return {"summary": doc["summary"], "verdict": doc["verdict"], "settings": doc["settings"],
            "row": row, "generatorHash": doc["generatorHash"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--suite", default="core",
                        help="smoke|core|stress|edges|chain|all (default core)")
    parser.add_argument("--configs", default="", help="comma separated config ids")
    parser.add_argument("--kind", default="", help="filter by layer kind")
    parser.add_argument("--generator", default=GENERATOR_DEFAULT)
    parser.add_argument("--reps", type=int, default=REPS_DEFAULT)
    parser.add_argument("--warmup", type=int, default=WARMUP_DEFAULT)
    parser.add_argument("--capture", action="store_true", help="capture the baseline")
    parser.add_argument("--list", action="store_true", help="list suites and exit")
    parser.add_argument("--json", action="store_true", help="print the full JSON result")
    args = parser.parse_args(argv)

    if args.list:
        print(json.dumps(listConfigs(), indent=2))
        return 0

    configs = [c.strip() for c in args.configs.split(",") if c.strip()] or None
    try:
        result = runBench(generator=args.generator, suite=args.suite, configs=configs,
                          kind=args.kind or None, reps=args.reps, warmup=args.warmup,
                          capture_baseline=args.capture)
    except (RuntimeError, ValueError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(result["summary"])
        print("  device: %s | %s | geomean %.4f ms | generator %s"
              % (result["settings"]["device"], result["settings"]["platform"],
                 result["aggregate"]["geomeanMs"], result["generatorHash"]))
    return 0 if result["verdict"] != "correctness_failure" else 1


if __name__ == "__main__":
    sys.exit(main())
