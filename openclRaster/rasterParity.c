// Offscreen parity harness for the OpenCL rasterizer: renders the same scene and camera
// with the CPU ray tracer and the GPU rasterizer, then reports how far apart they are and
// dumps both to BMP. Run from the repo root (it loads assets/models and the skybox).
//
//	make -C openclRaster run
//
// Expected differences are listed at the bottom of printSummary: the raster path is the
// "not fancy" subset (no specular, sky reflection, shadows, emission indirect, AO).
#include "raster.h"

#include "../object/scene.h"
#include "../object/material/material.h"
#include "../render/render.h"
#include "../render/cpu/ray.h"
#include "../render/color/color.h"
#include "../skybox/skybox.h"
#include "../util/saveImage.h"
#include "../util/threadPool.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define OUT_DIR "openclRaster/out"

typedef struct {
	int coveredBoth;
	int coveredRasterOnly;
	int coveredRayOnly;
	int depthMismatch; // covered by both, view-Z apart by more than DEPTH_TOLERANCE
	float maxDepthDiff;
	int colorMismatch; // covered by both, LDR apart by more than COLOR_TOLERANCE
	int maxColorDiff;
	int colorDiffHist[9]; // mismatched pixels bucketed by channel diff (32 LDR steps)
	int colorMismatchObj[32];
	int rasterCloser; // raster surface in front of the ray surface (ray missed geometry?)
	int rayCloser;	  // ray surface in front of the raster surface (raster overdraws?)
	int normalMismatch; // shared pixels where the geometric normal differs by more than NORMAL_TOLERANCE
	float maxNormalDiff;
	int normalMismatchObj[32]; // which objects the normal diffs land on (textured = expected)
	// Where the depth disagrees most: same object/triangle means a math bug, different
	// object means the two paths picked different surfaces at that pixel.
	int worstCount;
	int worstIdx[8];
	float worstDepth[8];
} DiffStats;

#define DEPTH_TOLERANCE 0.01f
#define COLOR_TOLERANCE 8
#define NORMAL_TOLERANCE 0.05f // radians between shading normal and face normal

static float nowSeconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (float)ts.tv_sec + (float)ts.tv_nsec * 1e-9f;
}

static int compareFloats(const void *a, const void *b) {
	float fa = *(const float *)a, fb = *(const float *)b;
	return fa < fb ? -1 : fa > fb ? 1 : 0;
}

static float medianOf(float *values, int count) {
	qsort(values, count, sizeof(float), compareFloats);
	return values[count / 2];
}

// The ray tracer writes sky pixels; the rasterizer leaves them to the caller. Clearing to
// a known value first makes "not covered" comparable on both sides.
static void resetGbuffers(Camera *cam) {
	int pixels = cam->screenWidth * cam->screenHeight;
	memset(cam->framebufferHdr, 0, pixels * sizeof(float3));
	memset(cam->normalBuffer, 0, pixels * sizeof(float3));
	memset(cam->positionBuffer, 0, pixels * sizeof(float3));
	for (int i = 0; i < pixels; i++) {
		cam->depthBuffer[i] = DEPTH_FAR;
		cam->objectIdBuffer[i] = -1;
		cam->triangleIdBuffer[i] = -1;
	}
}

static void packLdr(const float3 *hdr, uint32 *ldr, int pixels) {
	for (int i = 0; i < pixels; i++) {
		float3 c = hdr[i];
		// Same tonemap as ConvertToLdr: overflow spreads from the max channel, then clamps.
		float e = fmaxf(c.x, fmaxf(c.y, c.z)) - 1.0f;
		if (e > 0.0f) {
			c.x = fminf(c.x + e, 1.0f);
			c.y = fminf(c.y + e, 1.0f);
			c.z = fminf(c.z + e, 1.0f);
		}
		uint8 r = (uint8)(c.x > 1.0f ? 255 : c.x < 0.0f ? 0 : (int)(c.x * 255.0f));
		uint8 g = (uint8)(c.y > 1.0f ? 255 : c.y < 0.0f ? 0 : (int)(c.y * 255.0f));
		uint8 b = (uint8)(c.z > 1.0f ? 255 : c.z < 0.0f ? 0 : (int)(c.z * 255.0f));
		ldr[i] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
	}
}

