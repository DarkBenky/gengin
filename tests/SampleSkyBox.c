#include "SampleSkyBox.h"
#include <math.h>
#include <stdio.h>
#include "../render/color/color.h"

#define SAMPLES 100000

int main() {
	Skybox skybox;
	LoadSkybox(&skybox, "../skybox");

	float *timeTookOld = (float *)malloc(SAMPLES * sizeof(float));
	float *timeTookAVX = (float *)malloc(SAMPLES * sizeof(float));
	float *timeTookAVX8 = (float *)malloc(SAMPLES * sizeof(float));

	Color *resultsOld = (Color *)malloc(SAMPLES * sizeof(Color));
	Color *resultsAVX = (Color *)malloc(SAMPLES * sizeof(Color));
	Color *resultsAVX8 = (Color *)malloc(SAMPLES * sizeof(Color));

	float3 *testRays = (float3 *)malloc(SAMPLES * sizeof(float3));

	for (int i = 0; i < SAMPLES; i++) {
		testRays[i] = (float3){
			(float)rand() / RAND_MAX * 2.0f - 1.0f,
			(float)rand() / RAND_MAX * 2.0f - 1.0f,
			(float)rand() / RAND_MAX * 2.0f - 1.0f};
	}

	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		resultsOld[i] = SampleSkybox(&skybox, testRays[i]);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTookOld[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTookOld, SAMPLES);
	printf("========================================\n");
	printf("Old Method Performance:\n");
	printf("Average Time: %.2f ns\n", metrics.averageTime * 1e9f);
	printf("Median Time:  %.2f ns\n", metrics.medianTime * 1e9f);
	printf("Min Time:     %.2f ns\n", metrics.minTime * 1e9f);
	printf("Max Time:     %.2f ns\n", metrics.maxTime * 1e9f);
	printf("Variance:     %.2f\n", metrics.variance * 1e18f);
	printf("99th Pct:     %.2f ns\n", metrics.p99Time * 1e9f);

	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		resultsAVX[i] = SampleSkyboxAVX(&skybox, testRays[i]);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTookAVX[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	metrics = ComputePerformanceMetrics(timeTookAVX, SAMPLES);
	printf("========================================\n");
	printf("AVX Method Performance:\n");
	printf("Average Time: %.2f ns\n", metrics.averageTime * 1e9f);
	printf("Median Time:  %.2f ns\n", metrics.medianTime * 1e9f);
	printf("Min Time:     %.2f ns\n", metrics.minTime * 1e9f);
	printf("Max Time:     %.2f ns\n", metrics.maxTime * 1e9f);
	printf("Variance:     %.2f\n", metrics.variance * 1e18f);
	printf("99th Pct:     %.2f ns\n", metrics.p99Time * 1e9f);

	for (int i = 0; i < SAMPLES; i += 8) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		float dirs_x[8], dirs_y[8], dirs_z[8];
		// TODO: use this version it is match faster from Median Time:  30.00 ns to Median Time (per sample):  6.25 ns
		for (int j = 0; j < 8; j++) {
			dirs_x[j] = testRays[i + j].x;
			dirs_y[j] = testRays[i + j].y;
			dirs_z[j] = testRays[i + j].z;
		}
		SampleSkybox8(&skybox, dirs_x, dirs_y, dirs_z, &resultsAVX8[i]);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTookAVX8[i / 8] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	metrics = ComputePerformanceMetrics(timeTookAVX8, SAMPLES / 8);
	printf("========================================\n");
	printf("AVX8 Method Performance:\n");
	printf("Average Time (per sample): %.2f ns\n", metrics.averageTime * 1e9f / 8.0f);
	printf("Median Time (per sample):  %.2f ns\n", metrics.medianTime * 1e9f / 8.0f);
	printf("Min Time (per sample):     %.2f ns\n", metrics.minTime * 1e9f / 8.0f);
	printf("Max Time (per sample):     %.2f ns\n", metrics.maxTime * 1e9f / 8.0f);
	printf("Variance (per sample):     %.2f\n", metrics.variance * 1e18f / 64.0f);
	printf("99th Pct (per sample):     %.2f ns\n", metrics.p99Time * 1e9f / 8.0f);
	printf("========================================\n");

	// validate results
	int mismatch_countAVX = 0;
	int mismatch_countAVX8 = 0;
	for (int i = 0; i < SAMPLES; i++) {
		Color Old = resultsOld[i];
		Color AVX = resultsAVX[i];
		Color AVX8 = resultsAVX8[i / 8];

		float3 oldF = UnpackColor(Old);
		float3 avxF = UnpackColor(AVX);
		float3 avx8F = UnpackColor(AVX8);

		const float epsilon = 0.004f; // one channel can differ by up to 1/255 ≈ 0.0039 due to rounding, so we allow a small margin
		if (fabsf(oldF.x - avxF.x) > 0.004 || fabsf(oldF.y - avxF.y) > 0.004 || fabsf(oldF.z - avxF.z) > 0.004) {
			mismatch_countAVX++;
			if (mismatch_countAVX <= 10) {
				printf("Mismatch AVX at index %d: Old=0x%08X, AVX=0x%08X\n", i, Old, AVX);
			}
		}
		if (fabsf(oldF.x - avx8F.x) > epsilon || fabsf(oldF.y - avx8F.y) > epsilon || fabsf(oldF.z - avx8F.z) > epsilon) {
			mismatch_countAVX8++;
			if (mismatch_countAVX8 <= 10) {
				printf("Mismatch AVX8 at index %d: Old=0x%08X, AVX8=0x%08X\n", i, Old, AVX8);
			}
		}
	}
	printf("Total mismatches AVX: %d out of %d\n", mismatch_countAVX, SAMPLES);
	printf("Total mismatches AVX8: %d out of %d\n", mismatch_countAVX8, SAMPLES);

	free(timeTookOld);
	free(timeTookAVX);
	free(timeTookAVX8);
	free(resultsOld);
	free(resultsAVX);
	free(resultsAVX8);
	free(testRays);
	DestroySkybox(&skybox);
}