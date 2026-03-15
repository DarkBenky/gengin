#include "object.h"
#include "material/material.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "../math/scalar.h"
#include "../math/transform.h"
#include "../util/threadPool.h"
#include "../math/vector3.h"

static inline float3 TransformPoint(const Object *obj, float3 local) {
	return TransformPointTRS(local, obj->position, obj->rotation, obj->scale);
}

bool ObjectBehindCamera(const Object *obj, float3 camPos, float3 camForward) {
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

Frustum Frustum_FromCamera(const Camera *cam) {
	float3 fwd = Float3_Normalize(cam->forward);
	float3 rgt = Float3_Normalize(cam->right);
	float3 up = Float3_Normalize(cam->up);
	float half_y = cam->fovScale;
	float half_x = half_y * cam->aspect;
	float3 pos = cam->position;

	Frustum f;
	// near plane
	f.planes[0] = (FrustumPlane){fwd, -Float3_Dot(fwd, pos)};
	// left plane:   n = normalize(rgt + half_x * fwd)
	float3 nl = Float3_Normalize((float3){rgt.x + half_x * fwd.x, rgt.y + half_x * fwd.y, rgt.z + half_x * fwd.z});
	f.planes[1] = (FrustumPlane){nl, -Float3_Dot(nl, pos)};
	// right plane:  n = normalize(-rgt + half_x * fwd)
	float3 nr = Float3_Normalize((float3){-rgt.x + half_x * fwd.x, -rgt.y + half_x * fwd.y, -rgt.z + half_x * fwd.z});
	f.planes[2] = (FrustumPlane){nr, -Float3_Dot(nr, pos)};
	// bottom plane: n = normalize(up + half_y * fwd)
	float3 nb = Float3_Normalize((float3){up.x + half_y * fwd.x, up.y + half_y * fwd.y, up.z + half_y * fwd.z});
	f.planes[3] = (FrustumPlane){nb, -Float3_Dot(nb, pos)};
	// top plane:    n = normalize(-up + half_y * fwd)
	float3 nt = Float3_Normalize((float3){-up.x + half_y * fwd.x, -up.y + half_y * fwd.y, -up.z + half_y * fwd.z});
	f.planes[4] = (FrustumPlane){nt, -Float3_Dot(nt, pos)};
	return f;
}

bool Frustum_TestAABB(const Frustum *f, float3 bbMin, float3 bbMax) {
	for (int i = 0; i < 5; i++) {
		float3 n = f->planes[i].normal;
		// p-vertex: AABB corner furthest along n
		float px = n.x >= 0.0f ? bbMax.x : bbMin.x;
		float py = n.y >= 0.0f ? bbMax.y : bbMin.y;
		float pz = n.z >= 0.0f ? bbMax.z : bbMin.z;
		if (px * n.x + py * n.y + pz * n.z + f->planes[i].d < 0.0f)
			return false;
	}
	return true;
}

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib) {
	LoadObj(filename, obj, lib);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
	CreateObjectBVH(obj, &obj->bvh);
	Object_UpdateWorldBounds(obj);
	CalculateFaceEmissions(obj, lib);
}

// color => [RED][GREEN][BLUE] and [EMISSION] in alpha
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib, float emission, float roughness, float metallic) {
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

	int matIdx = MaterialLib_Add(lib, Material_Make(color, roughness, metallic, emission));
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
	CalculateFaceEmissions(obj, lib);
}

void Object_SetMaterial(Object *obj, MaterialLib *lib, Material mat) {
	for (int i = 0; i < obj->triangleCount; i++)
		lib->entries[obj->materialIds[i]] = mat;
}

void Object_SetColor(Object *obj, MaterialLib *lib, float3 color) {
	for (int i = 0; i < obj->triangleCount; i++)
		lib->entries[obj->materialIds[i]].color = color;
}

void Object_SetEmission(Object *obj, MaterialLib *lib, float emission) {
	for (int i = 0; i < obj->triangleCount; i++)
		lib->entries[obj->materialIds[i]].emission = emission;
}

void Object_SetRoughness(Object *obj, MaterialLib *lib, float roughness) {
	for (int i = 0; i < obj->triangleCount; i++)
		lib->entries[obj->materialIds[i]].roughness = roughness;
}

