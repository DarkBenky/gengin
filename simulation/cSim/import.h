#pragma once
#include "../../object/format.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	float3 relativePos;
	float3 rotationAxis;
	float rotationAngle;
	float rotationRate;		// degrees per second
	float maxRotationAngle; // maximum angle the surface can rotate to
	float minRotationAngle; // minimum angle the surface can rotate to
	float surfaceArea;		// in square meters
	float liftCoefficient;
	float dragCoefficient;
	float aspectRatio;
	float efficiency;
	float stallAngle;
	float camber;
	bool active;
	float targetRotationAngle;
} Surface;

typedef struct {
	char name[32];
	float3 position;
	float3 forward; // along this vector is the plane's nose pointing and trust is applied
	float3 rotation;
	Surface leftWing;
	Surface rightWing;
	Surface verticalStabilizer;
	Surface horizontalStabilizer;
	Surface leftAileron;
	Surface rightAileron;
	Surface rudder;
	Surface leftFlap;
	Surface rightFlap;
	Surface leftElevator;
	Surface rightElevator;
	float maxTrust;				  // in Newtons in max afterburner
	float currentTrustPercentage; // 0.0 to 1.0
	float baseMass;				  // in kg without fuel
	float fuelMass;				  // in kg
	float currentFuelPercentage;  // 0.0 to 1.0
	float burnRate;				  // in kg/s at max afterburner
	float burnWithoutAfterburner; // in kg/s at max without afterburner

	// added state fields for simulation
	float currentSpeed;	   // in m/s
	float currentAltitude; // in meters
	float bankAngle;	   // radians, positive = right wing down
	float bankRate;		   // radians/second
	float3 velocity;	   // world-space velocity vector; magnitude == currentSpeed
	float pitchRate;	   // rad/s, positive = nose up
	float yawRate;		   // rad/s, positive = nose right

	// Carried unbanked body frame.  Integrated by updatePlane with the nose
	// rotations instead of being re-derived from world up: a gravity-referenced
	// frame has no continuous choice near the vertical, which used to rotate
	// the whole frame 180 deg in one step at |fwd.y| = 0.99 and broke loops.
	float3 bodyRight;	   // unit; bank is applied on top of this frame
	float3 bodyUp;		   // = cross(forward, bodyRight)
} Plane;

int loadPlaneBin(Plane *plane, const char *path, float3 forward, float3 position, float speed, float throttle);
int savePlaneBin(const Plane *plane, const char *path);