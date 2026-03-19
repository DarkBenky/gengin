#include "cload.h"
#include <math.h>
#include <stdlib.h>

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

	size_t outBytes = (size_t)width * height * sizeof(float3);
	cr->outputBuf = CL_Buffer_Create(&cr->ctx, outBytes, CL_MEM_READ_WRITE);
	cr->outputCpu = malloc(outBytes);
	cr->depthBuf = CL_Buffer_Create(&cr->ctx, (size_t)width * height * sizeof(float), CL_MEM_READ_ONLY);
}

void CloudRenderer_Render(CloudRenderer *cr, Volume *vol, const Camera *cam, CloudParams params) {
	updateVolumeCache(vol);

	// upload scene depth so the kernel can occlude cloud samples behind geometry
	CL_Buffer_Write(&cr->ctx, &cr->depthBuf, cam->depthBuffer, (size_t)cr->width * cr->height * sizeof(float));

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
	CL_Buffer_Read(&cr->ctx, &cr->outputBuf, cr->outputCpu, (size_t)cr->width * cr->height * sizeof(float3));
}

void CloudRenderer_Composite(const CloudRenderer *cr, Camera *cam) {
	int n = cr->width * cr->height;
	for (int i = 0; i < n; i++) {
		float3 cloud = cr->outputCpu[i];
		float transmittance = cloud.w;
		if (transmittance > 0.998f) continue; // fully transparent, skip

		float oa = transmittance;

		Color bg = cam->framebuffer[i];
		float br = ((bg >> 16) & 0xFF) * (1.0f / 255.0f);
		float bgi = ((bg >> 8) & 0xFF) * (1.0f / 255.0f);
		float bb = (bg & 0xFF) * (1.0f / 255.0f);

		// standard volume render equation: final = background * transmittance + scattered
		float fr = br * oa + cloud.x;
		if (fr > 1.0f) fr = 1.0f;
		float fg = bgi * oa + cloud.y;
		if (fg > 1.0f) fg = 1.0f;
		float fb = bb * oa + cloud.z;
		if (fb > 1.0f) fb = 1.0f;

		cam->framebuffer[i] = 0xFF000000u | ((uint8)(fr * 255.0f) << 16) | ((uint8)(fg * 255.0f) << 8) | (uint8)(fb * 255.0f);
	}
}

void CloudRenderer_Destroy(CloudRenderer *cr) {
	CL_Buffer_Destroy(&cr->outputBuf);
	CL_Buffer_Destroy(&cr->depthBuf);
	CL_Pipeline_Destroy(&cr->pipeline);
	CL_Context_Destroy(&cr->ctx);
	free(cr->outputCpu);
	cr->outputCpu = NULL;
}
