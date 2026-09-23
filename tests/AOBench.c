#include "timings.h"
#include "../render/cpu/AO.h"

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

static void FillSyntheticScene(Camera *camera) {
	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const float halfW = (float)width * 0.5f;
	const float halfH = (float)height * 0.5f;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const int idx = y * width + x;

			const float u = ((float)x - halfW) * 0.05f;
			const float v = ((float)y - halfH) * 0.05f;

			const float h = 6.0f * sinf(u * 0.9f) * cosf(v * 1.1f)
			              + 2.0f * sinf((u + v) * 2.3f);
			const float dx = 5.4f * cosf(u * 0.9f) * cosf(v * 1.1f) + 4.6f * cosf((u + v) * 2.3f);
			const float dz = -6.6f * sinf(u * 0.9f) * sinf(v * 1.1f) + 4.6f * cosf((u + v) * 2.3f);

			const int sky = ((x / 64 + y / 64) % 5) == 0;

			float3 n = Float3_Normalize((float3){-dx, 1.0f, -dz});
			camera->positionBuffer[idx] = (float3){u, h, v};
			camera->normalBuffer[idx] = n;
			camera->depthBuffer[idx] = sky ? DEPTH_FAR : 30.0f + v * 0.25f;
		}
	}

	camera->fovScale = tanf(camera->fov * 0.5f * 3.14159265f / 180.0f);
	camera->aspect = (float)camera->screenWidth / (float)camera->screenHeight;
}

