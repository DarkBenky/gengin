#include "object.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Object_Init(Object *obj, float3 position, float3 rotation, float3 scale, const char *filename) {
	LoadObj(filename, obj);
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;
}

void CreateCube(Object *obj, float3 position, float3 rotation, float3 scale, float3 color) {
	obj->position = position;
	obj->rotation = rotation;
	obj->scale = scale;

	// Define the 8 vertices of the cube
	float3 vertices[8] = {
		{-0.5f, -0.5f, -0.5f},
		{0.5f, -0.5f, -0.5f},
		{0.5f, 0.5f, -0.5f},
		{-0.5f, 0.5f, -0.5f},
		{-0.5f, -0.5f, 0.5f},
		{0.5f, -0.5f, 0.5f},
		{0.5f, 0.5f, 0.5f},
		{-0.5f, 0.5f, 0.5f}};

	// Define the triangles for each face of the cube
	Triangle triangles[12] = {
		// Front face
		{vertices[4], vertices[5], vertices[6], {0, 0, 1}, color, 0.5f, 0.0f, 0.0f},
		{vertices[4], vertices[6], vertices[7], {0, 0, 1}, color, 0.5f, 0.0f, 0.0f},
		// Back face
		{vertices[1], vertices[0], vertices[3], {0, 0, -1}, color, 0.5f, 0.0f, 0.0f},
		{vertices[1], vertices[3], vertices[2], {0, 0, -1}, color, 0.5f, 0.0f, 0.0f},
		// Left face
		{vertices[0], vertices[4], vertices[7], {-1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[0], vertices[7], vertices[3], {-1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		// Right face
		{vertices[5], vertices[1], vertices[2], {1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[5], vertices[2], vertices[6], {1, 0, 0}, color, 0.5f, 0.0f, 0.0f},
		// Top face
		{vertices[3], vertices[7], vertices[6], {0, 1, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[3], vertices[6], vertices[2], {0, 1, 0}, color, 0.5f, 0.0f, 0.0f},
		// Bottom face
		{vertices[0], vertices[1], vertices[5], {0, -1, 0}, color, 0.5f, 0.0f, 0.0f},
		{vertices[0], vertices[5], vertices[4], {0, -1, 0}, color, 0.5f, 0.0f, 0.0f}};

	obj->triangles = (Triangle *)malloc(12 * sizeof(Triangle));
	if (obj->triangles == NULL) {
		fprintf(stderr, "Error: Could not allocate memory for cube triangles.\n");
		return;
	}
	memcpy(obj->triangles, triangles, 12 * sizeof(Triangle));
	obj->triangleCount = 12;
}

void Object_Destroy(Object *obj) {
	if (obj && obj->triangles) {
		free(obj->triangles);
		obj->triangles = NULL;
		obj->triangleCount = 0;
	}
}
