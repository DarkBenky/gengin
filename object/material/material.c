#include "material.h"
#include <stdlib.h>
#include <stdio.h>

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
