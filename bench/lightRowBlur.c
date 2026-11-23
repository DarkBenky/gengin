#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "lightRowBlur.h"

#define WIDTH 1280
#define ROWSTEP 4

static double nowSec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void LightRowPassV1(const float3 *restrict reflA, const float3 *restrict reflB, const float3 *restrict emitA,
					const float3 *restrict emitB, const float3 *restrict shadA, const float3 *restrict shadB,
					int width, int rows, int rowStep, float3 *restrict accC, float3 *restrict accE, float3 *restrict accS) {
	float3 winC[LRB_WIN], winE[LRB_WIN], winS[LRB_WIN];

	for (int row = 0; row < rows; row += rowStep) {
		int rowEnd = row + rowStep < rows ? row + rowStep : rows;
		for (int x0 = 0; x0 < width; x0 += LRB_TILE) {
			int x1 = x0 + LRB_TILE < width ? x0 + LRB_TILE : width;
			int w0 = x0 - LRB_BLUR_RADIUS < 0 ? 0 : x0 - LRB_BLUR_RADIUS;
			int w1 = x1 + LRB_BLUR_RADIUS > width ? width : x1 + LRB_BLUR_RADIUS;
			int n = x1 - x0;
			for (int r = row; r < rowEnd; r++) {
				const float3 *srcC = reflA, *srcE = emitA, *srcS = shadA;
				int srcOff = 0;
				if (r != row) {
					float t1 = (float)(r - row) / (float)rowStep;
					float t0 = 1.0f - t1;
					lrbBlendV1(winC, reflA + w0, reflB + w0, w1 - w0, t0, t1);
					lrbBlendV1(winE, emitA + w0, emitB + w0, w1 - w0, t0, t1);
					lrbBlendV1(winS, shadA + w0, shadB + w0, w1 - w0, t0, t1);
					srcC = winC;
					srcE = winE;
					srcS = winS;
					srcOff = w0;
				}
				lrbBlurV1(srcC + (x0 - srcOff), n, accC + x0);
				lrbBlurV1(srcE + (x0 - srcOff), n, accE + x0);
				lrbBlurV1(srcS + (x0 - srcOff), n, accS + x0);
			}
		}
	}
}

void LightRowPassV2(const float3 *restrict reflA, const float3 *restrict reflB, const float3 *restrict emitA,
					const float3 *restrict emitB, const float3 *restrict shadA, const float3 *restrict shadB,
					int width, int rows, int rowStep, float3 *restrict accC, float3 *restrict accE, float3 *restrict accS) {
	float3 blurCA[LRB_WIN], blurEA[LRB_WIN], blurSA[LRB_WIN];
	float3 blurCB[LRB_WIN], blurEB[LRB_WIN], blurSB[LRB_WIN];

	for (int row = 0; row < rows; row += rowStep) {
		int rowEnd = row + rowStep < rows ? row + rowStep : rows;
		for (int x0 = 0; x0 < width; x0 += LRB_TILE) {
			int x1 = x0 + LRB_TILE < width ? x0 + LRB_TILE : width;
			int w0 = x0 - LRB_BLUR_RADIUS < 0 ? 0 : x0 - LRB_BLUR_RADIUS;
			int w1 = x1 + LRB_BLUR_RADIUS > width ? width : x1 + LRB_BLUR_RADIUS;
			int n = x1 - x0;
			int wn = w1 - w0;
			lrbBlurV1(reflA + w0, wn, blurCA);
			lrbBlurV1(emitA + w0, wn, blurEA);
			lrbBlurV1(shadA + w0, wn, blurSA);
			int hasB = rowEnd - row > 1;
			if (hasB) {
				lrbBlurV1(reflB + w0, wn, blurCB);
				lrbBlurV1(emitB + w0, wn, blurEB);
				lrbBlurV1(shadB + w0, wn, blurSB);
			}
			for (int r = row; r < rowEnd; r++) {
				if (r == row || !hasB) {
					memcpy(accC + x0, blurCA + LRB_BLUR_RADIUS, (size_t)n * sizeof(float3));
					memcpy(accE + x0, blurEA + LRB_BLUR_RADIUS, (size_t)n * sizeof(float3));
					memcpy(accS + x0, blurSA + LRB_BLUR_RADIUS, (size_t)n * sizeof(float3));
				} else {
					float t1 = (float)(r - row) / (float)rowStep;
					float t0 = 1.0f - t1;
					lrbBlendV1(accC + x0, blurCA + LRB_BLUR_RADIUS, blurCB + LRB_BLUR_RADIUS, n, t0, t1);
					lrbBlendV1(accE + x0, blurEA + LRB_BLUR_RADIUS, blurEB + LRB_BLUR_RADIUS, n, t0, t1);
					lrbBlendV1(accS + x0, blurSA + LRB_BLUR_RADIUS, blurSB + LRB_BLUR_RADIUS, n, t0, t1);
				}
			}
		}
	}
}

