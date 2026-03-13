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
#include "keyboar/keyboar.h"

#define WNOW(ts) clock_gettime(CLOCK_MONOTONIC, &(ts))
#define WDIFF(a, b) ((double)((b).tv_sec - (a).tv_sec) + (double)((b).tv_nsec - (a).tv_nsec) * 1e-9)

#define ACCUMULATE_STATS 1024
#define GRID_COLS 32
#define GRID_ROWS 32

int main() {
	Input input;
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	ObjectList scene;
	ObjectList_Init(&scene, 1);

	// build checkerboard grid into a temporary list, then merge into one object
	ObjectList grid;
	ObjectList_Init(&grid, GRID_COLS * GRID_ROWS);
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			Object *obj = ObjectList_Add(&grid);
			CreateCube(obj, (float3){(col - GRID_COLS / 2) * 7.0f, -0.09f, 5.0f + row * 7.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){7.0f, 0.1f, 7.0f}, (float3){(col % 2) * 0.4f + 0.1f, (row % 2) * 0.4f + 0.1f, ((col + row) % 2) * 0.4f + 0.1f}, &matLib);
			Object_UpdateWorldBounds(obj);
		}
	}
	ObjectList_Merge(&grid, &scene);
	free(grid.objects);

	Object *plane = ObjectList_Add(&scene);
	LoadObj("assets/models/f16.bin", plane, &matLib);
	plane->position = (float3){0.0f, 10.0f, 20.0f};
	plane->rotation = (float3){0.0f, 0.0f, 0.0f};
	plane->scale = (float3){2.0f, 2.0f, 2.0f};
	CreateObjectBVH(plane, &plane->bvh);
	Object_UpdateWorldBounds(plane);

	// Precompute per-face emission for all scene objects
	for (int i = 0; i < scene.count; i++)
		Object_PrecomputeEmission(&scene.objects[i], &matLib);

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		ObjectList_Destroy(&scene);
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

	printf("Demo scene loaded. Total Tris: %d\n", ObjectList_CountTriangles(&scene));
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

		Input_Poll(&input, window);
		if (input.keysDown[KB_KEY_ESCAPE]) break;
		if (input.keys[KB_KEY_W]) CameraMoveForward(&camera, 0.2f);
		if (input.keys[KB_KEY_S]) CameraMoveForward(&camera, -0.2f);
		if (input.keys[KB_KEY_A]) CameraMoveRight(&camera, -0.2f);
		if (input.keys[KB_KEY_D]) CameraMoveRight(&camera, 0.2f);
		if (input.keys[KB_KEY_Q]) CameraMoveUp(&camera, 0.2f);
		if (input.keys[KB_KEY_E]) CameraMoveUp(&camera, -0.2f);
		if (input.mouse[MOUSE_LEFT]) CameraRotate(&camera, input.mouseDY * 0.005f, -input.mouseDX * 0.005f);

		// Keep plane in front of camera, facing the same direction, offset slightly below view center
		float3 fwd = Float3_Normalize(camera.forward);
		plane->position = Float3_Add(
			Float3_Add(camera.position, Float3_Scale(fwd, 10.0f)),
			Float3_Scale(Float3_Normalize(camera.up), -2.5f));
		plane->rotation = (float3){asinf(-fwd.y), atan2f(fwd.x, fwd.z), 0.0f};
		Object_UpdateWorldBounds(plane);

		WNOW(wA);
		RenderSetup(scene.objects, scene.count, &camera);
		WNOW(wB);
		double setupTime = WDIFF(wA, wB);
		accumSetupTime += setupTime;

		WNOW(wA);
		RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, threadPool, &skybox);

		// RASTERIZE
		// for (int i = 0; i < OBJECT_COUNT; i++) {
		// 	RenderObject(&objects[i], &camera, &matLib);
		// }
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
		// SSRPostProcess(&camera, threadPool, ssrTasks, 4);
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
			int maxTriangles60 = (int)(ObjectList_CountTriangles(&scene) * ((targetFrameTime * 1000.0) / avgRasterize));
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

	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	ObjectList_Destroy(&scene);
	DestroySkybox(&skybox);
	poolDestroy(threadPool);
	free(ssrTasks);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