void Object_SetMetallic(Object *obj, MaterialLib *lib, float metallic) {
	for (int i = 0; i < obj->triangleCount; i++)
		lib->entries[obj->materialIds[i]].metallic = metallic;
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

	// Cache rows of M = Diag(invScale) * InvRot
	// InvRot = Rx(-rx)*Ry(-ry)*Rz(-rz) = transpose of forward rotation Rz*Ry*Rx
	float cx = cosf(obj->rotation.x), sx = sinf(obj->rotation.x);
	float cy = cosf(obj->rotation.y), sy = sinf(obj->rotation.y);
	float cz = cosf(obj->rotation.z), sz = sinf(obj->rotation.z);
	float isx = 1.0f / obj->scale.x, isy = 1.0f / obj->scale.y, isz = 1.0f / obj->scale.z;
	obj->_invScale = (float3){isx * (cy * cz), isx * (cy * sz), isx * (-sy)};
	obj->_invRotSin = (float3){isy * (-cx * sz + sx * sy * cz), isy * (cx * cz + sx * sy * sz), isy * (sx * cy)};
	obj->_invRotCos = (float3){isz * (sx * sz + cx * sy * cz), isz * (-sx * cz + cx * sy * sz), isz * (cx * cy)};
	// Forward rotation rows — transpose of inverse rotation / no scale applied
	obj->_fwdRot0 = (float3){cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz};
	obj->_fwdRot1 = (float3){cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz};
	obj->_fwdRot2 = (float3){-sy, sx * cy, cx * cy};
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
		node->BBmin[0] = mn.x;
		node->BBmin[1] = mn.y;
		node->BBmin[2] = mn.z;
		node->BBmax[0] = mx.x;
		node->BBmax[1] = mx.y;
		node->BBmax[2] = mx.z;

		if (w.count <= 4) {
			node->triStart = w.start;
			node->triCount = w.count;
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

	// bring ray into local object space using the cached inverse TRS matrix
	// (computed in Object_UpdateWorldBounds — eliminates 12 trig calls per call)
	float3 t = {rayOrigin.x - obj->position.x, rayOrigin.y - obj->position.y, rayOrigin.z - obj->position.z};
	float3 r0 = obj->_invScale, r1 = obj->_invRotSin, r2 = obj->_invRotCos;
	rayOrigin = (float3){
		r0.x * t.x + r0.y * t.y + r0.z * t.z,
		r1.x * t.x + r1.y * t.y + r1.z * t.z,
		r2.x * t.x + r2.y * t.y + r2.z * t.z};
	rayDir = (float3){
		r0.x * rayDir.x + r0.y * rayDir.y + r0.z * rayDir.z,
		r1.x * rayDir.x + r1.y * rayDir.y + r1.z * rayDir.z,
		r2.x * rayDir.x + r2.y * rayDir.y + r2.z * rayDir.z};

	// precompute inverse direction once — replaces 3 divisions per BVH node with multiplications
	float3 invDir = {1.0f / rayDir.x, 1.0f / rayDir.y, 1.0f / rayDir.z};
	// bias = ro * invDir: precomputed once per ray, avoids recomputing it 6x per BVH node
	float3 bias = {rayOrigin.x * invDir.x, rayOrigin.y * invDir.y, rayOrigin.z * invDir.z};

	float bestT = FLT_MAX;
	int stack[64];
	int top = 0;
	stack[top++] = 0;

	while (top > 0) {
		const BVHNode *node = &bvh->nodes[stack[--top]];

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
			// test both children immediately — they're adjacent in the array (same cache line).
			// push farther child first so LIFO pops nearer child first → bestT converges faster.
			int li = node->leftFirst, ri = li + 1;
			float tl = rayAABB_inv(bias, invDir, bvh->nodes[li].BBmin, bvh->nodes[li].BBmax);
			float tr = rayAABB_inv(bias, invDir, bvh->nodes[ri].BBmin, bvh->nodes[ri].BBmax);
			if (tl >= bestT) tl = FLT_MAX;
			if (tr >= bestT) tr = FLT_MAX;
			if (tl <= tr) {
				if (tr < FLT_MAX) stack[top++] = ri;
				if (tl < FLT_MAX) stack[top++] = li;
			} else {
				if (tl < FLT_MAX) stack[top++] = li;
				if (tr < FLT_MAX) stack[top++] = ri;
			}
		}
	}

	if (*hitTriIdx >= 0 && hitPosWorld) {
		float3 lh = {
			rayOrigin.x + bestT * rayDir.x,
			rayOrigin.y + bestT * rayDir.y,
			rayOrigin.z + bestT * rayDir.z};
		// Use cached forward rotation matrix to avoid 6 trig calls in TransformPointTRS
		*hitPosWorld = (float3){
			obj->_fwdRot0.x * lh.x * obj->scale.x + obj->_fwdRot0.y * lh.y * obj->scale.y + obj->_fwdRot0.z * lh.z * obj->scale.z + obj->position.x,
			obj->_fwdRot1.x * lh.x * obj->scale.x + obj->_fwdRot1.y * lh.y * obj->scale.y + obj->_fwdRot1.z * lh.z * obj->scale.z + obj->position.y,
			obj->_fwdRot2.x * lh.x * obj->scale.x + obj->_fwdRot2.y * lh.y * obj->scale.y + obj->_fwdRot2.z * lh.z * obj->scale.z + obj->position.z};
	}
}

