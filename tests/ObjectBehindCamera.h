#include "../object/format.h"
#include "../object/object.h"
#include "../math/vector3.h"
#include <math.h>
#include "timings.h"
#include <immintrin.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <float.h>

static inline bool ObjectBehindCameraOld(const Object *obj, float3 camPos, float3 camForward) {
	// cull only if the entire world AABB is behind the camera plane
	float3 mn = obj->worldBBmin;
	float3 mx = obj->worldBBmax;
	// test all 8 corners — if any projects >= 0 along forward, the object is visible
	float3 corners[8] = {
		{mn.x, mn.y, mn.z},
		{mx.x, mn.y, mn.z},
		{mn.x, mx.y, mn.z},
		{mx.x, mx.y, mn.z},
		{mn.x, mn.y, mx.z},
		{mx.x, mn.y, mx.z},
		{mn.x, mx.y, mx.z},
		{mx.x, mx.y, mx.z},
	};
	for (int i = 0; i < 8; i++) {
		float3 toCorner = Float3_Sub(corners[i], camPos);
		if (Float3_Dot(toCorner, camForward) >= 0.0f)
			return false;
	}
	return true;
}

typedef struct { bool b0, b1; } Bool2;
typedef struct { bool b0, b1, b2, b3; } Bool4;

// SSE: test 2 objects simultaneously using the AABB support function.
// An object is behind camera iff the farthest-forward corner has negative projection.
static inline Bool2 ObjectBehindCameraV2(const Object *obj0, const Object *obj1, float3 camPos, float3 camForward) {
    __m128 fwd   = _mm_loadu_ps((const float *)&camForward);
    __m128 pos   = _mm_loadu_ps((const float *)&camPos);
    __m128 bmin0 = _mm_loadu_ps((const float *)&obj0->worldBBmin);
    __m128 bmax0 = _mm_loadu_ps((const float *)&obj0->worldBBmax);
    __m128 bmin1 = _mm_loadu_ps((const float *)&obj1->worldBBmin);
    __m128 bmax1 = _mm_loadu_ps((const float *)&obj1->worldBBmax);

    // support function: bmax where fwd >= 0, bmin where fwd < 0
    __m128 neg   = _mm_cmplt_ps(fwd, _mm_setzero_ps());
    __m128 best0 = _mm_blendv_ps(bmax0, bmin0, neg);
    __m128 best1 = _mm_blendv_ps(bmax1, bmin1, neg);

    // dot(best - camPos, camForward) using xyz lanes only (0x71)
    __m128 proj0 = _mm_dp_ps(_mm_sub_ps(best0, pos), fwd, 0x71);
    __m128 proj1 = _mm_dp_ps(_mm_sub_ps(best1, pos), fwd, 0x71);

    return (Bool2){ _mm_cvtss_f32(proj0) < 0.0f, _mm_cvtss_f32(proj1) < 0.0f };
}

// AVX: test 4 objects simultaneously.
// Transpose to SoA, compute projections with a 256-bit X+Y pass then add Z.
static inline Bool4 ObjectBehindCameraV4(
    const Object *obj0, const Object *obj1,
    const Object *obj2, const Object *obj3,
    float3 camPos, float3 camForward)
{
    __m128 bmin0 = _mm_loadu_ps((const float *)&obj0->worldBBmin);
    __m128 bmin1 = _mm_loadu_ps((const float *)&obj1->worldBBmin);
    __m128 bmin2 = _mm_loadu_ps((const float *)&obj2->worldBBmin);
    __m128 bmin3 = _mm_loadu_ps((const float *)&obj3->worldBBmin);
    __m128 bmax0 = _mm_loadu_ps((const float *)&obj0->worldBBmax);
    __m128 bmax1 = _mm_loadu_ps((const float *)&obj1->worldBBmax);
    __m128 bmax2 = _mm_loadu_ps((const float *)&obj2->worldBBmax);
    __m128 bmax3 = _mm_loadu_ps((const float *)&obj3->worldBBmax);

    // AoS->SoA: each result is one axis across all 4 boxes
    __m128 mn_t0 = _mm_unpacklo_ps(bmin0, bmin1);
    __m128 mn_t1 = _mm_unpackhi_ps(bmin0, bmin1);
    __m128 mn_t2 = _mm_unpacklo_ps(bmin2, bmin3);
    __m128 mn_t3 = _mm_unpackhi_ps(bmin2, bmin3);
    __m128 bminX = _mm_movelh_ps(mn_t0, mn_t2);
    __m128 bminY = _mm_movehl_ps(mn_t2, mn_t0);
    __m128 bminZ = _mm_movelh_ps(mn_t1, mn_t3);

    __m128 mx_t0 = _mm_unpacklo_ps(bmax0, bmax1);
    __m128 mx_t1 = _mm_unpackhi_ps(bmax0, bmax1);
    __m128 mx_t2 = _mm_unpacklo_ps(bmax2, bmax3);
    __m128 mx_t3 = _mm_unpackhi_ps(bmax2, bmax3);
    __m128 bmaxX = _mm_movelh_ps(mx_t0, mx_t2);
    __m128 bmaxY = _mm_movehl_ps(mx_t2, mx_t0);
    __m128 bmaxZ = _mm_movelh_ps(mx_t1, mx_t3);

    // support function: scalar sign of each fwd component selects entire axis row
    __m128 bestX = camForward.x >= 0.0f ? bmaxX : bminX;
    __m128 bestY = camForward.y >= 0.0f ? bmaxY : bminY;
    __m128 bestZ = camForward.z >= 0.0f ? bmaxZ : bminZ;

    // compute X and Y contributions in one 256-bit pass (lo=X, hi=Y)
    __m256 bestXY = _mm256_insertf128_ps(_mm256_castps128_ps256(bestX), bestY, 1);
    __m256 posXY  = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(camPos.x)),
                                          _mm_set1_ps(camPos.y), 1);
    __m256 fwdXY  = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(camForward.x)),
                                          _mm_set1_ps(camForward.y), 1);

    __m256 prodXY = _mm256_mul_ps(fwdXY, _mm256_sub_ps(bestXY, posXY));
    __m128 prodX  = _mm256_castps256_ps128(prodXY);
    __m128 prodY  = _mm256_extractf128_ps(prodXY, 1);
    __m128 prodZ  = _mm_mul_ps(_mm_set1_ps(camForward.z),
                               _mm_sub_ps(bestZ, _mm_set1_ps(camPos.z)));

    __m128 proj = _mm_add_ps(_mm_add_ps(prodX, prodY), prodZ);

    int mask = _mm_movemask_ps(_mm_cmplt_ps(proj, _mm_setzero_ps()));
    return (Bool4){ mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1 };
}