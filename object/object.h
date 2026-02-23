#ifndef OBJECT_H
#define OBJECT_H

#include "format.h"
#include "../load/loadObj.h"
#include "../math/matrix.h"

typedef struct Object {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	float3 worldBBmin;
	float3 worldBBmax;
	Triangle *triangles;
	int triangleCount;
	float4x4 transform;
	float4x4 invTransform;
	float4x4 rotationMatrix;
} Object;

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color);
void Object_UpdateTransform(Object *obj);
void Object_UpdateWorldBounds(Object *obj);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);

#endif // OBJECT_H