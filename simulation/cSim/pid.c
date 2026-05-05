#include "pid.h"
#include <math.h>

static float Pid_Step(Pid *pid, float error, float dt) {
	pid->integral = fmaxf(-pid->integralLimit, fminf(pid->integralLimit, pid->integral + error * dt));
	float derivative = (error - pid->prevError) / (dt > 1e-6f ? dt : 1e-6f);
	pid->prevError = error;
	return pid->kp * error + pid->ki * pid->integral + pid->kd * derivative;
}

static void pidInit(Pid *pid, float kp, float ki, float kd, float integralLimit) {
	pid->kp = kp;
	pid->ki = ki;
	pid->kd = kd;
	pid->integral = 0.0f;
	pid->prevError = 0.0f;
	pid->integralLimit = integralLimit;
}

void PlaneController_Init(PlaneController *ctrl) {
	// Outer roll loop: bank angle (rad) → desired roll rate (rad/s).
	// kp=2 means a 45-deg bank commands ~1.6 rad/s roll rate to level out.
	pidInit(&ctrl->rollAngle, 2.0f, 0.3f, 0.0f, 1.0f);

	// Inner roll loop: roll rate error → aileron [-1, 1].
	pidInit(&ctrl->rollRate, 1.2f, 0.1f, 0.0f, 0.5f);

	// Pitch rate controller: rate error → elevator [-1, 1].
	pidInit(&ctrl->pitchRate, 2.5f, 0.3f, 0.0f, 0.5f);

	// Yaw rate controller: coordinated-turn error + manual bias → rudder [-1, 1].
	pidInit(&ctrl->yawRate, 1.0f, 0.1f, 0.0f, 0.3f);
}

void PlaneController_Update(PlaneController *ctrl, Plane *plane, const Input *input, float dt) {
	// --- user intent ---
	float userRollRate  = 0.0f;
	float userPitchRate = 0.0f;
	float rudderBias    = 0.0f;

	if (input->keys[KB_KEY_A])      userRollRate  = -1.0f;  // roll left (rad/s)
	else if (input->keys[KB_KEY_D]) userRollRate  =  1.0f;  // roll right

	if (input->keys[KB_KEY_W])      userPitchRate =  0.4f;  // nose up (rad/s)
	else if (input->keys[KB_KEY_S]) userPitchRate = -0.4f;  // nose down

	if (input->keys[KB_KEY_Q])      rudderBias = -0.3f;     // yaw left
	else if (input->keys[KB_KEY_E]) rudderBias =  0.3f;     // yaw right

	// --- roll: cascade ---
	// When rolling manually the outer loop is bypassed to avoid fighting the pilot.
	float desiredRollRate;
	if (input->keys[KB_KEY_A] || input->keys[KB_KEY_D]) {
		desiredRollRate = userRollRate;
		ctrl->rollAngle.integral = 0.0f; // prevent integrator windup during manual roll
	} else {
		// Outer loop drives bank angle to zero (wings-level).
		desiredRollRate = Pid_Step(&ctrl->rollAngle, -plane->bankAngle, dt);
		desiredRollRate = fmaxf(-1.5f, fminf(1.5f, desiredRollRate));
	}

	float aileron = Pid_Step(&ctrl->rollRate, desiredRollRate - plane->bankRate, dt);
	planeSetAileron(plane, fmaxf(-1.0f, fminf(1.0f, aileron)));

	// --- pitch: direct rate control ---
	float elevator = Pid_Step(&ctrl->pitchRate, userPitchRate - plane->pitchRate, dt);
	planeSetElevator(plane, fmaxf(-1.0f, fminf(1.0f, elevator)));

	// --- yaw: coordinated turn + optional manual bias ---
	// In a balanced turn the required yaw rate equals (g/V)*tan(bank).
	float speed = fmaxf(plane->currentSpeed, 50.0f);
	float clampedBank = fmaxf(-1.2f, fminf(1.2f, plane->bankAngle));
	float coordYawRate = (9.81f / speed) * tanf(clampedBank);
	float rudder = Pid_Step(&ctrl->yawRate, coordYawRate - plane->yawRate, dt) + rudderBias;
	planeSetRudder(plane, fmaxf(-1.0f, fminf(1.0f, rudder)));
}
