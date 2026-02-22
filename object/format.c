#include "format.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN64(n) (((n) + 63) & ~(size_t)63)

void clearBuffers(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	memset(camera->framebuffer, 0, size * sizeof(uint32));
	memset(camera->normalBuffer, 0, size * sizeof(float3));
	memset(camera->positionBuffer, 0, size * sizeof(float3));
	memset(camera->reflectBuffer, 0, size * sizeof(float3));
	memset(camera->tempBuffer_1, 0, size * sizeof(float));
	memset(camera->tempBuffer_2, 0, size * sizeof(float));
	memset(camera->tempBuffer_3, 0, size * sizeof(float));
	memset(camera->tempFramebuffer, 0, size * sizeof(Color));
	memset(camera->tempFramebuffer2, 0, size * sizeof(Color));
	for (int i = 0; i < size; i++) {
		camera->depthBuffer[i] = FLT_MAX;
	}
}

void initCamera(Camera *camera, int screenWidth, int screenHeight, float fov, float3 position, float3 forward, float3 lightDir) {
	if (!camera) return;
	camera->screenWidth = screenWidth;
	camera->screenHeight = screenHeight;
	camera->fov = fov;
	camera->position = position;
	camera->lightDir = lightDir;
	camera->forward = forward;
	camera->framebuffer = (uint32 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(uint32)));
	camera->normalBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->positionBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->reflectBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->tempBuffer_1 = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->tempBuffer_2 = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->tempBuffer_3 = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->depthBuffer = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->tempFramebuffer = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->tempFramebuffer2 = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->shadowCache = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->reflectCache = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->frameCounter = 0;
	clearBuffers(camera);
	int size = screenWidth * screenHeight;
	for (int i = 0; i < size; i++)
		camera->shadowCache[i] = 1.0f;
	memset(camera->reflectCache, 0, size * sizeof(Color));
}

void destroyCamera(Camera *camera) {
	if (!camera) return;
	free(camera->framebuffer);
	free(camera->normalBuffer);
	free(camera->positionBuffer);
	free(camera->reflectBuffer);
	free(camera->tempBuffer_1);
	free(camera->tempBuffer_2);
	free(camera->tempBuffer_3);
	free(camera->depthBuffer);
	free(camera->tempFramebuffer);
	free(camera->tempFramebuffer2);
	free(camera->shadowCache);
	free(camera->reflectCache);
	camera->framebuffer = NULL;
	camera->normalBuffer = NULL;
	camera->positionBuffer = NULL;
	camera->reflectBuffer = NULL;
	camera->tempBuffer_1 = NULL;
	camera->tempBuffer_2 = NULL;
	camera->tempBuffer_3 = NULL;
	camera->depthBuffer = NULL;
	camera->tempFramebuffer = NULL;
	camera->tempFramebuffer2 = NULL;
	camera->shadowCache = NULL;
	camera->reflectCache = NULL;
}