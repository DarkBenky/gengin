#include "ray.h"

#include <math.h>
#include <stdlib.h>

#include "../../math/scalar.h"
#include "../../math/transform.h"
#include "../../math/vector3.h"

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

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera) {
	if (!objects || objectCount <= 0 || !camera) return;
	// TODO: Implement a more efficient shadow post-process, this is just a simple blur for demonstration
}