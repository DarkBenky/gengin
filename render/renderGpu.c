#include "renderGpu.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<CL/cl.h>)
#define RENDER_GPU_HAS_CL 1
#include <CL/cl.h>
#endif
#if __has_include(<CL/cl_gl.h>) && __has_include(<GL/gl.h>) && __has_include(<GL/glx.h>)
#define RENDER_GPU_HAS_GL_INTEROP 1
#include <CL/cl_gl.h>
#include <GL/gl.h>
#include <GL/glx.h>
#endif
#endif

#ifndef RENDER_GPU_HAS_CL
#define RENDER_GPU_HAS_CL 0
#endif

#ifndef RENDER_GPU_HAS_GL_INTEROP
#define RENDER_GPU_HAS_GL_INTEROP 0
#endif

#if RENDER_GPU_HAS_CL

static const char *kRenderKernelSrc =
	"__kernel void renderToTexture(write_only image2d_t outTex, float t) {                 \n"
	"    const int2 id = (int2)(get_global_id(0), get_global_id(1));                       \n"
	"    const int w = get_image_width(outTex);                                             \n"
	"    const int h = get_image_height(outTex);                                            \n"
	"    const float2 uv = (float2)(id.x / (float)w, id.y / (float)h);                     \n"
	"    const float r = 0.5f + 0.5f * sin(6.28318f * (uv.x + t * 0.15f));                 \n"
	"    const float g = 0.5f + 0.5f * sin(6.28318f * (uv.y + t * 0.20f));                 \n"
	"    const float b = 0.5f + 0.5f * sin(6.28318f * (uv.x + uv.y + t * 0.10f));          \n"
	"    write_imagef(outTex, id, (float4)(r, g, b, 1.0f));                                \n"
	"}                                                                                      \n"
	"__kernel void renderToBuffer(__global uint* outBuf, float t, int width, int height) { \n"
	"    const int gid = get_global_id(0);                                                  \n"
	"    const int x = gid % width;                                                         \n"
	"    const int y = gid / width;                                                         \n"
	"    if (x >= width || y >= height) return;                                             \n"
	"    const float2 uv = (float2)(x / (float)width, y / (float)height);                  \n"
	"    const uint r = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.x + t * 0.15f))));\n"
	"    const uint g = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.y + t * 0.20f))));\n"
	"    const uint b = (uint)(255.0f * (0.5f + 0.5f * sin(6.28318f * (uv.x + uv.y + t * 0.10f))));\n"
	"    outBuf[gid] = 0xFF000000u | (r << 16) | (g << 8) | b;                             \n"
	"}                                                                                      \n"
	"float edgef(float ax, float ay, float bx, float by, float cx, float cy) {             \n"
	"    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);                              \n"
	"}                                                                                      \n"
	"__kernel void renderSceneBuffer(__global uint* outBuf,                                 \n"
	"                                __global float4* triA,                                  \n"
	"                                __global float4* triB,                                  \n"
	"                                __global float4* triC,                                  \n"
	"                                __global int4* triBounds,                               \n"
	"                                __global uint* triColor,                                \n"
	"                                int triCount, int width, int height) {                  \n"
	"    const int gid = get_global_id(0);                                                   \n"
	"    const int x = gid % width;                                                          \n"
	"    const int y = gid / width;                                                          \n"
	"    if (x >= width || y >= height) return;                                              \n"
	"    float bestDepth = 3.402823466e+38f;                                                 \n"
	"    uint bestColor = 0xFF000000u;                                                       \n"
	"    const float px = (float)x + 0.5f;                                                   \n"
	"    const float py = (float)y + 0.5f;                                                   \n"
	"    for (int i = 0; i < triCount; i++) {                                                \n"
	"        int4 b = triBounds[i];                                                          \n"
	"        if (x < b.x || x > b.y || y < b.z || y > b.w) continue;                         \n"
	"        float4 a = triA[i];                                                             \n"
	"        float4 bb = triB[i];                                                            \n"
	"        float4 c = triC[i];                                                             \n"
	"        float w0 = edgef(a.z, a.w, bb.x, bb.y, px, py);                                 \n"
	"        float w1 = edgef(bb.x, bb.y, a.x, a.y, px, py);                                 \n"
	"        float w2 = edgef(a.x, a.y, a.z, a.w, px, py);                                   \n"
	"        if ((w0 * c.y) >= 0.0f && (w1 * c.y) >= 0.0f && (w2 * c.y) >= 0.0f) {          \n"
	"            float invZ = (w0 * bb.z + w1 * bb.w + w2 * c.x) * c.z;                     \n"
	"            float depth = 1.0f / invZ;                                                  \n"
	"            if (depth < bestDepth) {                                                    \n"
	"                bestDepth = depth;                                                      \n"
	"                bestColor = triColor[i];                                                \n"
	"            }                                                                            \n"
	"        }                                                                                \n"
	"    }                                                                                    \n"
	"    outBuf[gid] = bestColor;                                                             \n"
	"}                                                                                      \n";

