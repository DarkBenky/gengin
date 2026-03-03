#include "testRay.h"
#include "timings.h"
#include <math.h>
#define SAMPLES 20
#define GRID_COLS 6
#define GRID_ROWS 7
#define PLANE_COUNT (GRID_COLS * GRID_ROWS)
#define OBJECT_COUNT (PLANE_COUNT + 2)

void RenderScene(const Object *objects, int objectCount, Camera *camera) {
	if (!objects || !camera || objectCount <= 0) return;
	for (int row = 0; row < camera->screenHeight; row++) {
		for (int col = 0; col < camera->screenWidth; col++) {
			float3 rayDir = ComputeRayDirection(camera, col, row);
			Color color = IntersectBBoxColor(objects, objectCount, camera->position, rayDir);
			camera->framebuffer[row * camera->screenWidth + col] = color;
		}
	}
}

void RenderSceneRow(const Object *objects, int objectCount, Camera *camera, int row, int rowCount) {
	if (!objects || !camera || objectCount <= 0 || row < 0 || row >= camera->screenHeight) return;
	for (int r = row; r < row + rowCount && r < camera->screenHeight; r++) {
		for (int col = 0; col < camera->screenWidth; col++) {
			float3 rayDir = ComputeRayDirection(camera, col, r);
			Color color = IntersectBBoxColor(objects, objectCount, camera->position, rayDir);
			camera->framebuffer[r * camera->screenWidth + col] = color;
		}
	}
}

void RenderTaskFunction(void *arg) {
	RenderTask *task = (RenderTask *)arg;
	RenderSceneRow(task->objects, task->objectCount, task->camera, task->row, task->rowCount);
}

static void RunMultiThreaded(const Object *objects, int objectCount, Camera *camera, int rowsPerTask) {
	int height = camera->screenHeight;
	int taskCount = (height + rowsPerTask - 1) / rowsPerTask;
	RenderTask *tasks = malloc(sizeof(RenderTask) * taskCount);
	if (!tasks) return;

	ThreadPool *p = poolCreate(32, taskCount);

	float timeTook[SAMPLES] = {0};
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int t = 0; t < taskCount; t++) {
			tasks[t].objects = objects;
			tasks[t].objectCount = objectCount;
			tasks[t].camera = camera;
			tasks[t].row = t * rowsPerTask;
			tasks[t].rowCount = rowsPerTask;
			poolAdd(p, RenderTaskFunction, &tasks[t]);
		}
		poolWait(p);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	poolDestroy(p);
	free(tasks);

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Multi-threaded (%d rows/task, %d tasks):\n", rowsPerTask, taskCount);
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time:  %f seconds\n", metrics.medianTime);
	printf("Min Time:     %f seconds\n", metrics.minTime);
	printf("Max Time:     %f seconds\n", metrics.maxTime);
	printf("Variance:     %f\n", metrics.variance);
	printf("99th Pct:     %f seconds\n", metrics.p99Time);
}

int main() {
	int objectCount = OBJECT_COUNT;
	Object *objects = malloc(sizeof(Object) * objectCount);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		return 1;
	}

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	const float spacingX = 7.0f, spacingZ = 7.0f;
	const float startX = -(GRID_COLS - 1) * spacingX * 0.5f;
	const float startZ = 5.0f;
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			int idx = row * GRID_COLS + col;
			if (col % 2 == 0) {
				LoadObj("assets/models/r27.bin", &objects[idx], &matLib);
			} else {
				LoadObj("assets/models/f16.bin", &objects[idx], &matLib);
			}
			objects[idx].rotation = (float3){0.0f, -0.5708f, 0.0f};
			objects[idx].scale = (float3){2.0f, 2.0f, 2.0f};
			objects[idx].position = (float3){startX + col * spacingX, -0.09f, startZ + row * spacingZ};
			CreateObjectBVH(&objects[idx], &objects[idx].bvh);
			Object_UpdateWorldBounds(&objects[idx]);
		}
	}

	LoadObj("assets/models/map.bin", &objects[PLANE_COUNT], &matLib);
	objects[PLANE_COUNT].rotation = (float3){0.0f, 0.0f, 0.0f};
	objects[PLANE_COUNT].scale = (float3){5.0f, 9.5f, 5.0f};
	objects[PLANE_COUNT].position = (float3){0.0f, -75.0f, 0.0f};
	CreateObjectBVH(&objects[PLANE_COUNT], &objects[PLANE_COUNT].bvh);
	Object_UpdateWorldBounds(&objects[PLANE_COUNT]);

	CreateCube(&objects[PLANE_COUNT + 1], (float3){3.0f, 2.0f, 10.0f}, (float3){3.0f, 4.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, (float3){0.8f, 0.2f, 0.2f, 0.1f}, &matLib);
	Object_UpdateWorldBounds(&objects[PLANE_COUNT + 1]);

	printf("Scene loaded. Total triangles: %d\n", Scene_CountTriangles(objects, objectCount));

	Camera camera;
	initCamera(&camera, 800, 600, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	// Single-threaded baseline
	float timeTook[SAMPLES] = {0};
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		RenderScene(objects, objectCount, &camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Single-threaded Ray Performance:\n");
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time:  %f seconds\n", metrics.medianTime);
	printf("Min Time:     %f seconds\n", metrics.minTime);
	printf("Max Time:     %f seconds\n", metrics.maxTime);
	printf("Variance:     %f\n", metrics.variance);
	printf("99th Pct:     %f seconds\n", metrics.p99Time);

	// Multi-threaded: sweep rows-per-task
	Camera camMT;
	initCamera(&camMT, 800, 600, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	static const int rowsPerTaskValues[] = {1, 2, 4, 16, 32, 64};
	int testCount = (int)(sizeof(rowsPerTaskValues) / sizeof(rowsPerTaskValues[0]));
	for (int t = 0; t < testCount; t++) {
		RunMultiThreaded(objects, objectCount, &camMT, rowsPerTaskValues[t]);
	}
	printf("========================================\n");

	// Correctness check: single-threaded vs 1-row multi-threaded
	Camera camCheck;
	initCamera(&camCheck, 800, 600, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});
	RunMultiThreaded(objects, objectCount, &camCheck, 1);

	for (int i = 0; i < camera.screenWidth * camera.screenHeight; i++) {
		if (camera.framebuffer[i] != camCheck.framebuffer[i]) {
			printf("Pixel %d mismatch: single = 0x%08X, multi = 0x%08X\n", i, camera.framebuffer[i], camCheck.framebuffer[i]);
			Scene_Destroy(objects, objectCount);
			MaterialLib_Destroy(&matLib);
			destroyCamera(&camera);
			destroyCamera(&camMT);
			destroyCamera(&camCheck);
			return 1;
		}
	}
	printf("Correctness check passed.\n");

	Scene_Destroy(objects, objectCount);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	destroyCamera(&camMT);
	destroyCamera(&camCheck);
	return 0;
}