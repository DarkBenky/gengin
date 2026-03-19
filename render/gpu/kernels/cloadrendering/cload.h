#ifndef CLOAD_H
#define CLOAD_H

#include "../../format.h"
#include "../../../../object/object.h"
#include "../../../../object/format.h"

typedef struct {
	CL_Context ctx;
	CL_Pipeline pipeline;
	CL_Buffer outputBuf;
	CL_Buffer depthBuf; // scene depth buffer uploaded each frame
	float3 *outputCpu;
	int width;
	int height;
} CloudRenderer;

// One-time init: compiles kernel from kernelPath, allocates output buffers.
void CloudRenderer_Init(CloudRenderer *cr, int width, int height, const char *kernelPath);

typedef struct {
	float3 baseColor;
	float extinctionScale;	// optical density for opacity (try 10-30)
	float shadowExtinction; // extinction used only in shadow march — keep low (0.5-3) for visible lit clouds
	float scatterG;			// Henyey-Greenstein asymmetry (-1..1, 0=isotropic)
	float shadowDist;		// shadow march distance in local space
	float ambientLight;		// minimum light level on shadow side (0=hard, 0.3=soft)
} CloudParams;

// Render clouds for one frame. vol->gpuDensity must already be uploaded via UploadVolumeToGpu.
void CloudRenderer_Render(CloudRenderer *cr, Volume *vol, const Camera *cam, CloudParams params);

// Blend cloud luminance into cam->framebuffer (call after CloudRenderer_Render).
void CloudRenderer_Composite(const CloudRenderer *cr, Camera *cam);

void CloudRenderer_Destroy(CloudRenderer *cr);

#endif // CLOAD_H
