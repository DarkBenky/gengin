#ifndef RAY_H
#define RAY_H

#include <pthread.h>
#include "../../object/format.h"
#include "../../object/object.h"
#include "../../skybox/skybox.h"
#include "../../util/threadPool.h"
#include "../render.h"

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
} RayTraceTask;

typedef struct {
	RayTraceTask tasks[HEIGHT];
} RayTraceTaskQueue;

void RayTraceScene(const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib, RayTraceTaskQueue *taskQueue, ThreadPool *threadPool);

// Persistent raytrace workers — live for program lifetime, sync via barriers each frame
typedef struct RayTracer RayTracer;
RayTracer *RayTracerCreate(int nthreads);
void RayTracerDestroy(RayTracer *rt);
void RayTracerRender(RayTracer *rt, const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib);

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera, int resolution, int frameInterval);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
float3 ComputeRayDirection(const Camera *camera, int pixelX, int pixelY);
void applySkybox(const Skybox *skybox, Camera *camera, ThreadPool *threadPool, SkyBoxTaskQueue *taskQueue);

#endif // RAY_H