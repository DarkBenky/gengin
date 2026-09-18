#define CLOUD_STEPS    64
#define SHADOW_STEPS   8
#define GOD_RAY_STEPS  64

static float sampleDensity(
    __global const float *buf,
    float3 uvw)
{
    int xRes = (int)buf[0];
    int yRes = (int)buf[1];
    int zRes = (int)buf[2];
    __global const float *data = buf + 3;
    float fx = uvw.x * (xRes - 1);
    float fy = uvw.y * (yRes - 1);
    float fz = uvw.z * (zRes - 1);

    /* clamp((int)x, 0, hi) written as explicit bounds instead of OpenCL
       clamp() — PoCL lowers the builtin to an external _cl_clamp* runtime
       call; these branches compile to cmov on x86 (no mispredicts). */
    int ix = (int)fx; if (ix < 0) ix = 0; else if (ix > xRes - 2) ix = xRes - 2;
    int iy = (int)fy; if (iy < 0) iy = 0; else if (iy > yRes - 2) iy = yRes - 2;
    int iz = (int)fz; if (iz < 0) iz = 0; else if (iz > zRes - 2) iz = zRes - 2;

    float u = fx - ix, v = fy - iy, w = fz - iz;

    int xStride = yRes * zRes, yStride = zRes, zStride = 1;
    int base = ix * xStride + iy * yStride + iz * zStride;

    float d000 = data[base],                           d100 = data[base + xStride];
    float d010 = data[base + yStride],                 d110 = data[base + xStride + yStride];
    float d001 = data[base + zStride],                 d101 = data[base + xStride + zStride];
    float d011 = data[base + yStride + zStride],       d111 = data[base + xStride + yStride + zStride];

    /* mix(a,b,t) = a + (b - a) * t, inlined (PoCL extern-calls _cl_mixfff). */
    float i00 = d000 + (d100 - d000) * u;
    float i10 = d010 + (d110 - d010) * u;
    float i01 = d001 + (d101 - d001) * u;
    float i11 = d011 + (d111 - d011) * u;
    float j0  = i00 + (i10 - i00) * v;
    float j1  = i01 + (i11 - i01) * v;
    return j0 + (j1 - j0) * w;
}

static float3 shadowMarch(
    __global const float *buf,
    float3 pos, float3 toLight,
    float shadowDist, float3 shadowExtinction)
{
    float acc = 0.0f;
    float stepSize = shadowDist / SHADOW_STEPS;
    for (int s = 1; s <= SHADOW_STEPS; s++) {
        float3 p = pos + toLight * (s * stepSize);
        if (p.x < -0.5f || p.x > 0.5f ||
            p.y < -0.5f || p.y > 0.5f ||
            p.z < -0.5f || p.z > 0.5f) break;
        float3 uvw = p + (float3)(0.5f, 0.5f, 0.5f);
        acc += sampleDensity(buf, uvw) * stepSize;
    }
    return exp(-acc * shadowExtinction);
}

static float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0f - g2) / pow(1.0f + g2 - 2.0f * g * cosTheta, 1.5f);
}

#define LOCAL_W 128
#define LOCAL_PAD 32  // must be >= max radius

__kernel __attribute__((reqd_work_group_size(LOCAL_W, 1, 1)))
void blur(
    __global const float4 *input,
    __global float4 *output,
    int screenWidth,
    int screenHeight,
    int radius
) {
    __local float4 tile[LOCAL_W + 2 * LOCAL_PAD];

    int gx = get_global_id(0);
    int gy = get_global_id(1);
    int lx = get_local_id(0);

    if (gy >= screenHeight) return;

    // Load center + halo into local memory
    int load_x = clamp(gx, 0, screenWidth - 1);
    tile[lx + LOCAL_PAD] = input[gy * screenWidth + load_x];

    // Load left halo
    if (lx < LOCAL_PAD) {
        int halo_x = clamp(gx - LOCAL_PAD, 0, screenWidth - 1);
        tile[lx] = input[gy * screenWidth + halo_x];
    }
    // Load right halo
    if (lx >= LOCAL_W - LOCAL_PAD) {
        int halo_x = clamp(gx + LOCAL_PAD, 0, screenWidth - 1);
        tile[lx + 2 * LOCAL_PAD] = input[gy * screenWidth + halo_x];
    }

    barrier(CLK_LOCAL_MEM_FENCE);

    if (gx >= screenWidth) return;

    float4 sum = (float4)(0.0f);
    float weight = 1.0f / (float)(2 * radius + 1);

    for (int i = -radius; i <= radius; i++) {
        sum += tile[lx + LOCAL_PAD + i];
    }

    output[gy * screenWidth + gx] = sum * weight;
}

