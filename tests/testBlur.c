#include "testBlur.h"
#include "timings.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#define SAMPLES 128

static void FillTestImage(Camera *camera) {
	int width = camera->screenWidth;
	int height = camera->screenHeight;
	for (int i = 0; i < width * height; i++)
		camera->framebuffer[i] = 0xFF101010;

	unsigned int seed = 0xDEADBEEF;
	for (int i = 0; i < 1024; i++) {
		seed = seed * 1664525u + 1013904223u;
		int x = (seed >> 16) % (width - 80);
		seed = seed * 1664525u + 1013904223u;
		int y = (seed >> 16) % (height - 80);
		seed = seed * 1664525u + 1013904223u;
		int size = 20 + (int)((seed >> 16) % 80);
		seed = seed * 1664525u + 1013904223u;
		unsigned int color = 0xFF000000 | (seed & 0x00FFFFFF);
		for (int py = y; py < y + size && py < height; py++)
			for (int px = x; px < x + size && px < width; px++)
				camera->framebuffer[py * width + px] = color;
	}
}

void BlurTaskFunction(void *arg) {
	BlurTask *task = (BlurTask *)arg;
	BlurRow(task->camera, task->row);
}

void BlurRowUoutputTaskFunction(void *arg) {
	BlurTask *task = (BlurTask *)arg;
	BlurRowUoutput(task->camera, task->row, task->output);
}

void BlurRow(Camera *camera, int row) {
	if (!camera || row < 0 || row >= camera->screenHeight) return;
	for (int col = 0; col < camera->screenWidth; col++) {
		int red = 0, green = 0, blue = 0;
		int count = 0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				int x = col + dx;
				int y = row + dy;
				if (x >= 0 && x < camera->screenWidth && y >= 0 && y < camera->screenHeight) {
					Color c = camera->framebuffer[y * camera->screenWidth + x];
					int4 rgb = UnpackColorInt(c);
					red += rgb.x;
					green += rgb.y;
					blue += rgb.z;
					count++;
				}
			}
		}
		camera->framebuffer[row * camera->screenWidth + col] = PackColorSafe((float)red / count / 255.0f, (float)green / count / 255.0f, (float)blue / count / 255.0f);
	}
}

void BlurRowUoutput(Camera *camera, int row, uint32 *output) {
	if (!camera || row < 0 || row >= camera->screenHeight) return;
	for (int col = 0; col < camera->screenWidth; col++) {
		int red = 0, green = 0, blue = 0;
		int count = 0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				int x = col + dx;
				int y = row + dy;
				if (x >= 0 && x < camera->screenWidth && y >= 0 && y < camera->screenHeight) {
					Color c = camera->framebuffer[y * camera->screenWidth + x];
					int4 rgb = UnpackColorInt(c);
					red += rgb.x;
					green += rgb.y;
					blue += rgb.z;
					count++;
				}
			}
		}
		output[col] = PackColorSafe((float)red / count / 255.0f, (float)green / count / 255.0f, (float)blue / count / 255.0f);
	}
}

void BlurScene(Camera *camera) {
	if (!camera) return;
	for (int row = 0; row < camera->screenHeight; row++) {
		for (int col = 0; col < camera->screenWidth; col++) {
			int red = 0, green = 0, blue = 0;
			int count = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					int x = col + dx;
					int y = row + dy;
					if (x >= 0 && x < camera->screenWidth && y >= 0 && y < camera->screenHeight) {
						Color c = camera->framebuffer[y * camera->screenWidth + x];
						int4 rgb = UnpackColorInt(c);
						red += rgb.x;
						green += rgb.y;
						blue += rgb.z;
						count++;
					}
				}
			}
			camera->framebuffer[row * camera->screenWidth + col] = PackColorSafe((float)red / count / 255.0f, (float)green / count / 255.0f, (float)blue / count / 255.0f);
		}
	}
}