static void diffFrames(const Camera *cam, const float *rtDepth, const uint32 *rtLdr,
					   const float3 *rtNormal, const float *rasterDepth, const uint32 *rasterLdr,
					   DiffStats *stats) {
	memset(stats, 0, sizeof(*stats));
	const int pixels = cam->screenWidth * cam->screenHeight;
	for (int i = 0; i < pixels; i++) {
		int hitRay = rtDepth[i] < DEPTH_FAR;
		int hitRaster = rasterDepth[i] < DEPTH_FAR;
		if (hitRaster && !hitRay) stats->coveredRasterOnly++;
		if (hitRay && !hitRaster) stats->coveredRayOnly++;
		if (!hitRay || !hitRaster) continue;

		stats->coveredBoth++;
		float ddiff = fabsf(rtDepth[i] - rasterDepth[i]);
		if (ddiff > stats->maxDepthDiff) stats->maxDepthDiff = ddiff;
		if (ddiff > DEPTH_TOLERANCE) {
			stats->depthMismatch++;
			if (rasterDepth[i] < rtDepth[i]) stats->rasterCloser++;
			else stats->rayCloser++;
			// Keep the eight largest, insertion-sorted — enough to characterise the failure.
			int slot = stats->worstCount < 8 ? stats->worstCount++ : -1;
			if (slot < 0) {
				slot = 7;
				for (int k = 6; k >= 0; k--)
					if (ddiff > stats->worstDepth[k]) { slot = k; break; }
			}
			if (ddiff > stats->worstDepth[slot]) {
				for (int k = slot + 1; k < 8 && k < stats->worstCount; k++) {
					if (stats->worstDepth[k] >= ddiff) break;
					stats->worstDepth[k - 1] = stats->worstDepth[k];
					stats->worstIdx[k - 1] = stats->worstIdx[k];
					slot = k;
				}
				stats->worstDepth[slot] = ddiff;
				stats->worstIdx[slot] = i;
			}
		}

		const uint32 *a = &rtLdr[i], *b = &rasterLdr[i];
		int worst = 0;
		for (int shift = 0; shift <= 16; shift += 8) {
			int d = (int)((*a >> shift) & 0xFF) - (int)((*b >> shift) & 0xFF);
			if (d < 0) d = -d;
			if (d > worst) worst = d;
		}
		if (worst > stats->maxColorDiff) stats->maxColorDiff = worst;
		if (worst > COLOR_TOLERANCE) {
			stats->colorMismatch++;
			stats->colorDiffHist[worst / 32]++;
			stats->colorMismatchObj[cam->objectIdBuffer[i]]++;
		}

		// Geometric normals: the ray tracer stores the (possibly normal-mapped) shading
		// normal, the raster path the face normal, so they only have to agree on
		// untextured surfaces — this is a tolerance check, not an exact one.
		float3 nr = cam->normalBuffer[i], nn = rtNormal[i];
		float nlen = sqrtf(nr.x * nr.x + nr.y * nr.y + nr.z * nr.z);
		if (nlen > 1e-4f) {
			float dot = (nr.x * nn.x + nr.y * nn.y + nr.z * nn.z) / nlen;
			float ndiff = acosf(dot < -1.0f ? -1.0f : dot > 1.0f ? 1.0f : dot);
			if (ndiff > stats->maxNormalDiff) stats->maxNormalDiff = ndiff;
			if (ndiff > NORMAL_TOLERANCE) {
				stats->normalMismatch++;
				stats->normalMismatchObj[cam->objectIdBuffer[i]]++;
			}
		}
	}
}

