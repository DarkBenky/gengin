#include <MiniFB.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "object/format.h"
#include "object/object.h"
#include "object/scene.h"
#include "render/render.h"
#include "render/cpu/font.h"
#include "render/cpu/ray.h"
#include "render/cpu/ssr.h"
#include "render/color/color.h"
#include "load/loadObj.h"
#include "math/vector3.h"
#include "skybox/skybox.h"
#include "util/threadPool.h"
#include "util/bench.h"

#ifdef USE_GPU
#include "render/gpu/raster.h"
#endif

#define WNOW(ts) clock_gettime(CLOCK_MONOTONIC, &(ts))
#define WDIFF(a, b) ((double)((b).tv_sec - (a).tv_sec) + (double)((b).tv_nsec - (a).tv_nsec) * 1e-9)

#define ACCUMULATE_STATS 1024
#define GRID_COLS 6
#define GRID_ROWS 7
#define PLANE_COUNT (GRID_COLS * GRID_ROWS)
#define OBJECT_COUNT (PLANE_COUNT + 2)

int main() {
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	Object *objects = malloc(sizeof(Object) * OBJECT_COUNT);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		destroyCamera(&camera);
		MaterialLib_Destroy(&matLib);
		return 1;
	}

	const float spacingX = 7.0f, spacingZ = 7.0f;
	const float startX = -(GRID_COLS - 1) * spacingX * 0.5f;
	const float startZ = 5.0f;
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			int idx = row * GRID_COLS + col;
			if (col % 2 == 0) {
				LoadObj("assets/models/r27.bin", &objects[idx], &matLib);
				objects[idx].rotation = (float3){0.0f, -0.5708f, 0.0f};
				objects[idx].scale = (float3){2.0f, 2.0f, 2.0f};
				objects[idx].position = (float3){startX + col * spacingX, -0.09f, startZ + row * spacingZ};
				CreateObjectBVH(&objects[idx], &objects[idx].bvh);
				Object_UpdateWorldBounds(&objects[idx]);
			} else {
				LoadObj("assets/models/f16.bin", &objects[idx], &matLib);
				objects[idx].rotation = (float3){0.0f, -0.5708f, 0.0f};
				objects[idx].scale = (float3){2.0f, 2.0f, 2.0f};
				objects[idx].position = (float3){startX + col * spacingX, -0.09f, startZ + row * spacingZ};
				CreateObjectBVH(&objects[idx], &objects[idx].bvh);
				Object_UpdateWorldBounds(&objects[idx]);
			}
		}
	}

	// floor
	LoadObj("assets/models/map.bin", &objects[PLANE_COUNT], &matLib);
	objects[PLANE_COUNT].rotation = (float3){0.0f, 0.0f, 0.0f};
	objects[PLANE_COUNT].scale = (float3){2.0f, 2.0f, 2.0f};
	objects[PLANE_COUNT].position = (float3){0.0f, -7.0f, 0.0f};
	CreateObjectBVH(&objects[PLANE_COUNT], &objects[PLANE_COUNT].bvh);
	Object_UpdateWorldBounds(&objects[PLANE_COUNT]);

	CreateCube(&objects[PLANE_COUNT + 1], (float3){3.0f, 2.0f, 10.0f}, (float3){3.0f, 4.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, (float3){0.8f, 0.2f, 0.2f, 0.1f}, &matLib);
	Object_UpdateWorldBounds(&objects[PLANE_COUNT + 1]);

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		Scene_Destroy(objects, OBJECT_COUNT);
		destroyCamera(&camera);
		return 1;
	}
	mfb_set_target_fps(0);

	struct Alphabet alphabet;
	LoadAlphabet(&alphabet, "assets/chars");

	Skybox skybox;
	LoadSkybox(&skybox, "skybox");

	ThreadPool *threadPool = poolCreate(32, HEIGHT);
	RayTraceTaskQueue rayTaskQueue;
	SSRTask *ssrTasks = malloc(sizeof(SSRTask) * ((HEIGHT + 3) / 4));

#ifdef USE_GPU
	GPURaster *gpuRaster = GPURaster_Init(objects, OBJECT_COUNT, &matLib, WIDTH, HEIGHT);
	if (!gpuRaster) {
		fprintf(stderr, "Warning: GPU rasterizer unavailable, falling back to CPU ray tracer.\n");
	}
