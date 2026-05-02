#pragma once
#include "import.h"

typedef struct {
	float3 Lift;
	float3 Drag;
	float3 CenterOfMass;
	float3 CenterOfLift;
	float3 CenterOfDrag;
} AerodynamicForces;

float calculateAirDensity(float altitude);
void calculateSurfaceForces(const Surface *surface, float altitude, float airspeed, AerodynamicForces *forces);

void planeSetThrottle(Plane *plane, float pct);
void planeSetAileron(Plane *plane, float angle);
void planeSetElevator(Plane *plane, float angle);
void planeSetRudder(Plane *plane, float angle);
void planeSetFlap(Plane *plane, float angle);

void updatePlane(Plane *plane, float deltaTime, float3 *newForwardDirection);#pragma once
#include "import.h"

typedef struct {
	float3 Lift;
	float3 Drag;
	float3 CenterOfMass;
	float3 CenterOfLift;
	float3 CenterOfDrag;
} AerodynamicForces;

float calculateAirDensity(float altitude);
void calculateSurfaceForces(const Surface *surface, float altitude, float airspeed, AerodynamicForces *forces);

void planeSetThrottle(Plane *plane, float pct);
void planeSetAileron(Plane *plane, float angle);
void planeSetElevator(Plane *plane, float angle);
void planeSetRudder(Plane *plane, float angle);
void planeSetFlap(Plane *plane, float angle);

void updatePlane(Plane *plane, float deltaTime, float3 *newForwardDirection);