static void printWorstPixels(const Camera *cam, const float *rtDepth, const float *rasterDepth,
							 const int *rtObjectId, const DiffStats *stats) {
	printf("Worst depth mismatches (x, y, ray Z, raster Z, ray obj, raster obj/tri):\n");
	for (int k = 0; k < stats->worstCount; k++) {
		int i = stats->worstIdx[k];
		printf("  %4d %4d  %10.3f %10.3f  %3d  %3d/%7d\n",
			   i % cam->screenWidth, i / cam->screenWidth, rtDepth[i], rasterDepth[i],
			   rtObjectId[i], cam->objectIdBuffer[i], cam->triangleIdBuffer[i]);
	}
}

static void saveFrame(Camera *cam, const float3 *hdr, const char *path) {
	packLdr(hdr, cam->framebufferLdr, cam->screenWidth * cam->screenHeight);
	SaveImage(path, cam);
}

static void printSummary(const DiffStats *stats, float rasterMs) {
	printf("=====================================================\n");
	printf("Coverage\n");
	printf("  pixels hit by both:      %d\n", stats->coveredBoth);
	printf("  ray only (raster missed): %d\n", stats->coveredRayOnly);
	printf("  raster only (ray missed): %d\n", stats->coveredRasterOnly);
	printf("Depth (view Z, tolerance %.2f)\n", DEPTH_TOLERANCE);
	printf("  mismatched pixels:       %d\n", stats->depthMismatch);
	printf("    raster closer:         %d\n", stats->rasterCloser);
	printf("    ray closer:            %d\n", stats->rayCloser);
	printf("  max difference:          %.4f\n", stats->maxDepthDiff);
	printf("Color (LDR 0-255, tolerance %d)\n", COLOR_TOLERANCE);
	printf("  mismatched pixels:       %d\n", stats->colorMismatch);
	printf("  max channel difference:  %d\n", stats->maxColorDiff);
	printf("  diff histogram (32-step buckets):");
	for (int b = 0; b < 8; b++)
		if (stats->colorDiffHist[b]) printf(" %d-%d:%d", b * 32, b * 32 + 31, stats->colorDiffHist[b]);
	printf("\n  by object:");
	for (int i = 0; i < 32; i++)
		if (stats->colorMismatchObj[i]) printf(" obj%d=%d", i, stats->colorMismatchObj[i]);
	printf("\n");
	printf("Normals (radians, tolerance %.2f)\n", NORMAL_TOLERANCE);
	printf("  mismatched pixels:       %d\n", stats->normalMismatch);
	printf("  max angle difference:    %.4f\n", stats->maxNormalDiff);
	printf("  by object:");
	for (int i = 0; i < 32; i++)
		if (stats->normalMismatchObj[i]) printf(" obj%d=%d", i, stats->normalMismatchObj[i]);
	printf("\n");
	printf("Raster pass time (median): %.3f ms\n", rasterMs);
	printf("=====================================================\n");
	printf("Expected diffs (raster is the 'not fancy' subset of the ray tracer):\n");
	printf("  - no GGX specular, no sky reflection, no shadows, no emission indirect light\n");
	printf("  - no ambient occlusion (RayTraceScene applies it, the raster path does not)\n");
	printf("  - triangles with a vertex nearer than the near plane are skipped, not clipped\n");
	printf("  - sky pixels are the caller's: cleared to black here, sky-colored in the ray pass\n");
	printf("=====================================================\n");
}

// Coverage mask as a PGM (255 = a raster pixel, 0 = caller-owned sky). Kept next to the
// frame dumps so a geometry-only diff can ignore the sky the raster does not draw.
static void saveCoverageMask(const char *path, const float *depth, int width, int height) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "could not write %s\n", path);
		return;
	}
	fprintf(f, "P5\n%d %d\n255\n", width, height);
	for (int i = 0; i < width * height; i++)
		fputc(depth[i] < DEPTH_FAR ? 255 : 0, f);
	fclose(f);
}

