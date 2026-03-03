#ifndef SSR_H
#define SSR_H

#include "../../object/format.h"
#include "../../util/threadPool.h"

// Tune these for quality vs performance
#define SSR_MAX_STEPS 96
#define SSR_STEP_SIZE 0.22f
#define SSR_MAX_DIST 22.0f
#define SSR_DEPTH_BIAS 0.02f

typedef struct {
	int row;
	int rowCount;
	Camera *camera;
} SSRTask;

// Post-process pass: reads framebuffer+depthBuffer+normalBuffer+positionBuffer+reflectBuffer,
// blends SSR hits into framebuffer. Must run after the ray trace pass is fully complete.
// tasks must point to an array of at least ceil(height/rowsPerTask) SSRTask elements.
void SSRPostProcess(Camera *camera, ThreadPool *threadPool, SSRTask *tasks, int rowsPerTask);
void SSRPostProcessSingleThreaded(Camera *camera);

#endif // SSR_H
