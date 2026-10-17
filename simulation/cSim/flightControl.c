// TODO: hand made algo to find good surface control to target point in 3d space
// using iterative approaches where we try to minimize the loss based on this
// 1. try to apply roll so the top of the plane will be facing the target
// 2. apply this algo to pitch
// pitch = 0.5  // neutral
// for try in range(tries):
//     saveState()
//     apply(pitch)
//     for step in range(lookahead):
//         simStep()
//     nextLoss = distanceToTarget
//     restoreState()          // re-evaluate from same point each try

//     currentLoss = distanceToTarget
//     if nextLoss < currentLoss:
//         pitch *= 1.125
//     else:
//         pitch /= 1.125
// 3. try to same algo for yaw

#include "flightControl.h"
#include "missile.h"
#include <dirent.h>
#include <math.h>
#include <string.h>

// ---- definitions ----------------------------------------------------------

// Search step-size schedule for the momentum walk in getControllerOutputV5.
// The walk's total travel is learningRate / (1 - SEARCH_STEP_DECAY): at the old
// 0.95 that is lr0/(0.05) = 1.0 of the [0,1] box, i.e. the walk drifts a whole
// box-width in the smoothed gradient direction and saturates against the walls;
// measured on the pinned suite that overshoot costs control effort (the walk
// ends ~2x farther from neutral than the loss minimum) and slightly worse miss.
// 0.91 -> 0.56 of the box, closer to the loss minimum.
// SEARCH_MIN_TAIL_WALK stops the walk once the *remaining* travel budget
// learningRate/(1-SEARCH_STEP_DECAY) is below this value.  The pre-warm-start
// walk used 0.004 (53 iterations); with the walk starting at the plane's own
// surfaces the miss/cost trade is far flatter, so the threshold sits at its
// measured knee (replica sweep 0.06-0.20: miss +0.07..+0.63%, cost -56..-79%).
// The pinned suite at 0.06: miss 340.1 -> 341.4 m, effort 20.6 -> 17.9,
// controller cost 1872 -> 880 us/step.
// 0.06 -> 0.14 takes the walk from 24 to 15 iterations: pinned suite miss
// 335.15 -> 335.60 m, effort 13.63 -> 13.74, controller cost ~850 -> ~550
// us/step, and over six geometry sets (seeds 0/100/200/300/400/500, 120
// scenarios) the aggregate miss stays within +0.34%.  0.16 is where the miss
// starts to give way (+0.69%).  Disclosed: the drift tier spends more for the
// same miss with the shorter walk (effort 15.98 -> 21.41, 620 -> 1180
// saturated steps) while the other four tiers spend less, so the aggregate
// effort moves less than any single tier.
#define SEARCH_STEP_DECAY 0.91f
#define SEARCH_MIN_TAIL_WALK 0.14f

static const float3 PLANE_TARGET_OFFSET = {1000.0f, 1000.0f, 1000.0f, 0.0f};

// Missile sweep geometries.  Main is the long lead shot (at 1.7 km a Mach 2+
// missile overflies the target before its turn develops); beam is a close
// crossing that forces a hard pull.  A non-zero switchTime reverses the target
// lateral velocity mid-flight (jink).
typedef struct {
	const char *tag;
	float3 targetOffset;
	float3 targetVel;
	float3 targetVel2;
	float switchTime;
} MissileScenario;

static const MissileScenario MISSILE_SCENARIOS[] = {
	{"", {5000.0f, 1000.0f, 5000.0f, 0.0f}, {0.0f, 0.0f, -250.0f, 0.0f}},
	{" beam", {2500.0f, 1000.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -350.0f, 0.0f}},
	{" head-on", {6000.0f, 1000.0f, 0.0f, 0.0f}, {-250.0f, 0.0f, 0.0f, 0.0f}},
	{" tail", {4000.0f, 1000.0f, 0.0f, 0.0f}, {250.0f, 0.0f, 0.0f, 0.0f}},
	{" jink", {5000.0f, 1000.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -250.0f, 0.0f}, {0.0f, 0.0f, 250.0f, 0.0f}, 3.0f},
	{" long", {9000.0f, 1000.0f, 9000.0f, 0.0f}, {0.0f, 0.0f, -250.0f, 0.0f}},
	{" extreme", {14000.0f, 1000.0f, 14000.0f, 0.0f}, {0.0f, 0.0f, -250.0f, 0.0f}},
};

