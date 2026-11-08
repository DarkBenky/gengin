#ifndef OPENCL_RASTER_H
#define OPENCL_RASTER_H

#include "../render/gpu/format.h"
#include "../object/object.h"
#include "../object/scene.h"
#include "../object/material/material.h"
#include "../skybox/skybox.h"

// GPU record layouts — must match the structs in raster.cl (the kernel is compiled
// from source, so the layout cannot be shared with it).
typedef struct {
	float4 v1;
	float4 v2;
	float4 v3;
	float4 normal;
	uint uv01; // uv1.x | (uv1.y << 16)
	uint uv23;
	uint uv45;
	int materialId;
} RasterTri; // 112 bytes

typedef struct {
	float4 position;
	float4 scale;
	float4 rot0;
	float4 rot1;
	float4 rot2;
	int triStart;
	int triCount;
	int flags;
	int sceneIndex; // index into the caller's ObjectList, echoed into objectIdBuffer
} RasterObject; // 96 bytes

typedef struct {
	float4 color;
	float roughness;
	float metallic;
	float emission;
	int textureSlot;
} RasterMaterial; // 40 bytes

typedef struct {
	int offset;
	int size;
} RasterTexture; // 8 bytes

typedef struct {
	float4 position;
	float4 colorEmission; // xyz = material color * material emission
} RasterEmitter;

typedef struct {
	float4 center;
	float4 right;
	float4 up;
	float4 fwd;
	float4 params; // x = map size, y = world units per texel, z = 1 when enabled
} RasterShadow;

#define RASTER_FLAG_TEXTURE 1
#define RASTER_FLAG_CULL 2

// Which Camera buffers Raster_Render copies back: every transfer crosses PCIe (60 bytes
// per pixel for the full set), so the default is only what a frame needs.
#define RASTER_OUT_FRAMEBUFFER 1
#define RASTER_OUT_DEPTH 2
#define RASTER_OUT_NORMAL 4
#define RASTER_OUT_POSITION 8
#define RASTER_OUT_OBJECT_ID 16
#define RASTER_OUT_TRIANGLE_ID 32
#define RASTER_OUT_ALL 63
#define RASTER_OUT_FRAME (RASTER_OUT_FRAMEBUFFER | RASTER_OUT_DEPTH)

// Self-contained OpenCL rasterizer into a Camera's G-buffers; owns its context and buffers.
// Call RenderSetup(...) before Raster_Render, and Raster_UploadScene again after anything
// that rebuilds or merges geometry (ObjectList_Merge bakes transforms into the vertices).
typedef struct {
	CL_Context ctx;
	CL_Pipeline depthPipeline;
	CL_Pipeline shadePipeline;
	CL_Pipeline skyPipeline;
	CL_Pipeline aoPipeline;
	CL_Pipeline aoBlurPipeline;
	CL_Pipeline aoApplyPipeline;
	CL_Pipeline shadowPipeline;

	CL_Buffer triBuf;
	CL_Buffer triObjectBuf;
	CL_Buffer materialBuf;
	CL_Buffer textureTableBuf;
	CL_Buffer colorMapBuf;
	CL_Buffer normalMapBuf;
	CL_Buffer materialMapBuf;
	CL_Buffer emitterBuf; // up to RASTER_MAX_EMITTERS point lights, uploaded with the scene
	int emitterCount;

	CL_Buffer skyBuf;
	int skyWidth;
	int skyHeight;

	CL_Buffer objectBuf; // pinned — TRS + rotation rows, rewritten every frame
	CL_Buffer depthUBuf; // packed view-Z, cleared to 0xFFFFFFFF each frame
	CL_Buffer aoBufA;    // ambient occlusion, ping-ponged by the blur passes
	CL_Buffer aoBufB;
	CL_Buffer shadowBuf; // light-space depth (uint, monotonic float bits)
	float aoStrength; // 0 disables the AO passes; the default keeps contact darkening visible
	int shadowMapSize; // 0 disables the shadow pass; 2048 is plenty for this scene
	RasterShadow shadow; // rebuilt every frame by Raster_Render
	unsigned readbackMask; // RASTER_OUT_* bits; defaults to framebuffer + depth

	CL_Buffer framebufferBuf;
	CL_Buffer depthBuf;
	CL_Buffer normalBuf;
	CL_Buffer positionBuf;
	CL_Buffer objectIdBuf;
	CL_Buffer triangleIdBuf;

	// Host-side mirror of the static packing: triStarts[i] is where object i's triangles
	// begin in triBuf, in the same object order the kernels index objects by.
	int *triStarts;

	int width;
	int height;
	int triCount;
	int objectCount;
	int enabled; // 0 = OpenCL init failed; every entry point no-ops
} RasterRenderer;

// On failure the renderer disables itself, so a caller can stay on the CPU path.
void Raster_Init(RasterRenderer *rr, const char *kernelPath, int width, int height);

// Safe to call again after a scene rebuild.
void Raster_UploadScene(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib);

// With a skybox set the renderer fills uncovered pixels and the reflection samples it.
void Raster_SetSkybox(RasterRenderer *rr, const Skybox *skybox);

// Synchronous. Only readbackMask's buffers are transferred back. World bounds must be
// current (Object_UpdateWorldBounds); they size the shadow map. Uncovered pixels keep what
// the Camera already held, so the caller owns the sky/clear policy.
void Raster_Render(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib, Camera *cam);

void Raster_Destroy(RasterRenderer *rr);

#endif // OPENCL_RASTER_H
