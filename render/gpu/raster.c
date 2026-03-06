#include "raster.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format.h"

// Kernel file path relative to the working directory (executable location).
#define KERNEL_PATH "render/gpu/kernels/rester.cl"

// Initial "infinity" depth: 0x7F7F7F7F as uint ≈ 2.14e38 as float.
// This matches the value produced by memset(buf, 0x7F, …) on the CPU side.
#define DEPTH_INIT_UINT 0x7F7F7F7Fu

struct GPURaster {
	CL_Context ctx;
	CL_Pipeline pipeline;

	// Static geometry buffers (uploaded once at init)
	CL_Buffer v1, v2, v3;	// triangle vertices, one per triangle
	CL_Buffer normals;		// per-triangle normals
	CL_Buffer matIds;		// per-triangle material ids
	CL_Buffer triObjectIds; // per-triangle object index
	CL_Buffer matColors;	// per-material {r,g,b,roughness}
	CL_Buffer matProps;		// per-material {metallic,emission,0,0}

	// Per-object transform buffers (re-uploaded each frame if objects move)
	CL_Buffer objPositions;
	CL_Buffer objRotations;
	CL_Buffer objScales;

	// Per-frame output buffers (device-side)
	CL_Buffer framebufferGPU;
	CL_Buffer depthBufferInt; // float depth stored as uint for atomic_min
	CL_Buffer normalBufferGPU;
	CL_Buffer positionBufferGPU;
	CL_Buffer reflectBufferGPU;

	int totalTriangles;
	int objectCount;
	int screenWidth;
	int screenHeight;
};

// Count total triangles across all objects.
static int countTriangles(const Object *objects, int objectCount) {
	int total = 0;
	for (int i = 0; i < objectCount; i++)
		total += objects[i].triangleCount;
	return total;
}

GPURaster *GPURaster_Init(const Object *objects, int objectCount,
						  const MaterialLib *lib,
						  int screenWidth, int screenHeight) {
	if (!objects || objectCount <= 0 || !lib) return NULL;

	GPURaster *r = calloc(1, sizeof(GPURaster));
	if (!r) return NULL;

	r->ctx = CL_Context_Create();
	if (!r->ctx.context) {
		fprintf(stderr, "[GPURaster] Failed to create OpenCL context.\n");
		free(r);
		return NULL;
	}

	r->pipeline = CL_Pipeline_FromFile(&r->ctx, KERNEL_PATH, "renderObjects", NULL);
	if (!r->pipeline.kernel) {
		fprintf(stderr, "[GPURaster] Failed to compile kernel.\n");
		CL_Context_Destroy(&r->ctx);
		free(r);
		return NULL;
	}

	int total = countTriangles(objects, objectCount);
	r->totalTriangles = total;
	r->objectCount = objectCount;
	r->screenWidth = screenWidth;
	r->screenHeight = screenHeight;

	// Build flat triangle arrays and per-triangle object-id arrays.
	float3 *fv1 = malloc(total * sizeof(float3));
	float3 *fv2 = malloc(total * sizeof(float3));
	float3 *fv3 = malloc(total * sizeof(float3));
	float3 *fnorm = malloc(total * sizeof(float3));
	int *fmat = malloc(total * sizeof(int));
	int *fobj = malloc(total * sizeof(int));
	if (!fv1 || !fv2 || !fv3 || !fnorm || !fmat || !fobj) {
		fprintf(stderr, "[GPURaster] Out of memory building geometry arrays.\n");
		free(fv1);
		free(fv2);
		free(fv3);
		free(fnorm);
		free(fmat);
		free(fobj);
		CL_Pipeline_Destroy(&r->pipeline);
		CL_Context_Destroy(&r->ctx);
		free(r);
		return NULL;
	}

	int idx = 0;
	for (int i = 0; i < objectCount; i++) {
		for (int t = 0; t < objects[i].triangleCount; t++, idx++) {
			fv1[idx] = objects[i].v1[t];
			fv2[idx] = objects[i].v2[t];
			fv3[idx] = objects[i].v3[t];
			fnorm[idx] = objects[i].normals[t];
			fmat[idx] = objects[i].materialIds ? objects[i].materialIds[t] : 0;
			fobj[idx] = i;
		}
	}

	// Upload static geometry to GPU
	r->v1 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float3), fv1, CL_MEM_READ_ONLY);
	r->v2 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float3), fv2, CL_MEM_READ_ONLY);
	r->v3 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float3), fv3, CL_MEM_READ_ONLY);
	r->normals = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float3), fnorm, CL_MEM_READ_ONLY);
	r->matIds = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(int), fmat, CL_MEM_READ_ONLY);
	r->triObjectIds = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(int), fobj, CL_MEM_READ_ONLY);
	free(fv1);
	free(fv2);
	free(fv3);
	free(fnorm);
	free(fmat);
	free(fobj);

	// Pack material data into two float4 arrays
	int matCount = lib->count;
	float4 *matCol = malloc(matCount * sizeof(float4));
	float4 *matPrp = malloc(matCount * sizeof(float4));
	if (!matCol || !matPrp) {
		fprintf(stderr, "[GPURaster] Out of memory for materials.\n");
		free(matCol);
		free(matPrp);
		CL_Pipeline_Destroy(&r->pipeline);
		CL_Context_Destroy(&r->ctx);
		free(r);
		return NULL;
	}
	for (int i = 0; i < matCount; i++) {
		matCol[i] = (float4){lib->entries[i].color.x,
							 lib->entries[i].color.y,
							 lib->entries[i].color.z,
							 lib->entries[i].roughness};
		matPrp[i] = (float4){lib->entries[i].metallic,
							 lib->entries[i].emission, 0.0f, 0.0f};
	}
	r->matColors = CL_Buffer_CreateFromData(&r->ctx, matCount * sizeof(float4), matCol, CL_MEM_READ_ONLY);
	r->matProps = CL_Buffer_CreateFromData(&r->ctx, matCount * sizeof(float4), matPrp, CL_MEM_READ_ONLY);
	free(matCol);
	free(matPrp);

	// Per-object transform buffers (uploaded once; re-uploaded each frame via RenderObjects)
	r->objPositions = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float3), CL_MEM_READ_ONLY);
	r->objRotations = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float3), CL_MEM_READ_ONLY);
	r->objScales = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float3), CL_MEM_READ_ONLY);

	// Output buffers
	size_t pixelCount = (size_t)screenWidth * screenHeight;
	r->framebufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(uint32_t), CL_MEM_WRITE_ONLY);
	r->depthBufferInt = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(uint32_t), CL_MEM_READ_WRITE);
	r->normalBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4), CL_MEM_WRITE_ONLY);
	r->positionBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4), CL_MEM_WRITE_ONLY);
	r->reflectBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4), CL_MEM_WRITE_ONLY);

	printf("[GPURaster] Initialized: %d objects, %d triangles, %dx%d\n",
		   objectCount, total, screenWidth, screenHeight);
	return r;
}

