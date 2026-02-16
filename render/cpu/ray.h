#ifndef RAY_H
#define RAY_H

#include "../../object/format.h"
#include "../../object/object.h"
#include "../render.h"

void ShadowPostProcess(const Object *objects, int objectCount, Camera *camera);
bool IntersectAnyBBox(const Object *objects, int objectCount, float3 rayOrigin, float3 rayDir);

#endif // RAY_H