#include "../../object/format.h"
#include "../../math/vector3.h"
#include "../../util/threadPool.h"
#include <immintrin.h>
#include <stdbool.h>

#define MAX_FLOAT 3.402823466e+38F
#define SAMPLES 8
#define VARIATIONS 8
// Rows handed to one pool task in the legacy AOBench entry points below: bigger bands cut
// per-task overhead, smaller ones balance better
// Swept 1..64 rows/task on AOBench: 1 and 2 tie at the top, 8+ loses to balance, 16+ idles threads
#ifndef ROWS_PER_TASK
#define ROWS_PER_TASK 2
#endif
// Band for the two batches that make up the rendered frame's AO pass (the sample + row-blur rows
// and the framebuffer apply).  There the pool hand-off scales with the task count while the kernels
// do not: a no-op region already costs 2.2 ms for 360 tasks in situ, the pass queues ~900 tasks a
// frame against 5.5 ms of total pass time, and its full-resolution column-blur region measures ~0.
// 8-row bands cut the batch to 90 tasks and take ~1 ms (4.6% of the raster phase, 5/5 paired
// rounds) off the frame, while still leaving 90 bands to balance 720 rows over the pool; 20+ rows
// loses to tail imbalance.  Frames stay bit-identical (all 10 bench hashes).
#ifndef AO_ROWS_PER_TASK
#define AO_ROWS_PER_TASK 8
#endif
// Columns handed to one pool task for the column blur; wider bands amortize the strided sweep's cache lines
// Swept 2..64 columns/task on AOBench: 16 best (~6.2ms median), 2 worst (~7.9ms, 638 tiny tasks); 1 exceeds the pool ring
#ifndef COLUMNS_PER_TASK
#define COLUMNS_PER_TASK 16
#endif
// Wide-smoothing stage: 8 samples per pixel is chunky at the sample disk scale, so the final look comes from
// a heavily blurred low-resolution copy (box downsample, separable blur, bilinear upsample).
// Fewer sampled pixels plus a wider kernel is what turns the blobs into a gradient
#ifndef AO_SMOOTH_DOWNSCALE
#define AO_SMOOTH_DOWNSCALE 4
#endif
#ifndef AO_SMOOTH_ROUNDS
#define AO_SMOOTH_ROUNDS 2
#endif
// Row bands for the downsample/upsample tasks: these passes are tiny, so per-task overhead dominates
#ifndef AO_SMOOTH_ROWS_PER_TASK
#define AO_SMOOTH_ROWS_PER_TASK 16
#endif
// Blur bands for the wide stage's 270x180 buffer.  A band there is ~1/16 the size of a full-resolution
// one, so the pool hand-off costs more than the blur it feeds: the 212 tasks a frame needs today
// (2 rounds x row+col band) measure 2.19 ms of pure hand-off against 0.24 ms for 22 empty tasks.
// Coarser bands measure row blur 0.76 -> 0.21 ms and column blur 0.215 -> 0.147 ms with bit-identical
// output (bench/aoWide).  The band size is an explicit argument now: the full-resolution column pass
// (the only other caller) still passes COLUMNS_PER_TASK, and the legacy CalculateAmbientOcclusion*Mp
// entry points below still size their bands from ROWS_PER_TASK.
#ifndef AO_SMOOTH_BLUR_ROWS_PER_TASK
#define AO_SMOOTH_BLUR_ROWS_PER_TASK 8
#endif
#ifndef AO_SMOOTH_BLUR_COLUMNS_PER_TASK
#define AO_SMOOTH_BLUR_COLUMNS_PER_TASK 64
#endif

#define PIXEL_SKIP 5

static const float2 SAMPLES_PATTERN[VARIATIONS][SAMPLES] = {
	{
		{0.272f, -0.923f},
		{-0.555f, -0.753f},
		{-0.204f, -0.797f},
		{-0.908f, 0.266f},
		{0.069f, 0.373f},
		{-0.969f, -0.778f},
		{0.813f, -0.740f},
		{0.829f, -0.423f},
	},
	{
		{0.910f, -0.830f},
		{-0.400f, 0.297f},
		{-0.124f, 0.259f},
		{0.358f, -0.427f},
		{-0.278f, 0.076f},
		{-0.311f, 0.643f},
		{0.175f, -0.637f},
		{-0.870f, 0.513f},
	},
	{
		{-0.255f, -0.611f},
		{0.149f, -0.168f},
		{0.043f, 0.504f},
		{-0.858f, -0.720f},
		{-0.291f, 0.243f},
		{-0.966f, -0.167f},
		{-0.142f, 0.508f},
		{-0.601f, -0.611f},
	},
	{
		{0.706f, 0.873f},
		{-0.650f, -0.080f},
		{-0.147f, 0.763f},
		{0.147f, 0.902f},
		{0.864f, 0.729f},
		{0.243f, -0.923f},
		{0.025f, -0.945f},
		{-0.063f, -0.462f},
	},
	{
		{0.541f, -0.284f},
		{-0.472f, 0.426f},
		{0.594f, -0.625f},
		{0.977f, 0.123f},
		{0.708f, 0.411f},
		{0.010f, -0.182f},
		{-0.426f, 0.960f},
		{-0.594f, 0.860f},
	},
	{
		{-0.198f, 0.561f},
		{-0.606f, 0.302f},
		{-0.078f, -0.146f},
		{0.882f, -0.298f},
		{-0.463f, 0.979f},
		{0.366f, -0.647f},
		{0.430f, 0.083f},
		{0.992f, -0.246f},
	},
	{
		{-0.116f, 0.141f},
		{-0.447f, 0.491f},
		{0.746f, -0.437f},
		{0.697f, -0.304f},
		{0.565f, 0.066f},
		{-0.537f, -0.162f},
		{-0.289f, 0.708f},
		{-0.125f, -0.836f},
	},
	{
		{0.634f, 0.221f},
		{-0.735f, 0.486f},
		{0.158f, -0.921f},
		{-0.402f, -0.577f},
		{0.884f, 0.355f},
		{0.297f, 0.640f},
		{-0.913f, -0.089f},
		{0.048f, 0.352f},
	}};

#define ROTATIONS 16

static const float2 ROTATION_TABLE[ROTATIONS] = {
	{1.0f, 0.0f},
	{0.923879f, 0.382683f},
	{0.707107f, 0.707107f},
	{0.382683f, 0.923879f},
	{0.0f, 1.0f},
	{-0.382683f, 0.923879f},
	{-0.707107f, 0.707107f},
	{-0.923879f, 0.382683f},
	{-1.0f, 0.0f},
	{-0.923879f, -0.382683f},
	{-0.707107f, -0.707107f},
	{-0.382683f, -0.923879f},
	{0.0f, -1.0f},
	{0.382683f, -0.923879f},
	{0.707107f, -0.707107f},
	{0.923879f, -0.382683f},
};

#define KERNEL_SIZE 5
#define KERNEL_SIZE_HALF (KERNEL_SIZE / 2)
static const float lineKernelvaluse[KERNEL_SIZE] = {
	0.054488685f,
	0.244201342f,
	0.402619947f,
	0.244201342f,
	0.054488685f};

static const float INV_VALID[SAMPLES + 1] = {
	0.0f,
	1.0f,
	0.5f,
	0.333333f,
	0.25f,
	0.2f,
	0.166667f,
	0.142857f,
	0.125f,
};

static uint32 s = 0;

