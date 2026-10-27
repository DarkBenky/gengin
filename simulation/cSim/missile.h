#pragma once
#include "import.h"
#include "missileImport.h"
#include <math.h>

// Control helpers: norm = -1..1, pct = 0..100, 01 = 0..1; pct 50 and 01 0.5 are neutral.

typedef enum {
	MISSILE_BOOST,
	MISSILE_SUSTAIN,
	MISSILE_COAST,
	MISSILE_DEAD,
} MissilePhase;

typedef struct {
	MissileSpec spec;

	float3 position;
	float3 velocity;
	float3 forward; // unit, along velocity

	float fuelLeft; // kg
	int stage;
	float stageTime; // s
	float timeAlive;
	MissilePhase phase;
	int hit;

	// Commanded deflections, -1..1.
	float frontPitch;
	float frontYaw;
	float backPitch;
	float backYaw;
	float thrustPitch;
	float thrustYaw;

	// Actuated deflections the plant uses; slew toward the commands.
	float frontPitchAct, frontYawAct;
	float backPitchAct, backYawAct;
	float thrustPitchAct, thrustYawAct;
} Missile;

// Returns 0 on success.
int missileLoadBin(Missile *m, const char *path, float3 position, float3 velocity);

// Controller plus physics; returns 1 while the missile is flying.
int missileUpdate(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime);

// Physics only, no controller call.
int missileStep(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime);

// Fin lateral accel per unit deflection at the current dynamic pressure.
float missileFinAccelPerDeflection(const Missile *m);

#define MISSILE_LIFETIME 60.0f

static inline void missileSetFrontPitch(Missile *m, float v) {
	m->frontPitch = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetFrontPitchPct(Missile *m, float pct) {
	missileSetFrontPitch(m, pct * 0.02f - 1.0f);
}
static inline void missileSetFrontPitch01(Missile *m, float v) {
	missileSetFrontPitch(m, v * 2.0f - 1.0f);
}
static inline float missileGetFrontPitchPct(const Missile *m) {
	return (m->frontPitch + 1.0f) * 50.0f;
}
static inline float missileGetFrontPitch01(const Missile *m) {
	return (m->frontPitch + 1.0f) * 0.5f;
}
static inline float missileGetFrontPitchNorm(const Missile *m) {
	return m->frontPitch;
}

static inline void missileSetFrontYaw(Missile *m, float v) {
	m->frontYaw = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetFrontYawPct(Missile *m, float pct) {
	missileSetFrontYaw(m, pct * 0.02f - 1.0f);
}
static inline void missileSetFrontYaw01(Missile *m, float v) {
	missileSetFrontYaw(m, v * 2.0f - 1.0f);
}
static inline float missileGetFrontYawPct(const Missile *m) {
	return (m->frontYaw + 1.0f) * 50.0f;
}
static inline float missileGetFrontYaw01(const Missile *m) {
	return (m->frontYaw + 1.0f) * 0.5f;
}
static inline float missileGetFrontYawNorm(const Missile *m) {
	return m->frontYaw;
}

static inline void missileSetBackPitch(Missile *m, float v) {
	m->backPitch = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetBackPitchPct(Missile *m, float pct) {
	missileSetBackPitch(m, pct * 0.02f - 1.0f);
}
static inline void missileSetBackPitch01(Missile *m, float v) {
	missileSetBackPitch(m, v * 2.0f - 1.0f);
}
static inline float missileGetBackPitchPct(const Missile *m) {
	return (m->backPitch + 1.0f) * 50.0f;
}
static inline float missileGetBackPitch01(const Missile *m) {
	return (m->backPitch + 1.0f) * 0.5f;
}
static inline float missileGetBackPitchNorm(const Missile *m) {
	return m->backPitch;
}

static inline void missileSetBackYaw(Missile *m, float v) {
	m->backYaw = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetBackYawPct(Missile *m, float pct) {
	missileSetBackYaw(m, pct * 0.02f - 1.0f);
}
static inline void missileSetBackYaw01(Missile *m, float v) {
	missileSetBackYaw(m, v * 2.0f - 1.0f);
}
static inline float missileGetBackYawPct(const Missile *m) {
	return (m->backYaw + 1.0f) * 50.0f;
}
static inline float missileGetBackYaw01(const Missile *m) {
	return (m->backYaw + 1.0f) * 0.5f;
}
static inline float missileGetBackYawNorm(const Missile *m) {
	return m->backYaw;
}

static inline void missileSetThrustPitch(Missile *m, float v) {
	m->thrustPitch = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetThrustPitchPct(Missile *m, float pct) {
	missileSetThrustPitch(m, pct * 0.02f - 1.0f);
}
static inline void missileSetThrustPitch01(Missile *m, float v) {
	missileSetThrustPitch(m, v * 2.0f - 1.0f);
}
static inline float missileGetThrustPitchPct(const Missile *m) {
	return (m->thrustPitch + 1.0f) * 50.0f;
}
static inline float missileGetThrustPitch01(const Missile *m) {
	return (m->thrustPitch + 1.0f) * 0.5f;
}
static inline float missileGetThrustPitchNorm(const Missile *m) {
	return m->thrustPitch;
}

static inline void missileSetThrustYaw(Missile *m, float v) {
	m->thrustYaw = fmaxf(-1.0f, fminf(1.0f, v));
}
static inline void missileSetThrustYawPct(Missile *m, float pct) {
	missileSetThrustYaw(m, pct * 0.02f - 1.0f);
}
static inline void missileSetThrustYaw01(Missile *m, float v) {
	missileSetThrustYaw(m, v * 2.0f - 1.0f);
}
static inline float missileGetThrustYawPct(const Missile *m) {
	return (m->thrustYaw + 1.0f) * 50.0f;
}
static inline float missileGetThrustYaw01(const Missile *m) {
	return (m->thrustYaw + 1.0f) * 0.5f;
}
static inline float missileGetThrustYawNorm(const Missile *m) {
	return m->thrustYaw;
}

// Applies the missile controller output to the six deflections.
void missileFlightControl(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime);
