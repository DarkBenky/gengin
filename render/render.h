#ifndef RENDER_H
#define RENDER_H

#include "../object/format.h"
#include "../object/object.h"

void RenderObject(const Object *obj, const Camera *camera);
void RenderObjects(const Object *objects, int objectCount, Camera *camera);
#endif // RENDER_H