#include "../object/format.h"
#include <math.h>
#include "timings.h"
#include <immintrin.h>

static inline float rayAABB_inv(float3 bias, float3 invRd, const float *mn, const float *mx) {
	float tx0 = mn[0] * invRd.x - bias.x, tx1 = mx[0] * invRd.x - bias.x;
	float ty0 = mn[1] * invRd.y - bias.y, ty1 = mx[1] * invRd.y - bias.y;
	float tz0 = mn[2] * invRd.z - bias.z, tz1 = mx[2] * invRd.z - bias.z;
	float tmin = fmaxf(fmaxf(fminf(tx0, tx1), fminf(ty0, ty1)), fminf(tz0, tz1));
	float tmax = fminf(fminf(fmaxf(tx0, tx1), fmaxf(ty0, ty1)), fmaxf(tz0, tz1));
	return tmax < tmin ? FLT_MAX : tmin;
}

static inline float rayAABB_invV2(float3 bias, float3 invRd, const float3 mn, const float3 mx) {
	float tx0 = mn.x * invRd.x - bias.x, tx1 = mx.x * invRd.x - bias.x;
	float ty0 = mn.y * invRd.y - bias.y, ty1 = mx.y * invRd.y - bias.y;
	float tz0 = mn.z * invRd.z - bias.z, tz1 = mx.z * invRd.z - bias.z;
	float tmin = fmaxf(fmaxf(fminf(tx0, tx1), fminf(ty0, ty1)), fminf(tz0, tz1));
	float tmax = fminf(fminf(fmaxf(tx0, tx1), fmaxf(ty0, ty1)), fmaxf(tz0, tz1));
	return tmax < tmin ? FLT_MAX : tmin;
}

static inline float rayAABB_invV3(float3 bias, float3 invRd, const float3 mn, const float3 mx) {
	float3 t0 = (float3){fmaf(mn.x, invRd.x, -bias.x), fmaf(mn.y, invRd.y, -bias.y), fmaf(mn.z, invRd.z, -bias.z)};
	float3 t1 = (float3){fmaf(mx.x, invRd.x, -bias.x), fmaf(mx.y, invRd.y, -bias.y), fmaf(mx.z, invRd.z, -bias.z)};
	float3 tlo = (float3){fminf(t0.x, t1.x), fminf(t0.y, t1.y), fminf(t0.z, t1.z)};
	float3 thi = (float3){fmaxf(t0.x, t1.x), fmaxf(t0.y, t1.y), fmaxf(t0.z, t1.z)};
	float tmin = fmaxf(fmaxf(tlo.x, tlo.y), tlo.z);
	float tmax = fminf(fminf(thi.x, thi.y), thi.z);
	return tmax < tmin ? FLT_MAX : tmin;
}

// Pack 8 float3 (float4 with ignored .w) into SoA for AVX2 — call once before batching
static inline void transpose_float3x8_to_soa(
	const float3 v[8],
	float *x8, float *y8, float *z8 // must be 32-byte aligned
) {
	for (int i = 0; i < 8; i++) {
		x8[i] = v[i].x;
		y8[i] = v[i].y;
		z8[i] = v[i].z;
	}
}

