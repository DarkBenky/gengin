// Maneuver regression tests for the flight model in simulate.c: sustained
// full-stick pulls (bounded AoA, no frame flips), vertical climb, continuous
// roll, and a modulated pull that completes full loops.  One PASS/FAIL line
// per case; exits nonzero if any case fails.  Run from the repo root.

#include <stdio.h>
#include <math.h>
#include "simulation/cSim/import.h"
#include "simulation/cSim/simulate.h"

static float clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float angleBetween(float3 a, float3 b) {
	float d = a.x * b.x + a.y * b.y + a.z * b.z;
	float cx = a.y * b.z - a.z * b.y, cy = a.z * b.x - a.x * b.z, cz = a.x * b.y - a.y * b.x;
	return atan2f(sqrtf(cx * cx + cy * cy + cz * cz), d) * 57.2958f;
}

static float pitchDeg(const Plane *p) { float y = p->forward.y; return asinf(y < -1 ? -1 : (y > 1 ? 1 : y)) * 57.2958f; }
static float gammaDeg(const Plane *p);
static float aoaDegOf(const Plane *p);

static float runLoop(float elev, float thr, float spd0, const char *label) {
	Plane p; loadPlaneBin(&p, "simulation/simModels/F-16C.bin", (float3){0, 0, 1, 0}, (float3){0, 1000, 0, 0}, spd0, thr);
	const float dt = 1.0f / 60.0f;
	for (int i = 0; i < 60; i++) { planeSetElevator01(&p, 0.5f); planeSetAileron01(&p, 0.5f); updatePlane(&p, dt, NULL); }
	planeSetElevator01(&p, elev);
	float cum = 0.0f, prev = atan2f(p.forward.y, p.forward.z);
	float3 prevRight = planeGetRightVector(&p);
	float maxJump = 0.0f, maxAoa = 0.0f;
	int nonFinite = 0;
	for (int i = 0; i < 1800; i++) {
		planeSetElevator01(&p, elev);
		updatePlane(&p, dt, NULL);
		float cur = atan2f(p.forward.y, p.forward.z);
		float d = cur - prev;
		while (d > 3.14159265f) d -= 6.28318531f;
		while (d < -3.14159265f) d += 6.28318531f;
		cum += d; prev = cur;
		float3 r = planeGetRightVector(&p);
		float j = angleBetween(r, prevRight); prevRight = r;
		if (j > maxJump) maxJump = j;
		if (i > 120 && fabsf(aoaDegOf(&p)) > maxAoa) maxAoa = fabsf(aoaDegOf(&p));
		if (!isfinite(p.forward.x) || !isfinite(p.forward.y) || !isfinite(p.forward.z)) nonFinite = 1;
	}
	int ok = (maxAoa < 70.0f) && (maxJump < 6.0f) && !nonFinite;
	printf("%-10s elev=%.1f thr=%.2f spd0=%.0f cumPitch=%8.1f maxAoA=%5.1f jump=%5.2f  endAlt=%7.1f  spd=%.1f  => %s\n",
		label, elev, thr, spd0, cum * 57.2958f, maxAoa, maxJump, p.position.y, p.currentSpeed, ok ? "PASS" : "FAIL");
	return ok;
}

static int runVertical(void) {
	Plane p; loadPlaneBin(&p, "simulation/simModels/F-16C.bin", (float3){0, 0, 1, 0}, (float3){0, 1000, 0, 0}, 250.0f, 0.9f);
	const float dt = 1.0f / 60.0f;
	for (int i = 0; i < 60; i++) { planeSetElevator01(&p, 0.5f); planeSetAileron01(&p, 0.5f); updatePlane(&p, dt, NULL); }
	float prevPitch = pitchDeg(&p);
	int good = 0, n = 900;
	for (int i = 0; i < n; i++) {
		float pitch = pitchDeg(&p);
		float rate = (pitch - prevPitch) / dt; prevPitch = pitch;
		float cmd = 0.5f - (85.0f - pitch) * 0.01f + rate * 0.005f;
		planeSetElevator01(&p, clampf01(cmd));
		updatePlane(&p, dt, NULL);
		if (i > 120 && pitchDeg(&p) > 60.0f) good++;
		if (i % 150 == 0) printf("  [vert t=%5.1fs pitch=%7.2f alt=%8.1f spd=%6.1f]\n", (i + 60) * dt, pitchDeg(&p), p.position.y, p.currentSpeed);
	}
	float frac = (float)good / (float)(n - 120);
	int ok = (frac > 0.6f) && (p.position.y > 1200.0f);
	printf("vertical   frac(pitch>60)=%.2f  finalPitch=%7.2f deg  alt=%7.1f  spd=%.1f  => %s\n", frac, pitchDeg(&p), p.position.y, p.currentSpeed, ok ? "PASS" : "FAIL");
	return ok;
}

