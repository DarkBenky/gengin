#include "object.h"
#include "material/material.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "../math/scalar.h"
#include "../math/transform.h"
#include "../util/threadPool.h"

static inline float3 TransformPoint(const Object *obj, float3 local) {
	return TransformPointTRS(local, obj->position, obj->rotation, obj->scale);
}

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib) {
	LoadObj(filename, obj, lib);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
	CreateObjectBVH(obj, &obj->bvh);
	Object_UpdateWorldBounds(obj);
}

// color => [RED][GREEN][BLUE] and [EMISSION] in alpha
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib) {
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
	obj->_temp = ((uint8)(color.x * 255.0f) << 16) | ((uint8)(color.y * 255.0f) << 8) | (uint8)(color.z * 255.0f);

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

	int matIdx = MaterialLib_Add(lib, Material_Make(color, 0.5f, 0.0f, color.w));
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
	CreateObjectBVH(obj, &obj->bvh);
	Object_UpdateWorldBounds(obj);
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
	free(obj->bvh.nodes);
	obj->bvh.nodes = NULL;
	free(obj->bvh.triIndices);
	obj->bvh.triIndices = NULL;
	obj->bvh.nodeCount = 0;
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

Color IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir) {
	float closestT = FLT_MAX;
	Color hitColor = 0;

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

float3 ComputeCentroid(const Object *obj, const int *ObjectIdx, int ObjectsCount) {
	float3 centroid = {0, 0, 0};
	for (int i = 0; i < ObjectsCount; i++) {
		int idx = ObjectIdx[i];
		for (int j = 0; j < obj[idx].triangleCount; j++) {
			centroid.x += (obj[idx].v1[j].x + obj[idx].v2[j].x + obj[idx].v3[j].x) / 3.0f;
			centroid.y += (obj[idx].v1[j].y + obj[idx].v2[j].y + obj[idx].v3[j].y) / 3.0f;
			centroid.z += (obj[idx].v1[j].z + obj[idx].v2[j].z + obj[idx].v3[j].z) / 3.0f;
		}
	}
	if (ObjectsCount > 0) {
		float invCount = 1.0f / ObjectsCount;
		centroid.x *= invCount;
		centroid.y *= invCount;
		centroid.z *= invCount;
	}
	return centroid;
}

void CreateObjectBVH(Object *obj, BVH *bvh) {
	if (!obj || !bvh || obj->triangleCount == 0) return;

	int n = obj->triangleCount;
	bvh->nodes = malloc(2 * n * sizeof(BVHNode));
	bvh->triIndices = malloc(n * sizeof(int));
	bvh->nodeCount = 0;
	if (!bvh->nodes || !bvh->triIndices) {
		free(bvh->nodes);
		free(bvh->triIndices);
		bvh->nodes = NULL;
		bvh->triIndices = NULL;
		return;
	}

	for (int i = 0; i < n; i++)
		bvh->triIndices[i] = i;

		// --- helpers as local macros to avoid polluting namespace ---
#define TRI_CENTROID_AXIS(obj, t, ax) \
	(((obj)->v1[(t)].ax + (obj)->v2[(t)].ax + (obj)->v3[(t)].ax) * 0.333333f)

	// iterative build using an explicit work stack
	typedef struct {
		int nodeIdx;
		int start;
		int count;
	} WorkItem;
	WorkItem stack[64];
	int top = 0;

	// allocate root
	bvh->nodeCount = 1;
	stack[top++] = (WorkItem){0, 0, n};

	while (top > 0) {
		WorkItem w = stack[--top];
		BVHNode *node = &bvh->nodes[w.nodeIdx];
		int *idx = bvh->triIndices + w.start;

		// compute AABB of this set of triangles
		float3 mn = {FLT_MAX, FLT_MAX, FLT_MAX};
		float3 mx = {FLT_MIN, FLT_MIN, FLT_MIN};
		for (int i = 0; i < w.count; i++) {
			int t = idx[i];
			float3 verts[3] = {obj->v1[t], obj->v2[t], obj->v3[t]};
			for (int v = 0; v < 3; v++) {
				mn.x = MinF32(mn.x, verts[v].x);
				mx.x = MaxF32(mx.x, verts[v].x);
				mn.y = MinF32(mn.y, verts[v].y);
				mx.y = MaxF32(mx.y, verts[v].y);
				mn.z = MinF32(mn.z, verts[v].z);
				mx.z = MaxF32(mx.z, verts[v].z);
			}
		}
		node->BBmin = mn;
		node->BBmax = mx;

		if (w.count <= 4) {
			node->triStart = w.start;
			node->triCount = w.count;
			node->leftFirst = -1;
			continue;
		}

		// longest axis median split
		float dx = mx.x - mn.x, dy = mx.y - mn.y, dz = mx.z - mn.z;
		int axis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz ? 1 : 2);
		float split = (axis == 0 ? mn.x + mx.x : (axis == 1 ? mn.y + mx.y : mn.z + mx.z)) * 0.5f;

		// partition in-place around split centroid
		int lo = 0, hi = w.count - 1;
		while (lo <= hi) {
			int t = idx[lo];
			float c = axis == 0 ? TRI_CENTROID_AXIS(obj, t, x)
								: (axis == 1 ? TRI_CENTROID_AXIS(obj, t, y) : TRI_CENTROID_AXIS(obj, t, z));
			if (c <= split) {
				lo++;
			} else {
				int tmp = idx[lo];
				idx[lo] = idx[hi];
				idx[hi] = tmp;
				hi--;
			}
		}
		int mid = lo;
		if (mid == 0 || mid == w.count) mid = w.count / 2; // degenerate fallback

		int leftIdx = bvh->nodeCount;
		bvh->nodeCount += 2;
		node->leftFirst = leftIdx;
		node->triCount = 0;

		stack[top++] = (WorkItem){leftIdx, w.start, mid};
		stack[top++] = (WorkItem){leftIdx + 1, w.start + mid, w.count - mid};
	}
#undef TRI_CENTROID_AXIS
}

