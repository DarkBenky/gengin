#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "color/color.h"

#define M_PI 3.14159265f

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
	return (float3){
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x};
}

static inline float3 Float3_Normalize(float3 v) {
	float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
	float invLen = 1.0f / sqrtf(lenSq);

	return (float3){v.x * invLen, v.y * invLen, v.z * invLen};
}

static inline float3 Float3_Reflect(float3 i, float3 n) {
	float k = 2.0f * Float3_Dot(n, i);
	return Float3_Sub(i, Float3_Scale(n, k));
}

static inline float Q_rsqrt(float number) {
	union {
		float f;
		uint32_t i;
	} conv = {.f = number};
	conv.i = 0x5F375A86 - (conv.i >> 1);
	conv.f *= 1.5f - (number * 0.5f * conv.f * conv.f);
	return conv.f;
}

static inline float3 Float3_Normalize_(float3 v) {
	float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
	float invLen = Q_rsqrt(lenSq);
	return (float3){v.x * invLen, v.y * invLen, v.z * invLen};
}

static inline float3 Float3_Reflect_(float3 i, float3 n) {
	float k = 2.0f * Float3_Dot(n, i);
	return Float3_Sub(i, Float3_Scale(n, k));
}

#include <xmmintrin.h>
#include <emmintrin.h>

static inline __m128 mm_dot3_ps(__m128 a, __m128 b) {
	__m128 mul = _mm_mul_ps(a, b);
	__m128 yz = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 2, 1));
	__m128 z = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 0, 2));
	return _mm_add_ss(_mm_add_ss(mul, yz), z);
}

static inline float3 Float3_Normalize__(float3 v) {
	__m128 vec = _mm_set_ps(0.0f, v.z, v.y, v.x);
	__m128 d = mm_dot3_ps(vec, vec);
	__m128 dot = _mm_shuffle_ps(d, d, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 inv = _mm_rsqrt_ps(dot);
	__m128 half = _mm_set1_ps(0.5f);
	__m128 three = _mm_set1_ps(3.0f);
	inv = _mm_mul_ps(_mm_mul_ps(half, inv),
					 _mm_sub_ps(three, _mm_mul_ps(_mm_mul_ps(dot, inv), inv)));
	__m128 res = _mm_mul_ps(vec, inv);
	float3 out;
	_mm_store_ss(&out.x, res);
	_mm_store_ss(&out.y, _mm_shuffle_ps(res, res, _MM_SHUFFLE(1, 1, 1, 1)));
	_mm_store_ss(&out.z, _mm_shuffle_ps(res, res, _MM_SHUFFLE(2, 2, 2, 2)));
	return out;
}

static inline float3 Float3_Reflect__(float3 i, float3 n) {
	__m128 vi = _mm_set_ps(0.0f, i.z, i.y, i.x);
	__m128 vn = _mm_set_ps(0.0f, n.z, n.y, n.x);
	__m128 d = mm_dot3_ps(vn, vi);
	__m128 dotB = _mm_shuffle_ps(d, d, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 two = _mm_set1_ps(2.0f);
	__m128 res = _mm_sub_ps(vi, _mm_mul_ps(_mm_mul_ps(two, dotB), vn));
	float3 out;
	_mm_store_ss(&out.x, res);
	_mm_store_ss(&out.y, _mm_shuffle_ps(res, res, _MM_SHUFFLE(1, 1, 1, 1)));
	_mm_store_ss(&out.z, _mm_shuffle_ps(res, res, _MM_SHUFFLE(2, 2, 2, 2)));
	return out;
}

/* Normalize 4 vectors SoA — ~2.5x faster than 4 scalar calls */
static inline void Float3_Normalize4(float3 v[4]) {
	__m128 x = _mm_set_ps(v[3].x, v[2].x, v[1].x, v[0].x);
	__m128 y = _mm_set_ps(v[3].y, v[2].y, v[1].y, v[0].y);
	__m128 z = _mm_set_ps(v[3].z, v[2].z, v[1].z, v[0].z);
	__m128 ls = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, x), _mm_mul_ps(y, y)), _mm_mul_ps(z, z));
	__m128 inv = _mm_rsqrt_ps(ls);
	__m128 h = _mm_set1_ps(0.5f), t = _mm_set1_ps(3.0f);
	inv = _mm_mul_ps(_mm_mul_ps(h, inv), _mm_sub_ps(t, _mm_mul_ps(_mm_mul_ps(ls, inv), inv)));
	x = _mm_mul_ps(x, inv);
	y = _mm_mul_ps(y, inv);
	z = _mm_mul_ps(z, inv);
	float xb[4], yb[4], zb[4];
	_mm_storeu_ps(xb, x);
	_mm_storeu_ps(yb, y);
	_mm_storeu_ps(zb, z);
	for (int k = 0; k < 4; k++) {
		v[k].x = xb[k];
		v[k].y = yb[k];
		v[k].z = zb[k];
	}
}

