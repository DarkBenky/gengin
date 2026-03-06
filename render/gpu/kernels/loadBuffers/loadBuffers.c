#include "loadBuffers.h"
#include <math.h>

static Arena createArena(size_t objectSize, int capacity) {
	Arena arena;
	arena.memory = malloc(objectSize * capacity);
	arena.objectSize = objectSize;
	arena.len = 0;
	arena.capacity = capacity;
	return arena;
}

void addToArena(Arena *arena, void *object) {
	if (arena->len >= arena->capacity) {
		int newCap = arena->capacity * 2;
		void *resized = realloc(arena->memory, arena->objectSize * newCap);
		if (!resized) {
			fprintf(stderr, "Error: Could not grow Arena.\n");
			return;
		}
		arena->memory = resized;
		arena->capacity = newCap;
	}
	memcpy((char *)arena->memory + arena->len * arena->objectSize, object, arena->objectSize);
	arena->len++;
}

void clearArena(Arena *arena) {
	arena->len = 0;
}

void setObjectSize(Arena *arena, size_t newSize) {
	if (newSize == arena->objectSize) return;
	void *resized = realloc(arena->memory, newSize * arena->capacity);
	if (!resized) {
		fprintf(stderr, "Error: Could not resize Arena.\n");
		return;
	}
	arena->memory = resized;
	arena->objectSize = newSize;
}

void resetArena(Arena *arena, size_t objectSize) {
	setObjectSize(arena, objectSize);
	clearArena(arena);
}

void destroyArena(Arena *arena) {
	free(arena->memory);
	arena->memory = NULL;
	arena->objectSize = 0;
	arena->len = arena->capacity = 0;
}

CL_Buffer loadObjectIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &i);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(int), arena->memory, flag);
}

CL_Buffer loadObjectIdOffsets(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));
	int offset = 0;
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &offset);
		offset += objects[i].triangleCount;
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(int), arena->memory, flag);
}

CL_Buffer loadObjectPositions(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(float3));
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &objects[i].position);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(float3), arena->memory, flag);
}

CL_Buffer loadObjectRotations(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(float3));
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &objects[i].rotation);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(float3), arena->memory, flag);
}

CL_Buffer loadObjectScales(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(float3));
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &objects[i].scale);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(float3), arena->memory, flag);
}

// Loads all triangle v1 vertices from all objects into a flat buffer.
CL_Buffer loadObjectVertices(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(float3));
	for (int i = 0; i < objectCount; i++) {
		for (int t = 0; t < objects[i].triangleCount; t++) {
			addToArena(arena, &objects[i].v1[t]);
		}
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(float3), arena->memory, flag);
}

CL_Buffer loadObjectNormals(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(float3));
	for (int i = 0; i < objectCount; i++) {
		for (int t = 0; t < objects[i].triangleCount; t++) {
			addToArena(arena, &objects[i].normals[t]);
		}
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(float3), arena->memory, flag);
}

CL_Buffer loadObjectMaterialIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));
	for (int i = 0; i < objectCount; i++) {
		for (int t = 0; t < objects[i].triangleCount; t++) {
			addToArena(arena, &objects[i].materialIds[t]);
		}
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(int), arena->memory, flag);
}

CL_Buffer loadObjectTriangleCounts(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &objects[i].triangleCount);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(int), arena->memory, flag);
}

