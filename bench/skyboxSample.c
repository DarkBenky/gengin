#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "skyboxSample.h"

#define FACE_W 2048
#define FACE_H 2048
#define GRID_W 1280
#define GRID_H 720
#define GRID_N (GRID_W * GRID_H)
#define REPS 12
#define THREADS 32

typedef Color (*SkyFn)(const BenchSkybox *, float3);

static const char *gNames[5] = {"V1 original", "V2 floatclamp", "V3 lut", "V4 lut+floatclamp", "V5 noclamp(diag)"};
static SkyFn gFns[5] = {SkyV1, SkyV2, SkyV3, SkyV4, SkyV5};

static BenchSkybox gSbPlain;
static BenchSkybox gSbHuge;
static uint32 *gAllocPlain;
static uint32 *gAllocHuge;
static float3 *gGrid;
static float3 *gScatter;
static uint32 gSink;

static uint32 hash32(uint32 x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static double nowSec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void genDirs(void) {
	/* perspective camera grid: coherent, the escaped primary ray distribution */
	const float tanHalf = 0.57735f;
	for (int y = 0; y < GRID_H; y++) {
		for (int x = 0; x < GRID_W; x++) {
			float px = (2.0f * (float)x / (float)(GRID_W - 1) - 1.0f) * tanHalf;
			float py = (1.0f - 2.0f * (float)y / (float)(GRID_H - 1)) * tanHalf;
			float dx = px, dy = py + 0.55f, dz = 1.0f;
			float len = sqrtf(dx * dx + dy * dy + dz * dz);
			gGrid[y * GRID_W + x] = (float3){dx / len, dy / len, dz / len, 0.0f};
		}
	}

	/* uniform sphere directions: models scattered reflection rays */
	uint32 s = 0x12345678u;
	for (int i = 0; i < GRID_N; i++) {
		s = hash32(s + (uint32)i);
		float z = 2.0f * (float)(s & 0xFFFF) / 65535.0f - 1.0f;
		float a = 6.2831853f * (float)((s >> 16) & 0xFFFF) / 65535.0f;
		float r = sqrtf(1.0f - z * z);
		gScatter[i] = (float3){r * cosf(a), r * sinf(a), z, 0.0f};
	}
}

static uint32 *allocFaces(BenchSkybox *sb, int huge) {
	size_t faceLen = (size_t)FACE_W * FACE_H;
	size_t bytes = faceLen * 6 * sizeof(uint32);
	uint32 *mem = malloc(bytes);
	if (!mem) {
		fprintf(stderr, "alloc failed\n");
		exit(1);
	}
	if (huge) {
		uintptr_t start = (uintptr_t)mem;
		uintptr_t aligned = (start + 4095u) & ~(uintptr_t)4095u;
		size_t advLen = (bytes - (size_t)(aligned - start)) & ~(size_t)4095u;
		if (madvise((void *)aligned, advLen, MADV_HUGEPAGE) != 0) {
			perror("madvise");
			exit(1);
		}
	}
	for (size_t i = 0; i < faceLen * 6; i++) mem[i] = 0xFF000000u | (hash32((uint32)i) & 0x00FFFFFFu);
	sb->front = mem + faceLen * 0;
	sb->back = mem + faceLen * 1;
	sb->left = mem + faceLen * 2;
	sb->right = mem + faceLen * 3;
	sb->top = mem + faceLen * 4;
	sb->bottom = mem + faceLen * 5;
	sb->imageWidth = FACE_W;
	sb->imageHeight = FACE_H;
	return mem;
}

static void reportAnonHuge(const char *tag) {
	FILE *f = fopen("/proc/self/smaps_rollup", "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "AnonHugePages", 13) == 0) printf("  %-10s %s", tag, line);
	}
	fclose(f);
}

typedef struct {
	SkyFn fn;
	const BenchSkybox *sb;
	const float3 *dirs;
	int start, end;
	int reps;
	uint32 sink;
} Work;

static void *runSlice(void *arg) {
	Work *wk = (Work *)arg;
	uint32 acc = 0;
	for (int r = 0; r < wk->reps; r++)
		for (int i = wk->start; i < wk->end; i++) acc += wk->fn(wk->sb, wk->dirs[i]);
	wk->sink = acc;
	return NULL;
}

static double timeSingle(SkyFn fn, const BenchSkybox *sb, const float3 *dirs, int n, int reps) {
	double best = 1e30;
	for (int pass = 0; pass < 3; pass++) {
		double t0 = nowSec();
		uint32 acc = 0;
		for (int r = 0; r < reps; r++)
			for (int i = 0; i < n; i++) acc += fn(sb, dirs[i]);
		double t1 = nowSec();
		gSink += acc;
		if (t1 - t0 < best) best = t1 - t0;
	}
	return best;
}

static double timeThreaded(SkyFn fn, const BenchSkybox *sb, const float3 *dirs, int n, int reps) {
	pthread_t th[THREADS];
	Work wk[THREADS];
	int per = n / THREADS;
	double best = 1e30;
	for (int pass = 0; pass < 3; pass++) {
		double t0 = nowSec();
		for (int t = 0; t < THREADS; t++) {
			wk[t] = (Work){fn, sb, dirs, t * per, (t == THREADS - 1) ? n : (t + 1) * per, reps, 0};
			pthread_create(&th[t], NULL, runSlice, &wk[t]);
		}
		for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL);
		double t1 = nowSec();
		if (t1 - t0 < best) best = t1 - t0;
	}
	for (int t = 0; t < THREADS; t++) gSink += wk[t].sink;
	return best;
}

