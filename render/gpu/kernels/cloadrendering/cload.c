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

	cr->outputBuf = CL_BUFFER_NEW(&cr->ctx, float3, (size_t)width * height,
		CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR);
	cr->depthBuf = CL_BUFFER_NEW(&cr->ctx, float, (size_t)width * height, CL_MEM_READ_ONLY);
	cr->godRayBuf = CL_BUFFER_NEW(&cr->ctx, float4, (size_t)width * height,
		CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR);

	cr->outputCpu = (float3 *)CL_Buffer_Map(&cr->ctx, &cr->outputBuf, CL_MAP_READ);
	cr->godRayCpu = (float4 *)CL_Buffer_Map(&cr->ctx, &cr->godRayBuf, CL_MAP_READ | CL_MAP_WRITE);
}

void CloudRenderer_Render(CloudRenderer *cr, Volume *vol, const Camera *cam, CloudParams params) {
	updateVolumeCache(vol);

	// upload scene depth so the kernel can occlude cloud samples behind geometry
	CL_Buffer_Write(&cr->ctx, &cr->depthBuf, cam->depthBuffer, (size_t)cr->width * cr->height * sizeof(float));

	int samplesPerPixel = 1;
	CL_ARGS_BEGIN(&cr->pipeline)
		CL_ARG_RAW(sizeof(float3), &vol->position)
		CL_ARG_RAW(sizeof(float3), &vol->rotation)
		CL_ARG_RAW(sizeof(float3), &vol->scale)
		CL_ARG_RAW(sizeof(float3), &vol->_invScale)
		CL_ARG_RAW(sizeof(float3), &vol->_invRotSin)
		CL_ARG_RAW(sizeof(float3), &vol->_invRotCos)
		CL_ARG_RAW(sizeof(float3), &vol->_fwdRot0)
		CL_ARG_RAW(sizeof(float3), &vol->_fwdRot1)
		CL_ARG_RAW(sizeof(float3), &vol->_fwdRot2)
		CL_ARG_BUFFER(&vol->gpuDensity)
		CL_ARG_RAW(sizeof(float3), &cam->position)
		CL_ARG_RAW(sizeof(float3), &cam->forward)
		CL_ARG_RAW(sizeof(float3), &cam->up)
		CL_ARG_RAW(sizeof(float3), &cam->right)
		CL_ARG_FLOAT(cam->fovScale)
		CL_ARG_INT(cam->screenWidth)
		CL_ARG_INT(cam->screenHeight)
		CL_ARG_RAW(sizeof(float3), &cam->lightDir)
		CL_ARG_RAW(sizeof(float3), &params.baseColor)
		CL_ARG_FLOAT(params.extinctionScale)
		CL_ARG_FLOAT(params.shadowExtinction)
		CL_ARG_FLOAT(params.scatterG)
		CL_ARG_FLOAT(params.shadowDist)
		CL_ARG_FLOAT(params.ambientLight)
		CL_ARG_BUFFER(&cr->outputBuf)
		CL_ARG_INT(samplesPerPixel)
		CL_ARG_BUFFER(&cr->depthBuf)
	CL_ARGS_END
	CL_Dispatch2D_Auto(&cr->ctx, &cr->pipeline, (size_t)cr->width, (size_t)cr->height);

	if (params.godRays) {
		// project lightDir to screen space to find the sun position
		float3 ld = cam->lightDir;
		float ll = sqrtf(ld.x * ld.x + ld.y * ld.y + ld.z * ld.z);
		float3 ls = {ld.x / ll, ld.y / ll, ld.z / ll, 0.0f};
		float sdx = (ls.x * cam->right.x + ls.y * cam->right.y + ls.z * cam->right.z) / (cam->aspect * cam->fovScale);
		float sdy = (ls.x * cam->up.x + ls.y * cam->up.y + ls.z * cam->up.z) / cam->fovScale;
		float2 sunPos = {(sdx + 1.0f) * 0.5f, (1.0f - sdy) * 0.5f};

		CL_ARGS_BEGIN(&cr->godRayPipeline)
			CL_ARG_BUFFER(&cr->outputBuf)
			CL_ARG_BUFFER(&cr->depthBuf)
			CL_ARG_INT(cr->width)
			CL_ARG_INT(cr->height)
			CL_ARG_RAW(sizeof(float2), &sunPos)
			CL_ARG_RAW(sizeof(float3), &params.godRayColor)
			CL_ARG_FLOAT(params.godRayIntensity)
			CL_ARG_FLOAT(params.godRayDecay)
			CL_ARG_BUFFER(&cr->godRayBuf)
		CL_ARGS_END
		CL_Dispatch2D_Auto(&cr->ctx, &cr->godRayPipeline, (size_t)cr->width, (size_t)cr->height);
	} else {
		memset(cr->godRayCpu, 0, (size_t)cr->width * cr->height * sizeof(float4));
	}
}

void CloudRenderer_Composite(const CloudRenderer *cr, Camera *cam) {
	int n = cr->width * cr->height;
	for (int i = 0; i < n; i++) {
		float3 cloud = cr->outputCpu[i];
		float transmittance = cloud.w;
		float4 gr = cr->godRayCpu[i];

		// skip pixel if fully transparent and no god ray contribution
		if (transmittance > 0.998f && gr.x < 0.001f && gr.y < 0.001f && gr.z < 0.001f) continue;

		Color bg = cam->framebuffer[i];
		float br = ((bg >> 16) & 0xFF) * (1.0f / 255.0f);
		float bgi = ((bg >> 8) & 0xFF) * (1.0f / 255.0f);
		float bb = (bg & 0xFF) * (1.0f / 255.0f);

		float fr = br, fg = bgi, fb = bb;

		if (transmittance < 0.998f) {
			fr = br * transmittance + cloud.x;
			fg = bgi * transmittance + cloud.y;
			fb = bb * transmittance + cloud.z;
		}

		// god rays are additive on all pixels so shafts appear in clear air too
		fr += gr.x;
		fg += gr.y;
		fb += gr.z;

		if (fr > 1.0f) fr = 1.0f;
		if (fg > 1.0f) fg = 1.0f;
		if (fb > 1.0f) fb = 1.0f;

		cam->framebuffer[i] = 0xFF000000u | ((uint8)(fr * 255.0f) << 16) | ((uint8)(fg * 255.0f) << 8) | (uint8)(fb * 255.0f);
	}
}

void CloudRenderer_Destroy(CloudRenderer *cr) {
	CL_Buffer_Unmap(&cr->ctx, &cr->outputBuf, cr->outputCpu);
	CL_Buffer_Unmap(&cr->ctx, &cr->godRayBuf, cr->godRayCpu);
	cr->outputCpu = NULL;
	cr->godRayCpu = NULL;
	CL_Buffer_Destroy(&cr->outputBuf);
	CL_Buffer_Destroy(&cr->depthBuf);
	CL_Buffer_Destroy(&cr->godRayBuf);
	CL_Pipeline_Destroy(&cr->pipeline);
	CL_Pipeline_Destroy(&cr->godRayPipeline);
	CL_Context_Destroy(&cr->ctx);
}
