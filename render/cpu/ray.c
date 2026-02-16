#include "ray.h"

#include <math.h>
#include <stdlib.h>

#include "../../math/scalar.h"
#include "../../math/transform.h"
#include "../../math/vector3.h"
#include "../color/color.h"

void BlurBuffer(float3 *src, float3 *temp, int width, int height, int radius) {
	if (radius <= 0) return;

	float invSize = 1.0f / (2 * radius + 1);

	for (int y = 0; y < height; y++) {
		int rowStart = y * width;
		float sum = 0.0f;

		for (int x = 0; x < radius; x++) {
			sum += src[rowStart + x].x;
		}

		for (int x = 0; x < width; x++) {
			int right = x + radius;
			int left = x - radius - 1;

			if (right < width) sum += src[rowStart + right].x;
			if (left >= 0) sum -= src[rowStart + left].x;

			temp[rowStart + x].x = sum * invSize;
		}
	}

	for (int x = 0; x < width; x++) {
		float sum = 0.0f;

		for (int y = 0; y < radius; y++) {
			sum += temp[y * width + x].x;
		}

		for (int y = 0; y < height; y++) {
			int bottom = y + radius;
			int top = y - radius - 1;

			if (bottom < height) sum += temp[bottom * width + x].x;
			if (top >= 0) sum -= temp[top * width + x].x;

			float v = sum * invSize;
			src[y * width + x] = (float3){v, v, v};
		}
	}
}

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera, int resolution) {
	if (!objects || objectCount <= 0 || !camera) return;
	int width = camera->screenWidth;
	int height = camera->screenHeight;
	float3 lightDir = camera->lightDir;
	float bias = 0.01f;
	int step = MaxF32(1, resolution);

	for (int i = 0; i < width * height; i++) {
		camera->tempBuffer_1[i] = (float3){1.0f, 1.0f, 1.0f};
	}

	for (int y = 0; y < height; y += step) {
		for (int x = 0; x < width; x += step) {
			int idx = y * width + x;
			if (camera->depthBuffer[idx] >= FLT_MAX || camera->depthBuffer[idx] <= 0.0f) continue;

			float3 worldPos = camera->positionBuffer[idx];
			float3 normal = camera->normalBuffer[idx];

			if (normal.y < 0.5f) continue;

			float3 biasedPos = Float3_Add(worldPos, Float3_Scale(normal, bias));

			if (IntersectAnyBBox(objects, objectCount, biasedPos, lightDir)) {
				for (int dy = 0; dy < step && y + dy < height; dy++) {
					for (int dx = 0; dx < step && x + dx < width; dx++) {
						int fillIdx = (y + dy) * width + (x + dx);
						if (camera->depthBuffer[fillIdx] < FLT_MAX && camera->depthBuffer[fillIdx] > 0.0f) {
							float3 fillNormal = camera->normalBuffer[fillIdx];
							if (fillNormal.y >= 0.5f) {
								camera->tempBuffer_1[fillIdx] = (float3){0.0f, 0.0f, 0.0f};
							}
						}
					}
				}
			}
		}
	}
	BlurBuffer(camera->tempBuffer_1, camera->tempBuffer_2, width, height, 5);
	for (int i = 0; i < width * height; i++) {
		float shadowAmount = 1.0f - camera->tempBuffer_1[i].x;
		camera->framebuffer[i] = DarkenColor(camera->framebuffer[i], shadowAmount * 0.5f);
	}
}