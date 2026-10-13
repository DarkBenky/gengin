#include "ssr.h"
#include "../../math/vector3.h"
#include <string.h>
#include <math.h>

static void SSRProcessRow(Camera *camera, int row) {
	int W = camera->screenWidth;
	int H = camera->screenHeight;

	float3 camPos = camera->position;
	float3 fwd = Float3_Normalize(camera->forward);
	float3 rgt = Float3_Normalize(camera->right);
	float3 up_ = Float3_Normalize(camera->up);
	float fovS = camera->fovScale;
	float aspect = camera->aspect;

	for (int x = 0; x < W; x++) {
		int idx = row * W + x;

		if (camera->depthBuffer[idx] >= DEPTH_FAR) continue;

		float3 worldPos = camera->positionBuffer[idx];
		float3 n = camera->normalBuffer[idx];
		float3 reflDir = camera->reflectBuffer[idx];
		float reflectivity = reflDir.w; // (1-roughness) stored by the renderer
		if (reflectivity < 0.05f) continue;

		// Fresnel scaled by surface reflectivity
		float3 toEye = Float3_Normalize(Float3_Sub(camPos, worldPos));
		float NdotV = fmaxf(0.0f, n.x * toEye.x + n.y * toEye.y + n.z * toEye.z);
		float inv = 1.0f - NdotV;
		float inv2 = inv * inv;
		float reflectStrength = reflectivity * (0.04f + 0.96f * (inv2 * inv2 * inv) + 0.5f * reflectivity);
		if (reflectStrength > 1.0f) reflectStrength = 1.0f;

		// Start march slightly above surface to avoid self-intersection
		float cx = worldPos.x + n.x * SSR_DEPTH_BIAS;
		float cy = worldPos.y + n.y * SSR_DEPTH_BIAS;
		float cz = worldPos.z + n.z * SSR_DEPTH_BIAS;

		for (int step = 0; step < SSR_MAX_STEPS; step++) {
			cx += reflDir.x * SSR_STEP_SIZE;
			cy += reflDir.y * SSR_STEP_SIZE;
			cz += reflDir.z * SSR_STEP_SIZE;

			// World to camera-relative
			float rx = cx - camPos.x;
			float ry = cy - camPos.y;
			float rz = cz - camPos.z;

			float depth = rx * fwd.x + ry * fwd.y + rz * fwd.z;
			if (depth < 0.01f) continue;

			float invD = 1.0f / (depth * fovS);
			float ndcX = (rx * rgt.x + ry * rgt.y + rz * rgt.z) * invD / aspect;
			float ndcY = (rx * up_.x + ry * up_.y + rz * up_.z) * invD;

			float sx = (ndcX + 1.0f) * 0.5f * W;
			float sy = (-ndcY + 1.0f) * 0.5f * H;

			if (sx < 1.0f || sx >= W - 1.0f || sy < 1.0f || sy >= H - 1.0f) continue;

			int px = (int)sx, py = (int)sy;
			int pidx = py * W + px;

			float sceneDepth = camera->depthBuffer[pidx];
			float threshold = SSR_STEP_SIZE * 1.5f + depth * 0.001f;
			float diff = depth - sceneDepth;

			if (sceneDepth > 0.01f && diff > 0.0f && diff < threshold) {
				// Bilinear fetch from the stable pre-pass copy in tempFramebufferHdr
				int x1 = px + 1 < W ? px + 1 : px;
				int y1 = py + 1 < H ? py + 1 : py;
				float fx = sx - px, fy = sy - py;

				float3 c00 = camera->tempFramebufferHdr[py * W + px];
				float3 c10 = camera->tempFramebufferHdr[py * W + x1];
				float3 c01 = camera->tempFramebufferHdr[y1 * W + px];
				float3 c11 = camera->tempFramebufferHdr[y1 * W + x1];
				float3 top = {c00.x + (c10.x - c00.x) * fx, c00.y + (c10.y - c00.y) * fx, c00.z + (c10.z - c00.z) * fx};
				float3 bot = {c01.x + (c11.x - c01.x) * fx, c01.y + (c11.y - c01.y) * fx, c01.z + (c11.z - c01.z) * fx};
				float3 sr = {top.x + (bot.x - top.x) * fy, top.y + (bot.y - top.y) * fy, top.z + (bot.z - top.z) * fy};

				if (sr.x + sr.y + sr.z <= (3.0f / 255.0f)) break; // black hit — no useful data

				float st = fminf(reflectStrength, 1.0f);
				float sit = 1.0f - st;

				float3 base = camera->tempFramebufferHdr[idx];
				camera->framebufferHdr[idx] = (float3){
					base.x * sit + sr.x * st,
					base.y * sit + sr.y * st,
					base.z * sit + sr.z * st};
				break;
			}
		}
	}
}

static void SSRRowTask(void *arg) {
	SSRTask *task = arg;
	for (int r = task->row; r < task->row + task->rowCount && r < task->camera->screenHeight; r++) {
		SSRProcessRow(task->camera, r);
	}
}

void SSRPostProcessSingleThreaded(Camera *camera) {
	if (!camera) return;
	int size = camera->screenWidth * camera->screenHeight;
	memcpy(camera->tempFramebufferHdr, camera->framebufferHdr, size * sizeof(float3));
	for (int row = 0; row < camera->screenHeight; row++)
		SSRProcessRow(camera, row);
}

void SSRPostProcess(Camera *camera, ThreadPool *threadPool, SSRTask *tasks, int rowsPerTask) {
	if (!camera || !threadPool || !tasks) return;
	int size = camera->screenWidth * camera->screenHeight;
	int taskCount = (camera->screenHeight + rowsPerTask - 1) / rowsPerTask;
	// Snapshot must happen before any task reads tempFramebufferHdr
	memcpy(camera->tempFramebufferHdr, camera->framebufferHdr, size * sizeof(float3));
	for (int t = 0; t < taskCount; t++) {
		tasks[t].row = t * rowsPerTask;
		tasks[t].rowCount = rowsPerTask;
		tasks[t].camera = camera;
		poolAdd(threadPool, SSRRowTask, &tasks[t]);
	}
	poolWait(threadPool);
}
