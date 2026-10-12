// Missile demo/regression; run from the repo root.

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "math/vector3.h"
#include "simulation/cSim/import.h"
#include "simulation/cSim/missile.h"
#include "simulation/cSim/simulate.h"

#define DT (1.0f / 60.0f)
#define MAX_STEPS ((int)(MISSILE_LIFETIME / DT))

typedef enum { SC_TAIL, SC_HEADON, SC_CROSS, SC_JINK, SC_BEAM } ScenarioKind;

static const char *scenarioNames[] = {"tail", "head-on", "cross", "jink", "beam"};

typedef struct {
	float3 targetPos;
	float3 targetVel;
	float3 targetVel2; // reversed at switchTime (jink)
	float3 missileVel;
	float switchTime; // 0 = no switch
} Scenario;

// Realistic engagement geometry, not a lucky spawn.
static Scenario makeScenario(ScenarioKind kind) {
	Scenario s = {0};
	s.missileVel = (float3){0.0f, 0.0f, 300.0f, 0.0f};
	switch (kind) {
	case SC_TAIL:
		s.targetPos = (float3){0.0f, 1000.0f, 4000.0f, 0.0f};
		s.targetVel = (float3){0.0f, 0.0f, 250.0f, 0.0f};
		break;
	case SC_HEADON:
		s.targetPos = (float3){0.0f, 1000.0f, 8000.0f, 0.0f};
		s.targetVel = (float3){0.0f, 0.0f, -250.0f, 0.0f};
		break;
	case SC_CROSS:
		s.targetPos = (float3){0.0f, 1000.0f, 3000.0f, 0.0f};
		s.targetVel = (float3){250.0f, 0.0f, 0.0f, 0.0f};
		break;
	case SC_JINK:
		s.targetPos = (float3){0.0f, 1000.0f, 5000.0f, 0.0f};
		s.targetVel = (float3){250.0f, 0.0f, 0.0f, 0.0f};
		s.targetVel2 = (float3){-250.0f, 0.0f, 0.0f, 0.0f};
		s.switchTime = 3.0f;
		break;
	case SC_BEAM:
		s.targetPos = (float3){0.0f, 1000.0f, 2500.0f, 0.0f};
		s.targetVel = (float3){350.0f, 0.0f, 0.0f, 0.0f};
		break;
	}
	return s;
}

