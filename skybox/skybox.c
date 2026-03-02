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

void LoadSkybox(Skybox *skybox, const char *directory) {
	if (!skybox || !directory) return;
	memset(skybox, 0, sizeof(*skybox));

	int w, h;
	skybox->front  = loadFace(directory, "front",  &w, &h); skybox->imageWidth = w; skybox->imageHeight = h;
	skybox->back   = loadFace(directory, "back",   &w, &h);
	skybox->left   = loadFace(directory, "left",   &w, &h);
	skybox->right  = loadFace(directory, "right",  &w, &h);
	skybox->top    = loadFace(directory, "top",    &w, &h);
	skybox->bottom = loadFace(directory, "bottom", &w, &h);
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
