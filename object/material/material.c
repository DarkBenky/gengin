#include "material.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void MaterialLib_Init(MaterialLib *lib, int initialCapacity) {
	if (initialCapacity <= 0) initialCapacity = 64;
	lib->entries = (Material *)malloc((size_t)initialCapacity * sizeof(Material));
	if (!lib->entries) {
		fprintf(stderr, "Error: Could not allocate MaterialLib.\n");
		lib->count = lib->capacity = 0;
		return;
	}
	lib->count = 0;
	lib->capacity = initialCapacity;
}

void MaterialLib_Destroy(MaterialLib *lib) {
	if (!lib) return;
	// Free each unique Textures pointer once.
	for (int i = 0; i < lib->count; i++) {
		if (!lib->entries[i].textures) continue;
		int duplicate = 0;
		for (int j = 0; j < i; j++) {
			if (lib->entries[j].textures == lib->entries[i].textures) {
				duplicate = 1;
				break;
			}
		}
		if (!duplicate) Textures_Destroy(lib->entries[i].textures);
	}
	free(lib->entries);
	lib->entries = NULL;
	lib->count = lib->capacity = 0;
}

int MaterialLib_Add(MaterialLib *lib, Material mat) {
	if (!lib) return -1;
	if (lib->count >= lib->capacity) {
		int newCap = lib->capacity * 2;
		Material *resized = (Material *)realloc(lib->entries, (size_t)newCap * sizeof(Material));
		if (!resized) {
			fprintf(stderr, "Error: Could not grow MaterialLib.\n");
			return -1;
		}
		lib->entries = resized;
		lib->capacity = newCap;
	}
	lib->entries[lib->count] = mat;
	return lib->count++;
}

int MaterialLib_FindOrAdd(MaterialLib *lib, Material mat) {
	for (int i = 0; i < lib->count; i++) {
		if (memcmp(&lib->entries[i], &mat, sizeof(Material)) == 0)
			return i;
	}
	return MaterialLib_Add(lib, mat);
}

void packMaterials(int *materialIds, int count, MaterialLib *lib) {
	for (int i = 0; i < count; i++) {
		int id = materialIds[i];
		if (id <= 0 || id >= lib->count) continue;
		for (int j = 0; j < id; j++) {
			if (memcmp(&lib->entries[j], &lib->entries[id], sizeof(Material)) == 0) {
				materialIds[i] = j;
				break;
			}
		}
	}
}

void Textures_Destroy(Textures *tex) {
	if (!tex) return;
	free(tex->texels);
	free(tex);
}

// Staged a block at a time so the load peak stays one Textures.
#define TEXTURE_STAGE_TEXELS 4096

static void texturesLoadFail(Textures *tex, Color *stageColor, uint8 *stageNormal, uint16 *stageMaterial) {
	free(stageColor);
	free(stageNormal);
	free(stageMaterial);
	Textures_Destroy(tex);
}

Textures *Textures_LoadFromFile(FILE *file, uint32 textureSize) {
	size_t texelCount = (size_t)textureSize * textureSize;
	Textures *tex = (Textures *)malloc(sizeof(Textures));
	if (!tex) {
		fprintf(stderr, "Error: Could not allocate Textures.\n");
		return NULL;
	}
	tex->size = textureSize;
	tex->texels = (Texel *)malloc(texelCount * sizeof(Texel));
	if (!tex->texels) {
		fprintf(stderr, "Error: Could not allocate textures.\n");
		free(tex);
		return NULL;
	}

	const size_t stageCount = TEXTURE_STAGE_TEXELS;
	Color *stageColor = (Color *)malloc(stageCount * sizeof(Color));
	uint8 *stageNormal = (uint8 *)malloc(stageCount * 3);
	uint16 *stageMaterial = (uint16 *)malloc(stageCount * sizeof(uint16));
	if (!stageColor || !stageNormal || !stageMaterial) {
		fprintf(stderr, "Error: Could not allocate texture staging.\n");
		texturesLoadFail(tex, stageColor, stageNormal, stageMaterial);
		return NULL;
	}

	for (size_t base = 0; base < texelCount; base += stageCount) {
		size_t n = texelCount - base < stageCount ? texelCount - base : stageCount;

		// ColorMap: RGBA uint8 packed as uint32, read directly.
		if (fread(stageColor, sizeof(Color), n, file) != n) {
			fprintf(stderr, "Error: Failed to read colorMap.\n");
			texturesLoadFail(tex, stageColor, stageNormal, stageMaterial);
			return NULL;
		}
		for (size_t i = 0; i < n; i++)
			tex->texels[base + i].color = stageColor[i];
	}

	// NormalMap: stored as RGB (3 bytes/pixel), unpack to RGBA with full alpha.
	for (size_t base = 0; base < texelCount; base += stageCount) {
		size_t n = texelCount - base < stageCount ? texelCount - base : stageCount;

		if (fread(stageNormal, 1, n * 3, file) != n * 3) {
			fprintf(stderr, "Error: Failed to read normalMap.\n");
			texturesLoadFail(tex, stageColor, stageNormal, stageMaterial);
			return NULL;
		}
		for (size_t i = 0; i < n; i++) {
			uint8 *px = (uint8 *)&tex->texels[base + i].normal;
			px[0] = stageNormal[i * 3];
			px[1] = stageNormal[i * 3 + 1];
			px[2] = stageNormal[i * 3 + 2];
			px[3] = 0xFF;
		}
	}

	// MaterialMap: [roughness, metallic] as uint16, read directly.
	for (size_t base = 0; base < texelCount; base += stageCount) {
		size_t n = texelCount - base < stageCount ? texelCount - base : stageCount;

		if (fread(stageMaterial, sizeof(uint16), n, file) != n) {
			fprintf(stderr, "Error: Failed to read MaterialMap.\n");
			texturesLoadFail(tex, stageColor, stageNormal, stageMaterial);
			return NULL;
		}
		for (size_t i = 0; i < n; i++)
			tex->texels[base + i].mat = stageMaterial[i];
	}

	free(stageColor);
	free(stageNormal);
	free(stageMaterial);
	return tex;
}
