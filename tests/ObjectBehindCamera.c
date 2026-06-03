// NO LONGER USED

#include "ObjectBehindCamera.h"

#define SAMPLES 100000

int main() {
	Object *objects = malloc(sizeof(Object) * SAMPLES);
	bool *results = malloc(sizeof(bool) * SAMPLES);
	Bool2 *resultsV2 = malloc(sizeof(Bool2) * SAMPLES / 2);
	Bool4 *resultsV4 = malloc(sizeof(Bool4) * SAMPLES / 4);

	for (int i = 0; i < SAMPLES; i++) {
		objects[i].worldBBmin = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		objects[i].worldBBmax = (float3){objects[i].worldBBmin.x + rand() / (float)RAND_MAX,
										 objects[i].worldBBmin.y + rand() / (float)RAND_MAX,
										 objects[i].worldBBmin.z + rand() / (float)RAND_MAX};
	}

	float3 *camPos = (float3 *)malloc(sizeof(float3) * SAMPLES);
	float3 *camForward = (float3 *)malloc(sizeof(float3) * SAMPLES);

	for (int i = 0; i < SAMPLES; i++) {
		camPos[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		camForward[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		results[i] = ObjectBehindCameraOld(&objects[i], camPos[i], camForward[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeScalar = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i += 2) {
		Bool2 res = ObjectBehindCameraV2(&objects[i], &objects[i + 1], camPos[i], camForward[i]);
		resultsV2[i / 2] = res;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeV2 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i += 4) {
		Bool4 res = ObjectBehindCameraV4(&objects[i], &objects[i + 1], &objects[i + 2], &objects[i + 3], camPos[i], camForward[i]);
		resultsV4[i / 4] = res;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double timeV4 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("========================================\n");
	printf("ObjectBehindCamera scalar time: %.3f ns per object\n", timeScalar);
	printf("ObjectBehindCameraV2 time: %.3f ns per object\n", timeV2);
	printf("ObjectBehindCameraV4 time: %.3f ns per object\n", timeV4);
	printf("Speedup V2: %.2fx\n", timeScalar / timeV2);
	printf("Speedup V4: %.2fx\n", timeScalar / timeV4);

	int hitScalar = 0, hitV2 = 0, hitV4 = 0;
	for (int i = 0; i < SAMPLES; i++)
		hitScalar += results[i];
	for (int i = 0; i < SAMPLES / 2; i++)
		hitV2 += resultsV2[i].b0 + resultsV2[i].b1;
	for (int i = 0; i < SAMPLES / 4; i++)
		hitV4 += resultsV4[i].b0 + resultsV4[i].b1 + resultsV4[i].b2 + resultsV4[i].b3;

	printf("Behind rate scalar: %.2f%%\n", 100.0 * hitScalar / SAMPLES);
	printf("Behind rate V2:     %.2f%%\n", 100.0 * hitV2 / SAMPLES);
	printf("Behind rate V4:     %.2f%%\n", 100.0 * hitV4 / SAMPLES);

	free(objects);
	free(results);
	free(resultsV2);
	free(resultsV4);
	return 0;
}
