#include "RayBoxItersect.h"
#include <time.h>

#define SAMPLES 100000

int main() {
	Object *objects = malloc(sizeof(Object) * SAMPLES);
	for (int i = 0; i < SAMPLES; i++) {
		objects[i].worldBBmin = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		objects[i].worldBBmax = (float3){objects[i].worldBBmin.x + rand() / (float)RAND_MAX,
										 objects[i].worldBBmin.y + rand() / (float)RAND_MAX,
										 objects[i].worldBBmin.z + rand() / (float)RAND_MAX};
	}

	float3 *rayOrigins = malloc(sizeof(float3) * SAMPLES);
	float3 *rayDirs = malloc(sizeof(float3) * SAMPLES);
	for (int i = 0; i < SAMPLES; i++) {
		rayOrigins[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		rayDirs[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
	}

	float2 *results = malloc(sizeof(float2) * SAMPLES);
	float4 *resultsV2 = malloc(sizeof(float4) * SAMPLES / 2);
	RayBoxResult4 *resultsV4 = malloc(sizeof(RayBoxResult4) * (SAMPLES / 4));

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		float tMin, tMax;
		RayBoxItersectOld(&objects[i], rayOrigins[i], rayDirs[i], &tMin, &tMax);
		results[i] = (float2){tMin, tMax};
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeOld = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i += 2) {
		RayBoxResult2 res = RayBoxIntersectV2(&objects[i], &objects[i + 1], rayOrigins[i], rayDirs[i]);
		resultsV2[i / 2] = (float4){res.tMin0, res.tMax0, res.tMin1, res.tMax1};
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeV2 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i += 4) {
		resultsV4[i / 4] = RayBoxIntersectV4(&objects[i], &objects[i + 1], &objects[i + 2], &objects[i + 3], rayOrigins[i], rayDirs[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeV4 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("========================================\n");
	printf("RayBoxItersectOld time: %.3f ns per box\n", timeOld);
	printf("RayBoxItersectV2  time: %.3f ns per box\n", timeV2);
	printf("RayBoxItersectV4  time: %.3f ns per box\n", timeV4);
	printf("Speedup V2: %.2fx\n", timeOld / timeV2);
	printf("Speedup V4: %.2fx\n", timeOld / timeV4);

	// Distribution check: with uniform random data all three should hit the same
	// fraction of boxes. V2/V4 test each group against one shared ray so per-box
	// results differ from old, but the hit rate converges statistically.
	int hitsOld = 0, hitsV2 = 0, hitsV4 = 0;
	for (int i = 0; i < SAMPLES; i++)
		hitsOld += (results[i].x != FLT_MAX);
	for (int i = 0; i < SAMPLES / 2; i++)
		hitsV2 += (resultsV2[i].x != FLT_MAX) + (resultsV2[i].z != FLT_MAX);
	for (int i = 0; i < SAMPLES / 4; i++)
		for (int k = 0; k < 4; k++)
			hitsV4 += (resultsV4[i].tMin[k] != FLT_MAX);
	printf("Hit rate -- old: %.2f%%  V2: %.2f%%  V4: %.2f%%\n",
		100.0 * hitsOld / SAMPLES,
		100.0 * hitsV2  / SAMPLES,
		100.0 * hitsV4  / SAMPLES);

	free(objects);
	free(results);
	free(resultsV2);
	free(resultsV4);
}

// TODO: use V4 insted of current implementation
// Running: tests/RayBoxItersect
// RayBoxItersectOld time: 33.867 ns per box
// RayBoxItersectV2  time: 30.182 ns per box
// RayBoxItersectV4  time: 17.644 ns per box
// Speedup V2: 1.12x
// Speedup V4: 1.92x
// Hit rate -- old: 19.69%  V2: 19.66%  V4: 19.78%
