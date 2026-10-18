#include "timings.h"
#include "saveImage.h"
#include "../render/color/color.h"
#include "../ui/editor/editor.h"
#include "../object/material/material.h"
#include "../render/render.h"
#include "../render/cpu/ray.h"
#include "../skybox/skybox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>

#define SAMPLES 100
#define WIDTH   1280
#define HEIGHT  720

// Real rendering through the production path: a floor, three cubes (the middle
// one is the pick target) and two spheres, drawn by RayTraceScene so the
// highlight and cursor can be eyeballed on shaded geometry instead of flat quads.
static void BuildScene(ObjectList *scene, MaterialLib *matLib) {
	Object *floor = ObjectList_Add(scene);
	CreateCube(floor, (float3){0.0f, -0.6f, 11.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){44.0f, 0.2f, 44.0f},
	           (float3){0.75f, 0.75f, 0.72f}, matLib, 0.0f, 0.9f, 0.0f);

	Object *cubeL = ObjectList_Add(scene);
	CreateCube(cubeL, (float3){-10.0f, 2.5f, 12.0f}, (float3){0.0f, 0.5f, 0.0f}, (float3){6.0f, 6.0f, 6.0f},
	           (float3){0.80f, 0.25f, 0.25f}, matLib, 0.0f, 0.6f, 0.0f);

	Object *cubeM = ObjectList_Add(scene);
	CreateCube(cubeM, (float3){0.0f, 2.5f, 11.0f}, (float3){0.0f, 0.3f, 0.0f}, (float3){6.0f, 6.0f, 6.0f},
	           (float3){0.25f, 0.70f, 0.35f}, matLib, 0.0f, 0.35f, 0.0f);

	Object *cubeR = ObjectList_Add(scene);
	CreateCube(cubeR, (float3){10.0f, 2.5f, 12.0f}, (float3){0.0f, -0.5f, 0.0f}, (float3){6.0f, 6.0f, 6.0f},
	           (float3){0.30f, 0.40f, 0.85f}, matLib, 0.0f, 0.45f, 0.0f);

	Object *sphereL = ObjectList_Add(scene);
	CreateSphere(sphereL, (float3){-7.0f, 1.5f, 5.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f},
	             (float3){0.85f, 0.65f, 0.15f}, matLib, 0.0f, 0.25f, 0.8f);

	Object *sphereR = ObjectList_Add(scene);
	CreateSphere(sphereR, (float3){7.0f, 1.5f, 5.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f},
	             (float3){0.20f, 0.70f, 0.80f}, matLib, 0.0f, 0.15f, 0.4f);
}

static float3 TintColor(float3 color) {
	color.x *= highlightColor.x;
	color.y *= highlightColor.y;
	color.z *= highlightColor.z;
	return color;
}

static int PixelClose(float3 a, float3 b, float tolerance) {
	return fabsf(a.x - b.x) <= tolerance &&
	       fabsf(a.y - b.y) <= tolerance &&
	       fabsf(a.z - b.z) <= tolerance;
}

typedef void (*highlightFn)(editorUi *ui, ThreadPool *pool, int px, int py);

// One fresh frame + one highlight apply, compared against a pre-apply reference:
// only target and cursor pixels may change, everything else stays bit-identical.
// outline=1 checks the edge variant: silhouette pixels white, interior tinted.
static int IsEdgePixel(const int *objectIdBuffer, int x, int y, int width, int height, int targetId) {
	if (x == 0 || objectIdBuffer[y * width + x - 1] != targetId) return 1;
	if (x == width - 1 || objectIdBuffer[y * width + x + 1] != targetId) return 1;
	if (y == 0 || objectIdBuffer[(y - 1) * width + x] != targetId) return 1;
	if (y == height - 1 || objectIdBuffer[(y + 1) * width + x] != targetId) return 1;
	return 0;
}