// NOTE: too slow makes it like 2 times slower
// static inline uint32 fastHash(int x, int y) {
//     uint32 state = s++;

//     uint32 h =
//         (uint32)x * 0x9E3779B1u ^
//         (uint32)y * 0x85EBCA67u ^
//         state;

//     h ^= h >> 16;
//     h *= 0x7FEB352Du;
//     h ^= h >> 13;
//     h *= 0x846CA68Bu;
//     h ^= h >> 16;

//     return h;
// }

static inline void fastHashReset(void) {
	s = 0;
}

static inline uint32_t fastHash(int x, int y) {
	uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA67u;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 13;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

static inline float Clamp(float val, float lower, float upper) {
	if (val < lower) return lower;
	if (val > upper) return upper;
	return val;
}

static void CalculateAmbientOcclusion(Camera *camera) {
	const float pixelRadius = 16.0f; // sample spread in pixels
	const float worldRadius = 20.5f; // max world-space distance (scene units)
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;

	for (int i = 0; i < camera->screenHeight; i++) {
		for (int j = 0; j < camera->screenWidth; j++) {
			int idx = i * camera->screenWidth + j;

			// Sky / no geometry: no occlusion, overwrite with fully lit so old data never persists.
			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				camera->ambientOcclusionBuffer[idx] = 1.0f;
				continue;
			}

			float3 normal = camera->normalBuffer[idx];
			float3 position = camera->positionBuffer[idx];

			float occlusion = 0.0f;
			int validSamples = 0;

			const float2 *pattern = SAMPLES_PATTERN[fastHash(i, j) % VARIATIONS];

			for (int k = 0; k < SAMPLES; k++) {
				float sampleX = j + pattern[k].x * pixelRadius;
				float sampleY = i + pattern[k].y * pixelRadius;

				if (sampleX < 0 || sampleX >= camera->screenWidth ||
					sampleY < 0 || sampleY >= camera->screenHeight) {
					continue;
				}

				int sampleIndex = (int)sampleY * camera->screenWidth + (int)sampleX;
				float3 samplePos = camera->positionBuffer[sampleIndex];

				float3 dir = Float3_Sub(samplePos, position);
				float nd = Float3_Dot(normal, dir);
				if (nd <= 0.0f) continue;

				float dist2 = Float3_Dot(dir, dir);
				if (dist2 < bias2 || dist2 > worldRadius2) continue;

				float dist = sqrtf(dist2);
				occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
				validSamples++;
			}

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
}

static void CalculateAmbientOcclusionV2(Camera *camera) {
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;
	const float focalLengthPixels = (camera->screenHeight * 0.5f) / camera->fovScale;

	for (int i = 0; i < camera->screenHeight; i++) {
		for (int j = 0; j < camera->screenWidth; j++) {
			int idx = i * camera->screenWidth + j;

			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				camera->ambientOcclusionBuffer[idx] = 1.0f;
				continue;
			}

			float3 normal = camera->normalBuffer[idx];
			float3 position = camera->positionBuffer[idx];
			float viewDepth = camera->depthBuffer[idx];

			float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
			pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

			uint32_t h = fastHash(i, j);
			const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
			float ca = rotation.x, sa = rotation.y;

			float occlusion = 0.0f;
			int validSamples = 0;

			const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

			for (int k = 0; k < SAMPLES; k++) {
				float sx = pattern[k].x;
				float sy = pattern[k].y;
				float rx = sx * ca - sy * sa;
				float ry = sx * sa + sy * ca;

				float sampleX = j + rx * pixelRadius;
				float sampleY = i + ry * pixelRadius;

				if (sampleX < 0 || sampleX >= camera->screenWidth ||
					sampleY < 0 || sampleY >= camera->screenHeight) {
					continue;
				}

				int sampleIndex = (int)sampleY * camera->screenWidth + (int)sampleX;

				if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
					continue;
				}

				float3 samplePos = camera->positionBuffer[sampleIndex];
				float3 dir = Float3_Sub(samplePos, position);
				float nd = Float3_Dot(normal, dir);
				if (nd <= 0.0f) continue;

				float dist2 = Float3_Dot(dir, dir);
				if (dist2 < bias2 || dist2 > worldRadius2) continue;

				float dist = sqrtf(dist2);
				occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
				validSamples++;
			}

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
}

typedef struct {
	int row;  // first row of the band
	int rows; // rows in the band
	Camera *camera;
} AmbientOcclusionTask;

static void CalculateAmbientOcclusionRow(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;

	const float pixelRadius = 16.0f; // sample spread in pixels
	const float worldRadius = 20.5f; // max world-space distance (scene units)
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;

	for (int row = task->row; row < endRow; row++) {
		for (int j = 0; j < width; j++) {
			const int idx = row * width + j;

			// Sky / no geometry: no occlusion, overwrite with fully lit so old data never persists.
			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				camera->ambientOcclusionBuffer[idx] = 1.0f;
				continue;
			}

			float3 normal = camera->normalBuffer[idx];
			float3 position = camera->positionBuffer[idx];

			float occlusion = 0.0f;
			int validSamples = 0;

			const float2 *pattern = SAMPLES_PATTERN[fastHash(row, j) % VARIATIONS];

			for (int k = 0; k < SAMPLES; k++) {
				float sampleX = j + pattern[k].x * pixelRadius;
				float sampleY = row + pattern[k].y * pixelRadius;

				if (sampleX < 0 || sampleX >= width ||
					sampleY < 0 || sampleY >= height) {
					continue;
				}

				int sampleIndex = (int)sampleY * width + (int)sampleX;
				float3 samplePos = camera->positionBuffer[sampleIndex];

				float3 dir = Float3_Sub(samplePos, position);
				float nd = Float3_Dot(normal, dir);
				if (nd <= 0.0f) continue;

				float dist2 = Float3_Dot(dir, dir);
				if (dist2 < bias2 || dist2 > worldRadius2) continue;

				float dist = sqrtf(dist2);
				occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
				validSamples++;
			}

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
}

static void CalculateAmbientOcclusionV2Row(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;
	const float focalLengthPixels = (height * 0.5f) / camera->fovScale;
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;

	for (int row = task->row; row < endRow; row++) {
		for (int j = 0; j < width; j++) {
			const int idx = row * width + j;

			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				camera->ambientOcclusionBuffer[idx] = 1.0f;
				continue;
			}

			float3 normal = camera->normalBuffer[idx];
			float3 position = camera->positionBuffer[idx];
			float viewDepth = camera->depthBuffer[idx];

			float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
			pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

			uint32_t h = fastHash(row, j);
			const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
			float ca = rotation.x, sa = rotation.y;

			float occlusion = 0.0f;
			int validSamples = 0;

			const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

			for (int k = 0; k < SAMPLES; k++) {
				float sx = pattern[k].x;
				float sy = pattern[k].y;
				float rx = sx * ca - sy * sa;
				float ry = sx * sa + sy * ca;

				float sampleX = j + rx * pixelRadius;
				float sampleY = row + ry * pixelRadius;

				if (sampleX < 0 || sampleX >= width ||
					sampleY < 0 || sampleY >= height) {
					continue;
				}

				int sampleIndex = (int)sampleY * width + (int)sampleX;

				if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
					continue;
				}

				float3 samplePos = camera->positionBuffer[sampleIndex];
				float3 dir = Float3_Sub(samplePos, position);
				float nd = Float3_Dot(normal, dir);
				if (nd <= 0.0f) continue;

				float dist2 = Float3_Dot(dir, dir);
				if (dist2 < bias2 || dist2 > worldRadius2) continue;

				float dist = sqrtf(dist2);
				occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
				validSamples++;
			}

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			camera->ambientOcclusionBuffer[idx] = ao;

			// float3 baseColor = UnpackColor(camera->framebuffer[idx]);

			// baseColor.x = baseColor.x * ao;
			// baseColor.y = baseColor.y * ao;
			// baseColor.z = baseColor.z * ao;

			// camera->framebuffer[idx]= PackColorF(baseColor);
		}
	}
}

static void CalculateAmbientOcclusionV2RowPlus(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;
	const float focalLengthPixels = (height * 0.5f) / camera->fovScale;
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;

	float rowValues[width];

	for (int row = task->row; row < endRow; row++) {
		for (int j = 0; j < width; j++) {
			const int idx = row * width + j;
			const int lineNumber = endRow - row;

			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				rowValues[j] = 1.0f;
				continue;
			}

			float3 normal = camera->normalBuffer[idx];
			float3 position = camera->positionBuffer[idx];
			float viewDepth = camera->depthBuffer[idx];

			float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
			pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

			uint32_t h = fastHash(row, j);
			const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
			float ca = rotation.x, sa = rotation.y;

			float occlusion = 0.0f;
			int validSamples = 0;

			const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

			for (int k = 0; k < SAMPLES; k++) {
				float sx = pattern[k].x;
				float sy = pattern[k].y;
				float rx = sx * ca - sy * sa;
				float ry = sx * sa + sy * ca;

				float sampleX = j + rx * pixelRadius;
				float sampleY = row + ry * pixelRadius;

				if (sampleX < 0 || sampleX >= width ||
					sampleY < 0 || sampleY >= height) {
					continue;
				}

				int sampleIndex = (int)sampleY * width + (int)sampleX;

				if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
					continue;
				}

				float3 samplePos = camera->positionBuffer[sampleIndex];
				float3 dir = Float3_Sub(samplePos, position);
				float nd = Float3_Dot(normal, dir);
				if (nd <= 0.0f) continue;

				float dist2 = Float3_Dot(dir, dir);
				if (dist2 < bias2 || dist2 > worldRadius2) continue;

				float dist = sqrtf(dist2);
				occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
				validSamples++;
			}

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			rowValues[j] = ao;

			// float3 baseColor = UnpackColor(camera->framebuffer[idx]);

			// baseColor.x = baseColor.x * ao;
			// baseColor.y = baseColor.y * ao;
			// baseColor.z = baseColor.z * ao;

			// camera->framebuffer[idx]= PackColorF(baseColor);
		}

		// start from kernel size half and end early to avoid bound checks
		for (int j = KERNEL_SIZE_HALF; j < width - KERNEL_SIZE_HALF; j++) {
			const int idx = row * width + j;
			float sum = 0.0f;
			for (int k = 0; k < KERNEL_SIZE; k++) {
				sum += rowValues[j + k - KERNEL_SIZE_HALF] * lineKernelvaluse[k];
			}
			camera->ambientOcclusionBuffer[idx] = sum;
		}
	}
}

