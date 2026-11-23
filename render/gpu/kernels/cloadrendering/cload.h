#ifndef CLOAD_H
#define CLOAD_H

#include "../../format.h"
#include "../../../../object/object.h"
#include "../../../../object/format.h"

typedef struct {
	CL_Context ctx;
	int ownsCtx; // 1 if ctx was created here, 0 if it belongs to the window (CL/GL sharing)
	CL_Pipeline pipeline;
	CL_Buffer outputBuf;
	CL_Buffer depthBuf; // scene depth buffer uploaded each frame
	CL_Pipeline godRayPipeline;
	CL_Pipeline godRaySrcPipeline;
	CL_Buffer godRayBuf;
	CL_Buffer godRaySrcBuf; // fused transmittance*isSky, one float per pixel
	CL_Pipeline compositePipeline;
	CL_Buffer framebufferBuf; // pinned float4 HDR framebuffer — upload framebufferHdr, composite on GPU
	CL_Pipeline blurPipeline;
	CL_Buffer outputBlurBuf;
	CL_Pipeline tonemapPipeline; // HDR -> 8-bit BGRA, output goes to ldrBuf
	CL_Buffer ldrBuf; // device-side 8-bit BGRA frame, passed to mfb_update_gpu
	int width;
	int height;
	int enabled; // 0 = OpenCL init failed or GENGIN_NO_CLOUDS set; every entry point no-ops
} CloudRenderer;

// One-time init: compiles kernel from kernelPath, allocates output buffers.
// sharedCtx: CL context created from the display window (CL/GL sharing) so the tonemapped frame can be presented without a read back. NULL = create a private context.
void CloudRenderer_Init(CloudRenderer *cr, int width, int height, const char *kernelPath, CL_Context *sharedCtx);

typedef struct {
	float3 baseColor;
	float3 extinctionScale;	 // optical density per channel (try 10-30)
	float3 shadowExtinction; // per-channel extinction in shadow march — keep low (0.5-3)
	float scatterG;			 // Henyey-Greenstein asymmetry (-1..1, 0=isotropic)
	float shadowDist;		 // shadow march distance in local space
	float3 ambientLight;	 // minimum light level on shadow side, can be colored
	// god rays — set godRays=1 to enable, sunScreenPos computed automatically from cam->lightDir
	int godRays;
	float3 godRayColor;	   // additive tint, e.g. (1, 0.95, 0.8)
	float godRayIntensity; // brightness multiplier, try 1.0-2.0
	float godRayDecay;	   // per-step attenuation, try 0.97
	int blurRadius;		   // gaussian blur radius in pixels (0 = skip, standard 3-8)
} CloudParams;

// Render clouds for one frame. vol->gpuDensity must already be uploaded via UploadVolumeToGpu.
void CloudRenderer_Render(CloudRenderer *cr, Volume *vol, const Camera *cam, CloudParams params);

// Blend clouds + god rays onto the GPU copy of cam->framebufferHdr (call after CloudRenderer_Render).
// keepOnGpu: skip the HDR read back, the frame stays on device for CloudRenderer_Tonemap.
void CloudRenderer_Composite(CloudRenderer *cr, Camera *cam, bool keepOnGpu);

// Tonemap the GPU framebuffer into cr->ldrBuf (8-bit BGRA). Uploads cam->framebufferHdr itself when
// the composite pass is disabled. Returns 0 when the GPU path is unavailable.
int CloudRenderer_Tonemap(CloudRenderer *cr, Camera *cam);

void CloudRenderer_Destroy(CloudRenderer *cr);

#endif // CLOAD_H
