#ifndef IMG_METHODS_H
#define IMG_METHODS_H

#include "../object/format.h"
#include <math.h>
#include "../render/color/color.h"

static inline void DecimateBuffer(float3 *src, float3 *dst, int width, int height, int factor) {
	int dstWidth = width / factor;
	int dstHeight = height / factor;
	float invSamples = 1.0f / (float)(factor * factor);
	for (int y = 0; y < dstHeight; y++) {
		for (int x = 0; x < dstWidth; x++) {
			float3 accum = {0.0f, 0.0f, 0.0f};
			for (int ky = 0; ky < factor; ky++) {
				const float3 *row = src + (y * factor + ky) * width + x * factor;
				for (int kx = 0; kx < factor; kx++) {
					accum.x += row[kx].x;
					accum.y += row[kx].y;
					accum.z += row[kx].z;
				}
			}
			dst[y * dstWidth + x] = (float3){accum.x * invSamples, accum.y * invSamples, accum.z * invSamples};
		}
	}
}

// tmp must point to srcHeight * dstWidth float3 elements
static inline void UpsampleBilinear(float3 *src, float3 *tmp, float3 *dst,
									int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
	// horizontal pass: src (srcHeight x srcWidth) -> tmp (srcHeight x dstWidth)
	float scaleX = (float)(srcWidth - 1) / (float)(dstWidth - 1);
	float scaleY = (float)(srcHeight - 1) / (float)(dstHeight - 1);
	for (int y = 0; y < srcHeight; y++) {
		const float3 *srow = src + y * srcWidth;
		float3 *trow = tmp + y * dstWidth;
		for (int x = 0; x < dstWidth; x++) {
			float u = x * scaleX;
			int x0 = (int)u;
			int x1 = x0 + 1 < srcWidth ? x0 + 1 : x0;
			float t = u - x0;
			float it = 1.0f - t;
			trow[x] = (float3){srow[x0].x * it + srow[x1].x * t,
							   srow[x0].y * it + srow[x1].y * t,
							   srow[x0].z * it + srow[x1].z * t};
		}
	}
	// vertical pass: tmp (srcHeight x dstWidth) -> dst (dstHeight x dstWidth)
	for (int y = 0; y < dstHeight; y++) {
		float v = y * scaleY;
		int y0 = (int)v;
		int y1 = y0 + 1 < srcHeight ? y0 + 1 : y0;
		float t = v - y0;
		float it = 1.0f - t;
		const float3 *r0 = tmp + y0 * dstWidth;
		const float3 *r1 = tmp + y1 * dstWidth;
		float3 *drow = dst + y * dstWidth;
		for (int x = 0; x < dstWidth; x++) {
			drow[x] = (float3){r0[x].x * it + r1[x].x * t,
							   r0[x].y * it + r1[x].y * t,
							   r0[x].z * it + r1[x].z * t};
		}
	}
}

// tmp must point to width * height float3 elements
static inline void BoxBlur3x3(float3 *src, float3 *tmp, float3 *dst, int width, int height) {
	// horizontal pass: 1x3 box into tmp
	for (int y = 0; y < height; y++) {
		const float3 *row = src + y * width;
		float3 *trow = tmp + y * width;
		trow[0] = (float3){(row[0].x + row[1].x) * 0.5f,
						   (row[0].y + row[1].y) * 0.5f,
						   (row[0].z + row[1].z) * 0.5f};
		for (int x = 1; x < width - 1; x++) {
			trow[x] = (float3){(row[x - 1].x + row[x].x + row[x + 1].x) * (1.0f / 3.0f),
							   (row[x - 1].y + row[x].y + row[x + 1].y) * (1.0f / 3.0f),
							   (row[x - 1].z + row[x].z + row[x + 1].z) * (1.0f / 3.0f)};
		}
		trow[width - 1] = (float3){(row[width - 2].x + row[width - 1].x) * 0.5f,
								   (row[width - 2].y + row[width - 1].y) * 0.5f,
								   (row[width - 2].z + row[width - 1].z) * 0.5f};
	}
	// vertical pass: 3x1 box into dst — top row
	for (int x = 0; x < width; x++) {
		dst[x] = (float3){(tmp[x].x + tmp[width + x].x) * 0.5f,
						  (tmp[x].y + tmp[width + x].y) * 0.5f,
						  (tmp[x].z + tmp[width + x].z) * 0.5f};
	}
	// interior rows
	for (int y = 1; y < height - 1; y++) {
		const float3 *ta = tmp + (y - 1) * width;
		const float3 *tb = tmp + y * width;
		const float3 *tc = tmp + (y + 1) * width;
		float3 *drow = dst + y * width;
		for (int x = 0; x < width; x++) {
			drow[x] = (float3){(ta[x].x + tb[x].x + tc[x].x) * (1.0f / 3.0f),
							   (ta[x].y + tb[x].y + tc[x].y) * (1.0f / 3.0f),
							   (ta[x].z + tb[x].z + tc[x].z) * (1.0f / 3.0f)};
		}
	}
	// bottom row
	const float3 *ta = tmp + (height - 2) * width;
	const float3 *tb = tmp + (height - 1) * width;
	float3 *drow = dst + (height - 1) * width;
	for (int x = 0; x < width; x++) {
		drow[x] = (float3){(ta[x].x + tb[x].x) * 0.5f,
						   (ta[x].y + tb[x].y) * 0.5f,
						   (ta[x].z + tb[x].z) * 0.5f};
	}
}

#endif // IMG_METHODS_H