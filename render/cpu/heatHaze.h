#include "../../object/format.h"
#include "../../math/vector3.h"
#include "../../util/threadPool.h"
#include <immintrin.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NOISE_SIZE 128
float noiseVolume[NOISE_SIZE][NOISE_SIZE][NOISE_SIZE];

static inline float hash3D(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u
               + (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

static void initNoise() {
    for (int x = 0; x < NOISE_SIZE; x++) {
        for (int y = 0; y < NOISE_SIZE; y ++) {
            for (int z = 0; z < NOISE_SIZE; z++) {
                noiseVolume[x][y][z] = hash3D(x, y, z);
            }
        }
    }
}

// 8 points per call; the hash has no lookup tables, so everything stays in registers
// NOTE: simd back magic not sure how this shit works :(
static inline void noise3d2x8(const float *x, const float *y, const float *z,
                              float *nx, float *ny) {
    __m256 px = _mm256_loadu_ps(x), py = _mm256_loadu_ps(y), pz = _mm256_loadu_ps(z);
    __m256 fx = _mm256_floor_ps(px), fy = _mm256_floor_ps(py), fz = _mm256_floor_ps(pz);
    __m256i xi = _mm256_cvttps_epi32(fx), yi = _mm256_cvttps_epi32(fy), zi = _mm256_cvttps_epi32(fz);
    __m256 ax = _mm256_sub_ps(px, fx), ay = _mm256_sub_ps(py, fy), az = _mm256_sub_ps(pz, fz);
    const __m256 three = _mm256_set1_ps(3.0f), one = _mm256_set1_ps(1.0f);
    ax = _mm256_mul_ps(_mm256_mul_ps(ax, ax), _mm256_sub_ps(three, _mm256_add_ps(ax, ax)));
    ay = _mm256_mul_ps(_mm256_mul_ps(ay, ay), _mm256_sub_ps(three, _mm256_add_ps(ay, ay)));
    az = _mm256_mul_ps(_mm256_mul_ps(az, az), _mm256_sub_ps(three, _mm256_add_ps(az, az)));
    __m256 wx[2] = { _mm256_sub_ps(one, ax), ax };
    __m256 wy[2] = { _mm256_sub_ps(one, ay), ay };
    __m256 wz[2] = { _mm256_sub_ps(one, az), az };
    uint32_t dx = 374761393u, dy = 668265263u, dz = 2147483647u;
    __m256i base = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(xi, _mm256_set1_epi32((int)dx)),
                         _mm256_mullo_epi32(yi, _mm256_set1_epi32((int)dy))),
        _mm256_mullo_epi32(zi, _mm256_set1_epi32((int)dz)));
    const uint32_t co[8] = { 0, dx, dy, dx + dy, dz, dx + dz, dy + dz, dx + dy + dz };
    const __m256 rcp = _mm256_set1_ps(1.0f / 65535.0f);
    __m256 accA = _mm256_setzero_ps(), accB = _mm256_setzero_ps();
    for (int c = 0; c < 8; c++) {
        __m256i s = _mm256_add_epi32(base, _mm256_set1_epi32((int)co[c]));
        __m256i h = _mm256_mullo_epi32(_mm256_xor_si256(s, _mm256_srli_epi32(s, 13)),
                                       _mm256_set1_epi32(1274126177));
        h = _mm256_xor_si256(h, _mm256_srli_epi32(h, 16));
        __m256 va = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(h, _mm256_set1_epi32(0xFFFF))), rcp);
        __m256 vb = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(h, 16)), rcp);
        __m256 w = _mm256_mul_ps(wx[c & 1], _mm256_mul_ps(wy[(c >> 1) & 1], wz[(c >> 2) & 1]));
        accA = _mm256_fmadd_ps(w, va, accA);
        accB = _mm256_fmadd_ps(w, vb, accB);
    }
    _mm256_storeu_ps(nx, accA);
    _mm256_storeu_ps(ny, accB);
}

typedef struct Emitter {
    float3 Pos;
    float3 Dir; // Dir + Length of the bloom
    float width;
    float strength;
    float noiseFrequency;
    float noiseSpeed;
} Emitter;

typedef struct EmitterPreCompute {
    float3 axis;
    float3 dirToCamera;
    float distanceToCamera; // camPos - emitter pos
    float f; // dot(axis, w)
} EmitterPreCompute;

typedef struct EmitterManger {
    Emitter *emitters;
    EmitterPreCompute *precomputedEmitterValues;
    int len;
    int cap;
} EmitterManger;

static void initEmitterManger(EmitterManger *m, int initCapacity) {
    m->emitters = malloc((size_t)initCapacity * sizeof(Emitter));
    m->precomputedEmitterValues = malloc((size_t)initCapacity * sizeof(EmitterPreCompute));
    if (!m->emitters || !m->precomputedEmitterValues) {
        free(m->emitters);
        free(m->precomputedEmitterValues);
        m->emitters = NULL;
        m->precomputedEmitterValues = NULL;
        initCapacity = 0;
    }
    m->len = 0;
    m->cap = initCapacity;
}

static void addEmitter(EmitterManger *m, const Emitter *e) {
    if (m->len >= m->cap) {
        int newCap = m->cap ? m->cap * 2 : 8;
        Emitter *resizedEmitters = realloc(m->emitters, (size_t)newCap * sizeof(Emitter));
        if (!resizedEmitters) {
            fprintf(stderr, "Error: Could not grow emitter list.\n");
            return;
        }
        m->emitters = resizedEmitters;
        EmitterPreCompute *resizedPrecomputed = realloc(m->precomputedEmitterValues, (size_t)newCap * sizeof(EmitterPreCompute));
        if (!resizedPrecomputed) {
            fprintf(stderr, "Error: Could not grow precomputed emitter values.\n");
            return;
        }
        m->precomputedEmitterValues = resizedPrecomputed;
        m->cap = newCap;
    }
    m->emitters[m->len++] = *e;
}

static void clearEmitterManger(EmitterManger *m) {
    m->len = 0;
}

static void freeEmitterManger(EmitterManger *m) {
    free(m->emitters);
    free(m->precomputedEmitterValues);
    m->emitters = NULL;
    m->precomputedEmitterValues = NULL;
    m->len = 0;
    m->cap = 0;
}

static void heatHaze(Camera *restrict cam, EmitterManger *restrict emitters) {};