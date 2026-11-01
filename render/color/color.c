#include "color.h"
#include "../../math/scalar.h"
#include "../../object/format.h"
#include <immintrin.h>

Color PackColor(float r, float g, float b) {
	uint8 r8 = (uint8)(r * 255.0f);
	uint8 g8 = (uint8)(g * 255.0f);
	uint8 b8 = (uint8)(b * 255.0f);
	return (Color)((r8 << 16) | (g8 << 8) | b8);
}

Color PackColorSafe(float r, float g, float b) {
	uint8 R = (uint8)(r > 1.0f ? 255 : r < 0.0f ? 0
												: (int)(r * 255.0f));
	uint8 G = (uint8)(g > 1.0f ? 255 : g < 0.0f ? 0
												: (int)(g * 255.0f));
	uint8 B = (uint8)(b > 1.0f ? 255 : b < 0.0f ? 0
												: (int)(b * 255.0f));
	return 0xFF000000 | (Color)(R << 16) | (Color)(G << 8) | B;
}

// Single HDR -> LDR stage for the whole frame. Tonemap matches the old per-pixel
// hdrToLDR: overflow spreads from the max channel into the others, then clamps; no gamma.
static inline uint32 ConvertPixelToLdr(float3 c) {
	float e = fmaxf(c.x, fmaxf(c.y, c.z)) - 1.0f;
	if (e > 0.0f) {
		c.x = fminf(c.x + e, 1.0f);
		c.y = fminf(c.y + e, 1.0f);
		c.z = fminf(c.z + e, 1.0f);
	}
	return PackColorSafe(c.x, c.y, c.z);
}

void ConvertToLdr(const float3 *inHdr, uint32 *outLdr, int width, int height) {
	if (!inHdr || !outLdr) return;
	const int count = width * height;
	for (int i = 0; i < count; i++)
		outLdr[i] = ConvertPixelToLdr(inHdr[i]);
}

typedef struct {
	const float3 *inHdr;
	uint32 *outLdr;
	int startRow;
	int rows;
	int width;
} ConvertToLdrTask;

static void ConvertToLdrRows(void *arg) {
	const ConvertToLdrTask *task = arg;
	const int start = task->startRow * task->width;
	const int count = task->rows * task->width;
	int i = 0;
	const __m128 zero = _mm_setzero_ps();
	const __m128 one = _mm_set1_ps(1.0f);
	const __m128 s255 = _mm_set1_ps(255.0f);
	const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);
	for (; i + 4 <= count; i += 4) {
		__m128 p0 = _mm_loadu_ps(&task->inHdr[start + i + 0].x);
		__m128 p1 = _mm_loadu_ps(&task->inHdr[start + i + 1].x);
		__m128 p2 = _mm_loadu_ps(&task->inHdr[start + i + 2].x);
		__m128 p3 = _mm_loadu_ps(&task->inHdr[start + i + 3].x);
		_MM_TRANSPOSE4_PS(p0, p1, p2, p3);
		__m128 mx = _mm_max_ps(p0, _mm_max_ps(p1, p2));
		__m128 e = _mm_sub_ps(mx, one);
		__m128 add = _mm_and_ps(e, _mm_cmpgt_ps(e, zero));
		__m128 x = _mm_min_ps(_mm_max_ps(_mm_add_ps(p0, add), zero), one);
		__m128 y = _mm_min_ps(_mm_max_ps(_mm_add_ps(p1, add), zero), one);
		__m128 z = _mm_min_ps(_mm_max_ps(_mm_add_ps(p2, add), zero), one);
		__m128i xi = _mm_cvttps_epi32(_mm_mul_ps(x, s255));
		__m128i yi = _mm_cvttps_epi32(_mm_mul_ps(y, s255));
		__m128i zi = _mm_cvttps_epi32(_mm_mul_ps(z, s255));
		__m128i packed = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(xi, 16), _mm_slli_epi32(yi, 8)), zi);
		_mm_storeu_si128((__m128i *)&task->outLdr[start + i], _mm_or_si128(packed, alpha));
	}
	for (; i < count; i++)
		task->outLdr[start + i] = ConvertPixelToLdr(task->inHdr[start + i]);
}

