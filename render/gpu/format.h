#ifndef CL_APP_H
#define CL_APP_H

#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../object/format.h"

// TYPES
typedef struct {
    cl_platform_id   platform;
    cl_device_id     device;
    cl_context       context;
    cl_command_queue queue;
} CL_Context;

typedef struct {
    cl_program program;
    cl_kernel  kernel;
    char       name[64];
    float     timeTook; // for profiling
} CL_Pipeline;

typedef struct {
    cl_mem buf;
    size_t size;        // size in bytes
    cl_mem_flags flags; // CL_MEM_READ_ONLY / CL_MEM_WRITE_ONLY / CL_MEM_READ_WRITE
} CL_Buffer;

typedef struct {
    cl_mem image;
    int    width;
    int    height;
} CL_Image;

// CONTEXT
CL_Context CL_Context_Create();
void       CL_Context_Destroy(CL_Context* ctx);


// PIPELINE  (compiled kernel)
CL_Pipeline CL_Pipeline_FromFile(CL_Context* ctx, const char* path, const char* kernel_name, const char* options);
CL_Pipeline CL_Pipeline_FromSource(CL_Context* ctx, const char* src, const char* kernel_name, const char* options);

void CL_Pipeline_Destroy(CL_Pipeline* pip);

// BUFFER  (raw data on GPU — floats, structs, arrays)
CL_Buffer CL_Buffer_Create(CL_Context* ctx, size_t size, cl_mem_flags flags);
CL_Buffer CL_Buffer_CreateFromData(CL_Context* ctx, size_t size, void* data, cl_mem_flags flags);
// Allocates a pinned (page-locked) buffer for DMA-direct host<->device transfers.
// Map the returned buffer with CL_Buffer_Map before CPU access, Unmap after.
CL_Buffer CL_Buffer_CreatePinned(CL_Context* ctx, size_t size, cl_mem_flags flags);
void      CL_Buffer_Destroy(CL_Buffer* buf);

void  CL_Buffer_Write(CL_Context* ctx, CL_Buffer* buf, void* data, size_t size);
void  CL_Buffer_Read (CL_Context* ctx, CL_Buffer* buf, void* out,  size_t size);
// GPU-side copy — no CPU round-trip
void  CL_Buffer_Copy (CL_Context* ctx, CL_Buffer* src, CL_Buffer* dst, size_t size);
// Fill entire buffer with a repeated pattern (pattern_size bytes, e.g. 4 for float zero)
void  CL_Buffer_Fill (CL_Context* ctx, CL_Buffer* buf, const void* pattern, size_t pattern_size);
// Map/unmap for pinned buffers; returns a host pointer valid until Unmap
void* CL_Buffer_Map  (CL_Context* ctx, CL_Buffer* buf, cl_map_flags map_flags);
void  CL_Buffer_Unmap(CL_Context* ctx, CL_Buffer* buf, void* ptr);

// IMAGE  (2D texture on GPU)
CL_Image CL_Image_Create(CL_Context* ctx, int width, int height);
void     CL_Image_Destroy(CL_Image* img);

void CL_Image_Write(CL_Context* ctx, CL_Image* img, float* data);
void CL_Image_Read (CL_Context* ctx, CL_Image* img, float* out);

// KERNEL ARGS  — call these before CL_Dispatch
void CL_SetArgBuffer(CL_Pipeline* pip, int index, CL_Buffer* buf);
void CL_SetArgImage (CL_Pipeline* pip, int index, CL_Image*  img);
void CL_SetArgInt   (CL_Pipeline* pip, int index, int        val);
void CL_SetArgFloat (CL_Pipeline* pip, int index, float      val);
void CL_SetArgVec2  (CL_Pipeline* pip, int index, float2     val);
void CL_SetArgVec3  (CL_Pipeline* pip, int index, float3     val);
void CL_SetArgVec4  (CL_Pipeline* pip, int index, float4     val);

// DISPATCH
void CL_Dispatch1D(CL_Context* ctx, CL_Pipeline* pip, size_t global, size_t local);
void CL_Dispatch2D(CL_Context* ctx, CL_Pipeline* pip, size_t width, size_t height, size_t local_x, size_t local_y);
void CL_Dispatch3D(CL_Context* ctx, CL_Pipeline* pip, size_t x, size_t y, size_t z, size_t lx, size_t ly, size_t lz);

// UTILS
char*  CL_LoadFile(const char* path);
void   CL_CheckError(cl_int err, const char* label);
size_t CL_GetMaxWorkGroupSize(CL_Context* ctx);
void   CL_Finish(CL_Context* ctx);

#endif // CL_APP_H