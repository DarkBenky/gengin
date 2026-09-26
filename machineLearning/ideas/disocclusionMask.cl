// TODO: test other implementation that will be faster then this baseline implementation
__kernel void disocclusionMaskFloat(
    const int screenWidth,
    const int screenHeight,
    __global const float2 *motionVector,
    __global const float  *currDepth,
    __global const float  *prevDepth,
    __global float *disocclusionBuff
) {
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= screenWidth || y >= screenHeight) return;
    int idx = y * screenWidth + x;

    const float DEPTH_FAR_THRESHOLD = 1e29f;

    float thisDepth = currDepth[idx];
    if (thisDepth >= DEPTH_FAR_THRESHOLD) {
        disocclusionBuff[idx] = 1.0f; // sky this frame no history relationship
        return;
    }

    float2 uv = (float2)((x + 0.5f) / (float)screenWidth,
                          (y + 0.5f) / (float)screenHeight);
    float2 mv = motionVector[idx];
    float2 prevUV = uv - mv;

    if (prevUV.x < 0.0f || prevUV.x > 1.0f || prevUV.y < 0.0f || prevUV.y > 1.0f) {
        disocclusionBuff[idx] = 1.0f;
        return;
    }

    float fx = prevUV.x * screenWidth  - 0.5f;
    float fy = prevUV.y * screenHeight - 0.5f;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    int x1 = x0 + 1, y1 = y0 + 1;
    float tx = fx - x0, ty = fy - y0;
    x0 = clamp(x0, 0, screenWidth  - 1);
    x1 = clamp(x1, 0, screenWidth  - 1);
    y0 = clamp(y0, 0, screenHeight - 1);
    y1 = clamp(y1, 0, screenHeight - 1);

    float d00 = prevDepth[y0 * screenWidth + x0];
    float d10 = prevDepth[y0 * screenWidth + x1];
    float d01 = prevDepth[y1 * screenWidth + x0];
    float d11 = prevDepth[y1 * screenWidth + x1];
    float dTop    = mix(d00, d10, tx);
    float dBottom = mix(d01, d11, tx);
    float sampledPrevDepth = mix(dTop, dBottom, ty);

    if (sampledPrevDepth >= DEPTH_FAR_THRESHOLD) {
        disocclusionBuff[idx] = 1.0f; // reprojected into sky nothing valid to reproject from
        return;
    }

    const float DISOCCLUSION_THRESHOLD = 0.02f;
    float depthDelta = fabs(thisDepth - sampledPrevDepth);
    float relThreshold = DISOCCLUSION_THRESHOLD * max(thisDepth, 1.0f);
    disocclusionBuff[idx] = clamp(depthDelta / relThreshold - 1.0f, 0.0f, 1.0f);
}

// TODO: test other implementation that will be faster then this baseline implementation
// TODO: Implement this version and test it / benchmark it
// __kernel void disocclusionMaskImg(

// ) {

// }