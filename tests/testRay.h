#ifndef TEST_RAY_H
#define TEST_RAY_H

#include "../util/threadPool.h"
#include "../math/scalar.h"
#include "../math/transform.h"
#include "../math/vector3.h"
#include "../render/color/color.h"
#include "../object/format.h"
#include "../object/object.h"
#include "../object/scene.h"
#include "../load/loadObj.h"
#include "../render/render.h"
#include "../render/cpu/ray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void RenderScene(const Object *objects, int objectCount, Camera *camera);
void RenderSceneRow(const Object *objects, int objectCount, Camera *camera, int row, int rowCount);
void RenderTaskFunction(void *arg);

typedef struct {
	const Object *objects;
	int objectCount;
	Camera *camera;
	int row;
	int rowCount;
} RenderTask;

#endif