typedef struct {
	int iteration;
	float aileronValue;
	float aileronLoss;
	float elevatorValue;
	float elevatorLoss;
	float rudderValue;
	float rudderLoss;
	float distanceToTarget;
	float lossAngle;
	float3 planePosition;
	float3 targetPosition;
	float3 planeVelocity;
	float3 planeForward;
	float3 planeUp;
	float3 planeRight;
} PlaneControlLogs;

typedef struct {
	PlaneControlLogs *logs;
	int len;
	int cap;
} Logs;

// ---- helpers --------------------------------------------------------------

static inline float alignmentLoss(const Plane *plane, float3 target) {
	float3 planePosition = plane->position;
	float3 toTarget = Float3_Normalize(Float3_Sub(target, planePosition));
	float3 forward = planeGetForwardVector(plane);

	float alignment = Float3_Dot(forward, toTarget); // 1 = perfect, -1 = opposite
	return -alignment;								 // -1 = perfect alignment, 1 = opposite direction
}

static inline float alignmentLossVelocity(const Plane *plane, float3 target) {
	float3 planePosition = plane->position;
	float3 toTarget = Float3_Normalize(Float3_Sub(target, planePosition));
	float3 forward = Float3_Normalize(plane->velocity);

	float alignment = Float3_Dot(forward, toTarget); // 1 = perfect, -1 = opposite
	return -alignment;								 // -1 = perfect alignment, 1 = opposite direction
}

static inline float distanceToTarget(const Plane *plane, float3 target) {
	return Float3_Length(Float3_Sub(target, plane->position));
}

void addLog(Logs *logs, PlaneControlLogs log) {
	if (logs->len >= logs->cap) {
		int newCap = logs->cap == 0 ? 16 : logs->cap * 2;
		logs->logs = realloc(logs->logs, newCap * sizeof(PlaneControlLogs));
		logs->cap = newCap;
	}
	logs->logs[logs->len++] = log;
}

void freeLogs(Logs *logs) {
	free(logs->logs);
	logs->logs = NULL;
	logs->len = 0;
	logs->cap = 0;
}

static void writeLogRows(FILE *file, const char *label, const Logs *logs) {
	for (int i = 0; i < logs->len; i++) {
		const PlaneControlLogs *log = &logs->logs[i];
		fprintf(file, "%s,%d,%.3f,%.4f,%.3f,%.4f,%.3f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.7f\n",
				label, log->iteration, log->aileronValue, log->aileronLoss, log->elevatorValue, log->elevatorLoss, log->rudderValue, log->rudderLoss, log->distanceToTarget, log->planePosition.x, log->planePosition.y, log->planePosition.z, log->targetPosition.x, log->targetPosition.y, log->targetPosition.z, log->planeVelocity.x, log->planeVelocity.y, log->planeVelocity.z, log->planeForward.x, log->planeForward.y, log->planeForward.z, log->planeUp.x, log->planeUp.y, log->planeUp.z, log->planeRight.x, log->planeRight.y, log->planeRight.z, log->lossAngle);
	}
}


float rollLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	// Projection kept unnormalized: atan2 is scale-invariant, while
	// normalizing a near-degenerate projection amplified float noise.
	float3 idealUp = Float3_Sub(toTarget, Float3_Scale(forward, Float3_Dot(toTarget, forward)));

	float cross = Float3_Dot(Float3_Cross(up, idealUp), forward);
	float dot = Float3_Dot(up, idealUp);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect roll alignment
}

float pitchLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 right = planeGetRightVector(plane);

	float3 idealForward = Float3_Sub(toTarget, Float3_Scale(right, Float3_Dot(toTarget, right)));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), right);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect pitch alignment
}

float yawLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	float3 idealForward = Float3_Sub(toTarget, Float3_Scale(up, Float3_Dot(toTarget, up)));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), up);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect yaw alignment
}

// Deployed plane loss: 2.25x horizon in the same 16 steps (measured better
// than fine steps at equal cost) plus a control-effort term; the alignment
// gain and the effort term only work as a pair. The rudder is charged 2:1 -
// it is the yaw axis, so deflection buys no turn, only sideslip and drag.
static float evaluateLossV2PlusTuned2(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	const float simDeltaTime = deltaTime * 2.25f;

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float minDist = currentDist;
	float runningAlignment = 0.0f;
	float runningAlignVel = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, simDeltaTime, NULL);
		runningAlignment += alignmentLoss(&simPlane, target);
		runningAlignVel += alignmentLossVelocity(&simPlane, target);
		float d = distanceToTarget(&simPlane, target);
		if (d < minDist) minDist = d;
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignVel = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float distImprovement = (finalDist - currentDist) * 0.45f + (minDist - currentDist) * 1.10f;
	float overshootTerm = (finalDist - minDist) * 1.0f;

	float alignWeight = 1.0f + 1.5f / (1.0f + currentDist * 0.0065f);

	const float effortWeight = 1.0f;
	float effort = 3.0f * fabsf(values[0] - 0.5f) + 1.5f * fabsf(values[1] - 0.5f) + 1.5f * fabsf(values[2] - 0.5f);

	float loss = (finalAlignment + finalAlignVel) * alignWeight + 0.5f * ((runningAlignment / (float)ctrl->LookaheadSteps) * alignWeight + (runningAlignVel / (float)ctrl->LookaheadSteps) * alignWeight) + distImprovement + overshootTerm + effortWeight * effort;

	return loss;
}

// TODO: test idea to change look ahead based on change of angle of loss
// TODO: test idea to use multiple look ahead periods for example 16 with 60 FPS, 8 with 30 FPS, 4 with 15 FPS, and 2 with 7.5 FPS, and then combine the losses from each of these look ahead periods to get a more robust loss evaluation