static int finiteV(float3 v) {
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The outcomes a working guidance law must produce (regression gate): every
// class connects head-on and in the tail chase; this agile set must also
// re-lead the reversing jink and the close fast beam crossing.  Heavy, slow or
// older airframes and the cross geometry are informational (printed, not
// gated).
static int shouldHit(const char *model, ScenarioKind kind) {
	static const char *jinkSet[] = {"AIM-9B", "AIM-9X", "R-3S", "R-73", "R-77", "AIM-120C", "MISSILE"};
	static const char *beamSet[] = {"AIM-9B", "AIM-9X", "R-3S", "R-73", "R-77", "MISSILE"};
	switch (kind) {
	case SC_HEADON:
	case SC_TAIL:
		return 1;
	case SC_JINK:
		for (int i = 0; i < (int)(sizeof(jinkSet) / sizeof(jinkSet[0])); i++) {
			if (strcmp(model, jinkSet[i]) == 0) return 1;
		}
		return 0;
	case SC_BEAM:
		for (int i = 0; i < (int)(sizeof(beamSet) / sizeof(beamSet[0])); i++) {
			if (strcmp(model, beamSet[i]) == 0) return 1;
		}
		return 0;
	case SC_CROSS:
		return 0;
	}
	return 0;
}

static int runMissile(const char *model, ScenarioKind kind) {
	char path[128];
	snprintf(path, sizeof(path), "simulation/simModels/%s.bin", model);

	Missile m;
	Scenario sc = makeScenario(kind);
	if (missileLoadBin(&m, path, (float3){0.0f, 1000.0f, 0.0f, 0.0f}, sc.missileVel) != 0) {
		printf("%-10s %-8s LOAD FAIL\n", model, scenarioNames[kind]);
		return 0;
	}

	float minDist = 1e30f, maxSpeed = 0.0f, burnout = -1.0f, tHit = -1.0f;
	int hit = 0, unstable = 0;
	float3 targetPos = sc.targetPos;
	float3 targetVel = sc.targetVel;
	for (int i = 0; i < MAX_STEPS; i++) {
		float t = i * DT;
		if (sc.switchTime > 0.0f && t >= sc.switchTime) targetVel = sc.targetVel2;
		targetPos = Float3_Add(targetPos, Float3_Scale(targetVel, DT));

		if (burnout < 0.0f && m.phase == MISSILE_COAST) burnout = t;
		if (!missileUpdate(&m, targetPos, targetVel, DT)) {
			if (m.hit) { hit = 1; tHit = t; }
			break;
		}
		if (!finiteV(m.position) || !finiteV(m.velocity)) { unstable = 1; break; }

		float d = Float3_Length(Float3_Sub(targetPos, m.position));
		if (d < minDist) minDist = d;
		float sp = Float3_Length(m.velocity);
		if (sp > maxSpeed) maxSpeed = sp;
	}

	int expectHit = shouldHit(model, kind);
	int ok = !unstable && (!expectHit || hit);
	printf("%-10s %-8s %s%s  tHit=%6.1f  miss=%8.1f  vmax=%6.1f  burnout=%5.1f\n",
		   model, scenarioNames[kind], hit ? "HIT " : "MISS",
		   (expectHit && !hit) ? " <EXPECTED HIT>" : "",
		   tHit, minDist, maxSpeed, burnout);
	return ok;
}

// Plant check: TVC yaw during burn, full fin pitch -> climb (via missileStep).
static int runDeflect(const char *model) {
	char path[128];
	snprintf(path, sizeof(path), "simulation/simModels/%s.bin", model);

	Missile m;
	if (missileLoadBin(&m, path, (float3){0.0f, 1000.0f, 0.0f, 0.0f}, (float3){0.0f, 0.0f, 300.0f, 0.0f}) != 0) {
		printf("%-10s %-8s LOAD FAIL\n", model, "deflect");
		return 0;
	}
	float3 farTarget = {0.0f, 200000.0f, 200000.0f, 0.0f};
	float3 zeroVel = {0.0f, 0.0f, 0.0f, 0.0f};
	int ok = 1;
	float tvcVx = 0.0f;

	if (m.spec.tvcMaxDeg > 0.0f) {
		for (int i = 0; i < 120; i++) {
			missileSetThrustYaw(&m, 1.0f);
			if (!missileStep(&m, farTarget, zeroVel, DT)) break;
		}
		tvcVx = m.velocity.x;
		ok = isfinite(tvcVx) && fabsf(tvcVx) > 15.0f;
		missileSetThrustYaw(&m, 0.0f);
	}

	for (int i = 0; i < 300; i++) {
		missileSetFrontPitch(&m, 1.0f);
		missileSetBackPitch(&m, 1.0f);
		if (!missileStep(&m, farTarget, zeroVel, DT)) break;
	}
	ok = ok && isfinite(m.velocity.y) && m.velocity.y > 15.0f;

	printf("%-10s %-8s tvx=%7.1f  vy=%7.1f  %s\n", model, "deflect", tvcVx, m.velocity.y, ok ? "OK" : "FAIL");
	return ok;
}

// Sustained-turn comparison: hold ~20 deg AoA for 15 s; TurnKing must beat Brick.
static int runTurn(const char *model, float *outHeading) {
	char path[128];
	snprintf(path, sizeof(path), "simulation/simModels/%s.bin", model);

	Plane p;
	if (loadPlaneBin(&p, path, (float3){0.0f, 0.0f, 1.0f, 0.0f},
					 (float3){0.0f, 1000.0f, 0.0f, 0.0f}, 250.0f, 1.0f) != 0) {
		printf("%-10s %-8s LOAD FAIL\n", model, "turn");
		*outHeading = 0.0f;
		return 0;
	}
	for (int i = 0; i < 60; i++) {
		planeSetElevator01(&p, 0.5f);
		planeSetAileron01(&p, 0.5f);
		updatePlane(&p, DT, NULL);
	}

	float cum = 0.0f, prev = atan2f(p.forward.x, p.forward.z);
	int unstable = 0;
	for (int i = 0; i < 900; i++) {
		float3 v = p.velocity;
		float sp = Float3_Length(v);
		float vf = sp > 1.0f ? Float3_Dot(v, p.forward) / sp : 1.0f;
		float vu = sp > 1.0f ? Float3_Dot(v, planeGetUpVector(&p)) / sp : 0.0f;
		float aoa = atan2f(-vu, fmaxf(vf, 0.01f)) * 57.2958f;
		planeSetElevator01(&p, clampf01(0.5f - (20.0f - aoa) * 0.015f + p.pitchRate * 0.03f));
		updatePlane(&p, DT, NULL);

		float cur = atan2f(p.forward.x, p.forward.z);
		float d = cur - prev;
		while (d > 3.14159265f) d -= 6.28318531f;
		while (d < -3.14159265f) d += 6.28318531f;
		cum += d;
		prev = cur;
		if (!finiteV(p.position) || !finiteV(p.velocity) || !isfinite(cum)) { unstable = 1; break; }
	}

	printf("%-10s %-8s cumHeading=%8.1f deg  spd=%6.1f  alt=%8.1f  %s\n",
		   model, "turn", cum * 57.2958f, p.currentSpeed, p.position.y, unstable ? "UNSTABLE" : "OK");
	*outHeading = fabsf(cum) * 57.2958f;
	return !unstable;
}

int main(void) {
	static const char *models[] = {"AIM-9B", "AIM-9X", "AIM-7E", "AIM-120A", "AIM-120B", "AIM-120C", "AIM-120C7", "AIM-54C",
								   "R-3S", "R-73", "R-27ER", "R-77", "R-33", "MISSILE"};
	int ok = 1;
	for (int mi = 0; mi < (int)(sizeof(models) / sizeof(models[0])); mi++) {
		for (int sc = 0; sc < 5; sc++) {
			if (!runMissile(models[mi], (ScenarioKind)sc)) ok = 0;
		}
		if (!runDeflect(models[mi])) ok = 0;
	}

	// The turn king must out-turn the brick.
	float turnKing = 0.0f, brick = 0.0f;
	if (!runTurn("TurnKing", &turnKing)) ok = 0;
	if (!runTurn("Brick", &brick)) ok = 0;
	if (!(turnKing > brick)) {
		printf("turn compare FAIL: TurnKing %.1f <= Brick %.1f\n", turnKing, brick);
		ok = 0;
	} else {
		printf("turn compare OK: TurnKing %.1f > Brick %.1f\n", turnKing, brick);
	}

	printf("OVERALL: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
