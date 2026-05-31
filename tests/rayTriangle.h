#include "../object/format.h"
#include <math.h>
#include "timings.h"
#include <immintrin.h>

#define MinFloat 1e-30f

static bool rayTriangle(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2, float *tOut) {
	const float eps = 1e-7f;
	float3 e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
	float3 e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
	float3 h = {rd.y * e2.z - rd.z * e2.y, rd.z * e2.x - rd.x * e2.z, rd.x * e2.y - rd.y * e2.x};
	float a = e1.x * h.x + e1.y * h.y + e1.z * h.z;
	if (fabsf(a) < eps) return false;
	float f = 1.0f / a;
	float3 s = {ro.x - v0.x, ro.y - v0.y, ro.z - v0.z};
	float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
	if (u < 0.0f || u > 1.0f) return false;
	float3 q = {s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
	float v = f * (rd.x * q.x + rd.y * q.y + rd.z * q.z);
	if (v < 0.0f || u + v > 1.0f) return false;
	float t = f * (e2.x * q.x + e2.y * q.y + e2.z * q.z);
	if (t < eps) return false;
	*tOut = t;
	return true;
}

static bool rayTriangleNew(float3 ro, float3 rd,
                        float3 v0, float3 v1, float3 v2, float *tOut) {
    const float eps = 1e-7f;

    float3 e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
    float3 e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};

    float3 h = {rd.y*e2.z - rd.z*e2.y,
                rd.z*e2.x - rd.x*e2.z,
                rd.x*e2.y - rd.y*e2.x};

    float a = e1.x*h.x + e1.y*h.y + e1.z*h.z;
    if (fabsf(a) < eps) return false;

    float3 s = {ro.x - v0.x, ro.y - v0.y, ro.z - v0.z};

    // Test u without dividing — just check sign relative to a
    float u_num = s.x*h.x + s.y*h.y + s.z*h.z;
    if (a > 0.0f ? (u_num < 0.0f || u_num > a) : (u_num > 0.0f || u_num < a))
        return false;

    float3 q = {s.y*e1.z - s.z*e1.y,
                s.z*e1.x - s.x*e1.z,
                s.x*e1.y - s.y*e1.x};

    float v_num = rd.x*q.x + rd.y*q.y + rd.z*q.z;
    if (a > 0.0f ? (v_num < 0.0f || u_num + v_num > a) : (v_num > 0.0f || u_num + v_num < a))
        return false;

    float t_num = e2.x*q.x + e2.y*q.y + e2.z*q.z;
    if (a > 0.0f ? t_num < eps : t_num > -eps)
        return false;

    *tOut = t_num / a;
    return true;
}

static float rayTriangleNewV2(float3 ro, float3 rd,
                        float3 v0, float3 v1, float3 v2) {
    const float eps = 1e-7f;

    float3 e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
    float3 e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};

    float3 h = {rd.y*e2.z - rd.z*e2.y,
                rd.z*e2.x - rd.x*e2.z,
                rd.x*e2.y - rd.y*e2.x};

    float a = e1.x*h.x + e1.y*h.y + e1.z*h.z;
    if (fabsf(a) < eps) return MinFloat;

    float3 s = {ro.x - v0.x, ro.y - v0.y, ro.z - v0.z};

    // Test u without dividing — just check sign relative to a
    float u_num = s.x*h.x + s.y*h.y + s.z*h.z;
    if (a > 0.0f ? (u_num < 0.0f || u_num > a) : (u_num > 0.0f || u_num < a))
        return MinFloat;

    float3 q = {s.y*e1.z - s.z*e1.y,
                s.z*e1.x - s.x*e1.z,
                s.x*e1.y - s.y*e1.x};

    float v_num = rd.x*q.x + rd.y*q.y + rd.z*q.z;
    if (a > 0.0f ? (v_num < 0.0f || u_num + v_num > a) : (v_num > 0.0f || u_num + v_num < a))
        return MinFloat;

    float t_num = e2.x*q.x + e2.y*q.y + e2.z*q.z;
    if (a > 0.0f ? t_num < eps : t_num > -eps)
        return MinFloat;

    return t_num / a;
}

