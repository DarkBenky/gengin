// testRayColumnBench.c — benchmarks row- vs column-based ray tracing on a static
// scene (ground plane + cubes + fighter jets) and saves both rendered frames as
// BMPs so they can be compared visually.
// Compile with: make test testRayColumnBench
#include "testRayColumnBench.h"
#include "timings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SAMPLES 128
#define GRID_COLS 6
#define GRID_ROWS 7
#define CUBE_COUNT (GRID_COLS * GRID_ROWS)
#define F16_COUNT 3
#define OBJECT_COUNT (1 + CUBE_COUNT + F16_COUNT) // plane + cubes + fighter jets

// static scene: ground plane, cube grid and a few fighter jets/missiles on top
static void BuildBenchScene(Object *objects, MaterialLib *lib) {
	int idx = 0;

	// ground plane (flat cube)
	CreateCube(&objects[idx], (float3){0.0f, -0.5f, 10.0f}, (float3){0.0f, 0.0f, 0.0f},
			   (float3){80.0f, 1.0f, 80.0f}, (float3){0.32f, 0.34f, 0.38f}, lib, 0.0f, 0.9f, 0.0f);
	objects[idx].prevPostion = objects[idx].position;
	objects[idx].prevRotation = objects[idx].rotation;
	objects[idx].prevScale = objects[idx].scale;
	idx++;

	// cube grid with deterministic material variation
	const float spacingX = 8.0f, spacingZ = 8.0f;
	const float startX = -(GRID_COLS - 1) * spacingX * 0.5f;
	const float startZ = 10.0f - (GRID_ROWS - 1) * spacingZ * 0.5f;
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			float3 color = {
				0.2f + 0.8f * ((idx * 37) % 11) / 10.0f,
				0.2f + 0.8f * ((idx * 53) % 11) / 10.0f,
				0.2f + 0.8f * ((idx * 71) % 11) / 10.0f,
			};
			float emission = (idx % 4 == 0) ? 6.0f : 0.0f;
			float roughness = 0.1f + 0.7f * ((idx * 13) % 5) / 4.0f;
			float metallic = (idx % 3 == 0) ? 0.8f : 0.0f;

			CreateCube(&objects[idx], (float3){startX + col * spacingX, 1.0f, startZ + row * spacingZ},
					   (float3){0.0f, 0.0f, 0.0f}, (float3){2.0f, 2.0f, 2.0f}, color, lib,
					   emission, roughness, metallic);
			objects[idx].prevPostion = objects[idx].position;
			objects[idx].prevRotation = objects[idx].rotation;
			objects[idx].prevScale = objects[idx].scale;
			idx++;
		}
	}

	// fighter jets — staggered on the plane behind the cube grid
	for (int i = 0; i < F16_COUNT; i++) {
		LoadObj("assets/models/f16.bin", &objects[idx], lib);
		objects[idx].position = (float3){-15.0f + i * 6.0f, 0.8f, 38.0f + (i % 2) * 6.0f};
		objects[idx].rotation = (float3){0.0f, (float)(i % 2 == 0 ? -0.5f : 0.5f), 0.0f};
		objects[idx].scale = (float3){2.0f, 2.0f, 2.0f};
		objects[idx].prevPostion = objects[idx].position;
		objects[idx].prevRotation = objects[idx].rotation;
		objects[idx].prevScale = objects[idx].scale;
		CreateObjectBVH(&objects[idx], &objects[idx].bvh);
		Object_UpdateWorldBounds(&objects[idx]);
		idx++;
	}
}

static void InitBenchCamera(Camera *camera) {
	initCamera(camera, WIDTH, HEIGHT, 90.0f, (float3){0.0f, 2.0f, -7.0f},
			   (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});
}

static void RenderRow(const Object *objects, int objectCount, Camera *camera, ThreadPool *pool,
					  RayTraceTaskQueue *queue, const MaterialLib *lib, const Skybox *skybox) {
	RayTraceScene(objects, objectCount, camera, lib, queue, pool, skybox);
}

