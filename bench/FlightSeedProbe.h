// Diagnostic harness: replays the flightBench scenario generator (fbDraw) and
// rollout (fbRun) over many more seeds than the pinned 4-per-tier suite, to
// separate a real controller change from the chaos band of a 20-scenario
// rollout.  Build and run:
//   make build/bench/FlightSeedProbe && ./build/bench/FlightSeedProbe 96
// (seeds per tier).
#ifndef FLIGHT_SEED_PROBE_H
#define FLIGHT_SEED_PROBE_H

#define FLIGHT_BENCH 1
// flightBench.c is included for its scenario generator (fbDraw) and rollout
// (fbRun); its own main is renamed so this probe can own main().
#define main fbSuiteMain
#include "../simulation/cSim/flightBench.c"
#undef main

#include "../simulation/cSim/simulate.c"
#include "../simulation/cSim/import.c"

// flightControl.h has no include guard, so missile.c cannot be pulled into this
// TU as well; the probe only flies planes, so the two missile entry points that
// flightBench.c references are stubbed out.
int missileLoadBin(Missile *m, const char *path, float3 position, float3 velocity) {
	(void)m; (void)path; (void)position; (void)velocity;
	return 1;
}
int missileUpdate(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime) {
	(void)m; (void)targetPosition; (void)targetVelocity; (void)deltaTime;
	return 0;
}

#endif