static int runRoll(void) {
	Plane p; loadPlaneBin(&p, "simulation/simModels/F-16C.bin", (float3){0, 0, 1, 0}, (float3){0, 1000, 0, 0}, 250.0f, 1.0f);
	const float dt = 1.0f / 60.0f;
	for (int i = 0; i < 60; i++) { planeSetElevator01(&p, 0.5f); planeSetAileron01(&p, 0.5f); updatePlane(&p, dt, NULL); }
	planeSetAileron01(&p, 1.0f);
	float cum = 0.0f, prev = p.bankAngle, maxJump = 0.0f;
	float3 prevRight = planeGetRightVector(&p);
	int nonFinite = 0;
	for (int i = 0; i < 600; i++) {
		planeSetAileron01(&p, 1.0f);
		updatePlane(&p, dt, NULL);
		float cur = p.bankAngle;
		float d = cur - prev;
		while (d > 3.14159265f) d -= 6.28318531f;
		while (d < -3.14159265f) d += 6.28318531f;
		cum += d; prev = cur;
		float3 r = planeGetRightVector(&p);
		float j = angleBetween(r, prevRight); prevRight = r;
		if (j > maxJump) maxJump = j;
		if (!isfinite(p.bankAngle)) nonFinite = 1;
	}
	int ok = (fabsf(cum) >= 6.28318531f) && (maxJump < 12.0f) && !nonFinite;
	printf("roll       cumBank=%9.1f deg  maxRightJump=%5.2f  alt=%7.1f  spd=%.1f  => %s\n", cum * 57.2958f, maxJump, p.position.y, p.currentSpeed, ok ? "PASS" : "FAIL");
	return ok;
}

static float gammaDeg(const Plane *p) {
	float3 v = p->velocity;
	return atan2f(v.y, sqrtf(v.x * v.x + v.z * v.z)) * 57.2958f;
}
static float aoaDegOf(const Plane *p) {
	float3 fwd = planeGetForwardVector(p), up = planeGetUpVector(p);
	float3 v = p->velocity;
	float sp = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	float vf = sp > 1 ? (v.x * fwd.x + v.y * fwd.y + v.z * fwd.z) / sp : 1.0f;
	float vu = sp > 1 ? (v.x * up.x + v.y * up.y + v.z * up.z) / sp : 0.0f;
	return atan2f(-vu, fmaxf(vf, 0.01f)) * 57.2958f;
}

static int runModulatedLoop(void) {
	Plane p; loadPlaneBin(&p, "simulation/simModels/F-16C.bin", (float3){0, 0, 1, 0}, (float3){0, 1000, 0, 0}, 250.0f, 1.0f);
	const float dt = 1.0f / 60.0f;
	for (int i = 0; i < 60; i++) { planeSetElevator01(&p, 0.5f); planeSetAileron01(&p, 0.5f); updatePlane(&p, dt, NULL); }
	float cum = 0.0f, prev = atan2f(p.forward.y, p.forward.z), maxAoa = 0.0f;
	int nonFinite = 0;
	for (int i = 0; i < 3600; i++) {
		float aoa = aoaDegOf(&p);
		planeSetElevator01(&p, clampf01(0.5f - (20.0f - aoa) * 0.015f + p.pitchRate * 0.03f));
		updatePlane(&p, dt, NULL);
		float cur = atan2f(p.forward.y, p.forward.z);
		float d = cur - prev;
		while (d > 3.14159265f) d -= 6.28318531f;
		while (d < -3.14159265f) d += 6.28318531f;
		cum += d; prev = cur;
		float a2 = fabsf(aoaDegOf(&p));
		if (a2 > maxAoa) maxAoa = a2;
		if (!isfinite(cur)) { nonFinite = 1; break; }
	}
	int ok = (fabsf(cum) >= 5.93412f) && (maxAoa < 60.0f) && !nonFinite;
	printf("modLoop    cumPitch=%8.1f deg  maxAoA=%5.1f  endAlt=%7.1f  spd=%.1f  => %s\n", cum * 57.2958f, maxAoa, p.position.y, p.currentSpeed, ok ? "PASS" : "FAIL");
	return ok;
}

int main(void) {
	int a = runLoop(1.0f, 0.8f, 220.0f, "loop+");
	int b = runLoop(0.0f, 0.8f, 220.0f, "loop-");
	runLoop(1.0f, 1.0f, 220.0f, "loop+AB");
	runLoop(0.0f, 1.0f, 220.0f, "loop-AB");
	runLoop(1.0f, 1.0f, 320.0f, "loop+fast");
	runLoop(0.0f, 1.0f, 320.0f, "loop-fast");
	int c = runVertical();
	int d = runRoll();
	int e = runModulatedLoop();
	printf("OVERALL: %s\n", (a && b && c && d && e) ? "PASS" : "FAIL");
	return (a && b && c && d && e) ? 0 : 1;
}
