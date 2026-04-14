#ifndef MATERIAL_H
#define MATERIAL_H

#include "../format.h"

typedef struct Material {
	float3 color;
	float roughness;
	float metallic;
	float emission;
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

static inline Material Material_Make(float3 color, float roughness, float metallic, float emission) {
	return (Material){color, roughness, metallic, emission};
}

#endif // MATERIAL_H
