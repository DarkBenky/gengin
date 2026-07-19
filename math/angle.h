#ifndef MATH_ANGLE_H
#define MATH_ANGLE_H
#define M_PI 3.14159265358979323846f

#include <math.h>

#include "../object/format.h"


static inline float angleToRadian(float angle) {
    return angle * M_PI / 180.0f;
}

static inline float radianToAngle(float radian) {
    return radian * 180.0f / M_PI;
}

#endif