static int CheckHighlight(editorUi *ui, ThreadPool *pool, Camera *camera, ObjectList *scene, MaterialLib *matLib,
                          RayTraceTaskQueue *rayTaskQueue, const Skybox *skybox, int px, int py, int targetId, highlightFn apply, int outline) {
	RenderSetup(scene->objects, scene->count, camera);
	RayTraceScene(scene->objects, scene->count, camera, matLib, rayTaskQueue, pool, skybox);

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	float3 *reference = malloc(sizeof(float3) * width * height);
	memcpy(reference, camera->framebufferHdr, sizeof(float3) * width * height);

	apply(ui, pool, px, py);

	int mismatches = 0;
	int targetPixels = 0;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const int idx = y * width + x;
			const int objectId = camera->objectIdBuffer[idx];
			int onCursor = (y == py && abs(x - px) <= CURSOR_ARM) ||
			               (x == px && abs(y - py) <= CURSOR_ARM);
			float3 expected;
			float tolerance = 0.0f;
			if (onCursor) {
				expected = (float3){1.0f, 1.0f, 1.0f};
			} else if (objectId == targetId) {
				targetPixels++;
				if (outline && IsEdgePixel(camera->objectIdBuffer, x, y, width, height, targetId)) {
					expected = (float3){1.0f, 1.0f, 1.0f};
				} else {
					expected = TintColor(reference[idx]);
					tolerance = 1e-4f; // fast-math may reassociate the channel multiplies between call sites
				}
			} else {
				expected = reference[idx];
			}
			if (!PixelClose(camera->framebufferHdr[idx], expected, tolerance)) {
				if (mismatches < 5) {
					float3 got = camera->framebufferHdr[idx];
					printf("MISMATCH at %d: (%.6f, %.6f, %.6f) vs (%.6f, %.6f, %.6f)\n", idx,
					       got.x, got.y, got.z, expected.x, expected.y, expected.z);
				}
				mismatches++;
			}
		}
	}
	free(reference);

	if (targetPixels < 1000) mismatches++; // the picked cube should cover a sizeable part of the frame

	return mismatches;
}

