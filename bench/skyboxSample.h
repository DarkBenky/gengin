#ifndef SKYBOX_SAMPLE_BENCH_H
#define SKYBOX_SAMPLE_BENCH_H

#include <math.h>
#include <stdint.h>
#include "../object/format.h"

typedef struct {
	uint32 *front;
	uint32 *back;
	uint32 *left;
	uint32 *right;
	uint32 *top;
	uint32 *bottom;
	int imageWidth;
	int imageHeight;
} BenchSkybox;

/* V1: verbatim copy of skybox/skybox.c sampleFace + SampleSkybox */
static inline Color bFaceV1(const uint32 *face, int w, int h, float u, float v) {
	if (!face) return 0xFF101010u;
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	if (x < 0)
		x = 0;
	else if (x >= w)
		x = w - 1;
	if (y < 0)
		y = 0;
	else if (y >= h)
		y = h - 1;
	return face[y * w + x];
}

static inline Color SkyV1(const BenchSkybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	int w = skybox->imageWidth;
	int h = skybox->imageHeight;

	float ax = dir.x < 0 ? -dir.x : dir.x;
	float ay = dir.y < 0 ? -dir.y : dir.y;
	float az = dir.z < 0 ? -dir.z : dir.z;

	float u, v;

	if (ax >= ay && ax >= az) {
		if (dir.x > 0) {
			u = 0.5f + 0.5f * (-dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceV1(skybox->right, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceV1(skybox->left, w, h, u, v);
		}
	} else if (ay >= ax && ay >= az) {
		if (dir.y > 0) {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (dir.z / ay);
			return bFaceV1(skybox->top, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (-dir.z / ay);
			return bFaceV1(skybox->bottom, w, h, u, v);
		}
	} else {
		if (dir.z > 0) {
			u = 0.5f + 0.5f * (dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceV1(skybox->front, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (-dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceV1(skybox->back, w, h, u, v);
		}
	}
}

/* uv clamped in float space, so the per-axis integer clamp chain disappears */
static inline Color bFaceFloatClamp(const uint32 *face, int w, int h, float u, float v) {
	if (!face) return 0xFF101010u;
	u = fminf(fmaxf(u, 0.0f), 1.0f);
	v = fminf(fmaxf(v, 0.0f), 1.0f);
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	return face[y * w + x];
}

/* V2: V1 face chain, float-space clamp */
static inline Color SkyV2(const BenchSkybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	int w = skybox->imageWidth;
	int h = skybox->imageHeight;

	float ax = dir.x < 0 ? -dir.x : dir.x;
	float ay = dir.y < 0 ? -dir.y : dir.y;
	float az = dir.z < 0 ? -dir.z : dir.z;

	float u, v;

	if (ax >= ay && ax >= az) {
		if (dir.x > 0) {
			u = 0.5f + 0.5f * (-dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceFloatClamp(skybox->right, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceFloatClamp(skybox->left, w, h, u, v);
		}
	} else if (ay >= ax && ay >= az) {
		if (dir.y > 0) {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (dir.z / ay);
			return bFaceFloatClamp(skybox->top, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (-dir.z / ay);
			return bFaceFloatClamp(skybox->bottom, w, h, u, v);
		}
	} else {
		if (dir.z > 0) {
			u = 0.5f + 0.5f * (dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceFloatClamp(skybox->front, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (-dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceFloatClamp(skybox->back, w, h, u, v);
		}
	}
}

/* branchless major-axis pick: (axis, sign) indexes one of the six faces,
   the uv components and their signs */
typedef struct {
	int8 uAxis, uSign, vAxis, vSign, face;
} BenchFaceSel;

static const BenchFaceSel gFaceSel[6] = {
	{2, -1, 1, -1, 3}, /* +x right */
	{2, 1, 1, -1, 2},  /* -x left */
	{0, 1, 2, 1, 4},   /* +y top */
	{0, 1, 2, -1, 5},  /* -y bottom */
	{0, 1, 1, -1, 0},  /* +z front */
	{0, -1, 1, -1, 1}, /* -z back */
};

static inline Color bFaceV3(const uint32 *face, int w, int h, float u, float v) {
	if (!face) return 0xFF101010u;
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	if (x < 0)
		x = 0;
	else if (x >= w)
		x = w - 1;
	if (y < 0)
		y = 0;
	else if (y >= h)
		y = h - 1;
	return face[y * w + x];
}

static inline Color SkyV3(const BenchSkybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	const float *d = &dir.x;
	float ax = fabsf(d[0]);
	float ay = fabsf(d[1]);
	float az = fabsf(d[2]);

	int axis = 0;
	float major = ax;
	if (ay > major) {
		major = ay;
		axis = 1;
	}
	if (az > major) {
		major = az;
		axis = 2;
	}

	const BenchFaceSel *sel = &gFaceSel[axis * 2 + (d[axis] > 0.0f ? 0 : 1)];
	const uint32 *faces[6] = {skybox->front, skybox->back, skybox->left, skybox->right, skybox->top, skybox->bottom};

	float u = 0.5f + 0.5f * ((float)sel->uSign * d[sel->uAxis]) / major;
	float v = 0.5f + 0.5f * ((float)sel->vSign * d[sel->vAxis]) / major;
	return bFaceV3(faces[sel->face], skybox->imageWidth, skybox->imageHeight, u, v);
}

/* V4: branchless selection and float-space clamp */
static inline Color SkyV4(const BenchSkybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	const float *d = &dir.x;
	float ax = fabsf(d[0]);
	float ay = fabsf(d[1]);
	float az = fabsf(d[2]);

	int axis = 0;
	float major = ax;
	if (ay > major) {
		major = ay;
		axis = 1;
	}
	if (az > major) {
		major = az;
		axis = 2;
	}

	const BenchFaceSel *sel = &gFaceSel[axis * 2 + (d[axis] > 0.0f ? 0 : 1)];
	const uint32 *faces[6] = {skybox->front, skybox->back, skybox->left, skybox->right, skybox->top, skybox->bottom};

	float u = 0.5f + 0.5f * ((float)sel->uSign * d[sel->uAxis]) / major;
	float v = 0.5f + 0.5f * ((float)sel->vSign * d[sel->vAxis]) / major;
	return bFaceFloatClamp(faces[sel->face], skybox->imageWidth, skybox->imageHeight, u, v);
}

/* V5: V1 with no clamp at all - upper bound on what the clamp chain costs */
static inline Color bFaceNoClamp(const uint32 *face, int w, int h, float u, float v) {
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	return face[y * w + x];
}

static inline Color SkyV5(const BenchSkybox *skybox, const float3 dir) {
	int w = skybox->imageWidth;
	int h = skybox->imageHeight;

	float ax = fabsf(dir.x);
	float ay = fabsf(dir.y);
	float az = fabsf(dir.z);

	float u, v;

	if (ax >= ay && ax >= az) {
		if (dir.x > 0) {
			u = 0.5f + 0.5f * (-dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceNoClamp(skybox->right, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return bFaceNoClamp(skybox->left, w, h, u, v);
		}
	} else if (ay >= ax && ay >= az) {
		if (dir.y > 0) {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (dir.z / ay);
			return bFaceNoClamp(skybox->top, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (-dir.z / ay);
			return bFaceNoClamp(skybox->bottom, w, h, u, v);
		}
	} else {
		if (dir.z > 0) {
			u = 0.5f + 0.5f * (dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceNoClamp(skybox->front, w, h, u, v);
		} else {
			u = 0.5f + 0.5f * (-dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return bFaceNoClamp(skybox->back, w, h, u, v);
		}
	}
}

#endif // SKYBOX_SAMPLE_BENCH_H
