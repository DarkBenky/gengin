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
	const int objectCount = DemoScene_ObjectCount();
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 30.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){6.0f, 8.0f, -6.0f});

	Object *objects = malloc(sizeof(Object) * objectCount);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		destroyCamera(&camera);
		return 1;
	}

	DemoScene_Build(objects);

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
			{1.5f, 1.5f}, {-1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
		camera.jitter = jitterPattern[frame & 3];
		clock_t start = clock();
		// DemoScene_Update(objects, frame);

		RenderObjects(objects, objectCount, &camera);
		ShadowPostProcess(objects, objectCount, &camera, shadowResolution, 4);

		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, "HELLO FROM FONT LOADER 012345", 20, 20, 1.5f, c);
		accumRenderTime += (double)(clock() - start) / CLOCKS_PER_SEC;

		clock_t presentStart = clock();
		mfb_update(window, camera.framebuffer);
		accumPresentTime += (double)(clock() - presentStart) / CLOCKS_PER_SEC;

		accumFrames++;

		if ((frame % ACCUMULATE_STATS) == 0) {
			double avgRender = accumRenderTime / accumFrames * 1000.0;
			double avgSync = accumSyncTime / accumFrames * 1000.0;
			double avgPresent = accumPresentTime / accumFrames * 1000.0;
			double avgTotal = avgRender + avgSync + avgPresent;
			double avgFps = 1000.0 / avgTotal;
			double targetFrameTime = 1.0 / 60.0;
			int maxTriangles60 = (int)(Scene_CountTriangles(objects, objectCount) * ((targetFrameTime * 1000.0) / avgRender));
			printf("Frame %d  render: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, avgRender, avgSync, avgPresent, avgTotal, avgFps, maxTriangles60, shadowResolution);
			accumRenderTime = accumSyncTime = accumPresentTime = 0.0;
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
	destroyCamera(&camera);
	return 0;
}
