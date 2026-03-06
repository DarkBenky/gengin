// OpenCL rasterizer kernel
// Each work item handles one triangle across all objects.
// Output buffers use the same memory layout as the CPU camera buffers:
//   framebuffer     : uint32  packed 0xFF_RR_GG_BB
//   depthBufferInt  : uint32  float depth stored as uint for atomic_min
//   normalBuffer    : float4  xyz = normal,    w = 0  (matches C float3 struct)
//   positionBuffer  : float4  xyz = world pos, w = 0
//   reflectBuffer   : float4  xyz = reflect,   w = 0

typedef struct {
    float m[3][3];
} RotMat;

// Matches CPU BuildRotMat3: Rz * Ry * Rx
static RotMat buildRotMat(float rx, float ry, float rz) {
    float cx = cos(rx), sx = sin(rx);
    float cy = cos(ry), sy = sin(ry);
    float cz = cos(rz), sz = sin(rz);
    RotMat m;
    m.m[0][0] = cy * cz;             m.m[0][1] = sx*sy*cz - cx*sz;  m.m[0][2] = cx*sy*cz + sx*sz;
    m.m[1][0] = cy * sz;             m.m[1][1] = sx*sy*sz + cx*cz;  m.m[1][2] = cx*sy*sz - sx*cz;
    m.m[2][0] = -sy;                 m.m[2][1] = sx * cy;            m.m[2][2] = cx * cy;
    return m;
}

static float3 applyRotMat(float3 v, RotMat m) {
    return (float3)(
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z);
}

