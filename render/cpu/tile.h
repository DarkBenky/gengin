#ifndef TILE_H
#define TILE_H

#include "../../object/format.h"

typedef struct {
	const uint32 *pixels;
	int width;
	int height;
} Tile;

void drawTile(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y);
void drawTileScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY);

#endif // TILE_H
