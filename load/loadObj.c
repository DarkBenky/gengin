#include "loadObj.h"
#include "../object/object.h"
#include <stdio.h>
#include <stdlib.h>
#include "../util/bbox.h"

void LoadObj(const char *filename, Object *obj) {
	if (filename == NULL || obj == NULL) {
		fprintf(stderr, "Error: Invalid filename or object pointer.\n");
		return;
	}
	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		fprintf(stderr, "Error: Could not open file %s\n", filename);
		return;
	}

	// read first 32 bytes
	uint32 fileSize, triangleStructSize;
	fread(&fileSize, sizeof(uint32), 1, file);
	fread(&triangleStructSize, sizeof(uint32), 1, file);

	uint32 triangleCount = (fileSize - 8) / triangleStructSize;

	printf("File size: %u bytes\n", fileSize);
	printf("Triangle struct size: %u bytes\n", triangleStructSize);
	printf("Triangle count: %u\n", triangleCount);

	obj->v1 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->v2 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->v3 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->normals = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->colors = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->roughness = (float *)malloc(triangleCount * sizeof(float));
	obj->metallic = (float *)malloc(triangleCount * sizeof(float));
	obj->emission = (float *)malloc(triangleCount * sizeof(float));

	if (!obj->v1 || !obj->v2 || !obj->v3 || !obj->normals || !obj->colors ||
		!obj->roughness || !obj->metallic || !obj->emission) {
		fprintf(stderr, "Error: Could not allocate memory for triangles.\n");
		Object_Destroy(obj);
		fclose(file);
		return;
	}
	obj->triangleCount = triangleCount;

	float3 BBmin = {FLT_MAX, FLT_MAX, FLT_MAX};
	float3 BBmax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	for (uint32 i = 0; i < triangleCount; i++) {
		float3 v1, v2, v3;
		float3 normal, color;
		float Roughness, Metallic, Emission;

		fread(&v1, sizeof(float3), 1, file);
		fread(&v2, sizeof(float3), 1, file);
		fread(&v3, sizeof(float3), 1, file);

		UpdateBoundingBox(&BBmin, &BBmax, v1);
		UpdateBoundingBox(&BBmin, &BBmax, v2);
		UpdateBoundingBox(&BBmin, &BBmax, v3);

		fread(&normal, sizeof(float3), 1, file);
		fread(&Roughness, sizeof(float), 1, file);
		fread(&Metallic, sizeof(float), 1, file);
		fread(&Emission, sizeof(float), 1, file);
		fread(&color, sizeof(float3), 1, file);

		obj->v1[i] = v1;
		obj->v2[i] = v2;
		obj->v3[i] = v3;
		obj->normals[i] = normal;
		obj->roughness[i] = Roughness;
		obj->metallic[i] = Metallic;
		obj->emission[i] = Emission;
		obj->colors[i] = color;
	}
	obj->BBmin = BBmin;
	obj->BBmax = BBmax;
	fclose(file);
}