int main(void) {
	Camera camera;
	memset(&camera, 0, sizeof(camera)); // deterministic seed for the ray tracer
	initCamera(&camera, WIDTH, HEIGHT, 60.0f,
	           (float3){0.0f, 2.5f, -7.0f},
	           (float3){0.0f, -0.2f, 1.0f},
	           (float3){0.3f, 0.8f, -0.55f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	ObjectList scene;
	ObjectList_Init(&scene, 8);
	BuildScene(&scene, &matLib);

	Skybox skybox;
	LoadSkybox(&skybox, "skybox");

	struct Alphabet alphabet;
	LoadAlphabet(&alphabet, "assets/chars");

	editorUi ui;
	Input input = {0};
	createEditorUi(&scene, &ui, &camera, &input, &alphabet);

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	ThreadPool *pool = poolCreate(ncpu > 1 ? (int)ncpu - 1 : 1, WIDTH);

	RayTraceTaskQueue rayTaskQueue;
	RenderSetup(scene.objects, scene.count, &camera);
	RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, pool, &skybox);

	// pick the middle cube through the real picker path, at its pixel closest to the image centre
	const int targetId = 2;
	int px = -1, py = -1;
	long bestDistSq = -1;
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (camera.objectIdBuffer[y * WIDTH + x] != targetId) continue;
			long dx = x - WIDTH / 2, dy = y - HEIGHT / 2;
			long distSq = dx * dx + dy * dy;
			if (bestDistSq < 0 || distSq < bestDistSq) {
				bestDistSq = distSq;
				px = x;
				py = y;
			}
		}
	}
	if (px < 0) {
		printf("FAIL: object %d not visible in the rendered frame\n", targetId);
		return 1;
	}
	selectObject(&ui, px, py);
	if (ui.selectedObject != &scene.objects[targetId]) {
		printf("FAIL: selectObject picked the wrong object\n");
		return 1;
	}
	printf("Picked object %d at pixel (%d, %d)\n", targetId, px, py);

	float timesV1[SAMPLES];
	float timesV2[SAMPLES];
	float timesEdge[SAMPLES];

	applyHighlight(&ui, pool, px, py);

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		applyHighlight(&ui, pool, px, py);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV1[s] = (float)(t1.tv_sec - t0.tv_sec)
		           + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		applyHighlightV2(&ui, pool, px, py);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2[s] = (float)(t1.tv_sec - t0.tv_sec)
		           + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		applyHighlightEdge(&ui, pool, px, py);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesEdge[s] = (float)(t1.tv_sec - t0.tv_sec)
		             + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics mV1 = ComputePerformanceMetrics(timesV1, SAMPLES);
	PerformanceMetrics mV2 = ComputePerformanceMetrics(timesV2, SAMPLES);
	PerformanceMetrics mEdge = ComputePerformanceMetrics(timesEdge, SAMPLES);

	printf("=== EditorHighlightBench: highlight over %dx%d, %d samples ===\n",
	       WIDTH, HEIGHT, SAMPLES);
	printf("%-20s %8s  %10s  %9s  %7s\n", "Benchmark", "avg(ms)", "median(ms)", "p99(ms)", "speedup");
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "applyHighlight", mV1.averageTime * 1e3f, mV1.medianTime * 1e3f, mV1.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "applyHighlightV2", mV2.averageTime * 1e3f, mV2.medianTime * 1e3f, mV2.p99Time * 1e3f,
	       mV1.medianTime / mV2.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "applyHighlightEdge", mEdge.averageTime * 1e3f, mEdge.medianTime * 1e3f, mEdge.p99Time * 1e3f,
	       mV1.medianTime / mEdge.medianTime);

	// Correctness: each variant gets its own fresh frame and pre-apply reference
	// (the timed loops compound the tint, so those pixels are not a valid reference).
	int mismatchesV1 = CheckHighlight(&ui, pool, &camera, &scene, &matLib, &rayTaskQueue, &skybox, px, py, targetId, applyHighlight, 0);
	ConvertToLdr(camera.framebufferHdr, camera.framebufferLdr, WIDTH, HEIGHT);
	SaveImage("tests/img/editor_highlight.bmp", &camera);
	printf("Image dumped to tests/img/editor_highlight.bmp\n");

	int mismatchesV2 = CheckHighlight(&ui, pool, &camera, &scene, &matLib, &rayTaskQueue, &skybox, px, py, targetId, applyHighlightV2, 0);
	int mismatchesEdge = CheckHighlight(&ui, pool, &camera, &scene, &matLib, &rayTaskQueue, &skybox, px, py, targetId, applyHighlightEdge, 1);
	ConvertToLdr(camera.framebufferHdr, camera.framebufferLdr, WIDTH, HEIGHT);
	SaveImage("tests/img/editor_highlight_edge.bmp", &camera);
	printf("Image dumped to tests/img/editor_highlight_edge.bmp\n");

	if (mismatchesV1)
		printf("CORRECTNESS FAIL: applyHighlight %d mismatching pixels\n", mismatchesV1);
	else
		printf("Correctness: OK (applyHighlight tint matches highlightColor, cursor drawn)\n");
	if (mismatchesV2)
		printf("CORRECTNESS FAIL: applyHighlightV2 %d mismatching pixels\n", mismatchesV2);
	else
		printf("Correctness: OK (applyHighlightV2 tint matches highlightColor, cursor drawn)\n");
	if (mismatchesEdge)
		printf("CORRECTNESS FAIL: applyHighlightEdge %d mismatching pixels\n", mismatchesEdge);
	else
		printf("Correctness: OK (applyHighlightEdge outline white, interior tinted, cursor drawn)\n");

	RenderSetup(scene.objects, scene.count, &camera);
	RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, pool, &skybox);
	drawStats(&ui, 20, 20, 300, 260);
	ConvertToLdr(camera.framebufferHdr, camera.framebufferLdr, WIDTH, HEIGHT);
	SaveImage("tests/img/editor_stats_inactive.bmp", &camera);
	printf("Image dumped to tests/img/editor_stats_inactive.bmp\n");

	// Stat panel showcase: the editor's stats overlay at two panel sizes. Full
	// editor stack (highlight + cursor + panel), tonemapped once on save.
	activateEditorUi(&ui);
	struct {
		const char *path;
		int width;
		int height;
	} statVariants[2] = {
		{"tests/img/editor_stats_small.bmp", 300, 260},
		{"tests/img/editor_stats_large.bmp", 600, 520},
	};
	for (int v = 0; v < 2; v++) {
		RenderSetup(scene.objects, scene.count, &camera);
		RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, pool, &skybox);
		applyHighlight(&ui, pool, px, py);
		drawStats(&ui, 20, 20, statVariants[v].width, statVariants[v].height);
		ConvertToLdr(camera.framebufferHdr, camera.framebufferLdr, WIDTH, HEIGHT);
		SaveImage(statVariants[v].path, &camera);
		printf("Image dumped to %s\n", statVariants[v].path);
	}

	poolDestroy(pool);
	DestroySkybox(&skybox);
	MaterialLib_Destroy(&matLib);
	ObjectList_Destroy(&scene);
	destroyCamera(&camera);
	return (mismatchesV1 || mismatchesV2 || mismatchesEdge) != 0;
}
