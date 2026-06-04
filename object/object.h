#ifndef OBJECT_H
#define OBJECT_H

#include <math.h>
#include <immintrin.h>

#define EMISSION_RESOLUTION 32

#include "format.h"
#include "../load/loadObj.h"
#include "material/material.h"
#include "../render/gpu/format.h"

typedef struct {
    float tMin[4];
    float tMax[4];
} RayBoxResult4;

typedef enum VolumeType {
	VOLUME_NONE,
	VOLUME_CLOUD, // for rendering of cloads
} VolumeType;

typedef struct Volume {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	float3 worldBBmin;
	float3 worldBBmax;

	// cached inverse transform — recomputed in Object_UpdateWorldBounds
	float3 _invScale;  // row 0 of M = Diag(invScale)*InvRot
	float3 _invRotSin; // row 1 of M
	float3 _invRotCos; // row 2 of M
	// cached forward rotation matrix Rz*Ry*Rx — for rotating normals without trig each hit
	float3 _fwdRot0;
	float3 _fwdRot1;
	float3 _fwdRot2;

	VolumeType type; // determines how to sample the volume on gpu

	float xResolution;
	float yResolution;
	float zResolution;

	float *density; // for clouds: density[x + y*xRes + z*xRes*yRes]

	// GPU
	CL_Buffer gpuDensity; // for clouds: density[x + y*xRes + z*xRes*yRes]
} Volume;

typedef struct BVHNode {
	// Internal nodes: SoA bounds of both children.
	// Per axis layout: {child0.mn, child0.mx, child1.mn, child1.mx}
	// soa[0..3] = x-axis, soa[4..7] = y-axis, soa[8..11] = z-axis
	float soa[12] __attribute__((aligned(16))); // 48 bytes
	union {
		int leftFirst; // internal: left child index
		int triStart;  // leaf: start in triIndices
	};
	int triCount;  // 0 = internal, >0 = leaf
	int _pad[2];   // pad to 64 bytes
} BVHNode;        // 64 bytes — 1 per cache line

typedef struct BVH {
	BVHNode *nodes;
	int *triIndices; // reordered triangle indices
	int nodeCount;
} BVH;
typedef struct EmissionMap {
	float3 emissionMap[EMISSION_RESOLUTION][EMISSION_RESOLUTION]; // precomputed per-face emission for a 32x32 grid of world positions (for direct lighting)
} EmissionMap;

typedef struct UvCords {
	uint16 uv1x;
	uint16 uv1y;
	uint16 uv2x;
	uint16 uv2y;
	uint16 uv3x;
	uint16 uv3y;
} UvCords;

typedef struct Object {
	float3 position;
	float3 rotation;
	float3 scale;
	float3 BBmin;
	float3 BBmax;
	float3 worldBBmin;
	float3 worldBBmax;

	// cached inverse transform — recomputed in Object_UpdateWorldBounds
	float3 _invScale;  // row 0 of M = Diag(invScale)*InvRot
	float3 _invRotSin; // row 1 of M
	float3 _invRotCos; // row 2 of M
	// cached forward rotation matrix Rz*Ry*Rx — for rotating normals without trig each hit
	float3 _fwdRot0;
	float3 _fwdRot1;
	float3 _fwdRot2;

	Color _temp; // pre-packed RGB color for BBOX hits

	float3 *v1;
	float3 *v2;
	float3 *v3;
	float3 *normals;
	UvCords *uvs;

	int *materialIds;
	int triangleCount;

	bool hasTexture;
	bool hasEmission; // quick check to skip emission sampling when no faces emit
	EmissionMap frontFaceEmission;
	EmissionMap backFaceEmission;
	EmissionMap leftFaceEmission;
	EmissionMap rightFaceEmission;
	EmissionMap topFaceEmission;
	EmissionMap bottomFaceEmission;

	BVH bvh;
} Object;

void LoadVolume(Volume *vol, const char *filename, float3 position, float3 rotation, float3 scale, VolumeType type);
void UploadVolumeToGpu(Volume *vol, CL_Context *ctx);

// calculate per-face emission maps with orthographic projection
void CalculateFaceEmissions(Object *obj, MaterialLib *lib);
// trace ray in direction of query object if we hit something before query object return 0 else return emission from query object
float3 SampleEmission(const Object *objs, int objCount, float3 position, float3 direction, int queryObject, MaterialLib *lib);

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename, MaterialLib *lib);
void Object_Destroy(Object *obj);
void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color, MaterialLib *lib, float emission, float roughness, float metallic);
void Object_UpdateWorldBounds(Object *obj);
void RayBoxItersect(const Object *obj, float3 rayOrigin, float3 rayDir, float *tMin, float *tMax);
RayBoxResult4 RayBoxIntersectV4(const Object *obj0, const Object *obj1,const Object *obj2, const Object *obj3,float3 rayOrigin, float3 rayDir);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
Color IntersectBBoxColor(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);
bool ObjectBehindCamera(const Object *obj, float3 camPos, float3 camForward);

