#ifndef OBJECT_H
#define OBJECT_H

#include "format.h"
#include "../load/loadObj.h"
#include "material/material.h"

typedef struct BVHNode {
	float3 BBmin;
	float3 BBmax;
	int leftFirst;
	int triStart;
	int triCount;
} BVHNode;

typedef struct BVH {
	BVHNode *nodes;
	int nodeCount;
} BVH;

// typedef struct ObjectManager {
// TODO: BVH for objects in the scene for faster ray intersection
// }

typedef struct Object {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	float3 worldBBmin;
	float3 worldBBmax;

	Color _temp; // pre-packed RGB color for BBOX hits

	float3 *v1;
	float3 *v2;
	float3 *v3;
	float3 *normals;
	int *materialIds;
	int triangleCount;
} Object;

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib);
void Object_UpdateWorldBounds(Object *obj);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
Color IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);

// void CreteObjectBVH(Object *obj); TODO: Implement BVH for faster ray intersection
// void DestroyObjectBVH(Object *obj);
// void IntersectBVH(const Object *obj, float3 rayOrigin, float3 rayDir, int *hitTriIdx);

#endif // OBJECT_H