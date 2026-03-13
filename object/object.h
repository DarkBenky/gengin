#ifndef OBJECT_H
#define OBJECT_H

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

	float3 worldCenter;      // precomputed world-space AABB center
	float3 avgEmission;      // average face emission (color * strength) across emissive tris
	float3 *faceEmission;    // per-triangle precomputed emission: color * emission_strength (NULL if !hasEmission)
	int hasEmission;         // 1 if any face has emission > 0

	float3 *v1;
	float3 *v2;
	float3 *v3;
	float3 *normals;
	int *materialIds;
	int triangleCount;

	BVH bvh;
} Object;

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib);
void Object_UpdateWorldBounds(Object *obj);
void Object_PrecomputeEmission(Object *obj, const MaterialLib *lib);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
Color IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
bool ObjectBehindCamera(const Object *obj, float3 camPos, float3 camForward);

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

#endif // OBJECT_H