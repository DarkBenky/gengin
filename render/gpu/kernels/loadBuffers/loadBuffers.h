#ifndef LOAD_BUFFERS_H
#define LOAD_BUFFERS_H

#include "../../../object/format.h"
#include "../../../object/object.h"
#include "../../format.h"

typedef struct Arena {
	void *memory;
	size_t objectSize;
	int len;
	int capacity;
} Arena;

// we use arena to avoid alocations each frame for each buffer
Arena createArena(size_t objectSize, int capacity);
void addToArena(Arena *arena, void *object);
void clearArena(Arena *arena);
void resetArena(Arena *arena, size_t objectSize);
void setObjectSize(Arena *arena, size_t newSize);
void destroyArena(Arena *arena);

// Object metadata
CL_Buffer loadObjectIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectIdOffsets(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);

// Geometry
CL_Buffer loadObjectPositions(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectRotations(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectScales(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectVertices(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectNormals(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectMaterialIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);
CL_Buffer loadObjectTriangleCounts(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena);

// Precompute screen-space bounding boxes for each object to optimize rasterization.
CL_Buffer loadObjectScreenBounds(Object *objects, int objectCount, Camera camera, CL_Context *ctx, cl_mem_flags flag, Arena *arena);

// Material data
CL_Buffer loadMaterials(MaterialLib *lib, CL_Context *ctx, cl_mem_flags flag);

// Output buffers for rasterization results
CL_Buffer createPositionDepthBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer createColorBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer createNormalBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer createReflectionBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer createMaterialIdBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer createObjectIdBuffer(int width, int height, CL_Context *ctx, cl_mem_flags flag);

// Per-frame partial updates — writes only the changed per-object slice without
// rebuilding the whole CPU-side buffer.  Pass the already-created buffer from
// the functions above and the index of the single object that moved.
void updateObjectPosition(CL_Buffer *buf, Object *objects, int index, CL_Context *ctx);
void updateObjectRotation(CL_Buffer *buf, Object *objects, int index, CL_Context *ctx);
void updateObjectScale(CL_Buffer *buf, Object *objects, int index, CL_Context *ctx);

#endif // LOAD_BUFFERS_H