__kernel void renderClouds(
    // object transform
    float3 position,
    float3 rotation,
    float3 scale,
    // cached inverse TRS matrix rows: localP = M * (worldP - position)
    float3 _invM0,
    float3 _invM1,
    float3 _invM2,
    // cached forward rotation rows (unused for volumes, kept for API consistency)
    float3 _fwdRot0,
    float3 _fwdRot1,
    float3 _fwdRot2,
    // cloud density volume: buf[0..2] = xRes,yRes,zRes; buf[3..] = density[x + y*xRes + z*xRes*yRes]
    __global const float *buf,
    // camera
    float3 camPos,
    float3 camForward,
    float3 camUp,
    float3 camRight,
    float camFov,
    int screenWidth,
    int screenHeight,
    float3 lightDir,
    // material
    float3 baseColor,
    // tunable params
    float3 extinctionScale,
    float3 shadowExtinction,
    float scatterG,
    float shadowDist,
    float3 ambientLight,
    // output
    __global float4 *output,
    int samplesPerPixel,
    __global const float *sceneDepth  // CPU depth buffer in t-units along unnormalized ray
) {
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= screenWidth || y >= screenHeight) return;

    int idx = y * screenWidth + x;

    // Build primary ray
    float ndcX  = ((float)x + 0.5f) / (float)screenWidth  * 2.0f - 1.0f;
    float ndcY  = 1.0f - ((float)y + 0.5f) / (float)screenHeight * 2.0f;
    float aspect = (float)screenWidth / (float)screenHeight;
    // rawRay is the unnormalized ray direction (same as CPU ray tracer)
    float3 rawRay = camForward + camRight * (ndcX * aspect * camFov) + camUp * (ndcY * camFov);
    float rawRayLen = sqrt(rawRay.x*rawRay.x + rawRay.y*rawRay.y + rawRay.z*rawRay.z); /* length() */
    float3 rayDir = rawRay / rawRayLen;

    // Transform ray to object local space ([0,1]^3)
    float3 d = camPos - position;
    float3 localOrigin = (float3)(_invM0.x*d.x + _invM0.y*d.y + _invM0.z*d.z,
                                  _invM1.x*d.x + _invM1.y*d.y + _invM1.z*d.z,
                                  _invM2.x*d.x + _invM2.y*d.y + _invM2.z*d.z);
    // localDir computed unnormalized, then length() + division (normalize inlined)
    float3 ld0 = (float3)(_invM0.x*rayDir.x + _invM0.y*rayDir.y + _invM0.z*rayDir.z,
                          _invM1.x*rayDir.x + _invM1.y*rayDir.y + _invM1.z*rayDir.z,
                          _invM2.x*rayDir.x + _invM2.y*rayDir.y + _invM2.z*rayDir.z);
    float invLdLen = 1.0f / sqrt(ld0.x*ld0.x + ld0.y*ld0.y + ld0.z*ld0.z);
    float3 localDir = ld0 * invLdLen;
    // lightDir points toward the light (same convention as CPU ray tracer)
    float invTlLen = 1.0f / sqrt(lightDir.x*lightDir.x + lightDir.y*lightDir.y + lightDir.z*lightDir.z);
    float3 toLight = lightDir * invTlLen;
    float3 lt0 = (float3)(_invM0.x*toLight.x + _invM0.y*toLight.y + _invM0.z*toLight.z,
                          _invM1.x*toLight.x + _invM1.y*toLight.y + _invM1.z*toLight.z,
                          _invM2.x*toLight.x + _invM2.y*toLight.y + _invM2.z*toLight.z);
    float invLlLen = 1.0f / sqrt(lt0.x*lt0.x + lt0.y*lt0.y + lt0.z*lt0.z);
    float3 localLight = lt0 * invLlLen;

    // Compute scale factor from local-space t to CPU-depth t:
    // CPU depth = dot(hitPos - camPos, unnormRay) = t_worldNorm * rawRayLen
    // local t -> worldNorm t: t_worldNorm = t_local / length(M_inv * rayDir)
    float3 localDir_unnorm = (float3)(_invM0.x*rayDir.x + _invM0.y*rayDir.y + _invM0.z*rayDir.z,
                                      _invM1.x*rayDir.x + _invM1.y*rayDir.y + _invM1.z*rayDir.z,
                                      _invM2.x*rayDir.x + _invM2.y*rayDir.y + _invM2.z*rayDir.z);
    float localToCpuDepth = rawRayLen / sqrt(localDir_unnorm.x*localDir_unnorm.x + localDir_unnorm.y*localDir_unnorm.y + localDir_unnorm.z*localDir_unnorm.z); // converts local t to CPU depth units

    // Convert scene depth to local-space t for clipping
    float sceneCpuDepth = sceneDepth[idx];
    float tDepth = (sceneCpuDepth < 1e29f) ? (sceneCpuDepth / localToCpuDepth) : 1e30f;

    // Slab AABB test against [-0.5, 0.5]^3 (TRS local space convention)
    float3 invDir = 1.0f / localDir;
    float3 t0 = (-0.5f - localOrigin) * invDir;
    float3 t1 = ( 0.5f - localOrigin) * invDir;
    /* fmin/fmax inlined (handles NaN-lenient value selection the same way) */
    float teA = (t0.x < t1.x) ? t0.x : t1.x, teB = (t0.y < t1.y) ? t0.y : t1.y, teC = (t0.z < t1.z) ? t0.z : t1.z;
    float ta  = (teA > teB) ? teA : teB; float tEntry = (ta > teC) ? ta : teC;
    float txA = (t0.x > t1.x) ? t0.x : t1.x, txB = (t0.y > t1.y) ? t0.y : t1.y, txC = (t0.z > t1.z) ? t0.z : t1.z;
    float ttA = (txA < txB) ? txA : txB; float tExit = (ttA < txC) ? ttA : txC;

    if (tExit <= tEntry || tExit < 0.0f) {
        output[idx] = (float4)(0.0f, 0.0f, 0.0f, 1.0f); // no cloud hit: transmittance=1 (fully transparent)
        return;
    }
    tEntry = (tEntry > 0.0f) ? tEntry : 0.0f;
    tExit  = (tExit < tDepth) ? tExit : tDepth; // clip against scene geometry
    if (tExit <= tEntry) {
        output[idx] = (float4)(0.0f, 0.0f, 0.0f, 1.0f); // fully occluded by geometry
        return;
    }

    float stepSize   = (tExit - tEntry) / (float)CLOUD_STEPS;
    // cosTheta between view direction and toward-light: positive = forward scatter (viewer on same side as light)
    float cosTheta   = -(rayDir.x*toLight.x + rayDir.y*toLight.y + rayDir.z*toLight.z); /* dot() */
    float hg = henyeyGreenstein(cosTheta, scatterG);
    float phase      = (hg < 4.0f) ? hg : 4.0f; /* min(), hoisted to avoid double pow() */
    float3 transmittance = (float3)(1.0f, 1.0f, 1.0f);
    float3 scattered = (float3)(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < CLOUD_STEPS; i++) {
        float3 localPos = localOrigin + localDir * (tEntry + (i + 0.5f) * stepSize);

        // remap localPos from [-0.5,0.5] to [0,1] for density lookup
        float3 uvw = localPos + (float3)(0.5f, 0.5f, 0.5f);
        float dens = sampleDensity(buf, uvw);
        if (dens < 0.005f) continue;

        float3 extinction     = dens * extinctionScale;
        float3 sampleTransmit = exp(-extinction * stepSize);
        /* fmax(ambientLight, shadowMarch) component-wise element min/max */
        float3 sm = shadowMarch(buf, localPos, localLight, shadowDist, shadowExtinction);
        float3 shadowLight = (float3)((ambientLight.x > sm.x) ? ambientLight.x : sm.x,
                                      (ambientLight.y > sm.y) ? ambientLight.y : sm.y,
                                      (ambientLight.z > sm.z) ? ambientLight.z : sm.z);

        // Energy-conserving single-scatter integral
        float3 luminance = baseColor * (shadowLight * phase);
        scattered += luminance * transmittance * (1.0f - sampleTransmit) / extinction;

        transmittance *= sampleTransmit;
        if (transmittance.x < 0.005f && transmittance.y < 0.005f && transmittance.z < 0.005f) break; /* all() */
    }

    // store luminance transmittance in .w for compositing; background * T + scattered per channel
    float lumT = transmittance.x * 0.2126f + transmittance.y * 0.7152f + transmittance.z * 0.0722f; /* dot() */
    output[idx] = (float4)(scattered.x, scattered.y, scattered.z, lumT);
}

