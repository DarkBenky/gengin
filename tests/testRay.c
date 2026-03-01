#include "testRay.h"
#include "timings.h"
#include <math.h>
#define OBJECT_COUNT 100
#define SAMPLES 100

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

void RenderSceneRow(const Object *objects, int objectCount, Camera *camera, int row) {
	if (!objects || !camera || objectCount <= 0 || row < 0 || row >= camera->screenHeight) return;
	for (int col = 0; col < camera->screenWidth; col++) {
		float3 rayDir = ComputeRayDirection(camera, col, row);
		Color color = IntersectBBoxColor(objects, objectCount, camera->position, rayDir);
		camera->framebuffer[row * camera->screenWidth + col] = color;
	}
}

void RenderTaskFunction(void *arg) {
	RenderTask *task = (RenderTask *)arg;
	RenderSceneRow(task->objects, task->objectCount, task->camera, task->row);
}

int main() {
	// Create a simple scene with one object
	Object objects[OBJECT_COUNT];
	for (int i = 0; i < OBJECT_COUNT; i++) {
		CreateCube(&objects[i], (float3){i * 2.0f, 0.0f, 5.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, (float3){1.0f, 1.0f, 1.0f}, NULL);
	}

	Camera camera;
	initCamera(&camera, 800, 600, 90.0f, (float3){0.0f, 0.0f, 0.0f}, (float3){0.0f, 0.0f, 1.0f}, (float3){1.0f, 1.0f, 1.0f});

	// Single-threaded test
	float timeTook[SAMPLES] = {0};
	for (int i = 0; i < SAMPLES; i++) {
		// printf("Running single-threaded sample %d/%d...\n", i + 1, SAMPLES);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		RenderScene(objects, OBJECT_COUNT, &camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Single-threaded Ray Performance:\n");
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time: %f seconds\n", metrics.medianTime);
	printf("Min Time: %f seconds\n", metrics.minTime);
	printf("Max Time: %f seconds\n", metrics.maxTime);
	printf("Variance: %f\n", metrics.variance);
	printf("99th Percentile Time: %f seconds\n", metrics.p99Time);
	printf("========================================\n");

	Camera camera2;
	initCamera(&camera2, 800, 600, 90.0f, (float3){0.0f, 0.0f, 0.0f}, (float3){0.0f, 0.0f, 1.0f}, (float3){1.0f, 1.0f, 1.0f});

	// Multi-threaded test

	ThreadPool *p = poolCreate(32, camera2.screenHeight);
	RenderTask tasks[1024];
	for (int i = 0; i < SAMPLES; i++) {
		// printf("Running multi-threaded sample %d/%d...\n", i + 1, SAMPLES);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int row = 0; row < camera2.screenHeight; row++) {
			tasks[row].objects = objects;
			tasks[row].objectCount = OBJECT_COUNT;
			tasks[row].camera = &camera2;
			tasks[row].row = row;
			poolAdd(p, RenderTaskFunction, &tasks[row]);
		}
		poolWait(p);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	poolDestroy(p);
	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Multi-threaded Ray Performance:\n");
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time: %f seconds\n", metrics.medianTime);
	printf("Min Time: %f seconds\n", metrics.minTime);
	printf("Max Time: %f seconds\n", metrics.maxTime);
	printf("Variance: %f\n", metrics.variance);
	printf("99th Percentile Time: %f seconds\n", metrics.p99Time);
	printf("========================================\n");

	// check correctness of a few pixels
	for (int i = 0; i < camera.screenWidth * camera.screenHeight; i++) {
		if (camera.framebuffer[i] != camera2.framebuffer[i]) {
			printf("Pixel %d mismatch: single-threaded color = 0x%08X, multi-threaded color = 0x%08X\n", i, camera.framebuffer[i], camera2.framebuffer[i]);
			exit(1);
		}
	}
}