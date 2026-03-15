#ifndef RAY_H
#define RAY_H

#include <pthread.h>
#include "../../object/format.h"
#include "../../object/object.h"
#include "../../skybox/skybox.h"
#include "../../util/threadPool.h"
#include "../render.h"

#define REFLECTION_RESOLUTION 4 // 1 = full, 2 = half, 4 = quarter, etc.
#define BLUR_RADIUS 3
#define TOP_EMISSIVE_OBJECTS 3 // only consider the top N closest emissive objects for reflections to save ray casts

typedef struct {
	int row;
	Camera *camera;
	Skybox *skybox;
} SkyBoxTask;

typedef struct {
	SkyBoxTask tasks[HEIGHT];
} SkyBoxTaskQueue;

typedef struct {
	int row;
	Camera *camera;
	const Object *objects;
	int objectCount;
	const MaterialLib *lib;
	const Skybox *skybox;
	Frustum frustum;
	const int *frustumPassIndices;
	int frustumPassCount;
} RayTraceTask;

typedef struct {
	RayTraceTask tasks[HEIGHT];
} RayTraceTaskQueue;

void RayTraceScene(const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib, RayTraceTaskQueue *taskQueue, ThreadPool *threadPool, const Skybox *skybox);

// Persistent raytrace workers — live for program lifetime, sync via barriers each frame
typedef struct RayTracer RayTracer;
RayTracer *RayTracerCreate(int nthreads);
void RayTracerDestroy(RayTracer *rt);
void RayTracerRender(RayTracer *rt, const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib);

typedef struct {
	float3 pos;
	float3 normal; // world-space, normalized
	Material mat;
	int objIdx;
	int triIdx;
} RayHit;

// Wrapper around the internal rayCollision — resolves normal and material on hit.
// Returns true if something was hit. excludeObj is the object index to skip (-1 for none).
bool RayCast(Object *objects, int objectCount, float3 rayOrigin, float3 rayDir, int excludeObj, const MaterialLib *lib, RayHit *hit);

// void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera, int resolution, int frameInterval);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
float3 ComputeRayDirection(const Camera *camera, int pixelX, int pixelY);

void DitherPostProcess(Camera *camera, int frame);
void DitherOrderedPostProcess(Camera *camera, int frame);

#endif // RAY_H