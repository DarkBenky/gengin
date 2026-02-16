#include "object.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../math/scalar.h"
#include "../math/transform.h"

static inline float3 TransformPoint(const Object *obj, float3 local) {
	return TransformPointTRS(local, obj->position, obj->rotation, obj->scale);
}

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename) {
	LoadObj(filename, obj);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
}

void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color) {
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;

	// Define the 8 vertices of the cube
	float3 vertices[8] = {
		{-0.5f, -0.5f, -0.5f},
		{0.5f, -0.5f, -0.5f},
		{0.5f, 0.5f, -0.5f},
		{-0.5f, 0.5f, -0.5f},
		{-0.5f, -0.5f, 0.5f},
		{0.5f, -0.5f, 0.5f},
		{0.5f, 0.5f, 0.5f},
		{-0.5f, 0.5f, 0.5f}};

	// Define the triangles for each face of the cube
	Triangle triangles[12] = {
		// Front face
		{vertices[4], vertices[5], vertices[6], {0, 0, 1}, color, 0.5f, 0.0f, 0.0f},
		{vertices[4], vertices[6], vertices[7], {0, 0, 1}, color, 0.5f, 0.0f, 0.0f},
		// Back face
		{vertices[1], vertices[0], vertices[3], {0, 0, -1}, color, 0.5f, 0.0f, 0.0f},
		{vertices[1], vertices[3], vertices[2], {0, 0, -1}, color, 0.5f, 0.0f, 0.0f},
		// Left face
		{vertices[0], vertices[4], vertices[7], {-1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[0], vertices[7], vertices[3], {-1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		// Right face
		{vertices[5], vertices[1], vertices[2], {1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[5], vertices[2], vertices[6], {1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		// Top face
		{vertices[3], vertices[7], vertices[6], {0, 1, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[3], vertices[6], vertices[2], {0, 1, 0}, color, 0.5f, 0.0f, 0.0f},
		// Bottom face
		{vertices[0], vertices[1], vertices[5], {0, -1, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[0], vertices[5], vertices[4], {0, -1, 0}, color, 0.5f, 0.0f, 0.0f}};

	obj->triangles = (Triangle *)malloc(12 * sizeof(Triangle));
	if (obj->triangles == NULL) {
		fprintf(stderr, "Error: Could not allocate memory for cube triangles.\n");
		return;
	}
	memcpy(obj->triangles, triangles, 12 * sizeof(Triangle));
	obj->triangleCount = 12;
	obj->BBmin = (float3){-0.5f, -0.5f, -0.5f};
	obj->BBmax = (float3){0.5f, 0.5f, 0.5f};
}

void Object_Destroy(Object *obj) {
	if (obj && obj->triangles) {
		free(obj->triangles);
		obj->triangles = NULL;
		obj->triangleCount = 0;
	}
}

void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax) {
	if (!obj || !tMin || !tMax) return;

	const float3 localMin = obj->BBmin;
	const float3 localMax = obj->BBmax;

	const float3 localCorners[8] = {
		{localMin.x, localMin.y, localMin.z},
		{localMax.x, localMin.y, localMin.z},
		{localMin.x, localMax.y, localMin.z},
		{localMax.x, localMax.y, localMin.z},
		{localMin.x, localMin.y, localMax.z},
		{localMax.x, localMin.y, localMax.z},
		{localMin.x, localMax.y, localMax.z},
		{localMax.x, localMax.y, localMax.z}};

	float3 worldMin = {FLT_MAX, FLT_MAX, FLT_MAX};
	float3 worldMax = {FLT_MIN, FLT_MIN, FLT_MIN};
	for (int i = 0; i < 8; i++) {
		float3 p = TransformPoint(obj, localCorners[i]);
		worldMin.x = MinF32(worldMin.x, p.x);
		worldMin.y = MinF32(worldMin.y, p.y);
		worldMin.z = MinF32(worldMin.z, p.z);
		worldMax.x = MaxF32(worldMax.x, p.x);
		worldMax.y = MaxF32(worldMax.y, p.y);
		worldMax.z = MaxF32(worldMax.z, p.z);
	}

	const float eps = 1e-8f;
	float tNear = 0.0f;
	float tFar = FLT_MAX;

	float3 boundsMin = worldMin;
	float3 boundsMax = worldMax;
	float3 origin = rayOrigin;
	float3 dir = rayDir;

	float *minArr = &boundsMin.x;
	float *maxArr = &boundsMax.x;
	float *origArr = &origin.x;
	float *dirArr = &dir.x;

	for (int axis = 0; axis < 3; axis++) {
		float d = dirArr[axis];
		float o = origArr[axis];
		float bmin = minArr[axis];
		float bmax = maxArr[axis];

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

		tNear = MaxF32(tNear, t0);
		tFar = MinF32(tFar, t1);
		if (tNear > tFar) {
			*tMin = FLT_MAX;
			*tMax = FLT_MIN;
			return;
		}
	}

	*tMin = tNear;
	*tMax = tFar;
}

bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir) {
	for (int i = 0; i < objectCount; i++) {
		float tMin, tMax;
		RayBoxItersect(&objects[i], rayOrigin, rayDir, &tMin, &tMax);
		if (tMin < tMax) {
			return true;
		}
	}
	return false;
}