void ConvertToLdrMp(const float3 *inHdr, uint32 *outLdr, int width, int height, ThreadPool *threadPool) {
	if (!inHdr || !outLdr || !threadPool || width <= 0 || height <= 0) return;
	const int rowsPerTask = 45;
	const int taskCount = (height + rowsPerTask - 1) / rowsPerTask;
	ConvertToLdrTask tasks[taskCount];
	for (int t = 0; t < taskCount; t++) {
		const int row = t * rowsPerTask;
		const int rows = height - row < rowsPerTask ? height - row : rowsPerTask;
		tasks[t] = (ConvertToLdrTask){inHdr, outLdr, row, rows, width};
		poolAdd(threadPool, ConvertToLdrRows, &tasks[t]);
	}
	poolWait(threadPool);
}

void VisualizeBuffer(const Camera *camera, int mode) {
	if (mode == 0) return; // VIEW_COLOR
	const int n = camera->screenWidth * camera->screenHeight;

	if (mode == 1) { // VIEW_NORMALS
		for (int i = 0; i < n; i++) {
			float3 nm = camera->normalBuffer[i];
			camera->framebufferLdr[i] = PackColorSafe(
				nm.x * 0.5f + 0.5f,
				nm.y * 0.5f + 0.5f,
				nm.z * 0.5f + 0.5f);
		}
		return;
	}

	if (mode == 3) { // VIEW_REFLECT
		for (int i = 0; i < n; i++) {
			float3 r = camera->reflectBuffer[i];
			camera->framebufferLdr[i] = PackColorSafe(
				r.x * 0.5f + 0.5f,
				r.y * 0.5f + 0.5f,
				r.z * 0.5f + 0.5f);
		}
		return;
	}

	if (mode == 2) { // VIEW_DEPTH
		float dMin = DEPTH_FAR, dMax = 0.0f;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d > 0.0f && d < DEPTH_FAR) {
				if (d < dMin) dMin = d;
				if (d > dMax) dMax = d;
			}
		}
		float range = dMax - dMin;
		if (range < 1e-6f) range = 1.0f;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			float t = (d <= 0.0f || d >= DEPTH_FAR) ? 0.0f : 1.0f - (d - dMin) / range;
			camera->framebufferLdr[i] = PackColorSafe(t, t, t);
		}
		return;
	}

	if (mode == 4) { // VIEW_POSITION
		float3 pMin = {DEPTH_FAR, DEPTH_FAR, DEPTH_FAR};
		float3 pMax = {-DEPTH_FAR, -DEPTH_FAR, -DEPTH_FAR};
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) continue;
			float3 p = camera->positionBuffer[i];
			if (p.x < pMin.x) pMin.x = p.x;
			if (p.y < pMin.y) pMin.y = p.y;
			if (p.z < pMin.z) pMin.z = p.z;
			if (p.x > pMax.x) pMax.x = p.x;
			if (p.y > pMax.y) pMax.y = p.y;
			if (p.z > pMax.z) pMax.z = p.z;
		}
		float3 invRange = {
			pMax.x > pMin.x ? 1.0f / (pMax.x - pMin.x) : 0.0f,
			pMax.y > pMin.y ? 1.0f / (pMax.y - pMin.y) : 0.0f,
			pMax.z > pMin.z ? 1.0f / (pMax.z - pMin.z) : 0.0f};
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			float3 p = camera->positionBuffer[i];
			camera->framebufferLdr[i] = PackColorSafe(
				(p.x - pMin.x) * invRange.x,
				(p.y - pMin.y) * invRange.y,
				(p.z - pMin.z) * invRange.z);
		}
		return;
	}

	if (mode == 5) { // VIEW_OBJECT_ID
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			uint32 h = (uint32)camera->objectIdBuffer[i] * 2654435761u;
			h ^= h >> 13;
			camera->framebufferLdr[i] = 0xFF000000u | ((h & 0xFF) << 16) | (h & 0xFF00) | ((h >> 16) & 0xFF);
		}
		return;
	}

	if (mode == 6) { // VIEW_TRIANGLE_ID
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			uint32 h = (uint32)camera->triangleIdBuffer[i] * 2654435761u;
			h ^= h >> 13;
			camera->framebufferLdr[i] = 0xFF000000u | ((h & 0xFF) << 16) | (h & 0xFF00) | ((h >> 16) & 0xFF);
		}
		return;
	}

	if (mode == 7) { // VIEW_UV
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			uvMap uv = camera->uvBuffer[i];
			camera->framebufferLdr[i] = 0xFF000000u | (((uint32)uv.x >> 8) << 16) | (((uint32)uv.y >> 8) << 8);
		}
		return;
	}

	if (mode == 8) { // VIEW_EMISSION
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			float3 e = camera->bloomBuffer[i];
			camera->framebufferLdr[i] = PackColorSafe(e.x * 100.0f, e.y * 100.0f, e.z * 100.0f);
		}
		return;
	}

	if (mode == 9) { // VIEW_MOTION
		float maxAbs = 1e-6f;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) continue;
			float2 m = camera->motionVectorBuffer[i];
			if (fabsf(m.x) > maxAbs) maxAbs = fabsf(m.x);
			if (fabsf(m.y) > maxAbs) maxAbs = fabsf(m.y);
		}
		float scale = 0.5f / maxAbs;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d <= 0.0f || d >= DEPTH_FAR) {
				camera->framebufferLdr[i] = 0xFF000000u;
				continue;
			}
			float2 m = camera->motionVectorBuffer[i];
			camera->framebufferLdr[i] = PackColorSafe(m.x * scale + 0.5f, m.y * scale + 0.5f, 0.0f);
		}
		return;
	}
}

