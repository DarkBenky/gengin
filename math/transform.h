#ifndef MATH_TRANSFORM_H
#define MATH_TRANSFORM_H

#include <math.h>

#include "../object/format.h"

static inline float3 RotateX(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x,
		v.y * c - v.z * s,
		v.y * s + v.z * c};
}

static inline float3 RotateY(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x * c + v.z * s,
		v.y,
		-v.x * s + v.z * c};
}

static inline float3 RotateZ(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x * c - v.y * s,
		v.x * s + v.y * c,
		v.z};
}

static inline float3 RotateXYZ(float3 v, float3 rotation) {
	v = RotateX(v, rotation.x);
	v = RotateY(v, rotation.y);
	v = RotateZ(v, rotation.z);
	return v;
}

static inline float3 TransformPointTRS(float3 local, float3 position, float3 rotation, float3 scale) {
	float3 scaled = {
		local.x * scale.x,
		local.y * scale.y,
		local.z * scale.z};
	float3 rotated = RotateXYZ(scaled, rotation);
	return (float3){
		rotated.x + position.x,
		rotated.y + position.y,
		rotated.z + position.z};
}

#endif // MATH_TRANSFORM_H