static float rayTriangleNewV3(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float3 e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
	float3 e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
	float3 h = {rd.y * e2.z - rd.z * e2.y, rd.z * e2.x - rd.x * e2.z, rd.x * e2.y - rd.y * e2.x};
	float a = e1.x * h.x + e1.y * h.y + e1.z * h.z;
	if (fabsf(a) < eps) return MinFloat;
	float f = 1.0f / a;
	float3 s = {ro.x - v0.x, ro.y - v0.y, ro.z - v0.z};
	float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
	if (u < 0.0f || u > 1.0f) return MinFloat;
	float3 q = {s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
	float v = f * (rd.x * q.x + rd.y * q.y + rd.z * q.z);
	if (v < 0.0f || u + v > 1.0f) return MinFloat;
	float t = f * (e2.x * q.x + e2.y * q.y + e2.z * q.z);
	if (t < eps) return MinFloat;
	return t;
}

// V4: Scalar fields — explicit individual floats to avoid struct load/store overhead
static float rayTriangleNewV4(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	float a = e1x * hx + e1y * hy + e1z * hz;
	if (fabsf(a) < eps) return MinFloat;
	float f = 1.0f / a;

	float u = f * (sx * hx + sy * hy + sz * hz);
	if (u < 0.0f || u > 1.0f) return MinFloat;

	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	float v = f * (rd.x * qx + rd.y * qy + rd.z * qz);
	if (v < 0.0f || u + v > 1.0f) return MinFloat;

	float t = f * (e2x * qx + e2y * qy + e2z * qz);
	if (t < eps) return MinFloat;
	return t;
}

// V5: Speculative q — compute q before u check to overlap cross-product latency with u eval
static float rayTriangleNewV5(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	float a = e1x * hx + e1y * hy + e1z * hz;
	if (fabsf(a) < eps) return MinFloat;
	float invA = 1.0f / a;

	// Compute u numerator and q concurrently — q depends only on s,e1
	float uNum = sx * hx + sy * hy + sz * hz;
	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	float u = invA * uNum;
	if (u < 0.0f || u > 1.0f) return MinFloat;

	float v = invA * (rd.x * qx + rd.y * qy + rd.z * qz);
	if (v < 0.0f || u + v > 1.0f) return MinFloat;

	float t = invA * (e2x * qx + e2y * qy + e2z * qz);
	if (t < eps) return MinFloat;
	return t;
}

// V6: Combined branch — compute everything, single branch at end, reduces branch mispredicts
static float rayTriangleNewV6(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	float a = e1x * hx + e1y * hy + e1z * hz;
	if (fabsf(a) < eps) return MinFloat;
	float invA = 1.0f / a;

	float uNum = sx * hx + sy * hy + sz * hz;
	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	float u = invA * uNum;
	float v = invA * (rd.x * qx + rd.y * qy + rd.z * qz);
	float t = invA * (e2x * qx + e2y * qy + e2z * qz);

	if (u < 0.0f || u > 1.0f || v < 0.0f || u + v > 1.0f || t < eps)
		return MinFloat;
	return t;
}

// V7: SSE intrinsics for cross and dot products on float3 (16-byte, fits __m128)
static inline __m128 cross128(__m128 a, __m128 b) {
	__m128 a_yzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
	__m128 b_zxy = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
	__m128 a_zxy = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 1, 0, 2));
	__m128 b_yzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
	return _mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy), _mm_mul_ps(a_zxy, b_yzx));
}

static inline __m128 dot128(__m128 a, __m128 b) {
	__m128 m = _mm_mul_ps(a, b);
	__m128 h = _mm_hadd_ps(m, m);
	return _mm_hadd_ps(h, h);
}