Color PackColorF(float3 Color) {
	return PackColor(Color.x, Color.y, Color.z);
}

Color BlendColors(Color c1, Color c2, float t) {
	float3 col1 = UnpackColor(c1);
	float3 col2 = UnpackColor(c2);
	float3 blended = {
		col1.x * (1.0f - t) + col2.x * t,
		col1.y * (1.0f - t) + col2.y * t,
		col1.z * (1.0f - t) + col2.z * t};
	return PackColorF(blended);
}

float3 UnpackColor(Color c) {
	float r = ((c >> 16) & 0xFF) / 255.0f;
	float g = ((c >> 8) & 0xFF) / 255.0f;
	float b = (c & 0xFF) / 255.0f;
	return (float3){r, g, b, 0.0f};
}

int4 UnpackColorInt(Color c) {
	int r = (c >> 16) & 0xFF;
	int g = (c >> 8) & 0xFF;
	int b = c & 0xFF;
	return (int4){r, g, b, 0};
}

Color ApplyGamma(Color c, float gamma) {
	float3 col = UnpackColor(c);
	col.x = powf(col.x, 1.0f / gamma);
	col.y = powf(col.y, 1.0f / gamma);
	col.z = powf(col.z, 1.0f / gamma);
	return PackColorF(col);
}

Color ApplyExposure(Color c, float exposure) {
	float3 col = UnpackColor(c);
	float expFactor = powf(2.0f, exposure);
	col.x *= expFactor;
	col.y *= expFactor;
	col.z *= expFactor;
	return PackColorF(col);
}

Color ApplyToneMapping(Color c) {
	float3 col = UnpackColor(c);
	col.x = col.x / (col.x + 1.0f);
	col.y = col.y / (col.y + 1.0f);
	col.z = col.z / (col.z + 1.0f);
	return PackColorF(col);
}