static void CalculateAmbientOcclusionV2RowPlusSkip(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;
	const float focalLengthPixels = (height * 0.5f) / camera->fovScale;
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;
	const float invSkip = 1.0f / PIXEL_SKIP;

	float rowValues[width];

	for (int row = task->row; row < endRow; row++) {
		// Sample every PIXEL_SKIP-th column; the skipped pixels are lerped in place
		// as soon as their right sample lands, so the blur pass below never reads
		// unwritten entries and no clear/fill pass is needed.
		for (int j = 0; j < width; j += PIXEL_SKIP) {
			const int idx = row * width + j;

			float ao;
			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				ao = 1.0f;
			} else {
				float3 normal = camera->normalBuffer[idx];
				float3 position = camera->positionBuffer[idx];
				float viewDepth = camera->depthBuffer[idx];

				float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
				pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

				uint32_t h = fastHash(row, j);
				const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
				float ca = rotation.x, sa = rotation.y;

				float occlusion = 0.0f;
				int validSamples = 0;

				const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

				for (int k = 0; k < SAMPLES; k++) {
					float sx = pattern[k].x;
					float sy = pattern[k].y;
					float rx = sx * ca - sy * sa;
					float ry = sx * sa + sy * ca;

					float sampleX = j + rx * pixelRadius;
					float sampleY = row + ry * pixelRadius;

					if (sampleX < 0 || sampleX >= width ||
						sampleY < 0 || sampleY >= height) {
						continue;
					}

					int sampleIndex = (int)sampleY * width + (int)sampleX;

					if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
						continue;
					}

					float3 samplePos = camera->positionBuffer[sampleIndex];
					float3 dir = Float3_Sub(samplePos, position);
					float nd = Float3_Dot(normal, dir);
					if (nd <= 0.0f) continue;

					float dist2 = Float3_Dot(dir, dir);
					if (dist2 < bias2 || dist2 > worldRadius2) continue;

					float dist = sqrtf(dist2);
					occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
					validSamples++;
				}

				ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			}

			rowValues[j] = ao;

			if (j >= PIXEL_SKIP) {
				const float prev = rowValues[j - PIXEL_SKIP];
				const float step = (ao - prev) * invSkip;
				for (int t = 1; t < PIXEL_SKIP; t++) {
					rowValues[j - PIXEL_SKIP + t] = prev + step * t;
				}
			}
		}

		// width rarely divides PIXEL_SKIP; replicate the last sample over the tail
		const int lastSample = ((width - 1) / PIXEL_SKIP) * PIXEL_SKIP;
		for (int j = lastSample + 1; j < width; j++) {
			rowValues[j] = rowValues[lastSample];
		}

		// start from kernel size half and end early to avoid bound checks
		for (int j = KERNEL_SIZE_HALF; j < width - KERNEL_SIZE_HALF; j++) {
			const int idx = row * width + j;
			float sum = 0.0f;
			for (int k = 0; k < KERNEL_SIZE; k++) {
				sum += rowValues[j + k - KERNEL_SIZE_HALF] * lineKernelvaluse[k];
			}
			camera->ambientOcclusionBuffer[idx] = sum;
		}
	}
}

/* ---------------------------------------------------------------------------
 * AVX2 (8-lane) body shared by both depth-weighted 5-tap AO blur passes.
 *
 * Both passes are the same separable box-approximation kernel, evaluated on a
 * different axis: the per-row pass inside CalculateAmbientOcclusionV2RowPlusSkip
 * BetterBlur() (horizontal, stride 1) and the vertical pass in
 * ColumnBlurColumnsBetter().  The scalar bodies below are kept verbatim as the
 * fallback/tail path; the vector path evaluates the same per-lane expression
 * sequence (same tap order, same Clamp, same `weightSum > 1e-5f` fallback), so
 * the only difference is FMA contraction / reassociation that -ffast-math
 * already allows (measured max |diff| 3.6e-07 on the AO buffer, 0 pixels off by
 * more than 1e-5, image_mse 0.00).
 * ------------------------------------------------------------------------- */
#define AO_DEPTH_REJECT_SCALE 0.02f // tune: smaller = stricter edge preservation

