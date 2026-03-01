#include "timings.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int floatCmp(const void *a, const void *b) {
	float fa = *(const float *)a;
	float fb = *(const float *)b;
	return (fa > fb) - (fa < fb);
}

PerformanceMetrics ComputePerformanceMetrics(const float *timeTook, int samples) {
	PerformanceMetrics m = {0};
	if (!timeTook || samples <= 0) return m;

	m.minTime = timeTook[0];
	m.maxTime = timeTook[0];

	for (int i = 0; i < samples; i++) {
		m.averageTime += timeTook[i];
		if (timeTook[i] < m.minTime) m.minTime = timeTook[i];
		if (timeTook[i] > m.maxTime) m.maxTime = timeTook[i];
	}
	m.averageTime /= samples;

	for (int i = 0; i < samples; i++) {
		float diff = timeTook[i] - m.averageTime;
		m.variance += diff * diff;
	}
	m.variance /= samples;

	float *sorted = malloc(samples * sizeof(float));
	memcpy(sorted, timeTook, samples * sizeof(float));
	qsort(sorted, samples, sizeof(float), floatCmp);

	m.medianTime = (samples % 2 == 0)
					   ? (sorted[samples / 2 - 1] + sorted[samples / 2]) * 0.5f
					   : sorted[samples / 2];

	int p99Index = (int)(0.99f * samples);
	if (p99Index >= samples) p99Index = samples - 1;
	m.p99Time = sorted[p99Index];

	free(sorted);
	return m;
}