#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef BENCH_DURATION
#define BENCH_DURATION 5.0
#endif

#define BENCH_MAX_FRAMES 100000

typedef struct {
	double *times;
	int count;
	struct timespec start;
	struct timespec frameStart;
} Bench;

#ifdef BENCH_MODE

static inline int benchCmpDouble(const void *a, const void *b) {
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

static inline double benchDiff(struct timespec a, struct timespec b) {
	return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
}

static inline void benchInit(Bench *b) {
	b->times = malloc(sizeof(double) * BENCH_MAX_FRAMES);
	b->count = 0;
	clock_gettime(CLOCK_MONOTONIC, &b->start);
}

static inline void benchFrameStart(Bench *b) {
	clock_gettime(CLOCK_MONOTONIC, &b->frameStart);
}

static inline int benchFrameEnd(Bench *b) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (b->count < BENCH_MAX_FRAMES)
		b->times[b->count++] = benchDiff(b->frameStart, now) * 1000.0;
	return benchDiff(b->start, now) >= BENCH_DURATION;
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
		fprintf(f, "  \"p99_ms\": %.3f\n", p99);
		fprintf(f, "}\n");
		fclose(f);
		printf("  saved  : bench_results.json\n");
	}
}

static inline void benchFree(Bench *b) {
	free(b->times);
	b->times = NULL;
}

#else

static inline void benchInit(Bench *b) {
	(void)b;
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