Color AdjustContrast(Color c, float contrast) {
	float3 col = UnpackColor(c);
	col.x = (col.x - 0.5f) * contrast + 0.5f;
	col.y = (col.y - 0.5f) * contrast + 0.5f;
	col.z = (col.z - 0.5f) * contrast + 0.5f;
	return PackColorF(col);
}

Color ApplyColorCorrection(Color c, float gamma, float exposure, float contrast, float saturation, float hueShift, float3 modulate) {
	Color result = ApplyGamma(c, gamma);
	result = ApplyExposure(result, exposure);
	result = ApplyToneMapping(result);
	result = AdjustContrast(result, contrast);
	result = AdjustSaturation(result, saturation);
	result = HueShiftColor(result, hueShift);
	result = ModulateColorF(result, modulate);
	return result;
}

Color ScaleChannel(Color c, float rScale, float gScale, float bScale) {
	float3 col = UnpackColor(c);
	col.x *= rScale;
	col.y *= gScale;
	col.z *= bScale;
	return PackColorF(col);
}

Color LerpColor(Color c1, Color c2, float t) {
	float3 col1 = UnpackColor(c1);
	float3 col2 = UnpackColor(c2);
	float3 lerped = {
		col1.x * (1.0f - t) + col2.x * t,
		col1.y * (1.0f - t) + col2.y * t,
		col1.z * (1.0f - t) + col2.z * t};
	return PackColorF(lerped);
}

Color ClampColor(Color c) {
	float3 col = UnpackColor(c);
	col.x = MinF32(1.0f, MaxF32(0.0f, col.x));
	col.y = MinF32(1.0f, MaxF32(0.0f, col.y));
	col.z = MinF32(1.0f, MaxF32(0.0f, col.z));
	return PackColorF(col);
}

Color AddColors(Color c1, Color c2) {
	float3 col1 = UnpackColor(c1);
	float3 col2 = UnpackColor(c2);
	float3 added = {
		col1.x + col2.x,
		col1.y + col2.y,
		col1.z + col2.z};
	return PackColorF(added);
}

Color SubtractColors(Color c1, Color c2) {
	float3 col1 = UnpackColor(c1);
	float3 col2 = UnpackColor(c2);
	float3 subtracted = {
		col1.x - col2.x,
		col1.y - col2.y,
		col1.z - col2.z};
	return PackColorF(subtracted);
}

Color MultiplyColors(Color c1, Color c2) {
	float3 col1 = UnpackColor(c1);
	float3 col2 = UnpackColor(c2);
	float3 multiplied = {
		col1.x * col2.x,
		col1.y * col2.y,
		col1.z * col2.z};
	return PackColorF(multiplied);
}

Color ScaleColor(Color c, float s) {
	float3 col = UnpackColor(c);
	col.x *= s;
	col.y *= s;
	col.z *= s;
	return PackColorF(col);
}

Color ModulateColor(Color c, float r, float g, float b) {
	float3 col = UnpackColor(c);
	col.x *= r;
	col.y *= g;
	col.z *= b;
	return PackColorF(col);
}

Color ModulateColorF(Color c, float3 mod) {
	float3 col = UnpackColor(c);
	col.x *= mod.x;
	col.y *= mod.y;
	col.z *= mod.z;
	return PackColorF(col);
}

Color InvertColor(Color c) {
	float3 col = UnpackColor(c);
	col.x = 1.0f - col.x;
	col.y = 1.0f - col.y;
	col.z = 1.0f - col.z;
	return PackColorF(col);
}

Color GrayscaleColor(Color c) {
	float3 col = UnpackColor(c);
	float gray = 0.299f * col.x + 0.587f * col.y + 0.114f * col.z;
	return PackColor(gray, gray, gray);
}

Color DesaturateColor(Color c, float amount) {
	float3 col = UnpackColor(c);
	float gray = 0.299f * col.x + 0.587f * col.y + 0.114f * col.z;
	col.x = col.x * (1.0f - amount) + gray * amount;
	col.y = col.y * (1.0f - amount) + gray * amount;
	col.z = col.z * (1.0f - amount) + gray * amount;
	return PackColorF(col);
}

