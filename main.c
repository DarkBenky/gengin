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
#define WIDTH 800
#define HEIGHT 600
#define ACCUMULATE_STATS 256

int main() {
	// TestFunctions();

	const int objectCount = DemoScene_ObjectCount();
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 30.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	Object *objects = malloc(sizeof(Object) * objectCount);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		destroyCamera(&camera);
		MaterialLib_Destroy(&matLib);
		return 1;
	}

	DemoScene_Build(objects, &matLib);

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		Scene_Destroy(objects, objectCount);
		destroyCamera(&camera);
		return 1;
	}

	struct Alphabet alphabet;
	LoadAlphabet(&alphabet, "assets/chars");

	printf("Demo scene loaded. Total Tris: %d\n", Scene_CountTriangles(objects, objectCount));
	printf("Press 1-5 to change shadow resolution (1=highest, 5=lowest)\n");
	mfb_set_target_fps(0);

	int frame = 0;
	int shadowResolution = 4;
	double accumRenderTime = 0.0;
	double accumSetupTime = 0.0;
	double accumShadowTime = 0.0;
	double accumSyncTime = 0.0;
	double accumPresentTime = 0.0;
	int accumFrames = 0;

	clock_t syncStart = clock();
	while (mfb_wait_sync(window)) {
		double syncTime = (double)(clock() - syncStart) / CLOCKS_PER_SEC;
		accumSyncTime += syncTime;

		frame++;
		clearBuffers(&camera);
		const float2 jitterPattern[4] = {
			{0.5f, 0.5f}, {-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
		camera.jitter = jitterPattern[frame & 3];
		clock_t start = clock();
		// DemoScene_Update(objects, frame);

		RenderSetup(objects, objectCount, &camera);
		double setupTime = (double)(clock() - start) / CLOCKS_PER_SEC;

		clock_t rasterStart = clock();
		for (int i = 0; i < objectCount; i++)
			RenderObject(&objects[i], &camera, &matLib);
		accumRenderTime += setupTime + (double)(clock() - rasterStart) / CLOCKS_PER_SEC;
		accumSetupTime += setupTime;

		clock_t shadowStart = clock();
		ShadowPostProcess(objects, objectCount, &camera, shadowResolution, 64);
		accumShadowTime += (double)(clock() - shadowStart) / CLOCKS_PER_SEC;
		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, "HELLO FROM FONT LOADER 012345", 20, 20, 1.5f, c);

		clock_t presentStart = clock();
		mfb_update(window, camera.framebuffer);
		accumPresentTime += (double)(clock() - presentStart) / CLOCKS_PER_SEC;

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
			int maxTriangles60 = (int)(Scene_CountTriangles(objects, objectCount) * ((targetFrameTime * 1000.0) / avgRasterize));
			printf("Frame %d  setup: %.2f ms  raster: %.2f ms  shadow: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, avgSetup, avgRasterize, avgShadow, avgSync, avgPresent, avgTotal, avgFps, maxTriangles60, shadowResolution);
			accumRenderTime = accumSetupTime = accumShadowTime = accumSyncTime = accumPresentTime = 0.0;
			accumFrames = 0;
		}

		syncStart = clock();
	}

	mfb_close(window);
	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	Scene_Destroy(objects, objectCount);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
