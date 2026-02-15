#ifndef RENDER_GPU_H
#define RENDER_GPU_H

#include <stdbool.h>
#include <stdint.h>

#include "../object/format.h"
#include "../object/object.h"

typedef struct RenderGpu {
	void *clPlatform;
	void *clDevice;
	void *clContext;
	void *clQueue;
	void *clProgram;
	void *clKernel;
	void *clImage;
	void *clBuffer;
	void *clTriA;
	void *clTriB;
	void *clTriC;
	void *clTriBounds;
	void *clTriColor;
	void *hostTriA;
	void *hostTriB;
	void *hostTriC;
	void *hostTriBounds;
	void *hostTriColor;
	uint32_t *hostFramebuffer;

	uint32_t glTexture;
	int width;
	int height;
	int mode;
	int triCapacity;
	bool initialized;
} RenderGpu;

bool RenderGpu_Init(RenderGpu *gpu, int width, int height, uint32_t glTexture);
bool RenderGpu_Render(RenderGpu *gpu, float timeSeconds);
bool RenderGpu_InitBuffer(RenderGpu *gpu, int width, int height, uint32_t *hostFramebuffer);
bool RenderGpu_RenderBuffer(RenderGpu *gpu, float timeSeconds);
bool RenderGpu_RenderSceneBuffer(RenderGpu *gpu, const Object *objects, int objectCount, const Camera *camera);
void RenderGpu_Shutdown(RenderGpu *gpu);

#endif
