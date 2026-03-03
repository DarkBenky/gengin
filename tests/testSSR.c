#include "testSSR.h"
#include "../math/vector3.h"
#include "../math/transform.h"
#include "../math/scalar.h"
#include "../render/color/color.h"
#include "../render/cpu/ray.h"
#include "../render/cpu/ssr.h"
#include <math.h>

#define SAMPLES 10
#define OBJECT_COUNT 5 // 1 floor + 4 colored cubes

#define SKY_COLOR 0xFF5588BBu

// Fills framebuffer, depthBuffer, normalBuffer, positionBuffer, reflectBuffer for one row.
static void RenderRow(const Object *objects, int objectCount, const MaterialLib *lib, Camera *camera, int row) {
	int W = camera->screenWidth;
	int H = camera->screenHeight;

	float3 orig = camera->position;
	float3 fwd = Float3_Normalize(camera->forward);
	float3 rgt = Float3_Normalize(camera->right);
	float3 up_ = Float3_Normalize(camera->up);
	float aspect = camera->aspect;
	float fovS = camera->fovScale;
	float3 lightDir = Float3_Normalize(camera->lightDir);

	float ndcY = 1.0f - (row + 0.5f) / (float)H * 2.0f;
	float yscale = ndcY * fovS;
	float bx = fwd.x + up_.x * yscale;
	float by = fwd.y + up_.y * yscale;
	float bz = fwd.z + up_.z * yscale;
	float sx = rgt.x * aspect * fovS;
	float sy = rgt.y * aspect * fovS;
	float sz = rgt.z * aspect * fovS;

	for (int x = 0; x < W; x++) {
		int idx = row * W + x;
		float ndcX = (x + 0.5f) / (float)W * 2.0f - 1.0f;
		float dx = bx + sx * ndcX;
		float dy = by + sy * ndcX;
		float dz = bz + sz * ndcX;
		float inv = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz);
		dx *= inv;
		dy *= inv;
		dz *= inv;

		float bestT = DEPTH_FAR;
		int bestObj = -1, bestTri = -1;
		float3 bestHit = {0};

		for (int i = 0; i < objectCount; i++) {
			float bmin, bmax;
			RayBoxItersect(&objects[i], orig, (float3){dx, dy, dz}, &bmin, &bmax);
			if (bmin >= bmax || bmin >= bestT) continue;
			int tri = -1;
			float3 hp;
			IntersectBVH(&objects[i], &objects[i].bvh, orig, (float3){dx, dy, dz}, &tri, &hp);
			if (tri < 0) continue;
			float t = (hp.x - orig.x) * dx + (hp.y - orig.y) * dy + (hp.z - orig.z) * dz;
			if (t > 0.0f && t < bestT) {
				bestT = t;
				bestObj = i;
				bestTri = tri;
				bestHit = hp;
			}
		}

		if (bestObj < 0) {
			camera->framebuffer[idx] = SKY_COLOR;
			camera->depthBuffer[idx] = DEPTH_FAR;
			camera->normalBuffer[idx] = (float3){0};
			camera->positionBuffer[idx] = (float3){0};
			camera->reflectBuffer[idx] = (float3){dx, dy, dz};
			continue;
		}

		const Object *obj = &objects[bestObj];
		float3 n = RotateXYZ(obj->normals[bestTri], obj->rotation);
		float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
		if (nlen > 1e-6f) {
			float ni = 1.0f / nlen;
			n.x *= ni;
			n.y *= ni;
			n.z *= ni;
		}

		float3 color = {0.8f, 0.8f, 0.8f};
		float roughness = 0.8f, emission = 0.0f;
		if (lib && obj->materialIds) {
			int mid = obj->materialIds[bestTri];
			if (mid >= 0 && mid < lib->count) {
				color = lib->entries[mid].color;
				roughness = lib->entries[mid].roughness;
				emission = lib->entries[mid].emission;
			}
		}

		float diff = n.x * lightDir.x + n.y * lightDir.y + n.z * lightDir.z;
		if (diff < 0.0f) diff = 0.0f;
		float lit = 0.12f + 0.88f * diff;

		float hx = lightDir.x - dx, hy = lightDir.y - dy, hz = lightDir.z - dz;
		float hlen = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz);
		float NdotH = fmaxf(0.0f, (n.x * hx + n.y * hy + n.z * hz) * hlen);
		float spec2 = NdotH * NdotH;
		float spec = spec2 * spec2 * spec2 * spec2 * (1.0f - roughness) * 0.4f;
		if (diff <= 0.0f) spec = 0.0f;

		uint8 r = (uint8)(fminf(color.x * lit + spec + color.x * emission, 1.0f) * 255.0f);
		uint8 g = (uint8)(fminf(color.y * lit + spec + color.y * emission, 1.0f) * 255.0f);
		uint8 b = (uint8)(fminf(color.z * lit + spec + color.z * emission, 1.0f) * 255.0f);

		float dot2 = 2.0f * (n.x * dx + n.y * dy + n.z * dz);
		float3 reflDir = {dx - n.x * dot2, dy - n.y * dot2, dz - n.z * dot2};

		camera->framebuffer[idx] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
		// Store view-Z (dot with forward) so SSR depth comparison is consistent
		camera->depthBuffer[idx] = (bestHit.x - orig.x) * fwd.x + (bestHit.y - orig.y) * fwd.y + (bestHit.z - orig.z) * fwd.z;
		camera->normalBuffer[idx] = n;
		camera->positionBuffer[idx] = bestHit;
		// w stores (1-roughness) so SSR knows per-pixel reflectivity
		camera->reflectBuffer[idx] = (float3){reflDir.x, reflDir.y, reflDir.z, 1.0f - roughness};
	}
}

