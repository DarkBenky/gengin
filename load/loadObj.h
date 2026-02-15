#ifndef LOADOBJ_H
#define LOADOBJ_H

#include "../object/format.h"

typedef struct Object Object;

void LoadObj(const char *filename, Object *obj);

#endif // LOADOBJ_H