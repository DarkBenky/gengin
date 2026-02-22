#ifndef OBJECT_H
#define OBJECT_H

#include "format.h"
#include "../load/loadObj.h"

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

	float3 _temp; // used for testing and debugging, can be removed later

	float3 *v1;
	float3 *v2;
	float3 *v3;
	float3 *normals;
	float3 *colors;
	float *roughness;
	float *metallic;
	float *emission;
	int triangleCount;
} Object;

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color);
void Object_UpdateWorldBounds(Object *obj);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
float3 IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);

// void CreteObjectBVH(Object *obj); TODO: Implement BVH for faster ray intersection
// void DestroyObjectBVH(Object *obj);
// void IntersectBVH(const Object *obj, float3 rayOrigin, float3 rayDir, int *hitTriIdx);


#endif // OBJECT_H