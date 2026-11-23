#ifndef LIGHT_ROW_BLUR_BENCH_H
#define LIGHT_ROW_BLUR_BENCH_H

#include <immintrin.h>
#include <math.h>
#include <string.h>
#include "../object/format.h"

#define LRB_BLUR_RADIUS 3
#define LRB_TILE 256
#define LRB_WIN (LRB_TILE + 2 * LRB_BLUR_RADIUS)

/* V1: the shipped pipeline - blend the two slices into the window, then blur
   the window (one blur per row, four rows per slice block). */
static inline void lrbBlendV1(float3 *win, const float3 *a, const float3 *b, int n, float t0, float t1) {
	int i = 0;
	for (; i + 2 <= n; i += 2) {
		__m256 va = _mm256_loadu_ps((const float *)(a + i));
		__m256 vb = _mm256_loadu_ps((const float *)(b + i));
		_mm256_storeu_ps((float *)(win + i), _mm256_fmadd_ps(vb, _mm256_set1_ps(t1), _mm256_mul_ps(va, _mm256_set1_ps(t0))));
	}
	for (; i < n; i++)
		win[i] = (float3){a[i].x * t0 + b[i].x * t1, a[i].y * t0 + b[i].y * t1, a[i].z * t0 + b[i].z * t1, 0.0f};
}

static inline void lrbBlurV1(const float3 *src, int n, float3 *out) {
	const __m256 invW = _mm256_set1_ps(1.0f / (float)(LRB_BLUR_RADIUS * 2 + 1));
	int x = 0;
	for (; x + 2 <= n; x += 2) {
		__m256 acc = _mm256_setzero_ps();
		for (int d = LRB_BLUR_RADIUS; d >= -LRB_BLUR_RADIUS; d--) acc = _mm256_add_ps(acc, _mm256_loadu_ps((const float *)(src + x + d)));
		_mm256_storeu_ps((float *)(out + x), _mm256_mul_ps(acc, invW));
	}
	for (; x < n; x++) {
		float3 s = {0.0f, 0.0f, 0.0f, 0.0f};
		for (int d = -LRB_BLUR_RADIUS; d <= LRB_BLUR_RADIUS; d++) s = (float3){s.x + src[x + d].x, s.y + src[x + d].y, s.z + src[x + d].z, 0.0f};
		out[x] = (float3){s.x / 7.0f, s.y / 7.0f, s.z / 7.0f, 0.0f};
	}
}

void LightRowPassV1(const float3 *restrict reflA, const float3 *restrict reflB, const float3 *restrict emitA,
					const float3 *restrict emitB, const float3 *restrict shadA, const float3 *restrict shadB,
					int width, int rows, int rowStep, float3 *restrict accC, float3 *restrict accE, float3 *restrict accS);

/* V2: blur each slice once per block, then blend the blurred windows. */
void LightRowPassV2(const float3 *restrict reflA, const float3 *restrict reflB, const float3 *restrict emitA,
					const float3 *restrict emitB, const float3 *restrict shadA, const float3 *restrict shadB,
					int width, int rows, int rowStep, float3 *restrict accC, float3 *restrict accE, float3 *restrict accS);

#endif // LIGHT_ROW_BLUR_BENCH_H