/* centreDepth * scale <= 1/FLT_MAX AND >= 0: 1/(d*scale) overflows to inf, the
 * centre tap evaluates 0*inf and the scalar body's own result there depends on
 * how clang lowers Clamp()/the final select under -ffast-math.  The vector path
 * makes it deterministic: such lanes fall back to the un-blurred value, which is
 * what the source-level `weightSum > 1e-5f` fallback expresses.  The renderer
 * cannot produce such a depth (depthBuffer holds a positive hit distance or
 * DEPTH_FAR = 1e30f), and the non-negative half of the test keeps the mask off
 * the finite-negative case, where the scalar does blur. */
#define AO_INV_OVERFLOW_THRESHOLD 2.9387359e-39f

#define AO_TAP8(v, d, k)                                                             \
	do {                                                                             \
		const __m256 diff_ = _mm256_and_ps(_mm256_sub_ps((d), centerDepth), absMask); \
		__m256 w_ = _mm256_sub_ps(one, _mm256_mul_ps(diff_, invT));                   \
		w_ = _mm256_max_ps(zero, _mm256_min_ps(w_, one));                             \
		w_ = _mm256_mul_ps(w_, _mm256_set1_ps(lineKernelvaluse[k]));                  \
		sum = _mm256_fmadd_ps((v), w_, sum);                                          \
		wsum = _mm256_add_ps(wsum, w_);                                               \
	} while (0)

/* v0..v4 / d0..d4 are the five taps centred on the lane group, centerDepth the
 * centre-lane depth and invT = 1/(centerDepth * AO_DEPTH_REJECT_SCALE). */
static inline void aoBlurTaps8(__m256 v0, __m256 v1, __m256 v2, __m256 v3, __m256 v4,
							   __m256 d0, __m256 d1, __m256 d2, __m256 d3, __m256 d4,
							   __m256 centerDepth, __m256 invT, __m256 *oS, __m256 *oW) {
	const __m256 zero = _mm256_setzero_ps();
	const __m256 one = _mm256_set1_ps(1.0f);
	const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
	__m256 sum = zero;
	__m256 wsum = zero;

	AO_TAP8(v0, d0, 0);
	AO_TAP8(v1, d1, 1);
	AO_TAP8(v2, d2, 2);
	AO_TAP8(v3, d3, 3);
	AO_TAP8(v4, d4, 4);

	*oS = sum;
	*oW = wsum;
}

static inline __m256 aoBlurInvT8(__m256 centerDepth, __m256 depthScale, __m256 *degenerate) {
	const __m256 prod = _mm256_mul_ps(centerDepth, depthScale);
	const __m256 underflow =
		_mm256_cmp_ps(prod, _mm256_set1_ps(AO_INV_OVERFLOW_THRESHOLD), _CMP_LE_OQ);
	const __m256 nonNegative = _mm256_cmp_ps(prod, _mm256_setzero_ps(), _CMP_GE_OQ);
	*degenerate = _mm256_and_ps(underflow, nonNegative);
	return _mm256_div_ps(_mm256_set1_ps(1.0f), prod);
}

static inline __m256 aoBlurFinish8(__m256 sum, __m256 wsum, __m256 centerValue,
								   __m256 degenerate) {
	const __m256 inv = _mm256_div_ps(sum, wsum);
	const __m256 ok = _mm256_cmp_ps(wsum, _mm256_set1_ps(1e-5f), _CMP_GT_OQ);
	const __m256 r = _mm256_blendv_ps(centerValue, inv, ok);
	return _mm256_blendv_ps(r, centerValue, degenerate);
}

/* One scalar column of the vertical pass - the original body, used for the
 * `< 8` tail of a task's column band. */
static void aoColBlurScalarColumn(float *restrict image, const float *restrict depthBuffer,
								  int width, int height, int x) {
	float colValues[height];

	for (int y = 0; y < height; y++) {
		colValues[y] = image[y * width + x];
	}

	// start from kernel size half and end early to avoid bound checks
	for (int y = KERNEL_SIZE_HALF; y < height - KERNEL_SIZE_HALF; y++) {
		const int centerIdx = y * width + x;
		const float centerDepth = depthBuffer[centerIdx];
		const float invDepthThreshold = 1.0f / (centerDepth * AO_DEPTH_REJECT_SCALE);

		float sum = 0.0f;
		float weightSum = 0.0f;
		for (int i = 0; i < KERNEL_SIZE; i++) {
			const int tapY = y + i - KERNEL_SIZE_HALF;
			const float depthDiff = fabsf(depthBuffer[tapY * width + x] - centerDepth);
			const float depthWeight = Clamp(1.0f - depthDiff * invDepthThreshold, 0.0f, 1.0f);
			const float w = lineKernelvaluse[i] * depthWeight;

			sum += colValues[tapY] * w;
			weightSum += w;
		}
		image[centerIdx] = weightSum > 1e-5f ? sum / weightSum : colValues[y];
	}
}

/* Eight columns of the vertical pass at once.  The scalar body loads 5 strided
 * taps per output pixel (one cache line each) and snapshots the whole column
 * into a `colValues[height]` VLA; this keeps the five tap rows of the group in
 * registers, so each source row is loaded once per 8 columns instead of 8
 * times, and drops the VLA entirely.  Stores are delayed by KERNEL_SIZE_HALF
 * rows, which is what makes the in-place form (image read and written) safe.
 * Caller must guarantee height >= KERNEL_SIZE + 1 and x0 + 8 <= width. */
static void aoColBlurVec8(float *restrict image, const float *restrict depth,
						  int width, int height, int x0) {
	const float *pin = image + x0;
	const float *pd = depth + x0;
	float *pout = image + x0;
	const int yStart = KERNEL_SIZE_HALF;
	const int yEnd = height - KERNEL_SIZE_HALF;
	const __m256 depthScale = _mm256_set1_ps(AO_DEPTH_REJECT_SCALE);

	__m256 v0 = _mm256_loadu_ps(pin + (yStart - 2) * width);
	__m256 v1 = _mm256_loadu_ps(pin + (yStart - 1) * width);
	__m256 v2 = _mm256_loadu_ps(pin + (yStart + 0) * width);
	__m256 v3 = _mm256_loadu_ps(pin + (yStart + 1) * width);
	__m256 v4 = _mm256_loadu_ps(pin + (yStart + 2) * width);
	__m256 d0 = _mm256_loadu_ps(pd + (yStart - 2) * width);
	__m256 d1 = _mm256_loadu_ps(pd + (yStart - 1) * width);
	__m256 d2 = _mm256_loadu_ps(pd + (yStart + 0) * width);
	__m256 d3 = _mm256_loadu_ps(pd + (yStart + 1) * width);
	__m256 d4 = _mm256_loadu_ps(pd + (yStart + 2) * width);

	__m256 pend0 = _mm256_setzero_ps();
	__m256 pend1 = _mm256_setzero_ps();

	for (int y = yStart; y < yEnd; y++) {
		const __m256 centerDepth = d2;
		__m256 degenerate;
		const __m256 invT = aoBlurInvT8(centerDepth, depthScale, &degenerate);
		__m256 sum, wsum;
		aoBlurTaps8(v0, v1, v2, v3, v4, d0, d1, d2, d3, d4, centerDepth, invT, &sum, &wsum);
		const __m256 res = aoBlurFinish8(sum, wsum, v2, degenerate);

		// result of row y - 2: its taps are behind us, so the store is safe now
		if (y - 2 >= yStart) _mm256_storeu_ps(pout + (y - 2) * width, pend1);
		pend1 = pend0;
		pend0 = res;

		v0 = v1; v1 = v2; v2 = v3; v3 = v4;
		d0 = d1; d1 = d2; d2 = d3; d3 = d4;
		{
			int ny = y + KERNEL_SIZE_HALF + 1;
			if (ny >= height) ny = height - 1;
			v4 = _mm256_loadu_ps(pin + ny * width);
			d4 = _mm256_loadu_ps(pd + ny * width);
		}
	}

	_mm256_storeu_ps(pout + (yEnd - 2) * width, pend1);
	_mm256_storeu_ps(pout + (yEnd - 1) * width, pend0);
}

