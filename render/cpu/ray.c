#include "../../math/matrix.h"

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera, int resolution) {
	if (!objects || objectCount <= 0 || !camera) return;
	int width = camera->screenWidth;
	int height = camera->screenHeight;
	float3 lightDir = camera->lightDir;
	float bias = 0.01f;
	int step = MaxF32(1, resolution);

	// Clear shadow buffer
	for (int i = 0; i < width * height; i++) {
		camera->tempBuffer_1[i] = (float3){1.0f, 1.0f, 1.0f};
	}

	// Precompute per-object data for better cache coherence
	typedef struct {
		float3 worldBBmin;
		float3 worldBBmax;
	} ObjectShadowData;
	
	ObjectShadowData *shadowData = (ObjectShadowData *)malloc(sizeof(ObjectShadowData) * objectCount);
	if (!shadowData) return;
	
	for (int i = 0; i < objectCount; i++) {
		shadowData[i].worldBBmin = objects[i].worldBBmin;
		shadowData[i].worldBBmax = objects[i].worldBBmax;
	}

	// Shadow ray casting with optimized bounding box tests
	for (int y = 0; y < height; y += step) {
		for (int x = 0; x < width; x += step) {
			int idx = y * width + x;
			if (camera->depthBuffer[idx] >= FLT_MAX || camera->depthBuffer[idx] <= 0.0f) continue;

			float3 worldPos = camera->positionBuffer[idx];
			float3 normal = camera->normalBuffer[idx];

			if (normal.y < 0.5f) continue;

			float3 biasedPos = Float3_Add(worldPos, Float3_Scale(normal, bias));

			// Early-out shadow test using precomputed world bounds
			bool inShadow = false;
			for (int objIdx = 0; objIdx < objectCount && !inShadow; objIdx++) {
				float3 rayOrigin = biasedPos;
				float3 rayDir = lightDir;
				
				// Ray-box intersection using precomputed world bounds
				float3 invDir = {
					fabsf(rayDir.x) > 1e-6f ? 1.0f / rayDir.x : 1e6f,
					fabsf(rayDir.y) > 1e-6f ? 1.0f / rayDir.y : 1e6f,
					fabsf(rayDir.z) > 1e-6f ? 1.0f / rayDir.z : 1e6f
				};
				
				float3 t0 = {
					(shadowData[objIdx].worldBBmin.x - rayOrigin.x) * invDir.x,
					(shadowData[objIdx].worldBBmin.y - rayOrigin.y) * invDir.y,
					(shadowData[objIdx].worldBBmin.z - rayOrigin.z) * invDir.z
				};
				
				float3 t1 = {
					(shadowData[objIdx].worldBBmax.x - rayOrigin.x) * invDir.x,
					(shadowData[objIdx].worldBBmax.y - rayOrigin.y) * invDir.y,
					(shadowData[objIdx].worldBBmax.z - rayOrigin.z) * invDir.z
				};
				
				float tmin = MaxF32(MaxF32(MinF32(t0.x, t1.x), MinF32(t0.y, t1.y)), MinF32(t0.z, t1.z));
				float tmax = MinF32(MinF32(MaxF32(t0.x, t1.x), MaxF32(t0.y, t1.y)), MaxF32(t0.z, t1.z));
				
				if (tmax >= tmin && tmax > 0.0f) {
					inShadow = true;
				}
			}

			if (inShadow) {
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
	
	free(shadowData);
	
	BlurBuffer(camera->tempBuffer_1, camera->tempBuffer_2, width, height, 5);
	for (int i = 0; i < width * height; i++) {
		float shadowAmount = 1.0f - camera->tempBuffer_1[i].x;
		camera->framebuffer[i] = DarkenColor(camera->framebuffer[i], shadowAmount * 0.5f);
	}
}