/* Reflect 4 vectors against the same normal, then normalize — all in one SoA pass */
static inline void Float3_ReflectNormalize4(const float3 v[4], float3 n, float3 out[4]) {
	__m128 nx = _mm_set1_ps(n.x), ny = _mm_set1_ps(n.y), nz = _mm_set1_ps(n.z);
	__m128 vx = _mm_set_ps(v[3].x, v[2].x, v[1].x, v[0].x);
	__m128 vy = _mm_set_ps(v[3].y, v[2].y, v[1].y, v[0].y);
	__m128 vz = _mm_set_ps(v[3].z, v[2].z, v[1].z, v[0].z);
	__m128 dot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, vx), _mm_mul_ps(ny, vy)), _mm_mul_ps(nz, vz));
	__m128 two = _mm_set1_ps(2.0f);
	__m128 td = _mm_mul_ps(two, dot);
	__m128 rx = _mm_sub_ps(vx, _mm_mul_ps(td, nx));
	__m128 ry = _mm_sub_ps(vy, _mm_mul_ps(td, ny));
	__m128 rz = _mm_sub_ps(vz, _mm_mul_ps(td, nz));
	__m128 ls = _mm_add_ps(_mm_add_ps(_mm_mul_ps(rx, rx), _mm_mul_ps(ry, ry)), _mm_mul_ps(rz, rz));
	__m128 inv = _mm_rsqrt_ps(ls);
	__m128 h = _mm_set1_ps(0.5f), t3 = _mm_set1_ps(3.0f);
	inv = _mm_mul_ps(_mm_mul_ps(h, inv), _mm_sub_ps(t3, _mm_mul_ps(_mm_mul_ps(ls, inv), inv)));
	rx = _mm_mul_ps(rx, inv);
	ry = _mm_mul_ps(ry, inv);
	rz = _mm_mul_ps(rz, inv);
	float xb[4], yb[4], zb[4];
	_mm_storeu_ps(xb, rx);
	_mm_storeu_ps(yb, ry);
	_mm_storeu_ps(zb, rz);
	for (int k = 0; k < 4; k++) {
		out[k].x = xb[k];
		out[k].y = yb[k];
		out[k].z = zb[k];
	}
}

static inline int Min(int a, int b) {
	return a < b ? a : b;
}
static inline int Max(int a, int b) {
	return a > b ? a : b;
}
static inline float MinF(float a, float b) {
	return a < b ? a : b;
}
static inline float MaxF(float a, float b) {
	return a > b ? a : b;
}

static float EdgeFunction(float ax, float ay, float bx, float by, float cx, float cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static float3 RotateY(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x * c + v.z * s,
		v.y,
		-v.x * s + v.z * c};
}

static float3 RotateX(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x,
		v.y * c - v.z * s,
		v.y * s + v.z * c};
}

static float3 RotateZ(float3 v, float angle) {
	float c = cosf(angle);
	float s = sinf(angle);
	return (float3){
		v.x * c - v.y * s,
		v.x * s + v.y * c,
		v.z};
}

static float3 RotateXYZ(float3 v, float3 rotation) {
	v = RotateX(v, rotation.x);
	v = RotateY(v, rotation.y);
	v = RotateZ(v, rotation.z);
	return v;
}

static float3 ReflectBasedOnMaterial(float3 rayDir, float3 normal, float roughness, float seed) {
	float u = fmodf(seed * 127.1f + 311.7f, 1.0f);
	float v = fmodf(seed * 269.5f + 183.3f, 1.0f);

	float a = roughness * roughness;
	float phi = 2.0f * M_PI * u;
	float cosTheta = sqrtf((1.0f - v) / fmaxf(1.0f + (a * a - 1.0f) * v, 1e-6f));
	float sinTheta = sqrtf(fmaxf(1.0f - cosTheta * cosTheta, 0.0f));

	float3 H_tangent = (float3){sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta};

	float3 up = (fabsf(normal.y) < 0.999f) ? (float3){0, 1, 0} : (float3){1, 0, 0};
	float3 tangent = Float3_Normalize(Float3_Cross(up, normal));
	float3 bitangent = Float3_Cross(normal, tangent);

	float3 H = Float3_Normalize((float3){
		H_tangent.x * tangent.x + H_tangent.y * bitangent.x + H_tangent.z * normal.x,
		H_tangent.x * tangent.y + H_tangent.y * bitangent.y + H_tangent.z * normal.y,
		H_tangent.x * tangent.z + H_tangent.y * bitangent.z + H_tangent.z * normal.z,
	});

	float3 reflectDir = Float3_Reflect(rayDir, H);

	if (Float3_Dot(reflectDir, normal) < 0.0f)
		reflectDir = Float3_Reflect(reflectDir, normal);

	return Float3_Normalize(reflectDir);
}