// Set material properties on every triangle of an object (updates the shared MaterialLib entries).
void Object_SetMaterial(Object *obj, MaterialLib *lib, Material mat);
void Object_SetColor(Object *obj, MaterialLib *lib, float3 color);
void Object_SetEmission(Object *obj, MaterialLib *lib, float emission);
void Object_SetRoughness(Object *obj, MaterialLib *lib, float roughness);
void Object_SetMetallic(Object *obj, MaterialLib *lib, float metallic);

void CreateObjectBVH(Object *obj, BVH *bvh);
void DestroyObjectBVH(BVH *bvh);
void IntersectBVH(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir, int *hitTriIdx, float3 *hitPosWorld);
bool IntersectBVH_Shadow(const Object *obj, const BVH *bvh, float3 rayOrigin, float3 rayDir);
void getBvhStats(const BVH *bvh, int *outNodeCount, int *outTriCount);

// Perspective frustum — 5 planes (near, left, right, bottom, top), all inward-facing.
// Test: dot(normal, P) + d >= 0 means P is on the inside of the plane.
typedef struct {
	float3 normal;
	float d;
} FrustumPlane;

typedef struct {
	FrustumPlane planes[5];
} Frustum;

Frustum Frustum_FromCamera(const Camera *cam);
// Returns true if the AABB may be visible (not fully outside any plane).
bool Frustum_TestAABB(const Frustum *f, float3 bbMin, float3 bbMax);

// Branchless slab test with precomputed bias = ro * invRd.
// Returns tmin on hit, FLT_MAX on miss. Shared between object.c and ray.c.
static inline float rayAABB_inv(float3 bias, float3 invRd, const float *mn, const float *mx) {
	float tx0 = mn[0] * invRd.x - bias.x, tx1 = mx[0] * invRd.x - bias.x;
	float ty0 = mn[1] * invRd.y - bias.y, ty1 = mx[1] * invRd.y - bias.y;
	float tz0 = mn[2] * invRd.z - bias.z, tz1 = mx[2] * invRd.z - bias.z;
	float tmin = fmaxf(fmaxf(fminf(tx0, tx1), fminf(ty0, ty1)), fminf(tz0, tz1));
	float tmax = fminf(fminf(fmaxf(tx0, tx1), fmaxf(ty0, ty1)), fmaxf(tz0, tz1));
	tmin = fmaxf(tmin, 0.0f);
	return tmax < tmin ? FLT_MAX : tmin;
}

// Test 2 BVHNode children simultaneously using SSE.
// soa layout (BVHNode.soa): per axis {child0.mn, child0.mx, child1.mn, child1.mx}
// out[0] = tmin for child0, out[1] = tmin for child1; FLT_MAX on miss.
static inline void rayAABB_inv_x2_soa(float3 bias, float3 invRd, const float *soa, float out[2]) {
	__m128 tx = _mm_fmsub_ps(_mm_loadu_ps(soa + 0), _mm_set1_ps(invRd.x), _mm_set1_ps(bias.x));
	__m128 ty = _mm_fmsub_ps(_mm_loadu_ps(soa + 4), _mm_set1_ps(invRd.y), _mm_set1_ps(bias.y));
	__m128 tz = _mm_fmsub_ps(_mm_loadu_ps(soa + 8), _mm_set1_ps(invRd.z), _mm_set1_ps(bias.z));
	__m128 tx_sw = _mm_shuffle_ps(tx, tx, _MM_SHUFFLE(2, 3, 0, 1));
	__m128 ty_sw = _mm_shuffle_ps(ty, ty, _MM_SHUFFLE(2, 3, 0, 1));
	__m128 tz_sw = _mm_shuffle_ps(tz, tz, _MM_SHUFFLE(2, 3, 0, 1));
	__m128 tmin = _mm_max_ps(_mm_max_ps(_mm_min_ps(tx, tx_sw), _mm_min_ps(ty, ty_sw)), _mm_min_ps(tz, tz_sw));
	__m128 tmax = _mm_min_ps(_mm_min_ps(_mm_max_ps(tx, tx_sw), _mm_max_ps(ty, ty_sw)), _mm_max_ps(tz, tz_sw));
	tmin = _mm_max_ps(tmin, _mm_setzero_ps());
	__m128 result = _mm_blendv_ps(tmin, _mm_set1_ps(FLT_MAX), _mm_cmplt_ps(tmax, tmin));
	out[0] = _mm_cvtss_f32(result);
	out[1] = _mm_cvtss_f32(_mm_shuffle_ps(result, result, _MM_SHUFFLE(2, 2, 2, 2)));
}

#endif // OBJECT_H