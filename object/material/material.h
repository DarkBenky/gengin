#ifndef MATERIAL_H
#define MATERIAL_H

#include "../format.h"
#include <stdio.h>

#define TEXTURE_SIZE 4096
typedef struct Textures {
	Color colorMap[TEXTURE_SIZE][TEXTURE_SIZE];     // RGBA8
	Color normalMap[TEXTURE_SIZE][TEXTURE_SIZE];    // RGBA8
	uint16 MaterialMap[TEXTURE_SIZE][TEXTURE_SIZE]; // [roughness (uint8), metallic (uint8)]
} Textures;

typedef struct Material {
	float3 color;
	float roughness;
	float metallic;
	float emission;
	Textures *textures;
} Material;

// One shared library - all objects reference entries by index.
typedef struct MaterialLib {
	Material *entries;
	int count;
	int capacity;
} MaterialLib;

void MaterialLib_Init(MaterialLib *lib, int initialCapacity);
void MaterialLib_Destroy(MaterialLib *lib);

// Add a material; returns its index. Returns -1 on allocation failure.
int MaterialLib_Add(MaterialLib *lib, Material mat);

// Returns the index of a matching material if one already exists, otherwise adds it.
int MaterialLib_FindOrAdd(MaterialLib *lib, Material mat);

// Remaps materialIds[0..count) so that equal materials share the lowest lib index.
// Use after loading with a non-deduplicating path.
void packMaterials(int *materialIds, int count, MaterialLib *lib);

// Allocates a Textures block and reads ColorMap/NormalMap/MaterialMap from the
// current file position. NormalMap is stored as RGB (3 bytes/pixel) in the binary
// and unpacked to RGBA here. Returns NULL on allocation or read failure.
Textures *Textures_LoadFromFile(FILE *file);

void Textures_Destroy(Textures *tex);

static inline Material Material_Make(float3 color, float roughness, float metallic, float emission, Textures *textures) {
	return (Material){color, roughness, metallic, emission, textures};
}

#endif // MATERIAL_H
