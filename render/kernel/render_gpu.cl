__kernel void renderToTexture(write_only image2d_t outTex, float t) {
    const int2 id = (int2)(get_global_id(0), get_global_id(1));
    const int w = get_image_width(outTex);
    const int h = get_image_height(outTex);
    const float2 uv = (float2)(id.x / (float)w, id.y / (float)h);
    const float r = 0.5f + 0.5f * sin(6.28318f * (uv.x + t * 0.15f));
    const float g = 0.5f + 0.5f * sin(6.28318f * (uv.y + t * 0.20f));
    const float b = 0.5f + 0.5f * sin(6.28318f * (uv.x + uv.y + t * 0.10f));
    write_imagef(outTex, id, (float4)(r, g, b, 1.0f));
}

__kernel void renderToBuffer(__global uint* outBuf, float t, int width, int height) {
    const int gid = get_global_id(0);
    const int x = gid % width;
    const int y = gid / width;
    if (x >= width || y >= height) return;
    const float2 uv = (float2)(x / (float)width, y / (float)height);
    const uint r = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.x + t * 0.15f))));
    const uint g = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.y + t * 0.20f))));
    const uint b = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.x + uv.y + t * 0.10f))));
    outBuf[gid] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

float edgef(float ax, float ay, float bx, float by, float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

__kernel void renderSceneBuffer(__global uint* outBuf,
                                __global float4* triA,
                                __global float4* triB,
                                __global float4* triC,
                                __global int4* triBounds,
                                __global uint* triColor,
                                int triCount, int width, int height) {
    const int gid = get_global_id(0);
    const int x = gid % width;
    const int y = gid / width;
    if (x >= width || y >= height) return;

    float bestDepth = 3.402823466e+38f;
    uint bestColor = 0xFF000000u;
    const float px = (float)x + 0.5f;
    const float py = (float)y + 0.5f;

    for (int i = 0; i < triCount; i++) {
        int4 b = triBounds[i];
        if (x < b.x || x > b.y || y < b.z || y > b.w) continue;

        float4 a = triA[i];
        float4 bb = triB[i];
        float4 c = triC[i];

        float w0 = edgef(a.z, a.w, bb.x, bb.y, px, py);
        float w1 = edgef(bb.x, bb.y, a.x, a.y, px, py);
        float w2 = edgef(a.x, a.y, a.z, a.w, px, py);

        if ((w0 * c.y) >= 0.0f && (w1 * c.y) >= 0.0f && (w2 * c.y) >= 0.0f) {
            float invZ = (w0 * bb.z + w1 * bb.w + w2 * c.x) * c.z;
            float depth = 1.0f / invZ;
            if (depth < bestDepth) {
                bestDepth = depth;
                bestColor = triColor[i];
            }
        }
    }

    outBuf[gid] = bestColor;
}