void GPURaster_RenderObjects(GPURaster *raster,
							 const Object *objects, int objectCount,
							 Camera *camera) {
	if (!raster || !objects || !camera) return;

	// Upload per-object transforms (may change each frame)
	float3 *pos = malloc(objectCount * sizeof(float3));
	float3 *rot = malloc(objectCount * sizeof(float3));
	float3 *sca = malloc(objectCount * sizeof(float3));
	if (!pos || !rot || !sca) {
		free(pos);
		free(rot);
		free(sca);
		return;
	}
	for (int i = 0; i < objectCount; i++) {
		pos[i] = objects[i].position;
		rot[i] = objects[i].rotation;
		sca[i] = objects[i].scale;
	}
	CL_Buffer_Write(&raster->ctx, &raster->objPositions, pos, objectCount * sizeof(float3));
	CL_Buffer_Write(&raster->ctx, &raster->objRotations, rot, objectCount * sizeof(float3));
	CL_Buffer_Write(&raster->ctx, &raster->objScales, sca, objectCount * sizeof(float3));
	free(pos);
	free(rot);
	free(sca);

	size_t pixelCount = (size_t)raster->screenWidth * raster->screenHeight;

	// Clear depth buffer: 0x7F7F7F7F ≈ 2.14e38 — any real depth is smaller.
	uint32_t depthInit = DEPTH_INIT_UINT;
	CL_Buffer_Fill(&raster->ctx, &raster->depthBufferInt,
				   &depthInit, sizeof(uint32_t),
				   pixelCount * sizeof(uint32_t));

	// Clear framebuffer to a sky-blue background
	uint32_t skyColor = 0xFF87CEEBu;
	CL_Buffer_Fill(&raster->ctx, &raster->framebufferGPU,
				   &skyColor, sizeof(uint32_t),
				   pixelCount * sizeof(uint32_t));

	// Pack camera vectors into float4 (C float3 = struct with w=0, matching OpenCL float4)
	float4 camRight = {camera->right.x, camera->right.y, camera->right.z, 0.0f};
	float4 camUp = {camera->up.x, camera->up.y, camera->up.z, 0.0f};
	float4 camForward = {camera->forward.x, camera->forward.y, camera->forward.z, 0.0f};
	float4 camPos = {camera->position.x, camera->position.y, camera->position.z, 0.0f};
	float4 lightDir = {camera->renderLightDir.x, camera->renderLightDir.y, camera->renderLightDir.z, 0.0f};
	float4 halfVec = {camera->halfVec.x, camera->halfVec.y, camera->halfVec.z, 0.0f};

	// Set all kernel arguments
	cl_kernel k = raster->pipeline.kernel;
	int arg = 0;
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->v1.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->v2.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->v3.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->normals.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->matIds.buf);
	clSetKernelArg(k, arg++, sizeof(int), &raster->totalTriangles);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->triObjectIds.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->objPositions.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->objRotations.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->objScales.buf);
	clSetKernelArg(k, arg++, sizeof(float4), &camRight);
	clSetKernelArg(k, arg++, sizeof(float4), &camUp);
	clSetKernelArg(k, arg++, sizeof(float4), &camForward);
	clSetKernelArg(k, arg++, sizeof(float4), &camPos);
	clSetKernelArg(k, arg++, sizeof(float4), &lightDir);
	clSetKernelArg(k, arg++, sizeof(float4), &halfVec);
	clSetKernelArg(k, arg++, sizeof(float), &camera->fovScale);
	clSetKernelArg(k, arg++, sizeof(float), &camera->aspect);
	clSetKernelArg(k, arg++, sizeof(float), &camera->jitter.x);
	clSetKernelArg(k, arg++, sizeof(float), &camera->jitter.y);
	clSetKernelArg(k, arg++, sizeof(int), &camera->screenWidth);
	clSetKernelArg(k, arg++, sizeof(int), &camera->screenHeight);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->matColors.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->matProps.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->framebufferGPU.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->depthBufferInt.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->normalBufferGPU.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->positionBufferGPU.buf);
	clSetKernelArg(k, arg++, sizeof(cl_mem), &raster->reflectBufferGPU.buf);

	// Dispatch: one work item per triangle, group size 64
	size_t global = ((size_t)raster->totalTriangles + 63) & ~(size_t)63;
	CL_Dispatch1D(&raster->ctx, &raster->pipeline, global, 64);

	printf("[GPURaster] kernel time: %.2f ms\n", raster->pipeline.timeTook);

	// Read results back to camera CPU buffers
	CL_Buffer_Read(&raster->ctx, &raster->framebufferGPU,
				   camera->framebuffer, pixelCount * sizeof(uint32_t));

	// Depth buffer: the GPU stores it as uint (bit-identical to float for positive values)
	CL_Buffer_Read(&raster->ctx, &raster->depthBufferInt,
				   camera->depthBuffer, pixelCount * sizeof(float));

	// C float3 = 16 bytes (struct with w=0), OpenCL float4 = 16 bytes — same layout
	CL_Buffer_Read(&raster->ctx, &raster->normalBufferGPU,
				   camera->normalBuffer, pixelCount * sizeof(float3));
	CL_Buffer_Read(&raster->ctx, &raster->positionBufferGPU,
				   camera->positionBuffer, pixelCount * sizeof(float3));
	CL_Buffer_Read(&raster->ctx, &raster->reflectBufferGPU,
				   camera->reflectBuffer, pixelCount * sizeof(float3));
}

void GPURaster_Destroy(GPURaster *raster) {
	if (!raster) return;
	CL_Buffer_Destroy(&raster->v1);
	CL_Buffer_Destroy(&raster->v2);
	CL_Buffer_Destroy(&raster->v3);
	CL_Buffer_Destroy(&raster->normals);
	CL_Buffer_Destroy(&raster->matIds);
	CL_Buffer_Destroy(&raster->triObjectIds);
	CL_Buffer_Destroy(&raster->matColors);
	CL_Buffer_Destroy(&raster->matProps);
	CL_Buffer_Destroy(&raster->objPositions);
	CL_Buffer_Destroy(&raster->objRotations);
	CL_Buffer_Destroy(&raster->objScales);
	CL_Buffer_Destroy(&raster->framebufferGPU);
	CL_Buffer_Destroy(&raster->depthBufferInt);
	CL_Buffer_Destroy(&raster->normalBufferGPU);
	CL_Buffer_Destroy(&raster->positionBufferGPU);
	CL_Buffer_Destroy(&raster->reflectBufferGPU);
	CL_Pipeline_Destroy(&raster->pipeline);
	CL_Context_Destroy(&raster->ctx);
	free(raster);
}
