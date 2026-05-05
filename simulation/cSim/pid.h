#pragma once
#include "simulate.h"
#include "../../keyboar/keyboar.h"

// Single-axis PID state.
typedef struct {
	float kp, ki, kd;
	float integral;
	float prevError;
	float integralLimit;
} Pid;

// Cascade flight controller for one plane instance.
// Roll uses an outer (bank angle → desired roll rate) + inner (roll rate → aileron) loop.
// Pitch is a direct rate controller. Yaw auto-coordinates turns with optional manual bias.
typedef struct {
	Pid rollAngle; // outer: bank angle (rad) → desired roll rate (rad/s)
	Pid rollRate;  // inner: roll rate error → aileron [-1, 1]
	Pid pitchRate; // pitch rate error → elevator [-1, 1]
	Pid yawRate;   // coordinated yaw rate error → rudder [-1, 1]
} PlaneController;

void PlaneController_Init(PlaneController *ctrl);

// Polls input, runs PID loops, and writes control surface commands to the plane.
// Call once per frame before updatePlane(); dt must match the simulation step.
void PlaneController_Update(PlaneController *ctrl, Plane *plane, const Input *input, float dt);
