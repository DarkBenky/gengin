#include "rayTraceRowVLA.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

int main() {
    int width = WIDTH;
    int iter = 500000;

    float3 *refl_out = aligned_alloc(64, width * sizeof(float3));
    float3 *em_out = aligned_alloc(64, width * sizeof(float3));
    float3 *shd_out = aligned_alloc(64, width * sizeof(float3));
    float3 *refl_out2 = aligned_alloc(64, width * sizeof(float3));
    float3 *em_out2 = aligned_alloc(64, width * sizeof(float3));
    float3 *shd_out2 = aligned_alloc(64, width * sizeof(float3));
    float *depth = aligned_alloc(64, width * sizeof(float));
    float *depth2 = aligned_alloc(64, width * sizeof(float));

    if (!refl_out || !em_out || !shd_out || !refl_out2 || !em_out2 || !shd_out2 || !depth || !depth2) {
        fprintf(stderr, "aligned_alloc failed\n");
        return 1;
    }

    float seed = 1.0f;

    for (int i = 0; i < 10000; i++) {
        V1_original(refl_out, em_out, shd_out, width, seed + i * 0.01f, depth);
        V2_nomemset(refl_out2, em_out2, shd_out2, width, seed + i * 0.01f, depth2);
    }

    V1_original(refl_out, em_out, shd_out, width, seed, depth);
    V2_nomemset(refl_out2, em_out2, shd_out2, width, seed, depth2);
    int mismatch = 0;
    for (int x = 0; x < width; x++) {
        if (refl_out[x].x != refl_out2[x].x ||
            em_out[x].y != em_out2[x].y ||
            shd_out[x].z != shd_out2[x].z) {
            mismatch++;
        }
    }
    printf("Correctness: %s (mismatches=%d)\n", mismatch == 0 ? "PASS" : "FAIL", mismatch);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < iter; i++) {
        V1_original(refl_out, em_out, shd_out, width, seed + i * 0.01f, depth);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double v1_ns = (t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec);
    double v1_per_call = v1_ns / iter;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < iter; i++) {
        V2_nomemset(refl_out2, em_out2, shd_out2, width, seed + i * 0.01f, depth2);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double v2_ns = (t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec);
    double v2_per_call = v2_ns / iter;

    printf("V1 (original, 3x memset): %.2f ns/call\n", v1_per_call);
    printf("V2 (sky-path init):       %.2f ns/call\n", v2_per_call);
    printf("Speedup: %.2fx\n", v1_per_call / v2_per_call);

    free(refl_out);
    free(em_out);
    free(shd_out);
    free(refl_out2);
    free(em_out2);
    free(shd_out2);
    free(depth);
    free(depth2);

    return 0;
}