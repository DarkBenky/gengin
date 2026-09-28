// Layer benchmark for the generated kernels (machineLearning).
//
//   build/mlbench/kernelBench --manifest build/mlbench/manifest.tsv \
//                             --cl <generated>.cl [--reps N] [--warmup N] [--json PATH]
//
// One row per config comes from the manifest (written by llmOpt/ml_bench.py); the
// init/run glue for each row is compiled in from a generated shim, so the tracked
// source here stays independent of the config list.  Each config reports init time,
// per-call time (median/p10/p90 over reps), GFLOP/s, GB/s and the deviation from a
// PyTorch reference (max abs/rel, first bad index).
//
// Device: GENGIN_CL_DEVICE (name substring or index) and GENGIN_CL_PLATFORM
// (name substring) override the default, which is the first GPU device found.
//
// stdlib + OpenCL only.
#include "kernelBench.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ML_MAX_CONFIGS 96
#define ML_DEFAULT_REPS 20
#define ML_DEFAULT_WARMUP 2
#define ML_MAX_SHAPE 512

typedef struct {
	char id[ML_MAX_ID];
	char kind[16];
	char shape[ML_MAX_SHAPE];
	int inFloats;
	int outFloats;
	double flops;
	double bytes;
	int activation;
	int accumulate;
	int reps;
	float absTol;
	float relTol;
	char dataDir[ML_MAX_PATH];
} MlConfig;

static MlConfig configs[ML_MAX_CONFIGS];
static int configCount = 0;

typedef struct {
	float *host;        // pinned/mapped host view (NULL when unavailable)
	CL_Buffer buf;
} MlIoBuffer;

void MlLogError(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "kernelBench: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

static void *mlAlloc(size_t bytes) {
	void *p = malloc(bytes);
	if (p == NULL) {
		MlLogError("out of host memory (%zu bytes)", bytes);
		exit(2);
	}
	return p;
}

static double nowMs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int mlCompareDouble(const void *a, const void *b) {
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

// --- device selection -------------------------------------------------------

static int mlContains(const char *haystack, const char *needle) {
	if (needle == NULL || *needle == '\0') {
		return 0;
	}
	size_t n = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		if (strncasecmp(p, needle, n) == 0) {
			return 1;
		}
	}
	return 0;
}

static CL_Context mlCreateContext(char *outPlatform, size_t platformLen,
                                  char *outDevice, size_t deviceLen) {
	CL_Context ctx = {0};
	const char *wantPlatform = getenv("GENGIN_CL_PLATFORM");
	const char *wantDevice = getenv("GENGIN_CL_DEVICE");
	int wantDeviceIndex = -1;
	if (wantDevice != NULL && wantDevice[0] >= '0' && wantDevice[0] <= '9') {
		wantDeviceIndex = atoi(wantDevice);
	}

	cl_platform_id platforms[16];
	cl_uint platformCount = 0;
	if (clGetPlatformIDs(16, platforms, &platformCount) != CL_SUCCESS || platformCount == 0) {
		MlLogError("no OpenCL platforms found");
		return ctx;
	}

	cl_platform_id fallbackPlatform = 0;
	cl_device_id fallbackDevice = 0;
	for (cl_uint p = 0; p < platformCount; p++) {
		char pname[256] = {0};
		clGetPlatformInfo(platforms[p], CL_PLATFORM_NAME, sizeof(pname), pname, NULL);
		if (wantPlatform != NULL && *wantPlatform && !mlContains(pname, wantPlatform)) {
			continue;
		}
		cl_device_id devices[16];
		cl_uint deviceCount = 0;
		if (clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, 16, devices, &deviceCount) != CL_SUCCESS) {
			continue;
		}
		for (cl_uint d = 0; d < deviceCount; d++) {
			char dname[256] = {0};
			clGetDeviceInfo(devices[d], CL_DEVICE_NAME, sizeof(dname), dname, NULL);
			cl_device_type dtype = 0;
			clGetDeviceInfo(devices[d], CL_DEVICE_TYPE, sizeof(dtype), &dtype, NULL);
			int isGpu = (dtype & CL_DEVICE_TYPE_GPU) != 0;
			if (wantDeviceIndex >= 0 && (int)d != wantDeviceIndex) {
				continue;
			}
			if (wantDeviceIndex < 0 && wantDevice != NULL && *wantDevice && !mlContains(dname, wantDevice)) {
				continue;
			}
			if (fallbackDevice == 0) {
				fallbackPlatform = platforms[p];
				fallbackDevice = devices[d];
			}
			if (wantDeviceIndex >= 0 || (wantDevice != NULL && *wantDevice) || isGpu) {
				ctx.platform = platforms[p];
				ctx.device = devices[d];
				snprintf(outPlatform, platformLen, "%s", pname);
				snprintf(outDevice, deviceLen, "%s", dname);
				ctx.context = clCreateContext(NULL, 1, &ctx.device, NULL, NULL, NULL);
				ctx.queue = clCreateCommandQueue(ctx.context, ctx.device, 0, NULL);
				return ctx;
			}
		}
	}
	if (fallbackDevice != 0) {
		ctx.platform = fallbackPlatform;
		ctx.device = fallbackDevice;
		char pname[256] = {0};
		clGetPlatformInfo(ctx.platform, CL_PLATFORM_NAME, sizeof(pname), pname, NULL);
		clGetDeviceInfo(ctx.device, CL_DEVICE_NAME, sizeof(cl_device_type), outDevice, NULL);
		snprintf(outPlatform, platformLen, "%s", pname);
		ctx.context = clCreateContext(NULL, 1, &ctx.device, NULL, NULL, NULL);
		ctx.queue = clCreateCommandQueue(ctx.context, ctx.device, 0, NULL);
		return ctx;
	}
	MlLogError("no OpenCL device matched platform=%s device=%s",
	           wantPlatform ? wantPlatform : "*", wantDevice ? wantDevice : "*");
	return ctx;
}

