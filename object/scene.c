#include "scene.h"

#include <math.h>
#include <stdlib.h>

enum {
	kCubeCount = 100,
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

	CreateCube(&objects[kGroundIndex], (float3){0.0f, kGroundY, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){40.0f, 0.2f, 40.0f}, (float3){0.32f, 0.34f, 0.38f});
	Object_UpdateWorldBounds(&objects[kGroundIndex]);

	const int cols = 10;
	const int rows = 10;
	const float spacingX = 1.1f;
	const float spacingZ = 1.3f;
	const float startX = -((cols - 1) * spacingX) * 0.5f;
	const float startZ = 9.0f;

	for (int i = 0; i < kCubeCount; i++) {
		int ix = i % cols;
		int iz = i / cols;
		float tx = (float)ix / (float)(cols - 1);
		float tz = (float)iz / (float)(rows - 1);
		float x = startX + (float)ix * spacingX;
		float z = startZ + (float)iz * spacingZ;
		float3 cubeColor = (float3){0.2f + 0.7f * tx, 0.15f + 0.7f * (1.0f - tz), 0.3f + 0.6f * (1.0f - tx)};

		float rotateX = (float)(i % 5) * 0.628f;
		float rotateY = (float)(i % 7) * 0.449f;
		float rotateZ = (float)(i % 3) * 0.524f;
		float3 rotation = (float3){rotateX, rotateY, rotateZ};

		CreateCube(&objects[kCubesStart + i], (float3){x, 0.2f, z}, rotation, (float3){0.65f, 0.65f, 0.65f}, cubeColor);
		Object_UpdateWorldBounds(&objects[kCubesStart + i]);
	}
}

void DemoScene_Update(Object *objects, int frame) {
	if (!objects) return;

	const int cols = 10;
	const float spacingX = 1.1f;
	const float spacingZ = 1.3f;
	const float startX = -((cols - 1) * spacingX) * 0.5f;
	const float startZ = 9.0f;

	for (int i = 0; i < kCubeCount; i++) {
		Object *cube = &objects[kCubesStart + i];
		int ix = i % cols;
		int iz = i / cols;

		cube->rotation.y += 0.018f + 0.002f * (float)(i % 7);
		cube->rotation.x += 0.005f + 0.001f * (float)(i % 3);

		float orbit = (float)frame * (0.006f + 0.0008f * (float)(i % 11));
		float baseX = startX + (float)ix * spacingX;
		float baseZ = startZ + (float)iz * spacingZ;
		cube->position.x = baseX + 0.2f * sinf(orbit + (float)ix);
		cube->position.z = baseZ + 0.2f * cosf(orbit * 0.7f + (float)iz);
		cube->position.y = 0.2f + 0.2f * sinf(orbit * 0.5f + (float)i * 0.3f);

		Object_UpdateWorldBounds(cube);
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
