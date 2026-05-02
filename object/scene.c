#include "scene.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "../math/transform.h"

enum {
	kGroundIndex = 0,
	kRocketIndex = 1,
	kObjectCount = 2,
};

static const float kGroundY = -1.25f;

int DemoScene_ObjectCount(void) {
	return kObjectCount;
}

void DemoScene_Build(Object *objects, MaterialLib *lib) {
	if (!objects) return;

	CreateCube(&objects[kGroundIndex], (float3){0.0f, kGroundY, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){40.0f, 0.2f, 40.0f}, (float3){0.32f, 0.34f, 0.38f}, lib, 0.0f, 0.9f, 0.0f);
	Object_UpdateWorldBounds(&objects[kGroundIndex]);

	Object_Init(&objects[kRocketIndex], (float3){0.0f, 0.0f, 10.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, "assets/models/r27.bin", lib);
	Object_UpdateWorldBounds(&objects[kRocketIndex]);
}

void DemoScene_Update(Object *objects, int frame) {
	(void)objects;
	(void)frame;
}

void Scene_Destroy(Object *objects, int objectCount) {
	if (!objects) return;
	for (int i = 0; i < objectCount; i++) {
		Object_Destroy(&objects[i]);
	}
	free(objects);
}

int Scene_CountTriangles(const Object *objects, int objectCount) {
	if (!objects || objectCount <= 0) return 0;
	int total = 0;
	for (int i = 0; i < objectCount; i++) {
		total += objects[i].triangleCount;
	}
	return total;
}

void ObjectList_Init(ObjectList *list, int initialCapacity) {
	list->count = 0;
	list->capacity = initialCapacity > 0 ? initialCapacity : 8;
	list->objects = malloc(sizeof(Object) * list->capacity);
}

Object *ObjectList_Add(ObjectList *list) {
	if (list->count == list->capacity) {
		list->capacity *= 2;
		list->objects = realloc(list->objects, sizeof(Object) * list->capacity);
	}
	Object *obj = &list->objects[list->count++];
	*obj = (Object){0};
	return obj;
}

void ObjectList_Destroy(ObjectList *list) {
	Scene_Destroy(list->objects, list->count);
	list->objects = NULL;
	list->count = 0;
	list->capacity = 0;
}

int ObjectList_CountTriangles(const ObjectList *list) {
	return Scene_CountTriangles(list->objects, list->count);
}

void ObjectList_Remove(ObjectList *list, int index) {
	if (index < 0 || index >= list->count) return;
	Object_Destroy(&list->objects[index]);
	int last = list->count - 1;
	if (index != last)
		list->objects[index] = list->objects[last];
	list->count--;
}

void ObjectList_Merge(ObjectList *src, ObjectList *dst) {
	if (!src || src->count == 0) return;

	int totalTris = 0;
	for (int i = 0; i < src->count; i++)
		totalTris += src->objects[i].triangleCount;
	if (totalTris == 0) return;

	Object *out = ObjectList_Add(dst);
	out->position = (float3){0};
	out->rotation = (float3){0};
	out->scale = (float3){1.0f, 1.0f, 1.0f};

	out->v1 = malloc(totalTris * sizeof(float3));
	out->v2 = malloc(totalTris * sizeof(float3));
	out->v3 = malloc(totalTris * sizeof(float3));
	out->normals = malloc(totalTris * sizeof(float3));
	out->materialIds = malloc(totalTris * sizeof(int));
	out->triangleCount = totalTris;

	float3 bbMin = {FLT_MAX, FLT_MAX, FLT_MAX};
	float3 bbMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	int t = 0;
	for (int i = 0; i < src->count; i++) {
		Object *obj = &src->objects[i];
		for (int j = 0; j < obj->triangleCount; j++, t++) {
			// bake transform into vertices so the merged object sits at world origin
			float3 a = TransformPointTRS(obj->v1[j], obj->position, obj->rotation, obj->scale);
			float3 b = TransformPointTRS(obj->v2[j], obj->position, obj->rotation, obj->scale);
			float3 c = TransformPointTRS(obj->v3[j], obj->position, obj->rotation, obj->scale);
			out->v1[t] = a;
			out->v2[t] = b;
			out->v3[t] = c;
			out->normals[t] = RotateXYZ(obj->normals[j], obj->rotation);
			out->materialIds[t] = obj->materialIds ? obj->materialIds[j] : -1;

			// expand merged AABB
			float3 pts[3] = {a, b, c};
			for (int p = 0; p < 3; p++) {
				if (pts[p].x < bbMin.x) bbMin.x = pts[p].x;
				if (pts[p].y < bbMin.y) bbMin.y = pts[p].y;
				if (pts[p].z < bbMin.z) bbMin.z = pts[p].z;
				if (pts[p].x > bbMax.x) bbMax.x = pts[p].x;
				if (pts[p].y > bbMax.y) bbMax.y = pts[p].y;
				if (pts[p].z > bbMax.z) bbMax.z = pts[p].z;
			}
		}
	}

	out->BBmin = bbMin;
	out->BBmax = bbMax;
	CreateObjectBVH(out, &out->bvh);
	Object_UpdateWorldBounds(out);

	// destroy src objects and reset the list
	for (int i = 0; i < src->count; i++)
		Object_Destroy(&src->objects[i]);
	src->count = 0;
}
