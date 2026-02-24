#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_INIT_BITS 0x7F7FFFFF // bits of FLT_MAX

void GpuRaster_Init(GpuRaster *r, int width, int height, const char *kernelDir) {
	memset(r, 0, sizeof(*r));
	r->width = width;
	r->height = height;

	r->ctx = CL_Context_Create();
	if (!r->ctx.context) {
		printf("[GpuRaster] failed to create OpenCL context\n");
		return;
	}

	// Build kernel path
	char path[512];
	snprintf(path, sizeof(path), "%s/rester.cl", kernelDir);

	r->rasterPip = CL_Pipeline_FromFile(&r->ctx, path, "renderAll", NULL);
	r->albedoPip = CL_Pipeline_FromFile(&r->ctx, path, "resolveAlbedo", NULL);
	if (!r->rasterPip.kernel || !r->albedoPip.kernel) {
		printf("[GpuRaster] kernel compilation failed\n");
		return;
	}

	int pixels = width * height;
	r->depthBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(int), CL_MEM_READ_WRITE);
	r->normalBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(float) * 4, CL_MEM_READ_WRITE); // float3 padded to float4 (16 bytes) in OpenCL global memory
	r->matObjBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(GpuMatObj), CL_MEM_READ_WRITE);
	r->albedoBuf = CL_Buffer_Create(&r->ctx, pixels * sizeof(uint32_t), CL_MEM_WRITE_ONLY);

	r->gpuOk = 1;
}

void GpuRaster_Clear(GpuRaster *r) {
	if (!r->gpuOk) return;

	r->lastRasterMs = 0.0f;
	r->lastResolveMs = 0.0f;

	int pixels = r->width * r->height;

	// Fill depth with FLT_MAX bits — no host allocation needed
	int depthVal = DEPTH_INIT_BITS;
	clEnqueueFillBuffer(r->ctx.queue, r->depthBuf.buf, &depthVal, sizeof(int),
						0, (size_t)pixels * sizeof(int), 0, NULL, NULL);

	// Clear material/object IDs to -1 (no hit)
	GpuMatObj noHit = {-1, -1};
	clEnqueueFillBuffer(r->ctx.queue, r->matObjBuf.buf, &noHit, sizeof(GpuMatObj),
						0, (size_t)pixels * sizeof(GpuMatObj), 0, NULL, NULL);

	clFinish(r->ctx.queue);
}

void GpuRaster_UploadScene(GpuRaster *r, const Object *objects, int objectCount, const MaterialLib *lib) {
	if (!r->gpuOk) return;

	// Count total triangles across all objects
	int total = 0;
	for (int i = 0; i < objectCount; i++)
		total += objects[i].triangleCount;

	// Destroy any previously allocated scene buffers
	if (r->flatV1Buf.buf) CL_Buffer_Destroy(&r->flatV1Buf);
	if (r->flatV2Buf.buf) CL_Buffer_Destroy(&r->flatV2Buf);
	if (r->flatV3Buf.buf) CL_Buffer_Destroy(&r->flatV3Buf);
	if (r->flatNormalBuf.buf) CL_Buffer_Destroy(&r->flatNormalBuf);
	if (r->flatMatIdBuf.buf) CL_Buffer_Destroy(&r->flatMatIdBuf);
	if (r->triObjIdBuf.buf) CL_Buffer_Destroy(&r->triObjIdBuf);
	if (r->transformBuf.buf) CL_Buffer_Destroy(&r->transformBuf);
	if (r->materialBuf.buf) CL_Buffer_Destroy(&r->materialBuf);

	// Build flat geometry arrays — each float3 is 16 bytes (padded to float4)
	size_t stride = sizeof(float) * 4;
	size_t geomSize = (size_t)total * stride;
	float *flatV1 = malloc(geomSize);
	float *flatV2 = malloc(geomSize);
	float *flatV3 = malloc(geomSize);
	float *flatNormal = malloc(geomSize);
	int *flatMatId = malloc((size_t)total * sizeof(int));
	int *triObjId = malloc((size_t)total * sizeof(int));

	int dst = 0;
	for (int i = 0; i < objectCount; i++) {
		const Object *obj = &objects[i];
		for (int t = 0; t < obj->triangleCount; t++) {
			memcpy(flatV1 + dst * 4, &obj->v1[t], stride);
			memcpy(flatV2 + dst * 4, &obj->v2[t], stride);
			memcpy(flatV3 + dst * 4, &obj->v3[t], stride);
			memcpy(flatNormal + dst * 4, &obj->normals[t], stride);
			flatMatId[dst] = obj->materialIds[t];
			triObjId[dst] = i;
			dst++;
		}
	}

	r->flatV1Buf = CL_Buffer_CreateFromData(&r->ctx, geomSize, flatV1, CL_MEM_READ_ONLY);
	r->flatV2Buf = CL_Buffer_CreateFromData(&r->ctx, geomSize, flatV2, CL_MEM_READ_ONLY);
	r->flatV3Buf = CL_Buffer_CreateFromData(&r->ctx, geomSize, flatV3, CL_MEM_READ_ONLY);
	r->flatNormalBuf = CL_Buffer_CreateFromData(&r->ctx, geomSize, flatNormal, CL_MEM_READ_ONLY);
	r->flatMatIdBuf = CL_Buffer_CreateFromData(&r->ctx, (size_t)total * sizeof(int), flatMatId, CL_MEM_READ_ONLY);
	r->triObjIdBuf = CL_Buffer_CreateFromData(&r->ctx, (size_t)total * sizeof(int), triObjId, CL_MEM_READ_ONLY);
	r->totalTriangles = total;
	r->objectCount = objectCount;
	free(flatV1);
	free(flatV2);
	free(flatV3);
	free(flatNormal);
	free(flatMatId);
	free(triObjId);

	// Pre-allocate per-frame transform buffer: objectCount * 3 float4s
	r->transformBuf = CL_Buffer_Create(&r->ctx, (size_t)objectCount * 3 * sizeof(float) * 4, CL_MEM_READ_ONLY);

	// Upload material palette
	GpuMaterial *gpuMats = malloc(lib->count * sizeof(GpuMaterial));
	for (int i = 0; i < lib->count; i++) {
		const Material *m = &lib->entries[i];
		gpuMats[i].r = m->color.x;
		gpuMats[i].g = m->color.y;
		gpuMats[i].b = m->color.z;
		gpuMats[i]._pad0 = 0.0f;
		gpuMats[i].roughness = m->roughness;
		gpuMats[i].metallic = m->metallic;
		gpuMats[i].emission = m->emission;
		gpuMats[i]._pad1 = 0.0f;
	}
	r->materialBuf = CL_Buffer_CreateFromData(&r->ctx, lib->count * sizeof(GpuMaterial), gpuMats, CL_MEM_READ_ONLY);
	r->matCount = lib->count;
	free(gpuMats);
}

