#include "ray.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../math/scalar.h"
#include "../../math/transform.h"
#include "../../math/vector3.h"
#include "../color/color.h"
#include "../../skybox/skybox.h"
#include "../../util/threadPool.h"

static inline Color PackColorFast01(float3 color) {
	uint8 r = (uint8)(color.x * 255.0f);
	uint8 g = (uint8)(color.y * 255.0f);
	uint8 b = (uint8)(color.z * 255.0f);
	return (Color)((r << 16) | (g << 8) | b);
}

static inline Color BlendColors50(Color a, Color b) {
	return ((a & 0x00FEFEFEu) + (b & 0x00FEFEFEu)) >> 1;
}

// Lerp the 6 precomputed directional emissions using a local-space normal.
// Each face contributes max(0, dot(n, faceDir)) * faceEmission — no trig needed.
static inline float3 SampleObjectEmission(const Object *obj, float3 localNormal) {
	float3 r = {0.0f, 0.0f, 0.0f};
	// +X -X +Y -Y +Z -Z
	if (localNormal.x > 0.0f) { r.x += localNormal.x * obj->faceEmissions[0].x; r.y += localNormal.x * obj->faceEmissions[0].y; r.z += localNormal.x * obj->faceEmissions[0].z; }
	if (localNormal.x < 0.0f) { float w = -localNormal.x; r.x += w * obj->faceEmissions[1].x; r.y += w * obj->faceEmissions[1].y; r.z += w * obj->faceEmissions[1].z; }
	if (localNormal.y > 0.0f) { r.x += localNormal.y * obj->faceEmissions[2].x; r.y += localNormal.y * obj->faceEmissions[2].y; r.z += localNormal.y * obj->faceEmissions[2].z; }
	if (localNormal.y < 0.0f) { float w = -localNormal.y; r.x += w * obj->faceEmissions[3].x; r.y += w * obj->faceEmissions[3].y; r.z += w * obj->faceEmissions[3].z; }
	if (localNormal.z > 0.0f) { r.x += localNormal.z * obj->faceEmissions[4].x; r.y += localNormal.z * obj->faceEmissions[4].y; r.z += localNormal.z * obj->faceEmissions[4].z; }
	if (localNormal.z < 0.0f) { float w = -localNormal.z; r.x += w * obj->faceEmissions[5].x; r.y += w * obj->faceEmissions[5].y; r.z += w * obj->faceEmissions[5].z; }
	return r;
}

static void BlurColorBuffer(Color *src, Color *temp, int width, int height, int radius) {
	if (radius <= 0) return;

	for (int y = 0; y < height; y++) {
		const Color *row = src + y * width;
		Color *out = temp + y * width;
		for (int x = 0; x < width; x++) {
			uint32 r = 0, g = 0, b = 0, n = 0;
			int x0 = x - radius, x1 = x + radius;
			if (x0 < 0) x0 = 0;
			if (x1 >= width) x1 = width - 1;
			for (int k = x0; k <= x1; k++) {
				Color c = row[k];
				if (!c) continue;
				r += (c >> 16) & 0xFF;
				g += (c >> 8) & 0xFF;
				b += c & 0xFF;
				n++;
			}
			out[x] = n ? (0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n)) : 0;
		}
	}

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32 r = 0, g = 0, b = 0, n = 0;
			int y0 = y - radius, y1 = y + radius;
			if (y0 < 0) y0 = 0;
			if (y1 >= height) y1 = height - 1;
			for (int k = y0; k <= y1; k++) {
				Color c = temp[k * width + x];
				if (!c) continue;
				r += (c >> 16) & 0xFF;
				g += (c >> 8) & 0xFF;
				b += c & 0xFF;
				n++;
			}
			src[y * width + x] = n ? (0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n)) : 0;
		}
	}
}

void BlurBuffer(float *src, float *temp, int width, int height, int radius) {
	if (radius <= 0) return;

	// Pre-pass: skip blur entirely if no pixel deviates from 1.0
	int hasShadow = 0;
	for (int i = 0; i < width * height; i++) {
		if (src[i] < 1.0f) {
			hasShadow = 1;
			break;
		}
	}
	if (!hasShadow) return;

	float invSize = 1.0f / (2 * radius + 1);

	// Horizontal pass — skip rows that are entirely 1.0f
	for (int y = 0; y < height; y++) {
		int rowStart = y * width;

		int rowHasShadow = 0;
		for (int x = 0; x < width; x++) {
			if (src[rowStart + x] < 1.0f) {
				rowHasShadow = 1;
				break;
			}
		}
		if (!rowHasShadow) {
			for (int x = 0; x < width; x++)
				temp[rowStart + x] = 1.0f;
			continue;
		}

		float sum = 0.0f;
		for (int x = 0; x < radius; x++) {
			sum += src[rowStart + x];
		}

		for (int x = 0; x < width; x++) {
			int right = x + radius;
			int left = x - radius - 1;

			if (right < width) sum += src[rowStart + right];
			if (left >= 0) sum -= src[rowStart + left];

			temp[rowStart + x] = sum * invSize;
		}
	}

	// Vertical pass — skip columns that are entirely 1.0f after horizontal pass
	for (int x = 0; x < width; x++) {
		int colHasShadow = 0;
		for (int y = 0; y < height; y++) {
			if (temp[y * width + x] < 1.0f) {
				colHasShadow = 1;
				break;
			}
		}
		if (!colHasShadow) {
			for (int y = 0; y < height; y++)
				src[y * width + x] = 1.0f;
			continue;
		}

		float sum = 0.0f;
		for (int y = 0; y < radius; y++) {
			sum += temp[y * width + x];
		}

		for (int y = 0; y < height; y++) {
			int bottom = y + radius;
			int top = y - radius - 1;

			if (bottom < height) sum += temp[bottom * width + x];
			if (top >= 0) sum -= temp[top * width + x];

			src[y * width + x] = sum * invSize;
		}
	}
}