// simd version processing 8 AABBs at once, returns array of 8 results
// TODO: use this function to speed up ray tracer
static inline void rayAABB_invV4_avx2(
	float3 bias,		// ray origin * inv ray direction
	float3 invRd,		// precomputed once per ray: 1.0f / ray direction
	const float3 mn[8], // 8 boundingBox mins
	const float3 mx[8], // 8 boundingBox maxs
	float *out			// 8 results, must be 32-byte aligned
) {
	float mn_x8[8] __attribute__((aligned(32))), mn_y8[8] __attribute__((aligned(32))), mn_z8[8] __attribute__((aligned(32)));
	float mx_x8[8] __attribute__((aligned(32))), mx_y8[8] __attribute__((aligned(32))), mx_z8[8] __attribute__((aligned(32)));

	// scatter xyz out of float4, skip .w
	transpose_float3x8_to_soa(mn, mn_x8, mn_y8, mn_z8);
	transpose_float3x8_to_soa(mx, mx_x8, mx_y8, mx_z8);

	__m256 bias_x = _mm256_set1_ps(bias.x);
	__m256 bias_y = _mm256_set1_ps(bias.y);
	__m256 bias_z = _mm256_set1_ps(bias.z);
	__m256 ird_x = _mm256_set1_ps(invRd.x);
	__m256 ird_y = _mm256_set1_ps(invRd.y);
	__m256 ird_z = _mm256_set1_ps(invRd.z);

	__m256 tx0 = _mm256_fmsub_ps(_mm256_load_ps(mn_x8), ird_x, bias_x);
	__m256 tx1 = _mm256_fmsub_ps(_mm256_load_ps(mx_x8), ird_x, bias_x);
	__m256 ty0 = _mm256_fmsub_ps(_mm256_load_ps(mn_y8), ird_y, bias_y);
	__m256 ty1 = _mm256_fmsub_ps(_mm256_load_ps(mx_y8), ird_y, bias_y);
	__m256 tz0 = _mm256_fmsub_ps(_mm256_load_ps(mn_z8), ird_z, bias_z);
	__m256 tz1 = _mm256_fmsub_ps(_mm256_load_ps(mx_z8), ird_z, bias_z);

	__m256 tmin = _mm256_max_ps(_mm256_max_ps(_mm256_min_ps(tx0, tx1),
											  _mm256_min_ps(ty0, ty1)),
								_mm256_min_ps(tz0, tz1));
	__m256 tmax = _mm256_min_ps(_mm256_min_ps(_mm256_max_ps(tx0, tx1),
											  _mm256_max_ps(ty0, ty1)),
								_mm256_max_ps(tz0, tz1));

	__m256 miss = _mm256_cmp_ps(tmax, tmin, _CMP_LT_OQ);
	__m256 result = _mm256_blendv_ps(tmin, _mm256_set1_ps(FLT_MAX), miss);

	_mm256_store_ps(out, result);
}

// simd version processing 4 AABBs at once with SSE, returns array of 4 results
// TODO: use this function to speed up ray tracer (also need to add transpose for 4 boxes)
static inline void rayAABB_invV5_sse_x4(
    float3 bias, // ray origin * inv ray direction
    float3 invRd, // precomputed once per ray: 1.0f / ray direction
    const float3 mn[4], // 4 boundingBox mins
    const float3 mx[4], // 4 boundingBox maxs
    float *out  // 4 results, 16-byte aligned
) {
    float mn_x4[4] __attribute__((aligned(16))) = {mn[0].x, mn[1].x, mn[2].x, mn[3].x};
    float mn_y4[4] __attribute__((aligned(16))) = {mn[0].y, mn[1].y, mn[2].y, mn[3].y};
    float mn_z4[4] __attribute__((aligned(16))) = {mn[0].z, mn[1].z, mn[2].z, mn[3].z};
    float mx_x4[4] __attribute__((aligned(16))) = {mx[0].x, mx[1].x, mx[2].x, mx[3].x};
    float mx_y4[4] __attribute__((aligned(16))) = {mx[0].y, mx[1].y, mx[2].y, mx[3].y};
    float mx_z4[4] __attribute__((aligned(16))) = {mx[0].z, mx[1].z, mx[2].z, mx[3].z};

    __m128 bias_x = _mm_set1_ps(bias.x);
    __m128 bias_y = _mm_set1_ps(bias.y);
    __m128 bias_z = _mm_set1_ps(bias.z);
    __m128 ird_x  = _mm_set1_ps(invRd.x);
    __m128 ird_y  = _mm_set1_ps(invRd.y);
    __m128 ird_z  = _mm_set1_ps(invRd.z);

    __m128 tx0 = _mm_fmsub_ps(_mm_load_ps(mn_x4), ird_x, bias_x);
    __m128 tx1 = _mm_fmsub_ps(_mm_load_ps(mx_x4), ird_x, bias_x);
    __m128 ty0 = _mm_fmsub_ps(_mm_load_ps(mn_y4), ird_y, bias_y);
    __m128 ty1 = _mm_fmsub_ps(_mm_load_ps(mx_y4), ird_y, bias_y);
    __m128 tz0 = _mm_fmsub_ps(_mm_load_ps(mn_z4), ird_z, bias_z);
    __m128 tz1 = _mm_fmsub_ps(_mm_load_ps(mx_z4), ird_z, bias_z);

    __m128 tmin = _mm_max_ps(_mm_max_ps(_mm_min_ps(tx0, tx1),
                                         _mm_min_ps(ty0, ty1)),
                                         _mm_min_ps(tz0, tz1));
    __m128 tmax = _mm_min_ps(_mm_min_ps(_mm_max_ps(tx0, tx1),
                                         _mm_max_ps(ty0, ty1)),
                                         _mm_max_ps(tz0, tz1));

    __m128 miss   = _mm_cmplt_ps(tmax, tmin);
    __m128 result = _mm_blendv_ps(tmin, _mm_set1_ps(FLT_MAX), miss);

    _mm_store_ps(out, result);
}