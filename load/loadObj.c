#include "loadObj.h"
#include <stdio.h>
#include <stdlib.h>
#include "bbox.h"

void LoadObj(const char* filename, Object* obj) {
    if (filename == NULL || obj == NULL) {
        fprintf(stderr, "Error: Invalid filename or object pointer.\n");
        return;
    }
    FILE* file = fopen(filename, "r");
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

    obj->triangles = (Triangle*)malloc(triangleCount * sizeof(Triangle));
    if (obj->triangles == NULL) {
        fprintf(stderr, "Error: Could not allocate memory for triangles.\n");
        fclose(file);
        return;
    }
    obj->triangleCount = triangleCount;

    float3 BBmin = { FLT_MAX, FLT_MAX, FLT_MAX };
    float3 BBmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

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

        obj->triangles[i].v1 = v1;
        obj->triangles[i].v2 = v2;
        obj->triangles[i].v3 = v3;
        obj->triangles[i].normal = normal;
        obj->triangles[i].Roughness = Roughness;
        obj->triangles[i].Metallic = Metallic;
        obj->triangles[i].Emission = Emission;
        obj->triangles[i].color = color;
    }
    obj->BBmin = BBmin;
    obj->BBmax = BBmax;
    fclose(file);
}