bool IntersectBVH_Shadow(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir) {
	if (!obj || !bvh || bvh->nodeCount == 0) return false;
	float3 t = {rayOrigin.x - obj->position.x, rayOrigin.y - obj->position.y, rayOrigin.z - obj->position.z};
	float3 r0 = obj->_invScale, r1 = obj->_invRotSin, r2 = obj->_invRotCos;
	rayOrigin = (float3){
		r0.x * t.x + r0.y * t.y + r0.z * t.z,
		r1.x * t.x + r1.y * t.y + r1.z * t.z,
		r2.x * t.x + r2.y * t.y + r2.z * t.z};
	rayDir = (float3){
		r0.x * rayDir.x + r0.y * rayDir.y + r0.z * rayDir.z,
		r1.x * rayDir.x + r1.y * rayDir.y + r1.z * rayDir.z,
		r2.x * rayDir.x + r2.y * rayDir.y + r2.z * rayDir.z};
	float3 invDir = {1.0f / rayDir.x, 1.0f / rayDir.y, 1.0f / rayDir.z};
	float3 bias = {rayOrigin.x * invDir.x, rayOrigin.y * invDir.y, rayOrigin.z * invDir.z};
	int stack[64];
	int top = 0;
	stack[top++] = 0;
	while (top > 0) {
		const BVHNode *node = &bvh->nodes[stack[--top]];
		if (node->triCount > 0) {
			for (int i = 0; i < node->triCount; i++) {
				int ti = bvh->triIndices[node->triStart + i];
				float hit;
				if (rayTriangle(rayOrigin, rayDir, obj->v1[ti], obj->v2[ti], obj->v3[ti], &hit))
					return true;
			}
		} else {
			int li = node->leftFirst, ri = li + 1;
			if (rayAABB_inv(bias, invDir, bvh->nodes[li].BBmin, bvh->nodes[li].BBmax) < FLT_MAX) stack[top++] = li;
			if (rayAABB_inv(bias, invDir, bvh->nodes[ri].BBmin, bvh->nodes[ri].BBmax) < FLT_MAX) stack[top++] = ri;
		}
	}
	return false;
}

