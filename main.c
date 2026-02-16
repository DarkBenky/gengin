#include <MiniFB.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "object/format.h"
#include "object/object.h"
#include "object/scene.h"
#include "render/render.h"
#include "render/cpu/ray.h"
#define WIDTH 800
#define HEIGHT 600

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
	int totalTriangles = Scene_CountTriangles(objects, objectCount);

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		Scene_Destroy(objects, objectCount);
		destroyCamera(&camera);
		return 1;
	}

	printf("Demo scene loaded. Total Tris: %d\n", totalTriangles);
	printf("Press 1-5 to change shadow resolution (1=highest, 5=lowest)\n");

	int frame = 0;
	int shadowResolution = 3;
	while (mfb_wait_sync(window)) {
		frame++;
		clearBuffers(&camera);
		clock_t start = clock();
		DemoScene_Update(objects, frame);

		RenderObjects(objects, objectCount, &camera);

		ShadowPostProcess(objects, objectCount, &camera, shadowResolution);
		clock_t end = clock();
		double renderTime = (double)(end - start) / CLOCKS_PER_SEC;
		double fps = 1.0 / renderTime;
		double targetFrameTime = 1.0 / 60.0;
		int maxTriangles60 = (int)(totalTriangles * (targetFrameTime / renderTime));
		if ((frame % 30) == 0) {
			printf("Frame %d  Render: %.3f ms  FPS: %.2f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, renderTime * 1000.0, fps, maxTriangles60, shadowResolution);
		}

		mfb_update(window, camera.framebuffer);
	}

	mfb_close(window);
	Scene_Destroy(objects, objectCount);
	destroyCamera(&camera);
	return 0;
}
