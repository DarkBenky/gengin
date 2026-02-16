#ifndef MATH_SCALAR_H
#define MATH_SCALAR_H

static inline float MinF32(float a, float b) {
	return a < b ? a : b;
}

static inline float MaxF32(float a, float b) {
	return a > b ? a : b;
}

#endif // MATH_SCALAR_H