/* One pixel of the horizontal pass - the original inner body, used for the
 * `< 8` tail of a row. */
static inline float aoRowBlurScalarPixel(const float *rowValues, const float *rowDepths, int j) {
	const float centerDepth = rowDepths[j];
	const float invDepthThreshold = 1.0f / (centerDepth * AO_DEPTH_REJECT_SCALE);

	float sum = 0.0f;
	float weightSum = 0.0f;
	for (int k = 0; k < KERNEL_SIZE; k++) {
		const int tapIdx = j + k - KERNEL_SIZE_HALF;
		const float depthDiff = fabsf(rowDepths[tapIdx] - centerDepth);
		const float depthWeight = Clamp(1.0f - depthDiff * invDepthThreshold, 0.0f, 1.0f);
		const float w = lineKernelvaluse[k] * depthWeight;

		sum += rowValues[tapIdx] * w;
		weightSum += w;
	}
	return weightSum > 1e-5f ? sum / weightSum : rowValues[j];
}

/* Eight pixels of the horizontal pass at once, out of the caller's row copies.
 * Caller must guarantee x + 8 <= width - KERNEL_SIZE_HALF. */
static inline void aoRowBlurVec8(const float *rowValues, const float *rowDepths, float *out, int x) {
	const __m256 centerDepth = _mm256_loadu_ps(rowDepths + x);
	__m256 degenerate;
	const __m256 invT = aoBlurInvT8(centerDepth, _mm256_set1_ps(AO_DEPTH_REJECT_SCALE), &degenerate);
	__m256 sum, wsum;
	aoBlurTaps8(
		_mm256_loadu_ps(rowValues + x - 2), _mm256_loadu_ps(rowValues + x - 1),
		_mm256_loadu_ps(rowValues + x + 0), _mm256_loadu_ps(rowValues + x + 1),
		_mm256_loadu_ps(rowValues + x + 2),
		_mm256_loadu_ps(rowDepths + x - 2), _mm256_loadu_ps(rowDepths + x - 1),
		centerDepth, _mm256_loadu_ps(rowDepths + x + 1), _mm256_loadu_ps(rowDepths + x + 2),
		centerDepth, invT, &sum, &wsum);
	_mm256_storeu_ps(out + x, aoBlurFinish8(sum, wsum, _mm256_loadu_ps(rowValues + x), degenerate));
}

static void CalculateAmbientOcclusionV2RowPlusSkipBetterBlur(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;
	const float focalLengthPixels = (height * 0.5f) / camera->fovScale;
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;
	const float invSkip = 1.0f / PIXEL_SKIP;

	float rowValues[width];
	float rowDepths[width];
	float maxDepth = 0.0f;
	float minDepth = MAX_FLOAT;

	for (int row = task->row; row < endRow; row++) {
		for (int j = 0; j < width; j += PIXEL_SKIP) {
			const int idx = row * width + j;

			float ao;
			if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) {
				ao = 1.0f;
			} else {
				float3 normal = camera->normalBuffer[idx];
				float3 position = camera->positionBuffer[idx];
				float viewDepth = camera->depthBuffer[idx];

				float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
				pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

				uint32_t h = fastHash(row, j);
				const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
				float ca = rotation.x, sa = rotation.y;

				float occlusion = 0.0f;
				int validSamples = 0;

				const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

				for (int k = 0; k < SAMPLES; k++) {
					float sx = pattern[k].x;
					float sy = pattern[k].y;
					float rx = sx * ca - sy * sa;
					float ry = sx * sa + sy * ca;

					float sampleX = j + rx * pixelRadius;
					float sampleY = row + ry * pixelRadius;

					if (sampleX < 0 || sampleX >= width ||
						sampleY < 0 || sampleY >= height) {
						continue;
					}

					int sampleIndex = (int)sampleY * width + (int)sampleX;

					if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
						continue;
					}

					float3 samplePos = camera->positionBuffer[sampleIndex];
					float3 dir = Float3_Sub(samplePos, position);
					float nd = Float3_Dot(normal, dir);
					if (nd <= 0.0f) continue;

					float dist2 = Float3_Dot(dir, dir);
					if (dist2 < bias2 || dist2 > worldRadius2) continue;

					float dist = sqrtf(dist2);
					occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
					validSamples++;
				}

				ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
				if (camera->depthBuffer[idx] < minDepth) {
					minDepth = camera->depthBuffer[idx];
				}
				if (camera->depthBuffer[idx] > maxDepth) {
					maxDepth = camera->depthBuffer[idx];
				}
			}

			rowValues[j] = ao;
			rowDepths[j] = camera->depthBuffer[idx];

			if (j >= PIXEL_SKIP) {
				const float prev = rowValues[j - PIXEL_SKIP];
				const float step = (ao - prev) * invSkip;
				const float prevDepth = rowDepths[j - PIXEL_SKIP];
				const float stepDepth = (camera->depthBuffer[idx] - prevDepth) * invSkip;
				for (int t = 1; t < PIXEL_SKIP; t++) {
					rowValues[j - PIXEL_SKIP + t] = prev + step * t;
					rowDepths[j - PIXEL_SKIP + t] = prevDepth + stepDepth * t;
				}
			}
		}

		// width rarely divides PIXEL_SKIP; replicate the last sample over the tail
		const int lastSample = ((width - 1) / PIXEL_SKIP) * PIXEL_SKIP;
		for (int j = lastSample + 1; j < width; j++) {
			rowValues[j] = rowValues[lastSample];
			rowDepths[j] = camera->depthBuffer[lastSample];
		}

		// start from kernel size half and end early to avoid bound checks;
		// 8 pixels at a time, scalar tail for the remainder
		float *outRow = camera->ambientOcclusionBuffer + row * width;
		int j = KERNEL_SIZE_HALF;
		for (; j + 8 <= width - KERNEL_SIZE_HALF; j += 8) {
			aoRowBlurVec8(rowValues, rowDepths, outRow, j);
		}
		for (; j < width - KERNEL_SIZE_HALF; j++) {
			outRow[j] = aoRowBlurScalarPixel(rowValues, rowDepths, j);
		}
	}
	setMaxDepth(camera, maxDepth);
	setMinDepth(camera, minDepth);
}

static void CalculateAmbientOcclusionV2Plus(Camera *camera) {
	AmbientOcclusionTask task = {0, camera->screenHeight, camera};
	CalculateAmbientOcclusionV2RowPlus(&task);
}

