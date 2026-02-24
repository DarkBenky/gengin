typedef struct Material {
	float3 color;
	float roughness;
	float metallic;
	float emission;
} Material;

inline bool isVisible(
    const float3 point,
    const float3 normal,
    const float3 cameraPosition,
    const float3 cameraForward,
    const float3 cameraRight,
    const float3 cameraUp,
    float fovY,
    float aspect
)
{
    float3 toPoint = point - cameraPosition;
    float z = dot(toPoint, cameraForward);
    if (z <= 0.0f)
        return false;
    if (dot(normal, -toPoint) <= 0.0f)
        return false;
    float x = dot(toPoint, cameraRight);
    float y = dot(toPoint, cameraUp);
    float tanHalfFovY = tanf(fovY * 0.5f);
    float tanHalfFovX = tanHalfFovY * aspect;
    if (fabsf(x) > z * tanHalfFovX)
        return false;
    if (fabsf(y) > z * tanHalfFovY)
        return false;
    return true;
}


__kernel void renderObjects(
    // Inputs
    const int *objectIds,
    const int *objectIdOffsets, // [obj1 offset][obj2 offset][obj3 offset]...
    const int2 *objectScreenBounds, // [obj1 bounds][obj2 bounds][obj3 bounds]...
    const int objectCount,

    // Camera
    const float3 cameraRight,
    const float3 cameraUp,
    const float3 cameraForward,
    const float3 cameraPosition,
    const float fov,
    const int screenWidth,
    const int screenHeight,

    // Object data
    const float3 *objectPositions, // [obj1 pos][obj2 pos][obj3 pos]...
    const float3 *objectRotations, // [obj1 rot][obj2 rot][obj3 rot]...
    const float3 *objectScales, // [obj1 scale][obj2 scale][obj3 scale]...

    // Object Geometry
    __global const float3 *v1,
    __global const float3 *v2,
    __global const float3 *v3,
    __global const float3 *normal,
    __global const int *materialId,
    const int triangleCount,

    // Material data
    const Material *materials, // [mat1][mat2][mat3]...

    // Output
    float4 *positionDepthBuffer, // [x][y] = (pos.x, pos.y, pos.z, depth)
    float4 *colorBuffer, // [x][y] = (r, g, b, _unused)
    float3 *normalBuffer, // [x][y] = (nx, ny, nz)
    float3 *reflectionBuffer, // [x][y] = (rx, ry, rz)
    int *materialIdBuffer, // [x][y] = materialId
    int *objectIdBuffer // [x][y] = objectId
)