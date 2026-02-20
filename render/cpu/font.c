#include "font.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parseLetterIndex(const char *filename) {
	if (!filename || !filename[0]) return -1;

	char stem[32];
	const char *dot = strrchr(filename, '.');
	size_t stemLen = dot ? (size_t)(dot - filename) : strlen(filename);
	if (stemLen == 0 || stemLen >= sizeof(stem)) return -1;

	memcpy(stem, filename, stemLen);
	stem[stemLen] = '\0';

	int allDigits = 1;
	for (size_t i = 0; i < stemLen; i++) {
		if (!isdigit((unsigned char)stem[i])) {
			allDigits = 0;
			break;
		}
	}

	if (allDigits) {
		char *endPtr = NULL;
		long value = strtol(stem, &endPtr, 10);
		if (endPtr && *endPtr == '\0' && value >= 0 && value <= 255) {
			return (int)value;
		}
		return -1;
	}

	if (stemLen == 1) return (unsigned char)stem[0];
	return -1;
}

void LoadAlphabet(struct Alphabet *alphabet, const char *dirname) {
	if (!alphabet || !dirname) return;

	for (int i = 0; i < 256; i++) {
		alphabet->letters[i].character = (char)i;
		alphabet->letters[i].tile.width = 0;
		alphabet->letters[i].tile.height = 0;
		alphabet->letters[i].tile.pixels = NULL;
	}

	DIR *dir = opendir(dirname);
	if (!dir) {
		fprintf(stderr, "LoadAlphabet: could not open directory %s\n", dirname);
		return;
	}

	struct dirent *entry = NULL;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;

		int letterIndex = parseLetterIndex(entry->d_name);
		if (letterIndex < 0 || letterIndex > 255) continue;

		char filePath[1024];
		int pathLen = snprintf(filePath, sizeof(filePath), "%s/%s", dirname, entry->d_name);
		if (pathLen < 0 || (size_t)pathLen >= sizeof(filePath)) continue;

		FILE *file = fopen(filePath, "rb");
		if (!file) continue;

		uint32 width = 0;
		uint32 height = 0;
		if (fread(&width, sizeof(uint32), 1, file) != 1 || fread(&height, sizeof(uint32), 1, file) != 1) {
			fclose(file);
			continue;
		}

		if (width == 0 || height == 0 || width > 1024 || height > 1024) {
			fclose(file);
			continue;
		}

		size_t pixelCount = (size_t)width * (size_t)height;
		if (pixelCount > (size_t)1 << 24) {
			fclose(file);
			continue;
		}

		uint32 *pixels = (uint32 *)malloc(pixelCount * sizeof(uint32));
		if (!pixels) {
			fclose(file);
			continue;
		}

		size_t packedBytes = ((size_t)width + 7u) / 8u;
		unsigned char *rowData = (unsigned char *)malloc(packedBytes);
		if (!rowData) {
			free(pixels);
			fclose(file);
			continue;
		}

		int valid = 1;
		for (uint32 y = 0; y < height; y++) {
			if (fread(rowData, 1, packedBytes, file) != packedBytes) {
				valid = 0;
				break;
			}
			for (uint32 x = 0; x < width; x++) {
				size_t byteIndex = x / 8u;
				int bit = 7 - (int)(x % 8u);
				int on = (rowData[byteIndex] >> bit) & 1;
				pixels[(size_t)y * width + x] = on ? 0xFFFFFFFFu : 0x00000000u;
			}
		}

		free(rowData);
		fclose(file);

		if (!valid) {
			free(pixels);
			continue;
		}

		alphabet->letters[letterIndex].character = (char)letterIndex;
		alphabet->letters[letterIndex].tile.width = width;
		alphabet->letters[letterIndex].tile.height = height;
		alphabet->letters[letterIndex].tile.pixels = pixels;
	}

	int legacyLowCount = 0;
	int legacyHighCount = 0;
	for (int i = 0; i < 32; i++) {
		if (alphabet->letters[i].tile.pixels) legacyLowCount++;
	}
	for (int i = 224; i < 256; i++) {
		if (alphabet->letters[i].tile.pixels) legacyHighCount++;
	}

	if (legacyLowCount > 0 && legacyHighCount == 0) {
		for (int i = 255; i >= 32; i--) {
			alphabet->letters[i].tile = alphabet->letters[i - 32].tile;
		}
		for (int i = 0; i < 32; i++) {
			alphabet->letters[i].tile.width = 0;
			alphabet->letters[i].tile.height = 0;
			alphabet->letters[i].tile.pixels = NULL;
		}
	}

	closedir(dir);
}

void RenderText(uint32 *dst, int dstWidth, int dstHeight, struct Alphabet *alphabet, const char *text, int x, int y, float scale, uint32 color) {
	if (!dst || !alphabet || !text) return;

	if (scale <= 0.0f) scale = 1.0f;

	int cursorX = x;
	for (size_t i = 0; text[i] != '\0'; i++) {
		unsigned char code = (unsigned char)text[i];
		Tile tile = alphabet->letters[code].tile;

		if (tile.pixels && tile.width > 0 && tile.height > 0) {
			drawTileColorScaled(dst, dstWidth, dstHeight, tile, cursorX, y, scale, scale, color);
			cursorX += (int)((float)tile.width * scale);
		} else {
			cursorX += (int)(8.0f * scale);
		}
	}
}