static ControllerOutput getControllerOutputV5(const Controller *ctrl, float3 target, float deltaTime, float *momentum, float *prevLoss, int maxIterations) {
	ControllerOutput output = {0};

	// Start from the surfaces the plane actually has; a neutral start re-plans
	// the whole approach from scratch on every frame.
	float values[3] = {planeGetRudder01(&ctrl->plane), planeGetElevator01(&ctrl->plane), planeGetAileron01(&ctrl->plane)}; // yaw, pitch, roll
	float momentumCoefficient = 0.9f;	  // how much of the previous momentum to keep
	float learningRate = 0.05f;
	// Finite-difference probe span. At 0.025 the two probes differ by less than
	// the loss's step-to-step noise, so the gradient direction is noise-driven;
	// 0.05 (the flat 0.04-0.075 region) steers the same miss with ~22% less
	// actuator travel measured on the 20-scenario suite.
	float epsilon = 0.05f;

	float bestAxisLoss[3] = {FLT_MAX, FLT_MAX, FLT_MAX}; // best loss for yaw, pitch, roll

	// The iterate below is a momentum walk whose learning rate decays by 0.95
	// per step, so the value it stops on is not necessarily the best control it
	// saw - it can be a point the walk was merely passing through, and the
	// momentum handoff between frames makes it overshoot the loss minimum.
	// Command the best-scoring candidate the search already evaluated instead:
	// each iteration scores six single-axis perturbations anyway, so tracking
	// them costs no extra loss evaluations, no extra state and no extra passes
	// over the loss.  Measured on the 20-scenario suite (pinned baseline,
	// flightBench), aggregate miss 342.26 -> 340.46 m, integrated control
	// effort 20.49 -> 15.13 (-26.2%) and saturation steps 2365 -> 1625, with
	// every tier within 0.4% of baseline except jink (-2.1%, 566.4 -> 554.6 m).
	// Each candidate is clamped into [0,1] on the axis it perturbs, so the
	// commanded vector is always a legal surface setting.  Neutral is scored
	// once up front so it stays a candidate: a flat or tie-heavy loss then
	// returns the un-deflected command rather than a probe picked by loop
	// order (one shared loss evaluation per controller call, ~0.1% of cost).
	float bestProbe[3] = {0.5f, 0.5f, 0.5f};
	float bestProbeLoss = evaluateLossV2PlusTuned2(ctrl, bestProbe, target, deltaTime);

	for (int iter = 0; iter < maxIterations; iter++) {
		// Compute gradient for ALL axes simultaneously
		float gradient[3] = {0};

		for (int axis = 0; axis < 3; axis++) {
			// Perturb positively
			float perturbedPositive[3] = {values[0], values[1], values[2]};
			perturbedPositive[axis] = fminf(1.0f, perturbedPositive[axis] + epsilon);

			// Perturb negatively
			float perturbedNegative[3] = {values[0], values[1], values[2]};
			perturbedNegative[axis] = fmaxf(0.0f, perturbedNegative[axis] - epsilon);

			float lossPos = evaluateLossV2PlusTuned2(ctrl, perturbedPositive, target, deltaTime);
			float lossNeg = evaluateLossV2PlusTuned2(ctrl, perturbedNegative, target, deltaTime);

			if (lossPos < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossPos;
			}
			if (lossNeg < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossNeg;
			}

			if (lossPos < bestProbeLoss) {
				bestProbeLoss = lossPos;
				bestProbe[0] = perturbedPositive[0];
				bestProbe[1] = perturbedPositive[1];
				bestProbe[2] = perturbedPositive[2];
			}
			if (lossNeg < bestProbeLoss) {
				bestProbeLoss = lossNeg;
				bestProbe[0] = perturbedNegative[0];
				bestProbe[1] = perturbedNegative[1];
				bestProbe[2] = perturbedNegative[2];
			}

			// Central difference gradient, normalized by the actual probe span:
			// near 0/1 a perturbation is clamped, so 2*epsilon would overstate
			// the slope and the walk kept sticking to the control walls.
			float span = perturbedPositive[axis] - perturbedNegative[axis];
			gradient[axis] = (span > 1e-6f) ? (lossPos - lossNeg) / span : 0.0f;
		}

		// Normalize gradient to prevent explosion
		float gradMag = sqrtf(gradient[0] * gradient[0] +
							  gradient[1] * gradient[1] +
							  gradient[2] * gradient[2]);
		if (gradMag > 1e-6f) {
			gradient[0] /= gradMag;
			gradient[1] /= gradMag;
			gradient[2] /= gradMag;
		}

		// Update ALL values simultaneously (this couples them)
		for (int axis = 0; axis < 3; axis++) {
			momentum[axis] = momentumCoefficient * momentum[axis] - learningRate * gradient[axis];
			values[axis] += momentum[axis];
			if (values[axis] <= 0.0f || values[axis] >= 1.0f) {
				momentum[axis] = 0.0f; // reset momentum at boundary
			}
			values[axis] = fmaxf(0.0f, fminf(1.0f, values[axis]));
		}

		// Adaptive learning rate decay, plus the convergence exit documented
		// above.
		learningRate *= SEARCH_STEP_DECAY;
		if (learningRate < SEARCH_MIN_TAIL_WALK * (1.0f - SEARCH_STEP_DECAY)) break;
	}

	// Command the best control the search actually scored, not the iterate the
	// momentum walk happened to stop on.
	output.Rudder = bestProbe[0];
	output.Elevator = bestProbe[1];
	output.Aileron = bestProbe[2];

	output.RudderLoss = bestAxisLoss[0];
	output.ElevatorLoss = bestAxisLoss[1];
	output.AileronLoss = bestAxisLoss[2];

	float lossDiff = bestAxisLoss[0] - *prevLoss;
	float angleChange = atanf(lossDiff / (deltaTime));
	output.LossAngle = angleChange; // if we see this value above 0.0 something wrong is happening
	// TODO: implement handling of this situation
	*prevLoss = bestAxisLoss[0];

	return output;
}

