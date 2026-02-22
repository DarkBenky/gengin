#include "render.h"
#include <math.h>
#include <string.h>
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

void RenderObject(const Object *obj, const Camera *camera) {
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
		float roughness = obj->roughness[i];
		float shininess = (1.0f - roughness) * 128.0f + 1.0f;
		float spec = powf(NdotH, shininess);

		float metallic = obj->metallic[i];
		float3 baseColor = obj->colors[i];
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
			for (int x = minX; x <= maxX; x++) {
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
							float3 worldPos = (float3){
								(v0.x * p0 + v1.x * p1 + v2.x * p2) * invPSum,
								(v0.y * p0 + v1.y * p1 + v2.y * p2) * invPSum,
								(v0.z * p0 + v1.z * p1 + v2.z * p2) * invPSum};
							camera->positionBuffer[idx] = worldPos;
							float3 rayDir = Float3_Normalize(Float3_Sub(worldPos, camera->position));
							// camera->reflectBuffer[idx] = ReflectBasedOnMaterial(rayDir, normal, roughness, seed + (float)i);
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

void RenderObjects(const Object *objects, int objectCount, Camera *camera) {
	if (!objects || !camera || objectCount <= 0) return;
	camera->seed += FastSeed(camera->seed) + 0.01f;

	camera->right = Float3_Normalize(Float3_Cross((float3){0, 1, 0}, camera->forward));
	camera->up = Float3_Cross(camera->forward, camera->right);
	camera->aspect = (float)camera->screenWidth / (float)camera->screenHeight;
	camera->fovScale = tanf(camera->fov * 0.5f * 3.14159265f / 180.0f);
	camera->viewDir = Float3_Scale(camera->forward, -1.0f);
	camera->renderLightDir = Float3_Normalize((float3){0.5f, 0.7f, -0.5f});
	camera->halfVec = Float3_Normalize(Float3_Add(camera->renderLightDir, camera->viewDir));

	for (int i = 0; i < objectCount; i++) {
		RenderObject(&objects[i], camera);
	}
}
