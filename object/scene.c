#include "scene.h"
#include "object.h"
#include "loadObjects.h"
#include "material/material.h"

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

#define GRID_COLS 32
#define GRID_ROWS 32

static void AddTileGrid(ObjectList *list, MaterialLib *lib) {
	static const struct {
		float3 color;
		float roughness;
		float metallic;
	} palette[] = {
		{{0.90f, 0.78f, 0.08f}, 0.85f, 0.00f}, // yellow  - rough matte
		{{0.55f, 0.08f, 0.85f}, 0.10f, 0.05f}, // purple  - smooth
		{{0.85f, 0.60f, 0.05f}, 0.20f, 0.90f}, // gold    - metallic
		{{0.80f, 0.12f, 0.12f}, 0.75f, 0.05f}, // red     - rough
		{{0.08f, 0.75f, 0.85f}, 0.15f, 0.00f}, // cyan    - smooth
		{{0.88f, 0.88f, 0.88f}, 0.05f, 0.90f}, // silver  - mirror
		{{0.10f, 0.50f, 0.12f}, 0.90f, 0.00f}, // green   - rough matte
		{{0.90f, 0.38f, 0.05f}, 0.50f, 0.20f}, // orange  - semi-rough
	};

	ObjectList tiles;
	ObjectList_Init(&tiles, GRID_COLS * GRID_ROWS);
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			Object *obj = ObjectList_Add(&tiles);
			int pIdx = ((col * 7) ^ (row * 3) ^ (col + row * 5)) % 8;
			float emission = (row == GRID_ROWS - 1) ? 0.5f : 0.0f;
			CreateCube(obj, (float3){(col - GRID_COLS / 2) * 7.0f, -0.09f, 5.0f + row * 7.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){7.0f, 0.1f, 7.0f}, palette[pIdx].color, lib, emission, palette[pIdx].roughness, palette[pIdx].metallic);
			Object_UpdateWorldBounds(obj);
		}
	}
	ObjectList_Merge(&tiles, list);
	ObjectList_Destroy(&tiles);
}

static void AddEmissiveCubes(ObjectList *list, MaterialLib *lib) {
	Object *cube = ObjectList_Add(list);
	CreateCube(cube, (float3){-3.5f, 0.5f, 5.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){0.5f, 0.5f, 0.5f}, (float3){0.7f, 0.4f, 0.0f}, lib, 8.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube);

	Object *cube2 = ObjectList_Add(list);
	CreateCube(cube2, (float3){3.5f, 0.5f, 5.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){1.5f, 1.5f, 1.5f}, (float3){0.0f, 0.6f, 0.3f}, lib, 8.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube2);

	Object *cube3 = ObjectList_Add(list);
	CreateCube(cube3, (float3){10.5f, 0.5f, 5.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){1.5f, 1.5f, 1.5f}, (float3){0.4f, 0.0f, 0.5f}, lib, 8.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube3);
}

static void AddMaterialGrid(ObjectList *list, MaterialLib *lib) {
	static const float3 gridColors[6] = {
		{0.90f, 0.10f, 0.10f},
		{0.90f, 0.45f, 0.05f},
		{0.90f, 0.80f, 0.10f},
		{0.10f, 0.75f, 0.15f},
		{0.10f, 0.30f, 0.90f},
		{0.55f, 0.10f, 0.85f},
	};
	static const struct {
		float roughness;
		float metallic;
	} gridMats[4] = {
		{0.95f, 0.00f},
		{0.05f, 0.00f},
		{0.15f, 0.95f},
		{0.50f, 0.50f},
	};

	ObjectList grid;
	ObjectList_Init(&grid, 64);
	// each (ix, iz) column is one primitive from shapeTable; each iy level one material
	for (int ix = 0; ix < 4; ix++) {
		for (int iz = 0; iz < 4; iz++) {
			ShapeKind shape = (ShapeKind)(ix * 4 + iz);
			for (int iy = 0; iy < 4; iy++) {
				ObjectRecord rec = {0};
				rec.type = OBJ_SHAPE;
				rec.shape = shape;
				rec.position = (float3){22.0f + ix * 3.0f, 0.8f + iy * 3.0f, 11.0f + iz * 3.0f};
				rec.scale = (float3){2.0f, 2.0f, 2.0f};
				rec.material = (ObjectMaterial){gridColors[shape % 6], 0.0f, gridMats[iy].roughness, gridMats[iy].metallic};
				spawnShape(ObjectList_Add(&grid), &rec, lib);
			}
		}
	}
	ObjectList_Merge(&grid, list);
	ObjectList_Destroy(&grid);
}

