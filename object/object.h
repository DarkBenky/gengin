#ifndef OBJECT_H
#define OBJECT_H

#include <math.h>

#define EMISSION_RESOLUTION 32

#include "format.h"
#include "../load/loadObj.h"
#include "material/material.h"

typedef struct BVHNode {
	float BBmin[3]; // 12 bytes (no float3 padding waste)
	float BBmax[3]; // 12 bytes
	union {
		int leftFirst; // internal node: index of left child (right = leftFirst+1)
		int triStart;  // leaf node: start index into triIndices
	};
	int triCount; // 0 = internal node, >0 = leaf
} BVHNode;		  // 32 bytes — 2 nodes per cache line

typedef struct BVH {
	BVHNode *nodes;
	int *triIndices; // reordered triangle indices
	int nodeCount;
} BVH;
typedef struct EmissionMap {
	float3 emissionMap[EMISSION_RESOLUTION][EMISSION_RESOLUTION]; // precomputed per-face emission for a 32x32 grid of world positions (for direct lighting)
} EmissionMap;

typedef struct Object {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	float3 worldBBmin;
	float3 worldBBmax;

	// cached inverse transform — recomputed in Object_UpdateWorldBounds
	float3 _invScale;  // row 0 of M = Diag(invScale)*InvRot
	float3 _invRotSin; // row 1 of M
	float3 _invRotCos; // row 2 of M
	// cached forward rotation matrix Rz*Ry*Rx — for rotating normals without trig each hit
	float3 _fwdRot0;
	float3 _fwdRot1;
	float3 _fwdRot2;

	Color _temp; // pre-packed RGB color for BBOX hits

	float3 *v1;
	float3 *v2;
	float3 *v3;
	float3 *normals;
	int *materialIds;
	int triangleCount;

	bool hasEmission; // quick check to skip emission sampling when no faces emit
	EmissionMap frontFaceEmission;
	EmissionMap backFaceEmission;
	EmissionMap leftFaceEmission;
	EmissionMap rightFaceEmission;
	EmissionMap topFaceEmission;
	EmissionMap bottomFaceEmission;

	BVH bvh;
} Object;

// calculate per-face emission maps with orthographic projection
void CalculateFaceEmissions(Object *obj, MaterialLib *lib);
// trace ray in direction of query object if we hit something before query object return 0 else return emission from query object
float3 SampleEmission(const Object *objs, int objCount, float3 position, float3 direction, int queryObject, MaterialLib *lib);

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib, float emission, float roughness, float metallic);
void Object_UpdateWorldBounds(Object *obj);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
Color IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
bool ObjectBehindCamera(const Object *obj, float3 camPos, float3 camForward);

// Set material properties on every triangle of an object (updates the shared MaterialLib entries).
void Object_SetMaterial(Object *obj, MaterialLib *lib, Material mat);
void Object_SetColor(Object *obj, MaterialLib *lib, float3 color);
void Object_SetEmission(Object *obj, MaterialLib *lib, float emission);
void Object_SetRoughness(Object *obj, MaterialLib *lib, float roughness);
void Object_SetMetallic(Object *obj, MaterialLib *lib, float metallic);

void CreateObjectBVH(Object *obj, BVH *bvh);
void DestroyObjectBVH(BVH *bvh);
void IntersectBVH(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir, int *hitTriIdx, float3 *hitPosWorld);
bool IntersectBVH_Shadow(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir);

// Perspective frustum — 5 planes (near, left, right, bottom, top), all inward-facing.
// Test: dot(normal, P) + d >= 0 means P is on the inside of the plane.
typedef struct {
	float3 normal;
	float d;
} FrustumPlane;

typedef struct {
	FrustumPlane planes[5];
} Frustum;

Frustum Frustum_FromCamera(const Camera *cam);
// Returns true if the AABB may be visible (not fully outside any plane).
bool Frustum_TestAABB(const Frustum *f, float3 bbMin, float3 bbMax);

// Branchless slab test with precomputed bias = ro * invRd.
// Returns tmin on hit, FLT_MAX on miss. Shared between object.c and ray.c.
static inline float rayAABB_inv(float3 bias, float3 invRd, const float *mn, const float *mx) {
	float tx0 = mn[0] * invRd.x - bias.x, tx1 = mx[0] * invRd.x - bias.x;
	float ty0 = mn[1] * invRd.y - bias.y, ty1 = mx[1] * invRd.y - bias.y;
	float tz0 = mn[2] * invRd.z - bias.z, tz1 = mx[2] * invRd.z - bias.z;
	float tmin = fmaxf(fmaxf(fminf(tx0, tx1), fminf(ty0, ty1)), fminf(tz0, tz1));
	float tmax = fminf(fminf(fmaxf(tx0, tx1), fmaxf(ty0, ty1)), fmaxf(tz0, tz1));
	return tmax < tmin ? FLT_MAX : tmin;
}

#endif // OBJECT_H