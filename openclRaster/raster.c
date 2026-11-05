#include "raster.h"
#include "../math/vector3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Work-group size for the two scatter passes. The triangle count is the global size,
// rounded up so every launched item maps to a triangle (or exits on the bounds check).
#define RASTER_LOCAL_SIZE 64
// Screen-space passes (sky, AO) use square tiles; CL_Dispatch2D takes the global size as
// given, so the launch is rounded up here and the kernels bounds-check against the real
// width/height they receive as arguments.
#define RASTER_TILE 16
// Emissive objects are uploaded as point lights for the indirect term. The shade kernel
// loops them per pixel, so this stays small on purpose.
#define RASTER_MAX_EMITTERS 8
// 1 = map/memcpy/unmap, 0 = blocking clEnqueueReadBuffer. Measured on this driver: map is
// 12.8 ms/frame, read is 6.1 ms, so the read stays the default even though the buffers are
// pinned (a map here costs more than the DMA it replaces).
#define RASTER_READBACK_MAP 0

static void rasterDisable(RasterRenderer *rr, const char *why) {
	if (rr->enabled)
		printf("[raster] GPU raster pass disabled: %s\n", why);
	rr->enabled = 0;
}

// clCreateBuffer rejects size 0, and the kernels always bind every buffer argument, so an
// empty scene (no textures, no materials) still needs a real allocation to point at.
static CL_Buffer createStaticBuffer(RasterRenderer *rr, size_t size, const void *data) {
	if (size == 0) size = 4;
	return data ? CL_Buffer_CreateFromData(&rr->ctx, size, (void *)data, CL_MEM_READ_ONLY)
				: CL_Buffer_Create(&rr->ctx, size, CL_MEM_READ_ONLY);
}

// The CL helpers in render/gpu/format.c release handles unconditionally; a renderer that
// failed its init has NULL handles, so releases go through these guards.
static void freeBuffer(CL_Buffer *buf) {
	if (buf->buf) CL_Buffer_Destroy(buf);
	buf->buf = NULL;
}

static void freePipeline(CL_Pipeline *pip) {
	if (pip->kernel) clReleaseKernel(pip->kernel);
	if (pip->program) clReleaseProgram(pip->program);
	pip->kernel = NULL;
	pip->program = NULL;
}

void Raster_Init(RasterRenderer *rr, const char *kernelPath, int width, int height) {
	memset(rr, 0, sizeof(*rr)); // every early return must leave NULL handles, not stack garbage
	rr->width = width;
	rr->height = height;
	rr->enabled = 1;
	// Every transfer is a PCIe copy, so the frame path only takes what a frame needs.
	rr->readbackMask = RASTER_OUT_FRAME;
	// AO.h applies 0.55 to a term it then blurs over a 4x downsample. The reach here is
	// shorter (one object width, 24 px of screen), so a higher strength keeps the contact
	// darkening visible. Past ~1.2 the grazing-floor bias starts to show.
	rr->aoStrength = 1.0f;

	rr->ctx = CL_Context_Create();
	rr->depthPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeDepth", NULL);
	rr->shadePipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeShade", NULL);
	rr->skyPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeSky", NULL);
	rr->aoPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeAo", NULL);
	rr->aoBlurPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeAoBlur", NULL);
	rr->aoApplyPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeAoApply", NULL);
	rr->shadowPipeline = CL_Pipeline_FromFile(&rr->ctx, kernelPath, "rasterizeShadow", NULL);

	size_t pixels = (size_t)width * height;
	rr->objectBuf = CL_Buffer_CreatePinned(&rr->ctx, sizeof(RasterObject), CL_MEM_READ_ONLY);
	rr->depthUBuf = CL_Buffer_Create(&rr->ctx, pixels * sizeof(cl_ulong), CL_MEM_READ_WRITE);
	rr->aoBufA = CL_Buffer_Create(&rr->ctx, pixels * sizeof(float), CL_MEM_READ_WRITE);
	rr->aoBufB = CL_Buffer_Create(&rr->ctx, pixels * sizeof(float), CL_MEM_READ_WRITE);

	// A disabled shadow map still needs a valid buffer (the shade kernel always binds it;
	// shadow.params.z is what turns the lookup off).
	rr->shadowMapSize = 2048;
	rr->shadowBuf = CL_Buffer_Create(&rr->ctx, (size_t)rr->shadowMapSize * rr->shadowMapSize * sizeof(uint), CL_MEM_READ_WRITE);
	rr->framebufferBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(float4), CL_MEM_READ_WRITE);
	rr->depthBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(float), CL_MEM_READ_WRITE);
	rr->normalBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(float4), CL_MEM_READ_WRITE);
	rr->positionBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(float4), CL_MEM_READ_WRITE);
	rr->objectIdBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(int), CL_MEM_READ_WRITE);
	rr->triangleIdBuf = CL_Buffer_CreatePinned(&rr->ctx, pixels * sizeof(int), CL_MEM_READ_WRITE);

	if (rr->ctx.context == NULL || rr->ctx.queue == NULL ||
		rr->depthPipeline.kernel == NULL || rr->shadePipeline.kernel == NULL ||
		rr->skyPipeline.kernel == NULL || rr->aoPipeline.kernel == NULL ||
		rr->aoBlurPipeline.kernel == NULL || rr->aoApplyPipeline.kernel == NULL ||
		rr->shadowPipeline.kernel == NULL ||
		rr->depthUBuf.buf == NULL || rr->aoBufA.buf == NULL || rr->aoBufB.buf == NULL ||
		rr->shadowBuf.buf == NULL ||
		rr->framebufferBuf.buf == NULL ||
		rr->depthBuf.buf == NULL || rr->normalBuf.buf == NULL ||
		rr->positionBuf.buf == NULL || rr->objectIdBuf.buf == NULL ||
		rr->triangleIdBuf.buf == NULL) {
		rasterDisable(rr, "OpenCL init failed (device busy or out of resources)");
		return;
	}
}

