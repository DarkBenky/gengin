#ifndef RENDER_H
#define RENDER_H

#include "../object/format.h"
#include "../object/object.h"

void RenderObject(const Object *obj, const Camera *camera);
void RenderSetup(const Object *objects, int objectCount, Camera *camera);
void RenderObjects(const Object *objects, int objectCount, Camera *camera);
void TestFunctions();
#endif // RENDER_H