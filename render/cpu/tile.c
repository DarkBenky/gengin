#include "tile.h"

#include <string.h>

void drawTile(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y) {
	int srcX = 0, srcY = 0;
	int copyW = tile.width;
	int copyH = tile.height;

	// Clip left/top against dst
	if (x < 0) {
		srcX -= x;
		copyW += x;
		x = 0;
	}
	if (y < 0) {
		srcY -= y;
		copyH += y;
		y = 0;
	}

	// Clip right/bottom against dst
	if (x + copyW > dstWidth) copyW = dstWidth - x;
	if (y + copyH > dstHeight) copyH = dstHeight - y;

	if (copyW <= 0 || copyH <= 0) return;

	const size_t rowBytes = (size_t)copyW * sizeof(uint32);
	for (int row = 0; row < copyH; row++) {
		memcpy(
			dst + (y + row) * dstWidth + x,
			tile.pixels + (srcY + row) * tile.width + srcX,
			rowBytes);
	}
}

void drawTileScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY) {
	int scaledW = (int)(tile.width * scaleX);
	int scaledH = (int)(tile.height * scaleY);

	// Clip left/top against dst
	int dstX = x;
	int dstY = y;
	int srcX = 0;
	int srcY = 0;
	if (dstX < 0) {
		srcX -= (int)(dstX / scaleX);
		dstX = 0;
	}
	if (dstY < 0) {
		srcY -= (int)(dstY / scaleY);
		dstY = 0;
	}

	// Clip right/bottom against dst
	if (dstX + scaledW > dstWidth) scaledW = dstWidth - dstX;
	if (dstY + scaledH > dstHeight) scaledH = dstHeight - dstY;

	if (scaledW <= 0 || scaledH <= 0) return;

	for (int row = 0; row < scaledH; row++) {
		for (int col = 0; col < scaledW; col++) {
			int srcCol = srcX + (int)(col / scaleX);
			int srcRow = srcY + (int)(row / scaleY);
			dst[(dstY + row) * dstWidth + (dstX + col)] = tile.pixels[srcRow * tile.width + srcCol];
		}
	}
}
