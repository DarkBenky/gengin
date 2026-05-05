#include "pid.h"
#include <math.h>

// Control authority tuning constants.
#define MAX_MANUAL_ROLL_RATE     1.0f   // rad/s commanded when A/D held
#define MAX_PITCH_RATE_INPUT     0.4f   // rad/s commanded when W/S held
#define RUDDER_BIAS              0.3f   // rudder norm [-1,1] added when Q/E held
#define MAX_WINGS_LEVEL_RATE     1.5f   // rad/s cap on auto-leveling roll rate
#define MIN_COORDINATED_SPEED    50.0f  // m/s floor to avoid division issues at low speed
#define MAX_COORDINATED_BANK     1.2f   // rad (~69 deg) clamp for turn coordination

static float Pid_Step(Pid *pid, float error, float dt) {
	pid->integral = fmaxf(-pid->integralLimit, fminf(pid->integralLimit, pid->integral + error * dt));
	float derivative = (error - pid->prevError) / (dt > 1e-6f ? dt : 1e-6f);
	pid->prevError = error;
	return pid->kp * error + pid->ki * pid->integral + pid->kd * derivative;
}

static void Pid_Init(Pid *pid, float kp, float ki, float kd, float integralLimit) {
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
	Pid_Init(&ctrl->rollAngle, 2.0f, 0.3f, 0.0f, 1.0f);

	// Inner roll loop: roll rate error → aileron [-1, 1].
	Pid_Init(&ctrl->rollRate, 1.2f, 0.1f, 0.0f, 0.5f);

	// Pitch rate controller: rate error → elevator [-1, 1].
	Pid_Init(&ctrl->pitchRate, 2.5f, 0.3f, 0.0f, 0.5f);

	// Yaw rate controller: coordinated-turn error + manual bias → rudder [-1, 1].
	Pid_Init(&ctrl->yawRate, 1.0f, 0.1f, 0.0f, 0.3f);
}

void PlaneController_Update(PlaneController *ctrl, Plane *plane, const Input *input, float dt) {
	// --- user intent ---
	float userRollRate  = 0.0f;
	float userPitchRate = 0.0f;
	float rudderBias    = 0.0f;

	if (input->keys[KB_KEY_A])      userRollRate  = -MAX_MANUAL_ROLL_RATE;
	else if (input->keys[KB_KEY_D]) userRollRate  =  MAX_MANUAL_ROLL_RATE;

	if (input->keys[KB_KEY_W])      userPitchRate =  MAX_PITCH_RATE_INPUT;
	else if (input->keys[KB_KEY_S]) userPitchRate = -MAX_PITCH_RATE_INPUT;

	if (input->keys[KB_KEY_Q])      rudderBias = -RUDDER_BIAS;
	else if (input->keys[KB_KEY_E]) rudderBias =  RUDDER_BIAS;

	// --- roll: cascade ---
	// When rolling manually the outer loop is bypassed to avoid fighting the pilot.
	float desiredRollRate;
	if (input->keys[KB_KEY_A] || input->keys[KB_KEY_D]) {
		desiredRollRate = userRollRate;
		ctrl->rollAngle.integral = 0.0f; // prevent integrator windup during manual roll
	} else {
		// Outer loop drives bank angle to zero (wings-level).
		desiredRollRate = Pid_Step(&ctrl->rollAngle, -plane->bankAngle, dt);
		desiredRollRate = fmaxf(-MAX_WINGS_LEVEL_RATE, fminf(MAX_WINGS_LEVEL_RATE, desiredRollRate));
	}

	float aileron = Pid_Step(&ctrl->rollRate, desiredRollRate - plane->bankRate, dt);
	planeSetAileron(plane, fmaxf(-1.0f, fminf(1.0f, aileron)));

	// --- pitch: direct rate control ---
	float elevator = Pid_Step(&ctrl->pitchRate, userPitchRate - plane->pitchRate, dt);
	planeSetElevator(plane, fmaxf(-1.0f, fminf(1.0f, elevator)));

	// --- yaw: coordinated turn + optional manual bias ---
	// In a balanced turn the required yaw rate equals (g/V)*tan(bank).
	float speed = fmaxf(plane->currentSpeed, MIN_COORDINATED_SPEED);
	float clampedBank = fmaxf(-MAX_COORDINATED_BANK, fminf(MAX_COORDINATED_BANK, plane->bankAngle));
	float coordYawRate = (9.81f / speed) * tanf(clampedBank);
	float rudder = Pid_Step(&ctrl->yawRate, coordYawRate - plane->yawRate, dt) + rudderBias;
	planeSetRudder(plane, fmaxf(-1.0f, fminf(1.0f, rudder)));
}

