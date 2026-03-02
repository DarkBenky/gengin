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
#include "render/color/color.h"
#include "load/loadObj.h"
#include "math/vector3.h"
#include "skybox/skybox.h"
#include "util/threadPool.h"
#define ACCUMULATE_STATS 256
#define OBJECT_COUNT 3

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

	LoadObj("assets/models/r27.bin", &objects[0], &matLib);
	objects[0].rotation = (float3){0.0f, -0.5708f, 0.0f};
	objects[0].scale = (float3){2.0f, 2.0f, 2.0f};
	objects[0].position = (float3){0.0f, -0.09f, 10.0f - 1.33f};
	CreateObjectBVH(&objects[0], &objects[0].bvh);
	Object_UpdateWorldBounds(&objects[0]);

	CreateCube(&objects[1], (float3){3.0f, 2.0f, 10.0f}, (float3){3.0f, 4.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, (float3){0.8f, 0.2f, 0.2f, 0.1f}, &matLib);
	Object_UpdateWorldBounds(&objects[1]);

	// floor
	CreateCube(&objects[2], (float3){0.0f, -1.25f, 15.0f}, (float3){0.0f, -1.0f, 0.0f}, (float3){40.0f, 0.2f, 40.0f}, (float3){0.32f, 0.34f, 0.38f, 0.0f}, &matLib);
	Object_UpdateWorldBounds(&objects[2]);

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

	ThreadPool *threadPool = poolCreate(16, HEIGHT);
	SkyBoxTaskQueue skyTaskQueue;
	RayTraceTaskQueue rayTaskQueue;

	printf("Demo scene loaded. Total Tris: %d\n", Scene_CountTriangles(objects, OBJECT_COUNT));
	int frame = 0;
	int shadowResolution = 4;
	double accumRenderTime = 0.0;
	double accumSetupTime = 0.0;
	double accumShadowTime = 0.0;
	double accumSyncTime = 0.0;
	double accumPresentTime = 0.0;
	int accumFrames = 0;

#define WNOW(ts) clock_gettime(CLOCK_MONOTONIC, &(ts))
#define WDIFF(a, b) ((double)((b).tv_sec - (a).tv_sec) + (double)((b).tv_nsec - (a).tv_nsec) * 1e-9)

	struct timespec wSyncStart, wA, wB;
	WNOW(wSyncStart);
	while (1) {
		WNOW(wA);
		accumSyncTime += WDIFF(wSyncStart, wA);

		frame++;
		clearBuffers(&camera);
		const float2 jitterPattern[4] = {
			{0.5f, 0.5f}, {-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
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

		// RASTERIZE
		// for (int i = 0; i < OBJECT_COUNT; i++)
		// 	RenderObject(&objects[i], &camera, &matLib);

		WNOW(wA);
		RayTraceScene(objects, OBJECT_COUNT, &camera, &matLib, &rayTaskQueue, threadPool);
		WNOW(wB);
		accumRenderTime += setupTime + WDIFF(wA, wB);

		WNOW(wA);
		// ShadowPostProcess(objects, OBJECT_COUNT, &camera, shadowResolution, 64);
		WNOW(wB);
		accumShadowTime += WDIFF(wA, wB);

		applySkybox(&skybox, &camera, threadPool, &skyTaskQueue);

		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, "HELLO FROM FONT LOADER 012345", 20, 20, 1.5f, c);

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
			double avgSync = accumSyncTime / accumFrames * 1000.0;
			double avgPresent = accumPresentTime / accumFrames * 1000.0;
			double avgTotal = avgRender + avgShadow + avgSync + avgPresent;
			double avgFps = 1000.0 / avgTotal;
			double targetFrameTime = 1.0 / 60.0;
			int maxTriangles60 = (int)(Scene_CountTriangles(objects, OBJECT_COUNT) * ((targetFrameTime * 1000.0) / avgRasterize));
			printf("Frame %d  setup: %.2f ms  raster: %.2f ms  shadow: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, avgSetup, avgRasterize, avgShadow, avgSync, avgPresent, avgTotal, avgFps, maxTriangles60, shadowResolution);
			accumRenderTime = accumSetupTime = accumShadowTime = accumSyncTime = accumPresentTime = 0.0;
			accumFrames = 0;
		}

		WNOW(wSyncStart);
#ifdef PGO_MAX_FRAMES
		if (frame >= PGO_MAX_FRAMES) break;
#endif
	}

	mfb_close(window);
	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	Scene_Destroy(objects, OBJECT_COUNT);
	DestroySkybox(&skybox);
	poolDestroy(threadPool);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