static void AddReflectiveSpheres(ObjectList *list, MaterialLib *lib) {
	Object *sphere = ObjectList_Add(list);
	CreateSphereHighResolution(sphere, (float3){-7.0f, 1.5f, 9.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f}, (float3){0.85f, 0.65f, 0.15f}, lib, 0.0f, 0.25f, 0.8f);
	Object_UpdateWorldBounds(sphere);

	Object *sphere2 = ObjectList_Add(list);
	CreateSphereHighResolution(sphere2, (float3){7.0f, 1.5f, 9.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f}, (float3){0.20f, 0.70f, 0.80f}, lib, 0.0f, 0.15f, 0.4f);
	Object_UpdateWorldBounds(sphere2);

	Object *sphere3 = ObjectList_Add(list);
	CreateSphereHighResolution(sphere3, (float3){0.0f, 1.5f, 9.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f}, (float3){0.80f, 0.80f, 0.80f}, lib, 0.0f, 0.05f, 0.9f);
	Object_UpdateWorldBounds(sphere3);

	Object *sphere4 = ObjectList_Add(list);
	CreateSphereHighResolution(sphere4, (float3){14.0f, 1.5f, 9.5f}, (float3){0.0f, 0.0f, 0.0f}, (float3){4.0f, 4.0f, 4.0f}, (float3){0.80f, 0.80f, 0.80f}, lib, 0.0f, 0.05f, 0.9f);
	Object_UpdateWorldBounds(sphere4);
}

// One of every primitive in SHAPE enum order, spawned through the ObjectRecord
// path (loadObjects.h) so the future scene-file loader gets exercised.
static void AddShapeParade(ObjectList *list, MaterialLib *lib) {
	static const struct {
		float y;
		float3 rotation;
		float3 scale;
		float3 color;
		float roughness;
		float metallic;
	} parade[SHAPE_COUNT] = {
		[SHAPE_CUBE]        = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.85f, 0.35f, 0.10f}, 0.75f, 0.00f},
		[SHAPE_SPHERE]      = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.90f, 0.20f, 0.20f}, 0.80f, 0.00f},
		[SHAPE_SPHERE_HIGH] = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.95f, 0.55f, 0.15f}, 0.80f, 0.00f},
		[SHAPE_PLANE]       = {0.05f, {0.0f, 0.0f, 0.0f}, {2.4f, 2.4f, 2.4f}, {0.65f, 0.70f, 0.75f}, 0.90f, 0.00f},
		[SHAPE_CONE]        = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.90f, 0.75f, 0.10f}, 0.80f, 0.00f},
		[SHAPE_CAPSULE]     = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.35f, 0.75f, 0.25f}, 0.80f, 0.00f},
		[SHAPE_TORUS]       = {0.41f, {0.0f, 0.0f, 0.0f}, {2.4f, 2.4f, 2.4f}, {0.20f, 0.65f, 0.85f}, 0.80f, 0.00f},
		[SHAPE_DISK]        = {0.05f, {0.0f, 0.0f, 0.0f}, {1.9f, 1.9f, 1.9f}, {0.80f, 0.80f, 0.85f}, 0.80f, 0.00f},
		[SHAPE_PYRAMID]     = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.85f, 0.65f, 0.20f}, 0.80f, 0.00f},
		[SHAPE_PRISM]       = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.55f, 0.30f, 0.80f}, 0.80f, 0.00f},
		[SHAPE_HEMISPHERE]  = {0.02f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.25f, 0.45f, 0.85f}, 0.85f, 0.00f},
		[SHAPE_TUBE]        = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.75f, 0.30f, 0.60f}, 0.80f, 0.00f},
		[SHAPE_QUAD]        = {0.85f, {0.0f, 180.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.85f, 0.85f, 0.85f}, 0.85f, 0.00f},
		[SHAPE_UV_SPHERE]   = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.30f, 0.70f, 0.60f}, 0.80f, 0.00f},
		[SHAPE_ICOSPHERE]   = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.95f, 0.85f, 0.55f}, 0.80f, 0.00f},
		[SHAPE_CYLINDER]    = {0.85f, {0.0f, 0.0f, 0.0f}, {1.6f, 1.6f, 1.6f}, {0.55f, 0.55f, 0.60f}, 0.80f, 0.00f},
	};

	// front tile row, centered on the spawn camera axis (x = 5 in main.c), so the
	// row is on screen at startup - z = 30 put it behind the sphere band instead
	const float kParadeZ = 3.2f;
	const float spacing = 3.0f;
	const float centerX = 5.0f;
	for (int i = 0; i < SHAPE_COUNT; i++) {
		ObjectRecord rec = {0};
		rec.type = OBJ_SHAPE;
		rec.shape = i;
		rec.position = (float3){centerX + (i - (SHAPE_COUNT - 1) * 0.5f) * spacing, parade[i].y, kParadeZ};
		rec.rotation = parade[i].rotation;
		rec.scale = parade[i].scale;
		rec.material = (ObjectMaterial){parade[i].color, 0.0f, parade[i].roughness, parade[i].metallic};
		spawnShape(ObjectList_Add(list), &rec, lib);
	}
}

void Scene_BuildShowcase(ObjectList *list, MaterialLib *lib) {
	AddTileGrid(list, lib);
	AddEmissiveCubes(list, lib);
	AddMaterialGrid(list, lib);
	AddReflectiveSpheres(list, lib);
	AddShapeParade(list, lib);
}