void CalculateFaceEmissions(Object *obj, MaterialLib *lib) {
	if (!obj || !lib || obj->bvh.nodeCount == 0) return;

	obj->hasEmission = false;
	for (int t = 0; t < obj->triangleCount && !obj->hasEmission; t++) {
		int matId = obj->materialIds ? obj->materialIds[t] : -1;
		if (matId >= 0 && matId < lib->count && lib->entries[matId].emission > 0.0f)
			obj->hasEmission = true;
	}
	if (!obj->hasEmission) return;

	float3 bbMin = obj->BBmin, bbMax = obj->BBmax;
	float bMin[3] = {bbMin.x, bbMin.y, bbMin.z};
	float bMax[3] = {bbMax.x, bbMax.y, bbMax.z};

	// 6 orthographic sweeps in local mesh space — BVH lives here too, no TRS needed
	// uA/vA: face plane axes, wA: depth axis, wSide: 0=BBmin, 1=BBmax origin side
	static const struct {
		float dirX, dirY, dirZ;
		int uA, vA, wA, wSide;
	} faces[6] = {
		{0, 0, 1, 0, 1, 2, 0},	// front:  +Z from BBmin.z
		{0, 0, -1, 0, 1, 2, 1}, // back:   -Z from BBmax.z
		{1, 0, 0, 2, 1, 0, 0},	// right:  +X from BBmin.x
		{-1, 0, 0, 2, 1, 0, 1}, // left:   -X from BBmax.x
		{0, -1, 0, 0, 2, 1, 1}, // top:    -Y from BBmax.y (looking from above)
		{0, 1, 0, 0, 2, 1, 0},	// bottom: +Y from BBmin.y (looking from below)
	};
	EmissionMap *maps[6] = {
		&obj->frontFaceEmission,
		&obj->backFaceEmission,
		&obj->rightFaceEmission,
		&obj->leftFaceEmission,
		&obj->topFaceEmission,
		&obj->bottomFaceEmission,
	};

	for (int f = 0; f < 6; f++) {
		int uA = faces[f].uA, vA = faces[f].vA, wA = faces[f].wA;
		float uMin = bMin[uA], uSize = bMax[uA] - bMin[uA];
		float vMin = bMin[vA], vSize = bMax[vA] - bMin[vA];
		float wOrig = faces[f].wSide ? bMax[wA] + 0.001f : bMin[wA] - 0.001f;
		float3 dir = {faces[f].dirX, faces[f].dirY, faces[f].dirZ};
		float3 invDir = {
			fabsf(dir.x) > 1e-8f ? 1.0f / dir.x : FLT_MAX,
			fabsf(dir.y) > 1e-8f ? 1.0f / dir.y : FLT_MAX,
			fabsf(dir.z) > 1e-8f ? 1.0f / dir.z : FLT_MAX,
		};

		for (int h = 0; h < EMISSION_RESOLUTION; h++) {
			float vf = vMin + (h + 0.5f) / EMISSION_RESOLUTION * vSize;
			for (int w = 0; w < EMISSION_RESOLUTION; w++) {
				float uf = uMin + (w + 0.5f) / EMISSION_RESOLUTION * uSize;
				float ro[3];
				ro[uA] = uf;
				ro[vA] = vf;
				ro[wA] = wOrig;
				float3 rayO = {ro[0], ro[1], ro[2]};
				float3 bias = {rayO.x * invDir.x, rayO.y * invDir.y, rayO.z * invDir.z};

				float bestT = FLT_MAX;
				int hitTri = -1;
				int stack[64], top = 0;
				stack[top++] = 0;
				while (top > 0) {
					const BVHNode *node = &obj->bvh.nodes[stack[--top]];
					if (rayAABB_inv(bias, invDir, node->BBmin, node->BBmax) >= bestT) continue;
					if (node->triCount > 0) {
						for (int i = 0; i < node->triCount; i++) {
							int t = obj->bvh.triIndices[node->triStart + i];
							float hit;
							if (rayTriangle(rayO, dir, obj->v1[t], obj->v2[t], obj->v3[t], &hit) && hit < bestT) {
								bestT = hit;
								hitTri = t;
							}
						}
					} else {
						stack[top++] = node->leftFirst;
						stack[top++] = node->leftFirst + 1;
					}
				}

				float3 em = {0.0f, 0.0f, 0.0f};
				if (hitTri >= 0 && obj->materialIds) {
					int matId = obj->materialIds[hitTri];
					if (matId >= 0 && matId < lib->count && lib->entries[matId].emission > 0.0f) {
						float e = lib->entries[matId].emission;
						float3 c = lib->entries[matId].color;
						em = (float3){c.x * e, c.y * e, c.z * e};
					}
				}
				maps[f]->emissionMap[h][w] = em;
			}
		}
	}
}