static float edgeFn(float ax, float ay, float bx, float by, float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static uint packColor(float3 c) {
    uint r = (uint)(clamp(c.x, 0.0f, 1.0f) * 255.0f);
    uint g = (uint)(clamp(c.y, 0.0f, 1.0f) * 255.0f);
    uint b = (uint)(clamp(c.z, 0.0f, 1.0f) * 255.0f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Kernel: one work item per triangle
// C float3 = struct { float x,y,z,w } = 16 bytes = OpenCL float4, so all
// float3 CPU buffers map to float4 here.
__kernel void renderObjects(
    // Flat triangle geometry across all objects
    __global const float4 *v1,
    __global const float4 *v2,
    __global const float4 *v3,
    __global const float4 *normals,
    __global const int    *materialIds,
    const int              triangleCount,

    // Which object each triangle belongs to
    __global const int *triObjectIds,

    // Per-object transforms (indexed by object id)
    __global const float4 *objPositions,  // xyz = world position
    __global const float4 *objRotations,  // xyz = Euler angles (radians)
    __global const float4 *objScales,     // xyz = scale

    // Camera (precomputed by RenderSetup)
    const float4 camRight,
    const float4 camUp,
    const float4 camForward,
    const float4 camPosition,
    const float4 renderLightDir,
    const float4 halfVec,
    const float  fovScale,
    const float  aspect,
    const float  jitterX,
    const float  jitterY,
    const int    screenWidth,
    const int    screenHeight,

    // Materials packed as two float4 per entry
    __global const float4 *matColors,  // xyz = rgb, w = roughness
    __global const float4 *matProps,   // x = metallic, y = emission

    // Output buffers
    __global uint   *framebuffer,
    __global uint   *depthBufferInt,   // float depth stored as uint for atomic_min
    __global float4 *normalBuffer,
    __global float4 *positionBuffer,
    __global float4 *reflectBuffer
) {
    int triIdx = get_global_id(0);
    if (triIdx >= triangleCount) return;

    // Object transform
    int   objId  = triObjectIds[triIdx];
    float3 oPos  = objPositions[objId].xyz;
    float3 oRot  = objRotations[objId].xyz;
    float3 oSca  = objScales[objId].xyz;

    RotMat rot = buildRotMat(oRot.x, oRot.y, oRot.z);

    // Transform vertices to world space (rotate, scale, translate)
    float3 wv0 = applyRotMat(v1[triIdx].xyz, rot) * oSca + oPos;
    float3 wv1 = applyRotMat(v2[triIdx].xyz, rot) * oSca + oPos;
    float3 wv2 = applyRotMat(v3[triIdx].xyz, rot) * oSca + oPos;
    float3 wn  = applyRotMat(normals[triIdx].xyz, rot);

    // Back-face culling
    float3 camPos3 = camPosition.xyz;
    if (dot(wn, camPos3 - wv0) <= 0.0f) return;

    // Camera-space z for each vertex
    float3 camFwd3 = camForward.xyz;
    float z0 = dot(wv0 - camPos3, camFwd3);
    float z1 = dot(wv1 - camPos3, camFwd3);
    float z2 = dot(wv2 - camPos3, camFwd3);
    if (z0 <= 0.01f && z1 <= 0.01f && z2 <= 0.01f) return;

    // Project to NDC then screen space
    float3 camRight3 = camRight.xyz;
    float3 camUp3    = camUp.xyz;
    float3 c0 = wv0 - camPos3, c1 = wv1 - camPos3, c2 = wv2 - camPos3;

    float x0 = dot(c0, camRight3) / (z0 * fovScale * aspect);
    float y0 = dot(c0, camUp3)    / (z0 * fovScale);
    float x1 = dot(c1, camRight3) / (z1 * fovScale * aspect);
    float y1 = dot(c1, camUp3)    / (z1 * fovScale);
    float x2 = dot(c2, camRight3) / (z2 * fovScale * aspect);
    float y2 = dot(c2, camUp3)    / (z2 * fovScale);

    float hw = (float)screenWidth  * 0.5f;
    float hh = (float)screenHeight * 0.5f;
    float sx0 = (x0 + 1.0f) * hw + jitterX;
    float sy0 = (1.0f - y0) * hh + jitterY;
    float sx1 = (x1 + 1.0f) * hw + jitterX;
    float sy1 = (1.0f - y1) * hh + jitterY;
    float sx2 = (x2 + 1.0f) * hw + jitterX;
    float sy2 = (1.0f - y2) * hh + jitterY;

    int minX = max(0,              (int)min(min(sx0, sx1), sx2));
    int maxX = min(screenWidth-1,  (int)max(max(sx0, sx1), sx2));
    int minY = max(0,              (int)min(min(sy0, sy1), sy2));
    int maxY = min(screenHeight-1, (int)max(max(sy0, sy1), sy2));
    if (minX > maxX || minY > maxY) return;

    float area = edgeFn(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabs(area) <= 1e-8f) return;
    float areaSign = area > 0.0f ? 1.0f : -1.0f;
    float invArea  = 1.0f / area;
    float invZ0 = 1.0f / z0, invZ1 = 1.0f / z1, invZ2 = 1.0f / z2;

    // Lighting — Phong, matches CPU RenderObject
    int    matId  = materialIds[triIdx];
    float4 mc     = matColors[matId];
    float4 mp     = matProps[matId];
    float3 base   = mc.xyz;
    float  rough  = mc.w;
    float  metal  = mp.x;

    float3 ld3 = renderLightDir.xyz;
    float3 hv3 = halfVec.xyz;
    float NdotL    = max(0.0f, dot(wn, ld3));
    float NdotH    = max(0.0f, dot(wn, hv3));
    float shininess = (1.0f - rough) * 128.0f + 1.0f;
    float spec      = pow(NdotH, shininess);

    float3 diffuse  = base * ((1.0f - metal) * NdotL);
    float3 specCol  = base * metal + (float3)(1.0f - metal, 1.0f - metal, 1.0f - metal);
    float3 specular = specCol * (spec * (1.0f - rough * 0.7f));
    float3 ambient  = base * 0.1f;
    float3 fc       = clamp(ambient + diffuse + specular, 0.0f, 1.0f);
    uint   packed   = packColor(fc);

    // Incremental edge traversal
    float w0dx = sy2 - sy1, w0dy = -(sx2 - sx1);
    float w1dx = sy0 - sy2, w1dy = -(sx0 - sx2);
    float w2dx = sy1 - sy0, w2dy = -(sx1 - sx0);
    float startPx = minX + 0.5f, startPy = minY + 0.5f;
    float w0Row = edgeFn(sx1, sy1, sx2, sy2, startPx, startPy);
    float w1Row = edgeFn(sx2, sy2, sx0, sy0, startPx, startPy);
    float w2Row = edgeFn(sx0, sy0, sx1, sy1, startPx, startPy);

    for (int y = minY; y <= maxY; y++) {
        float w0 = w0Row, w1 = w1Row, w2 = w2Row;
        for (int x = minX; x <= maxX; x++) {
            if ((w0 * areaSign) >= 0.0f && (w1 * areaSign) >= 0.0f && (w2 * areaSign) >= 0.0f) {
                float invZ = (w0 * invZ0 + w1 * invZ1 + w2 * invZ2) * invArea;
                float depth = 1.0f / invZ;
                int   idx   = y * screenWidth + x;

                // Atomic depth test: positive floats preserve order as uint
                uint depthU = as_uint(depth);
                uint old = atomic_min(&depthBufferInt[idx], depthU);
                if (depthU < old) {
                    // We set a new minimum depth — write shading data
                    float p0 = w0 * invZ0, p1 = w1 * invZ1, p2 = w2 * invZ2;
                    float invP = invArea * depth;
                    float3 wp = (float3)(
                        (wv0.x*p0 + wv1.x*p1 + wv2.x*p2) * invP,
                        (wv0.y*p0 + wv1.y*p1 + wv2.y*p2) * invP,
                        (wv0.z*p0 + wv1.z*p1 + wv2.z*p2) * invP);
                    float3 rv   = wp - camPos3;
                    float3 refl = rv - 2.0f * dot(rv, wn) * wn;

                    framebuffer[idx]    = packed;
                    normalBuffer[idx]   = (float4)(wn,   0.0f);
                    positionBuffer[idx] = (float4)(wp,   0.0f);
                    reflectBuffer[idx]  = (float4)(refl, 0.0f);
                }
            }
            w0 += w0dx; w1 += w1dx; w2 += w2dx;
        }
        w0Row += w0dy; w1Row += w1dy; w2Row += w2dy;
    }
}