// Screen-space radial march from each pixel toward the sun.
// Reads transmittance from the cloud buffer as an occlusion mask; output is additive RGB.
__kernel void godRays(
    __global const float4 *cloudBuffer,
    __global const float  *sceneDepth,   // pixels with geometry (depth < 1e29) are not sky
    int    screenWidth,
    int    screenHeight,
    float2 sunScreenPos,
    float3 godRayColor,
    float  intensity,
    float  decay,
    __global float4 *output
) {
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= screenWidth || y >= screenHeight) return;

    float2 uv    = (float2)((x + 0.5f) / screenWidth, (y + 0.5f) / screenHeight);
    float2 delta = (sunScreenPos - uv) * (1.0f / GOD_RAY_STEPS);

    float accumDecay   = 1.0f;
    float illumination = 0.0f;
    float2 sampleUV    = uv;

    for (int i = 0; i < GOD_RAY_STEPS; i++) {
        sampleUV += delta;
        if (sampleUV.x < 0.0f || sampleUV.x >= 1.0f ||
            sampleUV.y < 0.0f || sampleUV.y >= 1.0f) break;
        int sx = (int)(sampleUV.x * screenWidth);
        int sy = (int)(sampleUV.y * screenHeight);
        int si = sy * screenWidth + sx;
        // only sky pixels (no geometry) act as the light source
        // terrain/objects occlude the shafts just like clouds do
        float isSky = (sceneDepth[si] >= 1e29f) ? 1.0f : 0.0f;
        // weight decreases with each step: samples closer to the sun contribute more
        float w = 1.0f - (float)i * (1.0f / GOD_RAY_STEPS);
        illumination += cloudBuffer[si].w * isSky * accumDecay * w;
        accumDecay   *= decay;
    }

    float v   = illumination * (intensity / GOD_RAY_STEPS);
    int   idx = y * screenWidth + x;
    output[idx] = (float4)(godRayColor.x * v, godRayColor.y * v, godRayColor.z * v, 0.0f);
}