typedef struct Float4Host {
	float x;
	float y;
	float z;
	float w;
} Float4Host;

typedef struct Int4Host {
	int x;
	int y;
	int z;
	int w;
} Int4Host;

static bool ClOk(cl_int err) {
	return err == CL_SUCCESS;
}

static inline float3 Float3_Sub(float3 a, float3 b) {
	return (float3){a.x - b.x, a.y - b.y, a.z - b.z};
}
static inline float3 Float3_Add(float3 a, float3 b) {
	return (float3){a.x + b.x, a.y + b.y, a.z + b.z};
}
static inline float3 Float3_Scale(float3 v, float s) {
	return (float3){v.x * s, v.y * s, v.z * s};
}
static inline float Float3_Dot(float3 a, float3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline float3 Float3_Cross(float3 a, float3 b) {
	return (float3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline float3 Float3_Normalize(float3 v) {
	float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len > 0.0f) {
		float invLen = 1.0f / len;
		return Float3_Scale(v, invLen);
	}
	return v;
}
static inline int MinI(int a, int b) {
	return a < b ? a : b;
}
static inline int MaxI(int a, int b) {
	return a > b ? a : b;
}
static inline float MinF(float a, float b) {
	return a < b ? a : b;
}
static inline float MaxF(float a, float b) {
	return a > b ? a : b;
}
static inline float EdgeFunction(float ax, float ay, float bx, float by, float cx, float cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}
static inline float3 RotateX(float3 v, float angle) {
	float c = cosf(angle), s = sinf(angle);
	return (float3){v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}
static inline float3 RotateY(float3 v, float angle) {
	float c = cosf(angle), s = sinf(angle);
	return (float3){v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
static inline float3 RotateZ(float3 v, float angle) {
	float c = cosf(angle), s = sinf(angle);
	return (float3){v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}
static inline float3 RotateXYZ(float3 v, float3 r) {
	return RotateZ(RotateY(RotateX(v, r.x), r.y), r.z);
}

static bool EnsureTriCapacity(RenderGpu *gpu, int triCapacity) {
	if (triCapacity <= gpu->triCapacity && gpu->clTriA && gpu->hostTriA) return true;

	if (gpu->clTriA) clReleaseMemObject((cl_mem)gpu->clTriA);
	if (gpu->clTriB) clReleaseMemObject((cl_mem)gpu->clTriB);
	if (gpu->clTriC) clReleaseMemObject((cl_mem)gpu->clTriC);
	if (gpu->clTriBounds) clReleaseMemObject((cl_mem)gpu->clTriBounds);
	if (gpu->clTriColor) clReleaseMemObject((cl_mem)gpu->clTriColor);
	free(gpu->hostTriA);
	free(gpu->hostTriB);
	free(gpu->hostTriC);
	free(gpu->hostTriBounds);
	free(gpu->hostTriColor);

	cl_context context = (cl_context)gpu->clContext;
	cl_int err = CL_SUCCESS;
	size_t n = (size_t)triCapacity;

	gpu->clTriA = clCreateBuffer(context, CL_MEM_READ_ONLY, n * sizeof(Float4Host), NULL, &err);
	if (!ClOk(err)) return false;
	gpu->clTriB = clCreateBuffer(context, CL_MEM_READ_ONLY, n * sizeof(Float4Host), NULL, &err);
	if (!ClOk(err)) return false;
	gpu->clTriC = clCreateBuffer(context, CL_MEM_READ_ONLY, n * sizeof(Float4Host), NULL, &err);
	if (!ClOk(err)) return false;
	gpu->clTriBounds = clCreateBuffer(context, CL_MEM_READ_ONLY, n * sizeof(Int4Host), NULL, &err);
	if (!ClOk(err)) return false;
	gpu->clTriColor = clCreateBuffer(context, CL_MEM_READ_ONLY, n * sizeof(uint32_t), NULL, &err);
	if (!ClOk(err)) return false;

	gpu->hostTriA = malloc(n * sizeof(Float4Host));
	gpu->hostTriB = malloc(n * sizeof(Float4Host));
	gpu->hostTriC = malloc(n * sizeof(Float4Host));
	gpu->hostTriBounds = malloc(n * sizeof(Int4Host));
	gpu->hostTriColor = malloc(n * sizeof(uint32_t));
	if (!gpu->hostTriA || !gpu->hostTriB || !gpu->hostTriC || !gpu->hostTriBounds || !gpu->hostTriColor) return false;

	gpu->triCapacity = triCapacity;
	return true;
}

static cl_command_queue CreateQueue(cl_context context, cl_device_id device, cl_int *err) {
#if defined(CL_VERSION_2_0)
	const cl_queue_properties props[] = {0};
	return clCreateCommandQueueWithProperties(context, device, props, err);
#else
	return clCreateCommandQueue(context, device, 0, err);
#endif
}

static bool InitBase(cl_platform_id *platformOut, cl_device_id *deviceOut,
					 cl_context *contextOut, cl_command_queue *queueOut) {
	cl_int err = CL_SUCCESS;
	cl_platform_id platform = NULL;
	err = clGetPlatformIDs(1, &platform, NULL);
	if (!ClOk(err) || !platform) return false;

	cl_device_id device = NULL;
	err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
	if (!ClOk(err) || !device) return false;

	cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
	if (!ClOk(err) || !context) return false;

	cl_command_queue queue = CreateQueue(context, device, &err);
	if (!ClOk(err) || !queue) {
		clReleaseContext(context);
		return false;
	}

	*platformOut = platform;
	*deviceOut = device;
	*contextOut = context;
	*queueOut = queue;
	return true;
}

bool RenderGpu_Init(RenderGpu *gpu, int width, int height, uint32_t glTexture) {
	if (!gpu || width <= 0 || height <= 0 || glTexture == 0) return false;
	memset(gpu, 0, sizeof(*gpu));

#if !RENDER_GPU_HAS_GL_INTEROP
	(void)glTexture;
	return false;
#else
	if (!gpu || width <= 0 || height <= 0 || glTexture == 0) return false;
	memset(gpu, 0, sizeof(*gpu));

	cl_int err = CL_SUCCESS;

	cl_platform_id platform = NULL;
	err = clGetPlatformIDs(1, &platform, NULL);
	if (!ClOk(err) || !platform) return false;

	cl_device_id device = NULL;
	err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
	if (!ClOk(err) || !device) return false;

	GLXContext glxContext = glXGetCurrentContext();
	Display *xDisplay = glXGetCurrentDisplay();
	if (!glxContext || !xDisplay) return false;

	cl_context_properties props[] = {
		CL_GL_CONTEXT_KHR,
		(cl_context_properties)glxContext,
		CL_GLX_DISPLAY_KHR,
		(cl_context_properties)xDisplay,
		CL_CONTEXT_PLATFORM,
		(cl_context_properties)platform,
		0};

	cl_context context = clCreateContext(props, 1, &device, NULL, NULL, &err);
	if (!ClOk(err) || !context) return false;

	cl_command_queue queue = CreateQueue(context, device, &err);
	if (!ClOk(err) || !queue) {
		clReleaseContext(context);
		return false;
	}

	cl_program program = clCreateProgramWithSource(context, 1, &kRenderKernelSrc, NULL, &err);
	if (!ClOk(err) || !program) {
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
	if (!ClOk(err)) {
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	cl_kernel kernel = clCreateKernel(program, "renderToTexture", &err);
	if (!ClOk(err) || !kernel) {
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	cl_mem image = clCreateFromGLTexture(context, CL_MEM_WRITE_ONLY, GL_TEXTURE_2D, 0, glTexture, &err);
	if (!ClOk(err) || !image) {
		clReleaseKernel(kernel);
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	gpu->clPlatform = platform;
	gpu->clDevice = device;
	gpu->clContext = context;
	gpu->clQueue = queue;
	gpu->clProgram = program;
	gpu->clKernel = kernel;
	gpu->clImage = image;
	gpu->glTexture = glTexture;
	gpu->width = width;
	gpu->height = height;
	gpu->mode = 1;
	gpu->initialized = true;

	return true;
#endif
}

bool RenderGpu_Render(RenderGpu *gpu, float timeSeconds) {
	if (!gpu || !gpu->initialized || gpu->mode != 1) return false;

#if !RENDER_GPU_HAS_GL_INTEROP
	(void)timeSeconds;
	return false;
#else

	cl_int err = CL_SUCCESS;
	cl_command_queue queue = (cl_command_queue)gpu->clQueue;
	cl_kernel kernel = (cl_kernel)gpu->clKernel;
	cl_mem image = (cl_mem)gpu->clImage;

	err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &image);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 1, sizeof(float), &timeSeconds);
	if (!ClOk(err)) return false;

	err = clEnqueueAcquireGLObjects(queue, 1, &image, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	size_t globalSize[2] = {(size_t)gpu->width, (size_t)gpu->height};
	err = clEnqueueNDRangeKernel(queue, kernel, 2, NULL, globalSize, NULL, 0, NULL, NULL);
	if (!ClOk(err)) {
		clEnqueueReleaseGLObjects(queue, 1, &image, 0, NULL, NULL);
		return false;
	}

	err = clEnqueueReleaseGLObjects(queue, 1, &image, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	err = clFinish(queue);
	return ClOk(err);
#endif
}

bool RenderGpu_InitBuffer(RenderGpu *gpu, int width, int height, uint32_t *hostFramebuffer) {
	if (!gpu || width <= 0 || height <= 0 || !hostFramebuffer) return false;
	memset(gpu, 0, sizeof(*gpu));

	cl_platform_id platform = NULL;
	cl_device_id device = NULL;
	cl_context context = NULL;
	cl_command_queue queue = NULL;
	if (!InitBase(&platform, &device, &context, &queue)) return false;

	cl_int err = CL_SUCCESS;
	cl_program program = clCreateProgramWithSource(context, 1, &kRenderKernelSrc, NULL, &err);
	if (!ClOk(err) || !program) {
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
	if (!ClOk(err)) {
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	cl_kernel kernel = clCreateKernel(program, "renderSceneBuffer", &err);
	if (!ClOk(err) || !kernel) {
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
	cl_mem buffer = clCreateBuffer(context, CL_MEM_WRITE_ONLY, bytes, NULL, &err);
	if (!ClOk(err) || !buffer) {
		clReleaseKernel(kernel);
		clReleaseProgram(program);
		clReleaseCommandQueue(queue);
		clReleaseContext(context);
		return false;
	}

	gpu->clPlatform = platform;
	gpu->clDevice = device;
	gpu->clContext = context;
	gpu->clQueue = queue;
	gpu->clProgram = program;
	gpu->clKernel = kernel;
	gpu->clBuffer = buffer;
	gpu->hostFramebuffer = hostFramebuffer;
	gpu->width = width;
	gpu->height = height;
	gpu->mode = 2;
	gpu->triCapacity = 0;
	gpu->initialized = true;
	return true;
}

bool RenderGpu_RenderBuffer(RenderGpu *gpu, float timeSeconds) {
	if (!gpu || !gpu->initialized || gpu->mode != 2) return false;

	cl_int err = CL_SUCCESS;
	cl_command_queue queue = (cl_command_queue)gpu->clQueue;
	cl_kernel kernel = (cl_kernel)gpu->clKernel;
	cl_mem buffer = (cl_mem)gpu->clBuffer;
	int width = gpu->width;
	int height = gpu->height;

	err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &buffer);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 1, sizeof(float), &timeSeconds);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 2, sizeof(int), &width);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 3, sizeof(int), &height);
	if (!ClOk(err)) return false;

	size_t globalSize = (size_t)width * (size_t)height;
	err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
	err = clEnqueueReadBuffer(queue, buffer, CL_TRUE, 0, bytes, gpu->hostFramebuffer, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	err = clFinish(queue);
	return ClOk(err);
}

bool RenderGpu_RenderSceneBuffer(RenderGpu *gpu, const Object *objects, int objectCount, const Camera *camera) {
	if (!gpu || !gpu->initialized || gpu->mode != 2 || !objects || !camera || objectCount <= 0) return false;

	int totalTriCount = 0;
	for (int i = 0; i < objectCount; i++)
		totalTriCount += objects[i].triangleCount;
	if (totalTriCount <= 0) return false;

	if (!EnsureTriCapacity(gpu, totalTriCount)) return false;

	Float4Host *triA = (Float4Host *)gpu->hostTriA;
	Float4Host *triB = (Float4Host *)gpu->hostTriB;
	Float4Host *triC = (Float4Host *)gpu->hostTriC;
	Int4Host *triBounds = (Int4Host *)gpu->hostTriBounds;
	uint32_t *triColor = (uint32_t *)gpu->hostTriColor;

	float3 right = Float3_Normalize(Float3_Cross((float3){0, 1, 0}, camera->forward));
	float3 up = Float3_Cross(camera->forward, right);
	float aspect = (float)camera->screenWidth / (float)camera->screenHeight;
	float fovScale = tanf(camera->fov * 0.5f * 3.14159265f / 180.0f);

	int triCount = 0;
	for (int oi = 0; oi < objectCount; oi++) {
		const Object *obj = &objects[oi];
		for (int ti = 0; ti < obj->triangleCount; ti++) {
			Triangle tri = obj->triangles[ti];
			float3 v0 = RotateXYZ(tri.v1, obj->rotation);
			float3 v1 = RotateXYZ(tri.v2, obj->rotation);
			float3 v2 = RotateXYZ(tri.v3, obj->rotation);
			float3 normal = Float3_Normalize(RotateXYZ(tri.normal, obj->rotation));

			v0 = (float3){v0.x * obj->scale.x, v0.y * obj->scale.y, v0.z * obj->scale.z};
			v1 = (float3){v1.x * obj->scale.x, v1.y * obj->scale.y, v1.z * obj->scale.z};
			v2 = (float3){v2.x * obj->scale.x, v2.y * obj->scale.y, v2.z * obj->scale.z};
			v0 = Float3_Add(v0, obj->position);
			v1 = Float3_Add(v1, obj->position);
			v2 = Float3_Add(v2, obj->position);

			float3 toCamera = Float3_Sub(camera->position, v0);
			if (Float3_Dot(normal, toCamera) <= 0.0f) continue;

			float3 v0Cam = Float3_Sub(v0, camera->position);
			float3 v1Cam = Float3_Sub(v1, camera->position);
			float3 v2Cam = Float3_Sub(v2, camera->position);
			float z0 = Float3_Dot(v0Cam, camera->forward);
			float z1 = Float3_Dot(v1Cam, camera->forward);
			float z2 = Float3_Dot(v2Cam, camera->forward);
			if (z0 <= 0.01f && z1 <= 0.01f && z2 <= 0.01f) continue;

			float x0 = Float3_Dot(v0Cam, right) / (z0 * fovScale * aspect);
			float y0 = Float3_Dot(v0Cam, up) / (z0 * fovScale);
			float x1 = Float3_Dot(v1Cam, right) / (z1 * fovScale * aspect);
			float y1 = Float3_Dot(v1Cam, up) / (z1 * fovScale);
			float x2 = Float3_Dot(v2Cam, right) / (z2 * fovScale * aspect);
			float y2 = Float3_Dot(v2Cam, up) / (z2 * fovScale);

			float sx0 = (x0 + 1.0f) * 0.5f * camera->screenWidth;
			float sy0 = (1.0f - y0) * 0.5f * camera->screenHeight;
			float sx1 = (x1 + 1.0f) * 0.5f * camera->screenWidth;
			float sy1 = (1.0f - y1) * 0.5f * camera->screenHeight;
			float sx2 = (x2 + 1.0f) * 0.5f * camera->screenWidth;
			float sy2 = (1.0f - y2) * 0.5f * camera->screenHeight;

			int minX = MaxI(0, (int)MinF(MinF(sx0, sx1), sx2));
			int maxX = MinI(camera->screenWidth - 1, (int)MaxF(MaxF(sx0, sx1), sx2));
			int minY = MaxI(0, (int)MinF(MinF(sy0, sy1), sy2));
			int maxY = MinI(camera->screenHeight - 1, (int)MaxF(MaxF(sy0, sy1), sy2));
			if (minX > maxX || minY > maxY) continue;

			float area = EdgeFunction(sx0, sy0, sx1, sy1, sx2, sy2);
			if (fabsf(area) <= 1e-8f) continue;

			float3 lightDir = Float3_Normalize((float3){0.5f, 0.7f, -0.5f});
			float3 viewDir = Float3_Normalize(Float3_Scale(camera->forward, -1.0f));
			float NdotL = MaxF(0.0f, Float3_Dot(normal, lightDir));
			float3 halfVec = Float3_Normalize(Float3_Add(lightDir, viewDir));
			float NdotH = MaxF(0.0f, Float3_Dot(normal, halfVec));
			float roughness = tri.Roughness;
			float shininess = (1.0f - roughness) * 128.0f + 1.0f;
			float spec = powf(NdotH, shininess);
			float metallic = tri.Metallic;
			float3 baseColor = tri.color;
			float3 diffuse = Float3_Scale(baseColor, (1.0f - metallic) * NdotL);
			float3 specColor = Float3_Scale(baseColor, metallic);
			specColor = Float3_Add(specColor, Float3_Scale((float3){1, 1, 1}, 1.0f - metallic));
			float3 specular = Float3_Scale(specColor, spec * (1.0f - roughness * 0.7f));
			float3 ambient = Float3_Scale(baseColor, 0.1f);
			float3 finalColor = Float3_Add(Float3_Add(ambient, diffuse), specular);
			finalColor.x = MinF(1.0f, finalColor.x);
			finalColor.y = MinF(1.0f, finalColor.y);
			finalColor.z = MinF(1.0f, finalColor.z);

			triA[triCount] = (Float4Host){sx0, sy0, sx1, sy1};
			triB[triCount] = (Float4Host){sx2, sy2, 1.0f / z0, 1.0f / z1};
			triC[triCount] = (Float4Host){1.0f / z2, area > 0.0f ? 1.0f : -1.0f, 1.0f / area, 0.0f};
			triBounds[triCount] = (Int4Host){minX, maxX, minY, maxY};
			uint8_t r = (uint8_t)(finalColor.x * 255.0f);
			uint8_t g = (uint8_t)(finalColor.y * 255.0f);
			uint8_t b = (uint8_t)(finalColor.z * 255.0f);
			triColor[triCount] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
			triCount++;
		}
	}

	cl_command_queue queue = (cl_command_queue)gpu->clQueue;
	cl_int err = CL_SUCCESS;
	err = clEnqueueWriteBuffer(queue, (cl_mem)gpu->clTriA, CL_FALSE, 0, (size_t)triCount * sizeof(Float4Host), triA, 0, NULL, NULL);
	if (!ClOk(err)) return false;
	err = clEnqueueWriteBuffer(queue, (cl_mem)gpu->clTriB, CL_FALSE, 0, (size_t)triCount * sizeof(Float4Host), triB, 0, NULL, NULL);
	if (!ClOk(err)) return false;
	err = clEnqueueWriteBuffer(queue, (cl_mem)gpu->clTriC, CL_FALSE, 0, (size_t)triCount * sizeof(Float4Host), triC, 0, NULL, NULL);
	if (!ClOk(err)) return false;
	err = clEnqueueWriteBuffer(queue, (cl_mem)gpu->clTriBounds, CL_FALSE, 0, (size_t)triCount * sizeof(Int4Host), triBounds, 0, NULL, NULL);
	if (!ClOk(err)) return false;
	err = clEnqueueWriteBuffer(queue, (cl_mem)gpu->clTriColor, CL_FALSE, 0, (size_t)triCount * sizeof(uint32_t), triColor, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	cl_kernel kernel = (cl_kernel)gpu->clKernel;
	cl_mem outBuf = (cl_mem)gpu->clBuffer;
	cl_mem clA = (cl_mem)gpu->clTriA;
	cl_mem clB = (cl_mem)gpu->clTriB;
	cl_mem clC = (cl_mem)gpu->clTriC;
	cl_mem clBounds = (cl_mem)gpu->clTriBounds;
	cl_mem clColor = (cl_mem)gpu->clTriColor;
	int width = gpu->width;
	int height = gpu->height;

	err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &outBuf);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 1, sizeof(cl_mem), &clA);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 2, sizeof(cl_mem), &clB);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 3, sizeof(cl_mem), &clC);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 4, sizeof(cl_mem), &clBounds);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 5, sizeof(cl_mem), &clColor);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 6, sizeof(int), &triCount);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 7, sizeof(int), &width);
	if (!ClOk(err)) return false;
	err = clSetKernelArg(kernel, 8, sizeof(int), &height);
	if (!ClOk(err)) return false;

	size_t globalSize = (size_t)width * (size_t)height;
	err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
	err = clEnqueueReadBuffer(queue, outBuf, CL_TRUE, 0, bytes, gpu->hostFramebuffer, 0, NULL, NULL);
	if (!ClOk(err)) return false;

	err = clFinish(queue);
	return ClOk(err);
}

void RenderGpu_Shutdown(RenderGpu *gpu) {
	if (!gpu) return;

	if (gpu->clImage) clReleaseMemObject((cl_mem)gpu->clImage);
	if (gpu->clBuffer) clReleaseMemObject((cl_mem)gpu->clBuffer);
	if (gpu->clTriA) clReleaseMemObject((cl_mem)gpu->clTriA);
	if (gpu->clTriB) clReleaseMemObject((cl_mem)gpu->clTriB);
	if (gpu->clTriC) clReleaseMemObject((cl_mem)gpu->clTriC);
	if (gpu->clTriBounds) clReleaseMemObject((cl_mem)gpu->clTriBounds);
	if (gpu->clTriColor) clReleaseMemObject((cl_mem)gpu->clTriColor);
	if (gpu->clKernel) clReleaseKernel((cl_kernel)gpu->clKernel);
	if (gpu->clProgram) clReleaseProgram((cl_program)gpu->clProgram);
	if (gpu->clQueue) clReleaseCommandQueue((cl_command_queue)gpu->clQueue);
	if (gpu->clContext) clReleaseContext((cl_context)gpu->clContext);

	free(gpu->hostTriA);
	free(gpu->hostTriB);
	free(gpu->hostTriC);
	free(gpu->hostTriBounds);
	free(gpu->hostTriColor);

	memset(gpu, 0, sizeof(*gpu));
}

#else

bool RenderGpu_Init(RenderGpu *gpu, int width, int height, uint32_t glTexture) {
	(void)width;
	(void)height;
	(void)glTexture;
	if (!gpu) return false;
	memset(gpu, 0, sizeof(*gpu));
	return false;
}

bool RenderGpu_Render(RenderGpu *gpu, float timeSeconds) {
	(void)gpu;
	(void)timeSeconds;
	return false;
}

bool RenderGpu_InitBuffer(RenderGpu *gpu, int width, int height, uint32_t *hostFramebuffer) {
	(void)width;
	(void)height;
	(void)hostFramebuffer;
	if (!gpu) return false;
	memset(gpu, 0, sizeof(*gpu));
	return false;
}

bool RenderGpu_RenderBuffer(RenderGpu *gpu, float timeSeconds) {
	(void)gpu;
	(void)timeSeconds;
	return false;
}

bool RenderGpu_RenderSceneBuffer(RenderGpu *gpu, const Object *objects, int objectCount, const Camera *camera) {
	(void)gpu;
	(void)objects;
	(void)objectCount;
	(void)camera;
	return false;
}

void RenderGpu_Shutdown(RenderGpu *gpu) {
	if (!gpu) return;
	memset(gpu, 0, sizeof(*gpu));
}

#endif
