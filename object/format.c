#include "format.h"
#include <stdlib.h>
#include <string.h>

void clearBuffers(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	memset(camera->framebuffer, 0, size * sizeof(uint32));
	memset(camera->normalBuffer, 0, size * sizeof(float3));
	for (int i = 0; i < size; i++) {
		camera->depthBuffer[i] = FLT_MAX;
	}
}

void initCamera(Camera *camera, int screenWidth, int screenHeight, float fov, float3 position, float3 forward) {
	if (!camera) return;
	camera->screenWidth = screenWidth;
	camera->screenHeight = screenHeight;
	camera->fov = fov;
	camera->position = position;
	camera->forward = forward;
	camera->framebuffer = (uint32 *)malloc(screenWidth * screenHeight * sizeof(uint32));
	camera->normalBuffer = (float3 *)malloc(screenWidth * screenHeight * sizeof(float3));
	camera->depthBuffer = (float *)malloc(screenWidth * screenHeight * sizeof(float));
	clearBuffers(camera);
}