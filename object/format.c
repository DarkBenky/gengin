#include "format.h"
#include <stdlib.h>
#include <string.h>

void clearBuffers(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	memset(camera->framebuffer, 0, size * sizeof(uint32));
	memset(camera->normalBuffer, 0, size * sizeof(float3));
	memset(camera->positionBuffer, 0, size * sizeof(float3));
	memset(camera->reflectBuffer, 0, size * sizeof(float3));
	memset(camera->tempBuffer_1, 0, size * sizeof(float3));
	memset(camera->tempBuffer_2, 0, size * sizeof(float3));
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
	camera->framebuffer = (uint32 *)malloc(screenWidth * screenHeight * sizeof(uint32));
	camera->normalBuffer = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->positionBuffer = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->reflectBuffer = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->tempBuffer_1 = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->tempBuffer_2 = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->depthBuffer = (float *)malloc(screenWidth * screenHeight * sizeof(float));
	clearBuffers(camera);
}

void destroyCamera(Camera *camera) {
	if (!camera) return;
	free(camera->framebuffer);
	free(camera->normalBuffer);
	free(camera->positionBuffer);
	free(camera->reflectBuffer);
	free(camera->tempBuffer_1);
	free(camera->tempBuffer_2);
	free(camera->depthBuffer);
	camera->framebuffer = NULL;
	camera->normalBuffer = NULL;
	camera->positionBuffer = NULL;
	camera->reflectBuffer = NULL;
	camera->tempBuffer_1 = NULL;
	camera->tempBuffer_2 = NULL;
	camera->depthBuffer = NULL;
}