// One Textures block per unique pointer — several materials in a loaded model share the
// same map, and uploading it twice would double the (already large) texture traffic.
typedef struct {
	Textures *tex;
	int slot;
} TextureSlot;

static int findTextureSlot(TextureSlot *slots, int count, Textures *tex) {
	for (int i = 0; i < count; i++)
		if (slots[i].tex == tex) return slots[i].slot;
	return -1;
}

// The CL record stores float4 (16-byte) slots while Object keeps float3, which is the same
// four floats with w as alignment padding — copy the three used components.
static inline float4 f4(float3 v) {
	return (float4){v.x, v.y, v.z, 0.0f};
}

void Raster_UploadScene(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib) {
	if (!rr->enabled || !scene || !lib) return;

	// Count what the scene actually contributes: objects without geometry are skipped, so
	// the triangle packing and the per-object triStart table stay in sync.
	int totalTris = 0;
	int objectCount = 0;
	for (int i = 0; i < scene->count; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->v1 || obj->triangleCount <= 0) continue;
		totalTris += obj->triangleCount;
		objectCount++;
	}
	if (totalTris == 0) {
		rr->triCount = 0;
		rr->objectCount = 0;
		return;
	}

	RasterTri *tris = malloc(sizeof(RasterTri) * totalTris);
	int *triObjectIdx = malloc(sizeof(int) * totalTris);
	int *triStarts = malloc(sizeof(int) * objectCount);
	if (!tris || !triObjectIdx || !triStarts) {
		free(tris);
		free(triObjectIdx);
		free(triStarts);
		rasterDisable(rr, "out of memory building the triangle batch");
		return;
	}

	int t = 0, o = 0;
	for (int i = 0; i < scene->count; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->v1 || obj->triangleCount <= 0) continue;
		triStarts[o] = t;
		for (int k = 0; k < obj->triangleCount; k++, t++) {
			tris[t] = (RasterTri){
				f4(obj->v1[k]),
				f4(obj->v2[k]),
				f4(obj->v3[k]),
				f4(obj->normals[k]),
				0,
				0,
				0,
				obj->materialIds ? obj->materialIds[k] : -1,
			};
			// The bin format stores UVs per triangle; primitives built in code may have none.
			if (obj->uvs) {
				const UvCords *uv = &obj->uvs[k];
				tris[t].uv01 = (uint)uv->uv1x | ((uint)uv->uv1y << 16);
				tris[t].uv23 = (uint)uv->uv2x | ((uint)uv->uv2y << 16);
				tris[t].uv45 = (uint)uv->uv3x | ((uint)uv->uv3y << 16);
			}
			triObjectIdx[t] = o;
		}
		o++;
	}

	// Texture table: unique Textures blocks, concatenated into flat map buffers. colorMap
	// and normalMap are RGBA8 (one uint per texel), MaterialMap is [rough, metal] pairs.
	// All three share the same texel offsets, so one RasterTexture entry addresses them.
	int textureCount = 0;
	size_t texelTotal = 0;
	TextureSlot *slots = malloc(sizeof(TextureSlot) * (lib->count > 0 ? lib->count : 1));
	for (int i = 0; i < lib->count; i++) {
		Textures *tex = lib->entries[i].textures;
		if (!tex || !tex->colorMap || !tex->MaterialMap) continue;
		if (findTextureSlot(slots, textureCount, tex) >= 0) continue;
		slots[textureCount].tex = tex;
		slots[textureCount].slot = textureCount;
		textureCount++;
		texelTotal += (size_t)tex->size * tex->size;
	}

	RasterTexture *texTable = textureCount ? malloc(sizeof(RasterTexture) * textureCount) : NULL;
	uint *colorMaps = texelTotal ? malloc(texelTotal * sizeof(uint)) : NULL;
	uint *normalMaps = texelTotal ? malloc(texelTotal * sizeof(uint)) : NULL;
	uint16 *materialMaps = texelTotal ? malloc(texelTotal * sizeof(uint16)) : NULL;
	if (textureCount && (!texTable || !colorMaps || !normalMaps || !materialMaps)) {
		free(slots);
		free(texTable);
		free(colorMaps);
		free(normalMaps);
		free(materialMaps);
		free(tris);
		free(triObjectIdx);
		free(triStarts);
		rasterDisable(rr, "out of memory building the texture atlas");
		return;
	}

	size_t cursor = 0;
	for (int s = 0; s < textureCount; s++) {
		Textures *tex = slots[s].tex;
		size_t texels = (size_t)tex->size * tex->size;
		texTable[s] = (RasterTexture){(int)cursor, (int)tex->size};
		memcpy(colorMaps + cursor, tex->colorMap, texels * sizeof(uint));
		memcpy(materialMaps + cursor, tex->MaterialMap, texels * sizeof(uint16));
		// Primitives built in code can carry a material without a normal map; leave the
		// slice at 0 so the decode yields a flat +Z tangent normal.
		if (tex->normalMap)
			memcpy(normalMaps + cursor, tex->normalMap, texels * sizeof(uint));
		else
			memset(normalMaps + cursor, 0, texels * sizeof(uint));
		cursor += texels;
	}

	// Material table indexed exactly like Object.materialIds, with textureSlot resolved
	// through the dedup table (-1 when the material has no maps).
	RasterMaterial *matTable = malloc(sizeof(RasterMaterial) * (lib->count > 0 ? lib->count : 1));
	for (int i = 0; i < lib->count; i++) {
		const Material *m = &lib->entries[i];
		int slot = (m->textures && m->textures->colorMap && m->textures->MaterialMap)
					   ? findTextureSlot(slots, textureCount, m->textures)
					   : -1;
		matTable[i] = (RasterMaterial){
			(float4){m->color.x, m->color.y, m->color.z, 0.0f},
			m->roughness,
			m->metallic,
			m->emission,
			slot,
		};
	}

	// Emissive objects become point lights for the indirect term. The engine flags them on
	// the Object, so this is read straight off the scene rather than the material table.
	RasterEmitter emitterTable[RASTER_MAX_EMITTERS];
	int emitterCount = 0;
	for (int i = 0; i < scene->count && emitterCount < RASTER_MAX_EMITTERS; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->hasEmission || !obj->v1 || obj->triangleCount <= 0) continue;
		// Take the strongest material on the object; merged emitters share one material each.
		float3 color = {0.0f, 0.0f, 0.0f};
		float emission = 0.0f;
		for (int k = 0; k < obj->triangleCount; k++) {
			int matId = obj->materialIds ? obj->materialIds[k] : -1;
			if (matId < 0 || matId >= lib->count) continue;
			if (lib->entries[matId].emission > emission) {
				emission = lib->entries[matId].emission;
				color = lib->entries[matId].color;
			}
		}
		if (emission <= 0.0f) continue;
		emitterTable[emitterCount] = (RasterEmitter){
			(float4){obj->position.x, obj->position.y, obj->position.z, 0.0f},
			(float4){color.x * emission, color.y * emission, color.z * emission, 0.0f},
		};
		emitterCount++;
	}

	// Replace the previous static buffers (re-upload after a scene rebuild frees the old ones).
	freeBuffer(&rr->triBuf);
	freeBuffer(&rr->triObjectBuf);
	freeBuffer(&rr->materialBuf);
	freeBuffer(&rr->textureTableBuf);
	freeBuffer(&rr->colorMapBuf);
	freeBuffer(&rr->normalMapBuf);
	freeBuffer(&rr->materialMapBuf);

	rr->triBuf = createStaticBuffer(rr, sizeof(RasterTri) * totalTris, tris);
	rr->triObjectBuf = createStaticBuffer(rr, sizeof(int) * totalTris, triObjectIdx);
	rr->materialBuf = createStaticBuffer(rr, sizeof(RasterMaterial) * lib->count, matTable);
	rr->textureTableBuf = createStaticBuffer(rr, sizeof(RasterTexture) * textureCount, texTable);
	rr->colorMapBuf = createStaticBuffer(rr, texelTotal * sizeof(uint), colorMaps);
	rr->normalMapBuf = createStaticBuffer(rr, texelTotal * sizeof(uint), normalMaps);
	rr->materialMapBuf = createStaticBuffer(rr, texelTotal * sizeof(uint16), materialMaps);
	freeBuffer(&rr->emitterBuf);
	rr->emitterBuf = createStaticBuffer(rr, sizeof(RasterEmitter) * emitterCount, emitterTable);
	rr->emitterCount = emitterCount;

	free(tris);
	free(triObjectIdx);
	free(texTable);
	free(colorMaps);
	free(normalMaps);
	free(materialMaps);
	free(slots);

	free(rr->triStarts);
	rr->triStarts = triStarts;
	rr->triCount = totalTris;
	rr->objectCount = objectCount;

	printf("[raster] uploaded %d triangles, %d objects, %d materials, %d texture maps (%zu texels), %d emitters\n",
		   totalTris, objectCount, lib->count, textureCount, texelTotal, emitterCount);
}

