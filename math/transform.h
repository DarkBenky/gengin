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

// Inverse rotation: undo Z, Y, X in reverse order with negated angles
static inline float3 InverseRotateXYZ(float3 v, float3 rotation) {
	v = RotateZ(v, -rotation.z);
	v = RotateY(v, -rotation.y);
	v = RotateX(v, -rotation.x);
	return v;
}

// Transform a world-space point into local object space
static inline float3 InverseTransformPointTRS(float3 world, float3 position, float3 rotation, float3 scale) {
	float3 t = {world.x - position.x, world.y - position.y, world.z - position.z};
	float3 r = InverseRotateXYZ(t, rotation);
	return (float3){r.x / scale.x, r.y / scale.y, r.z / scale.z};
}

// Transform a world-space direction into local object space (no translation)
static inline float3 InverseTransformDirTRS(float3 dir, float3 rotation, float3 scale) {
	float3 r = InverseRotateXYZ(dir, rotation);
	return (float3){r.x / scale.x, r.y / scale.y, r.z / scale.z};
}

#endif // MATH_TRANSFORM_H
