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
	free(tex);
}

Textures *Textures_LoadFromFile(FILE *file) {
	Textures *tex = (Textures *)malloc(sizeof(Textures));
	if (!tex) {
		fprintf(stderr, "Error: Could not allocate Textures.\n");
		return NULL;
	}

	// ColorMap: RGBA uint8 packed as uint32, read directly.
	if (fread(tex->colorMap, sizeof(uint32), TEXTURE_SIZE * TEXTURE_SIZE, file) != (size_t)(TEXTURE_SIZE * TEXTURE_SIZE)) {
		fprintf(stderr, "Error: Failed to read colorMap.\n");
		free(tex);
		return NULL;
	}

	// NormalMap: stored as RGB (3 bytes/pixel), unpack to RGBA with full alpha.
	for (int i = 0; i < TEXTURE_SIZE * TEXTURE_SIZE; i++) {
		uint8 rgb[3];
		if (fread(rgb, 1, 3, file) != 3) {
			fprintf(stderr, "Error: Failed to read normalMap.\n");
			free(tex);
			return NULL;
		}
		((uint8 *)&tex->normalMap[i / TEXTURE_SIZE][i % TEXTURE_SIZE])[0] = rgb[0];
		((uint8 *)&tex->normalMap[i / TEXTURE_SIZE][i % TEXTURE_SIZE])[1] = rgb[1];
		((uint8 *)&tex->normalMap[i / TEXTURE_SIZE][i % TEXTURE_SIZE])[2] = rgb[2];
		((uint8 *)&tex->normalMap[i / TEXTURE_SIZE][i % TEXTURE_SIZE])[3] = 0xFF;
	}

	// MaterialMap: [roughness, metallic] as uint16, read directly.
	if (fread(tex->MaterialMap, sizeof(uint16), TEXTURE_SIZE * TEXTURE_SIZE, file) != (size_t)(TEXTURE_SIZE * TEXTURE_SIZE)) {
		fprintf(stderr, "Error: Failed to read MaterialMap.\n");
		free(tex);
		return NULL;
	}

	return tex;
}