static float rayTriangleNewV7(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	__m128 vv0 = _mm_loadu_ps(&v0.x);
	__m128 vv1 = _mm_loadu_ps(&v1.x);
	__m128 vv2 = _mm_loadu_ps(&v2.x);
	__m128 vrd = _mm_loadu_ps(&rd.x);
	__m128 vro = _mm_loadu_ps(&ro.x);

	__m128 e1 = _mm_sub_ps(vv1, vv0);
	__m128 e2 = _mm_sub_ps(vv2, vv0);
	__m128 vs = _mm_sub_ps(vro, vv0);

	__m128 h = cross128(vrd, e2);
	float a = _mm_cvtss_f32(dot128(e1, h));
	if (fabsf(a) < eps) return MinFloat;
	float invA = 1.0f / a;

	float u = invA * _mm_cvtss_f32(dot128(vs, h));
	if (u < 0.0f || u > 1.0f) return MinFloat;

	__m128 q = cross128(vs, e1);
	float v = invA * _mm_cvtss_f32(dot128(vrd, q));
	if (v < 0.0f || u + v > 1.0f) return MinFloat;

	float t = invA * _mm_cvtss_f32(dot128(e2, q));
	if (t < eps) return MinFloat;
	return t;
}

// V8: V4 + compute uNum before fabsf/division to overlap with reciprocal latency
static float rayTriangleNewV8(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	float a = e1x * hx + e1y * hy + e1z * hz;
	float uNum = sx * hx + sy * hy + sz * hz; // overlap with fabsf + div
	if (fabsf(a) < eps) return MinFloat;
	float invA = 1.0f / a;

	float u = invA * uNum;
	if (u < 0.0f || u > 1.0f) return MinFloat;

	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	float v = invA * (rd.x * qx + rd.y * qy + rd.z * qz);
	if (v < 0.0f || u + v > 1.0f) return MinFloat;

	float t = invA * (e2x * qx + e2y * qy + e2z * qz);
	if (t < eps) return MinFloat;
	return t;
}

// V9: V8 + branch hints — tell the compiler which exits are likely/unlikely for random data
static float rayTriangleNewV9(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	float a = e1x * hx + e1y * hy + e1z * hz;
	float uNum = sx * hx + sy * hy + sz * hz;
	if (__builtin_expect(fabsf(a) < eps, 0)) return MinFloat;
	float invA = 1.0f / a;

	float u = invA * uNum;
	if (__builtin_expect(u < 0.0f || u > 1.0f, 1)) return MinFloat;

	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	float v = invA * (rd.x * qx + rd.y * qy + rd.z * qz);
	if (__builtin_expect(v < 0.0f || u + v > 1.0f, 1)) return MinFloat;

	float t = invA * (e2x * qx + e2y * qy + e2z * qz);
	if (__builtin_expect(t < eps, 1)) return MinFloat;
	return t;
}

// V10: V4 + explicit FMA for dot products to ensure fused multiply-add instructions
static float rayTriangleNewV10(float3 ro, float3 rd,
						float3 v0, float3 v1, float3 v2) {
	const float eps = 1e-7f;
	float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
	float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
	float sx  = ro.x - v0.x, sy  = ro.y - v0.y, sz  = ro.z - v0.z;

	// Cross product h = rd x e2 using explicit subtraction (FMA not beneficial here)
	float hx = rd.y * e2z - rd.z * e2y;
	float hy = rd.z * e2x - rd.x * e2z;
	float hz = rd.x * e2y - rd.y * e2x;

	// Dot product a = dot(e1, h) using FMA
	float a = fmaf(e1x, hx, fmaf(e1y, hy, e1z * hz));
	if (fabsf(a) < eps) return MinFloat;
	float invA = 1.0f / a;

	// u = invA * dot(s, h)
	float u = invA * fmaf(sx, hx, fmaf(sy, hy, sz * hz));
	if (u < 0.0f || u > 1.0f) return MinFloat;

	// Cross product q = s x e1
	float qx = sy * e1z - sz * e1y;
	float qy = sz * e1x - sx * e1z;
	float qz = sx * e1y - sy * e1x;

	// v = invA * dot(rd, q)
	float v = invA * fmaf(rd.x, qx, fmaf(rd.y, qy, rd.z * qz));
	if (v < 0.0f || u + v > 1.0f) return MinFloat;

	// t = invA * dot(e2, q)
	float t = invA * fmaf(e2x, qx, fmaf(e2y, qy, e2z * qz));
	if (t < eps) return MinFloat;
	return t;
}