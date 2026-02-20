#ifndef TILE_H
#define TILE_H

#include "../../object/format.h"

typedef struct {
	uint32 width;
	uint32 height;
	const uint32 *pixels;
} Tile;

void drawTile(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y);
void drawTileScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY);
void drawTileColor(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, uint32 color);
void drawTileColorScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY, uint32 color);

#endif // TILE_H
