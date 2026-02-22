#include "object.h"
#include "material/material.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "../math/scalar.h"
#include "../math/transform.h"

static inline float3 TransformPoint(const Object *obj, float3 local) {
	return TransformPointTRS(local, obj->position, obj->rotation, obj->scale);
}

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib) {
	LoadObj(filename, obj, lib);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
}

void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib) {
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
	obj->_temp = color; // store color in temp for now, can be removed later

	const int triCount = 12;

	obj->v1 = (float3 *)malloc(triCount * sizeof(float3));
	obj->v2 = (float3 *)malloc(triCount * sizeof(float3));
	obj->v3 = (float3 *)malloc(triCount * sizeof(float3));
	obj->normals = (float3 *)malloc(triCount * sizeof(float3));
	obj->materialIds = (int *)malloc(triCount * sizeof(int));

	if (!obj->v1 || !obj->v2 || !obj->v3 || !obj->normals || !obj->materialIds) {
		fprintf(stderr, "Error: Could not allocate memory for cube triangles.\n");
		Object_Destroy(obj);
		return;
	}

	const float3 verts[8] = {
		{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}};

	typedef struct {
		float3 a, b, c, n;
	} FaceTri;
	const FaceTri faces[12] = {
		{verts[4], verts[5], verts[6], {0, 0, 1}},
		{verts[4], verts[6], verts[7], {0, 0, 1}},
		{verts[1], verts[0], verts[3], {0, 0, -1}},
		{verts[1], verts[3], verts[2], {0, 0, -1}},
		{verts[0], verts[4], verts[7], {-1, 0, 0}},
		{verts[0], verts[7], verts[3], {-1, 0, 0}},
		{verts[5], verts[1], verts[2], {1, 0, 0}},
		{verts[5], verts[2], verts[6], {1, 0, 0}},
		{verts[3], verts[7], verts[6], {0, 1, 0}},
		{verts[3], verts[6], verts[2], {0, 1, 0}},
		{verts[0], verts[1], verts[5], {0, -1, 0}},
		{verts[0], verts[5], verts[4], {0, -1, 0}}};

	int matIdx = MaterialLib_Add(lib, Material_Make(color, 0.5f, 0.0f, 0.0f));
	for (int i = 0; i < triCount; i++) {
		obj->v1[i] = faces[i].a;
		obj->v2[i] = faces[i].b;
		obj->v3[i] = faces[i].c;
		obj->normals[i] = faces[i].n;
		obj->materialIds[i] = matIdx;
	}
	obj->triangleCount = triCount;
	obj->BBmin = (float3){-0.5f, -0.5f, -0.5f};
	obj->BBmax = (float3){0.5f, 0.5f, 0.5f};
}

void Object_Destroy(Object *obj) {
	if (!obj) return;
	free(obj->v1);
	obj->v1 = NULL;
	free(obj->v2);
	obj->v2 = NULL;
	free(obj->v3);
	obj->v3 = NULL;
	free(obj->normals);
	obj->normals = NULL;
	free(obj->materialIds);
	obj->materialIds = NULL;
	obj->triangleCount = 0;
}

void Object_UpdateWorldBounds(Object *obj) {
	if (!obj) return;

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

float3 IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir) {
	float closestT = FLT_MAX;
	float3 hitColor = {0, 0, 0};

	for (int i = 0; i < objectCount; i++) {
		float tMin, tMax;
		RayBoxItersect(&objects[i], rayOrigin, rayDir, &tMin, &tMax);
		if (tMin < tMax && tMin < closestT) {
			closestT = tMin;
			hitColor = objects[i]._temp;
		}
	}
	return hitColor;
}
