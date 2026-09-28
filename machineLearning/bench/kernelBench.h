// Shared types for the generated-layer benchmark (kernelBench.c + the generated shim).
//
// The harness (llmOpt/ml_bench.py) writes a manifest.tsv describing one row per
// config and a shim.c that exposes exactly one init/run pair per row. kernelBench
// only knows these types, so the config list stays outside the tracked source.
#ifndef KERNEL_BENCH_H
#define KERNEL_BENCH_H

#include "../../render/gpu/format.h"

#define ML_MAX_ID 256
#define ML_MAX_PATH 512

typedef struct {
	const char *id;        // "conv:w28_h28_c1_k3_n16"
	const char *shape;     // human readable shape, echoed into the JSON
	const char *kind;      // conv | pool | dense | softmax | chain
	int inFloats;
	int outFloats;
	int activation;        // KGenActivation value (see the generated header)
	int accumulate;        // 0 = overwrite output, 1 = add into the seeded output
	int reps;              // 0 = use the CLI default
	float absTol;
	float relTol;
	const char *dataDir;   // holds in.bin / w.bin / b.bin / prev.bin / ref.bin (files optional)
} MlEntry;

typedef int (*MlInitFn)(CL_Context *ctx, const char *clPath, const MlEntry *e);
typedef int (*MlRunFn)(CL_Context *ctx, const MlEntry *e, CL_Buffer *in, CL_Buffer *out);
typedef void (*MlDestroyFn)(void);

// The bench fills each entry's dataDir from the manifest before running it, so the
// generated shim can load w/b/prev files written by the PyTorch reference.
extern MlEntry kMlEntries[];
extern const int kMlEntryCount;
extern const MlInitFn kMlInit[];
extern const MlRunFn kMlRun[];
extern const MlDestroyFn kMlDestroy[];

// Provided by kernelBench.c, used by the generated shim.
int MlLoadFloats(const char *dir, const char *name, float *out, int count);
void MlLogError(const char *fmt, ...);

#endif // KERNEL_BENCH_H
