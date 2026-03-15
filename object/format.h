#ifndef FORMAT_H
#define FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#define WIDTH 1080
#define HEIGHT 720

#define FLT_MAX 3.402823466e+38F
#define FLT_MIN -3.402823466e+38F
// Depth sentinel: memset(0x7F) gives 0x7F7F7F7F ~= 3.394e38 per pixel, which is
// less than FLT_MAX. Use DEPTH_FAR for all depth empty-pixel checks.
#define DEPTH_FAR 1e30f

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;

typedef uint32_t Color;

typedef struct float3 {
	float x;
	float y;
	float z;
	float w; // padding for alignment
} float3;

typedef struct float2 {
	float x;
	float y;
} float2;

typedef struct float4 {
	float x;
	float y;
	float z;
	float w;
} float4;

typedef struct int4 {
	int x;
	int y;
	int z;
	int w;
} int4;

typedef struct Camera {
	float3 position;
	float3 forward;
	float fov;
	float2 jitter;
	int screenWidth;
	int screenHeight;
	float3 lightDir;
	/* precomputed per-frame, set by RenderObjects */
	float3 right;
	float3 up;
	float3 viewDir;
	float3 halfVec;
	float3 renderLightDir;
	float aspect;
	float fovScale;
	uint32 *framebuffer;
	int4 *accumulationBuffer;
	float3 *normalBuffer;
	float3 *positionBuffer;
	float3 *reflectBuffer;
	float3 *bloomBuffer;
	// float3 *bloomBufferTemp1;
	// float3 *bloomBufferTemp2;
	// float3 *bloomBufferTemp3;
	float *depthBuffer;
	float seed;
	Color *reflectCache;
	Color *tempFramebuffer;
	float *tempBuffer_1;
	float *tempBuffer_2;
	float *shadowCache;
	int *objectIdBuffer;
	int frameCounter;
} Camera;

void clearBuffers(Camera *camera);
void initCamera(Camera *camera, int width, int height, float fov, float3 position, float3 forward, float3 lightDir);
void destroyCamera(Camera *camera);
void CameraMoveForward(Camera *camera, float amount);
void CameraMoveRight(Camera *camera, float amount);
void CameraMoveUp(Camera *camera, float amount);
void CameraRotate(Camera *camera, float pitch, float yaw);

#endif // FORMAT_H