// The six faces are the same size, so one buffer holding them back to back plus the face
// dimensions is all the kernel needs to reproduce SampleSkybox.
void Raster_SetSkybox(RasterRenderer *rr, const Skybox *skybox) {
	if (!rr->enabled) return;

	const uint32 *faces[6] = {skybox ? skybox->right : NULL, skybox ? skybox->left : NULL,
							  skybox ? skybox->top : NULL, skybox ? skybox->bottom : NULL,
							  skybox ? skybox->front : NULL, skybox ? skybox->back : NULL};
	freeBuffer(&rr->skyBuf);
	rr->skyWidth = 0;
	rr->skyHeight = 0;

	// A partially loaded skybox would sample garbage on the missing faces; require all six.
	for (int i = 0; i < 6; i++)
		if (!faces[i]) return;
	if (skybox->imageWidth <= 0 || skybox->imageHeight <= 0) return;

	size_t faceTexels = (size_t)skybox->imageWidth * skybox->imageHeight;
	uint32 *packed = malloc(faceTexels * 6 * sizeof(uint32));
	if (!packed) {
		rasterDisable(rr, "out of memory packing the skybox");
		return;
	}
	for (int i = 0; i < 6; i++)
		memcpy(packed + faceTexels * i, faces[i], faceTexels * sizeof(uint32));

	rr->skyBuf = createStaticBuffer(rr, faceTexels * 6 * sizeof(uint32), packed);
	free(packed);
	if (!rr->skyBuf.buf) return;

	rr->skyWidth = skybox->imageWidth;
	rr->skyHeight = skybox->imageHeight;
	printf("[raster] uploaded skybox (%dx%d x6)\n", rr->skyWidth, rr->skyHeight);
}