static void CalculateAmbientOcclusionV3Row(void *arg) {
	AmbientOcclusionTask *restrict task = arg;
	Camera *restrict camera = task->camera;

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int endRow = task->row + task->rows;
	const float focalLengthPixels = (height * 0.5f) / camera->fovScale;
	const float worldRadius = 20.5f;
	const float bias = worldRadius * 0.02f;
	const float bias2 = bias * bias;
	const float worldRadius2 = worldRadius * worldRadius;
	const float invWorldRadius = 1.0f / worldRadius;

	for (int row = task->row; row < endRow; row++) {
		const int rowBase = row * width;

		// One SIMD sky test per 8-pixel block; the mask steers the whole block:
		// all sky -> vector fill, mixed -> lanes kept, geometry lanes run the body.
		for (int j = 0; j < width; j += 8) {
			const int idx = rowBase + j;
			const int lanes = (width - j < 8) ? width - j : 8;
			unsigned skyMask;

			if (lanes == 8) {
				__m256 vals = _mm256_loadu_ps(&camera->depthBuffer[idx]);
				__m256 gt = _mm256_cmp_ps(vals, _mm256_set1_ps(DEPTH_FAR), _CMP_GE_OQ);
				__m256 lt = _mm256_cmp_ps(vals, _mm256_setzero_ps(), _CMP_LE_OQ);
				skyMask = (unsigned)_mm256_movemask_ps(_mm256_or_ps(gt, lt));

				if (skyMask == 0xFFu) {
					_mm256_storeu_ps(&camera->ambientOcclusionBuffer[idx], _mm256_set1_ps(1.0f));
					continue;
				}
			} else {
				skyMask = 0;
				for (int t = 0; t < lanes; t++) {
					const float d = camera->depthBuffer[idx + t];
					if (d >= DEPTH_FAR || d <= 0.0f) skyMask |= 1u << t;
				}
			}

			for (int lane = 0; lane < lanes; lane++) {
				if (skyMask & (1u << lane)) {
					camera->ambientOcclusionBuffer[idx + lane] = 1.0f;
					continue;
				}

				const int px = j + lane;
				const int pidx = idx + lane;

				float3 normal = camera->normalBuffer[pidx];
				float3 position = camera->positionBuffer[pidx];
				float viewDepth = camera->depthBuffer[pidx];

				float pixelRadius = (focalLengthPixels * worldRadius) / viewDepth;
				pixelRadius = Clamp(pixelRadius, 2.0f, 48.0f);

				uint32_t h = fastHash(row, px);
				const float2 rotation = ROTATION_TABLE[(h >> 3) & (ROTATIONS - 1)];
				float ca = rotation.x, sa = rotation.y;

				float occlusion = 0.0f;
				int validSamples = 0;

				const float2 *pattern = SAMPLES_PATTERN[h % VARIATIONS];

				for (int k = 0; k < SAMPLES; k++) {
					float sx = pattern[k].x;
					float sy = pattern[k].y;
					float rx = sx * ca - sy * sa;
					float ry = sx * sa + sy * ca;

					float sampleX = px + rx * pixelRadius;
					float sampleY = row + ry * pixelRadius;

					if (sampleX < 0 || sampleX >= width ||
						sampleY < 0 || sampleY >= height) {
						continue;
					}

					int sampleIndex = (int)sampleY * width + (int)sampleX;

					if (camera->depthBuffer[sampleIndex] >= DEPTH_FAR || camera->depthBuffer[sampleIndex] <= 0.0f) {
						continue;
					}

					float3 samplePos = camera->positionBuffer[sampleIndex];
					float3 dir = Float3_Sub(samplePos, position);
					float nd = Float3_Dot(normal, dir);
					if (nd <= 0.0f) continue;

					float dist2 = Float3_Dot(dir, dir);
					if (dist2 < bias2 || dist2 > worldRadius2) continue;

					float dist = sqrtf(dist2);
					occlusion += (nd / dist) * (1.0f - dist * invWorldRadius);
					validSamples++;
				}

				float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
				camera->ambientOcclusionBuffer[pidx] = ao;
			}
		}
	}
}

typedef struct {
	int column;	 // first column of the band
	int columns; // columns in the band
	int width;
	int height;
	float *image;
} ColumnBlurTask;

typedef struct {
	int column;	 // first column of the band
	int columns; // columns in the band
	int width;
	int height;
	float *image;
	float *DepthBuffer;
} ColumnBlurTaskBetter;

static void ColumnBlurColumns(void *arg) {
	ColumnBlurTask *restrict task = arg;
	const int width = task->width;
	const int height = task->height;
	const int endColumn = task->column + task->columns;
	float *restrict image = task->image;

	float colValues[height];

	for (int x = task->column; x < endColumn; x++) {
		for (int y = 0; y < height; y++) {
			colValues[y] = image[y * width + x];
		}

		// start from kernel size half and end early to avoid bound checks
		for (int y = KERNEL_SIZE_HALF; y < height - KERNEL_SIZE_HALF; y++) {
			float sum = 0.0f;
			for (int i = 0; i < KERNEL_SIZE; i++) {
				sum += colValues[y + i - KERNEL_SIZE_HALF] * lineKernelvaluse[i];
			}
			image[y * width + x] = sum;
		}
	}
}

static void ColumnBlurColumnsBetter(void *arg) {
	ColumnBlurTaskBetter *restrict task = arg;
	const int width = task->width;
	const int height = task->height;
	const int endColumn = task->column + task->columns;
	float *restrict image = task->image;
	float *restrict depthBuffer = task->DepthBuffer;

	// 8 columns at a time (one continuous 32-byte load per tap row instead of 5
	// strided scalar taps per pixel), scalar tail for the band remainder
	int x = task->column;
	if (height >= KERNEL_SIZE + 1) {
		for (; x + 8 <= endColumn; x += 8) {
			aoColBlurVec8(image, depthBuffer, width, height, x);
		}
	}
	for (; x < endColumn; x++) {
		aoColBlurScalarColumn(image, depthBuffer, width, height, x);
	}
}

static void columnBlur(int width, int height, float *restrict image) {
	ColumnBlurTask task = {KERNEL_SIZE_HALF, width - 2 * KERNEL_SIZE_HALF, width, height, image};
	ColumnBlurColumns(&task);
}

static void columnBlurMp(int width, int height, float *restrict image, ThreadPool *threadPool) {
	// Same interior range as columnBlur(): the outer columns hold stale data.
	const int firstColumn = KERNEL_SIZE_HALF;
	const int endColumn = width - KERNEL_SIZE_HALF;
	const int taskCount = (endColumn - firstColumn + COLUMNS_PER_TASK - 1) / COLUMNS_PER_TASK;
	ColumnBlurTask tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int column = firstColumn + t * COLUMNS_PER_TASK;
		const int columns = endColumn - column < COLUMNS_PER_TASK ? endColumn - column : COLUMNS_PER_TASK;
		tasks[t] = (ColumnBlurTask){column, columns, width, height, image};
		poolAdd(threadPool, ColumnBlurColumns, &tasks[t]);
	}
	poolWait(threadPool);
}

static void columnBlurMpBetter(int width, int height, float *restrict image, ThreadPool *threadPool, float *restrict depthBuffer, int columnsPerTask) {
	// Same interior range as columnBlur(): the outer columns hold stale data.
	const int firstColumn = KERNEL_SIZE_HALF;
	const int endColumn = width - KERNEL_SIZE_HALF;
	const int taskCount = (endColumn - firstColumn + columnsPerTask - 1) / columnsPerTask;
	ColumnBlurTaskBetter tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int column = firstColumn + t * columnsPerTask;
		const int columns = endColumn - column < columnsPerTask ? endColumn - column : columnsPerTask;
		tasks[t] = (ColumnBlurTaskBetter){column, columns, width, height, image, depthBuffer};
		poolAdd(threadPool, ColumnBlurColumnsBetter, &tasks[t]);
	}
	poolWait(threadPool);
}

