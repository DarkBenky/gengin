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
// Depth-reject scale of the two AVX2 blur helpers.  The scalar blur bodies
// still spell the same literal out; a future tune must change both.
#define AO_DEPTH_REJECT_SCALE 0.02f
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

			float ao = Clamp(1.0f - occlusion * INV_VALID[validSamples], 0.0f, 1.0f);
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
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

/* ------------------------------------------------------------------ *
 *  AVX2 helpers for the two depth-weighted AO blur passes: the per-row
 *  horizontal blur and the full-frame vertical (column) blur.  Both are the
 *  same 5-tap depth-gated average, only the tap stride differs (1 for the row
 *  blur, `width` for the column blur), so the arithmetic lives in one place.
 * ------------------------------------------------------------------ */
static inline __m256 aoAbs8(__m256 x) {
	return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x);
}

/* Depth-weighted 5-tap blur of 8 lanes.  `values` and `depths` point at the
 * CENTER tap; tap k = -2..+2 sits `stride` elements away.  Tap order, Clamp
 * and the `weightSum > 1e-5f` fallback are the scalar expressions, lane by
 * lane.  Deliberately not `restrict`: the column blur reads and writes the
 * same buffer, so a store must not be reordered before these loads. */
static inline __m256 aoBlurTap8(const float *values, int vstride,
                                const float *depths, int dstride) {
	const __m256 k0 = _mm256_set1_ps(lineKernelvaluse[0]);
	const __m256 k1 = _mm256_set1_ps(lineKernelvaluse[1]);
	const __m256 k2 = _mm256_set1_ps(lineKernelvaluse[2]);
	const __m256 k3 = _mm256_set1_ps(lineKernelvaluse[3]);
	const __m256 k4 = _mm256_set1_ps(lineKernelvaluse[4]);
	const __m256 dscale = _mm256_set1_ps(AO_DEPTH_REJECT_SCALE);
	const __m256 zero = _mm256_setzero_ps();
	const __m256 one = _mm256_set1_ps(1.0f);
	const __m256 eps = _mm256_set1_ps(1e-5f);

	const __m256 cd = _mm256_loadu_ps(depths);
	const __m256 invDepthThreshold = _mm256_div_ps(one, _mm256_mul_ps(cd, dscale));

	__m256 sum = zero;
	__m256 weightSum = zero;

#define AO_BLUR_TAP(K, OFF)                                                                       \
	{                                                                                             \
		const __m256 tap = _mm256_max_ps(                                                         \
		    zero,                                                                                 \
		    _mm256_min_ps(_mm256_sub_ps(one,                                                      \
		                                _mm256_mul_ps(aoAbs8(_mm256_sub_ps(                       \
		                                                 _mm256_loadu_ps(depths + (OFF) * dstride),\
		                                                 cd)),                                    \
		                                              invDepthThreshold)),                        \
		                  one));                                                                  \
		const __m256 w = _mm256_mul_ps(k##K, tap);                                                \
		sum = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_loadu_ps(values + (OFF) * vstride), w));     \
		weightSum = _mm256_add_ps(weightSum, w);                                                  \
	}
	AO_BLUR_TAP(0, -2)
	AO_BLUR_TAP(1, -1)
	AO_BLUR_TAP(2, 0)
	AO_BLUR_TAP(3, 1)
	AO_BLUR_TAP(4, 2)
#undef AO_BLUR_TAP

	return _mm256_blendv_ps(_mm256_loadu_ps(values), _mm256_div_ps(sum, weightSum),
	                        _mm256_cmp_ps(weightSum, eps, _CMP_GT_OQ));
}

/* Horizontal blur of one row out of the caller's rowValues/rowDepths copies:
 * 8 pixels per iteration, scalar remainder.  The copies are distinct objects
 * from dstRow, so the store cannot perturb the taps. */