// Composites cloud + god-ray results onto the uint ARGB framebuffer.
// Equivalent to the CPU CloudRenderer_Composite loop, but runs parallel on GPU.
// framebuffer format: 0xFF000000 | R<<16 | G<<8 | B (same as cpu-side Color)
__kernel void compositeFrame(
    __global const float4 *cloudBuf,
    __global const float4 *godRayBuf,
    __global uint         *framebuffer,
    int screenWidth,
    int screenHeight
) {
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= screenWidth || y >= screenHeight) return;

    int idx = y * screenWidth + x;

    float4 cloud = cloudBuf[idx];
    float  transmittance = cloud.w;
    float4 gr = godRayBuf[idx];

    if (transmittance > 0.998f && gr.x < 0.001f && gr.y < 0.001f && gr.z < 0.001f) return;

    uint   bg  = framebuffer[idx];
    float  br  = ((bg >> 16) & 0xFFu) * (1.0f / 255.0f);
    float  bgi = ((bg >>  8) & 0xFFu) * (1.0f / 255.0f);
    float  bb  = ( bg        & 0xFFu) * (1.0f / 255.0f);

    float fr = br, fg = bgi, fb = bb;
    if (transmittance < 0.998f) {
        fr = br  * transmittance + cloud.x;
        fg = bgi * transmittance + cloud.y;
        fb = bb  * transmittance + cloud.z;
    }

    fr = clamp(fr + gr.x, 0.0f, 1.0f);
    fg = clamp(fg + gr.y, 0.0f, 1.0f);
    fb = clamp(fb + gr.z, 0.0f, 1.0f);

    framebuffer[idx] = 0xFF000000u
                     | ((uint)(fr * 255.0f) << 16)
                     | ((uint)(fg * 255.0f) <<  8)
                     |  (uint)(fb * 255.0f);
}