// Non-circular check of the base lighting path: the shape-parade objects (indices 5..N of
// the parade block) are untextured with metallic=0 and emission=0, so their shaded color is
// fully determined by the scene's own material color and the normal the parity already
// proved exact. This validates the lighting formula, tonemap and packing without comparing
// against the ray tracer's extra terms.
static void checkAnalyticLighting(const ObjectList *scene, const MaterialLib *lib,
								  const Camera *cam, const float *rasterDepth) {
	const int pixels = cam->screenWidth * cam->screenHeight;
	const float3 light = Float3_Normalize(cam->lightDir);
	int checked = 0, bad = 0;
	float worstDiff = 0.0f;

	for (int i = 0; i < pixels && checked < 4000; i++) {
		if (rasterDepth[i] >= DEPTH_FAR) continue;
		int objId = cam->objectIdBuffer[i];
		if (objId < 0 || objId >= scene->count) continue;
		const Object *obj = &scene->objects[objId];
		if (obj->hasTexture || !obj->materialIds) continue;
		int matId = obj->materialIds[cam->triangleIdBuffer[i]];
		if (matId < 0 || matId >= lib->count) continue;
		const Material *mat = &lib->entries[matId];
		if (mat->metallic > 0.0f || mat->emission > 0.0f || mat->textures) continue;

		float3 n = cam->normalBuffer[i];
		float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
		if (nlen < 1e-4f) continue;
		float ndl = fmaxf(0.0f, (n.x * light.x + n.y * light.y + n.z * light.z) / nlen);
		float lit = 0.02f + 0.98f * ndl;
		float3 hdr = {mat->color.x * lit, mat->color.y * lit, mat->color.z * lit};
		uint32 expect;
		packLdr(&hdr, &expect, 1);

		uint32 got = cam->framebufferLdr[i];
		int diff = 0;
		for (int shift = 0; shift <= 16; shift += 8) {
			int d = (int)((expect >> shift) & 0xFF) - (int)((got >> shift) & 0xFF);
			if (d < 0) d = -d;
			if (d > diff) diff = d;
		}
		checked++;
		if (diff > 1) {
			bad++;
			if (diff > worstDiff) worstDiff = (float)diff;
		}
	}
	printf("Analytic lighting check: %d pixels checked, %d off by >1 LDR (worst %d)\n",
		   checked, bad, (int)worstDiff);
}

