// Pipeline
// Pass 1: renderObject  — dispatch(triangleCount) per object, writes depth / normals / mat+obj IDs
// Pass 2: resolveAlbedo — dispatch(width, height), reads above + material buffer, writes ARGB pixels

typedef struct {
    float4 color;      // xyz = albedo rgb, w unused
    float  roughness;
    float  metallic;
    float  emission;
    float  _pad;
} GpuMaterial;

inline float3 rotateX(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return (float3)(v.x, v.y * c - v.z * s, v.y * s + v.z * c);
}

inline float3 rotateY(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return (float3)(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

inline float3 rotateZ(float3 v, float a) {
    float c = cos(a), s = sin(a);
    return (float3)(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}

inline float3 rotateXYZ(float3 v, float3 rot) {
    v = rotateX(v, rot.x);
    v = rotateY(v, rot.y);
    v = rotateZ(v, rot.z);
    return v;
}

inline float edgeFunc(float ax, float ay, float bx, float by, float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

// No atomics: plain read-check-write. Races at depth boundaries are benign (one valid triangle wins).

__kernel void renderObject(
    // Inputs
    const int objectId,

    // Camera
    const float3 cameraRight,
    const float3 cameraUp,
    const float3 cameraForward,
    const float3 cameraPosition,
    const float fov,
    const int screenWidth,
    const int screenHeight,

    // Object data
    const float3 objectPosition,
    const float3 objectRotation,
    const float3 objectScale,

    // Object Geometry
    __global const float3 *v1,
    __global const float3 *v2,
    __global const float3 *v3,
    __global const float3 *normals,
    __global const int    *materialId,
    const int triangleCount,

    // Output
    __global int    *depthBitsOut,            // float bits stored as int, positive IEEE 754 — int comparison matches float comparison
    __global float3       *normalOut,
    __global int2         *materialIdObjectIdOut
) {
    int tid = get_global_id(0);
    if (tid >= triangleCount) return;

    // Transform vertices: rotate then scale then translate (matches CPU pipeline)
    float3 p0 = rotateXYZ(v1[tid], objectRotation);
    float3 p1 = rotateXYZ(v2[tid], objectRotation);
    float3 p2 = rotateXYZ(v3[tid], objectRotation);
    p0 = (float3)(p0.x * objectScale.x, p0.y * objectScale.y, p0.z * objectScale.z) + objectPosition;
    p1 = (float3)(p1.x * objectScale.x, p1.y * objectScale.y, p1.z * objectScale.z) + objectPosition;
    p2 = (float3)(p2.x * objectScale.x, p2.y * objectScale.y, p2.z * objectScale.z) + objectPosition;

    float3 normal = normalize(rotateXYZ(normals[tid], objectRotation));

    // Back-face cull
    if (dot(normal, cameraPosition - p0) <= 0.0f) return;

    // Project to screen
    float aspect   = (float)screenWidth / (float)screenHeight;
    float fovScale = tan(fov * 0.5f * 3.14159265f / 180.0f);

    float3 c0 = p0 - cameraPosition;
    float3 c1 = p1 - cameraPosition;
    float3 c2 = p2 - cameraPosition;

    float z0 = dot(c0, cameraForward);
    float z1 = dot(c1, cameraForward);
    float z2 = dot(c2, cameraForward);
    if (z0 <= 0.01f && z1 <= 0.01f && z2 <= 0.01f) return;

    float x0 = dot(c0, cameraRight) / (z0 * fovScale * aspect);
    float y0 = dot(c0, cameraUp)    / (z0 * fovScale);
    float x1 = dot(c1, cameraRight) / (z1 * fovScale * aspect);
    float y1 = dot(c1, cameraUp)    / (z1 * fovScale);
    float x2 = dot(c2, cameraRight) / (z2 * fovScale * aspect);
    float y2 = dot(c2, cameraUp)    / (z2 * fovScale);

    float sx0 = (x0 + 1.0f) * 0.5f * screenWidth;
    float sy0 = (1.0f - y0) * 0.5f * screenHeight;
    float sx1 = (x1 + 1.0f) * 0.5f * screenWidth;
    float sy1 = (1.0f - y1) * 0.5f * screenHeight;
    float sx2 = (x2 + 1.0f) * 0.5f * screenWidth;
    float sy2 = (1.0f - y2) * 0.5f * screenHeight;

    int minX = max(0,              (int)min(min(sx0, sx1), sx2));
    int maxX = min(screenWidth  - 1, (int)max(max(sx0, sx1), sx2));
    int minY = max(0,              (int)min(min(sy0, sy1), sy2));
    int maxY = min(screenHeight - 1, (int)max(max(sy0, sy1), sy2));

    float area = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabs(area) <= 1e-8f) return;
    float areaSign = area > 0.0f ? 1.0f : -1.0f;
    float invArea  = 1.0f / area;

    float invZ0 = 1.0f / z0;
    float invZ1 = 1.0f / z1;
    float invZ2 = 1.0f / z2;

    int matId = materialId[tid];

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float py = y + 0.5f;
            float w0 = edgeFunc(sx1, sy1, sx2, sy2, px, py);
            float w1 = edgeFunc(sx2, sy2, sx0, sy0, px, py);
            float w2 = edgeFunc(sx0, sy0, sx1, sy1, px, py);
            if ((w0 * areaSign) < 0.0f || (w1 * areaSign) < 0.0f || (w2 * areaSign) < 0.0f)
                continue;

            float invZ = (w0 * invZ0 + w1 * invZ1 + w2 * invZ2) * invArea;
            float depth = 1.0f / invZ;
            int   idx   = y * screenWidth + x;

            int ival = as_int(depth);
            if (depthBitsOut[idx] > ival) {
                depthBitsOut[idx]           = ival;
                normalOut[idx]              = normal;
                materialIdObjectIdOut[idx]  = (int2)(matId, objectId);
            }
        }
    }
}

// Single dispatch over ALL object triangles — avoids per-object kernel launch overhead.
// transforms layout: 3 consecutive float4s per object: [pos.xyz, rot.xyz, scl.xyz]
__kernel void renderAll(
    const float3 cameraRight,
    const float3 cameraUp,
    const float3 cameraForward,
    const float3 cameraPosition,
    const float fov,
    const int screenWidth,
    const int screenHeight,

    __global const float3 *v1,
    __global const float3 *v2,
    __global const float3 *v3,
    __global const float3 *normals,
    __global const int    *materialId,
    __global const int    *triObjectId,
    const int triangleCount,

    __global const float4 *transforms,  // objectCount * 3 float4s

    __global int    *depthBitsOut,
    __global float3 *normalOut,
    __global int2   *materialIdObjectIdOut
) {
    int tid = get_global_id(0);
    if (tid >= triangleCount) return;

    int oid = triObjectId[tid];
    float3 objectPosition = transforms[oid * 3 + 0].xyz;
    float3 objectRotation = transforms[oid * 3 + 1].xyz;
    float3 objectScale    = transforms[oid * 3 + 2].xyz;

    float3 p0 = rotateXYZ(v1[tid], objectRotation);
    float3 p1 = rotateXYZ(v2[tid], objectRotation);
    float3 p2 = rotateXYZ(v3[tid], objectRotation);
    p0 = (float3)(p0.x * objectScale.x, p0.y * objectScale.y, p0.z * objectScale.z) + objectPosition;
    p1 = (float3)(p1.x * objectScale.x, p1.y * objectScale.y, p1.z * objectScale.z) + objectPosition;
    p2 = (float3)(p2.x * objectScale.x, p2.y * objectScale.y, p2.z * objectScale.z) + objectPosition;

    float3 normal = normalize(rotateXYZ(normals[tid], objectRotation));

    if (dot(normal, cameraPosition - p0) <= 0.0f) return;

    float aspect   = (float)screenWidth / (float)screenHeight;
    float fovScale = tan(fov * 0.5f * 3.14159265f / 180.0f);

    float3 c0 = p0 - cameraPosition;
    float3 c1 = p1 - cameraPosition;
    float3 c2 = p2 - cameraPosition;

    float z0 = dot(c0, cameraForward);
    float z1 = dot(c1, cameraForward);
    float z2 = dot(c2, cameraForward);
    if (z0 <= 0.01f && z1 <= 0.01f && z2 <= 0.01f) return;

    float x0 = dot(c0, cameraRight) / (z0 * fovScale * aspect);
    float y0 = dot(c0, cameraUp)    / (z0 * fovScale);
    float x1 = dot(c1, cameraRight) / (z1 * fovScale * aspect);
    float y1 = dot(c1, cameraUp)    / (z1 * fovScale);
    float x2 = dot(c2, cameraRight) / (z2 * fovScale * aspect);
    float y2 = dot(c2, cameraUp)    / (z2 * fovScale);

    float sx0 = (x0 + 1.0f) * 0.5f * screenWidth;
    float sy0 = (1.0f - y0) * 0.5f * screenHeight;
    float sx1 = (x1 + 1.0f) * 0.5f * screenWidth;
    float sy1 = (1.0f - y1) * 0.5f * screenHeight;
    float sx2 = (x2 + 1.0f) * 0.5f * screenWidth;
    float sy2 = (1.0f - y2) * 0.5f * screenHeight;

    int minX = max(0,               (int)min(min(sx0, sx1), sx2));
    int maxX = min(screenWidth  - 1, (int)max(max(sx0, sx1), sx2));
    int minY = max(0,               (int)min(min(sy0, sy1), sy2));
    int maxY = min(screenHeight - 1, (int)max(max(sy0, sy1), sy2));

    float area = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
    if (fabs(area) <= 1e-8f) return;
    float areaSign = area > 0.0f ? 1.0f : -1.0f;
    float invArea  = 1.0f / area;

    float invZ0 = 1.0f / z0, invZ1 = 1.0f / z1, invZ2 = 1.0f / z2;
    int matId = materialId[tid];

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edgeFunc(sx1, sy1, sx2, sy2, px, py);
            float w1 = edgeFunc(sx2, sy2, sx0, sy0, px, py);
            float w2 = edgeFunc(sx0, sy0, sx1, sy1, px, py);
            if ((w0 * areaSign) < 0.0f || (w1 * areaSign) < 0.0f || (w2 * areaSign) < 0.0f)
                continue;

            float invZ = (w0 * invZ0 + w1 * invZ1 + w2 * invZ2) * invArea;
            float depth = 1.0f / invZ;
            int   idx   = y * screenWidth + x;
            int   ival  = as_int(depth);
            if (depthBitsOut[idx] > ival) {
                depthBitsOut[idx]          = ival;
                normalOut[idx]             = normal;
                materialIdObjectIdOut[idx] = (int2)(matId, oid);
            }
        }
    }
}

// Resolves per-pixel material IDs into packed ARGB output (for albedo display)
__kernel void resolveAlbedo(
    const int screenWidth,
    const int screenHeight,
    __global const int2       *materialIdObjectIdIn,
    __global const GpuMaterial *materials,
    const int materialCount,
    __global int              *rgbaOut    // 0xAARRGGBB packed output
) {
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= screenWidth || y >= screenHeight) return;

    int idx    = y * screenWidth + x;
    int2 matObj = materialIdObjectIdIn[idx];

    if (matObj.x < 0 || matObj.x >= materialCount) {
        rgbaOut[idx] = 0xFF000000;
        return;
    }

    GpuMaterial mat = materials[matObj.x];
    int r = clamp((int)(mat.color.x * 255.0f), 0, 255);
    int g = clamp((int)(mat.color.y * 255.0f), 0, 255);
    int b = clamp((int)(mat.color.z * 255.0f), 0, 255);
    rgbaOut[idx] = 0xFF000000 | (r << 16) | (g << 8) | b;
}