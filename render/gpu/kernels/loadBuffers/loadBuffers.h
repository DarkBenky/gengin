#ifndef LOAD_BUFFERS_H
#define LOAD_BUFFERS_H

#include "../../../object/format.h"
#include "../../../object/object.h"
#include "../../format.h"

CL_Buffer loadObjectIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectIdOffsets(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);

CL_Buffer loadObjectPositions(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectRotations(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectScales(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectVertices(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectNormals(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectMaterialIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);
CL_Buffer loadObjectTriangleCounts(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag);

// Precompute screen-space bounding boxes for each object to optimize rasterization.
CL_Buffer loadObjectScreenBounds(Object *objects, int objectCount, Camera camera, CL_Context *ctx, cl_mem_flags flag);

CL_Buffer loadMaterials(MaterialLib *lib, CL_Context *ctx, cl_mem_flags flag);