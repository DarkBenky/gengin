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

	CalculateAmbientOcclusion(&camera);
	CalculateAmbientOcclusionMp(&camera, pool);
	CalculateAmbientOcclusionV2(&camera);
	CalculateAmbientOcclusionV2Mp(&camera, pool);

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

	PerformanceMetrics mSt = ComputePerformanceMetrics(timesSt, SAMPLES);
	PerformanceMetrics mMps = ComputePerformanceMetrics(timesMp, SAMPLES);
	PerformanceMetrics mV2St = ComputePerformanceMetrics(timesV2St, SAMPLES);
	PerformanceMetrics mV2Mp = ComputePerformanceMetrics(timesV2Mp, SAMPLES);

	printf("=== AOBench: AO over %dx%d, %d samples ===\n",
	       WIDTH, HEIGHT, SAMPLES);
	printf("V1 Single avg=%.3fms  median=%.3fms  p99=%.3fms\n",
	       mSt.averageTime * 1e3f, mSt.medianTime * 1e3f, mSt.p99Time * 1e3f);
	printf("V1 Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mMps.averageTime * 1e3f, mMps.medianTime * 1e3f, mMps.p99Time * 1e3f,
	       mSt.medianTime / mMps.medianTime);
	printf("V2 Single avg=%.3fms  median=%.3fms  p99=%.3fms\n",
	       mV2St.averageTime * 1e3f, mV2St.medianTime * 1e3f, mV2St.p99Time * 1e3f);
	printf("V2 Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mV2Mp.averageTime * 1e3f, mV2Mp.medianTime * 1e3f, mV2Mp.p99Time * 1e3f,
	       mV2St.medianTime / mV2Mp.medianTime);

	// V2's random rotation makes per-pixel AO differ by design; only check
	// that V2 self-agrees ST vs MP, and V1 MP matches V1 ST.
	int mismatches = 0;

	CalculateAmbientOcclusion(&camera);
	memcpy(camera.tempBuffer_2, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	CalculateAmbientOcclusionV2(&camera);
	memcpy(camera.tempBuffer_1, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	CalculateAmbientOcclusionV2Mp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (mismatches < 5)
				printf("V2 MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			mismatches++;
		}
	}

	CalculateAmbientOcclusion(&camera);
	CalculateAmbientOcclusionMp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_2[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			printf("V1 MISMATCH at %d: %.5f vs %.5f\n", i,
			       camera.tempBuffer_2[i], camera.ambientOcclusionBuffer[i]);
			mismatches++;
		}
	}

	if (mismatches)
		printf("CORRECTNESS FAIL: %d mismatching pixels\n", mismatches);
	else
		printf("Correctness: OK (all pixels match)\n");

	poolDestroy(pool);
	destroyCamera(&camera);
	return mismatches != 0;
}
