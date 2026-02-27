#ifndef TEST_RAY_H
#define TEST_RAY_H

#include "../util/threadPool.h"
#include "../math/scalar.h"
#include "../math/transform.h"
#include "../math/vector3.h"
#include "../render/color/color.h"
#include "../object/format.h"
#include "../object/object.h"
#include "../render/render.h"
#include "../render/cpu/ray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    float averageTime;
    float medianTime;
    float minTime;
    float maxTime;
    float variance;
    float p99Time;
} PerformanceMetrics;

PerformanceMetrics ComputePerformanceMetrics(const float *timeTook, int samples);

#endif