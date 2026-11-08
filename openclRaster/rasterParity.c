// Parity harness: same scene and camera through the CPU ray tracer and the OpenCL raster,
// reporting per-G-buffer differences. Run from the repo root: make -C openclRaster run
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
	int rasterCloser;	// raster surface in front of the ray surface (ray missed geometry?)
	int rayCloser;		// ray surface in front of the raster surface (raster overdraws?)
	int normalMismatch; // shared pixels where the geometric normal differs by more than NORMAL_TOLERANCE
	float maxNormalDiff;
	int normalMismatchObj[32]; // which objects the normal diffs land on (textured = expected)
	int objectIdMismatch;	   // shared pixels whose objectIdBuffer disagrees
	int triangleIdMismatch;	   // shared pixels whose triangleIdBuffer disagrees
	int positionMismatch;	   // shared pixels whose positionBuffer is further apart than the tolerance
	float maxPositionDiff;
	// Where the depth disagrees most: same object/triangle means a math bug, different
	// object means the two paths picked different surfaces at that pixel.
	int worstCount;
	int worstIdx[8];
	float worstDepth[8];
} DiffStats;

#define DEPTH_TOLERANCE 0.01f
#define COLOR_TOLERANCE 8
#define NORMAL_TOLERANCE 0.05f	 // radians between shading normal and face normal
#define POSITION_TOLERANCE 0.01f // world units between the two reconstructed surface points
// Raster_Render dispatches this many separable AO blur passes (must match raster.c).
#define AO_BLUR_PASSES 6

static float nowSeconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (float)ts.tv_sec + (float)ts.tv_nsec * 1e-9f;
}

static int compareFloats(const void *a, const void *b) {
	float fa = *(const float *)a, fb = *(const float *)b;
	return fa < fb ? -1 : fa > fb ? 1
								  : 0;
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
		uint8 r = (uint8)(c.x > 1.0f ? 255 : c.x < 0.0f ? 0
														: (int)(c.x * 255.0f));
		uint8 g = (uint8)(c.y > 1.0f ? 255 : c.y < 0.0f ? 0
														: (int)(c.y * 255.0f));
		uint8 b = (uint8)(c.z > 1.0f ? 255 : c.z < 0.0f ? 0
														: (int)(c.z * 255.0f));
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
			if (rasterDepth[i] < rtDepth[i])
				stats->rasterCloser++;
			else
				stats->rayCloser++;
			// Keep the eight largest, insertion-sorted — enough to characterise the failure.
			int slot = stats->worstCount < 8 ? stats->worstCount++ : -1;
			if (slot < 0) {
				slot = 7;
				for (int k = 6; k >= 0; k--)
					if (ddiff > stats->worstDepth[k]) {
						slot = k;
						break;
					}
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
			float ndiff = acosf(dot < -1.0f ? -1.0f : dot > 1.0f ? 1.0f
																 : dot);
			if (ndiff > stats->maxNormalDiff) stats->maxNormalDiff = ndiff;
			if (ndiff > NORMAL_TOLERANCE) {
				stats->normalMismatch++;
				stats->normalMismatchObj[cam->objectIdBuffer[i]]++;
			}
		}
	}
}

// The id buffers are part of the parity contract: consumers read objectIdBuffer as the
// scene object index and triangleIdBuffer as the index within that object's triangle
// array, so a mismatch here breaks callers even when the pixels look identical.
static void diffIds(const Camera *cam, const int *rtObjectId, const int *rtTriangleId,
					const float *rtDepth, const float *rasterDepth, DiffStats *stats) {
	const int pixels = cam->screenWidth * cam->screenHeight;
	int rayTriangleIdsWritten = 0;

	for (int i = 0; i < pixels; i++) {
		if (rtTriangleId[i] >= 0) rayTriangleIdsWritten++;
		if (rtDepth[i] >= DEPTH_FAR || rasterDepth[i] >= DEPTH_FAR) continue;
		if (rtObjectId[i] != cam->objectIdBuffer[i]) stats->objectIdMismatch++;
		if (rtTriangleId[i] != cam->triangleIdBuffer[i]) stats->triangleIdMismatch++;
	}
	// RayTraceScene uses the row kernel, which leaves triangleIdBuffer untouched (only the
	// unused column kernel fills it), so that comparison has no reference to compare to.
	if (rayTriangleIdsWritten == 0) stats->triangleIdMismatch = -1;
}

