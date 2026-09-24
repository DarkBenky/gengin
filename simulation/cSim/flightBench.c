// Deterministic, headless bench for the flight controller (simulation/cSim).
//
//   build/flightBench/flightBench                  JSON metrics, all scenarios
//   build/flightBench/flightBench --steps 400      shorter rollouts
//   build/flightBench/flightBench --threads 1      serial (reference run)
//   build/flightBench/flightBench --trace weave:2  one scenario + per-step trace
//
// The controller is driven exactly like the shipped driver in flightControl.c:
// getControllerOutputV5() with the tuned loss, surfaces set in 01 units, full
// throttle, updatePlane(dt).  Everything is seeded, so the same binary prints
// the same numbers and a stored baseline is meaningful: scenarios run in
// parallel but each one owns its plane and target state, and only costUs (a
// wall-clock median) differs between runs.
//
// Tiers (4 seeds each): static, drift, weave, step, jink.
// Per-scenario metrics: miss (closest approach), finalDist, hit + tHit against
// FB_HIT_RADIUS, effort (integrated |deflection|), saturation steps, stability
// flag, and the median per-step controller cost in microseconds.

#ifndef FLIGHT_BENCH
#define FLIGHT_BENCH 1
#endif
#include "flightControl.c"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define FB_HIT_RADIUS 25.0f
#define FB_DEFAULT_STEPS 1800
#define FB_DEFAULT_THREADS 8
#define FB_MAX_STEPS 4000
#define FB_TIERS 5
#define FB_SEEDS 4
#define FB_MAX_SCENARIOS (FB_TIERS * FB_SEEDS)

static const char *tierNames[FB_TIERS] = { "static", "drift", "weave", "step", "jink" };

typedef enum { TG_STATIC, TG_DRIFT, TG_WEAVE, TG_STEP, TG_JINK } TargetKind;

typedef struct {
	char id[24];
	const char *tier;
	TargetKind kind;
	uint32_t seed;
	float3 p0;
	float3 v;       // primary velocity (m/s)
	float3 v2;      // step tier: velocity after the switch
	float3 lateral; // weave axis (unit)
	float amplitude;
	float omega;
	float tSwitch;
	float jinkInterval;
	float jinkAccel;
} Scenario;

typedef struct {
	float3 position;
	float3 velocity;
	float t;
	float nextJink;
	uint32_t rng;
} TargetState;

typedef struct {
	const Scenario *scenario;
	float miss;
	float finalDist;
	float tHit;
	float effort;
	int hit;
	int satSteps;
	int unstable;
	double costUs;
} ScenarioResult;

