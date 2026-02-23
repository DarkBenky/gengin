#include "object.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../math/scalar.h"
#include "../math/transform.h"
#include "../math/matrix.h"

void Object_UpdateTransform(Object *obj) {
	if (!obj) return;
	obj->rotationMatrix = Matrix_RotationXYZ(obj->rotation);
	obj->transform = Matrix_TRS(obj->position, obj->rotation, obj->scale);
	obj->invTransform = Matrix_Invert(obj->transform);
}

static inline float3 TransformPoint(const Object *obj, float3 local) {
	return Matrix_TransformPoint(obj->transform, local);
}

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename) {
	LoadObj(filename, obj);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
	Object_UpdateTransform(obj);
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
	Object_UpdateTransform(obj);
}

void Object_Destroy(Object *obj) {
	if (obj && obj->triangles) {
		free(obj->triangles);
		obj->triangles = NULL;
		obj->triangleCount = 0;
	}
}

void Object_UpdateWorldBounds(Object *obj) {
	if (!obj) return;

	Object_UpdateTransform(obj);

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

	obj->worldBBmin = (float3){FLT_MAX, FLT_MAX, FLT_MAX};
	obj->worldBBmax = (float3){FLT_MIN, FLT_MIN, FLT_MIN};
	for (int i = 0; i < 8; i++) {
		float3 p = TransformPoint(obj, localCorners[i]);
		obj->worldBBmin.x = MinF32(obj->worldBBmin.x, p.x);
		obj->worldBBmin.y = MinF32(obj->worldBBmin.y, p.y);
		obj->worldBBmin.z = MinF32(obj->worldBBmin.z, p.z);
		obj->worldBBmax.x = MaxF32(obj->worldBBmax.x, p.x);
		obj->worldBBmax.y = MaxF32(obj->worldBBmax.y, p.y);
		obj->worldBBmax.z = MaxF32(obj->worldBBmax.z, p.z);
	}
}

void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax) {
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
