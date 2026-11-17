#include "saveImage.h"

void SaveImage(const char *filename, const Camera *camera) {
	if (!filename || !camera || !camera->framebufferLdr) return;

	FILE *f = fopen(filename, "wb");
	if (!f) {
		fprintf(stderr, "Error: Could not open %s for writing.\n", filename);
		return;
	}

	int width = camera->screenWidth;
	int height = camera->screenHeight;
	int rowSize = (width * 3 + 3) & ~3;
	int dataSize = rowSize * height;
	int fileSize = 14 + 40 + dataSize;

	// BMP file header (14 bytes)
	uint8 fileHeader[14] = {
		'B',
		'M',
		(uint8)(fileSize),
		(uint8)(fileSize >> 8),
		(uint8)(fileSize >> 16),
		(uint8)(fileSize >> 24),
		0,
		0,
		0,
		0, // reserved
		54,
		0,
		0,
		0, // pixel data offset
	};

	// DIB header — BITMAPINFOHEADER (40 bytes)
	uint8 dibHeader[40] = {
		40,
		0,
		0,
		0, // header size
		(uint8)(width),
		(uint8)(width >> 8),
		(uint8)(width >> 16),
		(uint8)(width >> 24),
		(uint8)(height),
		(uint8)(height >> 8),
		(uint8)(height >> 16),
		(uint8)(height >> 24),
		1,
		0, // color planes
		24,
		0, // bits per pixel
		0,
		0,
		0,
		0, // no compression
		(uint8)(dataSize),
		(uint8)(dataSize >> 8),
		(uint8)(dataSize >> 16),
		(uint8)(dataSize >> 24),
		0x13,
		0x0B,
		0,
		0, // ~72 dpi horizontal
		0x13,
		0x0B,
		0,
		0, // ~72 dpi vertical
		0,
		0,
		0,
		0, // colors in table
		0,
		0,
		0,
		0, // important colors
	};

	fwrite(fileHeader, 1, sizeof(fileHeader), f);
	fwrite(dibHeader, 1, sizeof(dibHeader), f);

	// BMP rows are bottom-up, pixels are BGR
	uint8 row[rowSize];
	for (int y = height - 1; y >= 0; y--) {
		for (int x = 0; x < width; x++) {
			uint32 c = camera->framebufferLdr[y * width + x];
			row[x * 3 + 0] = (c) & 0xFF;	   // B
			row[x * 3 + 1] = (c >> 8) & 0xFF;  // G
			row[x * 3 + 2] = (c >> 16) & 0xFF; // R
		}
		// zero-pad to rowSize
		for (int x = width * 3; x < rowSize; x++)
			row[x] = 0;
		fwrite(row, 1, rowSize, f);
	}

	if (fclose(f) != 0)
		fprintf(stderr, "Error: Could not write %s.\n", filename);
}
