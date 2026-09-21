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
	PerformanceMetrics mV3St = ComputePerformanceMetrics(timesV3St, SAMPLES);
	PerformanceMetrics mV3Mp = ComputePerformanceMetrics(timesV3Mp, SAMPLES);

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
	printf("V2Plus Single avg=%.3fms  median=%.3fms  p99=%.3fms\n",
	       mV2PlusSt.averageTime * 1e3f, mV2PlusSt.medianTime * 1e3f, mV2PlusSt.p99Time * 1e3f);
	printf("V2Plus Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mV2PlusMp.averageTime * 1e3f, mV2PlusMp.medianTime * 1e3f, mV2PlusMp.p99Time * 1e3f,
	       mV2PlusSt.medianTime / mV2PlusMp.medianTime);
	printf("V2PlusCol Single avg=%.3fms  median=%.3fms  p99=%.3fms\n",
	       mV2PlusColSt.averageTime * 1e3f, mV2PlusColSt.medianTime * 1e3f, mV2PlusColSt.p99Time * 1e3f);
	printf("V2PlusCol Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mV2PlusColMp.averageTime * 1e3f, mV2PlusColMp.medianTime * 1e3f, mV2PlusColMp.p99Time * 1e3f,
	       mV2PlusColSt.medianTime / mV2PlusColMp.medianTime);
	printf("V2PlusColSg Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mV2PlusColSgMp.averageTime * 1e3f, mV2PlusColSgMp.medianTime * 1e3f, mV2PlusColSgMp.p99Time * 1e3f,
	       mV2PlusColSt.medianTime / mV2PlusColSgMp.medianTime);
	printf("V3 Single avg=%.3fms  median=%.3fms  p99=%.3fms\n",
	       mV3St.averageTime * 1e3f, mV3St.medianTime * 1e3f, mV3St.p99Time * 1e3f);
	printf("V3 Multi  avg=%.3fms  median=%.3fms  p99=%.3fms  speedup=%.2fx\n",
	       mV3Mp.averageTime * 1e3f, mV3Mp.medianTime * 1e3f, mV3Mp.p99Time * 1e3f,
	       mV3St.medianTime / mV3Mp.medianTime);

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

	// V3 is V2's math plus the all-sky chunk skip, so both V3 paths must
	// reproduce V2 ST bit-for-bit (skipped pixels get the same 1.0).
	int v3Mismatches = 0;
	CalculateAmbientOcclusionV3(&camera);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (v3Mismatches < 5)
				printf("V3 ST MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			v3Mismatches++;
		}
	}
	CalculateAmbientOcclusionV3Mp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (v3Mismatches < 5)
				printf("V3 MP MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			v3Mismatches++;
		}
	}
	if (v3Mismatches)
		printf("V3 CORRECTNESS FAIL: %d mismatching pixels\n", v3Mismatches);
	mismatches += v3Mismatches;

	// V2Plus is V2's math plus the per-row blur; the left/right KERNEL_SIZE_HALF
	// columns are left unwritten, so only ST/MP self-agreement is checked.
	// tempBuffer_1 is free here; tempBuffer_2 still holds the V1 ST snapshot.
	int v2PlusMismatches = 0;
	CalculateAmbientOcclusionV2Plus(&camera);
	memcpy(camera.tempBuffer_1, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	CalculateAmbientOcclusionV2PlusMp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (v2PlusMismatches < 5)
				printf("V2Plus MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			v2PlusMismatches++;
		}
	}
	if (v2PlusMismatches)
		printf("V2Plus CORRECTNESS FAIL: %d mismatching pixels\n", v2PlusMismatches);
	mismatches += v2PlusMismatches;

	// V2PlusColumn is V2Plus plus the serial column pass; same border caveat,
	// so again only ST/MP self-agreement is checked (tempBuffer_1 is free).
	int v2PlusColMismatches = 0;
	CalculateAmbientOcclusionV2PlusColumn(&camera);
	memcpy(camera.tempBuffer_1, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	CalculateAmbientOcclusionV2PlusColumnMp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (v2PlusColMismatches < 5)
				printf("V2PlusCol MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			v2PlusColMismatches++;
		}
	}
	if (v2PlusColMismatches)
		printf("V2PlusCol CORRECTNESS FAIL: %d mismatching pixels\n", v2PlusColMismatches);
	mismatches += v2PlusColMismatches;

	// Sg = serial column pass (parallel row pass, single-threaded blur);
	// must reproduce the ST result bit-for-bit.
	int v2PlusColSgMismatches = 0;
	CalculateAmbientOcclusionV2PlusColumn(&camera);
	memcpy(camera.tempBuffer_1, camera.ambientOcclusionBuffer, sizeof(float) * WIDTH * HEIGHT);
	CalculateAmbientOcclusionV2PlusColumnSgMp(&camera, pool);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		if (fabsf(camera.tempBuffer_1[i] - camera.ambientOcclusionBuffer[i]) > 1e-4f) {
			if (v2PlusColSgMismatches < 5)
				printf("V2PlusColSg MISMATCH at %d: %.5f vs %.5f\n", i,
				       camera.tempBuffer_1[i], camera.ambientOcclusionBuffer[i]);
			v2PlusColSgMismatches++;
		}
	}
	if (v2PlusColSgMismatches)
		printf("V2PlusColSg CORRECTNESS FAIL: %d mismatching pixels\n", v2PlusColSgMismatches);
	mismatches += v2PlusColSgMismatches;

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
