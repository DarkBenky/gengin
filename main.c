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
#ifdef USE_GPU_RASTER
#include "render/gpu/raster.h"
#endif
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

#ifdef USE_GPU_RASTER
	GpuRaster gpuRaster;
	GpuRaster_Init(&gpuRaster, WIDTH, HEIGHT, "render/gpu/kernels");
	int useGpu = gpuRaster.gpuOk;
	if (useGpu)
		GpuRaster_UploadScene(&gpuRaster, objects, objectCount, &matLib);
	else
		printf("[main] GPU rasterizer unavailable, falling back to CPU\n");

	mfb_set_target_fps(useGpu ? 60 : 0);
#else
	int useGpu = 0;
	mfb_set_target_fps(0);
#endif

	printf("Demo scene loaded. Total Tris: %d\n", Scene_CountTriangles(objects, objectCount));
	printf("Press 1-5 to change shadow resolution (1=highest, 5=lowest)\n");

	int frame = 0;
	int shadowResolution = 4;
	double accumRenderTime = 0.0;
	double accumSetupTime = 0.0;
	double accumShadowTime = 0.0;
	double accumSyncTime = 0.0;
	double accumPresentTime = 0.0;
	double accumGpuRasterMs = 0.0;
	double accumGpuResolveMs = 0.0;
	double accumGpuReadbackMs = 0.0;
	double accumGpuReadBufMs = 0.0;
	double accumGpuShadowMs = 0.0;
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

		if (useGpu) {
#ifdef USE_GPU_RASTER
			GpuRaster_Clear(&gpuRaster);
			GpuRaster_RenderAll(&gpuRaster, &camera, objects, objectCount);
			GpuRaster_Resolve(&gpuRaster);
			clock_t rbStart = clock();
			GpuRaster_ReadAlbedo(&gpuRaster, camera.framebuffer);
			accumGpuReadbackMs += (double)(clock() - rbStart) / CLOCKS_PER_SEC * 1000.0;
			accumGpuRasterMs += gpuRaster.lastRasterMs;
			accumGpuResolveMs += gpuRaster.lastResolveMs;

			clock_t rbufStart = clock();
			GpuRaster_ReadBuffers(&gpuRaster, &camera);
			accumGpuReadBufMs += (double)(clock() - rbufStart) / CLOCKS_PER_SEC * 1000.0;
#endif
		} else {
			for (int i = 0; i < objectCount; i++)
				RenderObject(&objects[i], &camera, &matLib);
		}

		accumRenderTime += setupTime + (double)(clock() - rasterStart) / CLOCKS_PER_SEC;
		accumSetupTime += setupTime;

		clock_t shadowStart = clock();
		ShadowPostProcess(objects, objectCount, &camera, shadowResolution, 64);
		double shadowSec = (double)(clock() - shadowStart) / CLOCKS_PER_SEC;
		accumShadowTime += shadowSec;
#ifdef USE_GPU_RASTER
		if (useGpu) accumGpuShadowMs += shadowSec * 1000.0;
#endif
		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, "HELLO FROM FONT LOADER 012345", 20, 20, 1.5f, c);

		clock_t presentStart = clock();
		mfb_update(window, camera.framebuffer);
		accumPresentTime += (double)(clock() - presentStart) / CLOCKS_PER_SEC;

		accumFrames++;

		if ((frame % ACCUMULATE_STATS) == 0) {
			double avgRender = accumRenderTime / accumFrames * 1000.0;
			double avgSetup = accumSetupTime / accumFrames * 1000.0;
			double avgSync = accumSyncTime / accumFrames * 1000.0;
			double avgPresent = accumPresentTime / accumFrames * 1000.0;
			if (useGpu) {
				double avgKernelRaster = accumGpuRasterMs / accumFrames;
				double avgKernelResolve = accumGpuResolveMs / accumFrames;
				double avgReadback = accumGpuReadbackMs / accumFrames;
				double avgRasterize = avgRender - avgSetup;
				double avgTotal = avgRender + avgSync + avgPresent;
				double avgFps = 1000.0 / avgTotal;
				int maxTris60 = (int)(Scene_CountTriangles(objects, objectCount) * ((1000.0 / 60.0) / avgRasterize));
				printf("Frame %d  setup: %.2f ms  raster: %.2f ms  kernel-raster: %.2f ms  kernel-resolve: %.2f ms  readback: %.2f ms  readbuf: %.2f ms  shadow: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d\n",
					   frame, avgSetup, avgRasterize, avgKernelRaster, avgKernelResolve, avgReadback, accumGpuReadBufMs / accumFrames, accumGpuShadowMs / accumFrames, avgSync, avgPresent, avgTotal, avgFps, maxTris60);
			} else {
				double avgRasterize = avgRender - avgSetup;
				double avgShadow = accumShadowTime / accumFrames * 1000.0;
				double avgTotal = avgRender + avgShadow + avgSync + avgPresent;
				double avgFps = 1000.0 / avgTotal;
				int maxTris60 = (int)(Scene_CountTriangles(objects, objectCount) * ((1000.0 / 60.0) / avgRasterize));
				printf("Frame %d  setup: %.2f ms  raster: %.2f ms  shadow: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
					   frame, avgSetup, avgRasterize, avgShadow, avgSync, avgPresent, avgTotal, avgFps, maxTris60, shadowResolution);
			}
			accumRenderTime = accumSetupTime = accumShadowTime = accumSyncTime = accumPresentTime = 0.0;
			accumGpuRasterMs = accumGpuResolveMs = accumGpuReadbackMs = accumGpuReadBufMs = accumGpuShadowMs = 0.0;
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
#ifdef USE_GPU_RASTER
	if (useGpu) GpuRaster_Destroy(&gpuRaster);
#endif
	Scene_Destroy(objects, objectCount);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