static inline float FastSeed(float cameraSeed) {
	uint32 h = (uint32)(cameraSeed * 1013904223.0f);
	h ^= h >> 16;
	h *= 0x45d9f3bu;
	h ^= h >> 16;
	return (float)h * 2.3283064365e-10f;
}

void RenderObject(const Object *obj, const Camera *camera, const MaterialLib *lib) {
	if (!obj || !camera || !obj->v1) return;

	float seed = camera->seed;
	float3 right = camera->right;
	float3 up = camera->up;
	float aspect = camera->aspect;
	float fovScale = camera->fovScale;

	for (int i = 0; i < obj->triangleCount; i++) {
		float3 v0 = RotateXYZ(obj->v1[i], obj->rotation);
		float3 v1 = RotateXYZ(obj->v2[i], obj->rotation);
		float3 v2 = RotateXYZ(obj->v3[i], obj->rotation);
		float3 normal = Float3_Normalize(RotateXYZ(obj->normals[i], obj->rotation));

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

		float sx0 = (x0 + 1.0f) * 0.5f * camera->screenWidth + camera->jitter.x;
		float sy0 = (1.0f - y0) * 0.5f * camera->screenHeight + camera->jitter.y;
		float sx1 = (x1 + 1.0f) * 0.5f * camera->screenWidth + camera->jitter.x;
		float sy1 = (1.0f - y1) * 0.5f * camera->screenHeight + camera->jitter.y;
		float sx2 = (x2 + 1.0f) * 0.5f * camera->screenWidth + camera->jitter.x;
		float sy2 = (1.0f - y2) * 0.5f * camera->screenHeight + camera->jitter.y;

		int minX = Max(0, (int)MinF(MinF(sx0, sx1), sx2));
		int maxX = Min(camera->screenWidth - 1, (int)MaxF(MaxF(sx0, sx1), sx2));
		int minY = Max(0, (int)MinF(MinF(sy0, sy1), sy2));
		int maxY = Min(camera->screenHeight - 1, (int)MaxF(MaxF(sy0, sy1), sy2));

		float area = EdgeFunction(sx0, sy0, sx1, sy1, sx2, sy2);
		if (fabsf(area) <= 1e-8f) continue;
		float areaSign = area > 0.0f ? 1.0f : -1.0f;

		float invZ0 = 1.0f / z0;
		float invZ1 = 1.0f / z1;
		float invZ2 = 1.0f / z2;

		float3 norm = normal;

		float NdotL = MaxF(0.0f, Float3_Dot(norm, camera->renderLightDir));
		float NdotH = MaxF(0.0f, Float3_Dot(norm, camera->halfVec));
		const Material *mat = &lib->entries[obj->materialIds[i]];
		float roughness = mat->roughness;
		float shininess = (1.0f - roughness) * 128.0f + 1.0f;
		float spec = powf(NdotH, shininess);

		float metallic = mat->metallic;
		float3 baseColor = mat->color;
		float3 diffuse = Float3_Scale(baseColor, (1.0f - metallic) * NdotL);
		float3 specColor = Float3_Scale(baseColor, metallic);
		specColor = Float3_Add(specColor, Float3_Scale((float3){1, 1, 1}, 1.0f - metallic));
		float3 specular = Float3_Scale(specColor, spec * (1.0f - roughness * 0.7f));
		float3 ambient = Float3_Scale(baseColor, 0.1f);
		float3 finalColor = Float3_Add(Float3_Add(ambient, diffuse), specular);

		finalColor.x = MinF(1.0f, finalColor.x);
		finalColor.y = MinF(1.0f, finalColor.y);
		finalColor.z = MinF(1.0f, finalColor.z);

		// packedColor = 0xFF000000 | (R<<16) | (G<<8) | B  — kept for lit mode, see framebuffer write below
		uint32 packedColor = 0xFF000000 | ((uint8)(finalColor.x * 255.0f) << 16) | ((uint8)(finalColor.y * 255.0f) << 8) | (uint8)(finalColor.z * 255.0f);
		(void)packedColor;
		float invArea = 1.0f / area;
		float w0dx = sy2 - sy1;
		float w0dy = -(sx2 - sx1);
		float w1dx = sy0 - sy2;
		float w1dy = -(sx0 - sx2);
		float w2dx = sy1 - sy0;
		float w2dy = -(sx1 - sx0);
		float startPx = minX + 0.5f;
		float startPy = minY + 0.5f;
		float w0Row = EdgeFunction(sx1, sy1, sx2, sy2, startPx, startPy);
		float w1Row = EdgeFunction(sx2, sy2, sx0, sy0, startPx, startPy);
		float w2Row = EdgeFunction(sx0, sy0, sx1, sy1, startPx, startPy);

		for (int y = minY; y <= maxY; y++) {
			float w0 = w0Row;
			float w1 = w1Row;
			float w2 = w2Row;

			int x = minX;
			for (; x + 3 <= maxX; x += 4) {
				/* batch of 4 — gather pixels that pass coverage + depth */
				int bIdx[4];
				float3 bRay[4];
				int bCount = 0;

				for (int k = 0; k < 4; k++) {
					float cw0 = w0 + w0dx * k;
					float cw1 = w1 + w1dx * k;
					float cw2 = w2 + w2dx * k;
					if ((cw0 * areaSign) >= 0.0f && (cw1 * areaSign) >= 0.0f && (cw2 * areaSign) >= 0.0f) {
						float invZ = (cw0 * invZ0 + cw1 * invZ1 + cw2 * invZ2) * invArea;
						float depth = 1.0f / invZ;
						int idx = y * camera->screenWidth + (x + k);
						if (depth < camera->depthBuffer[idx]) {
							camera->depthBuffer[idx] = depth;
							camera->normalBuffer[idx] = normal;
							float p0 = cw0 * invZ0;
							float p1 = cw1 * invZ1;
							float p2 = cw2 * invZ2;
							float pSum = p0 + p1 + p2;
							if (pSum != 0.0f) {
								float invPSum = 1.0f / pSum;
								float3 worldPos = {
									(v0.x * p0 + v1.x * p1 + v2.x * p2) * invPSum,
									(v0.y * p0 + v1.y * p1 + v2.y * p2) * invPSum,
									(v0.z * p0 + v1.z * p1 + v2.z * p2) * invPSum};
								camera->positionBuffer[idx] = worldPos;
								bRay[bCount] = Float3_Sub(worldPos, camera->position);
								bIdx[bCount] = idx;
								bCount++;
							}
							camera->framebuffer[idx] = packedColor;
						}
					}
				}

				/* batch normalize + reflect for however many valid pixels we got */
				if (bCount > 0) {
					for (int k = bCount; k < 4; k++)
						bRay[k] = (float3){1.0f, 0.0f, 0.0f, 0.0f};
					Float3_Normalize4(bRay);
					float3 bRefl[4];
					Float3_ReflectNormalize4(bRay, norm, bRefl);
					for (int k = 0; k < bCount; k++)
						camera->reflectBuffer[bIdx[k]] = bRefl[k];
				}

				w0 += w0dx * 4;
				w1 += w1dx * 4;
				w2 += w2dx * 4;
			}

			/* tail — remaining pixels (0-3) handled scalar */
			for (; x <= maxX; x++) {
				if ((w0 * areaSign) >= 0.0f && (w1 * areaSign) >= 0.0f && (w2 * areaSign) >= 0.0f) {
					float invZ = (w0 * invZ0 + w1 * invZ1 + w2 * invZ2) * invArea;
					float depth = 1.0f / invZ;
					float p0 = w0 * invZ0;
					float p1 = w1 * invZ1;
					float p2 = w2 * invZ2;
					float pSum = p0 + p1 + p2;
					int idx = y * camera->screenWidth + x;
					if (depth < camera->depthBuffer[idx]) {
						camera->depthBuffer[idx] = depth;
						camera->normalBuffer[idx] = normal;
						if (pSum != 0.0f) {
							float invPSum = 1.0f / pSum;
							float3 worldPos = {
								(v0.x * p0 + v1.x * p1 + v2.x * p2) * invPSum,
								(v0.y * p0 + v1.y * p1 + v2.y * p2) * invPSum,
								(v0.z * p0 + v1.z * p1 + v2.z * p2) * invPSum};
							camera->positionBuffer[idx] = worldPos;
							float3 rayDir = Float3_Normalize(Float3_Sub(worldPos, camera->position));
							camera->reflectBuffer[idx] = Float3_Normalize(Float3_Reflect(rayDir, normal));
						}
						camera->framebuffer[idx] = packedColor;
					}
				}
				w0 += w0dx;
				w1 += w1dx;
				w2 += w2dx;
			}

			w0Row += w0dy;
			w1Row += w1dy;
			w2Row += w2dy;
		}
	}
}