// PN accel command mapped to fin deflections via the plant's per-deflection authority.
MissileControllerOutput getControllerOutputMissile(const Missile *missile, float3 targetPosition, float3 targetVelocity, float deltaTime) {
	float3 accelCmd = missileGuidanceAccel(missile->position, missile->velocity, targetPosition, targetVelocity, missile->spec.pnGain);

	float3 fwd = Float3_Normalize(missile->velocity);
	float3 rightRaw = Float3_Cross(fwd, (float3){0.0f, 1.0f, 0.0f, 0.0f});
	float3 right = (Float3_Length(rightRaw) < 1e-4f) ? (float3){1.0f, 0.0f, 0.0f, 0.0f} : Float3_Normalize(rightRaw);
	float3 up = Float3_Normalize(Float3_Cross(right, fwd));

	float authority = missileFinAccelPerDeflection(missile);
	float pitch = 0.0f;
	float yaw = 0.0f;
	if (authority > 1e-3f) {
		pitch = fminf(1.0f, fmaxf(-1.0f, Float3_Dot(accelCmd, up) / authority));
		yaw = fminf(1.0f, fmaxf(-1.0f, Float3_Dot(accelCmd, right) / authority));
	}

	MissileControllerOutput output = {
		.FrontPitch = 0.5f + 0.5f * pitch,
		.FrontYaw = 0.5f + 0.5f * yaw,
		.BackPitch = 0.5f + 0.5f * pitch,
		.BackYaw = 0.5f + 0.5f * yaw,
		.ThrustPitch = 0.5f,
		.ThrustYaw = 0.5f,
	};
	return output;
}

// Proportional-navigation guidance for the point-mass missile model.
float3 missileGuidanceAccel(float3 position, float3 velocity, float3 targetPosition, float3 targetVelocity, float pnGain) {
	float3 rel = Float3_Sub(targetPosition, position);
	float range = Float3_Length(rel);
	float speed = Float3_Length(velocity);
	if (range < 1e-3f || speed < 1e-3f) {
		return (float3){0.0f, 0.0f, 0.0f, 0.0f};
	}
	float3 los = Float3_Scale(rel, 1.0f / range);
	float3 relVel = Float3_Sub(targetVelocity, velocity);
	// Closing speed is floored so guidance stays alive in opening geometry.
	float closing = fmaxf(-Float3_Dot(relVel, los), 0.25f * speed);
	float3 losRate = Float3_Scale(Float3_Cross(rel, relVel), 1.0f / (range * range));
	float3 cmd = Float3_Scale(Float3_Cross(losRate, Float3_Scale(velocity, 1.0f / speed)), pnGain * closing);
	return Float3_Sub(cmd, Float3_Scale(velocity, Float3_Dot(cmd, velocity) / (speed * speed)));
}

