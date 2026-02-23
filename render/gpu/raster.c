#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_INIT_BITS 0x7F7FFFFF // bits of FLT_MAX

void GpuRaster_Init(GpuRaster *r, int width, int height, const char *kernelDir) {
	memset(r, 0, sizeof(*r));
	r->width  = width;
	r->height = height;

	r->ctx = CL_Context_Create();
	if (!r->ctx.context) {
		printf("[GpuRaster] failed to create OpenCL context\n");
		return;
	}

	// Build kernel path
	char path[512];
	snprintf(path, sizeof(path), "%s/rester.cl", kernelDir);

	r->rasterPip = CL_Pipeline_FromFile(&r->ctx, path, "renderObject", NULL);
	r->albedoPip = CL_Pipeline_FromFile(&r->ctx, path, "resolveAlbedo", NULL);
	if (!r->rasterPip.kernel || !r->albedoPip.kernel) {
		printf("[GpuRaster] kernel compilation failed\n");
		return;
	}

	int pixels = width * height;
	r->depthBuf  = CL_Buffer_Create(&r->ctx, pixels * sizeof(int),     CL_MEM_READ_WRITE);
	r->normalBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(float) * 4, CL_MEM_READ_WRITE); // float3 padded to float4 (16 bytes) in OpenCL global memory
	r->matObjBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(GpuMatObj), CL_MEM_READ_WRITE);
	r->albedoBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(uint32_t),  CL_MEM_WRITE_ONLY);

	r->gpuOk = 1;
}

void GpuRaster_Clear(GpuRaster *r) {
	if (!r->gpuOk) return;

	int pixels = r->width * r->height;

	// Fill depth with FLT_MAX bits
	int *depthInit = malloc(pixels * sizeof(int));
	if (depthInit) {
		for (int i = 0; i < pixels; i++) depthInit[i] = DEPTH_INIT_BITS;
		CL_Buffer_Write(&r->ctx, &r->depthBuf, depthInit, pixels * sizeof(int));
		free(depthInit);
	}

	// Clear material/object IDs to -1 (no hit)
	GpuMatObj *matInit = malloc(pixels * sizeof(GpuMatObj));
	if (matInit) {
		for (int i = 0; i < pixels; i++) matInit[i] = (GpuMatObj){-1, -1};
		CL_Buffer_Write(&r->ctx, &r->matObjBuf, matInit, pixels * sizeof(GpuMatObj));
		free(matInit);
	}
}

void GpuRaster_RenderObject(GpuRaster *r, const Object *obj, const Camera *cam, int objectId) {
	if (!r->gpuOk || !obj || !obj->v1 || obj->triangleCount <= 0) return;

	// Upload per-object vertex and normal data
	size_t triBytes = obj->triangleCount * sizeof(float) * 4; // float3 = 16 bytes per element
	CL_Buffer v1Buf = CL_Buffer_CreateFromData(&r->ctx, triBytes, obj->v1, CL_MEM_READ_ONLY);
	CL_Buffer v2Buf = CL_Buffer_CreateFromData(&r->ctx, triBytes, obj->v2, CL_MEM_READ_ONLY);
	CL_Buffer v3Buf = CL_Buffer_CreateFromData(&r->ctx, triBytes, obj->v3, CL_MEM_READ_ONLY);
	CL_Buffer nBuf  = CL_Buffer_CreateFromData(&r->ctx, triBytes, obj->normals, CL_MEM_READ_ONLY);
	CL_Buffer mBuf  = CL_Buffer_CreateFromData(&r->ctx, obj->triangleCount * sizeof(int), obj->materialIds, CL_MEM_READ_ONLY);

	CL_Pipeline *p = &r->rasterPip;
	int arg = 0;
	CL_SetArgInt   (p, arg++, objectId);
	CL_SetArgVec3  (p, arg++, cam->right.x,    cam->right.y,    cam->right.z);
	CL_SetArgVec3  (p, arg++, cam->up.x,       cam->up.y,       cam->up.z);
	CL_SetArgVec3  (p, arg++, cam->forward.x,  cam->forward.y,  cam->forward.z);
	CL_SetArgVec3  (p, arg++, cam->position.x, cam->position.y, cam->position.z);
	CL_SetArgFloat (p, arg++, cam->fov);
	CL_SetArgInt   (p, arg++, cam->screenWidth);
	CL_SetArgInt   (p, arg++, cam->screenHeight);
	CL_SetArgVec3  (p, arg++, obj->position.x, obj->position.y, obj->position.z);
	CL_SetArgVec3  (p, arg++, obj->rotation.x, obj->rotation.y, obj->rotation.z);
	CL_SetArgVec3  (p, arg++, obj->scale.x,    obj->scale.y,    obj->scale.z);
	CL_SetArgBuffer(p, arg++, &v1Buf);
	CL_SetArgBuffer(p, arg++, &v2Buf);
	CL_SetArgBuffer(p, arg++, &v3Buf);
	CL_SetArgBuffer(p, arg++, &nBuf);
	CL_SetArgBuffer(p, arg++, &mBuf);
	CL_SetArgInt   (p, arg++, obj->triangleCount);
	CL_SetArgBuffer(p, arg++, &r->depthBuf);
	CL_SetArgBuffer(p, arg++, &r->normalBuf);
	CL_SetArgBuffer(p, arg++, &r->matObjBuf);

	// One work item per triangle
	size_t global = (size_t)obj->triangleCount;
	size_t local  = 64;
	if (global < local) local = global;
	// Round up to multiple of local
	global = ((global + local - 1) / local) * local;

	CL_Dispatch1D(&r->ctx, p, global, local);

	CL_Buffer_Destroy(&v1Buf);
	CL_Buffer_Destroy(&v2Buf);
	CL_Buffer_Destroy(&v3Buf);
	CL_Buffer_Destroy(&nBuf);
	CL_Buffer_Destroy(&mBuf);
}

