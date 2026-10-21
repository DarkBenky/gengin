#include "../object/object.h"
#include <math.h>
#include "timings.h"
#include <immintrin.h>

static inline float MinF32(float a, float b) {
	return a < b ? a : b;
}

static inline float MaxF32(float a, float b) {
	return a > b ? a : b;
}

static inline void RayBoxItersectOld(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax) {
	if (!obj || !tMin || !tMax) return;

	float3 worldMin = obj->worldBBmin;
	float3 worldMax = obj->worldBBmax;

	*tMin = 0.0f;
	*tMax = FLT_MAX;

	const float eps = 1e-8f;
	for (int axis = 0; axis < 3; axis++) {
		float d = axis == 0 ? rayDir.x : (axis == 1 ? rayDir.y : rayDir.z);
		float o = axis == 0 ? rayOrigin.x : (axis == 1 ? rayOrigin.y : rayOrigin.z);
		float bmin = axis == 0 ? worldMin.x : (axis == 1 ? worldMin.y : worldMin.z);
		float bmax = axis == 0 ? worldMax.x : (axis == 1 ? worldMax.y : worldMax.z);

		if (fabsf(d) < eps) {
			if (o < bmin || o > bmax) {
				*tMin = FLT_MAX;
				*tMax = FLT_MIN;
				return;
			}
			continue;
		}

		float invD = 1.0f / d;
		float t0 = (bmin - o) * invD;
		float t1 = (bmax - o) * invD;
		if (t0 > t1) {
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}

		*tMin = MaxF32(*tMin, t0);
		*tMax = MinF32(*tMax, t1);
		if (*tMin > *tMax) {
			*tMin = FLT_MAX;
			*tMax = FLT_MIN;
			return;
		}
	}
}

typedef struct {
    float tMin0, tMax0;
    float tMin1, tMax1;
} RayBoxResult2;



// TODO: RayBoxIntersectV2 processing 2 boxes at once with SSE, returns both results in one struct
static inline RayBoxResult2 RayBoxIntersectV2(
    const Object *obj0, const Object *obj1,
    float3 rayOrigin, float3 rayDir)
{
    __m128 orig   = _mm_loadu_ps((const float *)&rayOrigin);
    __m128 dir    = _mm_loadu_ps((const float *)&rayDir);
    __m128 invDir = _mm_div_ps(_mm_set1_ps(1.0f), dir);

    __m128 bmin0 = _mm_loadu_ps((const float *)&obj0->worldBBmin);
    __m128 bmax0 = _mm_loadu_ps((const float *)&obj0->worldBBmax);
    __m128 bmin1 = _mm_loadu_ps((const float *)&obj1->worldBBmin);
    __m128 bmax1 = _mm_loadu_ps((const float *)&obj1->worldBBmax);

    __m128 t0_0 = _mm_mul_ps(_mm_sub_ps(bmin0, orig), invDir);
    __m128 t1_0 = _mm_mul_ps(_mm_sub_ps(bmax0, orig), invDir);
    __m128 t0_1 = _mm_mul_ps(_mm_sub_ps(bmin1, orig), invDir);
    __m128 t1_1 = _mm_mul_ps(_mm_sub_ps(bmax1, orig), invDir);

    __m128 tEnter0 = _mm_min_ps(t0_0, t1_0);
    __m128 tExit0  = _mm_max_ps(t0_0, t1_0);
    __m128 tEnter1 = _mm_min_ps(t0_1, t1_1);
    __m128 tExit1  = _mm_max_ps(t0_1, t1_1);

    __m128 enterLo = _mm_unpacklo_ps(tEnter0, tEnter1); 
    __m128 enterHi = _mm_unpackhi_ps(tEnter0, tEnter1); 
    __m128 exitLo  = _mm_unpacklo_ps(tExit0,  tExit1);
    __m128 exitHi  = _mm_unpackhi_ps(tExit0,  tExit1);

    __m128 enterMaxXY = _mm_max_ps(
        enterLo,
        _mm_shuffle_ps(enterLo, enterLo, _MM_SHUFFLE(1, 0, 3, 2))
    );
    __m128 exitMinXY = _mm_min_ps(
        exitLo,
        _mm_shuffle_ps(exitLo, exitLo, _MM_SHUFFLE(1, 0, 3, 2))
    );

    __m128 enterFinal = _mm_max_ps(_mm_max_ps(enterMaxXY, enterHi), _mm_setzero_ps());
    __m128 exitFinal  = _mm_min_ps(exitMinXY,  exitHi);

    float tMin0f = _mm_cvtss_f32(enterFinal);
    float tMax0f = _mm_cvtss_f32(exitFinal);
    float tMin1f = _mm_cvtss_f32(_mm_shuffle_ps(enterFinal, enterFinal, _MM_SHUFFLE(0,0,0,1)));
    float tMax1f = _mm_cvtss_f32(_mm_shuffle_ps(exitFinal,  exitFinal,  _MM_SHUFFLE(0,0,0,1)));

    RayBoxResult2 res;

    if (tMin0f > tMax0f) { res.tMin0 = FLT_MAX; res.tMax0 = -FLT_MAX; }
    else                  { res.tMin0 = tMin0f;  res.tMax0 = tMax0f;   }

    if (tMin1f > tMax1f) { res.tMin1 = FLT_MAX; res.tMax1 = -FLT_MAX; }
    else                  { res.tMin1 = tMin1f;  res.tMax1 = tMax1f;   }

    return res;
}
