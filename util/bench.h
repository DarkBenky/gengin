#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef BENCH_DURATION
#define BENCH_DURATION 5.0
#endif

#ifndef BENCH_WARMUP
#define BENCH_WARMUP 2.0
#endif

#ifndef BENCH_HASH_FRAMES
#define BENCH_HASH_FRAMES 10
#endif

#define BENCH_MAX_FRAMES 100000

typedef struct {
	double *times;
	int count;
	struct timespec start;
	struct timespec frameStart;
	uint32_t frameHashes[BENCH_HASH_FRAMES];
	uint8_t *framePixels[BENCH_HASH_FRAMES];
	int frameWidth;
	int frameHeight;
	int hashCount;
} Bench;

#ifdef BENCH_MODE

static inline uint32_t benchFnv1a(const uint32_t *pixels, int count) {
	uint32_t h = 2166136261u;
	const uint8_t *p = (const uint8_t *)pixels;
	int bytes = count * 4;
	for (int i = 0; i < bytes; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static inline int benchCmpDouble(const void *a, const void *b) {
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

static inline double benchDiff(struct timespec a, struct timespec b) {
	return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
}

static const char benchBase64Table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline char *benchBase64Encode(const uint8_t *data, int len) {
	int outLen = ((len + 2) / 3) * 4;
	char *out = malloc(outLen + 1);
	if (!out) return NULL;
	int i = 0, j = 0;
	while (i < len) {
		uint32_t t = ((uint32_t)data[i] << 16)
		           | (i+1 < len ? (uint32_t)data[i+1] << 8 : 0)
		           | (i+2 < len ? (uint32_t)data[i+2]      : 0);
		out[j++] = benchBase64Table[(t >> 18) & 0x3F];
		out[j++] = benchBase64Table[(t >> 12) & 0x3F];
		out[j++] = (i+1 < len) ? benchBase64Table[(t >> 6) & 0x3F] : '=';
		out[j++] = (i+2 < len) ? benchBase64Table[t        & 0x3F] : '=';
		i += 3;
	}
	out[j] = '\0';
	return out;
}

static inline void benchInit(Bench *b) {
	b->times = malloc(sizeof(double) * BENCH_MAX_FRAMES);
	b->count = 0;
	b->hashCount = 0;
	b->frameWidth = 0;
	b->frameHeight = 0;
	for (int i = 0; i < BENCH_HASH_FRAMES; i++)
		b->framePixels[i] = NULL;
	clock_gettime(CLOCK_MONOTONIC, &b->start);
}

static inline void benchCaptureFrame(Bench *b, const uint32_t *pixels, int width, int height) {
	if (b->hashCount >= BENCH_HASH_FRAMES) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (benchDiff(b->start, now) < BENCH_WARMUP) return;
	int pixelCount = width * height;
	b->frameWidth = width;
	b->frameHeight = height;
	b->frameHashes[b->hashCount] = benchFnv1a(pixels, pixelCount);
	int bytes = pixelCount * 4;
	b->framePixels[b->hashCount] = malloc(bytes);
	if (b->framePixels[b->hashCount])
		memcpy(b->framePixels[b->hashCount], pixels, bytes);
	b->hashCount++;
}

static inline void benchFrameStart(Bench *b) {
	clock_gettime(CLOCK_MONOTONIC, &b->frameStart);
}

static inline int benchFrameEnd(Bench *b) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = benchDiff(b->start, now);
	if (elapsed >= BENCH_WARMUP && b->count < BENCH_MAX_FRAMES)
		b->times[b->count++] = benchDiff(b->frameStart, now) * 1000.0;
	return elapsed >= BENCH_WARMUP + BENCH_DURATION;
}

static inline void benchReport(Bench *b) {
	if (b->count == 0) return;
	qsort(b->times, b->count, sizeof(double), benchCmpDouble);
	double sum = 0.0;
	for (int i = 0; i < b->count; i++)
		sum += b->times[i];
	double avg = sum / b->count;
	double median = (b->count & 1)
						? b->times[b->count / 2]
						: (b->times[b->count / 2 - 1] + b->times[b->count / 2]) * 0.5;
	int p99idx = (int)(0.99 * b->count);
	if (p99idx >= b->count) p99idx = b->count - 1;
	double p99 = b->times[p99idx];
	printf("\nBench results (%d frames, %.1f s)\n", b->count, (double)BENCH_DURATION);
	printf("  avg    : %.3f ms\n", avg);
	printf("  median : %.3f ms\n", median);
	printf("  p99    : %.3f ms\n", p99);
	FILE *f = fopen("bench_results.json", "w");
	if (f) {
		fprintf(f, "{\n");
		fprintf(f, "  \"frames\": %d,\n", b->count);
		fprintf(f, "  \"duration_s\": %.1f,\n", (double)BENCH_DURATION);
		fprintf(f, "  \"avg_ms\": %.3f,\n", avg);
		fprintf(f, "  \"median_ms\": %.3f,\n", median);
		fprintf(f, "  \"p99_ms\": %.3f,\n", p99);
		fprintf(f, "  \"frame_width\": %d,\n", b->frameWidth);
		fprintf(f, "  \"frame_height\": %d,\n", b->frameHeight);
		fprintf(f, "  \"frame_hashes\": [");
		for (int i = 0; i < b->hashCount; i++) {
			if (i > 0) fprintf(f, ", ");
			fprintf(f, "\"0x%08x\"", b->frameHashes[i]);
		}
		fprintf(f, "],\n");
		fprintf(f, "  \"frame_images\": [");
		for (int i = 0; i < b->hashCount; i++) {
			if (i > 0) fprintf(f, ", ");
			if (b->framePixels[i]) {
				char *b64 = benchBase64Encode(b->framePixels[i], b->frameWidth * b->frameHeight * 4);
				if (b64) {
					fprintf(f, "\"%s\"", b64);
					free(b64);
				} else {
					fprintf(f, "null");
				}
			} else {
				fprintf(f, "null");
			}
		}
		fprintf(f, "]\n");
		fprintf(f, "}\n");
		fclose(f);
		printf("  saved  : bench_results.json\n");
	}
}

static inline void benchFree(Bench *b) {
	free(b->times);
	b->times = NULL;
	for (int i = 0; i < BENCH_HASH_FRAMES; i++) {
		free(b->framePixels[i]);
		b->framePixels[i] = NULL;
	}
}

#else

static inline void benchInit(Bench *b) {
	(void)b;
}
static inline void benchCaptureFrame(Bench *b, const uint32_t *pixels, int width, int height) {
	(void)b;
	(void)pixels;
	(void)width;
	(void)height;
}
static inline void benchFrameStart(Bench *b) {
	(void)b;
}
static inline int benchFrameEnd(Bench *b) {
	(void)b;
	return 0;
}
static inline void benchReport(Bench *b) {
	(void)b;
}
static inline void benchFree(Bench *b) {
	(void)b;
}

#endif
