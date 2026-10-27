#ifndef LOADOBJ_H
#define LOADOBJ_H

#include "../object/format.h"

typedef struct Object Object;
typedef struct MaterialLib MaterialLib;

void LoadObj(const char *filename, Object *obj, MaterialLib *lib);
Error SaveObj(const char *filename, Object *obj, MaterialLib *lib, float boost);

#endif // LOADOBJ_H