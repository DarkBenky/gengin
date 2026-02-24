#ifndef GPU_RASTER_H
#define GPU_RASTER_H

#include "format.h"
#include "../../object/object.h"
#include "../../object/material/material.h"

// Per-material data laid out for the GPU (32 bytes, matches GpuMaterial in rester.cl)
typedef struct {
	float r, g, b, _pad0; // color (matches float4 color in kernel)
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
	CL_Context ctx;
	CL_Pipeline rasterPip; // single dispatch over all triangles (renderAll kernel)
	CL_Pipeline albedoPip; // resolveAlbedo kernel
	// Per-frame output buffers
	CL_Buffer depthBuf;
	CL_Buffer normalBuf;
	CL_Buffer matObjBuf;
	CL_Buffer albedoBuf;
	// Persistent scene geometry — uploaded once in UploadScene
	CL_Buffer flatV1Buf, flatV2Buf, flatV3Buf, flatNormalBuf, flatMatIdBuf;
	CL_Buffer triObjIdBuf;	// int per triangle: maps flat tri index -> object index
	CL_Buffer transformBuf; // objectCount * 3 float4s; re-written each frame
	int totalTriangles;
	int objectCount;
	// Pre-uploaded material palette
	CL_Buffer materialBuf;
	int matCount;
	int width;
	int height;
	int gpuOk;
	float lastRasterMs;
	float lastResolveMs;
} GpuRaster;

// Create context, compile kernels, allocate output buffers.
// kernelDir: path to the directory containing rester.cl (e.g. "render/gpu/kernels").
void GpuRaster_Init(GpuRaster *r, int width, int height, const char *kernelDir);

// Upload all object geometry and the material palette to the GPU once.
// Call after Init and before the first frame.  Re-call only if the scene changes.
void GpuRaster_UploadScene(GpuRaster *r, const Object *objects, int objectCount, const MaterialLib *lib);

// Reset depth buffer to FLT_MAX and clear material/object IDs.  Call once per frame before rendering.
void GpuRaster_Clear(GpuRaster *r);

// Single dispatch over all objects — uploads per-frame transforms and rasterizes.
void GpuRaster_RenderAll(GpuRaster *r, const Camera *cam, const Object *objects, int objectCount);

// Run the resolveAlbedo kernel using the pre-uploaded material palette.
void GpuRaster_Resolve(GpuRaster *r);

// Read back the ARGB framebuffer into pixels (width*height uint32_t values).
void GpuRaster_ReadAlbedo(GpuRaster *r, uint32_t *pixels);

// Read back depth + normals from GPU and reconstruct positionBuffer + reflectBuffer on CPU,
// so ShadowPostProcess can run on the GPU-rasterized frame.
void GpuRaster_ReadBuffers(GpuRaster *r, Camera *cam);

// Release all GPU resources.
void GpuRaster_Destroy(GpuRaster *r);

#endif // GPU_RASTER_H
