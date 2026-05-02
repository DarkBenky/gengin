#pragma once
#include "import.h"

// Setters accepting normalized -1 to 1 input for surfaces, 0 to 1 for throttle.
void planeSetThrottle(Plane *plane, float pct);
void planeSetAileron(Plane *plane, float angle);
void planeSetElevator(Plane *plane, float angle);
void planeSetRudder(Plane *plane, float angle);
void planeSetFlap(Plane *plane, float angle);

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