// The per-frame object table is tiny, so it is rebuilt from scratch every frame instead of
// diffing — the rotation rows are already cached by Object_UpdateWorldBounds (no trig on GPU).
static void uploadObjects(RasterRenderer *rr, const ObjectList *scene) {
	int capacity = scene->capacity > rr->objectCount ? scene->capacity : rr->objectCount;
	if (rr->objectBuf.size < (size_t)capacity * sizeof(RasterObject)) {
		freeBuffer(&rr->objectBuf);
		rr->objectBuf = CL_Buffer_CreatePinned(&rr->ctx, (size_t)capacity * sizeof(RasterObject), CL_MEM_READ_ONLY);
	}

	RasterObject *objs = CL_Buffer_Map(&rr->ctx, &rr->objectBuf, CL_MAP_WRITE_INVALIDATE_REGION);
	if (!objs) {
		rasterDisable(rr, "object upload map failed");
		return;
	}

	int o = 0;
	for (int i = 0; i < scene->count && o < rr->objectCount; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->v1 || obj->triangleCount <= 0) continue;
		int flags = 0;
		if (obj->hasTexture) flags |= RASTER_FLAG_TEXTURE;
		if (obj->cullBackfaces) flags |= RASTER_FLAG_CULL;
		objs[o] = (RasterObject){
			(float4){obj->position.x, obj->position.y, obj->position.z, 0.0f},
			(float4){obj->scale.x, obj->scale.y, obj->scale.z, 0.0f},
			(float4){obj->_fwdRot0.x, obj->_fwdRot0.y, obj->_fwdRot0.z, 0.0f},
			(float4){obj->_fwdRot1.x, obj->_fwdRot1.y, obj->_fwdRot1.z, 0.0f},
			(float4){obj->_fwdRot2.x, obj->_fwdRot2.y, obj->_fwdRot2.z, 0.0f},
			rr->triStarts[o],
			obj->triangleCount,
			flags,
			i,
		};
		o++;
	}

	CL_Buffer_Unmap(&rr->ctx, &rr->objectBuf, objs);
}

