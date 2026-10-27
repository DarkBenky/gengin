#pragma once

#define MISSILE_BIN_MAGIC "MSSL"
#define MISSILE_BIN_VERSION 1

typedef struct {
	float thrust;	// N
	float burnTime; // s
} MissileStageSpec;

// Mirrors simulation/missiles.go; keep the field order in sync.
typedef struct {
	char name[16];

	float length;	// m
	float diameter; // m
	float finSpanFront;
	float finSpanBack;
	float emptyMass;	  // kg
	float propellantMass; // kg
	float inertiaRoll;	  // kg*m^2
	float inertiaPitch;
	float inertiaYaw;

	float cnAlpha; // fin lift slope, per rad
	float cd0;
	float dampingRoll; // N*m per rad/s
	float dampingPitch;
	float dampingYaw;
	float stallAngle; // deg
	float maxG;

	float frontArea;	// m^2
	float frontMaxDeg;	// deg
	float frontRateDeg; // deg/s
	float backArea;		// m^2
	float backMaxDeg;
	float backRateDeg;
	float tvcMaxDeg; // deg
	float tvcRateDeg;

	int stageCount; // 1..3
	MissileStageSpec stages[3];

	float pnGain;
	float fuseRadius; // m
	float lifetime;	  // s
} MissileSpec;

// Returns 0 on success.
int loadMissileBin(MissileSpec *spec, const char *path);

int isMissileBin(const char *path);
