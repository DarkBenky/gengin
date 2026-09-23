#include "../../object/format.h"
#include "../../math/vector3.h"
#include "../../util/threadPool.h"
#include <immintrin.h>
#include <stdbool.h>

#define MAX_FLOAT 3.402823466e+38F
#define SAMPLES 8
#define VARIATIONS 8
// Rows handed to one pool task: bigger bands cut per-task overhead, smaller ones balance better
// Swept 1..64 rows/task on AOBench: 1 and 2 tie at the top, 8+ loses to balance, 16+ idles threads
#ifndef ROWS_PER_TASK
#define ROWS_PER_TASK 2
#endif
// Columns handed to one pool task for the column blur; wider bands amortize the strided sweep's cache lines
// Swept 2..64 columns/task on AOBench: 16 best (~6.2ms median), 2 worst (~7.9ms, 638 tiny tasks); 1 exceeds the pool ring
#ifndef COLUMNS_PER_TASK
#define COLUMNS_PER_TASK 16
#endif

#define PIXEL_SKIP 4

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

static void CalculateAmbientOcclusion(Camera *camera) {
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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

			float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
}

static inline float Clamp(float val, float lower, float upper) {
	if (val < lower) return lower;
	if (val > upper) return upper;
	return val;
}

static void CalculateAmbientOcclusionV2(Camera *camera) {
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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

			float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

			float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

			float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

			float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

				ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

				float ao = validSamples > 0 ? occlusion * INV_VALID[validSamples] : 1.0f;
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

static void CalculateAmbientOcclusionV3(Camera *camera) {
	AmbientOcclusionTask task = {0, camera->screenHeight, camera};
	CalculateAmbientOcclusionV3Row(&task);
}

static void CalculateAmbientOcclusionMp(Camera *camera, ThreadPool *threadPool) {
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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

// TODO: Too slow we need to make some resolution based version
static void CalculateAmbientOcclusionV2PlusColumnMp(Camera *camera, ThreadPool *threadPool) {
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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
	// TODO: Apply blur pass
	// TODO: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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

static void CalculateAmbientOcclusionV2PlusColumnSgMp(Camera *camera, ThreadPool *threadPool) {
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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
	// TODO: Apply blur pass
	// NOTE: Make it edge-aware Cheapest guard is to reject neighbour taps whose depthBuffer differs too much (or whose normal dot < ~0.8)
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

// TODO: test open cl implementation