void GpuRaster_RenderAll(GpuRaster *r, const Camera *cam, const Object *objects, int objectCount) {
	if (!r->gpuOk || r->totalTriangles <= 0) return;

	// Pack per-frame transforms: 3 float4s per object [pos, rot, scl]
	float *tr = malloc((size_t)objectCount * 3 * 4 * sizeof(float));
	for (int i = 0; i < objectCount; i++) {
		const Object *obj = &objects[i];
		int base = i * 3 * 4;
		tr[base + 0] = obj->position.x;
		tr[base + 1] = obj->position.y;
		tr[base + 2] = obj->position.z;
		tr[base + 3] = 0.0f;
		tr[base + 4] = obj->rotation.x;
		tr[base + 5] = obj->rotation.y;
		tr[base + 6] = obj->rotation.z;
		tr[base + 7] = 0.0f;
		tr[base + 8] = obj->scale.x;
		tr[base + 9] = obj->scale.y;
		tr[base + 10] = obj->scale.z;
		tr[base + 11] = 0.0f;
	}
	CL_Buffer_Write(&r->ctx, &r->transformBuf, tr, (size_t)objectCount * 3 * 4 * sizeof(float));
	free(tr);

	CL_Pipeline *p = &r->rasterPip;
	int arg = 0;
	CL_SetArgVec3(p, arg++, cam->right.x, cam->right.y, cam->right.z);
	CL_SetArgVec3(p, arg++, cam->up.x, cam->up.y, cam->up.z);
	CL_SetArgVec3(p, arg++, cam->forward.x, cam->forward.y, cam->forward.z);
	CL_SetArgVec3(p, arg++, cam->position.x, cam->position.y, cam->position.z);
	CL_SetArgFloat(p, arg++, cam->fov);
	CL_SetArgInt(p, arg++, cam->screenWidth);
	CL_SetArgInt(p, arg++, cam->screenHeight);
	CL_SetArgBuffer(p, arg++, &r->flatV1Buf);
	CL_SetArgBuffer(p, arg++, &r->flatV2Buf);
	CL_SetArgBuffer(p, arg++, &r->flatV3Buf);
	CL_SetArgBuffer(p, arg++, &r->flatNormalBuf);
	CL_SetArgBuffer(p, arg++, &r->flatMatIdBuf);
	CL_SetArgBuffer(p, arg++, &r->triObjIdBuf);
	CL_SetArgInt(p, arg++, r->totalTriangles);
	CL_SetArgBuffer(p, arg++, &r->transformBuf);
	CL_SetArgBuffer(p, arg++, &r->depthBuf);
	CL_SetArgBuffer(p, arg++, &r->normalBuf);
	CL_SetArgBuffer(p, arg++, &r->matObjBuf);

	size_t local = 64;
	size_t global = (size_t)r->totalTriangles;
	if (global < local) local = global;
	global = ((global + local - 1) / local) * local;

	CL_Dispatch1D(&r->ctx, p, global, local);
	r->lastRasterMs = p->timeTook;
}