Color HueShiftColor(Color c, float shift) {
	float3 col = UnpackColor(c);
	float r = col.x;
	float g = col.y;
	float b = col.z;

	float max = MaxF32(r, MaxF32(g, b));
	float min = MinF32(r, MinF32(g, b));
	float delta = max - min;

	if (delta < 1e-5f) return c; // No hue

	float hue;
	if (max == r) {
		hue = (g - b) / delta + (g < b ? 6.0f : 0.0f);
	} else if (max == g) {
		hue = (b - r) / delta + 2.0f;
	} else {
		hue = (r - g) / delta + 4.0f;
	}
	hue /= 6.0f;

	hue += shift;
	if (hue < 0.0f) hue += 1.0f;
	if (hue >= 1.0f) hue -= 1.0f;

	// Convert back to RGB
	int i = (int)(hue * 6.0f);
	float f = hue * 6.0f - i;
	float p = max * (1.0f - delta);
	float q = max * (1.0f - f * delta);
	float t = max * (1.0f - (1.0f - f) * delta);

	switch (i % 6) {
	case 0:
		r = max;
		g = t;
		b = p;
		break;
	case 1:
		r = q;
		g = max;
		b = p;
		break;
	case 2:
		r = p;
		g = max;
		b = t;
		break;
	case 3:
		r = p;
		g = q;
		b = max;
		break;
	case 4:
		r = t;
		g = p;
		b = max;
		break;
	case 5:
		r = max;
		g = p;
		b = q;
		break;
	}

	return PackColor(r, g, b);
}

Color AdjustSaturation(Color c, float saturation) {
	float3 col = UnpackColor(c);
	float gray = 0.299f * col.x + 0.587f * col.y + 0.114f * col.z;
	col.x = col.x * saturation + gray * (1.0f - saturation);
	col.y = col.y * saturation + gray * (1.0f - saturation);
	col.z = col.z * saturation + gray * (1.0f - saturation);
	return PackColorF(col);
}

Color QuantizeColor(Color c, int levels) {
	float3 col = UnpackColor(c);
	col.x = roundf(col.x * (levels - 1)) / (levels - 1);
	col.y = roundf(col.y * (levels - 1)) / (levels - 1);
	col.z = roundf(col.z * (levels - 1)) / (levels - 1);
	return PackColorF(col);
}

Color DarkenColor(Color c, float amount) {
	float3 col = UnpackColor(c);
	float factor = 1.0f - amount;
	col.x *= factor;
	col.y *= factor;
	col.z *= factor;
	return PackColorF(col);
}

// Operates on the .bin texel layout (R in bits 0-7, G 8-15, B 16-23, A 24-31)
// so it mirrors boostColor in tools/parseObj.go, alpha included.
Color BoostColor(Color c, float satAmount, float brightFactor) {
	if (satAmount == 1.0f && brightFactor == 1.0f) return c;

	float rf = (float)(c & 0xFF) / 255.0f;
	float gf = (float)((c >> 8) & 0xFF) / 255.0f;
	float bf = (float)((c >> 16) & 0xFF) / 255.0f;
	uint32 alpha = (c >> 24) & 0xFF;
	float lum = 0.299f * rf + 0.587f * gf + 0.114f * bf;

	float red = fminf(fmaxf((lum + satAmount * (rf - lum)) * brightFactor, 0.0f), 1.0f);
	float green = fminf(fmaxf((lum + satAmount * (gf - lum)) * brightFactor, 0.0f), 1.0f);
	float blue = fminf(fmaxf((lum + satAmount * (bf - lum)) * brightFactor, 0.0f), 1.0f);

	uint32 r8 = (uint32)(red * 255.0f);
	uint32 g8 = (uint32)(green * 255.0f);
	uint32 b8 = (uint32)(blue * 255.0f);
	return r8 | (g8 << 8) | (b8 << 16) | (alpha << 24);
}