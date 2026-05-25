#include "rayAABB_inv.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <float.h>

#define SAMPLES 99900000

static volatile float sink = 0.0f;

int main() {
	float3 *biasArr = (float3 *)malloc(SAMPLES * sizeof(float3));
	float3 *invRdArr = (float3 *)malloc(SAMPLES * sizeof(float3));
	float3 *mnArr = (float3 *)malloc(SAMPLES * sizeof(float3));
	float3 *mxArr = (float3 *)malloc(SAMPLES * sizeof(float3));
	float *resultsV1 = (float *)malloc(SAMPLES * sizeof(float));
	float *resultsV2 = (float *)malloc(SAMPLES * sizeof(float));
	float *resultsV3 = (float *)malloc(SAMPLES * sizeof(float));

	for (int i = 0; i < SAMPLES; i++) {
		biasArr[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		invRdArr[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		mnArr[i] = (float3){rand() / (float)RAND_MAX, rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
		mxArr[i] = (float3){mnArr[i].x + rand() / (float)RAND_MAX,
							mnArr[i].y + rand() / (float)RAND_MAX,
							mnArr[i].z + rand() / (float)RAND_MAX};
	}

	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++)
		resultsV1[i] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i].x, &mxArr[i].x);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV1[0];
	double nsV1 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++)
		resultsV2[i] = rayAABB_invV2(biasArr[i], invRdArr[i], mnArr[i], mxArr[i]);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV2[0];
	double nsV2 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < SAMPLES; i++)
		resultsV3[i] = rayAABB_invV3(biasArr[i], invRdArr[i], mnArr[i], mxArr[i]);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV3[0];
	double nsV3 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)SAMPLES;

	// V4: 1 ray vs 8 boxes per call
	int samplesV4 = (SAMPLES / 8) * 8;
	float outV4[8] __attribute__((aligned(32)));
	float *resultsV4 = (float *)malloc(samplesV4 * sizeof(float));
	// reference: scalar V1 with the same ray V4 uses per batch
	float *resultsV4_ref = (float *)malloc(samplesV4 * sizeof(float));

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < samplesV4; i += 8) {
		rayAABB_invV4_avx2(biasArr[i], invRdArr[i], &mnArr[i], &mxArr[i], outV4);
		for (int j = 0; j < 8; j++)
			resultsV4[i + j] = outV4[j];
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV4[0];
	double nsV4_batch = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(samplesV4 / 8);
	double nsV4_per = nsV4_batch / 8.0;

	for (int i = 0; i < samplesV4; i += 8)
		for (int j = 0; j < 8; j++)
			resultsV4_ref[i + j] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i + j].x, &mxArr[i + j].x);

	printf("========================================\n");
	printf("V1 rayAABB_inv   (pointers): %.3f ns/call\n", nsV1);
	printf("V2 rayAABB_invV2 (float3):   %.3f ns/call\n", nsV2);
	printf("V3 rayAABB_invV3 (fma):      %.3f ns/call\n", nsV3);
	printf("V4 rayAABB_invV4 (avx2 x8):  %.3f ns/batch(8)  |  %.3f ns/call (normalized)\n", nsV4_batch, nsV4_per);

	// V5: 1 ray vs 4 boxes per call (SSE)
	int samplesV5 = (SAMPLES / 4) * 4;
	float outV5[4] __attribute__((aligned(16)));
	float *resultsV5 = (float *)malloc(samplesV5 * sizeof(float));
	float *resultsV5_ref = (float *)malloc(samplesV5 * sizeof(float));

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < samplesV5; i += 4) {
		rayAABB_invV5_sse_x4(biasArr[i], invRdArr[i], &mnArr[i], &mxArr[i], outV5);
		for (int j = 0; j < 4; j++)
			resultsV5[i + j] = outV5[j];
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV5[0];
	double nsV5_batch = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(samplesV5 / 4);
	double nsV5_per = nsV5_batch / 4.0;

	for (int i = 0; i < samplesV5; i += 4)
		for (int j = 0; j < 4; j++)
			resultsV5_ref[i + j] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i + j].x, &mxArr[i + j].x);

	printf("V5 rayAABB_invV5 (sse  x4):  %.3f ns/batch(4)  |  %.3f ns/call (normalized)\n", nsV5_batch, nsV5_per);

	int mismatchesV2 = 0, mismatchesV3 = 0, mismatchesV4 = 0, mismatchesV5 = 0;
	for (int i = 0; i < samplesV5; i++) {
		float v1 = resultsV1[i], v2 = resultsV2[i], v3 = resultsV3[i];
		float v4 = (i < samplesV4) ? resultsV4[i] : resultsV4_ref[i];
		float ref4 = resultsV4_ref[i];
		float v5 = resultsV5[i], ref5 = resultsV5_ref[i];
		int m2 = (v1 == FLT_MAX) != (v2 == FLT_MAX);
		int m3 = (v1 == FLT_MAX) != (v3 == FLT_MAX);
		int m4 = (ref4 == FLT_MAX) != (v4 == FLT_MAX);
		int m5 = (ref5 == FLT_MAX) != (v5 == FLT_MAX);
		if (!m2 && v1 != FLT_MAX) m2 = fabsf(v1 - v2) > 1e-5f * (fabsf(v1) + 1.0f);
		if (!m3 && v1 != FLT_MAX) m3 = fabsf(v1 - v3) > 1e-5f * (fabsf(v1) + 1.0f);
		if (!m4 && ref4 != FLT_MAX) m4 = fabsf(ref4 - v4) > 1e-5f * (fabsf(ref4) + 1.0f);
		if (!m5 && ref5 != FLT_MAX) m5 = fabsf(ref5 - v5) > 1e-5f * (fabsf(ref5) + 1.0f);
		mismatchesV2 += m2;
		mismatchesV3 += m3;
		mismatchesV4 += m4;
		mismatchesV5 += m5;
	}
	printf("========================================\n");
	printf("Validation (same inputs, relative eps 1e-5):\n");
	printf("Mismatches V1 vs V2:  %d / %d\n", mismatchesV2, samplesV5);
	printf("Mismatches V1 vs V3:  %d / %d\n", mismatchesV3, samplesV5);
	printf("Mismatches V4 vs ref: %d / %d\n", mismatchesV4, samplesV5);
	printf("Mismatches V5 vs ref: %d / %d\n", mismatchesV5, samplesV5);

	free(biasArr);
	free(invRdArr);
	free(mnArr);
	free(mxArr);
	free(resultsV1);
	free(resultsV2);
	free(resultsV3);
	free(resultsV4);
	free(resultsV4_ref);
	free(resultsV5);
	free(resultsV5_ref);
	return 0;
}
