#include "rayTriangle.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#define SAMPLES 200000000

int main() {
	float3 *rayDir = malloc(SAMPLES * sizeof(float3));
	float3 *rayOrigin = malloc(SAMPLES * sizeof(float3));
	float3 *v0 = malloc(SAMPLES * sizeof(float3));
	float3 *v1 = malloc(SAMPLES * sizeof(float3));
	float3 *v2 = malloc(SAMPLES * sizeof(float3));
	float *tOutOld = malloc(SAMPLES * sizeof(float));
	float *tOutNew = malloc(SAMPLES * sizeof(float));
	float *tOutNewV2 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV3 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV4 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV5 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV6 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV7 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV8 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV9 = malloc(SAMPLES * sizeof(float));
	float *tOutNewV10 = malloc(SAMPLES * sizeof(float));
	memset(tOutOld, 0, SAMPLES * sizeof(float));
	memset(tOutNew, 0, SAMPLES * sizeof(float));
	memset(tOutNewV2, 0, SAMPLES * sizeof(float));
	memset(tOutNewV3, 0, SAMPLES * sizeof(float));
	memset(tOutNewV4, 0, SAMPLES * sizeof(float));
	memset(tOutNewV5, 0, SAMPLES * sizeof(float));
	memset(tOutNewV6, 0, SAMPLES * sizeof(float));
	memset(tOutNewV7, 0, SAMPLES * sizeof(float));
	memset(tOutNewV8, 0, SAMPLES * sizeof(float));
	memset(tOutNewV9, 0, SAMPLES * sizeof(float));
	memset(tOutNewV10, 0, SAMPLES * sizeof(float));

	for (int i = 0; i < SAMPLES; i++) {
		rayDir[i] = (float3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		rayOrigin[i] = (float3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		v0[i] = (float3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		v1[i] = (float3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		v2[i] = (float3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		rayTriangle(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i], &tOutOld[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsOld = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("========================================\n");
	printf("Old rayTriangle: %.3f ns/call\n", nsOld);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		rayTriangleNew(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i], &tOutNew[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNew = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New rayTriangle: %.3f ns/call\n", nsNew);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV2[i] = rayTriangleNewV2(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV2 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V2 rayTriangle: %.3f ns/call\n", nsNewV2);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV3[i] = rayTriangleNewV3(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV3 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V3 rayTriangle: %.3f ns/call\n", nsNewV3);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV4[i] = rayTriangleNewV4(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV4 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V4 rayTriangle (scalar fields): %.3f ns/call\n", nsNewV4);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV5[i] = rayTriangleNewV5(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV5 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V5 rayTriangle (speculative q): %.3f ns/call\n", nsNewV5);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV6[i] = rayTriangleNewV6(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV6 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V6 rayTriangle (combined branch): %.3f ns/call\n", nsNewV6);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV7[i] = rayTriangleNewV7(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV7 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V7 rayTriangle (SSE): %.3f ns/call\n", nsNewV7);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV8[i] = rayTriangleNewV8(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV8 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V8 rayTriangle (early uNum): %.3f ns/call\n", nsNewV8);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV9[i] = rayTriangleNewV9(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV9 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V9 rayTriangle (branch hints): %.3f ns/call\n", nsNewV9);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++) {
		tOutNewV10[i] = rayTriangleNewV10(rayOrigin[i], rayDir[i], v0[i], v1[i], v2[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double nsNewV10 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	printf("New V10 rayTriangle (FMA): %.3f ns/call\n", nsNewV10);

	int mismatches = 0;
	for (int i = 0; i < SAMPLES; i++) {
		float tOld = tOutOld[i];
		float tNew = tOutNew[i];
		float tNewV2 = tOutNewV2[i];
		float tNewV3 = tOutNewV3[i];
		float tNewV4 = tOutNewV4[i];
		float tNewV5 = tOutNewV5[i];
		float tNewV6 = tOutNewV6[i];
		float tNewV7 = tOutNewV7[i];
		float tNewV8 = tOutNewV8[i];
		float tNewV9 = tOutNewV9[i];
		float tNewV10 = tOutNewV10[i];
		if (fabsf(tOld - tNew) > 1e-5f || fabsf(tOld - tNewV2) > 1e-5f ||
			fabsf(tOld - tNewV3) > 1e-5f || fabsf(tOld - tNewV4) > 1e-5f ||
			fabsf(tOld - tNewV5) > 1e-5f || fabsf(tOld - tNewV6) > 1e-5f ||
			fabsf(tOld - tNewV7) > 1e-5f || fabsf(tOld - tNewV8) > 1e-5f ||
			fabsf(tOld - tNewV9) > 1e-5f || fabsf(tOld - tNewV10) > 1e-5f) {
			mismatches++;
			printf("Mismatch at index %d:\n", i);
			printf("  Old: %.6f\n", tOld);
			printf("  New: %.6f\n", tNew);
			printf("  NewV2: %.6f\n", tNewV2);
			printf("  NewV3: %.6f\n", tNewV3);
			printf("  NewV4: %.6f\n", tNewV4);
			printf("  NewV5: %.6f\n", tNewV5);
			printf("  NewV6: %.6f\n", tNewV6);
			printf("  NewV7: %.6f\n", tNewV7);
			printf("  NewV8: %.6f\n", tNewV8);
			printf("  NewV9: %.6f\n", tNewV9);
			printf("  NewV10: %.6f\n", tNewV10);
		}
	}

	printf("Mismatches: %d / %d\n", mismatches, SAMPLES);

	free(rayDir);
	free(rayOrigin);
	free(v0);
	free(v1);
	free(v2);
	free(tOutOld);
	free(tOutNew);
	free(tOutNewV2);
	free(tOutNewV3);
	free(tOutNewV4);
	free(tOutNewV5);
	free(tOutNewV6);
	free(tOutNewV7);
	free(tOutNewV8);
	free(tOutNewV9);
	free(tOutNewV10);
	return 0;
}

// TODO: use V10 insted of current implementation extra 4% speedup
// Old rayTriangle: 18.899 ns/call
// New rayTriangle: 21.062 ns/call
// New V2 rayTriangle: 21.303 ns/call
// New V3 rayTriangle: 18.561 ns/call
// New V4 rayTriangle (scalar fields): 18.198 ns/call
// New V5 rayTriangle (speculative q): 18.596 ns/call
// New V6 rayTriangle (combined branch): 18.227 ns/call
// New V7 rayTriangle (SSE): 19.777 ns/call
// New V8 rayTriangle (early uNum): 18.546 ns/call
// New V9 rayTriangle (branch hints): 19.348 ns/call
// New V10 rayTriangle (FMA): 18.173 ns/call

