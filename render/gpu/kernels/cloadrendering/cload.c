#include "cload.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Recomputes cached inverse TRS matrix rows on the Volume (mirrors Object_UpdateWorldBounds).
static void updateVolumeCache(Volume *vol) {
	float cx = cosf(vol->rotation.x), sx = sinf(vol->rotation.x);
	float cy = cosf(vol->rotation.y), sy = sinf(vol->rotation.y);
	float cz = cosf(vol->rotation.z), sz = sinf(vol->rotation.z);
	float isx = 1.0f / vol->scale.x, isy = 1.0f / vol->scale.y, isz = 1.0f / vol->scale.z;
	vol->_invScale = (float3){isx * (cy * cz), isx * (cy * sz), isx * (-sy), 0.0f};
	vol->_invRotSin = (float3){isy * (-cx * sz + sx * sy * cz), isy * (cx * cz + sx * sy * sz), isy * (sx * cy), 0.0f};
	vol->_invRotCos = (float3){isz * (sx * sz + cx * sy * cz), isz * (-sx * cz + cx * sy * sz), isz * (cx * cy), 0.0f};
	vol->_fwdRot0 = (float3){cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz, 0.0f};
	vol->_fwdRot1 = (float3){cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz, 0.0f};
	vol->_fwdRot2 = (float3){-sy, sx * cy, cx * cy, 0.0f};
}

void CloudRenderer_Init(CloudRenderer *cr, int width, int height, const char *kernelPath) {
	cr->width = width;
	cr->height = height;
	cr->ctx = CL_Context_Create();
	cr->pipeline = CL_Pipeline_FromFile(&cr->ctx, kernelPath, "renderClouds", NULL);
	cr->godRayPipeline = CL_Pipeline_FromFile(&cr->ctx, kernelPath, "godRays", NULL);
	cr->compositePipeline = CL_Pipeline_FromFile(&cr->ctx, kernelPath, "compositeFrame", NULL);

	// Pinned (page-locked) buffers — DMA-direct transfers, no driver staging copy
	cr->outputBuf = CL_Buffer_CreatePinned(&cr->ctx, (size_t)width * height * sizeof(float4), CL_MEM_READ_WRITE);
	cr->depthBuf = CL_Buffer_CreatePinned(&cr->ctx, (size_t)width * height * sizeof(float), CL_MEM_READ_ONLY);
	cr->godRayBuf = CL_Buffer_CreatePinned(&cr->ctx, (size_t)width * height * sizeof(float4), CL_MEM_READ_WRITE);
	cr->framebufferBuf = CL_Buffer_CreatePinned(&cr->ctx, (size_t)width * height * sizeof(uint32), CL_MEM_READ_WRITE);
}