int main() {
	mkdir("tests/img", 0755);

	Camera camera;
	initCamera(&camera, 800, 600, 90.0f, (float3){0.0f, 0.0f, 0.0f}, (float3){0.0f, 0.0f, 1.0f}, (float3){1.0f, 1.0f, 1.0f});

	int pixels = camera.screenWidth * camera.screenHeight;
	Color *backup = malloc(pixels * sizeof(Color));

	FillTestImage(&camera);
	memcpy(backup, camera.framebuffer, pixels * sizeof(Color));
	SaveImage("tests/img/original.bmp", &camera);

	// Single-threaded blur test
	float timeTook[SAMPLES] = {0};
	memcpy(camera.framebuffer, backup, pixels * sizeof(Color));
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		BlurScene(&camera);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	// apply one clean blur for the saved image
	BlurScene(&camera);
	SaveImage("tests/img/blur_single.bmp", &camera);

	PerformanceMetrics metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Single-threaded blur performance:\n");
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// Multi-threaded blur test (for each row we spawn a task)
	ThreadPool *pool = poolCreate(32, camera.screenHeight);
	BlurTask tasks[camera.screenHeight];

	memcpy(camera.framebuffer, backup, pixels * sizeof(Color));
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int row = 0; row < camera.screenHeight; row++) {
			tasks[row].camera = &camera;
			tasks[row].row = row;
			poolAdd(pool, BlurTaskFunction, &tasks[row]);
		}
		poolWait(pool);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	SaveImage("tests/img/blur_multi.bmp", &camera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Multi-threaded blur performance:\n");
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// Multi-threaded blur test with pattern
	memcpy(camera.framebuffer, backup, pixels * sizeof(Color));
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int phase = 0; phase < 3; phase++) {
			for (int row = phase; row < camera.screenHeight; row += 3) {
				tasks[row].camera = &camera;
				tasks[row].row = row;
				poolAdd(pool, BlurTaskFunction, &tasks[row]);
			}
			poolWait(pool);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}

	SaveImage("tests/img/blur_pattern.bmp", &camera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Multi-threaded blur (pattern) performance:\n");
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// Multi-threaded blur test with output buffer
	uint32 *outputBuffer = malloc(pixels * sizeof(uint32));
	memcpy(camera.framebuffer, backup, pixels * sizeof(Color));
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int row = 0; row < camera.screenHeight; row++) {
			tasks[row].camera = &camera;
			tasks[row].row = row;
			tasks[row].output = outputBuffer + row * camera.screenWidth;
			poolAdd(pool, BlurRowUoutputTaskFunction, &tasks[row]);
		}
		poolWait(pool);
		memcpy(camera.framebuffer, outputBuffer, pixels * sizeof(uint32));
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	SaveImage("tests/img/blur_output.bmp", &camera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("Multi-threaded blur (output buffer) performance:\n");
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// --- imgMethods.h filter tests ---
	int width = camera.screenWidth;
	int height = camera.screenHeight;
	float3 *f3src = malloc(pixels * sizeof(float3));
	float3 *f3tmp = malloc(pixels * sizeof(float3));
	float3 *f3dst = malloc(pixels * sizeof(float3));
	float3 *f3backup = malloc(pixels * sizeof(float3));

	for (int i = 0; i < pixels; i++) {
		Color c = backup[i];
		f3src[i] = (float3){
			(float)((c >> 16) & 0xFF) / 255.0f,
			(float)((c >> 8) & 0xFF) / 255.0f,
			(float)(c & 0xFF) / 255.0f,
			0.0f};
	}
	memcpy(f3backup, f3src, pixels * sizeof(float3));

	// BoxBlur3x3 benchmark
	memcpy(f3src, f3backup, pixels * sizeof(float3));
	float3 *boxSrc = f3src, *boxDst = f3dst;
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		BoxBlur3x3(boxSrc, f3tmp, boxDst, width, height);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
		float3 *swap = boxSrc;
		boxSrc = boxDst;
		boxDst = swap;
	}
	for (int i = 0; i < pixels; i++)
		camera.framebuffer[i] = PackColorSafe(boxSrc[i].x, boxSrc[i].y, boxSrc[i].z);
	SaveImage("tests/img/blur_box3x3.bmp", &camera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("BoxBlur3x3 performance:\n");
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// DecimateBuffer benchmark (factor=2)
	const int factor = 2;
	int decW = width / factor;
	int decH = height / factor;
	float3 *f3dec = malloc(decW * decH * sizeof(float3));

	memcpy(f3src, f3backup, pixels * sizeof(float3));
	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		DecimateBuffer(f3src, f3dec, width, height, factor);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	Camera decCamera;
	initCamera(&decCamera, decW, decH, 90.0f, (float3){0.0f, 0.0f, 0.0f}, (float3){0.0f, 0.0f, 1.0f}, (float3){1.0f, 1.0f, 1.0f});
	for (int i = 0; i < decW * decH; i++)
		decCamera.framebuffer[i] = PackColorSafe(f3dec[i].x, f3dec[i].y, f3dec[i].z);
	SaveImage("tests/img/decimate.bmp", &decCamera);
	destroyCamera(&decCamera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("DecimateBuffer (factor=%d, %dx%d -> %dx%d) performance:\n", factor, width, height, decW, decH);
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	// UpsampleBilinear benchmark (decW x decH -> width x height)
	float3 *f3up_tmp = malloc(decH * width * sizeof(float3));
	float3 *f3up_dst = malloc(pixels * sizeof(float3));

	for (int i = 0; i < SAMPLES; i++) {
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		UpsampleBilinear(f3dec, f3up_tmp, f3up_dst, decW, decH, width, height);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		timeTook[i] = (float)(t1.tv_sec - t0.tv_sec) + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-9f;
	}
	for (int i = 0; i < pixels; i++)
		camera.framebuffer[i] = PackColorSafe(f3up_dst[i].x, f3up_dst[i].y, f3up_dst[i].z);
	SaveImage("tests/img/upsample.bmp", &camera);

	metrics = ComputePerformanceMetrics(timeTook, SAMPLES);
	printf("========================================\n");
	printf("UpsampleBilinear (%dx%d -> %dx%d) performance:\n", decW, decH, width, height);
	printf("Average Time: %.6f seconds\n", metrics.averageTime);
	printf("Median Time: %.6f seconds\n", metrics.medianTime);
	printf("Min Time: %.6f seconds\n", metrics.minTime);
	printf("Max Time: %.6f seconds\n", metrics.maxTime);
	printf("Variance: %.6f\n", metrics.variance);
	printf("P99 Time: %.6f seconds\n", metrics.p99Time);

	free(f3src);
	free(f3tmp);
	free(f3dst);
	free(f3backup);
	free(f3dec);
	free(f3up_tmp);
	free(f3up_dst);

	poolDestroy(pool);
	free(outputBuffer);
	free(backup);
	destroyCamera(&camera);
	return 0;
}