int main(int argc, char **argv) {
	mkdir(OUT_DIR, 0755); // SaveImage does not create directories

	// Above HEIGHT the ray tracer skips its screen-space tile pre-filter (its guard is
	// screenHeight <= HEIGHT), so a larger frame A/B-tests that path without touching it.
	int renderWidth = argc > 1 ? atoi(argv[1]) : WIDTH;
	int renderHeight = argc > 2 ? atoi(argv[2]) : HEIGHT;
	printf("Rendering %dx%d\n", renderWidth, renderHeight);

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	ObjectList scene;
	ObjectList_Init(&scene, 8);
	Scene_BuildShowcase(&scene, &matLib);

	Object *plane = ObjectList_Add(&scene);
	LoadObj("assets/models/f16.bin", plane, &matLib);
	plane->position = (float3){0.0f, 10.0f, 20.0f};
	plane->scale = (float3){0.75f, 0.75f, 0.75f};
	CreateObjectBVH(plane, &plane->bvh);
	Object_UpdateWorldBounds(plane);

	Object *palace = ObjectList_Add(&scene);
	LoadObj("assets/models/palace2.bin", palace, &matLib);
	palace->position = (float3){-15.0f, 0.0f, 15.0f};
	palace->rotation = (float3){0.0f, 40.0f, 0.0f};
	CreateObjectBVH(palace, &palace->bvh);
	Object_UpdateWorldBounds(palace);

	printf("Scene: %d objects, %d triangles\n", scene.count, ObjectList_CountTriangles(&scene));

	Camera camera;
	initCamera(&camera, renderWidth, renderHeight, 90.0f, (float3){5.0f, 5.0f, -15.0f},
			   (float3){0.0f, -0.15f, 1.0f}, (float3){0.0f, 80.0f, -60.0f});

	Skybox skybox;
	LoadSkybox(&skybox, "skybox");

	ThreadPool *threadPool = poolCreate(32, renderWidth);
	RayTraceTaskQueue taskQueue;

	const int pixels = camera.screenWidth * camera.screenHeight;
	float *rtDepth = malloc(pixels * sizeof(float));
	float *rasterDepth = malloc(pixels * sizeof(float));
	uint32 *rtLdr = malloc(pixels * sizeof(uint32));
	uint32 *rasterLdr = malloc(pixels * sizeof(uint32));
	int *rtObjectId = malloc(pixels * sizeof(int));
	float3 *rtNormal = malloc(pixels * sizeof(float3));
	if (!rtDepth || !rasterDepth || !rtLdr || !rasterLdr || !rtObjectId || !rtNormal) {
		fprintf(stderr, "Failed to allocate comparison buffers\n");
		return 1;
	}

	// The ray tracer's row kernel ignores camera.jitter, so the parity run zeroes it and
	// both paths sample pixel centres. Jitter stays available to the raster API for the
	// caller's own subpixel policy.
	camera.jitter = (float2){0.0f, 0.0f};

	RenderSetup(scene.objects, scene.count, &camera);
	RayTraceScene(scene.objects, scene.count, &camera, &matLib, &taskQueue, threadPool, &skybox);
	memcpy(rtDepth, camera.depthBuffer, pixels * sizeof(float));
	memcpy(rtObjectId, camera.objectIdBuffer, pixels * sizeof(int));
	memcpy(rtNormal, camera.normalBuffer, pixels * sizeof(float3));
	saveFrame(&camera, camera.framebufferHdr, OUT_DIR "/parity_ray.bmp");
	memcpy(rtLdr, camera.framebufferLdr, pixels * sizeof(uint32));

	RasterRenderer raster;
	Raster_Init(&raster, "openclRaster/raster.cl", renderWidth, renderHeight);
	if (!raster.enabled) {
		fprintf(stderr, "Rasterizer unavailable — nothing to compare against\n");
		return 1;
	}
	Raster_UploadScene(&raster, &scene, &matLib);
	Raster_SetSkybox(&raster, &skybox);

	resetGbuffers(&camera);
	Raster_Render(&raster, &scene, &matLib, &camera);
	memcpy(rasterDepth, camera.depthBuffer, pixels * sizeof(float));
	saveFrame(&camera, camera.framebufferHdr, OUT_DIR "/parity_raster.bmp");
	saveCoverageMask(OUT_DIR "/coverage.pgm", rasterDepth, renderWidth, renderHeight);
	memcpy(rasterLdr, camera.framebufferLdr, pixels * sizeof(uint32));

	DiffStats stats;
	diffFrames(&camera, rtDepth, rtLdr, rtNormal, rasterDepth, rasterLdr, &stats);

	// Timing only — the image above is already captured.
	enum { PARITY_SAMPLES = 10 };
	float samples[PARITY_SAMPLES];
	for (int i = 0; i < PARITY_SAMPLES; i++) {
		float t0 = nowSeconds();
		Raster_Render(&raster, &scene, &matLib, &camera);
		samples[i] = (nowSeconds() - t0) * 1000.0f;
	}
	printf("Raster kernel times: depth %.3f ms + shade %.3f ms\n",
		   raster.depthPipeline.timeTook, raster.shadePipeline.timeTook);

	printSummary(&stats, medianOf(samples, PARITY_SAMPLES));
	checkAnalyticLighting(&scene, &matLib, &camera, rasterDepth);
	printWorstPixels(&camera, rtDepth, rasterDepth, rtObjectId, &stats);

	free(rtDepth);
	free(rasterDepth);
	free(rtLdr);
	free(rasterLdr);
	free(rtObjectId);
	free(rtNormal);
	Raster_Destroy(&raster);
	poolDestroy(threadPool);
	DestroySkybox(&skybox);
	destroyCamera(&camera);
	ObjectList_Destroy(&scene);
	MaterialLib_Destroy(&matLib);
	return 0;
}