// The reconstructed surface point should agree to well under a pixel: both paths interpolate
// the same vertices with the same perspective-correct weights, so any gap here is a bug.
static void diffPositions(const Camera *cam, const float3 *rtPos, const float *rtDepth,
						  const float *rasterDepth, DiffStats *stats) {
	const int pixels = cam->screenWidth * cam->screenHeight;
	for (int i = 0; i < pixels; i++) {
		if (rtDepth[i] >= DEPTH_FAR || rasterDepth[i] >= DEPTH_FAR) continue;
		float3 d = Float3_Sub(rtPos[i], cam->positionBuffer[i]);
		float dist = sqrtf(Float3_Dot(d, d));
		if (dist > stats->maxPositionDiff) stats->maxPositionDiff = dist;
		if (dist > POSITION_TOLERANCE) stats->positionMismatch++;
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
	printf("Ids (shared pixels)\n");
	printf("  objectId mismatched:     %d\n", stats->objectIdMismatch);
	if (stats->triangleIdMismatch < 0)
		printf("  triangleId:              no reference (RayTraceScene's row kernel does not write it)\n");
	else
		printf("  triangleId mismatched:   %d\n", stats->triangleIdMismatch);
	printf("Position (world units, tolerance %.3f)\n", POSITION_TOLERANCE);
	printf("  mismatched pixels:       %d\n", stats->positionMismatch);
	printf("  max distance:            %.6f\n", stats->maxPositionDiff);
	printf("Raster pass time (median): %.3f ms\n", rasterMs);
	printf("=====================================================\n");
	printf("Expected diffs:\n");
	printf("  - triangles with a vertex nearer than the near plane are skipped, not clipped\n");
	printf("  - sky pixels are the caller's: cleared to black here, sky-colored in the ray pass\n");
	printf("  - normals are face normals here, normal-mapped shading normals in the ray pass\n");
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

// Same point-light reduction the raster uploads: one entry per emissive object, carrying
// color*emission of its strongest material. Used by the analytic check to predict the
// additive indirect term without asking the GPU for it.
typedef struct {
	float3 position;
	float3 colorEmission;
} CheckEmitter;

static int gatherEmitters(const ObjectList *scene, const MaterialLib *lib, CheckEmitter *out, int cap) {
	int count = 0;
	for (int i = 0; i < scene->count && count < cap; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->hasEmission || !obj->v1 || obj->triangleCount <= 0) continue;
		float3 color = {0.0f, 0.0f, 0.0f};
		float emission = 0.0f;
		for (int k = 0; k < obj->triangleCount; k++) {
			int matId = obj->materialIds ? obj->materialIds[k] : -1;
			if (matId < 0 || matId >= lib->count) continue;
			if (lib->entries[matId].emission > emission) {
				emission = lib->entries[matId].emission;
				color = lib->entries[matId].color;
			}
		}
		if (emission <= 0.0f) continue;
		out[count++] = (CheckEmitter){obj->position,
									  {color.x * emission, color.y * emission, color.z * emission}};
	}
	return count;
}

static float3 emissiveIndirect(const CheckEmitter *emitters, int count, float3 pos, float3 n) {
	float3 sum = {0.0f, 0.0f, 0.0f};
	for (int e = 0; e < count; e++) {
		float3 toEmitter = Float3_Sub(emitters[e].position, pos);
		float dist2 = Float3_Dot(toEmitter, toEmitter) + 1e-6f;
		float3 toN = Float3_Scale(toEmitter, 1.0f / sqrtf(dist2));
		float nd = fabsf(Float3_Dot(n, toN));
		if (nd <= 0.0f) continue;
		float falloff = nd / dist2;
		sum.x += emitters[e].colorEmission.x * falloff;
		sum.y += emitters[e].colorEmission.y * falloff;
		sum.z += emitters[e].colorEmission.z * falloff;
	}
	return sum;
}

// Non-circular check of the shading chain: the shape-parade objects are untextured with
// metallic=0 and emission=0, so their color is a pure function of the material color, the
// normal (parity-proved exact), the camera position, the skybox and the emitters.
// Runs with AO and shadows off, which are separate terms.
static void checkAnalyticLighting(const ObjectList *scene, const MaterialLib *lib,
								  const Camera *cam, const Skybox *skybox, const float *rasterDepth) {
	const int pixels = cam->screenWidth * cam->screenHeight;
	const float3 light = Float3_Normalize(cam->lightDir);
	int checked = 0, bad = 0;
	float worstDiff = 0.0f;

	CheckEmitter emitters[8];
	int emitterCount = gatherEmitters(scene, lib, emitters, 8);

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

		float3 viewDir = Float3_Sub(cam->positionBuffer[i], cam->position);
		float viewLen = sqrtf(Float3_Dot(viewDir, viewDir));
		if (viewLen > 1e-6f) {
			viewDir = Float3_Scale(viewDir, 1.0f / viewLen);
			float ndv = fmaxf(1e-4f, -Float3_Dot(n, viewDir));
			float invNdv = 1.0f - ndv;
			float inv2 = invNdv * invNdv;
			float fresnel = 0.04f + 0.96f * (inv2 * inv2 * invNdv);

			float3 halfVec = Float3_Sub(light, viewDir);
			float halfLen = sqrtf(Float3_Dot(halfVec, halfVec));
			if (halfLen > 1e-6f) {
				halfVec = Float3_Scale(halfVec, 1.0f / halfLen);
				float ndh = fmaxf(0.0f, Float3_Dot(n, halfVec));
				float alpha = mat->roughness * mat->roughness;
				float alpha2 = alpha * alpha;
				float dg = ndh * ndh * (alpha2 - 1.0f) + 1.0f;
				float D = alpha2 / fmaxf(dg * dg, 1e-5f);
				float k = alpha * 0.5f;
				float gv = ndv / fmaxf(ndv * (1.0f - k) + k, 1e-5f);
				float gl = ndl / fmaxf(ndl * (1.0f - k) + k, 1e-5f);
				float spec = ndl > 0.0f ? (D * gv * gl * fresnel / fmaxf(4.0f * ndl * ndv, 1e-5f)) : 0.0f;
				hdr.x += spec;
				hdr.y += spec;
				hdr.z += spec;
			}

			float st = fminf(fresnel * (1.0f - mat->roughness) * 0.18f * (0.15f + 0.85f * ndl), 1.0f);
			if (st > 0.0f) {
				float dot2 = 2.0f * Float3_Dot(n, viewDir);
				float3 reflDir = {viewDir.x - n.x * dot2, viewDir.y - n.y * dot2, viewDir.z - n.z * dot2};
				float3 sky = UnpackColor(SampleSkybox(skybox, reflDir));
				hdr.x = hdr.x * (1.0f - st) + sky.x * st;
				hdr.y = hdr.y * (1.0f - st) + sky.y * st;
				hdr.z = hdr.z * (1.0f - st) + sky.z * st;
			}
		}

		// Emissive indirect is additive after the shadow term (shadows are off for this check).
		if (emitterCount > 0) {
			float3 emitted = emissiveIndirect(emitters, emitterCount, cam->positionBuffer[i], n);
			hdr.x += emitted.x;
			hdr.y += emitted.y;
			hdr.z += emitted.z;
		}

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
	// Third argument overrides the AO strength (0 = no AO passes), fourth the shadow map
	// size (0 = no shadows). Both default to whatever the renderer was initialised with.
	int hasAoOverride = argc > 3;
	float aoOverride = hasAoOverride ? (float)atof(argv[3]) : 0.0f;
	int shadowSize = argc > 4 ? atoi(argv[4]) : 2048;
	// Fifth argument overrides the read-back set (RASTER_OUT_* bits, default framebuffer+depth).
	unsigned readbackMask = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 0) : RASTER_OUT_FRAME;
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
	int *rtTriangleId = malloc(pixels * sizeof(int));
	float3 *rtNormal = malloc(pixels * sizeof(float3));
	float3 *rtPos = malloc(pixels * sizeof(float3));
	if (!rtDepth || !rasterDepth || !rtLdr || !rasterLdr || !rtObjectId || !rtTriangleId || !rtNormal || !rtPos) {
		fprintf(stderr, "Failed to allocate comparison buffers\n");
		return 1;
	}

	// The ray tracer's row kernel ignores camera.jitter, so the parity run zeroes it and
	// both paths sample pixel centres. Jitter stays available to the raster API for the
	// caller's own subpixel policy.
	camera.jitter = (float2){0.0f, 0.0f};

	// Reset first so every G-buffer starts from a known value: the ray row kernel does not
	// write triangleIdBuffer, and a defined baseline makes that visible instead of reading
	// whatever the allocation happened to contain.
	resetGbuffers(&camera);
	RenderSetup(scene.objects, scene.count, &camera);
	RayTraceScene(scene.objects, scene.count, &camera, &matLib, &taskQueue, threadPool, &skybox);
	memcpy(rtDepth, camera.depthBuffer, pixels * sizeof(float));
	memcpy(rtObjectId, camera.objectIdBuffer, pixels * sizeof(int));
	memcpy(rtTriangleId, camera.triangleIdBuffer, pixels * sizeof(int));
	memcpy(rtPos, camera.positionBuffer, pixels * sizeof(float3));
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
	if (hasAoOverride) raster.aoStrength = aoOverride;
	raster.shadowMapSize = shadowSize;
	// The harness compares every G-buffer, so it asks for all of them; the frame path
	// default is framebuffer + depth only.
	raster.readbackMask = RASTER_OUT_ALL;
	printf("AO strength %.2f, shadow map %d\n", raster.aoStrength, raster.shadowMapSize);

	resetGbuffers(&camera);
	Raster_Render(&raster, &scene, &matLib, &camera);
	memcpy(rasterDepth, camera.depthBuffer, pixels * sizeof(float));
	saveFrame(&camera, camera.framebufferHdr, OUT_DIR "/parity_raster.bmp");
	saveCoverageMask(OUT_DIR "/coverage.pgm", rasterDepth, renderWidth, renderHeight);
	memcpy(rasterLdr, camera.framebufferLdr, pixels * sizeof(uint32));

	DiffStats stats;
	diffFrames(&camera, rtDepth, rtLdr, rtNormal, rasterDepth, rasterLdr, &stats);
	diffIds(&camera, rtObjectId, rtTriangleId, rtDepth, rasterDepth, &stats);
	diffPositions(&camera, rtPos, rtDepth, rasterDepth, &stats);

	// The analytic check expects the AO- and shadow-free shading value, so it runs on a
	// separate frame with both extra terms disabled (geometry and normals are unchanged).
	float savedAo = raster.aoStrength;
	int savedShadow = raster.shadowMapSize;
	raster.aoStrength = 0.0f;
	raster.shadowMapSize = 0;
	Raster_Render(&raster, &scene, &matLib, &camera);
	packLdr(camera.framebufferHdr, camera.framebufferLdr, pixels);
	checkAnalyticLighting(&scene, &matLib, &camera, &skybox, rasterDepth);
	raster.aoStrength = savedAo;
	raster.shadowMapSize = savedShadow;

	// Timing only — the images above are already captured. Everything is measured from the
	// same camera and scene, so the three numbers are directly comparable.
	enum { PARITY_SAMPLES = 10 };
	float samples[PARITY_SAMPLES];
	unsigned savedMask = raster.readbackMask;
	raster.readbackMask = RASTER_OUT_FRAME; // what a real frame transfers
	for (int i = 0; i < PARITY_SAMPLES; i++) {
		float t0 = nowSeconds();
		Raster_Render(&raster, &scene, &matLib, &camera);
		samples[i] = (nowSeconds() - t0) * 1000.0f;
	}
	float colorOnlySamples[PARITY_SAMPLES];
	raster.readbackMask = RASTER_OUT_FRAMEBUFFER; // colour only: the minimum a frame needs
	for (int i = 0; i < PARITY_SAMPLES; i++) {
		float t0 = nowSeconds();
		Raster_Render(&raster, &scene, &matLib, &camera);
		colorOnlySamples[i] = (nowSeconds() - t0) * 1000.0f;
	}
	raster.readbackMask = readbackMask; // whatever the caller asked for
	float customSamples[PARITY_SAMPLES];
	for (int i = 0; i < PARITY_SAMPLES; i++) {
		float t0 = nowSeconds();
		Raster_Render(&raster, &scene, &matLib, &camera);
		customSamples[i] = (nowSeconds() - t0) * 1000.0f;
	}
	float fullSamples[10];
	raster.readbackMask = savedMask; // what the parity check needs
	for (int i = 0; i < 10; i++) {
		float t0 = nowSeconds();
		Raster_Render(&raster, &scene, &matLib, &camera);
		fullSamples[i] = (nowSeconds() - t0) * 1000.0f;
	}

	enum { CPU_SAMPLES = 3 };
	float raySamples[CPU_SAMPLES];
	for (int i = 0; i < CPU_SAMPLES; i++) {
		float t0 = nowSeconds();
		RenderSetup(scene.objects, scene.count, &camera);
		RayTraceScene(scene.objects, scene.count, &camera, &matLib, &taskQueue, threadPool, &skybox);
		raySamples[i] = (nowSeconds() - t0) * 1000.0f;
	}

	float legacySamples[CPU_SAMPLES];
	for (int i = 0; i < CPU_SAMPLES; i++) {
		float t0 = nowSeconds();
		RenderObjects(scene.objects, scene.count, &camera, &matLib);
		legacySamples[i] = (nowSeconds() - t0) * 1000.0f;
	}

	printf("=====================================================\n");
	printf("Frame time, %dx%d, %d triangles\n", renderWidth, renderHeight, raster.triCount);
	float rayMedian = medianOf(raySamples, CPU_SAMPLES);
	float rasterMedian = medianOf(samples, PARITY_SAMPLES);
	// Each pipeline records the time of its last dispatch, so the six blur passes have to
	// be accounted for explicitly.
	float blurTotal = raster.aoBlurPipeline.timeTook * AO_BLUR_PASSES;
	float kernelTotal = raster.shadowPipeline.timeTook + raster.depthPipeline.timeTook +
						raster.shadePipeline.timeTook + raster.skyPipeline.timeTook +
						raster.aoPipeline.timeTook + blurTotal + raster.aoApplyPipeline.timeTook;
	printf("  ray tracer    (CPU, RayTraceScene)   : %7.2f ms\n", rayMedian);
	printf("  CPU raster    (render.c RenderObjects): %7.2f ms\n", medianOf(legacySamples, CPU_SAMPLES));
	printf("  OpenCL raster (Raster_Render, all in) : %7.2f ms\n", rasterMedian);
	printf("  OpenCL raster (frame path, fb+depth)  : %7.2f ms   (colour only: %.2f, all 6 buffers: %.2f)\n",
		   rasterMedian, medianOf(colorOnlySamples, PARITY_SAMPLES), medianOf(fullSamples, 10));
	printf("  OpenCL kernel breakdown:\n");
	printf("      shadow %5.2f | depth %5.2f | shade %6.2f | sky %5.2f | ao %5.2f | blur x%d %5.2f | apply %5.2f\n",
		   raster.shadowPipeline.timeTook, raster.depthPipeline.timeTook, raster.shadePipeline.timeTook,
		   raster.skyPipeline.timeTook, raster.aoPipeline.timeTook, AO_BLUR_PASSES, blurTotal,
		   raster.aoApplyPipeline.timeTook);
	printf("      kernels total %.2f ms, read-back + dispatch overhead %.2f ms\n",
		   kernelTotal, rasterMedian - kernelTotal);
	printf("  vs ray tracer : %+.1f%% (%.2fx)   vs CPU raster: %+.1f%% (%.2fx)\n",
		   (rasterMedian / rayMedian - 1.0f) * 100.0f, rayMedian / rasterMedian,
		   (rasterMedian / medianOf(legacySamples, CPU_SAMPLES) - 1.0f) * 100.0f,
		   medianOf(legacySamples, CPU_SAMPLES) / rasterMedian);
	printf("  with kernels only: %.2fx faster than the ray tracer\n", rayMedian / kernelTotal);

	printSummary(&stats, medianOf(samples, PARITY_SAMPLES));
	printWorstPixels(&camera, rtDepth, rasterDepth, rtObjectId, &stats);

	free(rtDepth);
	free(rasterDepth);
	free(rtLdr);
	free(rasterLdr);
	free(rtObjectId);
	free(rtTriangleId);
	free(rtPos);
	free(rtNormal);
	Raster_Destroy(&raster);
	poolDestroy(threadPool);
	DestroySkybox(&skybox);
	destroyCamera(&camera);
	ObjectList_Destroy(&scene);
	MaterialLib_Destroy(&matLib);
	return 0;
}