// --- data ------------------------------------------------------------------

// Used by the generated shim to load weights/bias written by the PyTorch reference.
int MlLoadFloats(const char *dir, const char *name, float *out, int count) {
	char path[ML_MAX_PATH * 2];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *fh = fopen(path, "rb");
	if (fh == NULL) {
		return 0;
	}
	size_t got = fread(out, sizeof(float), (size_t)count, fh);
	fclose(fh);
	if (got != (size_t)count) {
		MlLogError("%s: expected %d floats, read %zu", path, count, got);
		return 0;
	}
	return 1;
}

static int mlReadFloats(const char *path, float *out, int count) {
	FILE *fh = fopen(path, "rb");
	if (fh == NULL) {
		return 0;
	}
	size_t got = fread(out, sizeof(float), (size_t)count, fh);
	fclose(fh);
	return got == (size_t)count;
}

static int mlBufferAlloc(CL_Context *ctx, MlIoBuffer *io, int floats) {
	size_t bytes = (size_t)floats * sizeof(float);
	io->host = NULL;
	io->buf = CL_Buffer_CreatePinned(ctx, bytes, CL_MEM_READ_WRITE);
	if (io->buf.buf != NULL) {
		return 0;
	}
	// Pinned allocation can be unavailable (no host-visible memory): fall back to a
	// plain device buffer plus explicit transfers.
	io->buf = CL_Buffer_Create(ctx, bytes, CL_MEM_READ_WRITE);
	io->host = NULL;
	return io->buf.buf == NULL ? 1 : 0;
}

// Host -> device. With a pinned buffer the write goes through a map, which must be
// unmapped before the next dispatch: the device only sees mapped writes on unmap.
static int mlBufferUpload(CL_Context *ctx, MlIoBuffer *io, const float *data, int floats) {
	size_t bytes = (size_t)floats * sizeof(float);
	if (io->buf.buf == NULL) {
		return 1;
	}
	if (io->buf.flags & CL_MEM_ALLOC_HOST_PTR) {
		float *view = CL_Buffer_Map(ctx, &io->buf, CL_MAP_WRITE_INVALIDATE_REGION);
		if (view == NULL) {
			return 1;
		}
		memcpy(view, data, bytes);
		CL_Buffer_Unmap(ctx, &io->buf, view);
		return 0;
	}
	CL_Buffer_Write(ctx, &io->buf, (void *)data, bytes);
	return 0;
}

// Device -> host (call after CL_Finish).
static int mlBufferDownload(CL_Context *ctx, MlIoBuffer *io, float *out, int floats) {
	size_t bytes = (size_t)floats * sizeof(float);
	if (io->buf.buf == NULL) {
		return 1;
	}
	if (io->buf.flags & CL_MEM_ALLOC_HOST_PTR) {
		float *view = CL_Buffer_Map(ctx, &io->buf, CL_MAP_READ);
		if (view == NULL) {
			return 1;
		}
		memcpy(out, view, bytes);
		CL_Buffer_Unmap(ctx, &io->buf, view);
		return 0;
	}
	CL_Buffer_Read(ctx, &io->buf, out, bytes);
	return 0;
}

