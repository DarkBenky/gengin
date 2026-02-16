#include "render.h"
#include <math.h>
#include <string.h>
#include "../math/matrix.h"

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
	float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len > 0.0f) {
		float invLen = 1.0f / len;
		return Float3_Scale(v, invLen);
	}
	return v;
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

void RenderObject(const Object *obj, const Camera *camera) {
	if (!obj || !camera || !obj->triangles) return;

	float3 right = Float3_Normalize(Float3_Cross((float3){0, 1, 0}, camera->forward));
	float3 up = Float3_Cross(camera->forward, right);

	float aspect = (float)camera->screenWidth / (float)camera->screenHeight;
	float fovScale = tanf(camera->fov * 0.5f * 3.14159265f / 180.0f);

	for (int i = 0; i < obj->triangleCount; i++) {
		Triangle tri = obj->triangles[i];

		float3 v0 = Matrix_TransformPoint(obj->transform, tri.v1);
		float3 v1 = Matrix_TransformPoint(obj->transform, tri.v2);
		float3 v2 = Matrix_TransformPoint(obj->transform, tri.v3);
		float3 normal = Float3_Normalize(Matrix_TransformVector(obj->transform, tri.normal));

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

		float3 lightDir = Float3_Normalize((float3){0.5f, 0.7f, -0.5f});
		float3 viewDir = Float3_Normalize(Float3_Scale(camera->forward, -1.0f));
		float3 norm = normal;

		float NdotL = MaxF(0.0f, Float3_Dot(norm, lightDir));
		float3 halfVec = Float3_Normalize(Float3_Add(lightDir, viewDir));
		float NdotH = MaxF(0.0f, Float3_Dot(norm, halfVec));
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

		uint8 packedR = (uint8)(finalColor.x * 255.0f);
		uint8 packedG = (uint8)(finalColor.y * 255.0f);
		uint8 packedB = (uint8)(finalColor.z * 255.0f);
		uint32 packedColor = 0xFF000000 | (packedR << 16) | (packedG << 8) | packedB;
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
	for (int i = 0; i < objectCount; i++) {
		RenderObject(&objects[i], camera);
	}
}