void CloudRenderer_Render(CloudRenderer *cr, Volume *vol, const Camera *cam, CloudParams params) {
	updateVolumeCache(vol);

	// Upload scene depth via pinned map — avoids a pageable memcpy inside the driver
	void *depthPtr = CL_Buffer_Map(&cr->ctx, &cr->depthBuf, CL_MAP_WRITE_INVALIDATE_REGION);
	memcpy(depthPtr, cam->depthBuffer, (size_t)cr->width * cr->height * sizeof(float));
	CL_Buffer_Unmap(&cr->ctx, &cr->depthBuf, depthPtr);

	cl_kernel k = cr->pipeline.kernel;
	int a = 0;
	clSetKernelArg(k, a++, sizeof(float3), &vol->position);
	clSetKernelArg(k, a++, sizeof(float3), &vol->rotation);
	clSetKernelArg(k, a++, sizeof(float3), &vol->scale);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_invScale);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_invRotSin);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_invRotCos);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_fwdRot0);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_fwdRot1);
	clSetKernelArg(k, a++, sizeof(float3), &vol->_fwdRot2);
	clSetKernelArg(k, a++, sizeof(cl_mem), &vol->gpuDensity.buf);
	clSetKernelArg(k, a++, sizeof(float3), &cam->position);
	clSetKernelArg(k, a++, sizeof(float3), &cam->forward);
	clSetKernelArg(k, a++, sizeof(float3), &cam->up);
	clSetKernelArg(k, a++, sizeof(float3), &cam->right);
	clSetKernelArg(k, a++, sizeof(float), &cam->fovScale);
	clSetKernelArg(k, a++, sizeof(int), &cam->screenWidth);
	clSetKernelArg(k, a++, sizeof(int), &cam->screenHeight);
	clSetKernelArg(k, a++, sizeof(float3), &cam->lightDir);
	clSetKernelArg(k, a++, sizeof(float3), &params.baseColor);
	clSetKernelArg(k, a++, sizeof(float), &params.extinctionScale);
	clSetKernelArg(k, a++, sizeof(float), &params.shadowExtinction);
	clSetKernelArg(k, a++, sizeof(float), &params.scatterG);
	clSetKernelArg(k, a++, sizeof(float), &params.shadowDist);
	clSetKernelArg(k, a++, sizeof(float), &params.ambientLight);
	int samplesPerPixel = 1;
	clSetKernelArg(k, a++, sizeof(cl_mem), &cr->outputBuf.buf);
	clSetKernelArg(k, a++, sizeof(int), &samplesPerPixel);
	clSetKernelArg(k, a++, sizeof(cl_mem), &cr->depthBuf.buf);
	CL_Dispatch2D(&cr->ctx, &cr->pipeline, (size_t)cr->width, (size_t)cr->height, 8, 8);

	if (params.godRays) {
		// project lightDir to screen space to find the sun position
		float3 ld = cam->lightDir;
		float ll = sqrtf(ld.x * ld.x + ld.y * ld.y + ld.z * ld.z);
		float3 ls = {ld.x / ll, ld.y / ll, ld.z / ll, 0.0f};
		float sdx = (ls.x * cam->right.x + ls.y * cam->right.y + ls.z * cam->right.z) / (cam->aspect * cam->fovScale);
		float sdy = (ls.x * cam->up.x + ls.y * cam->up.y + ls.z * cam->up.z) / cam->fovScale;
		float2 sunPos = {(sdx + 1.0f) * 0.5f, (1.0f - sdy) * 0.5f};

		cl_kernel gk = cr->godRayPipeline.kernel;
		int ga = 0;
		clSetKernelArg(gk, ga++, sizeof(cl_mem), &cr->outputBuf.buf);
		clSetKernelArg(gk, ga++, sizeof(cl_mem), &cr->depthBuf.buf);
		clSetKernelArg(gk, ga++, sizeof(int), &cr->width);
		clSetKernelArg(gk, ga++, sizeof(int), &cr->height);
		clSetKernelArg(gk, ga++, sizeof(float2), &sunPos);
		clSetKernelArg(gk, ga++, sizeof(float3), &params.godRayColor);
		clSetKernelArg(gk, ga++, sizeof(float), &params.godRayIntensity);
		clSetKernelArg(gk, ga++, sizeof(float), &params.godRayDecay);
		clSetKernelArg(gk, ga++, sizeof(cl_mem), &cr->godRayBuf.buf);
		CL_Dispatch2D(&cr->ctx, &cr->godRayPipeline, (size_t)cr->width, (size_t)cr->height, 8, 8);
	} else {
		float4 zero = {0};
		CL_Buffer_Fill(&cr->ctx, &cr->godRayBuf, &zero, sizeof(float4));
		CL_Finish(&cr->ctx);
	}
}

void CloudRenderer_Composite(CloudRenderer *cr, Camera *cam) {
	size_t fbBytes = (size_t)cr->width * cr->height * sizeof(uint32);

	// Upload CPU framebuffer (written by ray tracer) to pinned GPU buffer
	void *fbPtr = CL_Buffer_Map(&cr->ctx, &cr->framebufferBuf, CL_MAP_WRITE_INVALIDATE_REGION);
	memcpy(fbPtr, cam->framebuffer, fbBytes);
	CL_Buffer_Unmap(&cr->ctx, &cr->framebufferBuf, fbPtr);

	// Dispatch GPU composite: blends cloud + god ray onto the framebuffer in parallel
	CL_SetArgBuffer(&cr->compositePipeline, 0, &cr->outputBuf);
	CL_SetArgBuffer(&cr->compositePipeline, 1, &cr->godRayBuf);
	CL_SetArgBuffer(&cr->compositePipeline, 2, &cr->framebufferBuf);
	CL_SetArgInt(&cr->compositePipeline, 3, cr->width);
	CL_SetArgInt(&cr->compositePipeline, 4, cr->height);
	CL_Dispatch2D(&cr->ctx, &cr->compositePipeline, (size_t)cr->width, (size_t)cr->height, 8, 8);

	// Read blended framebuffer back via pinned map — DMA direct, no staging copy
	fbPtr = CL_Buffer_Map(&cr->ctx, &cr->framebufferBuf, CL_MAP_READ);
	memcpy(cam->framebuffer, fbPtr, fbBytes);
	CL_Buffer_Unmap(&cr->ctx, &cr->framebufferBuf, fbPtr);
}

void CloudRenderer_Destroy(CloudRenderer *cr) {
	CL_Buffer_Destroy(&cr->outputBuf);
	CL_Buffer_Destroy(&cr->depthBuf);
	CL_Buffer_Destroy(&cr->godRayBuf);
	CL_Buffer_Destroy(&cr->framebufferBuf);
	CL_Pipeline_Destroy(&cr->pipeline);
	CL_Pipeline_Destroy(&cr->godRayPipeline);
	CL_Pipeline_Destroy(&cr->compositePipeline);
	CL_Context_Destroy(&cr->ctx);
}