static void mlBufferFree(CL_Context *ctx, MlIoBuffer *io) {
	if (io->host != NULL) {
		CL_Buffer_Unmap(ctx, &io->buf, io->host);
		io->host = NULL;
	}
	CL_Buffer_Destroy(&io->buf);
}

// --- manifest --------------------------------------------------------------

static int mlReadManifest(const char *path) {
	FILE *fh = fopen(path, "r");
	if (fh == NULL) {
		MlLogError("cannot open manifest %s", path);
		return 1;
	}
	char line[4096];
	while (fgets(line, sizeof(line), fh) != NULL && configCount < ML_MAX_CONFIGS) {
		if (line[0] == '#' || line[0] == '\n') {
			continue;
		}
		MlConfig *c = &configs[configCount];
		char dataDir[ML_MAX_PATH] = {0};
		int fields = sscanf(line,
		                    "%255s\t%15s\t%511s\t%d\t%d\t%lf\t%lf\t%d\t%d\t%d\t%f\t%f\t%255s",
		                    c->id, c->kind, c->shape, &c->inFloats, &c->outFloats, &c->flops,
		                    &c->bytes, &c->activation, &c->accumulate, &c->reps, &c->absTol,
		                    &c->relTol, dataDir);
		if (fields < 12) {
			MlLogError("bad manifest row (%d fields): %s", fields, line);
			continue;
		}
		snprintf(c->dataDir, sizeof(c->dataDir), "%s", fields >= 13 ? dataDir : "");
		configCount++;
	}
	fclose(fh);
	return configCount == 0 ? 1 : 0;
}

// --- one config ------------------------------------------------------------

typedef struct {
	const char *id;
	const char *shape;
	const char *kind;
	int ok;
	char error[192];
	double initMs;
	double medianMs;
	double p10Ms;
	double p90Ms;
	double gflops;
	double gbps;
	double maxAbs;
	double maxRel;
	int firstBad;
	int outFloats;
	int timed;
} MlResult;

