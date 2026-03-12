#ifndef SCENE_H
#define SCENE_H

#include "object.h"

int DemoScene_ObjectCount(void);
void DemoScene_Build(Object *objects, MaterialLib *lib);
void DemoScene_Update(Object *objects, int frame);
void Scene_Destroy(Object *objects, int objectCount);
int Scene_CountTriangles(const Object *objects, int objectCount);

// Dynamic object list — no manual counting required
typedef struct {
	Object *objects;
	int count;
	int capacity;
} ObjectList;

void ObjectList_Init(ObjectList *list, int initialCapacity);
Object *ObjectList_Add(ObjectList *list);  // returns pointer to new zeroed slot
void ObjectList_Destroy(ObjectList *list); // destroys every object and frees the array
int ObjectList_CountTriangles(const ObjectList *list);

// Merge all objects in src into a single world-space mesh added to dst.
// Src objects are destroyed and the list is reset after merging.
void ObjectList_Merge(ObjectList *src, ObjectList *dst);

#endif // SCENE_H