typedef struct {
	int row;	 // first row of the band
	int rows; // rows in the band
	int width;
	float *image;
	float *DepthBuffer;
} RowBlurTaskBetter;

static void RowBlurRowsBetter(void *arg) {
	RowBlurTaskBetter *restrict task = arg;
	const int width = task->width;
	const int endRow = task->row + task->rows;
	float *restrict image = task->image;
	float *restrict depthBuffer = task->DepthBuffer;
	const float depthRejectScale = AO_DEPTH_REJECT_SCALE;

	float rowValues[width];
	float rowDepths[width];

	for (int y = task->row; y < endRow; y++) {
		const int rowBase = y * width;
		for (int x = 0; x < width; x++) {
			rowValues[x] = image[rowBase + x];
			rowDepths[x] = depthBuffer[rowBase + x];
		}

		// replicate over the outer columns: the AO passes never write them, so edge taps would read stale data
		rowValues[0] = rowValues[1] = rowValues[KERNEL_SIZE_HALF];
		rowValues[width - 2] = rowValues[width - 1] = rowValues[width - KERNEL_SIZE_HALF - 1];
		rowDepths[0] = rowDepths[1] = rowDepths[KERNEL_SIZE_HALF];
		rowDepths[width - 2] = rowDepths[width - 1] = rowDepths[width - KERNEL_SIZE_HALF - 1];

		for (int x = KERNEL_SIZE_HALF; x < width - KERNEL_SIZE_HALF; x++) {
			const float centerDepth = rowDepths[x];
			const float invDepthThreshold = 1.0f / (centerDepth * depthRejectScale);

			float sum = 0.0f;
			float weightSum = 0.0f;
			for (int i = 0; i < KERNEL_SIZE; i++) {
				const int tapX = x + i - KERNEL_SIZE_HALF;
				const float depthDiff = fabsf(rowDepths[tapX] - centerDepth);
				const float depthWeight = Clamp(1.0f - depthDiff * invDepthThreshold, 0.0f, 1.0f);
				const float w = lineKernelvaluse[i] * depthWeight;

				sum += rowValues[tapX] * w;
				weightSum += w;
			}
			image[rowBase + x] = weightSum > 1e-5f ? sum / weightSum : rowValues[x];
		}
	}
}

static void rowBlurMpBetter(int width, int height, float *restrict image, ThreadPool *threadPool, float *restrict depthBuffer, int rowsPerTask) {
	// Same interior row range as the column blur: the outer rows are never blurred vertically
	const int firstRow = KERNEL_SIZE_HALF;
	const int endRow = height - KERNEL_SIZE_HALF;
	const int taskCount = (endRow - firstRow + rowsPerTask - 1) / rowsPerTask;
	RowBlurTaskBetter tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int row = firstRow + t * rowsPerTask;
		const int rows = endRow - row < rowsPerTask ? endRow - row : rowsPerTask;
		tasks[t] = (RowBlurTaskBetter){row, rows, width, image, depthBuffer};
		poolAdd(threadPool, RowBlurRowsBetter, &tasks[t]);
	}
	poolWait(threadPool);
}

typedef struct {
	int row;	 // first row of the band (small buffer rows)
	int rows;
	int srcWidth;
	const float *restrict src;
	int dstWidth;
	float *restrict dst;
} DownsampleTask;

typedef struct {
	int row;	 // first row of the band (full-resolution rows)
	int rows;
	int dstWidth;
	float *restrict dst;
	int srcWidth;
	int srcHeight;
	const float *restrict src;
	// Per-column bilinear source pair + weight: the x mapping depends only on x and the fixed
	// downscale, so it is built once per call instead of floorf()+two clamps per output pixel.
	const int *restrict x0Tab;
	const int *restrict x1Tab;
	const float *restrict txTab;
} UpsampleTask;

static void DownsampleAoRows(void *arg) {
	DownsampleTask *restrict task = arg;
	const int scale = AO_SMOOTH_DOWNSCALE;
	const int srcWidth = task->srcWidth;
	const int minX = KERNEL_SIZE_HALF;
	const int maxX = srcWidth - KERNEL_SIZE_HALF - 1;
	const float *restrict src = task->src;
	const int dstWidth = task->dstWidth;
	float *restrict dst = task->dst;
	const int endRow = task->row + task->rows;
	const float inv = 1.0f / (float)(scale * scale);

	for (int oy = task->row; oy < endRow; oy++) {
		const int sy = oy * scale;
		for (int ox = 0; ox < dstWidth; ox++) {
			const int sx = ox * scale;
			float sum = 0.0f;
			for (int y = 0; y < scale; y++) {
				const float *restrict rowPtr = src + (sy + y) * srcWidth;
				for (int x = 0; x < scale; x++) {
					int px = sx + x;
					if (px < minX) px = minX;
					if (px > maxX) px = maxX;
					sum += rowPtr[px];
				}
			}
			dst[oy * dstWidth + ox] = sum * inv;
		}
	}
}

static void UpsampleAoRows(void *arg) {
	UpsampleTask *restrict task = arg;
	const int scale = AO_SMOOTH_DOWNSCALE;
	const int dstWidth = task->dstWidth;
	float *restrict dst = task->dst;
	const int srcWidth = task->srcWidth;
	const int srcHeight = task->srcHeight;
	const float *restrict src = task->src;
	const int *restrict x0Tab = task->x0Tab;
	const int *restrict x1Tab = task->x1Tab;
	const float *restrict txTab = task->txTab;
	const int endRow = task->row + task->rows;

	for (int y = task->row; y < endRow; y++) {
		const float fy = ((float)y + 0.5f) / (float)scale - 0.5f;
		int y0 = (int)floorf(fy);
		float ty = fy - (float)y0;
		if (y0 < 0) { y0 = 0; ty = 0.0f; }
		if (y0 >= srcHeight - 1) { y0 = srcHeight - 1; ty = 0.0f; }
		int y1 = y0 + 1;
		if (y1 >= srcHeight) y1 = srcHeight - 1;

		const float *restrict s0 = src + y0 * srcWidth;
		const float *restrict s1 = src + y1 * srcWidth;
		float *restrict d = dst + y * dstWidth;

		for (int x = 0; x < dstWidth; x++) {
			const int x0 = x0Tab[x];
			const int x1 = x1Tab[x];
			const float tx = txTab[x];

			const float top = s0[x0] + (s0[x1] - s0[x0]) * tx;
			const float bot = s1[x0] + (s1[x1] - s1[x0]) * tx;
			d[x] = top + (bot - top) * ty;
		}
	}
}