static int mlIndexForId(const char *id) {
	for (int i = 0; i < kMlEntryCount; i++) {
		if (strcmp(kMlEntries[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

static void mlRunConfig(CL_Context *ctx, const char *clPath, const MlConfig *cfg,
                        int warmup, int defaultReps, MlResult *res) {
	res->id = cfg->id;
	res->shape = cfg->shape;
	res->kind = cfg->kind;
	res->firstBad = -1;
	res->outFloats = cfg->outFloats;
	res->error[0] = '\0';

	int index = mlIndexForId(cfg->id);
	if (index < 0) {
		snprintf(res->error, sizeof(res->error), "no shim entry for %s", cfg->id);
		return;
	}
	const MlEntry *entry = &kMlEntries[index];
	if (entry->outFloats != cfg->outFloats || entry->inFloats != cfg->inFloats) {
		snprintf(res->error, sizeof(res->error), "shim/manifest size mismatch for %s", cfg->id);
		return;
	}
	// The shim loads weights/bias relative to the entry's data dir.
	kMlEntries[index].dataDir = cfg->dataDir;

	double t0 = nowMs();
	if (kMlInit[index](ctx, clPath, entry) != 0) {
		snprintf(res->error, sizeof(res->error), "init failed (kernel build or weights)");
		return;
	}
	res->initMs = nowMs() - t0;

	MlIoBuffer in = {0}, out = {0};
	if (mlBufferAlloc(ctx, &in, entry->inFloats) != 0 ||
	    mlBufferAlloc(ctx, &out, entry->outFloats) != 0) {
		snprintf(res->error, sizeof(res->error), "buffer allocation failed");
		mlBufferFree(ctx, &in);
		mlBufferFree(ctx, &out);
		kMlDestroy[index]();
		return;
	}

	float *hostIn = (float *)mlAlloc((size_t)entry->inFloats * sizeof(float));
	char path[ML_MAX_PATH * 2];
	snprintf(path, sizeof(path), "%s/in.bin", cfg->dataDir);
	if (!mlReadFloats(path, hostIn, entry->inFloats)) {
		snprintf(res->error, sizeof(res->error), "missing input %s", path);
		free(hostIn);
		mlBufferFree(ctx, &in);
		mlBufferFree(ctx, &out);
		kMlDestroy[index]();
		return;
	}
	if (mlBufferUpload(ctx, &in, hostIn, entry->inFloats) != 0) {
		snprintf(res->error, sizeof(res->error), "input upload failed");
	}
	CL_Finish(ctx);

	int reps = cfg->reps > 0 ? cfg->reps : defaultReps;
	double *times = (double *)mlAlloc((size_t)reps * sizeof(double));
	for (int i = 0; i < warmup; i++) {
		kMlRun[index](ctx, entry, &in.buf, &out.buf);
	}
	CL_Finish(ctx);
	for (int i = 0; i < reps; i++) {
		CL_Finish(ctx);
		double start = nowMs();
		kMlRun[index](ctx, entry, &in.buf, &out.buf);
		CL_Finish(ctx);
		times[i] = nowMs() - start;
	}
	qsort(times, (size_t)reps, sizeof(double), mlCompareDouble);
	res->medianMs = times[reps / 2];
	res->p10Ms = times[reps / 10];
	res->p90Ms = times[(reps * 9) / 10];
	res->timed = reps;
	if (res->medianMs > 0.0) {
		res->gflops = cfg->flops / (res->medianMs * 1e6);
		res->gbps = cfg->bytes / (res->medianMs * 1e6);
	}

	// Correctness: fresh output (seeded with prev.bin when accumulate) vs reference.
	{
		float *zeros = (float *)calloc((size_t)entry->outFloats, sizeof(float));
		mlBufferUpload(ctx, &out, zeros, entry->outFloats);
		free(zeros);
	}
	if (entry->accumulate) {
		float *prev = (float *)mlAlloc((size_t)entry->outFloats * sizeof(float));
		snprintf(path, sizeof(path), "%s/prev.bin", cfg->dataDir);
		if (mlReadFloats(path, prev, entry->outFloats)) {
			mlBufferUpload(ctx, &out, prev, entry->outFloats);
		} else {
			snprintf(res->error, sizeof(res->error), "accumulate config without prev.bin");
		}
		free(prev);
	}
	CL_Finish(ctx);
	kMlRun[index](ctx, entry, &in.buf, &out.buf);
	CL_Finish(ctx);

	float *got = (float *)mlAlloc((size_t)entry->outFloats * sizeof(float));
	if (mlBufferDownload(ctx, &out, got, entry->outFloats) != 0) {
		snprintf(res->error, sizeof(res->error), "output readback failed");
	}
	if (getenv("ML_DEBUG") != NULL) {
		fprintf(stderr, "    %s got[0..3] = %.4f %.4f %.4f %.4f\n", cfg->id,
		        got[0], got[1], got[2], got[3]);
	}
	float *want = (float *)mlAlloc((size_t)entry->outFloats * sizeof(float));
	snprintf(path, sizeof(path), "%s/ref.bin", cfg->dataDir);
	if (!mlReadFloats(path, want, entry->outFloats)) {
		snprintf(res->error, sizeof(res->error), "missing reference %s", path);
	} else {
		double maxAbs = 0.0, maxRel = 0.0;
		for (int i = 0; i < entry->outFloats; i++) {
			double diff = fabs((double)got[i] - (double)want[i]);
			double scale = fabs((double)want[i]);
			if (diff > maxAbs) {
				maxAbs = diff;
			}
			if (diff > cfg->absTol + cfg->relTol * scale) {
				if (res->firstBad < 0) {
					res->firstBad = i;
				}
			}
			if (scale > 1e-6) {
				double rel = diff / scale;
				if (rel > maxRel) {
					maxRel = rel;
				}
			}
		}
		res->maxAbs = maxAbs;
		res->maxRel = maxRel;
		res->ok = res->firstBad < 0 && res->error[0] == '\0';
	}

	free(hostIn);
	free(times);
	free(got);
	free(want);
	mlBufferFree(ctx, &in);
	mlBufferFree(ctx, &out);
	kMlDestroy[index]();
}

// --- report ----------------------------------------------------------------

int main(int argc, char **argv) {
	const char *manifest = NULL;
	const char *clPath = NULL;
	const char *jsonPath = NULL;
	int reps = ML_DEFAULT_REPS;
	int warmup = ML_DEFAULT_WARMUP;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
			manifest = argv[++i];
		} else if (strcmp(argv[i], "--cl") == 0 && i + 1 < argc) {
			clPath = argv[++i];
		} else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
			reps = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
			warmup = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
			jsonPath = argv[++i];
		} else {
			fprintf(stderr, "usage: kernelBench --manifest FILE --cl FILE [--reps N] "
			                "[--warmup N] [--json PATH]\n");
			return 2;
		}
	}
	if (manifest == NULL || clPath == NULL) {
		fprintf(stderr, "usage: kernelBench --manifest FILE --cl FILE [--reps N] "
		                "[--warmup N] [--json PATH]\n");
		return 2;
	}
	if (reps < 1 || reps > 10000) {
		MlLogError("--reps must be 1..10000");
		return 2;
	}
	if (mlReadManifest(manifest) != 0) {
		return 2;
	}

	FILE *clFile = fopen(clPath, "rb");
	if (clFile == NULL) {
		MlLogError("cannot open kernel source %s", clPath);
		return 2;
	}
	fclose(clFile);

	if (jsonPath != NULL && freopen(jsonPath, "w", stdout) == NULL) {
		MlLogError("cannot write %s", jsonPath);
		return 2;
	}

	char platformName[256] = {0};
	char deviceName[256] = {0};
	CL_Context ctx = mlCreateContext(platformName, sizeof(platformName),
	                                 deviceName, sizeof(deviceName));
	if (ctx.context == NULL || ctx.queue == NULL) {
		return 2;
	}

	MlResult results[ML_MAX_CONFIGS];
	for (int i = 0; i < configCount; i++) {
		mlRunConfig(&ctx, clPath, &configs[i], warmup, reps, &results[i]);
		if (results[i].ok) {
			fprintf(stderr, "  %-34s %8.3f ms  %7.1f GFLOP/s  init %6.1f ms\n",
			        results[i].id, results[i].medianMs, results[i].gflops, results[i].initMs);
		} else {
			fprintf(stderr, "  %-34s FAILED: %s%s\n", results[i].id,
			        results[i].error[0] ? results[i].error : "deviation over tolerance",
			        results[i].firstBad >= 0 ? " (first bad element shown in JSON)" : "");
		}
	}

	// --- JSON report: stdout, or the file when --json was given -----------------
	printf("{\n");
	printf("  \"version\": 1,\n");
	printf("  \"settings\": {\"platform\": \"%s\", \"device\": \"%s\", \"reps\": %d, "
	       "\"warmup\": %d, \"manifest\": \"%s\"},\n",
	       platformName, deviceName, reps, warmup, manifest);
	printf("  \"configs\": [\n");
	double logSumMs = 0.0, logSumFlops = 0.0;
	int logCount = 0, passCount = 0, timeCount = 0;
	for (int i = 0; i < configCount; i++) {
		MlResult *r = &results[i];
		if (r->ok) {
			passCount++;
		}
		if (r->timed > 0 && r->medianMs > 0.0) {
			logSumMs += log(r->medianMs);
			logSumFlops += log(r->gflops > 0.0 ? r->gflops : 1e-12);
			logCount++;
			timeCount++;
		}
		printf("    {\"id\": \"%s\", \"kind\": \"%s\", \"shape\": \"%s\", \"ok\": %s, "
		       "\"error\": \"%s\", \"initMs\": %.3f, \"medianMs\": %.6f, \"p10Ms\": %.6f, "
		       "\"p90Ms\": %.6f, \"gflops\": %.4f, \"gbps\": %.4f, \"maxAbs\": %.3e, "
		       "\"maxRel\": %.3e, \"firstBad\": %d, \"outFloats\": %d}%s\n",
		       r->id, r->kind, r->shape, r->ok ? "true" : "false", r->error,
		       r->initMs, r->medianMs, r->p10Ms, r->p90Ms, r->gflops, r->gbps,
		       r->maxAbs, r->maxRel, r->firstBad, r->outFloats,
		       (i + 1 < configCount) ? "," : "");
	}
	printf("  ],\n");
	printf("  \"aggregate\": {\"configs\": %d, \"passed\": %d, \"timed\": %d, "
	       "\"geomeanMs\": %.6f, \"geomeanGflops\": %.4f}\n",
	       configCount, passCount, timeCount,
	       logCount ? exp(logSumMs / logCount) : 0.0,
	       logCount ? exp(logSumFlops / logCount) : 0.0);
	printf("}\n");
	fflush(stdout);
	CL_Context_Destroy(&ctx);
	return passCount == configCount ? 0 : 1;
}
