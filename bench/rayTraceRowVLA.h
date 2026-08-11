#ifndef RAYTRACEROW_VLA_H
#define RAYTRACEROW_VLA_H

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIDTH 1080
#define REFLECTION_RESOLUTION 4
#define BLUR_RADIUS 3

typedef struct { float x, y, z, w; } float3;

// V1: Original with memset + continue
static void V1_original(float3 *refl_out, float3 *em_out, float3 *shd_out, int width, float seed, float *depth) {
    float3 catchReflections[width];
    float3 catchEmissions[width];
    float3 catchShadow[width];

    memset(catchReflections, 0, sizeof(float3) * width);
    memset(catchEmissions, 0, sizeof(float3) * width);
    memset(catchShadow, 0, sizeof(float3) * width);

    float3 catchReflection = {0,0,0,0};
    float3 catchEmission = {0,0,0,0};
    float3 catchShadowValue = {1,1,1,0};

    for (int x = 0; x < width; x++) {
        int is_sky = (x % 13 == 0);
        depth[x] = is_sky ? 1e10f : 1.0f;

        if (is_sky) {
            continue;
        }

        if (x % REFLECTION_RESOLUTION == 0) {
            catchReflection.x = seed + x * 0.1f;
            catchEmission.y = seed + x * 0.2f;
            catchShadowValue.z = seed + x * 0.3f;
        }
        catchReflections[x] = catchReflection;
        catchEmissions[x] = catchEmission;
        catchShadow[x] = catchShadowValue;
    }

    for (int x = 0; x < width; x++) {
        if (depth[x] >= 1e9f) continue;
        float3 acc = {0,0,0,0};
        int samples = 0;
        for (int k = BLUR_RADIUS * 2 + 1; k > 0; k--) {
            int kIdx = x + k - BLUR_RADIUS - 1;
            if (kIdx >= 0 && kIdx < width) {
                acc.x += catchReflections[kIdx].x;
                acc.y += catchEmissions[kIdx].y;
                acc.z += catchShadow[kIdx].z;
                samples++;
            }
        }
        if (samples > 0) {
            float invW = 1.0f / samples;
            refl_out[x].x = acc.x * invW;
            em_out[x].y = acc.y * invW;
            shd_out[x].z = acc.z * invW;
        }
    }
}

// V2: Optimized, no memset, writes default in sky path
static void V2_nomemset(float3 *refl_out, float3 *em_out, float3 *shd_out, int width, float seed, float *depth) {
    float3 catchReflections[width];
    float3 catchEmissions[width];
    float3 catchShadow[width];

    float3 catchReflection = {0,0,0,0};
    float3 catchEmission = {0,0,0,0};
    float3 catchShadowValue = {1,1,1,0};

    for (int x = 0; x < width; x++) {
        int is_sky = (x % 13 == 0);
        depth[x] = is_sky ? 1e10f : 1.0f;

        if (is_sky) {
            catchReflections[x] = (float3){0,0,0,0};
            catchEmissions[x] = (float3){0,0,0,0};
            catchShadow[x] = (float3){0,0,0,0};
            continue;
        }

        if (x % REFLECTION_RESOLUTION == 0) {
            catchReflection.x = seed + x * 0.1f;
            catchEmission.y = seed + x * 0.2f;
            catchShadowValue.z = seed + x * 0.3f;
        }
        catchReflections[x] = catchReflection;
        catchEmissions[x] = catchEmission;
        catchShadow[x] = catchShadowValue;
    }

    for (int x = 0; x < width; x++) {
        if (depth[x] >= 1e9f) continue;
        float3 acc = {0,0,0,0};
        int samples = 0;
        for (int k = BLUR_RADIUS * 2 + 1; k > 0; k--) {
            int kIdx = x + k - BLUR_RADIUS - 1;
            if (kIdx >= 0 && kIdx < width) {
                acc.x += catchReflections[kIdx].x;
                acc.y += catchEmissions[kIdx].y;
                acc.z += catchShadow[kIdx].z;
                samples++;
            }
        }
        if (samples > 0) {
            float invW = 1.0f / samples;
            refl_out[x].x = acc.x * invW;
            em_out[x].y = acc.y * invW;
            shd_out[x].z = acc.z * invW;
        }
    }
}
#endif