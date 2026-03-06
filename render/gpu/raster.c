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

	// Static geometry buffers (uploaded once at init / on Reload)
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

	// Pre-allocated host-side staging arrays — reused every frame, no per-frame malloc.
	float3 *hostPos;
	float3 *hostRot;
	float3 *hostSca;

	// Per-frame output buffers (device-side, pinned for fast DMA readback)
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

// Destroy geometry and material CL buffers (safe to call when buf == NULL).
static void destroyGeometry(GPURaster *r) {
	if (!r->v1.buf) return;
	CL_Buffer_Destroy(&r->v1);
	CL_Buffer_Destroy(&r->v2);
	CL_Buffer_Destroy(&r->v3);
	CL_Buffer_Destroy(&r->normals);
	CL_Buffer_Destroy(&r->matIds);
	CL_Buffer_Destroy(&r->triObjectIds);
	CL_Buffer_Destroy(&r->matColors);
	CL_Buffer_Destroy(&r->matProps);
	r->v1.buf = NULL;
}

// Upload flat geometry + material data to GPU.  Any existing buffers must
// already have been released via destroyGeometry() before calling this.
static bool uploadGeometry(GPURaster *r, const Object *objects, int objectCount,
						   const MaterialLib *lib) {
	int total = countTriangles(objects, objectCount);
	r->totalTriangles = total;

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
		return false;
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

	// This project's float3 is {x,y,z,w} = 16 bytes (same layout as float4); kernel declares float4.
	r->v1 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float4), fv1, CL_MEM_READ_ONLY);
	r->v2 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float4), fv2, CL_MEM_READ_ONLY);
	r->v3 = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float4), fv3, CL_MEM_READ_ONLY);
	r->normals = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(float4), fnorm, CL_MEM_READ_ONLY);
	r->matIds = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(int), fmat, CL_MEM_READ_ONLY);
	r->triObjectIds = CL_Buffer_CreateFromData(&r->ctx, total * sizeof(int), fobj, CL_MEM_READ_ONLY);
	free(fv1);
	free(fv2);
	free(fv3);
	free(fnorm);
	free(fmat);
	free(fobj);

	int matCount = lib->count;
	float4 *matCol = malloc(matCount * sizeof(float4));
	float4 *matPrp = malloc(matCount * sizeof(float4));
	if (!matCol || !matPrp) {
		fprintf(stderr, "[GPURaster] Out of memory for materials.\n");
		free(matCol);
		free(matPrp);
		destroyGeometry(r);
		return false;
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

	return true;
}

// (Re-)allocate per-object transform GPU buffers and host staging arrays.
// Destroys the old buffers / frees the old arrays first if they exist.
static bool allocTransformBuffers(GPURaster *r, int objectCount) {
	if (r->objPositions.buf) {
		CL_Buffer_Destroy(&r->objPositions);
		CL_Buffer_Destroy(&r->objRotations);
		CL_Buffer_Destroy(&r->objScales);
		free(r->hostPos);
		free(r->hostRot);
		free(r->hostSca);
		r->objPositions.buf = NULL;
	}

	r->hostPos = malloc(objectCount * sizeof(float3));
	r->hostRot = malloc(objectCount * sizeof(float3));
	r->hostSca = malloc(objectCount * sizeof(float3));
	if (!r->hostPos || !r->hostRot || !r->hostSca) {
		fprintf(stderr, "[GPURaster] Out of memory for host-side transform staging arrays.\n");
		free(r->hostPos);
		free(r->hostRot);
		free(r->hostSca);
		r->hostPos = r->hostRot = r->hostSca = NULL;
		return false;
	}

	// This project's float3 is {x,y,z,w} = 16 bytes; kernel declares float4 for transform arrays.
	r->objPositions = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float4), CL_MEM_READ_ONLY);
	r->objRotations = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float4), CL_MEM_READ_ONLY);
	r->objScales = CL_Buffer_Create(&r->ctx, objectCount * sizeof(float4), CL_MEM_READ_ONLY);
	r->objectCount = objectCount;
	return true;
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

	r->screenWidth = screenWidth;
	r->screenHeight = screenHeight;

	if (!uploadGeometry(r, objects, objectCount, lib)) {
		CL_Pipeline_Destroy(&r->pipeline);
		CL_Context_Destroy(&r->ctx);
		free(r);
		return NULL;
	}

	if (!allocTransformBuffers(r, objectCount)) {
		destroyGeometry(r);
		CL_Pipeline_Destroy(&r->pipeline);
		CL_Context_Destroy(&r->ctx);
		free(r);
		return NULL;
	}

	// Output buffers use CL_MEM_ALLOC_HOST_PTR so the runtime allocates
	// pinned memory, enabling direct DMA transfers instead of the extra
	// intermediate copy that pageable buffers require.
	size_t pixelCount = (size_t)screenWidth * screenHeight;
	r->framebufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(uint32_t),
										 CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);
	r->depthBufferInt = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(uint32_t),
										 CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR);
	r->normalBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4),
										  CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);
	r->positionBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4),
											CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);
	r->reflectBufferGPU = CL_Buffer_Create(&r->ctx, pixelCount * sizeof(float4),
										   CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);

	printf("[GPURaster] Initialized: %d objects, %d triangles, %dx%d\n",
		   objectCount, r->totalTriangles, screenWidth, screenHeight);
	return r;
}

