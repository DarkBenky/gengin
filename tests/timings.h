#ifndef TEST_TIMINGS_H
#define TEST_TIMINGS_H

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