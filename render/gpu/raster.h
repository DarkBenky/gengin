#ifndef GPU_RASTER_H
#define GPU_RASTER_H

#include "format.h"
#include "../../object/object.h"
#include "../../object/material/material.h"

// Per-material data laid out for the GPU (32 bytes, matches GpuMaterial in rester.cl)
typedef struct {
    float r, g, b, _pad0;   // color (matches float4 color in kernel)
    float roughness;
    float metallic;
    float emission;
    float _pad1;
} GpuMaterial;

// int2 returned by the rasterizer for each pixel
typedef struct {
    int materialId;
    int objectId;
} GpuMatObj;

typedef struct {
    CL_Context  ctx;
    CL_Pipeline rasterPip;   // renderObject kernel
    CL_Pipeline albedoPip;   // resolveAlbedo kernel
    CL_Buffer   depthBuf;    // int buffer — float bits for atomic depth test
    CL_Buffer   normalBuf;   // float4 buffer (float3 padded to 16 bytes)
    CL_Buffer   matObjBuf;   // GpuMatObj (int2) per pixel
    CL_Buffer   albedoBuf;   // uint32 ARGB per pixel
    int         width;
    int         height;
    int         gpuOk;       // 0 if init failed, 1 if ready
} GpuRaster;

// Create context, compile kernels, allocate output buffers.
// kernelDir: path to the directory containing rester.cl (e.g. "render/gpu/kernels").
void GpuRaster_Init(GpuRaster *r, int width, int height, const char *kernelDir);

// Reset depth buffer to FLT_MAX and clear material/object IDs.  Call once per frame before rendering.
void GpuRaster_Clear(GpuRaster *r);

// Rasterize one object onto the depth / normal / material-ID buffers.
// objectId is a unique integer for this object (used to fill the objectId channel).
void GpuRaster_RenderObject(GpuRaster *r, const Object *obj, const Camera *cam, int objectId);

// Upload material palette and run the resolveAlbedo kernel to produce the final ARGB image.
void GpuRaster_Resolve(GpuRaster *r, const MaterialLib *lib);

// Read back the ARGB framebuffer produced by GpuRaster_Resolve into caller-supplied pixels array.
// pixels must be at least width * height uint32_t values.
void GpuRaster_ReadAlbedo(GpuRaster *r, uint32_t *pixels);

// Release all GPU resources.
void GpuRaster_Destroy(GpuRaster *r);

#endif // GPU_RASTER_H