void GpuRaster_Resolve(GpuRaster *r) {
	if (!r->gpuOk || !r->materialBuf.buf) return;

	CL_Pipeline *p = &r->albedoPip;
	int arg = 0;
	CL_SetArgInt(p, arg++, r->width);
	CL_SetArgInt(p, arg++, r->height);
	CL_SetArgBuffer(p, arg++, &r->matObjBuf);
	CL_SetArgBuffer(p, arg++, &r->materialBuf);
	CL_SetArgInt(p, arg++, r->matCount);
	CL_SetArgBuffer(p, arg++, &r->albedoBuf);

	CL_Dispatch2D(&r->ctx, p, (size_t)r->width, (size_t)r->height, 16, 16);
	r->lastResolveMs = p->timeTook;
}

void GpuRaster_ReadAlbedo(GpuRaster *r, uint32_t *pixels) {
	if (!r->gpuOk || !pixels) return;
	CL_Buffer_Read(&r->ctx, &r->albedoBuf, pixels, (size_t)r->width * r->height * sizeof(uint32_t));
}

void GpuRaster_ReadBuffers(GpuRaster *r, Camera *cam) {
	if (!r->gpuOk || !cam) return;

	int pixels = r->width * r->height;

	// depth: stored as float bits in an int buffer — reinterpret directly into float depthBuffer
	CL_Buffer_Read(&r->ctx, &r->depthBuf, cam->depthBuffer, (size_t)pixels * sizeof(float));

	// normals: float4 per pixel in GPU, float3 (with w pad) per pixel on CPU — same memory layout
	CL_Buffer_Read(&r->ctx, &r->normalBuf, cam->normalBuffer, (size_t)pixels * sizeof(float3));

	// Reconstruct world-space positions from depth + camera matrices (needed by ShadowPostProcess)
	int w = r->width, h = r->height;
	float fovScale = cam->fovScale;
	float aspect = cam->aspect;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int idx = y * w + x;
			float depth = cam->depthBuffer[idx];
			if (depth >= FLT_MAX || depth <= 0.0f) {
				cam->positionBuffer[idx] = (float3){0, 0, 0, 0};
				cam->reflectBuffer[idx] = (float3){0, 0, 0, 0};
				continue;
			}
			// Unproject: view-space direction scaled by depth gives offset from camera
			float ndcX = ((x + 0.5f) / w * 2.0f - 1.0f) * fovScale * aspect;
			float ndcY = (1.0f - (y + 0.5f) / h * 2.0f) * fovScale;
			float3 pos = {
				cam->position.x + (cam->right.x * ndcX + cam->up.x * ndcY + cam->forward.x) * depth,
				cam->position.y + (cam->right.y * ndcX + cam->up.y * ndcY + cam->forward.y) * depth,
				cam->position.z + (cam->right.z * ndcX + cam->up.z * ndcY + cam->forward.z) * depth,
				0};
			cam->positionBuffer[idx] = pos;
			// Reflect view ray off surface normal for reflections
			float3 n = cam->normalBuffer[idx];
			float3 ray = {cam->right.x * ndcX + cam->up.x * ndcY + cam->forward.x,
						  cam->right.y * ndcX + cam->up.y * ndcY + cam->forward.y,
						  cam->right.z * ndcX + cam->up.z * ndcY + cam->forward.z, 0};
			float k = 2.0f * (n.x * ray.x + n.y * ray.y + n.z * ray.z);
			cam->reflectBuffer[idx] = (float3){ray.x - n.x * k, ray.y - n.y * k, ray.z - n.z * k, 0};
		}
	}
}

void GpuRaster_Destroy(GpuRaster *r) {
	if (!r->gpuOk) return;
	if (r->flatV1Buf.buf) CL_Buffer_Destroy(&r->flatV1Buf);
	if (r->flatV2Buf.buf) CL_Buffer_Destroy(&r->flatV2Buf);
	if (r->flatV3Buf.buf) CL_Buffer_Destroy(&r->flatV3Buf);
	if (r->flatNormalBuf.buf) CL_Buffer_Destroy(&r->flatNormalBuf);
	if (r->flatMatIdBuf.buf) CL_Buffer_Destroy(&r->flatMatIdBuf);
	if (r->triObjIdBuf.buf) CL_Buffer_Destroy(&r->triObjIdBuf);
	if (r->transformBuf.buf) CL_Buffer_Destroy(&r->transformBuf);
	if (r->materialBuf.buf) CL_Buffer_Destroy(&r->materialBuf);
	CL_Buffer_Destroy(&r->depthBuf);
	CL_Buffer_Destroy(&r->normalBuf);
	CL_Buffer_Destroy(&r->matObjBuf);
	CL_Buffer_Destroy(&r->albedoBuf);
	CL_Pipeline_Destroy(&r->rasterPip);
	CL_Pipeline_Destroy(&r->albedoPip);
	CL_Context_Destroy(&r->ctx);
	r->gpuOk = 0;
}
