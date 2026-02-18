#include "tile.h"

#include <string.h>

void BlitGlyph(uint32 *dst, int dstWidth, int dstHeight, GlyphBuf glyph, int x, int y) {
	int srcX = 0, srcY = 0;
	int copyW = glyph.width;
	int copyH = glyph.height;

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
			glyph.pixels + (srcY + row) * glyph.width + srcX,
			rowBytes);
	}
}
