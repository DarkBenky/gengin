#include "format.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../math/vector3.h"

#define ALIGN64(n) (((n) + 63) & ~(size_t)63)

void clearBuffers(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	// framebuffer: cleared to black background each frame.
	// memset(camera->framebuffer, 0, size * sizeof(uint32));
	// memset(camera->accumulationBuffer, 0, size * sizeof(int4));
	// depthBuffer: 0x7F7F7F7F ~= 3.4e38, indistinguishable from FLT_MAX for depth comparisons.
	// memset(camera->depthBuffer, 0x7F, size * sizeof(float));
	// normalBuffer, positionBuffer, reflectBuffer: only read at pixels where
	// depthBuffer < FLT_MAX (i.e. pixels written by RenderObject), so no clear needed.
	// tempBuffer_1: fully overwritten with 1.0f in ShadowPostProcess before any read.
	// tempBuffer_2: written by blur horizontal pass before vertical pass reads it.
	// objectIdBuffer: fully overwritten by ray tracer each frame.
	// tempFramebuffer/2, tempBuffer_3: not read before being written each frame.
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
	camera->accumulationBuffer = (int4 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(int4)));
	camera->depthBuffer = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->tempFramebuffer = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->tempFramebuffer2 = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->shadowCache = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->reflectCache = (Color *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(Color)));
	camera->objectIdBuffer = (int *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(int)));
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
	free(camera->objectIdBuffer);
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
	camera->objectIdBuffer = NULL;
}

void CameraMoveForward(Camera *camera, float amount) {
	if (!camera) return;
	camera->position.x += camera->forward.x * amount;
	camera->position.y += camera->forward.y * amount;
	camera->position.z += camera->forward.z * amount;
}

void CameraMoveRight(Camera *camera, float amount) {
	if (!camera) return;
	float3 right = Float3_Cross(camera->forward, (float3){0.0f, 1.0f, 0.0f});
	camera->position.x += right.x * amount;
	camera->position.y += right.y * amount;
	camera->position.z += right.z * amount;
}

void CameraMoveUp(Camera *camera, float amount) {
	if (!camera) return;
	camera->position.y += amount;
}

void CameraRotate(Camera *camera, float pitch, float yaw) {
	if (!camera) return;
	// rotate forward vector by yaw (around world up), then pitch (around camera right)
	float cosYaw = cosf(yaw);
	float sinYaw = sinf(yaw);
	float3 fwd = camera->forward;
	// yaw
	float3 fwd1 = {
		fwd.x * cosYaw - fwd.z * sinYaw,
		fwd.y,
		fwd.x * sinYaw + fwd.z * cosYaw};
	// pitch — rotate around camera's right axis
	float3 right = Float3_Normalize(Float3_Cross((float3){0.0f, 1.0f, 0.0f}, fwd1));
	float3 camUp = Float3_Cross(right, fwd1); // points upward relative to forward
	float cosPitch = cosf(pitch);
	float sinPitch = sinf(pitch);
	camera->forward = Float3_Normalize((float3){
		fwd1.x * cosPitch + camUp.x * sinPitch,
		fwd1.y * cosPitch + camUp.y * sinPitch,
		fwd1.z * cosPitch + camUp.z * sinPitch});
}