static float3 *allocBuf(size_t n) {
	float3 *p = malloc(n * sizeof(float3));
	if (!p) exit(1);
	return p;
}

static void fill(float3 *p, size_t n, uint32 seed) {
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1664525u + 1013904223u;
		float a = (float)((seed >> 8) & 0xFFFF) / 65535.0f;
		seed = seed * 1664525u + 1013904223u;
		float b = (float)((seed >> 8) & 0xFFFF) / 65535.0f;
		seed = seed * 1664525u + 1013904223u;
		float c = (float)((seed >> 8) & 0xFFFF) / 65535.0f;
		p[i] = (float3){a, b, c, 0.0f};
	}
}

int main(void) {
	size_t n = (size_t)WIDTH;
	size_t np = n + 2 * LRB_BLUR_RADIUS;
	float3 *reflA = allocBuf(np) + LRB_BLUR_RADIUS, *reflB = allocBuf(np) + LRB_BLUR_RADIUS;
	float3 *emitA = allocBuf(np) + LRB_BLUR_RADIUS, *emitB = allocBuf(np) + LRB_BLUR_RADIUS;
	float3 *shadA = allocBuf(np) + LRB_BLUR_RADIUS, *shadB = allocBuf(np) + LRB_BLUR_RADIUS;
	float3 *accC1 = allocBuf(n), *accE1 = allocBuf(n), *accS1 = allocBuf(n);
	float3 *accC2 = allocBuf(n), *accE2 = allocBuf(n), *accS2 = allocBuf(n);
	fill(reflA, n, 1u);
	fill(reflB, n, 2u);
	fill(emitA, n, 3u);
	fill(emitB, n, 4u);
	fill(shadA, n, 5u);
	fill(shadB, n, 6u);

	LightRowPassV1(reflA, reflB, emitA, emitB, shadA, shadB, WIDTH, 720, ROWSTEP, accC1, accE1, accS1);
	LightRowPassV2(reflA, reflB, emitA, emitB, shadA, shadB, WIDTH, 720, ROWSTEP, accC2, accE2, accS2);

	double maxC = 0.0, maxE = 0.0, maxS = 0.0, mse = 0.0;
	for (size_t i = 0; i < n; i++) {
		float d;
		d = fabsf(accC1[i].x - accC2[i].x); if (d > maxC) maxC = d; mse += d * d;
		d = fabsf(accC1[i].y - accC2[i].y); if (d > maxC) maxC = d; mse += d * d;
		d = fabsf(accC1[i].z - accC2[i].z); if (d > maxC) maxC = d; mse += d * d;
		d = fabsf(accE1[i].x - accE2[i].x); if (d > maxE) maxE = d; mse += d * d;
		d = fabsf(accE1[i].y - accE2[i].y); if (d > maxE) maxE = d; mse += d * d;
		d = fabsf(accE1[i].z - accE2[i].z); if (d > maxE) maxE = d; mse += d * d;
		d = fabsf(accS1[i].x - accS2[i].x); if (d > maxS) maxS = d; mse += d * d;
		d = fabsf(accS1[i].y - accS2[i].y); if (d > maxS) maxS = d; mse += d * d;
		d = fabsf(accS1[i].z - accS2[i].z); if (d > maxS) maxS = d; mse += d * d;
	}
	printf("validation: max abs diff C %.3e E %.3e S %.3e, mse %.3e\n", maxC, maxE, maxS, mse / (double)(n * 9));

	double best1 = 1e30, best2 = 1e30;
	for (int pass = 0; pass < 5; pass++) {
		double t0 = nowSec();
		LightRowPassV1(reflA, reflB, emitA, emitB, shadA, shadB, WIDTH, 720, ROWSTEP, accC1, accE1, accS1);
		double t1 = nowSec() - t0;
		t0 = nowSec();
		LightRowPassV2(reflA, reflB, emitA, emitB, shadA, shadB, WIDTH, 720, ROWSTEP, accC2, accE2, accS2);
		double t2 = nowSec() - t0;
		if (t1 < best1) best1 = t1;
		if (t2 < best2) best2 = t2;
	}
	double px = (double)WIDTH * 720.0;
	printf("V1 blend-then-blur : %.3f ms  (%.3f ns/px)\n", best1 * 1e3, best1 * 1e9 / px);
	printf("V2 blur-then-blend : %.3f ms  (%.3f ns/px)  delta %.1f%%\n", best2 * 1e3, best2 * 1e9 / px, (best2 / best1 - 1.0) * 100.0);
	printf("sink %08x\n", (unsigned)((unsigned)accC2[7].x * 1e6) ^ (unsigned)((unsigned)accS2[11].y * 1e6));
	return 0;
}