int main(void) {
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 60.0f,
	           (float3){0.0f, 0.0f, -30.0f},
	           (float3){0.0f, 0.0f, 1.0f},
	           (float3){0.3f, -0.8f, 0.2f});
	FillSyntheticScene(&camera);

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	ThreadPool *pool = poolCreate(ncpu > 1 ? (int)ncpu - 1 : 1, HEIGHT);

	float timesSt[SAMPLES], timesMp[SAMPLES], timesV2St[SAMPLES], timesV2Mp[SAMPLES];
	float timesV2PlusSt[SAMPLES], timesV2PlusMp[SAMPLES];
	float timesV2PlusColSt[SAMPLES], timesV2PlusColMp[SAMPLES];
	float timesV2PlusColSgMp[SAMPLES];
	float timesV2PlusColSkipMp[SAMPLES];
	float timesV3St[SAMPLES], timesV3Mp[SAMPLES];

	CalculateAmbientOcclusion(&camera);
	CalculateAmbientOcclusionMp(&camera, pool);
	CalculateAmbientOcclusionV2(&camera);
	CalculateAmbientOcclusionV2Mp(&camera, pool);
	CalculateAmbientOcclusionV2Plus(&camera);
	CalculateAmbientOcclusionV2PlusMp(&camera, pool);
	CalculateAmbientOcclusionV2PlusColumn(&camera);
	CalculateAmbientOcclusionV2PlusColumnMp(&camera, pool);
	CalculateAmbientOcclusionV2PlusColumnSgMp(&camera, pool);
	CalculateAmbientOcclusionV2PlusColumnMpPixelSkip(&camera, pool);
	CalculateAmbientOcclusionV3(&camera);
	CalculateAmbientOcclusionV3Mp(&camera, pool);

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusion(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesSt[s] = (float)(t1.tv_sec - t0.tv_sec)
		           + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionMp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesMp[s] = (float)(t1.tv_sec - t0.tv_sec)
		           + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2St[s] = (float)(t1.tv_sec - t0.tv_sec)
		             + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2Mp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2Mp[s] = (float)(t1.tv_sec - t0.tv_sec)
		             + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2Plus(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusSt[s] = (float)(t1.tv_sec - t0.tv_sec)
		                 + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2PlusMp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusMp[s] = (float)(t1.tv_sec - t0.tv_sec)
		                 + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2PlusColumn(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusColSt[s] = (float)(t1.tv_sec - t0.tv_sec)
		                    + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2PlusColumnMp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusColMp[s] = (float)(t1.tv_sec - t0.tv_sec)
		                    + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2PlusColumnSgMp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusColSgMp[s] = (float)(t1.tv_sec - t0.tv_sec)
		                       + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV2PlusColumnMpPixelSkip(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV2PlusColSkipMp[s] = (float)(t1.tv_sec - t0.tv_sec)
		                         + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV3(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV3St[s] = (float)(t1.tv_sec - t0.tv_sec)
		             + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	for (int s = 0; s < SAMPLES; s++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		CalculateAmbientOcclusionV3Mp(&camera, pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timesV3Mp[s] = (float)(t1.tv_sec - t0.tv_sec)
		             + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	PerformanceMetrics mSt = ComputePerformanceMetrics(timesSt, SAMPLES);
	PerformanceMetrics mMps = ComputePerformanceMetrics(timesMp, SAMPLES);
	PerformanceMetrics mV2St = ComputePerformanceMetrics(timesV2St, SAMPLES);
	PerformanceMetrics mV2Mp = ComputePerformanceMetrics(timesV2Mp, SAMPLES);
	PerformanceMetrics mV2PlusSt = ComputePerformanceMetrics(timesV2PlusSt, SAMPLES);
	PerformanceMetrics mV2PlusMp = ComputePerformanceMetrics(timesV2PlusMp, SAMPLES);
	PerformanceMetrics mV2PlusColSt = ComputePerformanceMetrics(timesV2PlusColSt, SAMPLES);
	PerformanceMetrics mV2PlusColMp = ComputePerformanceMetrics(timesV2PlusColMp, SAMPLES);
	PerformanceMetrics mV2PlusColSgMp = ComputePerformanceMetrics(timesV2PlusColSgMp, SAMPLES);
	PerformanceMetrics mV2PlusColSkipMp = ComputePerformanceMetrics(timesV2PlusColSkipMp, SAMPLES);
	PerformanceMetrics mV3St = ComputePerformanceMetrics(timesV3St, SAMPLES);
	PerformanceMetrics mV3Mp = ComputePerformanceMetrics(timesV3Mp, SAMPLES);

	printf("=== AOBench: AO over %dx%d, %d samples ===\n",
	       WIDTH, HEIGHT, SAMPLES);
	printf("%-20s %8s  %10s  %9s  %7s\n", "Benchmark", "avg(ms)", "median(ms)", "p99(ms)", "speedup");
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "V1 Single", mSt.averageTime * 1e3f, mSt.medianTime * 1e3f, mSt.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V1 Multi", mMps.averageTime * 1e3f, mMps.medianTime * 1e3f, mMps.p99Time * 1e3f,
	       mSt.medianTime / mMps.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "V2 Single", mV2St.averageTime * 1e3f, mV2St.medianTime * 1e3f, mV2St.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V2 Multi", mV2Mp.averageTime * 1e3f, mV2Mp.medianTime * 1e3f, mV2Mp.p99Time * 1e3f,
	       mV2St.medianTime / mV2Mp.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "V2Plus Single", mV2PlusSt.averageTime * 1e3f, mV2PlusSt.medianTime * 1e3f, mV2PlusSt.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V2Plus Multi", mV2PlusMp.averageTime * 1e3f, mV2PlusMp.medianTime * 1e3f, mV2PlusMp.p99Time * 1e3f,
	       mV2PlusSt.medianTime / mV2PlusMp.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "V2PlusCol Single", mV2PlusColSt.averageTime * 1e3f, mV2PlusColSt.medianTime * 1e3f, mV2PlusColSt.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V2PlusCol Multi", mV2PlusColMp.averageTime * 1e3f, mV2PlusColMp.medianTime * 1e3f, mV2PlusColMp.p99Time * 1e3f,
	       mV2PlusColSt.medianTime / mV2PlusColMp.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V2PlusColSg Multi", mV2PlusColSgMp.averageTime * 1e3f, mV2PlusColSgMp.medianTime * 1e3f, mV2PlusColSgMp.p99Time * 1e3f,
	       mV2PlusColSt.medianTime / mV2PlusColSgMp.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V2PlusColSkip Multi", mV2PlusColSkipMp.averageTime * 1e3f, mV2PlusColSkipMp.medianTime * 1e3f, mV2PlusColSkipMp.p99Time * 1e3f,
	       mV2PlusColMp.medianTime / mV2PlusColSkipMp.medianTime);
	printf("%-20s %8.3f  %10.3f  %9.3f  %7s\n",
	       "V3 Single", mV3St.averageTime * 1e3f, mV3St.medianTime * 1e3f, mV3St.p99Time * 1e3f, "-");
	printf("%-20s %8.3f  %10.3f  %9.3f  %6.2fx\n",
	       "V3 Multi", mV3Mp.averageTime * 1e3f, mV3Mp.medianTime * 1e3f, mV3Mp.p99Time * 1e3f,
	       mV3St.medianTime / mV3Mp.medianTime);

	// The fastHash keeps global call state now: per-pixel patterns depend on the
	// call order, so ST vs MP (and any cross-variant pair) no longer produce the
	// same image, and MP's interleaving is scheduling-dependent. Cross-run
	// equality checks are meaningless here; fastHashReset() makes a run repeatable.
	int mismatches = 0;

	// Same path + reset must reproduce bit-for-bit.
	fastHashReset();
	CalculateAmbientOcclusion(&camera);
	memcpy(camera.tempBuffer_2, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	fastHashReset();
	CalculateAmbientOcclusion(&camera);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_2[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (mismatches < 5)
				printf("V1 REPEAT MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_2[i], camera.ambientOcclusionBuffer[i]);
			mismatches++;
		}
	}

	if (mismatches)
		printf("CORRECTNESS FAIL: %d mismatching pixels\n", mismatches);
	else
		printf("Correctness: OK (stateful hash: same path reproduces after reset)\n");

	poolDestroy(pool);
	destroyCamera(&camera);
	return mismatches != 0;
}