#ifndef FLIGHT_BENCH
// One plane: control, physics and log for simSteps.
static void runPlaneModel(const char *name, int simSteps, float deltaTime, FILE *file) {
	char modelPath[256];
	snprintf(modelPath, sizeof(modelPath), "simulation/simModels/%s.bin", name);

	Plane plane;
	if (loadPlaneBin(&plane, modelPath, (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 180.0f, 1.0f) != 0) {
		printf("Failed to load model: %s\n", modelPath);
		return;
	}
	Controller ctrl;
	initController(&ctrl, &plane);
	float3 target = Float3_Add(plane.position, PLANE_TARGET_OFFSET);

	Logs logs = {0};
	float momentum[3] = {0.0f, 0.0f, 0.0f};
	float prevLoss = 0.0f;
	float minDistOverall = FLT_MAX;

	for (int iter = 0; iter < simSteps; iter++) {
		ControllerOutput out = getControllerOutputV5(&ctrl, target, deltaTime, momentum, &prevLoss, 128);

		planeSetAileron01(&ctrl.plane, out.Aileron);
		planeSetElevator01(&ctrl.plane, out.Elevator);
		planeSetRudder01(&ctrl.plane, out.Rudder);
		planeSetThrottle01(&ctrl.plane, 1.0f);

		updatePlane(&ctrl.plane, deltaTime, NULL);

		float dist = distanceToTarget(&ctrl.plane, target);
		if (dist < minDistOverall) minDistOverall = dist;

		float3 planeForward = planeGetForwardVector(&ctrl.plane);
		float3 planeUp = planeGetUpVector(&ctrl.plane);
		float3 planeRight = planeGetRightVector(&ctrl.plane);

		PlaneControlLogs log = {
			.iteration = iter,
			.aileronValue = out.Aileron,
			.aileronLoss = out.AileronLoss,
			.elevatorValue = out.Elevator,
			.elevatorLoss = out.ElevatorLoss,
			.rudderValue = out.Rudder,
			.rudderLoss = out.RudderLoss,
			.distanceToTarget = dist,
			.lossAngle = out.LossAngle,
			.planePosition = ctrl.plane.position,
			.targetPosition = target,
			.planeVelocity = ctrl.plane.velocity,
			.planeForward = planeForward,
			.planeUp = planeUp,
			.planeRight = planeRight,
		};
		addLog(&logs, log);
	}

	writeLogRows(file, name, &logs);
	printf("%s: minDist=%.2f finalDist=%.2f\n", name, minDistOverall, distanceToTarget(&ctrl.plane, target));
	freeLogs(&logs);
}

// One missile: control, physics and log for simSteps.
// Elevator = front pitch, Rudder = front yaw, Aileron = back pitch; LossAngle = phase.
static void runMissileModel(const char *name, const MissileScenario *sc, int simSteps, float deltaTime, FILE *file) {
	char modelPath[256];
	snprintf(modelPath, sizeof(modelPath), "simulation/simModels/%s.bin", name);
	char key[96];
	snprintf(key, sizeof(key), "%s%s", name, sc->tag);

	float3 spawn = {0.0f, 1000.0f, 0.0f, 0.0f};
	float3 velocity = {180.0f, 0.0f, 0.0f, 0.0f};
	float3 targetVel = sc->targetVel;

	Missile missile;
	if (missileLoadBin(&missile, modelPath, spawn, velocity) != 0) {
		printf("Failed to load missile: %s\n", modelPath);
		return;
	}
	float3 target = Float3_Add(spawn, sc->targetOffset);

	Logs logs = {0};
	float minDist = FLT_MAX;
	int iter = 0;
	for (; iter < simSteps; iter++) {
		float t = (float)iter * deltaTime;
		if (sc->switchTime > 0.0f && t >= sc->switchTime) targetVel = sc->targetVel2;
		MissileControllerOutput out = getControllerOutputMissile(&missile, target, targetVel, deltaTime);
		missileSetFrontPitch01(&missile, out.FrontPitch);
		missileSetFrontYaw01(&missile, out.FrontYaw);
		missileSetBackPitch01(&missile, out.BackPitch);
		missileSetBackYaw01(&missile, out.BackYaw);
		missileSetThrustPitch01(&missile, out.ThrustPitch);
		missileSetThrustYaw01(&missile, out.ThrustYaw);
		if (!missileStep(&missile, target, targetVel, deltaTime)) break;

		target = Float3_Add(target, Float3_Scale(targetVel, deltaTime));

		float dist = Float3_Length(Float3_Sub(target, missile.position));
		if (dist < minDist) minDist = dist;

		float3 rightRaw = Float3_Cross(missile.forward, (float3){0.0f, 1.0f, 0.0f, 0.0f});
		float3 right = (Float3_Length(rightRaw) < 1e-4f) ? (float3){1.0f, 0.0f, 0.0f, 0.0f} : Float3_Normalize(rightRaw);
		float3 up = Float3_Normalize(Float3_Cross(right, missile.forward));

		PlaneControlLogs log = {
			.iteration = iter,
			.aileronValue = missileGetBackPitch01(&missile),
			.aileronLoss = missileGetBackPitchNorm(&missile),
			.elevatorValue = missileGetFrontPitch01(&missile),
			.elevatorLoss = missileGetFrontPitchNorm(&missile),
			.rudderValue = missileGetFrontYaw01(&missile),
			.rudderLoss = missileGetFrontYawNorm(&missile),
			.distanceToTarget = dist,
			.lossAngle = (float)missile.phase,
			.planePosition = missile.position,
			.targetPosition = target,
			.planeVelocity = missile.velocity,
			.planeForward = missile.forward,
			.planeUp = up,
			.planeRight = right,
		};
		addLog(&logs, log);
	}

	writeLogRows(file, key, &logs);
	printf("%s: minDist=%.2f finalDist=%.2f hit=%d\n", key, minDist, Float3_Length(Float3_Sub(target, missile.position)), missile.hit);
	freeLogs(&logs);
}

static int nameCompare(const void *a, const void *b) {
	return strcmp((const char *)a, (const char *)b);
}

// Collect the .bin names of one kind (planes or missiles) from simModels.
static int listModels(int missiles, char names[][64], int maxNames) {
	DIR *dir = opendir("simulation/simModels");
	if (dir == NULL) {
		printf("Cannot open simulation/simModels (run from the repo root)\n");
		return 0;
	}
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && count < maxNames) {
		size_t len = strlen(entry->d_name);
		if (len < 4 || strcmp(entry->d_name + len - 4, ".bin") != 0) continue;
		entry->d_name[len - 4] = '\0';
		char modelPath[256];
		snprintf(modelPath, sizeof(modelPath), "simulation/simModels/%s.bin", entry->d_name);
		if (isMissileBin(modelPath) != missiles) continue;
		snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
		count++;
	}
	closedir(dir);
	qsort(names, count, sizeof(names[0]), nameCompare);
	return count;
}

