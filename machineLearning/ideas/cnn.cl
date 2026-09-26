#define TILE_DIM 16

// =====================================================================
// input layout:  input[(y*width+x)*stride + c]   (stride = channel count)
// filter layout: filter[c*kSize*kSize + fy*kSize + fx]
// accumulate: 0 = overwrite output, 1 = add to existing output
// =====================================================================

// ---------------------------------------------------------------
// Fixed 3x3
// ---------------------------------------------------------------
#define K3 3
#define R3 1

__kernel void conv3x3_generic(
    __global const float* input,
    __global const float* filter,
    __global float* output,
    const int width,
    const int height,
    const int stride,
    const int accumulate)
{
    __local float localFilter[K3 * K3];
    __local float tile[TILE_DIM + 2*R3][TILE_DIM + 2*R3];

    int lx = get_local_id(0);
    int ly = get_local_id(1);
    int gx = get_global_id(0);
    int gy = get_global_id(1);

    int groupOriginX = get_group_id(0) * TILE_DIM - R3;
    int groupOriginY = get_group_id(1) * TILE_DIM - R3;

    float sum = 0.0f;

    for (int c = 0; c < stride; c++) {
        int lid = ly * TILE_DIM + lx;
        if (lid < K3 * K3) {
            localFilter[lid] = filter[c * K3 * K3 + lid];
        }

        for (int ty = ly; ty < TILE_DIM + 2*R3; ty += TILE_DIM) {
            for (int tx = lx; tx < TILE_DIM + 2*R3; tx += TILE_DIM) {
                int srcX = groupOriginX + tx;
                int srcY = groupOriginY + ty;
                float val = 0.0f;
                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height) {
                    val = input[(srcY * width + srcX) * stride + c];
                }
                tile[ty][tx] = val;
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);

        if (gx < width && gy < height) {
            for (int fy = 0; fy < K3; fy++) {
                for (int fx = 0; fx < K3; fx++) {
                    sum += tile[ly + fy][lx + fx] * localFilter[fy * K3 + fx];
                }
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (gx < width && gy < height) {
        int idx = gy * width + gx;
        output[idx] = accumulate ? output[idx] + sum : sum;
    }
}

// ---------------------------------------------------------------
// Fixed 5x5
// ---------------------------------------------------------------
#define K5 5
#define R5 2

__kernel void conv5x5_generic(
    __global const float* input,
    __global const float* filter,
    __global float* output,
    const int width,
    const int height,
    const int stride,
    const int accumulate)
{
    __local float localFilter[K5 * K5];
    __local float tile[TILE_DIM + 2*R5][TILE_DIM + 2*R5];

    int lx = get_local_id(0);
    int ly = get_local_id(1);
    int gx = get_global_id(0);
    int gy = get_global_id(1);

    int groupOriginX = get_group_id(0) * TILE_DIM - R5;
    int groupOriginY = get_group_id(1) * TILE_DIM - R5;

    float sum = 0.0f;

    for (int c = 0; c < stride; c++) {
        int lid = ly * TILE_DIM + lx;
        if (lid < K5 * K5) {
            localFilter[lid] = filter[c * K5 * K5 + lid];
        }

        for (int ty = ly; ty < TILE_DIM + 2*R5; ty += TILE_DIM) {
            for (int tx = lx; tx < TILE_DIM + 2*R5; tx += TILE_DIM) {
                int srcX = groupOriginX + tx;
                int srcY = groupOriginY + ty;
                float val = 0.0f;
                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height) {
                    val = input[(srcY * width + srcX) * stride + c];
                }
                tile[ty][tx] = val;
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);

        if (gx < width && gy < height) {
            for (int fy = 0; fy < K5; fy++) {
                for (int fx = 0; fx < K5; fx++) {
                    sum += tile[ly + fy][lx + fx] * localFilter[fy * K5 + fx];
                }
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (gx < width && gy < height) {
        int idx = gy * width + gx;
        output[idx] = accumulate ? output[idx] + sum : sum;
    }
}

// ---------------------------------------------------------------
// Variable filter size (runtime kSize, odd, <= MAX_K)
// ---------------------------------------------------------------
#define MAX_K 15
#define MAX_R 7
#define MAX_TILE (TILE_DIM + 2*MAX_R)

__kernel void convVariable_generic(
    __global const float* input,
    __global const float* filter,
    __global float* output,
    const int width,
    const int height,
    const int kSize,
    const int stride,
    const int accumulate)
{
    __local float localFilter[MAX_K * MAX_K];
    __local float tile[MAX_TILE][MAX_TILE];

    int radius = (kSize - 1) / 2;
    int tileDim = TILE_DIM + 2 * radius;

    int lx = get_local_id(0);
    int ly = get_local_id(1);
    int gx = get_global_id(0);
    int gy = get_global_id(1);

    int localSize = get_local_size(0) * get_local_size(1);
    int lid = ly * get_local_size(0) + lx;
    int filterElems = kSize * kSize;

    int groupOriginX = get_group_id(0) * TILE_DIM - radius;
    int groupOriginY = get_group_id(1) * TILE_DIM - radius;

    float sum = 0.0f;

    for (int c = 0; c < stride; c++) {
        for (int i = lid; i < filterElems; i += localSize) {
            localFilter[i] = filter[c * filterElems + i];
        }

        for (int ty = ly; ty < tileDim; ty += TILE_DIM) {
            for (int tx = lx; tx < tileDim; tx += TILE_DIM) {
                int srcX = groupOriginX + tx;
                int srcY = groupOriginY + ty;
                float val = 0.0f;
                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height) {
                    val = input[(srcY * width + srcX) * stride + c];
                }
                tile[ty][tx] = val;
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);

        if (gx < width && gy < height) {
            for (int fy = 0; fy < kSize; fy++) {
                for (int fx = 0; fx < kSize; fx++) {
                    sum += tile[ly + fy][lx + fx] * localFilter[fy * kSize + fx];
                }
            }
        }

        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (gx < width && gy < height) {
        int idx = gy * width + gx;
        output[idx] = accumulate ? output[idx] + sum : sum;
    }
}

// =====================================================================
// Usage example (host side):
//
//   // e.g. camera->normalBuffer is float3[w*h] -> stride=3, cast to float*
//   clSetKernelArg(k, 0, sizeof(cl_mem), &normalBufferGPU);
//   clSetKernelArg(k, 1, sizeof(cl_mem), &filterGPU);   // stride*3*3 floats
//   clSetKernelArg(k, 2, sizeof(cl_mem), &outputGPU);
//   clSetKernelArg(k, 3, sizeof(int), &width);
//   clSetKernelArg(k, 4, sizeof(int), &height);
//   int stride = 3, accumulate = 0;
//   clSetKernelArg(k, 5, sizeof(int), &stride);
//   clSetKernelArg(k, 6, sizeof(int), &accumulate);
//   size_t local[2]  = {16, 16};
//   size_t global[2] = {ALIGN_UP(width,16), ALIGN_UP(height,16)};
//   clEnqueueNDRangeKernel(queue, k, 2, NULL, global, local, 0, NULL, NULL);
// =====================================================================