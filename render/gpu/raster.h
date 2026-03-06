#ifndef GPU_RASTER_H
#define GPU_RASTER_H

#include "../../object/format.h"
#include "../../object/object.h"
#include "../../object/material/material.h"

// Opaque GPU rasterizer state.
typedef struct GPURaster GPURaster;

// Initialize GPU rasterizer with static scene geometry.
// Returns NULL if no OpenCL device is available or compilation fails.
// Call once after loading the scene.
GPURaster *GPURaster_Init(const Object *objects, int objectCount,
                          const MaterialLib *lib,
                          int screenWidth, int screenHeight);

// Render all objects into the camera's output buffers using OpenCL.
// Fills: camera->framebuffer, depthBuffer, normalBuffer,
//        positionBuffer, reflectBuffer.
// Requires RenderSetup() to have been called first to populate camera
// vectors (right, up, fovScale, renderLightDir, halfVec, aspect).
void GPURaster_RenderObjects(GPURaster *raster,
                             const Object *objects, int objectCount,
                             Camera *camera);

// Reload geometry and material data when the scene changes (e.g. a different
// model is loaded).  This is equivalent to re-running the upload phase of
// GPURaster_Init without recreating the OpenCL context or output buffers.
// If objectCount differs from the value used at Init, the transform buffers
// are also reallocated to match the new count.
void GPURaster_Reload(GPURaster *raster,
                      const Object *objects, int objectCount,
                      const MaterialLib *lib);

void GPURaster_Destroy(GPURaster *raster);

#endif // GPU_RASTER_H