typedef struct {
	const Object *objects;
	int objectCount;
	const MaterialLib *lib;
	Camera *camera;
	int row;
} RenderTask;

static void RenderTaskFunc(void *arg) {
	RenderTask *t = arg;
	RenderRow(t->objects, t->objectCount, t->lib, t->camera, t->row);
}

static void RenderSceneParallel(const Object *objects, int objectCount, const MaterialLib *lib, Camera *camera, ThreadPool *pool, RenderTask *tasks) {
	for (int row = 0; row < camera->screenHeight; row++) {
		tasks[row] = (RenderTask){objects, objectCount, lib, camera, row};
		poolAdd(pool, RenderTaskFunc, &tasks[row]);
	}
	poolWait(pool);
}

static void RunSSRMultiThreaded(const Object *objects, int objectCount, const MaterialLib *lib,
								Camera *camera, ThreadPool *pool, RenderTask *renderTasks,
								SSRTask *ssrTasks, int rowsPerTask) {
	int height = camera->screenHeight;
	int taskCount = (height + rowsPerTask - 1) / rowsPerTask;

	float timeTook[SAMPLES] = {0};
	for (int i = 0; i < SAMPLES; i++) {
		RenderSceneParallel(objects, objectCount, lib, camera, pool, renderTasks);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		SSRPostProcess(camera, pool, ssrTasks, rowsPerTask);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("SSR Multi-threaded (%d rows/task, %d tasks):\n", rowsPerTask, taskCount);
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time:  %f seconds\n", metrics.medianTime);
	printf("Min Time:     %f seconds\n", metrics.minTime);
	printf("Max Time:     %f seconds\n", metrics.maxTime);
	printf("Variance:     %f\n", metrics.variance);
	printf("99th Pct:     %f seconds\n", metrics.p99Time);
}

int main() {
	int objectCount = OBJECT_COUNT;
	Object *objects = malloc(sizeof(Object) * objectCount);
	if (!objects) {
		fprintf(stderr, "Failed to allocate objects\n");
		return 1;
	}

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 16);

	// Wide flat floor — mirror-like (roughness set to 0.05 after creation)
	CreateCube(&objects[0], (float3){0.0f, 0.0f, 12.0f, 0.0f}, (float3){0.0f}, (float3){40.0f, 0.15f, 40.0f, 0.0f}, (float3){0.30f, 0.32f, 0.36f, 0.0f}, &matLib);
	matLib.entries[matLib.count - 1].roughness = 0.05f;

	// Colored cubes above — these will appear in the floor reflection
	CreateCube(&objects[1], (float3){-4.0f, 1.5f, 8.0f, 0.0f}, (float3){0.0f}, (float3){1.5f, 3.0f, 1.5f, 0.0f}, (float3){0.85f, 0.15f, 0.10f, 0.2f}, &matLib);
	CreateCube(&objects[2], (float3){0.0f, 1.8f, 10.0f, 0.0f}, (float3){0.0f}, (float3){1.5f, 3.5f, 1.5f, 0.0f}, (float3){0.15f, 0.75f, 0.20f, 0.2f}, &matLib);
	CreateCube(&objects[3], (float3){4.0f, 1.5f, 8.0f, 0.0f}, (float3){0.0f}, (float3){1.5f, 3.0f, 1.5f, 0.0f}, (float3){0.15f, 0.30f, 0.85f, 0.2f}, &matLib);
	CreateCube(&objects[4], (float3){0.0f, 1.2f, 14.0f, 0.0f}, (float3){0.0f}, (float3){1.5f, 2.5f, 1.5f, 0.0f}, (float3){0.85f, 0.70f, 0.10f, 0.2f}, &matLib);

	printf("Scene loaded. Total triangles: %d\n", Scene_CountTriangles(objects, objectCount));

	// Camera low and near-parallel to the floor — good grazing angle for SSR
	Camera camera;
	initCamera(&camera, 800, 600, 75.0f, (float3){0.0f, 1.2f, -2.0f}, (float3){0.0f, -0.18f, 1.0f}, (float3){5.0f, 8.0f, -4.0f});

	ThreadPool *pool = poolCreate(32, camera.screenHeight);
	RenderTask *renderTasks = malloc(sizeof(RenderTask) * camera.screenHeight);
	SSRTask *ssrTasks = malloc(sizeof(SSRTask) * camera.screenHeight);

	RenderSetup(objects, objectCount, &camera);

	// Render base scene without SSR and save
	RenderSceneParallel(objects, objectCount, &matLib, &camera, pool, renderTasks);
	SaveImage("tests/img/ssr_base.bmp", &camera);

	// ── Single-threaded SSR baseline ──────────────────────────────────────
	float timeTook[SAMPLES] = {0};
	for (int i = 0; i < SAMPLES; i++) {
		RenderSceneParallel(objects, objectCount, &matLib, &camera, pool, renderTasks);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		SSRPostProcessSingleThreaded(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	SaveImage("tests/img/ssr_single.bmp", &camera);
	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("SSR Single-threaded Post-process:\n");
	printf("Average Time: %f seconds\n", metrics.averageTime);
	printf("Median Time:  %f seconds\n", metrics.medianTime);
	printf("Min Time:     %f seconds\n", metrics.minTime);
	printf("Max Time:     %f seconds\n", metrics.maxTime);
	printf("Variance:     %f\n", metrics.variance);
	printf("99th Pct:     %f seconds\n", metrics.p99Time);

	// Build reference (single-threaded) for correctness check
	Camera camRef;
	initCamera(&camRef, 800, 600, 75.0f, (float3){0.0f, 1.2f, -2.0f}, (float3){0.0f, -0.18f, 1.0f}, (float3){5.0f, 8.0f, -4.0f});
	RenderSetup(objects, objectCount, &camRef);
	RenderSceneParallel(objects, objectCount, &matLib, &camRef, pool, renderTasks);
	SSRPostProcessSingleThreaded(&camRef);

	// ── Multi-threaded SSR: sweep rows-per-task ───────────────────────────
	Camera camMT;
	initCamera(&camMT, 800, 600, 75.0f, (float3){0.0f, 1.2f, -2.0f}, (float3){0.0f, -0.18f, 1.0f}, (float3){5.0f, 8.0f, -4.0f});
	RenderSetup(objects, objectCount, &camMT);

	static const int rowsPerTaskValues[] = {1, 2, 4, 16, 32, 64};
	int testCount = (int)(sizeof(rowsPerTaskValues) / sizeof(rowsPerTaskValues[0]));
	for (int t = 0; t < testCount; t++) {
		RunSSRMultiThreaded(objects, objectCount, &matLib, &camMT, pool, renderTasks, ssrTasks, rowsPerTaskValues[t]);
	}
	printf("========================================\n");
	SaveImage("tests/img/ssr_multi.bmp", &camMT);

	// Correctness: final multi result (1-row tasks same as single-threaded scan order)
	Camera camCheck;
	initCamera(&camCheck, 800, 600, 75.0f, (float3){0.0f, 1.2f, -2.0f}, (float3){0.0f, -0.18f, 1.0f}, (float3){5.0f, 8.0f, -4.0f});
	RenderSetup(objects, objectCount, &camCheck);
	RenderSceneParallel(objects, objectCount, &matLib, &camCheck, pool, renderTasks);
	SSRPostProcess(&camCheck, pool, ssrTasks, 1);

	for (int i = 0; i < camRef.screenWidth * camRef.screenHeight; i++) {
		if (camRef.framebuffer[i] != camCheck.framebuffer[i]) {
			printf("Pixel %d mismatch: single = 0x%08X, multi = 0x%08X\n", i, camRef.framebuffer[i], camCheck.framebuffer[i]);
			free(renderTasks);
			free(ssrTasks);
			poolDestroy(pool);
			Scene_Destroy(objects, objectCount);
			MaterialLib_Destroy(&matLib);
			destroyCamera(&camera);
			destroyCamera(&camRef);
			destroyCamera(&camMT);
			destroyCamera(&camCheck);
			return 1;
		}
	}
	printf("Correctness check passed.\n");
	printf("Images saved: tests/img/ssr_base.bmp  tests/img/ssr_single.bmp  tests/img/ssr_multi.bmp\n");

	free(renderTasks);
	free(ssrTasks);
	poolDestroy(pool);
	Scene_Destroy(objects, objectCount);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	destroyCamera(&camRef);
	destroyCamera(&camMT);
	destroyCamera(&camCheck);
	return 0;
}
