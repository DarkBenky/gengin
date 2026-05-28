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

	// V6: 2 boxes SSE with current BVHNode layout (Path A — _mm_setr_ps packing inside)
	int samplesV6 = (SAMPLES / 2) * 2;
	float outV6[2] __attribute__((aligned(16)));
	float *resultsV6 = (float *)malloc(samplesV6 * sizeof(float));
	float *resultsV6_ref = (float *)malloc(samplesV6 * sizeof(float));

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < samplesV6; i += 2) {
		rayAABB_inv_x2_sse(biasArr[i], invRdArr[i],
						   (const float *)&mnArr[i], (const float *)&mxArr[i],
						   (const float *)&mnArr[i + 1], (const float *)&mxArr[i + 1],
						   outV6);
		resultsV6[i] = outV6[0];
		resultsV6[i + 1] = outV6[1];
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV6[0];
	double nsV6_batch = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(samplesV6 / 2);
	double nsV6_per = nsV6_batch / 2.0;

	for (int i = 0; i < samplesV6; i += 2) {
		resultsV6_ref[i] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i].x, &mxArr[i].x);
		resultsV6_ref[i + 1] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i + 1].x, &mxArr[i + 1].x);
	}

	// V7: 2 boxes SSE with pre-packed SoA data (Path B — scalar-store packing included)
	int samplesV7 = (SAMPLES / 2) * 2;
	float soaV7[12] __attribute__((aligned(16)));
	float outV7[2] __attribute__((aligned(16)));
	float *resultsV7 = (float *)malloc(samplesV7 * sizeof(float));
	float *resultsV7_ref = (float *)malloc(samplesV7 * sizeof(float));

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < samplesV7; i += 2) {
		pack_2box_to_soa(
			(const float *)&mnArr[i], (const float *)&mxArr[i],
			(const float *)&mnArr[i + 1], (const float *)&mxArr[i + 1],
			soaV7);
		rayAABB_inv_x2_soa(biasArr[i], invRdArr[i], soaV7, outV7);
		resultsV7[i] = outV7[0];
		resultsV7[i + 1] = outV7[1];
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sink = resultsV7[0];
	double nsV7_batch = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(samplesV7 / 2);
	double nsV7_per = nsV7_batch / 2.0;

	// V7_noPack: V7 compute only — pre-pack all data before timing (ideal Path B)
	// Uses a separate, smaller sample count to keep memory usage reasonable
	int samplesV7np = (SAMPLES / 16) * 2; // smaller set for the pre-packed benchmark
	float *soaPrepacked = (float *)malloc((size_t)(samplesV7np / 2) * 12 * sizeof(float));
	if (soaPrepacked) {
		for (int i = 0; i < samplesV7np; i += 2) {
			pack_2box_to_soa(
				(const float *)&mnArr[i], (const float *)&mxArr[i],
				(const float *)&mnArr[i + 1], (const float *)&mxArr[i + 1],
				soaPrepacked + (i / 2) * 12);
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < samplesV7np; i += 2) {
			rayAABB_inv_x2_soa(biasArr[i], invRdArr[i], soaPrepacked + (i / 2) * 12, outV7);
			resultsV7[i] = outV7[0];
			resultsV7[i + 1] = outV7[1];
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		sink = resultsV7[0];
		double nsV7np_batch = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(samplesV7np / 2);
		double nsV7np_per = nsV7np_batch / 2.0;
		printf("V7 rayAABB_x2_soa  (ideal): %.3f ns/batch(2)  |  %.3f ns/call (normalized)  [pre-packed, no pack cost]\n", nsV7np_batch, nsV7np_per);
		free(soaPrepacked);
	} else {
		printf("V7 rayAABB_x2_soa  (ideal): (skipped — out of memory for prepack buffer)\n");
	}

	for (int i = 0; i < samplesV7; i += 2) {
		resultsV7_ref[i] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i].x, &mxArr[i].x);
		resultsV7_ref[i + 1] = rayAABB_inv(biasArr[i], invRdArr[i], &mnArr[i + 1].x, &mxArr[i + 1].x);
	}

	printf("V5 rayAABB_invV5 (sse  x4):  %.3f ns/batch(4)  |  %.3f ns/call (normalized)\n", nsV5_batch, nsV5_per);
	printf("V6 rayAABB_x2_sse  (cur):   %.3f ns/batch(2)  |  %.3f ns/call (normalized)\n", nsV6_batch, nsV6_per);
	printf("V7 rayAABB_x2_soa  (+pack): %.3f ns/batch(2)  |  %.3f ns/call (normalized)\n", nsV7_batch, nsV7_per);

	int mismatchesV2 = 0, mismatchesV3 = 0, mismatchesV4 = 0, mismatchesV5 = 0;
	int mismatchesV6 = 0, mismatchesV7 = 0;
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

	for (int i = 0; i < samplesV6; i++) {
		float v6 = resultsV6[i], ref6 = resultsV6_ref[i];
		int m6 = (ref6 == FLT_MAX) != (v6 == FLT_MAX);
		if (!m6 && ref6 != FLT_MAX) m6 = fabsf(ref6 - v6) > 1e-5f * (fabsf(ref6) + 1.0f);
		mismatchesV6 += m6;
	}
	for (int i = 0; i < samplesV7; i++) {
		float v7 = resultsV7[i], ref7 = resultsV7_ref[i];
		int m7 = (ref7 == FLT_MAX) != (v7 == FLT_MAX);
		if (!m7 && ref7 != FLT_MAX) m7 = fabsf(ref7 - v7) > 1e-5f * (fabsf(ref7) + 1.0f);
		mismatchesV7 += m7;
	}
	printf("Mismatches V6 vs ref: %d / %d\n", mismatchesV6, samplesV6);
	printf("Mismatches V7 vs ref: %d / %d\n", mismatchesV7, samplesV7);

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
	free(resultsV6);
	free(resultsV6_ref);
	free(resultsV7);
	free(resultsV7_ref);
	return 0;
}