float3 ComputeRayDirection(const Camera *camera, int pixelX, int pixelY) {
	float ndcX = (pixelX + 0.5f) / camera->screenWidth * 2.0f - 1.0f;
	float ndcY = 1.0f - (pixelY + 0.5f) / camera->screenHeight * 2.0f;

	float3 forward = Float3_Normalize(camera->forward);
	float3 rayDir = Float3_Add(
		Float3_Add(
			Float3_Scale(camera->right, ndcX * camera->aspect * camera->fovScale),
			Float3_Scale(camera->up, ndcY * camera->fovScale)),
		forward);
	return Float3_Normalize(rayDir);
}

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera, const int resolution, const int frameInterval) {
	if (!objects || objectCount <= 0 || !camera) return;
	int width = camera->screenWidth;
	int height = camera->screenHeight;
	int size = width * height;

	int recompute = (camera->frameCounter % frameInterval) == 0;
	camera->frameCounter++;

	if (recompute) {
		float3 lightDir = camera->lightDir;
		float bias = 0.01f;
		int step = MaxF32(1, resolution);
		const Color skyColor = 0x007FB2FFu;

		for (int i = 0; i < size; i++)
			camera->tempBuffer_1[i] = 1.0f;
		memset(camera->reflectCache, 0, size * sizeof(Color));

		for (int y = 0; y < height; y += step) {
			for (int x = 0; x < width; x += step) {
				int idx = y * width + x;
				if (camera->depthBuffer[idx] >= DEPTH_FAR || camera->depthBuffer[idx] <= 0.0f) continue;

				float3 worldPos = camera->positionBuffer[idx];
				float3 normal = camera->normalBuffer[idx];

				// if (normal.y < 0.5f) continue;

				float3 biasedPos = Float3_Add(worldPos, Float3_Scale(normal, bias));

				if (IntersectAnyBBox(objects, objectCount, biasedPos, lightDir)) {
					for (int dy = 0; dy < step && y + dy < height; dy++) {
						for (int dx = 0; dx < step && x + dx < width; dx++) {
							int fillIdx = (y + dy) * width + (x + dx);
							if (camera->depthBuffer[fillIdx] < DEPTH_FAR && camera->depthBuffer[fillIdx] > 0.0f) {
								float3 fillNormal = camera->normalBuffer[fillIdx];
								if (fillNormal.y >= 0.5f)
									camera->tempBuffer_1[fillIdx] = 0.0f;
							}
						}
					}
				}

				float3 reflDir = camera->reflectBuffer[idx];
				Color reflectionColor = IntersectBBoxColor(objects, objectCount, biasedPos, reflDir);
				Color blendColor = reflectionColor != 0 ? reflectionColor : skyColor;

				for (int dy = 0; dy < step && y + dy < height; dy++) {
					for (int dx = 0; dx < step && x + dx < width; dx++) {
						int fillIdx = (y + dy) * width + (x + dx);
						camera->reflectCache[fillIdx] = blendColor;
					}
				}
			}
		}

		BlurColorBuffer(camera->reflectCache, camera->tempFramebuffer, width, height, 3);
		BlurBuffer(camera->tempBuffer_1, camera->tempBuffer_2, width, height, 5);
		memcpy(camera->shadowCache, camera->tempBuffer_1, size * sizeof(float));
	}

	for (int i = 0; i < size; i++) {
		if (camera->reflectCache[i] != 0)
			camera->framebuffer[i] = BlendColors50(camera->framebuffer[i], camera->reflectCache[i]);
		// Integer fixed-point darken: factor = 0.5 + 0.5*shadowCache in [128..256]/256
		uint32 scale = 128 + (uint32)(camera->shadowCache[i] * 128.0f);
		Color c = camera->framebuffer[i];
		uint32 r = (((c >> 16) & 0xFF) * scale) >> 8;
		uint32 g = (((c >> 8) & 0xFF) * scale) >> 8;
		uint32 b = ((c & 0xFF) * scale) >> 8;
		camera->framebuffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

static inline float RandomFloat(float seed) {
	seed = fmodf(seed * 43758.5453f, 1.0f);
	return seed;
}

static inline float3 RandomHemisphereDirection(float3 normal, float seed) {
	float r1 = RandomFloat(seed);
	float r2 = RandomFloat(seed + 1.0f);

	// Concentric disk mapping — no sin/cos needed
	float a = 2.0f * r1 - 1.0f;
	float b = 2.0f * r2 - 1.0f;
	float phi, rad;
	if (a * a > b * b) {
		rad = a;
		phi = (M_PI / 4.0f) * (b / a);
	} else if (b != 0.0f) {
		rad = b;
		phi = (M_PI / 2.0f) - (M_PI / 4.0f) * (a / b);
	} else {
		return normal;
	} // degenerate, just return normal
	float sx = rad * cosf(phi); // still one sin/cos but on a much smaller range
	float sy = rad * sinf(phi);
	float cosTheta = sqrtf(fmaxf(0.0f, 1.0f - sx * sx - sy * sy));

	// Duff et al. 2017 cheap ONB — no cross products, no normalize
	float3 t, b3;
	float sign = (normal.z >= 0.0f) ? 1.0f : -1.0f;
	float aa = -1.0f / (sign + normal.z);
	float bb = normal.x * normal.y * aa;
	t = (float3){1.0f + sign * normal.x * normal.x * aa, sign * bb, -sign * normal.x};
	b3 = (float3){bb, sign + normal.y * normal.y * aa, -normal.y};

	return (float3){
		sx * t.x + sy * b3.x + cosTheta * normal.x,
		sx * t.y + sy * b3.y + cosTheta * normal.y,
		sx * t.z + sy * b3.z + cosTheta * normal.z,
	};
}

// viewDir: incoming ray direction (toward surface). roughness scatters, metallic boosts specular.
static inline float3 RandomHemisphereDirectionWithMaterial(float3 normal, float3 viewDir, float seed, Material mat) {
	float3 randomDir = RandomHemisphereDirection(normal, seed);
	float3 perfectReflect = Float3_Normalize(Float3_Reflect(viewDir, normal));
	// metals are always specular; smooth surfaces also reflect sharply
	float specularWeight = fmaxf(1.0f - mat.roughness, mat.metallic);
	return Float3_Normalize(Float3_Lerp(randomDir, perfectReflect, specularWeight));
}

static void SkyBoxTaskFunc(void *arg) {
	SkyBoxTask *task = arg;
	int row = task->row;
	Camera *camera = task->camera;
	Skybox *skybox = task->skybox;

	for (int x = 0; x < camera->screenWidth; x++) {
		int idx = row * camera->screenWidth + x;
		if (camera->depthBuffer[idx] >= DEPTH_FAR) {
			// No geometry — fill with sky using primary ray direction
			float3 rayDir = ComputeRayDirection(camera, x, row);
			camera->framebuffer[idx] = SampleSkybox(skybox, rayDir);
			continue;
		}

		// Geometry hit — blend sky reflection weighted by Fresnel angle of incidence
		float3 reflDir = camera->reflectBuffer[idx];
		Color skyColor = SampleSkybox(skybox, reflDir);
		float reflStr = camera->tempBuffer_2[idx];
		uint32 t = (uint32)(reflStr * 255.0f);
		uint32 it = 255u - t;
		Color base = camera->framebuffer[idx];
		uint32 nr = (((base >> 16) & 0xFF) * it + ((skyColor >> 16) & 0xFF) * t) >> 8;
		uint32 ng = (((base >> 8) & 0xFF) * it + ((skyColor >> 8) & 0xFF) * t) >> 8;
		uint32 nb = ((base & 0xFF) * it + (skyColor & 0xFF) * t) >> 8;
		camera->framebuffer[idx] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
	}
}

void applySkybox(const Skybox *skybox, Camera *camera, ThreadPool *threadPool, SkyBoxTaskQueue *taskQueue) {
	if (!skybox || !camera || !threadPool || !taskQueue) return;
	for (int row = 0; row < camera->screenHeight; row++) {
		SkyBoxTask *task = &taskQueue->tasks[row];
		task->row = row;
		task->camera = camera;
		task->skybox = (Skybox *)skybox;
		poolAdd(threadPool, SkyBoxTaskFunc, task);
	}
	poolWait(threadPool);
}

static void rayCollision(Object *objects, int objectCount, float3 rayOrigin, float3 rayDir, int excludeObj, int *hitObjIdx, int *hitTriIdx, float3 *hitPos) {
	*hitObjIdx = -1;
	if (hitTriIdx) *hitTriIdx = -1;
	if (hitPos) *hitPos = (float3){0.0f, 0.0f, 0.0f};

	// Fast shadow path: return as soon as any object is hit
	if (!hitTriIdx && !hitPos) {
		for (int i = 0; i < objectCount; i++) {
			if (i == excludeObj) continue;
			float bboxMin, bboxMax;
			RayBoxItersect(&objects[i], rayOrigin, rayDir, &bboxMin, &bboxMax);
			if (bboxMin >= bboxMax) continue;
			if (IntersectBVH_Shadow(&objects[i], &objects[i].bvh, rayOrigin, rayDir)) {
				*hitObjIdx = i;
				return;
			}
		}
		return;
	}

	float bestT = DEPTH_FAR;

	for (int i = 0; i < objectCount; i++) {
		if (ObjectBehindCamera(&objects[i], rayOrigin, rayDir)) continue;

		if (i == excludeObj) continue;
		// reject with world AABB first — cheap
		float bboxMin, bboxMax;
		RayBoxItersect(&objects[i], rayOrigin, rayDir, &bboxMin, &bboxMax);
		if (bboxMin >= bboxMax || bboxMin >= bestT) continue;

		int triIdx = -1;
		float3 hitPosLocal;
		IntersectBVH(&objects[i], &objects[i].bvh, rayOrigin, rayDir, &triIdx, &hitPosLocal);
		if (triIdx < 0) continue;

		float3 dv = {hitPosLocal.x - rayOrigin.x, hitPosLocal.y - rayOrigin.y, hitPosLocal.z - rayOrigin.z};
		float t = dv.x * rayDir.x + dv.y * rayDir.y + dv.z * rayDir.z;
		if (t > 0.0f && t < bestT) {
			bestT = t;
			*hitObjIdx = i;
			if (hitTriIdx) *hitTriIdx = triIdx;
			if (hitPos) *hitPos = hitPosLocal;
		}
	}
}

// if (RayCast(objects, objectCount, origin, dir, excludeObj, lib, &hit)) { /* hit.pos, hit.normal in world-space, hit.mat, hit.objIdx, hit.triIdx */ }
bool RayCast(Object *objects, int objectCount, float3 rayOrigin, float3 rayDir, int excludeObj, const MaterialLib *lib, RayHit *hit) {
	int objIdx, triIdx;
	float3 hitPos;
	rayCollision(objects, objectCount, rayOrigin, rayDir, excludeObj, &objIdx, &triIdx, &hitPos);
	if (objIdx < 0) return false;

	const Object *obj = &objects[objIdx];

	float3 ln0 = obj->normals[triIdx];
	float3 n = {
		obj->_fwdRot0.x * ln0.x + obj->_fwdRot0.y * ln0.y + obj->_fwdRot0.z * ln0.z,
		obj->_fwdRot1.x * ln0.x + obj->_fwdRot1.y * ln0.y + obj->_fwdRot1.z * ln0.z,
		obj->_fwdRot2.x * ln0.x + obj->_fwdRot2.y * ln0.y + obj->_fwdRot2.z * ln0.z};
	float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
	if (nlen > 1e-6f) {
		float ni = 1.0f / nlen;
		n.x *= ni;
		n.y *= ni;
		n.z *= ni;
	}

	Material mat = {.color = {0.8f, 0.8f, 0.8f}, .roughness = 0.8f};
	if (lib && obj->materialIds) {
		int matId = obj->materialIds[triIdx];
		if (matId >= 0 && matId < lib->count)
			mat = lib->entries[matId];
	}

	*hit = (RayHit){.pos = hitPos, .normal = n, .mat = mat, .objIdx = objIdx, .triIdx = triIdx};
	return true;
}

// Returns normalized direction from rayOrigin toward a random point on object's surface.
// outHitPos receives the sampled world-space point.
static float3 RandomObjectHitRay(const Object *object, float3 rayOrigin, float seed, float3 *outHitPos) {
	int triIdx = (int)(RandomFloat(seed) * object->triangleCount) % object->triangleCount;

	float3 v0 = object->v1[triIdx];
	float3 v1 = object->v2[triIdx];
	float3 v2 = object->v3[triIdx];

	// uniform barycentric sample
	float r1 = RandomFloat(seed + 1.0f);
	float r2 = RandomFloat(seed + 2.0f);
	if (r1 + r2 > 1.0f) {
		r1 = 1.0f - r1;
		r2 = 1.0f - r2;
	}
	float r0 = 1.0f - r1 - r2;

	float3 localPos = {
		r0 * v0.x + r1 * v1.x + r2 * v2.x,
		r0 * v0.y + r1 * v1.y + r2 * v2.y,
		r0 * v0.z + r1 * v1.z + r2 * v2.z,
	};

	// local -> world
	float3 worldPos = RotateXYZ(localPos, object->rotation);
	worldPos.x = worldPos.x * object->scale.x + object->position.x;
	worldPos.y = worldPos.y * object->scale.y + object->position.y;
	worldPos.z = worldPos.z * object->scale.z + object->position.z;

	if (outHitPos) *outHitPos = worldPos;

	return Float3_Normalize(Float3_Sub(worldPos, rayOrigin));
}

static void RayTraceRowFunc(void *arg) {
	RayTraceTask *task = arg;
	int row = task->row;
	Camera *camera = task->camera;
	const Object *objects = task->objects;
	int objectCount = task->objectCount;
	const MaterialLib *lib = task->lib;
	int width = camera->screenWidth;
	int height = camera->screenHeight;

	// hoist all per-frame constants out of the pixel loop
	float3 orig = camera->position;
	float3 lightDir = Float3_Normalize(camera->lightDir);
	float3 fwd = Float3_Normalize(camera->forward);
	float3 rgt = Float3_Normalize(camera->right);
	float3 up_ = Float3_Normalize(camera->up);
	float aspect = camera->aspect;
	float fovScale = camera->fovScale;

	// precompute per-row ray base and per-pixel right step
	float ndcY = 1.0f - (row + 0.5f) / (float)height * 2.0f;
	float yscale = ndcY * fovScale;
	float rx = fwd.x + up_.x * yscale;
	float ry = fwd.y + up_.y * yscale;
	float rz = fwd.z + up_.z * yscale;
	float sx = rgt.x * aspect * fovScale;
	float sy = rgt.y * aspect * fovScale;
	float sz = rgt.z * aspect * fovScale;

	float3 catchReflections[width]; // row-local reflection samples, blurred in post-pass
	float catchShadows[width];      // row-local shadow factors (1.0=lit, 0.0=shadow)
	float3 catchIndirect[width];    // row-local indirect (emission) samples
	memset(catchReflections, 0, sizeof(float3) * width);
	memset(catchIndirect, 0, sizeof(float3) * width);
	float3 catchReflection = {0.0f, 0.0f, 0.0f};
	float catchShadow = 1.0f;
	float3 catchIndirectSample = {0.0f, 0.0f, 0.0f};

	for (int x = 0; x < width; x++) {
		int idx = row * width + x;

		float ndcX = (x + 0.5f) / (float)width * 2.0f - 1.0f;
		float dx = rx + sx * ndcX;
		float dy = ry + sy * ndcX;
		float dz = rz + sz * ndcX;
		float inv = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz);
		dx *= inv;
		dy *= inv;
		dz *= inv;

		float bestT = DEPTH_FAR;
		int bestObj = -1, bestTri = -1;
		float3 bestHitPos = {0};

		for (int i = 0; i < objectCount; i++) {
			// frustum cull using precomputed camera planes
			if (!Frustum_TestAABB(&task->frustum, objects[i].worldBBmin, objects[i].worldBBmax)) continue;

			float bboxMin, bboxMax;
			RayBoxItersect(&objects[i], orig, (float3){dx, dy, dz}, &bboxMin, &bboxMax);
			if (bboxMin >= bboxMax || bboxMin >= bestT) continue;

			int triIdx = -1;
			float3 hitPos;
			IntersectBVH(&objects[i], &objects[i].bvh, orig, (float3){dx, dy, dz}, &triIdx, &hitPos);
			if (triIdx < 0) continue;

			float t = (hitPos.x - orig.x) * dx + (hitPos.y - orig.y) * dy + (hitPos.z - orig.z) * dz;
			if (t > 0.0f && t < bestT) {
				bestT = t;
				bestObj = i;
				bestTri = triIdx;
				bestHitPos = hitPos;
			}
		}

		if (bestObj < 0) {
			camera->depthBuffer[idx] = DEPTH_FAR;
			camera->objectIdBuffer[idx] = -1;
			camera->framebuffer[idx] = SampleSkybox(task->skybox, (float3){dx, dy, dz});
			camera->emissionBuffer[idx] = (float3){0};
			catchShadows[x] = 1.0f;
			catchIndirect[x] = (float3){0};
			continue;
		}

		const Object *obj = &objects[bestObj];

		float3 ln = obj->normals[bestTri];
		float3 n = {
			obj->_fwdRot0.x * ln.x + obj->_fwdRot0.y * ln.y + obj->_fwdRot0.z * ln.z,
			obj->_fwdRot1.x * ln.x + obj->_fwdRot1.y * ln.y + obj->_fwdRot1.z * ln.z,
			obj->_fwdRot2.x * ln.x + obj->_fwdRot2.y * ln.y + obj->_fwdRot2.z * ln.z};
		float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
		if (nlen > 1e-6f) {
			float ni = 1.0f / nlen;
			n.x *= ni;
			n.y *= ni;
			n.z *= ni;
		}

		float3 color = {0.8f, 0.8f, 0.8f};
		float emission = 0.0f;
		float roughness = 0.8f;
		if (lib && obj->materialIds) {
			int matId = obj->materialIds[bestTri];
			if (matId >= 0 && matId < lib->count) {
				color = lib->entries[matId].color;
				emission = lib->entries[matId].emission;
				roughness = lib->entries[matId].roughness;
			}
		}

		float3 sOrig = {bestHitPos.x + n.x * 0.01f, bestHitPos.y + n.y * 0.01f, bestHitPos.z + n.z * 0.01f};
		camera->normalBuffer[idx] = n;
		camera->positionBuffer[idx] = bestHitPos;

		// Shadow ray — sample every SHADOW_RESOLUTION pixels, hold last result between samples.
		// Emissive surfaces are self-lit: shadow never applies to them.
		if (emission > 0.0f) {
			catchShadow = 1.0f;
		} else if (x % SHADOW_RESOLUTION == 0) {
			int shadowHit = -1;
			rayCollision((Object *)objects, objectCount, sOrig, lightDir, bestObj, &shadowHit, NULL, NULL);
			catchShadow = (shadowHit < 0) ? 1.0f : 0.0f;
		}
		catchShadows[x] = catchShadow;

		// Lighting — computed fully lit; shadow is applied in the post-pass blur.
		float diffuse = n.x * lightDir.x + n.y * lightDir.y + n.z * lightDir.z;
		if (diffuse < 0.0f) diffuse = 0.0f;

		// Blinn-Phong specular — half vector between light and view (NdotH^8 approx)
		float hx = lightDir.x - dx, hy = lightDir.y - dy, hz = lightDir.z - dz;
		float hlen = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz);
		float NdotH = fmaxf(0.0f, (n.x * hx + n.y * hy + n.z * hz) * hlen);
		float spec2 = NdotH * NdotH;
		float spec4 = spec2 * spec2;
		float specular = (diffuse > 0.0f) ? (spec4 * spec4 * (1.0f - roughness) * 0.4f) : 0.0f;

		float lit = 0.12f + 0.88f * diffuse;

		uint8 r = (uint8)(fminf(color.x * lit + specular + color.x * emission, 1.0f) * 255.0f);
		uint8 g = (uint8)(fminf(color.y * lit + specular + color.y * emission, 1.0f) * 255.0f);
		uint8 b = (uint8)(fminf(color.z * lit + specular + color.z * emission, 1.0f) * 255.0f);

		// Fresnel-Schlick for sky reflection weight
		float NdotV = fmaxf(0.0f, -(n.x * dx + n.y * dy + n.z * dz));
		float invNdotV = 1.0f - NdotV;
		float inv2 = invNdotV * invNdotV;
		float fresnel = 0.04f + 0.96f * (inv2 * inv2 * invNdotV);
		float reflectStrength = fresnel * (1.0f - roughness);

		// perfect specular reflect dir — roughness already baked into reflectStrength
		float dot2 = 2.0f * (n.x * dx + n.y * dy + n.z * dz);
		float3 reflDir = {dx - n.x * dot2, dy - n.y * dot2, dz - n.z * dot2};

		// inline sky reflection blend: cheap lerp weighted by Fresnel * (1 - roughness)
		Color skyRefl = SampleSkybox(task->skybox, reflDir);
		uint32 st = (uint32)(reflectStrength * 255.0f);
		if (st > 255u) st = 255u;
		uint32 sit = 255u - st;
		uint32 nr = ((uint32)r * sit + ((skyRefl >> 16) & 0xFF) * st) >> 8;
		uint32 ng = ((uint32)g * sit + ((skyRefl >> 8) & 0xFF) * st) >> 8;
		uint32 nb = ((uint32)b * sit + (skyRefl & 0xFF) * st) >> 8;

		camera->framebuffer[idx] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
		// View-Z depth (dot with forward) keeps SSR depth comparisons consistent
		camera->depthBuffer[idx] = (bestHitPos.x - orig.x) * fwd.x + (bestHitPos.y - orig.y) * fwd.y + (bestHitPos.z - orig.z) * fwd.z;
		camera->objectIdBuffer[idx] = bestObj;
		// w = (1-roughness) for SSR reflectivity gating
		camera->reflectBuffer[idx] = (float3){reflDir.x, reflDir.y, reflDir.z, (1.0f - roughness)};

		// ray traced reflection — only cast every REFLECTION_RESOLUTION columns
		if (x % REFLECTION_RESOLUTION == 0) {
			float3 rOrig = {bestHitPos.x + n.x * 0.01f, bestHitPos.y + n.y * 0.01f, bestHitPos.z + n.z * 0.01f};
			RayHit rHit;
			if (RayCast((Object *)objects, objectCount, rOrig, reflDir, bestObj, lib, &rHit)) {
				catchReflection.x = rHit.mat.color.x;
				catchReflection.y = rHit.mat.color.y;
				catchReflection.z = rHit.mat.color.z;
				catchReflection.w = reflectStrength;
			} else {
				Color skyColor = SampleSkybox(task->skybox, reflDir);
				catchReflection.x = ((skyColor >> 16) & 0xFF) / 255.0f;
				catchReflection.y = ((skyColor >> 8) & 0xFF) / 255.0f;
				catchReflection.z = (skyColor & 0xFF) / 255.0f;
				catchReflection.w = reflectStrength;
			}
		}
		catchReflections[x] = catchReflection;

		// Indirect lighting (GI) — sample every INDIRECT_RESOLUTION pixels.
		// For each emissive object, cast one ray toward a random point on its surface.
		// The emissive object list is precomputed once per frame in RayTraceScene.
		if (task->emissiveCount > 0 && x % INDIRECT_RESOLUTION == 0) {
			catchIndirectSample = (float3){0.0f, 0.0f, 0.0f};
			for (int ei = 0; ei < task->emissiveCount; ei++) {
				int li = task->emissiveObjects[ei];
				if (li == bestObj) continue;

				float3 emitHitPos;
				float3 toEmitter = RandomObjectHitRay(&objects[li], sOrig,
					camera->seed + (float)x * 0.37f + (float)ei, &emitHitPos);

				// Cosine term — skip if emitter is behind the surface
				float NdotL = n.x * toEmitter.x + n.y * toEmitter.y + n.z * toEmitter.z;
				if (NdotL <= 0.0f) continue;

				// Discard if another object is between surface and emitter.
				// Full rayCollision finds the closest hit — if it's not the emitter, it's occluded.
				int hitObj = -1, hitTri = -1;
				float3 hitPos;
				rayCollision((Object *)objects, objectCount, sOrig, toEmitter, bestObj, &hitObj, &hitTri, &hitPos);
				if (hitObj != li || hitTri < 0) continue;

				// Distance-based falloff using precomputed world center
				float3 dv = {objects[li].worldCenter.x - sOrig.x,
					objects[li].worldCenter.y - sOrig.y,
					objects[li].worldCenter.z - sOrig.z};
				float distSq = dv.x * dv.x + dv.y * dv.y + dv.z * dv.z;
				float atten = NdotL / (1.0f + distSq * 0.05f);

				// Sample emission via 6-direction lerp using the hit triangle's local normal.
				// Local normals are precomputed in Object_PrecomputeEmission — no world transform needed.
				float3 emi = SampleObjectEmission(&objects[li], objects[li].normals[hitTri]);
				catchIndirectSample.x += emi.x * atten;
				catchIndirectSample.y += emi.y * atten;
				catchIndirectSample.z += emi.z * atten;
			}
		}
		catchIndirect[x] = catchIndirectSample;
	}

	// Post-pass: blur shadow + apply, blur indirect + add, blur reflections + blend, gamma.
	for (int x = 0; x < width; x++) {
		int pidx = row * width + x;
		if (camera->depthBuffer[pidx] >= DEPTH_FAR) continue;

		// --- Shadow blur + apply ---
		float shadowBlurred = 0.0f;
		int sc = 0;
		for (int k = x - BLUR_RADIUS; k <= x + BLUR_RADIUS; k++) {
			if ((unsigned)k < (unsigned)width) { shadowBlurred += catchShadows[k]; sc++; }
		}
		if (sc > 0) shadowBlurred /= sc;
		// Multiplicative shadow darkening: scale in [128,256]/256 → [0.5, 1.0] range
		uint32 shadowScale = 128 + (uint32)(shadowBlurred * 128.0f);
		Color fc = camera->framebuffer[pidx];
		uint32 sr = (((fc >> 16) & 0xFF) * shadowScale) >> 8;
		uint32 sg = (((fc >> 8) & 0xFF) * shadowScale) >> 8;
		uint32 sb = ((fc & 0xFF) * shadowScale) >> 8;
		camera->framebuffer[pidx] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;

		// --- Indirect light blur + add ---
		float3 indBlurred = {0.0f, 0.0f, 0.0f};
		int ic = 0;
		for (int k = x - INDIRECT_BLUR_RADIUS; k <= x + INDIRECT_BLUR_RADIUS; k++) {
			if ((unsigned)k < (unsigned)width) {
				indBlurred.x += catchIndirect[k].x;
				indBlurred.y += catchIndirect[k].y;
				indBlurred.z += catchIndirect[k].z;
				ic++;
			}
		}
		if (ic > 0) {
			float iinv = 1.0f / ic;
			indBlurred.x *= iinv;
			indBlurred.y *= iinv;
			indBlurred.z *= iinv;
		}
		// Store raw indirect emission for later bloom pass
		camera->emissionBuffer[pidx] = indBlurred;
		// Additive blend into framebuffer
		if (indBlurred.x > 0.0f || indBlurred.y > 0.0f || indBlurred.z > 0.0f) {
			Color ifc = camera->framebuffer[pidx];
			uint32 ir = ((ifc >> 16) & 0xFF) + (uint32)(indBlurred.x * 255.0f);
			uint32 ig = ((ifc >> 8) & 0xFF) + (uint32)(indBlurred.y * 255.0f);
			uint32 ib = (ifc & 0xFF) + (uint32)(indBlurred.z * 255.0f);
			if (ir > 255) ir = 255;
			if (ig > 255) ig = 255;
			if (ib > 255) ib = 255;
			camera->framebuffer[pidx] = 0xFF000000u | (ir << 16) | (ig << 8) | ib;
		}

		// --- Reflection blur + blend ---
		float reflectionStrength = camera->reflectBuffer[pidx].w;
		float3 accumulatedColor = {0.0f, 0.0f, 0.0f};
		int samples = 0;
		for (int kernelSize = BLUR_RADIUS * 2 + 1; kernelSize > 0; kernelSize--) {
			int kIdx = x + kernelSize - BLUR_RADIUS - 1;
			if (kIdx >= 0 && kIdx < width) {
				accumulatedColor.x += catchReflections[kIdx].x;
				accumulatedColor.y += catchReflections[kIdx].y;
				accumulatedColor.z += catchReflections[kIdx].z;
				samples++;
			}
		}
		if (samples > 0) {
			float invW = 1.0f / samples;
			accumulatedColor.x *= invW;
			accumulatedColor.y *= invW;
			accumulatedColor.z *= invW;
		}
		uint32 rr = (uint32)(fminf(accumulatedColor.x, 1.0f) * 255.0f);
		uint32 rg = (uint32)(fminf(accumulatedColor.y, 1.0f) * 255.0f);
		uint32 rb = (uint32)(fminf(accumulatedColor.z, 1.0f) * 255.0f);
		Color accumColor = 0xFF000000u | (rr << 16) | (rg << 8) | rb;
		Color baseColor = camera->framebuffer[pidx];
		camera->framebuffer[pidx] = LerpColor(baseColor, accumColor, reflectionStrength * reflectionStrength);
		camera->framebuffer[pidx] = ApplyGamma(camera->framebuffer[pidx], 1.2f);
		camera->framebuffer[pidx] = ScaleChannel(camera->framebuffer[pidx], 1.08f, 1.0f, 0.78f);
	}
}

