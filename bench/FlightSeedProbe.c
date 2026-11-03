#include "FlightSeedProbe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define PROBE_THREADS 24
#define PROBE_MAX 2000

typedef struct {
	Scenario scenario;
	int tier;
} ProbeCase;

static ProbeCase cases[PROBE_MAX];
static ScenarioResult results[PROBE_MAX];
static int caseCount;
static int nextIndex;
static char modelPathBuf[256];
static int stepsGlob;
static float dtGlob;

static void *probeWorker(void *arg) {
	(void)arg;
	for (;;) {
		int i = __atomic_fetch_add(&nextIndex, 1, __ATOMIC_RELAXED);
		if (i >= caseCount) {
			break;
		}
		results[i] = fbRun(&cases[i].scenario, stepsGlob, dtGlob, modelPathBuf, 0, NULL);
	}
	return NULL;
}

int main(int argc, char **argv) {
	int seeds = argc > 1 ? atoi(argv[1]) : 24;
	const char *model = argc > 2 ? argv[2] : "F-16C";
	stepsGlob = argc > 3 ? atoi(argv[3]) : 1800;
	dtGlob = argc > 4 ? (float)atof(argv[4]) : (1.0f / 60.0f);
	if (seeds < 1 || seeds * FB_TIERS > PROBE_MAX) {
		fprintf(stderr, "seeds must be 1..%d\n", PROBE_MAX / FB_TIERS);
		return 2;
	}
	snprintf(modelPathBuf, sizeof(modelPathBuf), "simulation/simModels/%s.bin", model);

	caseCount = 0;
	for (int tier = 0; tier < FB_TIERS; tier++) {
		for (uint32_t seed = 0; seed < (uint32_t)seeds; seed++) {
			fbDraw(&cases[caseCount].scenario, tier, seed);
			cases[caseCount].tier = tier;
			caseCount++;
		}
	}

	pthread_t threads[PROBE_THREADS];
	for (int i = 0; i < PROBE_THREADS; i++) {
		pthread_create(&threads[i], NULL, probeWorker, NULL);
	}
	for (int i = 0; i < PROBE_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	double tierSum[FB_TIERS] = {0};
	int tierHits[FB_TIERS] = {0};
	int unstable = 0;
	for (int i = 0; i < caseCount; i++) {
		tierSum[cases[i].tier] += results[i].miss;
		tierHits[cases[i].tier] += results[i].hit;
		unstable += results[i].unstable;
	}

	double total = 0.0;
	int hits = 0;
	for (int t = 0; t < FB_TIERS; t++) {
		total += tierSum[t];
		hits += tierHits[t];
	}
	printf("seeds=%d model=%s steps=%d dt=%.6f unstable=%d\n", seeds, model, stepsGlob, dtGlob, unstable);
	printf("agg miss %.3f hitRate %.3f\n", total / caseCount, (double)hits / caseCount);
	for (int t = 0; t < FB_TIERS; t++) {
		printf("  %-7s miss %8.3f hitRate %.3f\n", tierNames[t], tierSum[t] / seeds, (double)tierHits[t] / seeds);
	}
	return 0;
}