float3 SampleEmission(const Object *objs, int objCount, float3 position, float3 direction, int queryObject, MaterialLib *lib) {
	(void)lib;
	if (!objs || queryObject < 0 || queryObject >= objCount) return (float3){0};
	const Object *emitter = &objs[queryObject];
	if (!emitter->hasEmission) return (float3){0};

	// fast reject: does ray even reach the emitter?
	float emitTMin, emitTMax;
	RayBoxItersect(emitter, position, direction, &emitTMin, &emitTMax);
	if (emitTMin >= emitTMax || emitTMax < 0.0f) return (float3){0};

	// occlusion: AABB pre-filter then precise BVH shadow check for potential blockers
	for (int i = 0; i < objCount; i++) {
		if (i == queryObject) continue;
		float tMin, tMax;
		RayBoxItersect(&objs[i], position, direction, &tMin, &tMax);
		if (tMin < tMax && tMin > 0.0f && tMin < emitTMin)
			if (IntersectBVH_Shadow(&objs[i], &objs[i].bvh, position, direction))
				return (float3){0};
	}

	// transform position and direction into emitter local mesh space via cached inverse TRS
	float3 r0 = emitter->_invScale, r1 = emitter->_invRotSin, r2 = emitter->_invRotCos;
	float3 tp = {position.x - emitter->position.x, position.y - emitter->position.y, position.z - emitter->position.z};
	float3 lp = {
		r0.x * tp.x + r0.y * tp.y + r0.z * tp.z,
		r1.x * tp.x + r1.y * tp.y + r1.z * tp.z,
		r2.x * tp.x + r2.y * tp.y + r2.z * tp.z,
	};
	float3 ld = {
		r0.x * direction.x + r0.y * direction.y + r0.z * direction.z,
		r1.x * direction.x + r1.y * direction.y + r1.z * direction.z,
		r2.x * direction.x + r2.y * direction.y + r2.z * direction.z,
	};

	// select face by dominant local-space axis, intersect ray with face plane to get UV
	float3 bbMin = emitter->BBmin, bbMax = emitter->BBmax;
	float ax = fabsf(ld.x), ay = fabsf(ld.y), az = fabsf(ld.z);
	EmissionMap *map;
	float uf, vf, uMin, uSize, vMin, vSize;

	if (az >= ax && az >= ay) {
		float plane = ld.z > 0.0f ? bbMin.z : bbMax.z;
		float t = (plane - lp.z) / ld.z;
		uf = lp.x + t * ld.x;
		vf = lp.y + t * ld.y;
		uMin = bbMin.x;
		uSize = bbMax.x - bbMin.x;
		vMin = bbMin.y;
		vSize = bbMax.y - bbMin.y;
		map = ld.z > 0.0f ? &emitter->frontFaceEmission : &emitter->backFaceEmission;
	} else if (ax >= ay) {
		float plane = ld.x > 0.0f ? bbMin.x : bbMax.x;
		float t = (plane - lp.x) / ld.x;
		uf = lp.z + t * ld.z;
		vf = lp.y + t * ld.y;
		uMin = bbMin.z;
		uSize = bbMax.z - bbMin.z;
		vMin = bbMin.y;
		vSize = bbMax.y - bbMin.y;
		map = ld.x > 0.0f ? &emitter->rightFaceEmission : &emitter->leftFaceEmission;
	} else {
		float plane = ld.y < 0.0f ? bbMax.y : bbMin.y;
		float t = (plane - lp.y) / ld.y;
		uf = lp.x + t * ld.x;
		vf = lp.z + t * ld.z;
		uMin = bbMin.x;
		uSize = bbMax.x - bbMin.x;
		vMin = bbMin.z;
		vSize = bbMax.z - bbMin.z;
		map = ld.y < 0.0f ? &emitter->topFaceEmission : &emitter->bottomFaceEmission;
	}

	if (uSize < 1e-6f || vSize < 1e-6f) return (float3){0};
	int wi = (int)((uf - uMin) / uSize * EMISSION_RESOLUTION);
	int hi = (int)((vf - vMin) / vSize * EMISSION_RESOLUTION);
	wi = wi < 0 ? 0 : wi >= EMISSION_RESOLUTION ? EMISSION_RESOLUTION - 1
												: wi;
	hi = hi < 0 ? 0 : hi >= EMISSION_RESOLUTION ? EMISSION_RESOLUTION - 1
												: hi;
	return map->emissionMap[hi][wi];
}