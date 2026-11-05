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

// Must match RasterEmitter in raster.cl. One emissive object reduced to a point light.
typedef struct {
	float4 position;
	float4 colorEmission; // xyz = material color * material emission
} RasterEmitter;

// Must match RasterShadow in raster.cl. Built from the scene bounds and the light
// direction each frame; the reciprocals ride in .w to keep it a single kernel argument.
typedef struct {
	float4 center;
	float4 right;
	float4 up;
	float4 fwd;
	float4 params; // x = map size, y = world units per texel, z = 1 when enabled
} RasterShadow;

#define RASTER_FLAG_TEXTURE 1
#define RASTER_FLAG_CULL 2

// Which Camera buffers Raster_Render copies back to the host. Every transfer crosses PCIe
// (60 bytes per pixel for the full set), so the default is the pair a frame actually needs
// and the rest is opt-in for consumers like the parity harness.
#define RASTER_OUT_FRAMEBUFFER 1
#define RASTER_OUT_DEPTH 2
#define RASTER_OUT_NORMAL 4
#define RASTER_OUT_POSITION 8
#define RASTER_OUT_OBJECT_ID 16
#define RASTER_OUT_TRIANGLE_ID 32
#define RASTER_OUT_ALL 63
#define RASTER_OUT_FRAME (RASTER_OUT_FRAMEBUFFER | RASTER_OUT_DEPTH)

// Self-contained OpenCL rasterizer: renders the scene into a Camera's G-buffers with a
// two-pass triangle raster (depth scatter, then shade scatter). Independent of the CPU
// ray tracer — it owns its CL context and buffers, and does no per-frame allocation.
//
// Call order per frame: RenderSetup(...) first (it fills right/up/aspect/fovScale), then
// Raster_Render. Raster_UploadScene must run again after anything that rebuilds or merges
// geometry (ObjectList_Merge bakes transforms into the vertex arrays).
typedef struct {
	CL_Context ctx;
	CL_Pipeline depthPipeline;
	CL_Pipeline shadePipeline;
	CL_Pipeline skyPipeline;
	CL_Pipeline aoPipeline;
	CL_Pipeline aoBlurPipeline;
	CL_Pipeline aoApplyPipeline;
	CL_Pipeline shadowPipeline;

	// static scene (uploaded once per scene rebuild)
	CL_Buffer triBuf;
	CL_Buffer triObjectBuf;
	CL_Buffer materialBuf;
	CL_Buffer textureTableBuf;
	CL_Buffer colorMapBuf;
	CL_Buffer normalMapBuf;
	CL_Buffer materialMapBuf;
	CL_Buffer emitterBuf; // up to RASTER_MAX_EMITTERS point lights, uploaded with the scene
	int emitterCount;

	// skybox faces, back to back (see Raster_SetSkybox); skyWidth 0 = no sky
	CL_Buffer skyBuf;
	int skyWidth;
	int skyHeight;

	// per-frame / scratch
	CL_Buffer objectBuf; // pinned — TRS + rotation rows, rewritten every frame
	CL_Buffer depthUBuf; // packed view-Z, cleared to 0xFFFFFFFF each frame
	CL_Buffer aoBufA;    // ambient occlusion, ping-ponged by the blur passes
	CL_Buffer aoBufB;
	CL_Buffer shadowBuf; // light-space depth (uint, monotonic float bits)
	float aoStrength; // 0 disables the AO passes; the default keeps contact darkening visible
	int shadowMapSize; // 0 disables the shadow pass; 2048 is plenty for this scene
	RasterShadow shadow; // rebuilt every frame by Raster_Render
	unsigned readbackMask; // RASTER_OUT_* bits; defaults to framebuffer + depth

	// pinned G-buffer readback, one buffer per Camera output it writes
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

// Compiles raster.cl from kernelPath and allocates the frame buffers. On failure the
// renderer disables itself and prints why, so a caller can keep running on the CPU path.
void Raster_Init(RasterRenderer *rr, const char *kernelPath, int width, int height);

// Rebuilds every static GPU buffer from the scene: triangles, per-triangle object index,
// material table, concatenated texture maps. Safe to call again after a scene rebuild.
void Raster_UploadScene(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib);

// Uploads the six skybox faces. With a skybox set the renderer fills the pixels no
// geometry covers (and the reflection term samples it); without one those pixels stay
// untouched and Raster_Render leaves the sky to the caller.
void Raster_SetSkybox(RasterRenderer *rr, const Skybox *skybox);

// Rasterizes the scene into cam->{framebufferHdr, depthBuffer, normalBuffer,
// positionBuffer, objectIdBuffer, triangleIdBuffer}. Synchronous (CL_Finish inside).
// Only the buffers selected by readbackMask are transferred back to the host; the rest are
// still computed on the GPU and simply not copied. Object world bounds must be current
// (Object_UpdateWorldBounds) — they size the shadow map. Without a skybox, pixels the
// rasterizer does not cover keep whatever the Camera already held — the caller owns the
// sky/clear policy.
void Raster_Render(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib, Camera *cam);

void Raster_Destroy(RasterRenderer *rr);

#endif // OPENCL_RASTER_H
