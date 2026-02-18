#ifndef TILE_H
#define TILE_H

#include "../../object/format.h"

typedef struct {
	const uint32 *pixels;
	int width;
	int height;
} GlyphBuf;

void BlitGlyph(uint32 *dst, int dstWidth, int dstHeight, GlyphBuf glyph, int x, int y);

#endif // TILE_H