#endif

	printf("Demo scene loaded. Total Tris: %d\n", Scene_CountTriangles(objects, OBJECT_COUNT));
	int frame = 0;
	int shadowResolution = 4;
	double frameTimes[4] = {0};
	double accumRenderTime = 0.0;
	double accumSetupTime = 0.0;
	double accumShadowTime = 0.0;
	double accumSSRTime = 0.0;
	double accumSyncTime = 0.0;
	double accumPresentTime = 0.0;
	int accumFrames = 0;

	struct timespec wSyncStart, wA, wB;
	WNOW(wSyncStart);

	Bench bench;
	benchInit(&bench);

	while (1) {
		benchFrameStart(&bench);
		WNOW(wA);
		accumSyncTime += WDIFF(wSyncStart, wA);

		frame++;
		clearBuffers(&camera);
		const float2 jitterPattern[4] = {
			{1.5f, 1.5f}, {-1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
		camera.jitter = jitterPattern[frame & 3];
		camera.seed = frame * (int)35527.0f << 16 | (int)11369.0f;
		// DemoScene_Update(objects, frame);

		// Rotating the camera around the scene
		float angle = frame * 0.001f;
		camera.position.x = sinf(angle) * 7.0f;
		camera.position.z = cosf(angle) * 7.0f + 10.0f;
		camera.lightDir = (float3){-sinf(angle + 1.57f) * 0.5f, 1.0f, -cosf(angle + 1.57f) * 0.5f};
		float3 toTarget = {-camera.position.x, -camera.position.y, 10.0f - camera.position.z};
		camera.forward = Float3_Normalize(toTarget);

		WNOW(wA);
		RenderSetup(objects, OBJECT_COUNT, &camera);
		WNOW(wB);
		double setupTime = WDIFF(wA, wB);
		accumSetupTime += setupTime;

		WNOW(wA);
#ifdef USE_GPU
		if (gpuRaster) {
			GPURaster_RenderObjects(gpuRaster, objects, OBJECT_COUNT, &camera);
		} else {
			RayTraceScene(objects, OBJECT_COUNT, &camera, &matLib, &rayTaskQueue, threadPool, &skybox);
		}
#else
		RayTraceScene(objects, OBJECT_COUNT, &camera, &matLib, &rayTaskQueue, threadPool, &skybox);

		// RASTERIZE
		// for (int i = 0; i < OBJECT_COUNT; i++) {
		// 	RenderObject(&objects[i], &camera, &matLib);
		// }
#endif
		WNOW(wB);
		double frameRenderTime = setupTime + WDIFF(wA, wB);
		frameTimes[frame & 3] = frameRenderTime;
		accumRenderTime += frameRenderTime;

		WNOW(wA);
		// ShadowPostProcess(objects, OBJECT_COUNT, &camera, shadowResolution, 64);
		// DitherPostProcess(&camera, frame);
		// DitherOrderedPostProcess(&camera, frame);
		WNOW(wB);
		accumShadowTime += WDIFF(wA, wB);

		WNOW(wA);
		SSRPostProcess(&camera, threadPool, ssrTasks, 4);
		WNOW(wB);
		accumSSRTime += WDIFF(wA, wB);

		benchCaptureFrame(&bench, camera.framebuffer, WIDTH * HEIGHT);

#ifndef BENCH_MODE
		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		char text[64];
		double avgFrameTime = (frameTimes[0] + frameTimes[1] + frameTimes[2] + frameTimes[3]) * 0.25;
		snprintf(text, sizeof(text), "FPS: %.1f", avgFrameTime > 0.0 ? 1.0 / avgFrameTime : 0.0);
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, text, 20, 20, 1.75f, c);
#endif

		WNOW(wA);
		if (mfb_update(window, camera.framebuffer) != STATE_OK) break;
		WNOW(wB);
		accumPresentTime += WDIFF(wA, wB);

		accumFrames++;

		if ((frame % ACCUMULATE_STATS) == 0) {
			double avgRender = accumRenderTime / accumFrames * 1000.0;
			double avgSetup = accumSetupTime / accumFrames * 1000.0;
			double avgRasterize = avgRender - avgSetup;
			double avgShadow = accumShadowTime / accumFrames * 1000.0;
			double avgSSR = accumSSRTime / accumFrames * 1000.0;
			double avgSync = accumSyncTime / accumFrames * 1000.0;
			double avgPresent = accumPresentTime / accumFrames * 1000.0;
			double avgTotal = avgRender + avgShadow + avgSSR + avgSync + avgPresent;
			double avgFps = 1000.0 / avgTotal;
			double targetFrameTime = 1.0 / 60.0;
			int maxTriangles60 = (int)(Scene_CountTriangles(objects, OBJECT_COUNT) * ((targetFrameTime * 1000.0) / avgRasterize));
			printf("Frame %d  setup: %.2f ms  raster: %.2f ms  shadow: %.2f ms  ssr: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, avgSetup, avgRasterize, avgShadow, avgSSR, avgSync, avgPresent, avgTotal, avgFps, maxTriangles60, shadowResolution);
			accumRenderTime = accumSetupTime = accumShadowTime = accumSSRTime = accumSyncTime = accumPresentTime = 0.0;
			accumFrames = 0;
		}

		WNOW(wSyncStart);
		if (benchFrameEnd(&bench)) break;
#ifdef PGO_MAX_FRAMES
		if (frame >= PGO_MAX_FRAMES) break;
#endif
	}

	mfb_close(window);
	benchReport(&bench);
	benchFree(&bench);

#ifdef USE_GPU
	GPURaster_Destroy(gpuRaster);
#endif

	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	Scene_Destroy(objects, OBJECT_COUNT);
	DestroySkybox(&skybox);
	poolDestroy(threadPool);
	free(ssrTasks);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