static void report(const char *label, int n, int reps, double sec) {
	printf("  %-22s %8.2f ns/call\n", label, sec * 1e9 / ((double)n * reps));
}

/* inputs a scene can actually produce: unit directions */
static int validate(void) {
	int bad = 0;
	const int N = 4000000;
	float3 *rnd = malloc(sizeof(float3) * (size_t)N);
	uint32 s = 0x9E3779B9u;
	for (int i = 0; i < N; i++) {
		s = hash32(s + 1u);
		float x = (float)(s & 0xFFF) / 2047.0f - 1.0f;
		s = hash32(s + 2u);
		float y = (float)(s & 0xFFF) / 2047.0f - 1.0f;
		s = hash32(s + 3u);
		float z = (float)(s & 0xFFF) / 2047.0f - 1.0f;
		float len = sqrtf(x * x + y * y + z * z);
		if (len < 1e-6f) len = 1e-6f;
		rnd[i] = (float3){x / len, y / len, z / len, 0.0f};
	}

	for (int v = 1; v < 4; v++) {
		int mism = 0;
		for (int i = 0; i < N && mism < 4; i++) {
			Color a = SkyV1(&gSbPlain, rnd[i]);
			Color b = gFns[v](&gSbPlain, rnd[i]);
			if (a != b) {
				printf("    mismatch %s dir=(%.7g %.7g %.7g) V1=%08x got=%08x\n", gNames[v], rnd[i].x, rnd[i].y, rnd[i].z, a, b);
				mism++;
			}
		}
		for (int i = 0; i < GRID_N; i++) {
			Color a = SkyV1(&gSbPlain, gGrid[i]);
			Color b = gFns[v](&gSbPlain, gGrid[i]);
			if (a != b) mism++;
			Color c = SkyV1(&gSbPlain, gScatter[i]);
			Color d = gFns[v](&gSbPlain, gScatter[i]);
			if (c != d) mism++;
		}
		static const float edge[][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
										{1, 1, 0}, {-1, -1, 0}, {0, 1, 1}, {1, 1, 1}, {-1, 1, -1}, {1e-20f, 0, 0}};
		for (int i = 0; i < (int)(sizeof(edge) / sizeof(edge[0])); i++) {
			float3 dv = {edge[i][0], edge[i][1], edge[i][2], 0.0f};
			Color a = SkyV1(&gSbPlain, dv);
			Color b = gFns[v](&gSbPlain, dv);
			if (a != b) {
				printf("    edge mismatch %s dir=(%g %g %g) V1=%08x got=%08x\n", gNames[v], dv.x, dv.y, dv.z, a, b);
				mism++;
			}
		}
		printf("  %-22s %s (%d mismatches over %d dirs)\n", gNames[v], mism ? "FAIL" : "PASS", mism, N);
		bad += mism;
	}
	free(rnd);
	return bad;
}

int main(void) {
	gAllocPlain = allocFaces(&gSbPlain, 0);
	gAllocHuge = allocFaces(&gSbHuge, 1);
	gGrid = malloc(sizeof(float3) * GRID_N);
	gScatter = malloc(sizeof(float3) * GRID_N);
	genDirs();
	reportAnonHuge("plain+huge");

	printf("skyboxSample: 6 faces %dx%d (%d MB), %d directions per pass, %d reps\n", FACE_W, FACE_H,
		   6 * FACE_W * FACE_H * 4 / (1024 * 1024), GRID_N, REPS);
	printf("validation\n");
	int bad = validate();

	for (int dist = 0; dist < 2; dist++) {
		const float3 *dirs = dist ? gScatter : gGrid;
		const char *dn = dist ? "scattered" : "coherent";
		for (int up = 0; up < 2; up++) {
			const BenchSkybox *sb = up ? &gSbHuge : &gSbPlain;
			const char *un = up ? "hugepages" : "plain    ";
			printf("timing, 1 thread, %s, %s\n", dn, un);
			for (int v = 0; v < 5; v++) report(gNames[v], GRID_N, REPS, timeSingle(gFns[v], sb, dirs, GRID_N, REPS));
			printf("timing, %d threads, %s, %s\n", THREADS, dn, un);
			for (int v = 0; v < 5; v++) report(gNames[v], GRID_N, REPS, timeThreaded(gFns[v], sb, dirs, GRID_N, REPS));
		}
	}

	printf("sink %08x, validation %s\n", gSink, bad ? "FAILED" : "passed");
	free(gAllocPlain);
	free(gAllocHuge);
	free(gGrid);
	free(gScatter);
	return bad ? 1 : 0;
}