static void readbackPinned(RasterRenderer *rr, CL_Buffer *buf, void *dst, size_t bytes) {
	// The buffers are host-visible (CL_MEM_ALLOC_HOST_PTR), so a blocking read gives the
	// driver one DMA instead of a map the caller then copies out of itself.
#if RASTER_READBACK_MAP
	void *ptr = CL_Buffer_Map(&rr->ctx, buf, CL_MAP_READ);
	if (!ptr) {
		rasterDisable(rr, "G-buffer read-back map failed");
		return;
	}
	memcpy(dst, ptr, bytes);
	CL_Buffer_Unmap(&rr->ctx, buf, ptr);
#else
	CL_Buffer_Read(&rr->ctx, buf, dst, bytes);
#endif
}

static void dispatchScreenTiled(RasterRenderer *rr, CL_Pipeline *pip, int width, int height) {
	size_t tilesW = ((size_t)width + RASTER_TILE - 1) / RASTER_TILE;
	size_t tilesH = ((size_t)height + RASTER_TILE - 1) / RASTER_TILE;
	CL_Dispatch2D(&rr->ctx, pip, tilesW * RASTER_TILE, tilesH * RASTER_TILE, RASTER_TILE, RASTER_TILE);
}

// Fits an orthographic light frame around the scene bounds. cam->lightDir points from the
// surface toward the light, so the map is rasterized looking the other way. Returns 0 when
// the bounds are unusable, which leaves the shadow pass disabled for that frame.
static int buildShadowFrame(RasterRenderer *rr, const ObjectList *scene, const Camera *cam) {
	float3 bmin = {FLT_MAX, FLT_MAX, FLT_MAX};
	float3 bmax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	int counted = 0;

	for (int i = 0; i < scene->count; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->v1 || obj->triangleCount <= 0) continue;
		bmin = (float3){fminf(bmin.x, obj->worldBBmin.x), fminf(bmin.y, obj->worldBBmin.y), fminf(bmin.z, obj->worldBBmin.z)};
		bmax = (float3){fmaxf(bmax.x, obj->worldBBmax.x), fmaxf(bmax.y, obj->worldBBmax.y), fmaxf(bmax.z, obj->worldBBmax.z)};
		counted++;
	}
	if (!counted || !(bmin.x <= bmax.x) || !(bmin.y <= bmax.y) || !(bmin.z <= bmax.z))
		return 0;

	float3 center = {(bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, (bmin.z + bmax.z) * 0.5f};
	float3 fwd = Float3_Scale(Float3_Normalize(cam->lightDir), -1.0f);
	// Any up hint that is not parallel to the light works; Y only fails for a light
	// pointing straight down, so fall back to Z in that case.
	float3 hint = fabsf(fwd.y) > 0.95f ? (float3){0.0f, 0.0f, 1.0f} : (float3){0.0f, 1.0f, 0.0f};
	float3 right = Float3_Normalize(Float3_Cross(hint, fwd));
	float3 up = Float3_Cross(fwd, right);

	// Fit the 8 corners of the world bounds so the whole scene lands inside the map.
	float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX;
	for (int c = 0; c < 8; c++) {
		float3 corner = {(c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z};
		float3 lv = Float3_Sub(corner, center);
		float x = Float3_Dot(lv, right);
		float y = Float3_Dot(lv, up);
		float z = Float3_Dot(lv, fwd);
		minX = fminf(minX, x);
		maxX = fmaxf(maxX, x);
		minY = fminf(minY, y);
		maxY = fmaxf(maxY, y);
		minZ = fminf(minZ, z);
	}

	float halfW = fmaxf(fmaxf(fabsf(minX), fabsf(maxX)), 1e-3f);
	float halfH = fmaxf(fmaxf(fabsf(minY), fabsf(maxY)), 1e-3f);
	float size = (float)rr->shadowMapSize;

	rr->shadow = (RasterShadow){
		(float4){center.x, center.y, center.z, 0.0f},
		(float4){right.x, right.y, right.z, 1.0f / halfW},
		(float4){up.x, up.y, up.z, 1.0f / halfH},
		(float4){fwd.x, fwd.y, fwd.z, minZ},
		// Bias is one texel in world units on the longer axis, widened at grazing angles.
		(float4){size, 2.0f * halfW / size, 1.0f, 0.0f},
	};
	return 1;
}

void Raster_Render(RasterRenderer *rr, const ObjectList *scene, const MaterialLib *lib, Camera *cam) {
	(void)lib;
	if (!rr->enabled || !scene || !cam) return;
	if (rr->triCount == 0) return;

	// The static batch is indexed by this exact packing; a geometry change without a
	// re-upload would rasterize the wrong triangles, so refuse instead of guessing.
	// (Same counts with different vertices still need a re-upload — the caller knows when
	// it merged or rebuilt geometry and must call Raster_UploadScene again.)
	int objects = 0, tris = 0;
	for (int i = 0; i < scene->count; i++) {
		const Object *obj = &scene->objects[i];
		if (!obj->v1 || obj->triangleCount <= 0) continue;
		objects++;
		tris += obj->triangleCount;
	}
	if (objects != rr->objectCount || tris != rr->triCount) {
		printf("[raster] scene geometry changed since Raster_UploadScene — re-upload required\n");
		return;
	}

	uploadObjects(rr, scene);
	if (!rr->enabled) return;

	// The ray tracer's row kernel normalizes forward/right/up per frame; the raster must
	// use the same basis or its projection samples different world directions per pixel.
	float3 camFwd = Float3_Normalize(cam->forward);
	float3 camRight = Float3_Normalize(cam->right);
	float3 camUp = Float3_Normalize(cam->up);

	// Shadow map first: the shade pass reads it. A cleared texel must hold a large finite
	// depth (not 0xFFFFFFFF, which decodes as NaN and would shadow everything).
	int shadowsOn = rr->shadowMapSize > 0 && buildShadowFrame(rr, scene, cam);
	if (!shadowsOn)
		rr->shadow.params.z = 0.0f;

	// clearBuffers() is a no-op in this engine, so every scratch and output buffer is cleared
	// here. The read-back copies whole buffers, so an uncovered pixel must come back as a
	// defined value (far depth / no object / black) rather than last frame's GPU contents.
	// The depth word is all ones: every real entry has smaller depth bits, so it always wins.
	uint far = 0xFFFFFFFFu;
	CL_Buffer_Fill(&rr->ctx, &rr->depthUBuf, &far, sizeof(far));

	float depthFar = DEPTH_FAR;
	CL_Buffer_Fill(&rr->ctx, &rr->depthBuf, &depthFar, sizeof(depthFar));
	int noId = -1;
	CL_Buffer_Fill(&rr->ctx, &rr->objectIdBuf, &noId, sizeof(noId));
	CL_Buffer_Fill(&rr->ctx, &rr->triangleIdBuf, &noId, sizeof(noId));
	float4 zero = {0};
	CL_Buffer_Fill(&rr->ctx, &rr->framebufferBuf, &zero, sizeof(zero));
	CL_Buffer_Fill(&rr->ctx, &rr->normalBuf, &zero, sizeof(zero));
	CL_Buffer_Fill(&rr->ctx, &rr->positionBuf, &zero, sizeof(zero));

	size_t global = ((size_t)rr->triCount + RASTER_LOCAL_SIZE - 1) / RASTER_LOCAL_SIZE * RASTER_LOCAL_SIZE;
	int width = cam->screenWidth, height = cam->screenHeight;

	if (shadowsOn) {
		float bigDepth = 1e30f;
		uint bigDepthBits;
		memcpy(&bigDepthBits, &bigDepth, sizeof(bigDepthBits));
		CL_Buffer_Fill(&rr->ctx, &rr->shadowBuf, &bigDepthBits, sizeof(bigDepthBits));

		cl_kernel shadowKernel = rr->shadowPipeline.kernel;
		int sa = 0;
		clSetKernelArg(shadowKernel, sa++, sizeof(cl_mem), &rr->triBuf.buf);
		clSetKernelArg(shadowKernel, sa++, sizeof(cl_mem), &rr->triObjectBuf.buf);
		clSetKernelArg(shadowKernel, sa++, sizeof(cl_mem), &rr->objectBuf.buf);
		clSetKernelArg(shadowKernel, sa++, sizeof(int), &rr->triCount);
		clSetKernelArg(shadowKernel, sa++, sizeof(RasterShadow), &rr->shadow);
		clSetKernelArg(shadowKernel, sa++, sizeof(cl_mem), &rr->shadowBuf.buf);
		CL_Dispatch1D(&rr->ctx, &rr->shadowPipeline, global, RASTER_LOCAL_SIZE);
	}

	cl_kernel k = rr->depthPipeline.kernel;
	int a = 0;
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->triBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->triObjectBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->objectBuf.buf);
	clSetKernelArg(k, a++, sizeof(int), &rr->triCount);
	clSetKernelArg(k, a++, sizeof(float3), &cam->position);
	clSetKernelArg(k, a++, sizeof(float3), &camRight);
	clSetKernelArg(k, a++, sizeof(float3), &camUp);
	clSetKernelArg(k, a++, sizeof(float3), &camFwd);
	clSetKernelArg(k, a++, sizeof(float), &cam->fovScale);
	clSetKernelArg(k, a++, sizeof(float), &cam->aspect);
	clSetKernelArg(k, a++, sizeof(float2), &cam->jitter);
	clSetKernelArg(k, a++, sizeof(int), &width);
	clSetKernelArg(k, a++, sizeof(int), &height);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthUBuf.buf);
	CL_Dispatch1D(&rr->ctx, &rr->depthPipeline, global, RASTER_LOCAL_SIZE);

	k = rr->shadePipeline.kernel;
	a = 0;
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->triBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->triObjectBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->objectBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->materialBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->textureTableBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->colorMapBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->normalMapBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->materialMapBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->skyBuf.buf);
	clSetKernelArg(k, a++, sizeof(int), &rr->skyWidth);
	clSetKernelArg(k, a++, sizeof(int), &rr->skyHeight);
	clSetKernelArg(k, a++, sizeof(RasterShadow), &rr->shadow);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->shadowBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->emitterBuf.buf);
	clSetKernelArg(k, a++, sizeof(int), &rr->emitterCount);
	clSetKernelArg(k, a++, sizeof(float3), &cam->position);
	clSetKernelArg(k, a++, sizeof(float3), &camRight);
	clSetKernelArg(k, a++, sizeof(float3), &camUp);
	clSetKernelArg(k, a++, sizeof(float3), &camFwd);
	clSetKernelArg(k, a++, sizeof(float), &cam->fovScale);
	clSetKernelArg(k, a++, sizeof(float), &cam->aspect);
	clSetKernelArg(k, a++, sizeof(float2), &cam->jitter);
	clSetKernelArg(k, a++, sizeof(float3), &cam->lightDir);
	clSetKernelArg(k, a++, sizeof(int), &width);
	clSetKernelArg(k, a++, sizeof(int), &height);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthUBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->framebufferBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->normalBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->positionBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->objectIdBuf.buf);
	clSetKernelArg(k, a++, sizeof(cl_mem), &rr->triangleIdBuf.buf);
	dispatchScreenTiled(rr, &rr->shadePipeline, width, height);

	size_t pixels = (size_t)width * height;

	// Sky fill: every pixel still holding the cleared depth word was missed by the scene.
	if (rr->skyWidth > 0) {
		k = rr->skyPipeline.kernel;
		a = 0;
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->skyBuf.buf);
		clSetKernelArg(k, a++, sizeof(int), &rr->skyWidth);
		clSetKernelArg(k, a++, sizeof(int), &rr->skyHeight);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthUBuf.buf);
		clSetKernelArg(k, a++, sizeof(float3), &camRight);
		clSetKernelArg(k, a++, sizeof(float3), &camUp);
		clSetKernelArg(k, a++, sizeof(float3), &camFwd);
		clSetKernelArg(k, a++, sizeof(float), &cam->fovScale);
		clSetKernelArg(k, a++, sizeof(float), &cam->aspect);
		clSetKernelArg(k, a++, sizeof(float2), &cam->jitter);
		clSetKernelArg(k, a++, sizeof(int), &width);
		clSetKernelArg(k, a++, sizeof(int), &height);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->framebufferBuf.buf);
		dispatchScreenTiled(rr, &rr->skyPipeline, width, height);
	}

	// Ambient occlusion, same chain as AO.h: raw SSAO, an edge-preserving separable blur,
	// then two rounds of a stride-4 gaussian that stands in for its 4x downsample blur.
	if (rr->aoStrength > 0.0f) {
		k = rr->aoPipeline.kernel;
		a = 0;
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthBuf.buf);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->normalBuf.buf);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->positionBuf.buf);
		clSetKernelArg(k, a++, sizeof(int), &width);
		clSetKernelArg(k, a++, sizeof(int), &height);
		clSetKernelArg(k, a++, sizeof(float), &cam->fovScale);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->aoBufA.buf);
		dispatchScreenTiled(rr, &rr->aoPipeline, width, height);

		// Six separable passes ping-pong between the two AO buffers; the last one must
		// land in aoBufA because the apply kernel reads from there.
		struct {
			int step;
			int alongX;
			int gated;
		} passes[6] = {
			{1, 1, 1},
			{1, 0, 1}, // edge-preserving blur
			{4, 1, 0},
			{4, 0, 0}, // wide smooth, round 1
			{4, 1, 0},
			{4, 0, 0}, // wide smooth, round 2
		};
		for (int p = 0; p < 6; p++) {
			CL_Buffer *src = (p % 2 == 0) ? &rr->aoBufA : &rr->aoBufB;
			CL_Buffer *dst = (p % 2 == 0) ? &rr->aoBufB : &rr->aoBufA;
			k = rr->aoBlurPipeline.kernel;
			a = 0;
			clSetKernelArg(k, a++, sizeof(cl_mem), &src->buf);
			clSetKernelArg(k, a++, sizeof(cl_mem), &rr->depthBuf.buf);
			clSetKernelArg(k, a++, sizeof(cl_mem), &dst->buf);
			clSetKernelArg(k, a++, sizeof(int), &width);
			clSetKernelArg(k, a++, sizeof(int), &height);
			clSetKernelArg(k, a++, sizeof(int), &passes[p].step);
			clSetKernelArg(k, a++, sizeof(int), &passes[p].alongX);
			clSetKernelArg(k, a++, sizeof(int), &passes[p].gated);
			dispatchScreenTiled(rr, &rr->aoBlurPipeline, width, height);
		}

		k = rr->aoApplyPipeline.kernel;
		a = 0;
		int pixelCount = width * height;
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->framebufferBuf.buf);
		clSetKernelArg(k, a++, sizeof(cl_mem), &rr->aoBufA.buf);
		clSetKernelArg(k, a++, sizeof(float), &rr->aoStrength);
		clSetKernelArg(k, a++, sizeof(int), &pixelCount);
		CL_Dispatch1D(&rr->ctx, &rr->aoApplyPipeline, ((size_t)pixelCount + RASTER_LOCAL_SIZE - 1) / RASTER_LOCAL_SIZE * RASTER_LOCAL_SIZE, RASTER_LOCAL_SIZE);
	}

	readbackPinned(rr, &rr->framebufferBuf, cam->framebufferHdr, pixels * sizeof(float3));
	if (rr->readbackMask & RASTER_OUT_DEPTH)
		readbackPinned(rr, &rr->depthBuf, cam->depthBuffer, pixels * sizeof(float));
	if (rr->readbackMask & RASTER_OUT_NORMAL)
		readbackPinned(rr, &rr->normalBuf, cam->normalBuffer, pixels * sizeof(float3));
	if (rr->readbackMask & RASTER_OUT_POSITION)
		readbackPinned(rr, &rr->positionBuf, cam->positionBuffer, pixels * sizeof(float3));
	if (rr->readbackMask & RASTER_OUT_OBJECT_ID)
		readbackPinned(rr, &rr->objectIdBuf, cam->objectIdBuffer, pixels * sizeof(int));
	if (rr->readbackMask & RASTER_OUT_TRIANGLE_ID)
		readbackPinned(rr, &rr->triangleIdBuf, cam->triangleIdBuffer, pixels * sizeof(int));
}