void RenderSetup(const Object *objects, int objectCount, Camera *camera) {
	if (!objects || !camera || objectCount <= 0) return;
	camera->seed += FastSeed(camera->seed) + 0.01f;
	camera->right = Float3_Normalize(Float3_Cross((float3){0, 1, 0}, camera->forward));
	camera->up = Float3_Cross(camera->forward, camera->right);
	camera->aspect = (float)camera->screenWidth / (float)camera->screenHeight;
	camera->fovScale = tanf(camera->fov * 0.5f * 3.14159265f / 180.0f);
	camera->viewDir = Float3_Scale(camera->forward, -1.0f);
	camera->renderLightDir = Float3_Normalize((float3){0.5f, 0.7f, -0.5f});
	camera->halfVec = Float3_Normalize(Float3_Add(camera->renderLightDir, camera->viewDir));
}

void RenderObjects(const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib) {
	if (!objects || !camera || objectCount <= 0) return;
	RenderSetup(objects, objectCount, camera);
	for (int i = 0; i < objectCount; i++) {
		RenderObject(&objects[i], camera, lib);
	}
}

float RandomFloat(float min, float max) {
	return min + (max - min) * ((float)rand() / (float)RAND_MAX);
}

void TestFunctions() {
	int iterations = 1000000;
	double accumTime = 0.0;
	clock_t start;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		float3 n = {0.0f, 1.0f, 0.0f};
		start = clock();
		float3 r = Float3_Reflect(v, n);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)r;
	}
	printf("Average Reflect time: %f microseconds\n", (accumTime / iterations) * 1e6);
	accumTime = 0.0;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		float3 n = {0.0f, 1.0f, 0.0f};
		start = clock();
		float3 r = Float3_Reflect_(v, n);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)r;
	}
	printf("Average Reflect_ time: %f microseconds\n", (accumTime / iterations) * 1e6);
	accumTime = 0.0;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		float3 n = {0.0f, 1.0f, 0.0f};
		start = clock();
		float3 r = Float3_Reflect__(v, n);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)r;
	}
	printf("Average Reflect__ time: %f microseconds\n", (accumTime / iterations) * 1e6);
	accumTime = 0.0;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		start = clock();
		float3 n = Float3_Normalize(v);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)n;
	}
	printf("Average Normalize time: %f microseconds\n", (accumTime / iterations) * 1e6);
	accumTime = 0.0;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		start = clock();
		float3 n = Float3_Normalize_(v);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)n;
	}
	printf("Average Normalize_ time: %f microseconds\n", (accumTime / iterations) * 1e6);
	accumTime = 0.0;
	for (int i = 0; i < iterations; i++) {
		float3 v = {RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)};
		start = clock();
		float3 n = Float3_Normalize__(v);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)n;
	}
	printf("Average Normalize__ time: %f microseconds\n", (accumTime / iterations) * 1e6);

	accumTime = 0.0;
	for (int i = 0; i < iterations; i += 4) {
		float3 v[4] = {
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
		};
		start = clock();
		Float3_Normalize4(v);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
	}
	printf("Average Normalize4 time: %f microseconds/vec (4 vecs/call)\n", (accumTime / (iterations / 4)) * 1e6 / 4.0);

	accumTime = 0.0;
	float3 refN = {0.0f, 1.0f, 0.0f};
	for (int i = 0; i < iterations; i += 4) {
		float3 v[4] = {
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
			{RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f), RandomFloat(-1.0f, 1.0f)},
		};
		float3 out[4];
		start = clock();
		Float3_ReflectNormalize4(v, refN, out);
		accumTime += (double)(clock() - start) / CLOCKS_PER_SEC;
		(void)out;
	}
	printf("Average ReflectNormalize4 time: %f microseconds/vec (4 vecs/call)\n", (accumTime / (iterations / 4)) * 1e6 / 4.0);
}