static void SmoothAmbientOcclusionWide(Camera *camera, ThreadPool *threadPool) {
	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int smallW = (width + AO_SMOOTH_DOWNSCALE - 1) / AO_SMOOTH_DOWNSCALE;
	const int smallH = (height + AO_SMOOTH_DOWNSCALE - 1) / AO_SMOOTH_DOWNSCALE;
	const int smallCount = smallW * smallH;
	if (smallW < KERNEL_SIZE || smallH < KERNEL_SIZE) return;

	float smallAO[smallCount];
	float smallDepth[smallCount];
	// constant depth keeps the reused blur passes a plain Gaussian; the wide stage is intentionally not edge-gated
	for (int i = 0; i < smallCount; i++) {
		smallDepth[i] = 100.0f;
	}

	const int downTaskCount = (smallH + AO_SMOOTH_ROWS_PER_TASK - 1) / AO_SMOOTH_ROWS_PER_TASK;
	DownsampleTask downTasks[downTaskCount];
	for (int t = 0; t < downTaskCount; t++) {
		const int row = t * AO_SMOOTH_ROWS_PER_TASK;
		const int rows = smallH - row < AO_SMOOTH_ROWS_PER_TASK ? smallH - row : AO_SMOOTH_ROWS_PER_TASK;
		downTasks[t] = (DownsampleTask){row, rows, width, camera->ambientOcclusionBuffer, smallW, smallAO};
		poolAdd(threadPool, DownsampleAoRows, &downTasks[t]);
	}
	poolWait(threadPool);

	for (int round = 0; round < AO_SMOOTH_ROUNDS; round++) {
		rowBlurMpBetter(smallW, smallH, smallAO, threadPool, smallDepth, AO_SMOOTH_BLUR_ROWS_PER_TASK);
		columnBlurMpBetter(smallW, smallH, smallAO, threadPool, smallDepth, AO_SMOOTH_BLUR_COLUMNS_PER_TASK);
	}

	const int upTaskCount = (height + AO_SMOOTH_ROWS_PER_TASK - 1) / AO_SMOOTH_ROWS_PER_TASK;
	// Built once per frame for the whole width instead of recomputed per output pixel.
	int upX0[width];
	int upX1[width];
	float upTx[width];
	for (int x = 0; x < width; x++) {
		const float fx = ((float)x + 0.5f) / (float)AO_SMOOTH_DOWNSCALE - 0.5f;
		int x0 = (int)floorf(fx);
		float tx = fx - (float)x0;
		if (x0 < 0) { x0 = 0; tx = 0.0f; }
		if (x0 >= smallW - 1) { x0 = smallW - 1; tx = 0.0f; }
		int x1 = x0 + 1;
		if (x1 >= smallW) x1 = smallW - 1;
		upX0[x] = x0;
		upX1[x] = x1;
		upTx[x] = tx;
	}
	UpsampleTask upTasks[upTaskCount];
	for (int t = 0; t < upTaskCount; t++) {
		const int row = t * AO_SMOOTH_ROWS_PER_TASK;
		const int rows = height - row < AO_SMOOTH_ROWS_PER_TASK ? height - row : AO_SMOOTH_ROWS_PER_TASK;
		upTasks[t] = (UpsampleTask){row, rows, width, camera->ambientOcclusionBuffer, smallW, smallH, smallAO, upX0, upX1, upTx};
		poolAdd(threadPool, UpsampleAoRows, &upTasks[t]);
	}
	poolWait(threadPool);
}

static void CalculateAmbientOcclusionV3(Camera *camera) {
	AmbientOcclusionTask task = {0, camera->screenHeight, camera};
	CalculateAmbientOcclusionV3Row(&task);
}

static void CalculateAmbientOcclusionMp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionRow, &tasks[t]);
	}
	poolWait(threadPool);
}

static void CalculateAmbientOcclusionV2Mp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2Row, &tasks[t]);
	}
	poolWait(threadPool);
}

static void CalculateAmbientOcclusionV2PlusMp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlus, &tasks[t]);
	}
	poolWait(threadPool);
}

static void CalculateAmbientOcclusionV2PlusColumn(Camera *camera) {
	AmbientOcclusionTask task = {0, camera->screenHeight, camera};
	CalculateAmbientOcclusionV2RowPlus(&task);
	columnBlur(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer);
}

static void CalculateAmbientOcclusionV2PlusColumnMp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlus, &tasks[t]);
	}
	poolWait(threadPool);
	columnBlurMp(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer, threadPool);
}

static void CalculateAmbientOcclusionV2PlusColumnMpPixelSkip(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlusSkip, &tasks[t]);
	}
	poolWait(threadPool);
	columnBlurMp(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer, threadPool);
}

static void CalculateAmbientOcclusionV2PlusColumnMpPixelSkipBetterBlur(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	camera->maxDepth = FLT_MIN;
	camera->minDepth = FLT_MAX;

	const int height = camera->screenHeight;
	const int taskCount = (height + AO_ROWS_PER_TASK - 1) / AO_ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * AO_ROWS_PER_TASK;
		const int rows = height - row < AO_ROWS_PER_TASK ? height - row : AO_ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlusSkipBetterBlur, &tasks[t]);
	}
	poolWait(threadPool);
	columnBlurMpBetter(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer, threadPool, camera->depthBuffer, COLUMNS_PER_TASK);
	SmoothAmbientOcclusionWide(camera, threadPool);
}

typedef struct {
	int row;	 // first row of the band
	int rows; // rows in the band
	int width;
	int height;
	float *AOBuffer;
	float strength; // 1 = full occlusion, >1 boosts contrast
	uint32 *Framebuffer;
} ApplyTask;

static void ApplyAoRows(void *arg) {
	ApplyTask *restrict task = arg;
	const int width = task->width;
	const int endRow = task->row + task->rows;
	const int firstColumn = KERNEL_SIZE_HALF;
	const int endColumn = width - KERNEL_SIZE_HALF;
	const float strength = task->strength;
	float *restrict aoBuffer = task->AOBuffer;
	uint32 *restrict framebuffer = task->Framebuffer;

	// Skip the outer columns: the AO passes never write them (stale data)
	for (int y = task->row; y < endRow; y++) {
		const int rowBase = y * width;
		for (int x = firstColumn; x < endColumn; x++) {
			const int idx = rowBase + x;
			const float ao = Clamp(1.0f - (1.0f - aoBuffer[idx]) * strength, 0.0f, 1.0f);
			const uint32 px = framebuffer[idx];
			const uint32 r = (uint32)(((px >> 16) & 0xFFu) * ao);
			const uint32 g = (uint32)(((px >> 8) & 0xFFu) * ao);
			const uint32 b = (uint32)((px & 0xFFu) * ao);

			framebuffer[idx] = (px & 0xFF000000u) | (r << 16) | (g << 8) | b;
		}
	}
}

static void applyAmbientOcclusion(Camera *camera, ThreadPool *threadPool, float strength) {
	if (!camera || !threadPool) return;

	CalculateAmbientOcclusionV2PlusColumnMpPixelSkipBetterBlur(camera, threadPool);

	const int width = camera->screenWidth;
	const int height = camera->screenHeight;
	const int taskCount = (height + AO_ROWS_PER_TASK - 1) / AO_ROWS_PER_TASK;
	ApplyTask tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int row = t * AO_ROWS_PER_TASK;
		const int rows = height - row < AO_ROWS_PER_TASK ? height - row : AO_ROWS_PER_TASK;
		tasks[t] = (ApplyTask){row, rows, width, height, camera->ambientOcclusionBuffer, strength, camera->framebuffer};
		poolAdd(threadPool, ApplyAoRows, &tasks[t]);
	}
	poolWait(threadPool);
}

static void CalculateAmbientOcclusionV2PlusColumnSgMp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlus, &tasks[t]);
	}
	poolWait(threadPool);
	columnBlur(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer);
}

static void CalculateAmbientOcclusionV3Mp(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV3Row, &tasks[t]);
	}
	poolWait(threadPool);
}

// NOTE: test open cl implementation
// No longer needed because cpu implementation is faster enough