void GpuRaster_Resolve(GpuRaster *r, const MaterialLib *lib) {
	if (!r->gpuOk) return;

	// Pack materials into GPU layout
	GpuMaterial *gpuMats = malloc(lib->count * sizeof(GpuMaterial));
	if (!gpuMats) return;
	for (int i = 0; i < lib->count; i++) {
		const Material *m = &lib->entries[i];
		gpuMats[i].r         = m->color.x;
		gpuMats[i].g         = m->color.y;
		gpuMats[i].b         = m->color.z;
		gpuMats[i]._pad0     = 0.0f;
		gpuMats[i].roughness = m->roughness;
		gpuMats[i].metallic  = m->metallic;
		gpuMats[i].emission  = m->emission;
		gpuMats[i]._pad1     = 0.0f;
	}

	CL_Buffer matBuf = CL_Buffer_CreateFromData(&r->ctx, lib->count * sizeof(GpuMaterial), gpuMats, CL_MEM_READ_ONLY);
	free(gpuMats);

	CL_Pipeline *p = &r->albedoPip;
	int arg = 0;
	CL_SetArgInt   (p, arg++, r->width);
	CL_SetArgInt   (p, arg++, r->height);
	CL_SetArgBuffer(p, arg++, &r->matObjBuf);
	CL_SetArgBuffer(p, arg++, &matBuf);
	CL_SetArgInt   (p, arg++, lib->count);
	CL_SetArgBuffer(p, arg++, &r->albedoBuf);

	CL_Dispatch2D(&r->ctx, p, (size_t)r->width, (size_t)r->height, 16, 16);

	CL_Buffer_Destroy(&matBuf);
}

void GpuRaster_ReadAlbedo(GpuRaster *r, uint32_t *pixels) {
	if (!r->gpuOk || !pixels) return;
	CL_Buffer_Read(&r->ctx, &r->albedoBuf, pixels, (size_t)r->width * r->height * sizeof(uint32_t));
}

void GpuRaster_Destroy(GpuRaster *r) {
	if (!r->gpuOk) return;
	CL_Buffer_Destroy(&r->depthBuf);
	CL_Buffer_Destroy(&r->normalBuf);
	CL_Buffer_Destroy(&r->matObjBuf);
	CL_Buffer_Destroy(&r->albedoBuf);
	CL_Pipeline_Destroy(&r->rasterPip);
	CL_Pipeline_Destroy(&r->albedoPip);
	CL_Context_Destroy(&r->ctx);
	r->gpuOk = 0;
}
