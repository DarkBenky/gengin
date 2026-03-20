#define CLOUD_STEPS    32
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

    int ix = clamp((int)fx, 0, xRes - 2);
    int iy = clamp((int)fy, 0, yRes - 2);
    int iz = clamp((int)fz, 0, zRes - 2);

    float u = fx - ix, v = fy - iy, w = fz - iz;

    // Python writes [x][y][z] order: x is major axis, z is minor
    int xStride = yRes * zRes, yStride = zRes, zStride = 1;
    int base = ix * xStride + iy * yStride + iz * zStride;

    float d000 = data[base],                           d100 = data[base + xStride];
    float d010 = data[base + yStride],                 d110 = data[base + xStride + yStride];
    float d001 = data[base + zStride],                 d101 = data[base + xStride + zStride];
    float d011 = data[base + yStride + zStride],       d111 = data[base + xStride + yStride + zStride];

    return mix(
        mix(mix(d000, d100, u), mix(d010, d110, u), v),
        mix(mix(d001, d101, u), mix(d011, d111, u), v),
        w);
}

static float shadowMarch(
    __global const float *buf,
    float3 pos, float3 toLight,
    float shadowDist, float shadowExtinction)
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
    float extinctionScale,
    float shadowExtinction,
    float scatterG,
    float shadowDist,
    float ambientLight,
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
    float rawRayLen = length(rawRay);
    float3 rayDir = rawRay / rawRayLen;

    // Transform ray to object local space ([0,1]^3)
    float3 d = camPos - position;
    float3 localOrigin = (float3)(dot(_invM0, d), dot(_invM1, d), dot(_invM2, d));
    float3 localDir    = normalize((float3)(dot(_invM0, rayDir), dot(_invM1, rayDir), dot(_invM2, rayDir)));
    // lightDir points toward the light (same convention as CPU ray tracer)
    float3 toLight = normalize(lightDir);
    float3 localLight  = normalize((float3)(dot(_invM0, toLight), dot(_invM1, toLight), dot(_invM2, toLight)));

    // Compute scale factor from local-space t to CPU-depth t:
    // CPU depth = dot(hitPos - camPos, unnormRay) = t_worldNorm * rawRayLen
    // local t -> worldNorm t: t_worldNorm = t_local / length(M_inv * rayDir)
    float3 localDir_unnorm = (float3)(dot(_invM0, rayDir), dot(_invM1, rayDir), dot(_invM2, rayDir));
    float localToCpuDepth = rawRayLen / length(localDir_unnorm); // converts local t to CPU depth units

    // Convert scene depth to local-space t for clipping
    float sceneCpuDepth = sceneDepth[idx];
    float tDepth = (sceneCpuDepth < 1e29f) ? (sceneCpuDepth / localToCpuDepth) : 1e30f;

    // Slab AABB test against [-0.5, 0.5]^3 (TRS local space convention)
    float3 invDir = 1.0f / localDir;
    float3 t0 = (-0.5f - localOrigin) * invDir;
    float3 t1 = ( 0.5f - localOrigin) * invDir;
    float tEntry = fmax(fmax(fmin(t0.x, t1.x), fmin(t0.y, t1.y)), fmin(t0.z, t1.z));
    float tExit  = fmin(fmin(fmax(t0.x, t1.x), fmax(t0.y, t1.y)), fmax(t0.z, t1.z));

    if (tExit <= tEntry || tExit < 0.0f) {
        output[idx] = (float4)(0.0f, 0.0f, 0.0f, 1.0f); // no cloud hit: transmittance=1 (fully transparent)
        return;
    }
    tEntry = fmax(tEntry, 0.0f);
    tExit  = fmin(tExit, tDepth); // clip against scene geometry
    if (tExit <= tEntry) {
        output[idx] = (float4)(0.0f, 0.0f, 0.0f, 1.0f); // fully occluded by geometry
        return;
    }

    float stepSize   = (tExit - tEntry) / (float)CLOUD_STEPS;
    // cosTheta between view direction and toward-light: positive = forward scatter (viewer on same side as light)
    float cosTheta   = -dot(rayDir, toLight);
    float phase      = min(henyeyGreenstein(cosTheta, scatterG), 4.0f);
    float transmittance = 1.0f;
    float3 scattered = (float3)(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < CLOUD_STEPS; i++) {
        float3 localPos = localOrigin + localDir * (tEntry + (i + 0.5f) * stepSize);

        // remap localPos from [-0.5,0.5] to [0,1] for density lookup
        float3 uvw = localPos + (float3)(0.5f, 0.5f, 0.5f);
        float dens = sampleDensity(buf, uvw);
        if (dens < 0.005f) continue;

        float extinction        = dens * extinctionScale;
        float sampleTransmit    = exp(-extinction * stepSize);
        float shadowLight = fmax(ambientLight, shadowMarch(buf, localPos, localLight, shadowDist, shadowExtinction));

        // Energy-conserving single-scatter integral
        float3 luminance = baseColor * (shadowLight * phase);
        scattered += luminance * transmittance * (1.0f - sampleTransmit) / extinction;

        transmittance *= sampleTransmit;
        if (transmittance < 0.005f) break;
    }

    // store transmittance in .w so composite can do: background * transmittance + scattered
    output[idx] = (float4)(scattered.x, scattered.y, scattered.z, transmittance);
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