#include "format.h"

CL_Context CL_Context_Create() {
	CL_Context ctx = {0};
	cl_int err;

	clGetPlatformIDs(1, &ctx.platform, NULL);
	clGetDeviceIDs(ctx.platform, CL_DEVICE_TYPE_GPU, 1, &ctx.device, NULL);

	char name[128];
	clGetDeviceInfo(ctx.device, CL_DEVICE_NAME, sizeof(name), name, NULL);
	printf("[CL] device: %s\n", name);

	ctx.context = clCreateContext(NULL, 1, &ctx.device, NULL, NULL, &err);
	CL_CheckError(err, "clCreateContext");

	cl_queue_properties props[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
	ctx.queue = clCreateCommandQueueWithProperties(ctx.context, ctx.device, props, &err);
	CL_CheckError(err, "clCreateCommandQueue");

	return ctx;
}

void CL_Context_Destroy(CL_Context *ctx) {
	clReleaseCommandQueue(ctx->queue);
	clReleaseContext(ctx->context);
}

CL_Pipeline CL_Pipeline_FromSource(CL_Context *ctx, const char *src, const char *kernel_name, const char *options) {
	CL_Pipeline pip = {0};
	cl_int err;

	strncpy(pip.name, kernel_name, sizeof(pip.name) - 1);

	pip.program = clCreateProgramWithSource(ctx->context, 1, &src, NULL, &err);
	CL_CheckError(err, "clCreateProgramWithSource");

	err = clBuildProgram(pip.program, 1, &ctx->device, options, NULL, NULL);
	if (err != CL_SUCCESS) {
		size_t log_size;
		clGetProgramBuildInfo(pip.program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
		char *log = malloc(log_size);
		clGetProgramBuildInfo(pip.program, ctx->device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
		printf("[CL] build error in '%s':\n%s\n", kernel_name, log);
		free(log);
		return pip;
	}

	pip.kernel = clCreateKernel(pip.program, kernel_name, &err);
	CL_CheckError(err, "clCreateKernel");

	printf("[CL] kernel '%s' compiled ok\n", kernel_name);
	return pip;
}

CL_Pipeline CL_Pipeline_FromFile(CL_Context *ctx, const char *path, const char *kernel_name, const char *options) {
	char *src = CL_LoadFile(path);
	if (!src) {
		printf("[CL] failed to load kernel file: %s\n", path);
		return (CL_Pipeline){0};
	}
	CL_Pipeline pip = CL_Pipeline_FromSource(ctx, src, kernel_name, options);
	free(src);
	return pip;
}

void CL_Pipeline_Destroy(CL_Pipeline *pip) {
	clReleaseKernel(pip->kernel);
	clReleaseProgram(pip->program);
}

CL_Buffer CL_Buffer_Create(CL_Context *ctx, size_t size, cl_mem_flags flags) {
	cl_int err;
	CL_Buffer buf = {.size = size, .flags = flags};
	buf.buf = clCreateBuffer(ctx->context, flags, size, NULL, &err);
	CL_CheckError(err, "clCreateBuffer");
	return buf;
}

CL_Buffer CL_Buffer_CreateFromData(CL_Context *ctx, size_t size, void *data, cl_mem_flags flags) {
	cl_int err;
	CL_Buffer buf = {.size = size, .flags = flags};
	buf.buf = clCreateBuffer(ctx->context, flags | CL_MEM_COPY_HOST_PTR, size, data, &err);
	CL_CheckError(err, "clCreateBuffer (from data)");
	return buf;
}

void CL_Buffer_Destroy(CL_Buffer *buf) {
	clReleaseMemObject(buf->buf);
}

void CL_Buffer_Write(CL_Context *ctx, CL_Buffer *buf, void *data, size_t size) {
	cl_int err = clEnqueueWriteBuffer(ctx->queue, buf->buf, CL_TRUE, 0, size, data, 0, NULL, NULL);
	CL_CheckError(err, "clEnqueueWriteBuffer");
}

void CL_Buffer_Read(CL_Context *ctx, CL_Buffer *buf, void *out, size_t size) {
	cl_int err = clEnqueueReadBuffer(ctx->queue, buf->buf, CL_TRUE, 0, size, out, 0, NULL, NULL);
	CL_CheckError(err, "clEnqueueReadBuffer");
}

CL_Image CL_Image_Create(CL_Context *ctx, int width, int height) {
	CL_Image img = {.width = width, .height = height};
	cl_int err;

	cl_image_format fmt = {CL_RGBA, CL_FLOAT};
	cl_image_desc desc = {
		.image_type = CL_MEM_OBJECT_IMAGE2D,
		.image_width = width,
		.image_height = height,
	};

	img.image = clCreateImage(ctx->context, CL_MEM_READ_WRITE, &fmt, &desc, NULL, &err);
	CL_CheckError(err, "clCreateImage");
	return img;
}

void CL_Image_Destroy(CL_Image *img) {
	clReleaseMemObject(img->image);
}

void CL_Image_Write(CL_Context *ctx, CL_Image *img, float *data) {
	size_t origin[3] = {0, 0, 0};
	size_t region[3] = {img->width, img->height, 1};
	clEnqueueWriteImage(ctx->queue, img->image, CL_TRUE, origin, region, 0, 0, data, 0, NULL, NULL);
}

void CL_Image_Read(CL_Context *ctx, CL_Image *img, float *out) {
	size_t origin[3] = {0, 0, 0};
	size_t region[3] = {img->width, img->height, 1};
	clEnqueueReadImage(ctx->queue, img->image, CL_TRUE, origin, region, 0, 0, out, 0, NULL, NULL);
}

void CL_SetArgBuffer(CL_Pipeline *pip, int index, CL_Buffer *buf) {
	clSetKernelArg(pip->kernel, index, sizeof(cl_mem), &buf->buf);
}
void CL_SetArgImage(CL_Pipeline *pip, int index, CL_Image *img) {
	clSetKernelArg(pip->kernel, index, sizeof(cl_mem), &img->image);
}
void CL_SetArgInt(CL_Pipeline *pip, int index, int val) {
	clSetKernelArg(pip->kernel, index, sizeof(int), &val);
}
void CL_SetArgFloat(CL_Pipeline *pip, int index, float val) {
	clSetKernelArg(pip->kernel, index, sizeof(float), &val);
}

void CL_Dispatch1D(CL_Context *ctx, CL_Pipeline *pip, size_t global, size_t local) {
	cl_event ev;
	cl_int err = clEnqueueNDRangeKernel(ctx->queue, pip->kernel, 1, NULL, &global, &local, 0, NULL, &ev);
	CL_CheckError(err, pip->name);
	clFinish(ctx->queue);

	cl_ulong t_start, t_end;
	clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(t_start), &t_start, NULL);
	clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(t_end), &t_end, NULL);
	pip->timeTook = (float)(t_end - t_start) * 1e-6f; // ms
	clReleaseEvent(ev);
}

void CL_Dispatch2D(CL_Context *ctx, CL_Pipeline *pip, size_t width, size_t height, size_t local_x, size_t local_y) {
	size_t global[2] = {width, height};
	size_t local[2] = {local_x, local_y};
	cl_event ev;
	cl_int err = clEnqueueNDRangeKernel(ctx->queue, pip->kernel, 2, NULL, global, local, 0, NULL, &ev);
	CL_CheckError(err, pip->name);
	clFinish(ctx->queue);

	cl_ulong t_start, t_end;
	clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(t_start), &t_start, NULL);
	clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(t_end), &t_end, NULL);
	pip->timeTook = (float)(t_end - t_start) * 1e-6f; // ms
	clReleaseEvent(ev);
}

char *CL_LoadFile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	rewind(f);
	char *buf = malloc(size + 1);
	fread(buf, 1, size, f);
	buf[size] = '\0';
	fclose(f);
	return buf;
}

void CL_CheckError(cl_int err, const char *label) {
	if (err == CL_SUCCESS) return;
	printf("[CL] error %d at '%s'\n", err, label);
}