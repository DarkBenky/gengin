#include "scene.h"

#include <math.h>
#include <stdlib.h>

enum {
	kCubeCount = 7,
	kGroundIndex = 0,
	kCubesStart = 1,
	kObjectCount = 1 + kCubeCount,
};

static const float kGroundY = -1.25f;

int DemoScene_ObjectCount(void) {
	return kObjectCount;
}

void DemoScene_Build(Object *objects) {
	if (!objects) return;

	CreateCube(&objects[kGroundIndex], (float3){0.0f, kGroundY, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){20.0f, 0.2f, 20.0f}, (float3){0.32f, 0.34f, 0.38f});

	for (int i = 0; i < kCubeCount; i++) {
		float t = (float)i / (float)kCubeCount;
		float x = -4.5f + (float)i * 1.8f;
		float z = 11.0f + ((i % 2) == 0 ? 0.0f : 2.0f);
		float3 cubeColor = (float3){0.25f + 0.55f * t, 0.35f + 0.45f * (1.0f - t), 0.55f + 0.35f * t};

		CreateCube(&objects[kCubesStart + i], (float3){x, 0.2f, z}, (float3){0.0f, 0.0f, 0.0f}, (float3){0.9f, 0.9f, 0.9f}, cubeColor);
	}
}

void DemoScene_Update(Object *objects, int frame) {
	if (!objects) return;

	for (int i = 0; i < kCubeCount; i++) {
		Object *cube = &objects[kCubesStart + i];

		cube->rotation.y += 0.02f + 0.003f * (float)i;
		cube->rotation.x += 0.006f + 0.001f * (float)(i % 3);

		float orbit = (float)frame * (0.008f + 0.001f * (float)i);
		float baseX = -4.5f + (float)i * 1.8f;
		float baseZ = 12.0f + ((i % 2) == 0 ? 0.0f : 2.0f);
		cube->position.x = baseX + 0.25f * sinf(orbit);
		cube->position.z = baseZ + 0.25f * cosf(orbit * 0.8f);
		cube->position.y = 0.2f + 0.15f * sinf(orbit * 0.6f + (float)i);
	}
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
