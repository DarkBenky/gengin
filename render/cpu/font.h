#ifndef FONT_H
#define FONT_H
#include "tile.h"

struct letter {
    char character;
    Tile tile;
};

struct Alphabet {
    struct letter letters[256];
};

void LoadAlphabet(struct Alphabet *alphabet, const char *dirname); // index directory load binary data letter is set by filename, tile is set by binary data
void RenderText(uint32 *dst, int dstWidth, int dstHeight, struct Alphabet *alphabet, const char *text, int x, int y, float scale, uint32 color);

#endif // FONT_H