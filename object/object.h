#ifndef OBJECT_H
#define OBJECT_H

#include "format.h"
#include "../load/loadObj.h"

typedef struct Object {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	Triangle *triangles;
	int triangleCount;
} Object;

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);

#endif // OBJECT_H