static void RenderColumn(const Object *objects, int objectCount, Camera *camera, ThreadPool *pool,
						 RayTraceTaskQueue *queue, const MaterialLib *lib, const Skybox *skybox) {
	RayTraceSceneColumn(objects, objectCount, camera, lib, queue, pool, skybox);
}

int main(void) {
	Object *objects = malloc(sizeof(Object) * OBJECT_COUNT);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		return 1;
	}

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);
	BuildBenchScene(objects, &matLib);

	Skybox skybox;
	LoadSkybox(&skybox, "skybox");

	// WIDTH >= HEIGHT, so this queue holds one task per column and the row path fits too
	ThreadPool *pool = poolCreate(32, WIDTH);
	if (!pool) {
		fprintf(stderr, "Failed to create thread pool\n");
		return 1;
	}
	RayTraceTaskQueue rayTaskQueue;

	Camera camRow, camCol;
	InitBenchCamera(&camRow);
	InitBenchCamera(&camCol);
	// static camera + scene: prev == current so motion vectors are zero
	RenderSetup(objects, OBJECT_COUNT, &camRow);
	RenderSetup(objects, OBJECT_COUNT, &camCol);
	ComputePrevCameraPos(&camRow);
	ComputePrevCameraPos(&camCol);

	printf("=== testRayColumnBench: plane + %d cubes (%d tris), %dx%d, 32 threads, %d samples ===\n",
		   CUBE_COUNT, Scene_CountTriangles(objects, OBJECT_COUNT), WIDTH, HEIGHT, SAMPLES);

	// render one frame with each traversal and save both for visual comparison
	RenderRow(objects, OBJECT_COUNT, &camRow, pool, &rayTaskQueue, &matLib, &skybox);
	SaveImage("tests/img/rayBench_row.bmp", &camRow);
	RenderColumn(objects, OBJECT_COUNT, &camCol, pool, &rayTaskQueue, &matLib, &skybox);
	SaveImage("tests/img/rayBench_column.bmp", &camCol);
	printf("Saved tests/img/rayBench_row.bmp and tests/img/rayBench_column.bmp\n");

	// warm-up
	for (int i = 0; i < 3; i++) {
		RenderRow(objects, OBJECT_COUNT, &camRow, pool, &rayTaskQueue, &matLib, &skybox);
		RenderColumn(objects, OBJECT_COUNT, &camCol, pool, &rayTaskQueue, &matLib, &skybox);
	}

	float timesRow[SAMPLES], timesCol[SAMPLES];
	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		RenderRow(objects, OBJECT_COUNT, &camRow, pool, &rayTaskQueue, &matLib, &skybox);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesRow[s] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		RenderColumn(objects, OBJECT_COUNT, &camCol, pool, &rayTaskQueue, &matLib, &skybox);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesCol[s] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics mRow = ComputePerformanceMetrics(timesRow, SAMPLES);
	PerformanceMetrics mCol = ComputePerformanceMetrics(timesCol, SAMPLES);

	printf("Row    avg=%.3fms  median=%.3fms  p99=%.3fms\n",
		   mRow.averageTime * 1e3f, mRow.medianTime * 1e3f, mRow.p99Time * 1e3f);
	printf("Column avg=%.3fms  median=%.3fms  p99=%.3fms\n",
		   mCol.averageTime * 1e3f, mCol.medianTime * 1e3f, mCol.p99Time * 1e3f);
	if (mRow.medianTime > 0.0f && mCol.medianTime > 0.0f) {
		float speedup = mRow.medianTime / mCol.medianTime;
		const char *faster = speedup >= 1.0f ? "Column" : "Row";
		printf("Speedup: %.2fx (%s is %.1f%% faster)\n", speedup, faster, fabsf(speedup - 1.0f) * 100.0f);
	}

	poolDestroy(pool);
	DestroySkybox(&skybox);
	destroyCamera(&camRow);
	destroyCamera(&camCol);
	Scene_Destroy(objects, OBJECT_COUNT);
	MaterialLib_Destroy(&matLib);
	return 0;
}
