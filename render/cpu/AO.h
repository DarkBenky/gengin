#include "../../object/format.h"
#include "../../math/vector3.h"
#include <stdio.h>
#include <stdbool.h>

#define EPSILON 1e-6f
#define MAX_FLOAT 3.402823466e+38F
#define SAMPLES 8

static const float2 SAMPLES_PATTERN[SAMPLES] = {
	{0.741f, -0.650f},
	{-0.274f, -0.382f},
	{0.656f, -0.469f},
	{0.052f, 0.628f},
	{-0.647f, 0.510f},
	{0.453f, 0.192f},
	{-0.403f, -0.519f},
	{0.127f, 0.012f},
};

// TODO: test different implementations of ambient occlusion and find fastest one
static void CalculateAmbientOcclusion(Camera *camera) {
	const float pixelRadius = 16.0f; // sample spread in pixels
	const float worldRadius = 20.5f; // max world-space distance (scene units)
	const float bias = worldRadius * 0.02f;

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

			for (int k = 0; k < SAMPLES; k++) {
				float sampleX = j + SAMPLES_PATTERN[k].x * pixelRadius;
				float sampleY = i + SAMPLES_PATTERN[k].y * pixelRadius;

				if (sampleX < 0 || sampleX >= camera->screenWidth ||
					sampleY < 0 || sampleY >= camera->screenHeight) {
					continue;
				}

				int sampleIndex = (int)sampleY * camera->screenWidth + (int)sampleX;
				float3 samplePos = camera->positionBuffer[sampleIndex];

				float3 dir = Float3_Sub(samplePos, position);
				float dist = Float3_Length(dir);

				if (dist < bias || dist > worldRadius) {
					continue;
				}

				float3 dirNorm = Float3_Normalize(dir);
				float facing = Float3_Dot(normal, dirNorm);
				if (facing <= EPSILON) {
					continue;
				}

				float distWeight = 1.0f - (dist / worldRadius);
				occlusion += facing * distWeight;
				validSamples++;
			}

			float ao = validSamples > 0 ? occlusion / validSamples : 1.0f;
			camera->ambientOcclusionBuffer[idx] = ao;
		}
	}
}