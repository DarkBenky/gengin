#ifndef SCENE_H
#define SCENE_H

#include "object.h"

int DemoScene_ObjectCount(void);
void DemoScene_Build(Object *objects, MaterialLib *lib);
void DemoScene_Update(Object *objects, int frame);
void Scene_Destroy(Object *objects, int objectCount);
int Scene_CountTriangles(const Object *objects, int objectCount);

#endif // SCENE_H