void Raster_Destroy(RasterRenderer *rr) {
	if (!rr) return;
	freeBuffer(&rr->triBuf);
	freeBuffer(&rr->triObjectBuf);
	freeBuffer(&rr->materialBuf);
	freeBuffer(&rr->textureTableBuf);
	freeBuffer(&rr->colorMapBuf);
	freeBuffer(&rr->normalMapBuf);
	freeBuffer(&rr->materialMapBuf);
	freeBuffer(&rr->emitterBuf);
	freeBuffer(&rr->skyBuf);
	freeBuffer(&rr->objectBuf);
	freeBuffer(&rr->depthUBuf);
	freeBuffer(&rr->aoBufA);
	freeBuffer(&rr->aoBufB);
	freeBuffer(&rr->shadowBuf);
	freeBuffer(&rr->framebufferBuf);
	freeBuffer(&rr->depthBuf);
	freeBuffer(&rr->normalBuf);
	freeBuffer(&rr->positionBuf);
	freeBuffer(&rr->objectIdBuf);
	freeBuffer(&rr->triangleIdBuf);
	freePipeline(&rr->depthPipeline);
	freePipeline(&rr->shadePipeline);
	freePipeline(&rr->skyPipeline);
	freePipeline(&rr->aoPipeline);
	freePipeline(&rr->aoBlurPipeline);
	freePipeline(&rr->aoApplyPipeline);
	freePipeline(&rr->shadowPipeline);
	if (rr->ctx.queue) CL_Context_Destroy(&rr->ctx);
	free(rr->triStarts);
	rr->triStarts = NULL;
}
