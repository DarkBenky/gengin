#include "sim.h"

typedef struct pixelInfo {
    float3 Color;
    float Opacity;
} pixelInfo;

typedef struct Ray {
    float3 origin;
    float3 dir;
} Ray;

// TODO
static pixelInfo TraceRayParticles(Ray ray, ParticleContainer *particles) {
    
};
