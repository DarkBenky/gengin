#ifndef FORMAT_H
#define FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#define FLT_MAX 3.402823466e+38F
#define FLT_MIN -3.402823466e+38F

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;

typedef struct float3 {
	float x;
	float y;
	float z;
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

typedef struct Triangle {
	float3 v1;
	float3 v2;
	float3 v3;
	float3 normal;
	float3 color;
	float Roughness;
	float Metallic;
	float Emission;
} Triangle;

typedef struct Camera {
    float3 position;
    float3 forward;
    float fov;
    int screenWidth;
    int screenHeight;
    uint32* framebuffer;
    float3* normalBuffer;
    float *depthBuffer;
} Camera;

void clearBuffers(Camera* camera);
void initCamera(Camera* camera, int width, int height, float fov, float3 position, float3 forward);

#endif // FORMAT_H