static void aoRowBlurAvx8(const float *rowValues, const float *rowDepths,
                          float *dstRow, int width) {
	const float depthRejectScale = AO_DEPTH_REJECT_SCALE;
	const int end = width - KERNEL_SIZE_HALF;
	int j = KERNEL_SIZE_HALF;

	for (; j + 8 <= end; j += 8) {
		_mm256_storeu_ps(dstRow + j, aoBlurTap8(rowValues + j, 1, rowDepths + j, 1));
	}
	// scalar remainder, identical to the original loop body
	for (; j < end; j++) {
		const float centerDepth = rowDepths[j];
		const float invDepthThreshold = 1.0f / (centerDepth * depthRejectScale);

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
		dstRow[j] = weightSum > 1e-5f ? sum / weightSum : rowValues[j];
	}
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

		// 8 pixels at a time, kernel size half to width - kernel size half
		aoRowBlurAvx8(rowValues, rowDepths, camera->ambientOcclusionBuffer + row * width, width);
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

/* Vertical blur of 8 adjacent columns.  `firstColumn..firstColumn+columnCount`
 * is the band; only whole groups of 8 are handled here (the caller runs the
 * scalar remainder).  The result of row y-2 is stored only after row y's taps
 * have been read, which is what makes the in-place update equivalent to the
 * scalar body's copy-the-column-first rule: output row y-1 and later never read
 * source row y-2 again, and row y's fallback centre value is still pristine
 * (row y is only written two iterations later).
 * `noinline` keeps the caller's `restrict` (and its stack VLA) out of this
 * block: the store into `image` must stay ordered after the tap loads. */
__attribute__((noinline))
static void aoColBlurAvx8(float *image, const float *depthBuffer, int width, int height,
                          int firstColumn, int columnCount) {
	const int last = height - KERNEL_SIZE_HALF;
	if (height < KERNEL_SIZE) return; // no output row exists

	for (int x = firstColumn; x + 8 <= firstColumn + columnCount; x += 8) {
		__m256 prev1 = _mm256_setzero_ps();
		__m256 prev2 = _mm256_setzero_ps();
		int computed = 0;

		for (int y = KERNEL_SIZE_HALF; y < last; y++) {
			const float *centerTap = image + y * width + x;
			float *center = image + y * width + x;
			const __m256 res = aoBlurTap8(centerTap, width, depthBuffer + y * width + x, width);

			if (computed >= 2) {
				_mm256_storeu_ps(center - 2 * width, prev2);
			}
			prev2 = prev1;
			prev1 = res;
			computed++;
		}
		if (computed >= 2) {
			_mm256_storeu_ps(image + (last - 2) * width + x, prev2);
		}
		if (computed >= 1) {
			_mm256_storeu_ps(image + (last - 1) * width + x, prev1);
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
	const float depthRejectScale = 0.02f; // tune: smaller = stricter edge preservation

	float colValues[height];

	// 8 columns at a time (AVX2); the scalar per-column body below keeps
	// handling any band remainder
	const int bandColumns = endColumn - task->column;
	const int vectorColumns = bandColumns - (bandColumns % 8);
	if (vectorColumns > 0) {
		aoColBlurAvx8(image, depthBuffer, width, height, task->column, vectorColumns);
	}

	for (int x = task->column + vectorColumns; x < endColumn; x++) {
		for (int y = 0; y < height; y++) {
			colValues[y] = image[y * width + x];
		}

		// start from kernel size half and end early to avoid bound checks
		for (int y = KERNEL_SIZE_HALF; y < height - KERNEL_SIZE_HALF; y++) {
			const int centerIdx = y * width + x;
			const float centerDepth = depthBuffer[centerIdx];
			const float invDepthThreshold = 1.0f / (centerDepth * depthRejectScale);

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

static void columnBlurMpBetter(int width, int height, float *restrict image, ThreadPool *threadPool, float *restrict depthBuffer) {
	// Same interior range as columnBlur(): the outer columns hold stale data.
	const int firstColumn = KERNEL_SIZE_HALF;
	const int endColumn = width - KERNEL_SIZE_HALF;
	const int taskCount = (endColumn - firstColumn + COLUMNS_PER_TASK - 1) / COLUMNS_PER_TASK;
	ColumnBlurTaskBetter tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int column = firstColumn + t * COLUMNS_PER_TASK;
		const int columns = endColumn - column < COLUMNS_PER_TASK ? endColumn - column : COLUMNS_PER_TASK;
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
	const float depthRejectScale = 0.02f; // tune: smaller = stricter edge preservation

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

static void rowBlurMpBetter(int width, int height, float *restrict image, ThreadPool *threadPool, float *restrict depthBuffer) {
	// Same interior row range as the column blur: the outer rows are never blurred vertically
	const int firstRow = KERNEL_SIZE_HALF;
	const int endRow = height - KERNEL_SIZE_HALF;
	const int taskCount = (endRow - firstRow + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	RowBlurTaskBetter tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int row = firstRow + t * ROWS_PER_TASK;
		const int rows = endRow - row < ROWS_PER_TASK ? endRow - row : ROWS_PER_TASK;
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
	const int endRow = task->row + task->rows;

	for (int y = task->row; y < endRow; y++) {
		const float fy = ((float)y + 0.5f) / (float)scale - 0.5f;
		int y0 = (int)floorf(fy);
		float ty = fy - (float)y0;
		if (y0 < 0) { y0 = 0; ty = 0.0f; }
		if (y0 >= srcHeight - 1) { y0 = srcHeight - 1; ty = 0.0f; }
		int y1 = y0 + 1;
		if (y1 >= srcHeight) y1 = srcHeight - 1;

		for (int x = 0; x < dstWidth; x++) {
			const float fx = ((float)x + 0.5f) / (float)scale - 0.5f;
			int x0 = (int)floorf(fx);
			float tx = fx - (float)x0;
			if (x0 < 0) { x0 = 0; tx = 0.0f; }
			if (x0 >= srcWidth - 1) { x0 = srcWidth - 1; tx = 0.0f; }
			int x1 = x0 + 1;
			if (x1 >= srcWidth) x1 = srcWidth - 1;

			const float top = src[y0 * srcWidth + x0] + (src[y0 * srcWidth + x1] - src[y0 * srcWidth + x0]) * tx;
			const float bot = src[y1 * srcWidth + x0] + (src[y1 * srcWidth + x1] - src[y1 * srcWidth + x0]) * tx;
			dst[y * dstWidth + x] = top + (bot - top) * ty;
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
		rowBlurMpBetter(smallW, smallH, smallAO, threadPool, smallDepth);
		columnBlurMpBetter(smallW, smallH, smallAO, threadPool, smallDepth);
	}

	const int upTaskCount = (height + AO_SMOOTH_ROWS_PER_TASK - 1) / AO_SMOOTH_ROWS_PER_TASK;
	UpsampleTask upTasks[upTaskCount];
	for (int t = 0; t < upTaskCount; t++) {
		const int row = t * AO_SMOOTH_ROWS_PER_TASK;
		const int rows = height - row < AO_SMOOTH_ROWS_PER_TASK ? height - row : AO_SMOOTH_ROWS_PER_TASK;
		upTasks[t] = (UpsampleTask){row, rows, width, camera->ambientOcclusionBuffer, smallW, smallH, smallAO};
		poolAdd(threadPool, UpsampleAoRows, &upTasks[t]);
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

static void CalculateAmbientOcclusionV2PlusColumnMpPixelSkipBetterBlur(Camera *camera, ThreadPool *threadPool) {
	if (!camera || !threadPool) return;

	camera->maxDepth = FLT_MIN;
	camera->minDepth = FLT_MAX;

	const int height = camera->screenHeight;
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	AmbientOcclusionTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (AmbientOcclusionTask){row, rows, camera};
		poolAdd(threadPool, CalculateAmbientOcclusionV2RowPlusSkipBetterBlur, &tasks[t]);
	}
	poolWait(threadPool);
	columnBlurMpBetter(camera->screenWidth, camera->screenHeight, camera->ambientOcclusionBuffer, threadPool, camera->depthBuffer);
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
	const int taskCount = (height + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
	ApplyTask tasks[taskCount];

	for (int t = 0; t < taskCount; t++) {
		const int row = t * ROWS_PER_TASK;
		const int rows = height - row < ROWS_PER_TASK ? height - row : ROWS_PER_TASK;
		tasks[t] = (ApplyTask){row, rows, width, height, camera->ambientOcclusionBuffer, strength, camera->framebuffer};
		poolAdd(threadPool, ApplyAoRows, &tasks[t]);
	}
	poolWait(threadPool);
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

// NOTE: test open cl implementation
// No longer needed because cpu implementation is faster enough