void GPURaster_RenderObjects(GPURaster *raster,
							 const Object *objects, int objectCount,
							 Camera *camera) {
	if (!raster || !objects || !camera) return;

	// Copy per-object transforms into the pre-allocated staging arrays — no
	// malloc/free here, the buffers were allocated once in GPURaster_Init.
	for (int i = 0; i < objectCount; i++) {
		raster->hostPos[i] = objects[i].position;
		raster->hostRot[i] = objects[i].rotation;
		raster->hostSca[i] = objects[i].scale;
	}
	CL_Buffer_Write(&raster->ctx, &raster->objPositions, raster->hostPos, objectCount * sizeof(float4));
	CL_Buffer_Write(&raster->ctx, &raster->objRotations, raster->hostRot, objectCount * sizeof(float4));
	CL_Buffer_Write(&raster->ctx, &raster->objScales, raster->hostSca, objectCount * sizeof(float4));

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

	// This project's float3 is {x,y,z,w} = 16 bytes, matching OpenCL float4.  Pass address directly.
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
	clSetKernelArg(k, arg++, sizeof(float4), &camera->right);
	clSetKernelArg(k, arg++, sizeof(float4), &camera->up);
	clSetKernelArg(k, arg++, sizeof(float4), &camera->forward);
	clSetKernelArg(k, arg++, sizeof(float4), &camera->position);
	clSetKernelArg(k, arg++, sizeof(float4), &camera->renderLightDir);
	clSetKernelArg(k, arg++, sizeof(float4), &camera->halfVec);
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

	// Read results back to camera CPU buffers using pinned map — avoids the
	// internal intermediate copy that clEnqueueReadBuffer uses for pageable memory.
	void *ptr;
	ptr = CL_Buffer_Map(&raster->ctx, &raster->framebufferGPU, CL_MAP_READ);
	memcpy(camera->framebuffer, ptr, pixelCount * sizeof(uint32_t));
	CL_Buffer_Unmap(&raster->ctx, &raster->framebufferGPU, ptr);

	ptr = CL_Buffer_Map(&raster->ctx, &raster->depthBufferInt, CL_MAP_READ);
	memcpy(camera->depthBuffer, ptr, pixelCount * sizeof(float));
	CL_Buffer_Unmap(&raster->ctx, &raster->depthBufferInt, ptr);

	// This project's float3 is {x,y,z,w} = 16 bytes; GPU wrote float4 — sizes match.
	ptr = CL_Buffer_Map(&raster->ctx, &raster->normalBufferGPU, CL_MAP_READ);
	memcpy(camera->normalBuffer, ptr, pixelCount * sizeof(float4));
	CL_Buffer_Unmap(&raster->ctx, &raster->normalBufferGPU, ptr);

	ptr = CL_Buffer_Map(&raster->ctx, &raster->positionBufferGPU, CL_MAP_READ);
	memcpy(camera->positionBuffer, ptr, pixelCount * sizeof(float4));
	CL_Buffer_Unmap(&raster->ctx, &raster->positionBufferGPU, ptr);

	ptr = CL_Buffer_Map(&raster->ctx, &raster->reflectBufferGPU, CL_MAP_READ);
	memcpy(camera->reflectBuffer, ptr, pixelCount * sizeof(float4));
	CL_Buffer_Unmap(&raster->ctx, &raster->reflectBufferGPU, ptr);
}

void GPURaster_Reload(GPURaster *raster,
					  const Object *objects, int objectCount,
					  const MaterialLib *lib) {
	if (!raster || !objects || objectCount <= 0 || !lib) return;

	destroyGeometry(raster);
	if (!uploadGeometry(raster, objects, objectCount, lib)) {
		fprintf(stderr, "[GPURaster] Reload: geometry upload failed.\n");
		return;
	}

	if (objectCount != raster->objectCount) {
		if (!allocTransformBuffers(raster, objectCount)) {
			fprintf(stderr, "[GPURaster] Reload: transform buffer realloc failed.\n");
			return;
		}
	}

	printf("[GPURaster] Reloaded: %d objects, %d triangles\n",
		   objectCount, raster->totalTriangles);
}

void GPURaster_Destroy(GPURaster *raster) {
	if (!raster) return;
	destroyGeometry(raster);
	CL_Buffer_Destroy(&raster->objPositions);
	CL_Buffer_Destroy(&raster->objRotations);
	CL_Buffer_Destroy(&raster->objScales);
	free(raster->hostPos);
	free(raster->hostRot);
	free(raster->hostSca);
	CL_Buffer_Destroy(&raster->framebufferGPU);
	CL_Buffer_Destroy(&raster->depthBufferInt);
	CL_Buffer_Destroy(&raster->normalBufferGPU);
	CL_Buffer_Destroy(&raster->positionBufferGPU);
	CL_Buffer_Destroy(&raster->reflectBufferGPU);
	CL_Pipeline_Destroy(&raster->pipeline);
	CL_Context_Destroy(&raster->ctx);
	free(raster);
}
