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
	free(tex->colorMap);
	free(tex->normalMap);
	free(tex->MaterialMap);
	free(tex);
}

Textures *Textures_LoadFromFile(FILE *file, uint32 textureSize) {
	size_t texelCount = (size_t)textureSize * textureSize;
	Textures *tex = (Textures *)malloc(sizeof(Textures));
	if (!tex) {
		fprintf(stderr, "Error: Could not allocate Textures.\n");
		return NULL;
	}
	tex->size = textureSize;
	tex->colorMap = (Color *)malloc(texelCount * sizeof(Color));
	tex->normalMap = (Color *)malloc(texelCount * sizeof(Color));
	tex->MaterialMap = (uint16 *)malloc(texelCount * sizeof(uint16));
	if (!tex->colorMap || !tex->normalMap || !tex->MaterialMap) {
		fprintf(stderr, "Error: Could not allocate textures.\n");
		Textures_Destroy(tex);
		return NULL;
	}

	// ColorMap: RGBA uint8 packed as uint32, read directly.
	if (fread(tex->colorMap, sizeof(Color), texelCount, file) != texelCount) {
		fprintf(stderr, "Error: Failed to read colorMap.\n");
		Textures_Destroy(tex);
		return NULL;
	}

	// NormalMap: stored as RGB (3 bytes/pixel), unpack to RGBA with full alpha.
	uint8 *normalPlane = (uint8 *)malloc(texelCount * 3);
	if (!normalPlane || fread(normalPlane, 1, texelCount * 3, file) != texelCount * 3) {
		fprintf(stderr, "Error: Failed to read normalMap.\n");
		free(normalPlane);
		Textures_Destroy(tex);
		return NULL;
	}
	for (size_t i = 0; i < texelCount; i++) {
		uint8 *px = (uint8 *)&tex->normalMap[i];
		px[0] = normalPlane[i * 3];
		px[1] = normalPlane[i * 3 + 1];
		px[2] = normalPlane[i * 3 + 2];
		px[3] = 0xFF;
	}
	free(normalPlane);

	// MaterialMap: [roughness, metallic] as uint16, read directly.
	if (fread(tex->MaterialMap, sizeof(uint16), texelCount, file) != texelCount) {
		fprintf(stderr, "Error: Failed to read MaterialMap.\n");
		Textures_Destroy(tex);
		return NULL;
	}

	return tex;
}