void DestroyObjectBVH(BVH *bvh) {
	if (!bvh) return;
	free(bvh->nodes);
	bvh->nodes = NULL;
	free(bvh->triIndices);
	bvh->triIndices = NULL;
	bvh->nodeCount = 0;
}

// Möller–Trumbore ray-triangle intersection
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

static float rayAABB(float3 ro, float3 rd, float3 mn, float3 mx) {
	float tmin = 0.0f, tmax = FLT_MAX;
	const float *o = &ro.x, *d = &rd.x, *bmin = &mn.x, *bmax = &mx.x;
	for (int i = 0; i < 3; i++) {
		if (fabsf(d[i]) < 1e-8f) {
			if (o[i] < bmin[i] || o[i] > bmax[i]) return FLT_MAX;
			continue;
		}
		float invD = 1.0f / d[i];
		float t0 = (bmin[i] - o[i]) * invD;
		float t1 = (bmax[i] - o[i]) * invD;
		if (t0 > t1) {
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		tmin = MaxF32(tmin, t0);
		tmax = MinF32(tmax, t1);
		if (tmin > tmax) return FLT_MAX;
	}
	return tmin;
}

void IntersectBVH(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir, int *hitTriIdx, float3 *hitPosWorld) {
	if (!obj || !bvh || !hitTriIdx || bvh->nodeCount == 0) return;
	*hitTriIdx = -1;

	// bring ray into local object space so it tests correctly against local-space triangles
	rayOrigin = InverseTransformPointTRS(rayOrigin, obj->position, obj->rotation, obj->scale);
	rayDir = InverseTransformDirTRS(rayDir, obj->rotation, obj->scale);

	float bestT = FLT_MAX;
	int stack[64];
	int top = 0;
	stack[top++] = 0;

	while (top > 0) {
		const BVHNode *node = &bvh->nodes[stack[--top]];
		if (rayAABB(rayOrigin, rayDir, node->BBmin, node->BBmax) >= bestT) continue;

		if (node->triCount > 0) {
			for (int i = 0; i < node->triCount; i++) {
				int t = bvh->triIndices[node->triStart + i];
				float hit;
				if (rayTriangle(rayOrigin, rayDir, obj->v1[t], obj->v2[t], obj->v3[t], &hit) && hit < bestT) {
					bestT = hit;
					*hitTriIdx = t;
				}
			}
		} else {
			stack[top++] = node->leftFirst;
			stack[top++] = node->leftFirst + 1;
		}
	}

	if (*hitTriIdx >= 0 && hitPosWorld) {
		float3 localHit = {
			rayOrigin.x + bestT * rayDir.x,
			rayOrigin.y + bestT * rayDir.y,
			rayOrigin.z + bestT * rayDir.z};
		*hitPosWorld = TransformPointTRS(localHit, obj->position, obj->rotation, obj->scale);
	}
}
