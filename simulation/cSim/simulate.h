#pragma once
#include "import.h"

float3 planeGetForwardVector(const Plane *plane);
float3 planeGetRightVector(const Plane *plane);
float3 planeGetUpVector(const Plane *plane);

// Setters accepting normalized -1 to 1 input for surfaces, 0 to 1 for throttle.
void planeSetThrottle(Plane *plane, float pct);
void planeSetAileron(Plane *plane, float angle);
void planeSetElevator(Plane *plane, float angle);
void planeSetRudder(Plane *plane, float angle);
void planeSetFlap(Plane *plane, float angle);
// Individual surface control — allows split left/right input for roll mixing.
// Flaps: right > left = right wing sinks (positive/right bank).
// Elevators: right > left = right tail sinks = right wing rises (negative/left bank).
// norm variants: -1 to 1; -1=full negative, 0=neutral, 1=full positive
void planeSetLeftFlap(Plane *plane, float norm);
void planeSetRightFlap(Plane *plane, float norm);
void planeSetLeftElevator(Plane *plane, float norm);
void planeSetRightElevator(Plane *plane, float norm);

// pct variants: 0-100%; 50=neutral, 0=full negative, 100=full positive
void planeSetLeftFlapPct(Plane *plane, float pct);
void planeSetRightFlapPct(Plane *plane, float pct);
void planeSetLeftElevatorPct(Plane *plane, float pct);
void planeSetRightElevatorPct(Plane *plane, float pct);

// 01 variants: 0-1; 0=full negative, 0.5=neutral, 1=full positive
void planeSetLeftFlap01(Plane *plane, float v);
void planeSetRightFlap01(Plane *plane, float v);
void planeSetLeftElevator01(Plane *plane, float v);
void planeSetRightElevator01(Plane *plane, float v);
// Returns 0-100%; 50 = neutral, 0 = full negative, 100 = full positive
float planeGetThrottlePct(const Plane *plane);
float planeGetAileronPct(const Plane *plane);
float planeGetElevatorPct(const Plane *plane);
float planeGetRudderPct(const Plane *plane);
float planeGetFlapPct(const Plane *plane);

// Returns 0-1; throttle: 0=off 1=full. surfaces: 0=full negative, 0.5=neutral, 1=full positive
float planeGetThrottle01(const Plane *plane);
float planeGetAileron01(const Plane *plane);
float planeGetElevator01(const Plane *plane);
float planeGetRudder01(const Plane *plane);
float planeGetFlap01(const Plane *plane);

// Returns -1 to 1; -1=full negative, 0=neutral, 1=full positive
float planeGetAileronNorm(const Plane *plane);
float planeGetElevatorNorm(const Plane *plane);
float planeGetRudderNorm(const Plane *plane);
float planeGetFlapNorm(const Plane *plane);

// Left-side getters for split-surface readback
// Pct: 0-100%; 50=neutral. 01: 0-1; 0.5=neutral. Norm: -1 to 1; 0=neutral.
float planeGetLeftFlapPct(const Plane *plane);
float planeGetLeftFlap01(const Plane *plane);
float planeGetLeftFlapNorm(const Plane *plane);
float planeGetLeftElevatorPct(const Plane *plane);
float planeGetLeftElevator01(const Plane *plane);
float planeGetLeftElevatorNorm(const Plane *plane);

// Setters accepting 0-100%; 50 = neutral
void planeSetThrottlePct(Plane *plane, float pct);
void planeSetAileronPct(Plane *plane, float pct);
void planeSetElevatorPct(Plane *plane, float pct);
void planeSetRudderPct(Plane *plane, float pct);
void planeSetFlapPct(Plane *plane, float pct);

// Setters accepting 0-1; throttle: 0=off 1=full. surfaces: 0=full negative, 0.5=neutral, 1=full positive
void planeSetThrottle01(Plane *plane, float v);
void planeSetAileron01(Plane *plane, float v);
void planeSetElevator01(Plane *plane, float v);
void planeSetRudder01(Plane *plane, float v);
void planeSetFlap01(Plane *plane, float v);

void updatePlane(Plane *plane, float deltaTime, float3 *newForwardDirection);

// Compute Euler angles (Rx->Ry->Rz order) for a +X-nose 3D model
// from the plane's forward direction and bank angle.
float3 planeGetEulerAngles(const Plane *plane);