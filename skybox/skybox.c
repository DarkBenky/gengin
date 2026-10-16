#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include "skybox.h"

static uint32 *loadJpeg(const char *path, int *outWidth, int *outHeight) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "Skybox: cannot open %s\n", path);
		return NULL;
	}

	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	int w = (int)cinfo.output_width;
	int h = (int)cinfo.output_height;
	uint32 *pixels = malloc((size_t)w * h * sizeof(uint32));
	if (!pixels) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}

	unsigned char *row = malloc((size_t)w * 3);
	while ((int)cinfo.output_scanline < h) {
		JSAMPROW rowPtr = row;
		jpeg_read_scanlines(&cinfo, &rowPtr, 1);
		int y = (int)cinfo.output_scanline - 1;
		for (int x = 0; x < w; x++) {
			unsigned char r = row[x * 3 + 0];
			unsigned char g = row[x * 3 + 1];
			unsigned char b = row[x * 3 + 2];
			pixels[y * w + x] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | b;
		}
	}
	free(row);
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);

	*outWidth  = w;
	*outHeight = h;
	return pixels;
}

static uint32 *loadFace(const char *dir, const char *name, int *w, int *h) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.jpg", dir, name);
	return loadJpeg(path, w, h);
}

// The source cube is 2048^2, i.e. 16 MB per face / 96 MB in total, and the
// lookups are footprint-bound (bench/skyres: 10.1 ns/lookup at 2048^2, 7.0 at
// 1024^2, 5.3 at 512^2).  A box-prefiltered SKY_DOWNSAMPLE-square cube is the
// cheapest sky that still resolves the source.
#define SKY_DOWNSAMPLE 2

static uint32 *downsampleFace(const uint32 *src, int w, int h) {
	int nw = w / SKY_DOWNSAMPLE, nh = h / SKY_DOWNSAMPLE;
	if (nw <= 0 || nh <= 0) return NULL;
	uint32 *dst = malloc((size_t)nw * nh * sizeof(uint32));
	if (!dst) return NULL;
	const uint32 n = SKY_DOWNSAMPLE * SKY_DOWNSAMPLE;
	for (int y = 0; y < nh; y++) {
		for (int x = 0; x < nw; x++) {
			uint32 r = 0, g = 0, b = 0;
			for (int dy = 0; dy < SKY_DOWNSAMPLE; dy++) {
				const uint32 *row = src + (size_t)(y * SKY_DOWNSAMPLE + dy) * w + x * SKY_DOWNSAMPLE;
				for (int dx = 0; dx < SKY_DOWNSAMPLE; dx++) {
					r += (row[dx] >> 16) & 0xFF;
					g += (row[dx] >> 8) & 0xFF;
					b += row[dx] & 0xFF;
				}
			}
			dst[(size_t)y * nw + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
		}
	}
	return dst;
}

void LoadSkybox(Skybox *skybox, const char *directory) {
	if (!skybox || !directory) return;
	memset(skybox, 0, sizeof(*skybox));

	static const char *names[6] = {"front", "back", "left", "right", "top", "bottom"};
	uint32 **slots[6] = {&skybox->front, &skybox->back, &skybox->left,
	                     &skybox->right, &skybox->top, &skybox->bottom};

	int w = 0, h = 0, uniform = 1;
	for (int i = 0; i < 6; i++) {
		int fw = 0, fh = 0;
		*slots[i] = loadFace(directory, names[i], &fw, &fh);
		if (i == 0) {
			w = fw;
			h = fh;
		} else if (!*slots[i] || fw != w || fh != h) {
			uniform = 0;
		}
	}
	if (!skybox->front || w <= 0 || h <= 0) uniform = 0;

	if (uniform && w % SKY_DOWNSAMPLE == 0 && h % SKY_DOWNSAMPLE == 0) {
		uint32 *small[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
		int ok = 1;
		for (int i = 0; i < 6; i++) {
			if (!(small[i] = downsampleFace(*slots[i], w, h))) { ok = 0; break; }
		}
		if (ok) {
			for (int i = 0; i < 6; i++) {
				free(*slots[i]);
				*slots[i] = small[i];
			}
			w /= SKY_DOWNSAMPLE;
			h /= SKY_DOWNSAMPLE;
		} else {
			for (int i = 0; i < 6; i++) free(small[i]);
		}
	}
	skybox->imageWidth = w;
	skybox->imageHeight = h;
}

void DestroySkybox(Skybox *skybox) {
	if (!skybox) return;
	free(skybox->front);  skybox->front  = NULL;
	free(skybox->back);   skybox->back   = NULL;
	free(skybox->left);   skybox->left   = NULL;
	free(skybox->right);  skybox->right  = NULL;
	free(skybox->top);    skybox->top    = NULL;
	free(skybox->bottom); skybox->bottom = NULL;
}

static inline Color sampleFace(const uint32 *face, int w, int h, float u, float v) {
	if (!face) return 0xFF101010u;
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	if (x < 0) x = 0; else if (x >= w) x = w - 1;
	if (y < 0) y = 0; else if (y >= h) y = h - 1;
	return face[y * w + x];
}

Color SampleSkybox(const Skybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	int w = skybox->imageWidth;
	int h = skybox->imageHeight;

	float ax = dir.x < 0 ? -dir.x : dir.x;
	float ay = dir.y < 0 ? -dir.y : dir.y;
	float az = dir.z < 0 ? -dir.z : dir.z;

	float u, v;

	if (ax >= ay && ax >= az) {
		// ±X face
		if (dir.x > 0) {
			// right: u = -Z/X, v = -Y/X
			u = 0.5f + 0.5f * (-dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return sampleFace(skybox->right, w, h, u, v);
		} else {
			// left: u = +Z/X, v = -Y/X
			u = 0.5f + 0.5f * (dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return sampleFace(skybox->left, w, h, u, v);
		}
	} else if (ay >= ax && ay >= az) {
		// ±Y face
		if (dir.y > 0) {
			// top: u = +X/Y, v = +Z/Y
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (dir.z / ay);
			return sampleFace(skybox->top, w, h, u, v);
		} else {
			// bottom: u = +X/Y, v = -Z/Y
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (-dir.z / ay);
			return sampleFace(skybox->bottom, w, h, u, v);
		}
	} else {
		// ±Z face
		if (dir.z > 0) {
			// front: u = +X/Z, v = -Y/Z
			u = 0.5f + 0.5f * (dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return sampleFace(skybox->front, w, h, u, v);
		} else {
			// back: u = -X/Z, v = -Y/Z
			u = 0.5f + 0.5f * (-dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return sampleFace(skybox->back, w, h, u, v);
		}
	}
}
