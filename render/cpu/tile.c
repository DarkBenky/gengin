#include "tile.h"

#include <string.h>

typedef struct {
	int dstX;
	int dstY;
	int srcX;
	int srcY;
	int width;
	int height;
} TileClip;

static int clipScaledTile(Tile tile, int dstWidth, int dstHeight, int x, int y, float scaleX, float scaleY, TileClip *clip) {
	int scaledW = (int)(tile.width * scaleX);
	int scaledH = (int)(tile.height * scaleY);
	int dstX = x;
	int dstY = y;
	int srcX = 0;
	int srcY = 0;

	// Clip left/top against dst
	if (dstX < 0) {
		srcX -= (int)(dstX / scaleX);
		scaledW += dstX;
		dstX = 0;
	}
	if (dstY < 0) {
		srcY -= (int)(dstY / scaleY);
		scaledH += dstY;
		dstY = 0;
	}

	// Clip right/bottom against dst
	if (dstX + scaledW > dstWidth) scaledW = dstWidth - dstX;
	if (dstY + scaledH > dstHeight) scaledH = dstHeight - dstY;

	clip->dstX = dstX;
	clip->dstY = dstY;
	clip->srcX = srcX;
	clip->srcY = srcY;
	clip->width = scaledW;
	clip->height = scaledH;
	return scaledW > 0 && scaledH > 0;
}

static int clipTile(Tile tile, int dstWidth, int dstHeight, int x, int y, TileClip *clip) {
	int srcX = 0;
	int srcY = 0;
	int width = tile.width;
	int height = tile.height;

	// Clip left/top against dst
	if (x < 0) {
		srcX -= x;
		width += x;
		x = 0;
	}
	if (y < 0) {
		srcY -= y;
		height += y;
		y = 0;
	}

	// Clip right/bottom against dst
	if (x + width > dstWidth) width = dstWidth - x;
	if (y + height > dstHeight) height = dstHeight - y;

	clip->dstX = x;
	clip->dstY = y;
	clip->srcX = srcX;
	clip->srcY = srcY;
	clip->width = width;
	clip->height = height;
	return width > 0 && height > 0;
}

void drawTile(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y) {
	TileClip clip;
	if (!clipTile(tile, dstWidth, dstHeight, x, y, &clip)) return;

	const size_t rowBytes = (size_t)clip.width * sizeof(uint32);
	for (int row = 0; row < clip.height; row++) {
		memcpy(
			dst + (clip.dstY + row) * dstWidth + clip.dstX,
			tile.pixels + (clip.srcY + row) * tile.width + clip.srcX,
			rowBytes);
	}
}

void drawTileScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY) {
	TileClip clip;
	if (!clipScaledTile(tile, dstWidth, dstHeight, x, y, scaleX, scaleY, &clip)) return;

	for (int row = 0; row < clip.height; row++) {
		for (int col = 0; col < clip.width; col++) {
			int srcCol = clip.srcX + (int)(col / scaleX);
			int srcRow = clip.srcY + (int)(row / scaleY);
			dst[(clip.dstY + row) * dstWidth + (clip.dstX + col)] = tile.pixels[srcRow * tile.width + srcCol];
		}
	}
}

void drawTileColor(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, uint32 color) {
	TileClip clip;
	if (!clipTile(tile, dstWidth, dstHeight, x, y, &clip)) return;

	for (int row = 0; row < clip.height; row++) {
		for (int col = 0; col < clip.width; col++) {
			uint32 src = tile.pixels[(clip.srcY + row) * tile.width + (clip.srcX + col)];
			if (src != 0x00000000)
				dst[(clip.dstY + row) * dstWidth + (clip.dstX + col)] = color;
		}
	}
}

void drawTileColorScaled(uint32 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY, uint32 color) {
	TileClip clip;
	if (!clipScaledTile(tile, dstWidth, dstHeight, x, y, scaleX, scaleY, &clip)) return;

	for (int row = 0; row < clip.height; row++) {
		for (int col = 0; col < clip.width; col++) {
			int srcCol = clip.srcX + (int)(col / scaleX);
			int srcRow = clip.srcY + (int)(row / scaleY);
			uint32 src = tile.pixels[srcRow * tile.width + srcCol];
			if (src != 0x00000000)
				dst[(clip.dstY + row) * dstWidth + (clip.dstX + col)] = color;
		}
	}
}

void drawTileColorScaledHdr(float3 *dst, int dstWidth, int dstHeight, Tile tile, int x, int y, float scaleX, float scaleY, float3 color) {
	TileClip clip;
	if (!clipScaledTile(tile, dstWidth, dstHeight, x, y, scaleX, scaleY, &clip)) return;

	for (int row = 0; row < clip.height; row++) {
		for (int col = 0; col < clip.width; col++) {
			int srcCol = clip.srcX + (int)(col / scaleX);
			int srcRow = clip.srcY + (int)(row / scaleY);
			uint32 src = tile.pixels[srcRow * tile.width + srcCol];
			if (src != 0x00000000)
				dst[(clip.dstY + row) * dstWidth + (clip.dstX + col)] = color;
		}
	}
}