CL_Buffer loadObjectScreenBounds(Object *objects, int objectCount, Camera camera, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);

	typedef struct {
		int minX, minY, maxX, maxY;
	} Bounds;
	resetArena(arena, sizeof(Bounds));

	float aspect = (float)camera.screenWidth / (float)camera.screenHeight;
	float fovScale = tanf(camera.fov * 0.5f * 3.14159265f / 180.0f);

	float3 fwd = camera.forward;
	float3 right = camera.right;
	float3 up = camera.up;

	for (int i = 0; i < objectCount; i++) {
		float3 corners[8];
		float3 bmin = objects[i].worldBBmin;
		float3 bmax = objects[i].worldBBmax;
		corners[0] = (float3){bmin.x, bmin.y, bmin.z};
		corners[1] = (float3){bmax.x, bmin.y, bmin.z};
		corners[2] = (float3){bmin.x, bmax.y, bmin.z};
		corners[3] = (float3){bmax.x, bmax.y, bmin.z};
		corners[4] = (float3){bmin.x, bmin.y, bmax.z};
		corners[5] = (float3){bmax.x, bmin.y, bmax.z};
		corners[6] = (float3){bmin.x, bmax.y, bmax.z};
		corners[7] = (float3){bmax.x, bmax.y, bmax.z};

		int sxMin = camera.screenWidth, syMin = camera.screenHeight;
		int sxMax = 0, syMax = 0;
		for (int k = 0; k < 8; k++) {
			float3 c = {corners[k].x - camera.position.x,
						corners[k].y - camera.position.y,
						corners[k].z - camera.position.z};
			float z = c.x * fwd.x + c.y * fwd.y + c.z * fwd.z;
			if (z <= 0.01f) {
				sxMin = 0;
				syMin = 0;
				sxMax = camera.screenWidth - 1;
				syMax = camera.screenHeight - 1;
				break;
			}
			float nx = (c.x * right.x + c.y * right.y + c.z * right.z) / (z * fovScale * aspect);
			float ny = (c.x * up.x + c.y * up.y + c.z * up.z) / (z * fovScale);
			int px = (int)((nx + 1.0f) * 0.5f * camera.screenWidth);
			int py = (int)((1.0f - ny) * 0.5f * camera.screenHeight);
			if (px < sxMin) sxMin = px;
			if (px > sxMax) sxMax = px;
			if (py < syMin) syMin = py;
			if (py > syMax) syMax = py;
		}
		if (sxMin < 0) sxMin = 0;
		if (syMin < 0) syMin = 0;
		if (sxMax >= camera.screenWidth) sxMax = camera.screenWidth - 1;
		if (syMax >= camera.screenHeight) syMax = camera.screenHeight - 1;

		Bounds b = {sxMin, syMin, sxMax, syMax};
		addToArena(arena, &b);
	}
	return CL_Buffer_CreateFromData(ctx, arena->len * sizeof(Bounds), arena->memory, flag);
}

// Pack Material entries into two float4 arrays to avoid struct-layout issues.
// matColors[i] = {r, g, b, roughness}
// matProps[i]  = {metallic, emission, 0, 0}
CL_Buffer loadMaterials(MaterialLib *lib, CL_Context *ctx, cl_mem_flags flag) {
	if (!lib || lib->count <= 0 || !ctx) exit(1);
	size_t sz = lib->count * sizeof(float4);
	float4 *buf = malloc(sz);
	if (!buf) exit(1);
	for (int i = 0; i < lib->count; i++) {
		buf[i].x = lib->entries[i].color.x;
		buf[i].y = lib->entries[i].color.y;
		buf[i].z = lib->entries[i].color.z;
		buf[i].w = lib->entries[i].roughness;
	}
	CL_Buffer result = CL_Buffer_CreateFromData(ctx, sz, buf, flag);
	free(buf);
	return result;
}

// Load material metallic/emission properties
CL_Buffer loadMaterialProps(MaterialLib *lib, CL_Context *ctx, cl_mem_flags flag) {
	if (!lib || lib->count <= 0 || !ctx) exit(1);
	size_t sz = lib->count * sizeof(float4);
	float4 *buf = malloc(sz);
	if (!buf) exit(1);
	for (int i = 0; i < lib->count; i++) {
		buf[i].x = lib->entries[i].metallic;
		buf[i].y = lib->entries[i].emission;
		buf[i].z = 0.0f;
		buf[i].w = 0.0f;
	}
	CL_Buffer result = CL_Buffer_CreateFromData(ctx, sz, buf, flag);
	free(buf);
	return result;
}

CL_Buffer createPositionDepthBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(float4), flag);
}

CL_Buffer createColorBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(float4), flag);
}

CL_Buffer createNormalBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(float4), flag);
}

CL_Buffer createReflectionBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(float4), flag);
}

CL_Buffer createMaterialIdBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(int), flag);
}

CL_Buffer createObjectIdBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag) {
	return CL_Buffer_Create(ctx, (size_t)width * height * sizeof(int), flag);
}
