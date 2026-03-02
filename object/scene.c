#include "scene.h"

#include <math.h>
#include <stdlib.h>

enum {
	kGroundIndex = 0,
	kRocketIndex = 1,
	kObjectCount = 2,
};

static const float kGroundY = -1.25f;

int DemoScene_ObjectCount(void) {
	return kObjectCount;
}

void DemoScene_Build(Object *objects, MaterialLib *lib) {
	if (!objects) return;

	CreateCube(&objects[kGroundIndex], (float3){0.0f, kGroundY, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){40.0f, 0.2f, 40.0f}, (float3){0.32f, 0.34f, 0.38f}, lib);
	Object_UpdateWorldBounds(&objects[kGroundIndex]);

	Object_Init(&objects[kRocketIndex], (float3){0.0f, 0.0f, 10.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, "assets/models/r27.bin", lib);
	Object_UpdateWorldBounds(&objects[kRocketIndex]);
}

void DemoScene_Update(Object *objects, int frame) {
	(void)objects;
	(void)frame;
}

void Scene_Destroy(Object *objects, int objectCount) {
	if (!objects) return;
	for (int i = 0; i < objectCount; i++) {
		Object_Destroy(&objects[i]);
	}
	free(objects);
}

int Scene_CountTriangles(const Object *objects, int objectCount) {
	if (!objects || objectCount <= 0) return 0;
	int total = 0;
	for (int i = 0; i < objectCount; i++) {
		total += objects[i].triangleCount;
	}
	return total;
}