static uint32_t fbRngNext(uint32_t *state) {
	uint32_t x = *state ? *state : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static float fbRange(uint32_t *state, float lo, float hi) {
	float unit = (float)(fbRngNext(state) & 0xffffffu) / 16777215.0f;
	return lo + (hi - lo) * unit;
}

static float3 fbLateral(float3 v) {
	float3 up = { 0.0f, 1.0f, 0.0f, 0.0f };
	float3 axis = Float3_Cross(v, up);
	if (Float3_Length(axis) < 1e-3f) {
		axis = Float3_Cross(v, (float3){ 1.0f, 0.0f, 0.0f, 0.0f });
	}
	if (Float3_Length(axis) < 1e-3f) {
		return (float3){ 1.0f, 0.0f, 0.0f, 0.0f };
	}
	return Float3_Normalize(axis);
}

static void fbDraw(Scenario *s, int tier, uint32_t seed) {
	uint32_t rng = 0x9e3779b9u + (uint32_t)tier * 0x85ebca6bu + seed * 0xc2b2ae35u;
	s->kind = (TargetKind)tier;
	s->tier = tierNames[tier];
	s->seed = seed;
	snprintf(s->id, sizeof(s->id), "%s:%u", tierNames[tier], seed);

	// Spawn 250-800 m out inside a forward cone (the plane starts at 180 m/s
	// pointing +z), so the geometry is reachable and the tiers separate by how
	// well the controller handles motion rather than by spawn luck.
	float distance = fbRange(&rng, 250.0f, 800.0f);
	float azimuth = fbRange(&rng, -1.31f, 1.31f);
	float elevation = fbRange(&rng, -0.17f, 0.61f);
	float3 direction = {
		cosf(elevation) * sinf(azimuth),
		sinf(elevation),
		cosf(elevation) * cosf(azimuth),
		0.0f,
	};
	s->p0 = Float3_Add((float3){ 0.0f, 1000.0f, 0.0f, 1.0f }, Float3_Scale(direction, distance));

	// Primary velocity: 12-45 m/s on a seeded horizontal heading.
	float speed = fbRange(&rng, 12.0f, 45.0f);
	float heading = fbRange(&rng, -3.14159265f, 3.14159265f);
	s->v = (float3){ speed * sinf(heading), 0.0f, speed * cosf(heading), 0.0f };
	s->v2 = s->v;
	s->lateral = fbLateral(s->v);
	s->amplitude = fbRange(&rng, 30.0f, 120.0f);
	s->omega = 6.28318531f / fbRange(&rng, 2.0f, 6.0f);
	s->tSwitch = fbRange(&rng, 2.0f, 5.0f);
	s->jinkInterval = fbRange(&rng, 0.5f, 1.5f);
	s->jinkAccel = fbRange(&rng, 6.0f, 30.0f);

	switch (s->kind) {
	case TG_STATIC:
		s->v = (float3){ 0.0f, 0.0f, 0.0f, 0.0f };
		break;
	case TG_STEP: {
		// Sudden direction change of 30-150 deg plus 0.6-1.5x speed change.
		float angle = fbRange(&rng, 0.52f, 2.62f);
		if (fbRange(&rng, 0.0f, 1.0f) < 0.5f) {
			angle = -angle;
		}
		float scale = fbRange(&rng, 0.6f, 1.5f);
		float ca = cosf(angle);
		float sa = sinf(angle);
		s->v2 = (float3){
			(s->v.x * ca - s->v.z * sa) * scale,
			s->v.y,
			(s->v.x * sa + s->v.z * ca) * scale,
			0.0f,
		};
		break;
	}
	default:
		break;
	}
}

static void fbTargetInit(const Scenario *s, TargetState *ts) {
	ts->position = s->p0;
	ts->velocity = s->v;
	ts->t = 0.0f;
	ts->nextJink = s->jinkInterval;
	ts->rng = s->seed * 747796405u + 2891336453u;
}

static void fbTargetStep(const Scenario *s, TargetState *ts, float dt) {
	ts->t += dt;
	switch (s->kind) {
	case TG_STATIC:
		ts->velocity = (float3){ 0.0f, 0.0f, 0.0f, 0.0f };
		break;
	case TG_WEAVE:
		ts->velocity = Float3_Add(
			s->v,
			Float3_Scale(s->lateral, s->amplitude * s->omega * cosf(s->omega * ts->t)));
		break;
	case TG_STEP:
		ts->velocity = (ts->t < s->tSwitch) ? s->v : s->v2;
		break;
	case TG_JINK:
		if (ts->t >= ts->nextJink) {
			ts->nextJink = ts->t + s->jinkInterval;
			float3 impulse = {
				fbRange(&ts->rng, -1.0f, 1.0f),
				fbRange(&ts->rng, -0.6f, 0.9f),
				fbRange(&ts->rng, -1.0f, 1.0f),
				0.0f,
			};
			float len = Float3_Length(impulse);
			if (len > 1e-4f) {
				ts->velocity = Float3_Add(ts->velocity, Float3_Scale(Float3_Scale(impulse, 1.0f / len), s->jinkAccel));
			}
			float speed = Float3_Length(ts->velocity);
			if (speed > 60.0f) {
				ts->velocity = Float3_Scale(Float3_Scale(ts->velocity, 1.0f / speed), 60.0f);
			}
		}
		break;
	default:
		break;
	}
	ts->position = Float3_Add(ts->position, Float3_Scale(ts->velocity, dt));
}

static int fbCompareDouble(const void *a, const void *b) {
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

typedef struct {
	const Scenario *scenario;
	int steps;
	float dt;
	ScenarioResult result;
} FbJob;

static ScenarioResult fbRun(const Scenario *s, int steps, float dt, FILE *trace);

static void *fbWorker(void *arg) {
	FbJob *job = (FbJob *)arg;
	job->result = fbRun(job->scenario, job->steps, job->dt, NULL);
	return NULL;
}

static ScenarioResult fbRun(const Scenario *s, int steps, float dt, FILE *trace) {
	ScenarioResult r = { 0 };
	r.scenario = s;
	r.tHit = -1.0f;

	Plane plane;
	if (loadPlaneBin(&plane, "simulation/simModels/F-16C.bin",
			(float3){ 0.0f, 0.0f, 1.0f, 0.0f }, (float3){ 0.0f, 1000.0f, 0.0f, 1.0f },
			180.0f, 1.0f) != 0) {
		fprintf(stderr, "flightBench: cannot load simulation/simModels/F-16C.bin\n");
		exit(2);
	}

	Controller ctrl;
	initController(&ctrl, &plane);
	TargetState target;
	fbTargetInit(s, &target);

	float momentum[3] = { 0.0f, 0.0f, 0.0f };
	float prevLoss = 0.0f;
	float minDist = FLT_MAX;
	double costs[FB_MAX_STEPS];
	int costCount = 0;

	for (int step = 0; step < steps; step++) {
		float3 targetPos = target.position;

		struct timespec begin;
		struct timespec end;
		clock_gettime(CLOCK_MONOTONIC, &begin);
		ControllerOutput out = getControllerOutputV5(&ctrl, targetPos, dt, momentum, &prevLoss, 128, evaluateLossV2PlusTuned2);
		clock_gettime(CLOCK_MONOTONIC, &end);
		if (costCount < FB_MAX_STEPS) {
			costs[costCount++] = (end.tv_sec - begin.tv_sec) * 1e6 + (end.tv_nsec - begin.tv_nsec) / 1e3;
		}

		planeSetAileron01(&ctrl.plane, out.Aileron);
		planeSetElevator01(&ctrl.plane, out.Elevator);
		planeSetRudder01(&ctrl.plane, out.Rudder);
		planeSetThrottle01(&ctrl.plane, 1.0f);
		updatePlane(&ctrl.plane, dt, NULL);
		fbTargetStep(s, &target, dt);

		float dist = Float3_Length(Float3_Sub(target.position, ctrl.plane.position));
		if (dist < minDist) {
			minDist = dist;
		}
		if (r.tHit < 0.0f && dist <= FB_HIT_RADIUS) {
			r.tHit = (float)(step + 1) * dt;
		}
		r.effort += (fabsf(out.Aileron - 0.5f) + fabsf(out.Elevator - 0.5f) + fabsf(out.Rudder - 0.5f)) * 2.0f * dt;
		if (fabsf(out.Aileron - 0.5f) >= 0.49f || fabsf(out.Elevator - 0.5f) >= 0.49f || fabsf(out.Rudder - 0.5f) >= 0.49f) {
			r.satSteps++;
		}
		if (!isfinite(ctrl.plane.position.x) || !isfinite(ctrl.plane.position.y) || !isfinite(ctrl.plane.position.z)
				|| !isfinite(ctrl.plane.velocity.x) || !isfinite(ctrl.plane.velocity.y) || !isfinite(ctrl.plane.velocity.z)) {
			r.unstable = 1;
		}
		if (trace != NULL) {
			fprintf(trace, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
				(float)(step + 1) * dt,
				ctrl.plane.position.x, ctrl.plane.position.y, ctrl.plane.position.z,
				target.position.x, target.position.y, target.position.z,
				out.Aileron, out.Elevator, out.Rudder, dist);
		}
	}

	r.miss = minDist;
	r.finalDist = Float3_Length(Float3_Sub(target.position, ctrl.plane.position));
	r.hit = r.miss <= FB_HIT_RADIUS;
	qsort(costs, (size_t)costCount, sizeof(double), fbCompareDouble);
	r.costUs = costCount ? costs[costCount / 2] : 0.0;
	return r;
}

static uint32_t fbHash(uint32_t h, const void *data, size_t len) {
	const unsigned char *bytes = (const unsigned char *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= bytes[i];
		h *= 16777619u;
	}
	return h;
}

static uint32_t fbSuiteHash(const Scenario *scenarios, int count, int steps, float dt) {
	uint32_t h = 2166136261u;
	h = fbHash(h, "flightBench-v1", 14);
	h = fbHash(h, &steps, sizeof(steps));
	h = fbHash(h, &dt, sizeof(dt));
	float hitRadius = FB_HIT_RADIUS;
	h = fbHash(h, &hitRadius, sizeof(hitRadius));
	for (int i = 0; i < count; i++) {
		h = fbHash(h, scenarios[i].id, strlen(scenarios[i].id));
		h = fbHash(h, &scenarios[i].p0, sizeof(float3));
		h = fbHash(h, &scenarios[i].v, sizeof(float3));
		h = fbHash(h, &scenarios[i].v2, sizeof(float3));
		h = fbHash(h, &scenarios[i].lateral, sizeof(float3));
		h = fbHash(h, &scenarios[i].amplitude, sizeof(float));
		h = fbHash(h, &scenarios[i].omega, sizeof(float));
		h = fbHash(h, &scenarios[i].tSwitch, sizeof(float));
		h = fbHash(h, &scenarios[i].jinkInterval, sizeof(float));
		h = fbHash(h, &scenarios[i].jinkAccel, sizeof(float));
	}
	return h;
}

int main(int argc, char **argv) {
	int steps = FB_DEFAULT_STEPS;
	int threads = FB_DEFAULT_THREADS;
	const char *traceId = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
			steps = atoi(argv[++i]);
			if (steps < 1 || steps > FB_MAX_STEPS) {
				fprintf(stderr, "flightBench: --steps must be 1..%d\n", FB_MAX_STEPS);
				return 2;
			}
		} else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			threads = atoi(argv[++i]);
			if (threads < 1 || threads > FB_MAX_SCENARIOS) {
				fprintf(stderr, "flightBench: --threads must be 1..%d\n", FB_MAX_SCENARIOS);
				return 2;
			}
		} else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
			traceId = argv[++i];
		} else {
			fprintf(stderr, "usage: flightBench [--steps N] [--threads N] [--trace <tier:seed>]\n");
			return 2;
		}
	}

	const float dt = 1.0f / 60.0f;
	Scenario scenarios[FB_MAX_SCENARIOS];
	int count = 0;
	for (int tier = 0; tier < FB_TIERS; tier++) {
		for (uint32_t seed = 0; seed < FB_SEEDS; seed++) {
			fbDraw(&scenarios[count], tier, seed);
			count++;
		}
	}

	if (traceId != NULL) {
		for (int i = 0; i < count; i++) {
			if (strcmp(scenarios[i].id, traceId) != 0) {
				continue;
			}
			FILE *trace = tmpfile();
			if (trace == NULL) {
				fprintf(stderr, "flightBench: cannot open trace buffer\n");
				return 2;
			}
			ScenarioResult r = fbRun(&scenarios[i], steps, dt, trace);
			printf("{\"version\":1,\"steps\":%d,\"dt\":%.9f,\"hitRadius\":%.1f,\"suiteHash\":\"%08x\","
				   "\"scenario\":{\"id\":\"%s\",\"tier\":\"%s\",\"seed\":%u,\"miss\":%.3f,\"finalDist\":%.3f,"
				   "\"hit\":%s,\"tHit\":%.3f,\"effort\":%.4f,\"satSteps\":%d,\"unstable\":%d,\"costUs\":%.2f},"
				   "\"traceHeader\":\"t,px,py,pz,tx,ty,tz,aileron,elevator,rudder,dist\",\"trace\":[",
				   steps, dt, FB_HIT_RADIUS, fbSuiteHash(scenarios, count, steps, dt),
				   r.scenario->id, r.scenario->tier, r.scenario->seed, r.miss, r.finalDist,
				   r.hit ? "true" : "false", r.tHit, r.effort, r.satSteps, r.unstable, r.costUs);
			rewind(trace);
			char line[512];
			int first = 1;
			while (fgets(line, sizeof(line), trace) != NULL) {
				size_t len = strlen(line);
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
					line[--len] = '\0';
				}
				printf("%s[%s]", first ? "" : ",", line);
				first = 0;
			}
			printf("]}\n");
			fclose(trace);
			return 0;
		}
		fprintf(stderr, "flightBench: unknown scenario %s\n", traceId);
		return 2;
	}

	ScenarioResult results[FB_MAX_SCENARIOS];
	FbJob jobs[FB_MAX_SCENARIOS];
	pthread_t workers[FB_MAX_SCENARIOS];
	if (threads > count) {
		threads = count;
	}
	for (int base = 0; base < count; base += threads) {
		int batch = (count - base < threads) ? count - base : threads;
		for (int k = 0; k < batch; k++) {
			jobs[base + k] = (FbJob){ &scenarios[base + k], steps, dt, { 0 } };
			if (pthread_create(&workers[k], NULL, fbWorker, &jobs[base + k]) != 0) {
				fprintf(stderr, "flightBench: cannot start worker thread\n");
				return 2;
			}
		}
		for (int k = 0; k < batch; k++) {
			pthread_join(workers[k], NULL);
		}
	}
	for (int i = 0; i < count; i++) {
		results[i] = jobs[i].result;
	}

	printf("{\n");
	printf("  \"version\": 1,\n");
	printf("  \"settings\": {\"steps\": %d, \"dt\": %.9f, \"hitRadius\": %.1f, \"model\": \"F-16C\", "
		   "\"loss\": \"V2PlusTuned2\", \"maxIterations\": 128},\n", steps, dt, FB_HIT_RADIUS);
	printf("  \"suiteHash\": \"%08x\",\n", fbSuiteHash(scenarios, count, steps, dt));
	printf("  \"scenarios\": [\n");
	for (int i = 0; i < count; i++) {
		const ScenarioResult *r = &results[i];
		printf("    {\"id\": \"%s\", \"tier\": \"%s\", \"seed\": %u, \"miss\": %.3f, \"finalDist\": %.3f, "
			   "\"hit\": %s, \"tHit\": %.3f, \"effort\": %.4f, \"satSteps\": %d, \"unstable\": %d, \"costUs\": %.2f}%s\n",
			   r->scenario->id, r->scenario->tier, r->scenario->seed, r->miss, r->finalDist,
			   r->hit ? "true" : "false", r->tHit, r->effort, r->satSteps, r->unstable, r->costUs,
			   (i + 1 < count) ? "," : "");
	}
	printf("  ],\n");
	printf("  \"tiers\": [\n");
	for (int tier = 0; tier < FB_TIERS; tier++) {
		double missSum = 0.0;
		double effortSum = 0.0;
		double costSum = 0.0;
		int hits = 0;
		int seen = 0;
		int satSteps = 0;
		for (int i = 0; i < count; i++) {
			if (results[i].scenario->kind != (TargetKind)tier) {
				continue;
			}
			missSum += results[i].miss;
			effortSum += results[i].effort;
			costSum += results[i].costUs;
			hits += results[i].hit ? 1 : 0;
			satSteps += results[i].satSteps;
			seen++;
		}
		printf("    {\"tier\": \"%s\", \"miss\": %.3f, \"hitRate\": %.3f, \"effort\": %.4f, \"costUs\": %.2f, \"satSteps\": %d}%s\n",
			   tierNames[tier], seen ? missSum / seen : 0.0, seen ? (double)hits / seen : 0.0,
			   seen ? effortSum / seen : 0.0, seen ? costSum / seen : 0.0, satSteps,
			   (tier + 1 < FB_TIERS) ? "," : "");
	}
	printf("  ],\n");

	double missSum = 0.0;
	double effortSum = 0.0;
	double costSum = 0.0;
	int hits = 0;
	int satSteps = 0;
	int unstable = 0;
	for (int i = 0; i < count; i++) {
		missSum += results[i].miss;
		effortSum += results[i].effort;
		costSum += results[i].costUs;
		hits += results[i].hit ? 1 : 0;
		satSteps += results[i].satSteps;
		unstable += results[i].unstable;
	}
	printf("  \"aggregate\": {\"miss\": %.3f, \"hitRate\": %.3f, \"effort\": %.4f, \"costUs\": %.2f, "
		   "\"satSteps\": %d, \"unstable\": %d, \"scenarios\": %d}\n",
		   count ? missSum / count : 0.0, count ? (double)hits / count : 0.0,
		   count ? effortSum / count : 0.0, count ? costSum / count : 0.0,
		   satSteps, unstable, count);
	printf("}\n");
	return 0;
}
