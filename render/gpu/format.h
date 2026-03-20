#ifndef CL_APP_H
#define CL_APP_H

#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    cl_mem          image;
    int             width;
    int             height;
    cl_image_format fmt;
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
void      CL_Buffer_Destroy(CL_Buffer* buf);

void CL_Buffer_Write(CL_Context* ctx, CL_Buffer* buf, void* data, size_t size);
void CL_Buffer_Read (CL_Context* ctx, CL_Buffer* buf, void* out,  size_t size);

// IMAGE  (2D texture on GPU)
CL_Image CL_Image_Create(CL_Context* ctx, int width, int height);
CL_Image CL_Image_CreateFormat(CL_Context* ctx, int width, int height,
                                cl_channel_order order, cl_channel_type type);
void     CL_Image_Destroy(CL_Image* img);

void CL_Image_Write(CL_Context* ctx, CL_Image* img, float* data);
void CL_Image_Read (CL_Context* ctx, CL_Image* img, float* out);

// KERNEL ARGS  — call these before CL_Dispatch
void CL_SetArgBuffer(CL_Pipeline* pip, int index, CL_Buffer* buf);
void CL_SetArgImage (CL_Pipeline* pip, int index, CL_Image*  img);
void CL_SetArgInt   (CL_Pipeline* pip, int index, int        val);
void CL_SetArgFloat (CL_Pipeline* pip, int index, float      val);
void CL_SetArgUInt  (CL_Pipeline* pip, int index, unsigned int val);
void CL_SetArgFloat2(CL_Pipeline* pip, int index, float x, float y);
void CL_SetArgFloat3(CL_Pipeline* pip, int index, float x, float y, float z);
void CL_SetArgFloat4(CL_Pipeline* pip, int index, float x, float y, float z, float w);
void CL_SetArgRaw   (CL_Pipeline* pip, int index, size_t size, const void* val);

// Sequential-arg macros (auto-increment index)
#define CL_ARGS_BEGIN(pip)  { int cl_arg_idx_ = 0; CL_Pipeline *cl_pip_ = (pip);
#define CL_ARG_BUFFER(buf)  CL_SetArgBuffer(cl_pip_, cl_arg_idx_++, (buf));
#define CL_ARG_IMAGE(img)   CL_SetArgImage (cl_pip_, cl_arg_idx_++, (img));
#define CL_ARG_INT(val)     CL_SetArgInt   (cl_pip_, cl_arg_idx_++, (val));
#define CL_ARG_UINT(val)    CL_SetArgUInt  (cl_pip_, cl_arg_idx_++, (val));
#define CL_ARG_FLOAT(val)   CL_SetArgFloat (cl_pip_, cl_arg_idx_++, (val));
#define CL_ARG_FLOAT2(x,y)  CL_SetArgFloat2(cl_pip_, cl_arg_idx_++, (x), (y));
#define CL_ARG_FLOAT3(x,y,z) CL_SetArgFloat3(cl_pip_, cl_arg_idx_++, (x),(y),(z));
#define CL_ARG_FLOAT4(x,y,z,w) CL_SetArgFloat4(cl_pip_, cl_arg_idx_++, (x),(y),(z),(w));
#define CL_ARG_RAW(size,ptr) CL_SetArgRaw  (cl_pip_, cl_arg_idx_++, (size), (ptr));
#define CL_ARGS_END         }

// Typed-buffer convenience macros
#define CL_BUFFER_NEW(ctx, T, count, flags) \
    CL_Buffer_Create((ctx), sizeof(T) * (count), (flags))

#define CL_BUFFER_NEW_DATA(ctx, T, data, count, flags) \
    CL_Buffer_CreateFromData((ctx), sizeof(T) * (count), (data), (flags))

#define CL_BUFFER_READ(ctx, buf, T, out, count) \
    CL_Buffer_Read((ctx), (buf), (out), sizeof(T) * (count))

#define CL_BUFFER_WRITE(ctx, buf, T, data, count) \
    CL_Buffer_Write((ctx), (buf), (data), sizeof(T) * (count))

// DISPATCH
void CL_Dispatch1D(CL_Context* ctx, CL_Pipeline* pip, size_t global, size_t local);
void CL_Dispatch2D(CL_Context* ctx, CL_Pipeline* pip, size_t width, size_t height, size_t local_x, size_t local_y);
void CL_Dispatch1D_Auto(CL_Context* ctx, CL_Pipeline* pip, size_t count);
void CL_Dispatch2D_Auto(CL_Context* ctx, CL_Pipeline* pip, size_t width, size_t height);

// BUFFER UTILS
void CL_Buffer_Copy (CL_Context* ctx, CL_Buffer* src, CL_Buffer* dst, size_t size);
void *CL_Buffer_Map  (CL_Context* ctx, CL_Buffer* buf, cl_map_flags flags);
void  CL_Buffer_Unmap(CL_Context* ctx, CL_Buffer* buf, void* mapped_ptr);

// DEVICE INFO
void CL_Context_PrintInfo(CL_Context* ctx);

// UTILS
char* CL_LoadFile(const char* path);
void CL_CheckError(cl_int err, const char* label);

#endif // CL_APP_H