void RayTraceScene(const Object *objects, int objectCount, Camera *camera, const MaterialLib *lib, RayTraceTaskQueue *taskQueue, ThreadPool *threadPool, const Skybox *skybox) {
	if (!objects || objectCount <= 0 || !camera || !taskQueue || !threadPool) return;

	// Precompute emissive object list once per frame — avoids scanning all objects per pixel
	int emissiveObjects[objectCount];
	int emissiveCount = 0;
	for (int i = 0; i < objectCount; i++) {
		if (objects[i].hasEmission)
			emissiveObjects[emissiveCount++] = i;
	}

	Frustum frustum = Frustum_FromCamera(camera);
	for (int row = 0; row < camera->screenHeight; row++) {
		taskQueue->tasks[row] = (RayTraceTask){row, camera, objects, objectCount, lib, skybox, frustum, emissiveObjects, emissiveCount};
		poolAdd(threadPool, RayTraceRowFunc, &taskQueue->tasks[row]);
	}
	poolWait(threadPool);
}

void DitherPostProcess(Camera *camera, int frame) {
	if (!camera) return;
	int width = camera->screenWidth;
	int height = camera->screenHeight;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int idx = y * width + x;
			Color c = camera->framebuffer[idx];
			int r = (c >> 16) & 0xFF;
			int g = (c >> 8) & 0xFF;
			int b = c & 0xFF;

			// per-pixel per-frame hash — no spatial or temporal coherence
			uint32 h = (uint32)(x * 1619 + y * 31337 + frame * 6791);
			h ^= h >> 16;
			h *= 0x45d9f3bu;
			h ^= h >> 16;
			int bias = (int)(h & 0xFF) - 128;
			bias = bias * 24 / 128; // scale to [-24, +24]

			r += bias;
			if (r < 0)
				r = 0;
			else if (r > 255)
				r = 255;
			g += bias;
			if (g < 0)
				g = 0;
			else if (g > 255)
				g = 255;
			b += bias;
			if (b < 0)
				b = 0;
			else if (b > 255)
				b = 255;

			camera->framebuffer[idx] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
		}
	}
}

void DitherOrderedPostProcess(Camera *camera, int frame) {
	if (!camera) return;
	int width = camera->screenWidth;
	int height = camera->screenHeight;

	// Bayer 4x4 matrix, normalized to [0,1]
	static const float bayer[4][4] = {
		{0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f},
		{12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f},
		{3.0f / 16.0f, 11.0f / 16.0f, 1.0f / 16.0f, 9.0f / 16.0f},
		{15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f, 5.0f / 16.0f},
	};

	// slide pattern upward 1 row per frame — wraps naturally via & 3
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int idx = y * width + x;
			Color c = camera->framebuffer[idx];
			int r = (c >> 16) & 0xFF;
			int g = (c >> 8) & 0xFF;
			int b = c & 0xFF;

			float t = bayer[(y + frame) & 3][x & 3];
			int bias = (int)(t * 48.0f) - 24; // [-24, +24] range (~9%)

			r += bias;
			if (r < 0)
				r = 0;
			else if (r > 255)
				r = 255;
			g += bias;
			if (g < 0)
				g = 0;
			else if (g > 255)
				g = 255;
			b += bias;
			if (b < 0)
				b = 0;
			else if (b > 255)
				b = 255;

			camera->framebuffer[idx] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
		}
	}
}