// main testing loop to debug controller
int main() {
	const int simSteps = 2000;
	const float deltaTime = 1.0f / 60.0f;

	FILE *file = fopen("simulation/cSim/flightControlLogs.csv", "w");
	if (file == NULL) {
		printf("Failed to open simulation/cSim/flightControlLogs.csv\n");
		return 1;
	}
	fprintf(file, "Model,Iteration,Aileron,AileronLoss,Elevator,ElevatorLoss,Rudder,RudderLoss,DistanceToTarget,PlanePositionX,PlanePositionY,PlanePositionZ,TargetPositionX,TargetPositionY,TargetPositionZ,PlaneVelocityX,PlaneVelocityY,PlaneVelocityZ,PlaneForwardX,PlaneForwardY,PlaneForwardZ,PlaneUpX,PlaneUpY,PlaneUpZ,PlaneRightX,PlaneRightY,PlaneRightZ,LossAngleChange\n");

	char planes[64][64];
	int planeCount = listModels(0, planes, 64);
	char missiles[64][64];
	int missileCount = listModels(1, missiles, 64);

	printf("=== plane sweep ===\n");
	for (int i = 0; i < planeCount; i++) {
		runPlaneModel(planes[i], simSteps, deltaTime, file);
	}

	printf("=== missile sweep ===\n");
	for (int i = 0; i < missileCount; i++) {
		for (int s = 0; s < (int)(sizeof(MISSILE_SCENARIOS) / sizeof(MISSILE_SCENARIOS[0])); s++) {
			runMissileModel(missiles[i], &MISSILE_SCENARIOS[s], simSteps, deltaTime, file);
		}
	}

	fclose(file);
	return 0;
}
#endif /* FLIGHT_BENCH */
