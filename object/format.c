#include "format.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../math/vector3.h"

#define ALIGN64(n) (((n) + 63) & ~(size_t)63)

Error Error_Make(int code, const char *msg) {
	Error err;
	err.err = code;
	if (msg)
		snprintf(err.msg, sizeof(err.msg), "%s", msg);
	else
		err.msg[0] = '\0';
	return err;
}

Error Error_Ok(void) {
	return Error_Make(nil, "OK");
}

void clearBuffers(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	// framebuffer: cleared to black background each frame.
	// memset(camera->framebufferLdr, 0, size * sizeof(uint32));
	// depthBuffer: 0x7F7F7F7F ~= 3.4e38, indistinguishable from FLT_MAX for depth comparisons.
	// memset(camera->depthBuffer, 0x7F, size * sizeof(float));
	// normalBuffer, positionBuffer, reflectBuffer: only read at pixels where
	// depthBuffer < FLT_MAX (i.e. pixels written by RenderObject), so no clear needed.
	// tempBuffer_1: fully overwritten with 1.0f in ShadowPostProcess before any read.
	// tempBuffer_2: written by blur horizontal pass before vertical pass reads it.
	// objectIdBuffer: fully overwritten by ray tracer each frame.
}

static void freeCameraBuffers(Camera *camera) {
	free(camera->framebufferLdr);
	free(camera->framebufferHdr);
	free(camera->tempFramebufferHdr);
	free(camera->normalBuffer);
	free(camera->positionBuffer);
	free(camera->reflectBuffer);
	free(camera->bloomBuffer);
	free(camera->bloomTemp);
	free(camera->bloomDst);
	free(camera->depthBuffer);
	free(camera->ambientOcclusionBuffer);
	free(camera->reflectCache);
	free(camera->tempBuffer_1);
	free(camera->tempBuffer_2);
	free(camera->shadowCache);
	free(camera->objectIdBuffer);
	free(camera->uvBuffer);
	free(camera->triangleIdBuffer);
	free(camera->motionVectorBuffer);
	camera->framebufferLdr = NULL;
	camera->framebufferHdr = NULL;
	camera->tempFramebufferHdr = NULL;
	camera->normalBuffer = NULL;
	camera->positionBuffer = NULL;
	camera->reflectBuffer = NULL;
	camera->bloomBuffer = NULL;
	camera->bloomTemp = NULL;
	camera->bloomDst = NULL;
	camera->depthBuffer = NULL;
	camera->ambientOcclusionBuffer = NULL;
	camera->reflectCache = NULL;
	camera->tempBuffer_1 = NULL;
	camera->tempBuffer_2 = NULL;
	camera->shadowCache = NULL;
	camera->objectIdBuffer = NULL;
	camera->uvBuffer = NULL;
	camera->triangleIdBuffer = NULL;
	camera->motionVectorBuffer = NULL;
}

static int cameraBuffersAllocated(const Camera *camera) {
	return camera->framebufferLdr && camera->framebufferHdr && camera->tempFramebufferHdr &&
		   camera->normalBuffer && camera->positionBuffer && camera->reflectBuffer &&
		   camera->bloomBuffer && camera->bloomTemp && camera->bloomDst &&
		   camera->depthBuffer && camera->ambientOcclusionBuffer && camera->reflectCache &&
		   camera->tempBuffer_1 && camera->tempBuffer_2 && camera->shadowCache &&
		   camera->objectIdBuffer && camera->uvBuffer && camera->triangleIdBuffer &&
		   camera->motionVectorBuffer;
}

void initCamera(Camera *camera, int screenWidth, int screenHeight, float fov, float3 position, float3 forward, float3 lightDir) {
	if (!camera) return;
	camera->screenWidth = screenWidth;
	camera->screenHeight = screenHeight;
	camera->fov = fov;
	camera->position = position;
	camera->lightDir = lightDir;
	camera->forward = forward;
	camera->framebufferLdr = (uint32 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(uint32)));
	camera->framebufferHdr = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->tempFramebufferHdr = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->normalBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->positionBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->reflectBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->bloomBuffer = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->bloomTemp = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->bloomDst = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->depthBuffer = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->ambientOcclusionBuffer = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->reflectCache = (float3 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float3)));
	camera->tempBuffer_1 = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->tempBuffer_2 = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->shadowCache = (float *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float)));
	camera->objectIdBuffer = (int *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(int)));
	camera->uvBuffer = (uvMap *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(uvMap)));
	camera->triangleIdBuffer = (int *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(int)));
	camera->motionVectorBuffer = (float2 *)aligned_alloc(64, ALIGN64(screenWidth * screenHeight * sizeof(float2)));
	if (!cameraBuffersAllocated(camera)) {
		freeCameraBuffers(camera);
		return;
	}
	camera->frameCounter = 0;
	camera->maxDepth = 0.0f;
	camera->minDepth = DEPTH_FAR;
	pthread_mutex_init(&camera->depthLock, NULL);
	clearBuffers(camera);
}

void destroyCamera(Camera *camera) {
	if (!camera || !camera->framebufferLdr) return;
	freeCameraBuffers(camera);
	camera->frameCounter = 0;
	pthread_mutex_destroy(&camera->depthLock);
}

void setMaxDepth(Camera *camera, float maxDepth) {
	if (!camera) return;
	pthread_mutex_lock(&camera->depthLock);
	if (maxDepth > camera->maxDepth) {
		camera->maxDepth = maxDepth;
	}
	pthread_mutex_unlock(&camera->depthLock);
}

void setMinDepth(Camera *camera, float minDepth) {
	if (!camera) return;
	pthread_mutex_lock(&camera->depthLock);
	if (minDepth < camera->minDepth) {
		camera->minDepth = minDepth;
	}
	pthread_mutex_unlock(&camera->depthLock);
}

static float3 cameraRightFor(float3 forward) {
	return Float3_Normalize(Float3_Cross((float3){0.0f, 1.0f, 0.0f}, forward));
}

static void cameraTranslate(Camera *camera, float3 direction, float amount) {
	camera->position.x += direction.x * amount;
	camera->position.y += direction.y * amount;
	camera->position.z += direction.z * amount;
}

void CameraMoveForward(Camera *camera, float amount) {
	if (!camera) return;
	cameraTranslate(camera, camera->forward, amount);
}

void CameraMoveRight(Camera *camera, float amount) {
	if (!camera) return;
	cameraTranslate(camera, cameraRightFor(camera->forward), amount);
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
	float3 right = cameraRightFor(fwd1);
	float3 camUp = Float3_Cross(right, fwd1); // points upward relative to forward
	float cosPitch = cosf(pitch);
	float sinPitch = sinf(pitch);
	camera->forward = Float3_Normalize((float3){
		fwd1.x * cosPitch + camUp.x * sinPitch,
		fwd1.y * cosPitch + camUp.y * sinPitch,
		fwd1.z * cosPitch + camUp.z * sinPitch});
}

void ComputePrevCameraPos(Camera *camera) {
	camera->prevPosition = camera->position;
	camera->prevForward = camera->forward;
	camera->prevRight = camera->right;
	camera->prevUp = camera->up;
	camera->prevFovScale = camera->fovScale;
	camera->prevAspect = camera->aspect;
};