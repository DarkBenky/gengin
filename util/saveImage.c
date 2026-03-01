#include "saveImage.h"

void SaveImage(const char *filename, const Camera *camera) {
	if (!filename || !camera || !camera->framebuffer) return;

	FILE *f = fopen(filename, "wb");
	if (!f) return;

	int width = camera->screenWidth;
	int height = camera->screenHeight;
	int rowSize = (width * 3 + 3) & ~3;
	int dataSize = rowSize * height;
	int fileSize = 14 + 40 + dataSize;

	// BMP file header (14 bytes)
	unsigned char fileHeader[14] = {
		'B', 'M',
		(unsigned char)(fileSize),
		(unsigned char)(fileSize >> 8),
		(unsigned char)(fileSize >> 16),
		(unsigned char)(fileSize >> 24),
		0, 0, 0, 0,	 // reserved
		54, 0, 0, 0, // pixel data offset
	};

	// DIB header — BITMAPINFOHEADER (40 bytes)
	unsigned char dibHeader[40] = {
		40, 0, 0, 0, // header size
		(unsigned char)(width), (unsigned char)(width >> 8),
		(unsigned char)(width >> 16), (unsigned char)(width >> 24),
		(unsigned char)(height), (unsigned char)(height >> 8),
		(unsigned char)(height >> 16), (unsigned char)(height >> 24),
		1, 0,		// color planes
		24, 0,		// bits per pixel
		0, 0, 0, 0, // no compression
		(unsigned char)(dataSize), (unsigned char)(dataSize >> 8),
		(unsigned char)(dataSize >> 16), (unsigned char)(dataSize >> 24),
		0x13, 0x0B, 0, 0, // ~72 dpi horizontal
		0x13, 0x0B, 0, 0, // ~72 dpi vertical
		0, 0, 0, 0,		  // colors in table
		0, 0, 0, 0,		  // important colors
	};

	fwrite(fileHeader, 1, sizeof(fileHeader), f);
	fwrite(dibHeader, 1, sizeof(dibHeader), f);

	// BMP rows are bottom-up, pixels are BGR
	unsigned char row[rowSize];
	for (int y = height - 1; y >= 0; y--) {
		for (int x = 0; x < width; x++) {
			unsigned int c = camera->framebuffer[y * width + x];
			row[x * 3 + 0] = (c) & 0xFF;	   // B
			row[x * 3 + 1] = (c >> 8) & 0xFF;  // G
			row[x * 3 + 2] = (c >> 16) & 0xFF; // R
		}
		// zero-pad to rowSize
		for (int x = width * 3; x < rowSize; x++)
			row[x] = 0;
		fwrite(row, 1, rowSize, f);
	}

	fclose(f);
}
