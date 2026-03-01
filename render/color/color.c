#include "color.h"
#include "../../math/scalar.h"
#include "../../object/format.h"

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

void VisualizeBuffer(const Camera *camera, int mode) {
	if (mode == 0) return; // VIEW_COLOR
	const int n = camera->screenWidth * camera->screenHeight;

	if (mode == 1) { // VIEW_NORMALS
		for (int i = 0; i < n; i++) {
			float3 nm = camera->normalBuffer[i];
			camera->framebuffer[i] = PackColorSafe(
				nm.x * 0.5f + 0.5f,
				nm.y * 0.5f + 0.5f,
				nm.z * 0.5f + 0.5f);
		}
		return;
	}

	if (mode == 3) { // VIEW_REFLECT
		for (int i = 0; i < n; i++) {
			float3 r = camera->reflectBuffer[i];
			camera->framebuffer[i] = PackColorSafe(
				r.x * 0.5f + 0.5f,
				r.y * 0.5f + 0.5f,
				r.z * 0.5f + 0.5f);
		}
		return;
	}

	if (mode == 2) { // VIEW_DEPTH
		float dMin = 1e38f, dMax = 0.0f;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			if (d > 0.0f && d < 1e38f) {
				if (d < dMin) dMin = d;
				if (d > dMax) dMax = d;
			}
		}
		float range = dMax - dMin;
		if (range < 1e-6f) range = 1.0f;
		for (int i = 0; i < n; i++) {
			float d = camera->depthBuffer[i];
			float t = (d <= 0.0f || d >= 1e38f) ? 0.0f : 1.0f - (d - dMin) / range;
			camera->framebuffer[i] = PackColorSafe(t, t, t);
		}
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
