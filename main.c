#include <MiniFB.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "object/format.h"
#include "object/object.h"
#include "render/render.h"
#ifdef USE_GPU
#include "render/renderGpu.h"
#endif
#define WIDTH 800
#define HEIGHT 600
#define GRID_X 20
#define GRID_Y 10
#define GRID_Z 3

int main() {
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 90.0f, (float3){0, 0, 0}, (float3){0, 0, 1});

	const int objectCount = GRID_X * GRID_Y * GRID_Z;

	Object *objects = malloc(sizeof(Object) * objectCount);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		return 1;
	}

	float spacing = 1.5f;
	float startX = -((GRID_X - 1) * spacing) * 0.5f;
	float startY = -((GRID_Y - 1) * spacing) * 0.5f;
	float startZ = 12.0f;

	int idx = 0;
	for (int z = 0; z < GRID_Z; z++) {
		for (int y = 0; y < GRID_Y; y++) {
			for (int x = 0; x < GRID_X; x++) {
				float3 pos = {
					startX + x * spacing,
					startY + y * spacing,
					startZ + z * 3.0f};
				float3 color = {
					(float)x / (float)(GRID_X - 1),
					(float)y / (float)(GRID_Y - 1),
					(float)z / (float)(GRID_Z - 1)};
				CreateCube(&objects[idx], pos, (float3){0, 0, 0}, (float3){0.9f, 0.9f, 0.9f}, color);
				idx++;
			}
		}
	}

	int totalTriangles = 0;
	for (int i = 0; i < objectCount; i++) {
		totalTriangles += objects[i].triangleCount;
	}

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		free(camera.framebuffer);
		free(camera.normalBuffer);
		free(camera.depthBuffer);
		for (int i = 0; i < objectCount; i++) {
			Object_Destroy(&objects[i]);
		}
		free(objects);
		return 1;
	}

	printf("Single-thread  Total Cubes: %d  Total Tris: %d\n", objectCount, totalTriangles);

#ifdef USE_GPU
	RenderGpu gpu;
	bool gpuOk = RenderGpu_InitBuffer(&gpu, WIDTH, HEIGHT, camera.framebuffer);
	if (!gpuOk) {
		fprintf(stderr, "GPU mode init failed, falling back to CPU\n");
	} else {
		printf("GPU scene raster path active\n");
	}
#endif

	int frame = 0;
	while (mfb_wait_sync(window)) {
		frame++;
		clearBuffers(&camera);
		clock_t start = clock();

		// rotate objects
		for (int i = 0; i < objectCount; i++) {
			objects[i].rotation.y += 0.01f;
			objects[i].rotation.x += 0.004f;
		}

#ifdef USE_GPU
		if (gpuOk) {
			if (!RenderGpu_RenderSceneBuffer(&gpu, objects, objectCount, &camera)) {
				for (int i = 0; i < objectCount; i++) {
					RenderObject(&objects[i], &camera);
				}
			}
		} else {
			for (int i = 0; i < objectCount; i++) {
				RenderObject(&objects[i], &camera);
			}
		}
#else
		for (int i = 0; i < objectCount; i++) {
			RenderObject(&objects[i], &camera);
		}
#endif
		clock_t end = clock();
		double renderTime = (double)(end - start) / CLOCKS_PER_SEC;
		double fps = 1.0 / renderTime;
		double targetFrameTime = 1.0 / 60.0;
		int maxTriangles60 = (int)(totalTriangles * (targetFrameTime / renderTime));
		if ((frame % 30) == 0) {
			printf("Frame %d  Render: %.3f ms  FPS: %.2f  Est Tris@60: %d\n",
				   frame, renderTime * 1000.0, fps, maxTriangles60);
		}

		mfb_update(window, camera.framebuffer);
	}
#ifdef USE_GPU
	if (gpuOk) {
		RenderGpu_Shutdown(&gpu);
	}
#endif

	mfb_close(window);

	free(camera.framebuffer);
	free(camera.normalBuffer);
	free(camera.depthBuffer);
	for (int i = 0; i < objectCount; i++) {
		Object_Destroy(&objects[i]);
	}
	free(objects);
	return 0;
}
