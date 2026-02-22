// Pipeline
// Render 2D texture containing triangle IDs and object IDs for each pixel\
// Render Albedo, Normal, Depth, etc. using the triangle and object IDs from the first pass

// check if a triangle is visible from the camera (back-face culling + frustum culling)
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
    __global const float3 *normal,
    __global const int *materialId,
    const int triangleCount,

    // Output
    __global float *depthOut, // used for depth testing
    __global float3 *normalOut, // need to be rendered here because is part of Object data
    __global int2 *materialIdObjectIdOut,
)