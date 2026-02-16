#ifndef MATH_VECTOR3_H
#define MATH_VECTOR3_H

#include <math.h>

#include "../object/format.h"

static inline float3 Float3_Sub(float3 a, float3 b) {
	return (float3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline float3 Float3_Add(float3 a, float3 b) {
	return (float3){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline float3 Float3_Scale(float3 v, float s) {
	return (float3){v.x * s, v.y * s, v.z * s};
}

static inline float3 Float3_Normalize(float3 v) {
	float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len > 0.0f) {
		float invLen = 1.0f / len;
		return (float3){v.x * invLen, v.y * invLen, v.z * invLen};
	}
	return v;
}

static inline float3 PositionToRayDir(float3 from, float3 to) {
	return Float3_Normalize(Float3_Sub(to, from));
}

#endif // MATH_VECTOR3_H
