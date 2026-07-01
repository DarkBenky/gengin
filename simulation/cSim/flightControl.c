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
#include <math.h>
#include <string.h>

#define memcopy(dst, src) memcpy((dst), (src), sizeof(*(dst)))
#define MAX_FLOAT 3.402823466e+38F

float rollLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	float3 idealUp = Float3_Normalize(Float3_Sub(toTarget, Float3_Scale(forward, Float3_Dot(toTarget, forward))));

	float cross = Float3_Dot(Float3_Cross(up, idealUp), forward);
	float dot = Float3_Dot(up, idealUp);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect roll alignment
}

float pitchLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 right = planeGetRightVector(plane);

	float3 idealForward = Float3_Normalize(Float3_Sub(toTarget, Float3_Scale(right, Float3_Dot(toTarget, right))));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), right);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect pitch alignment
}

float yawLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	float3 idealForward = Float3_Normalize(Float3_Sub(toTarget, Float3_Scale(up, Float3_Dot(toTarget, up))));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), up);
	float dot = Float3_Dot(forward, idealForward);

	return fabsf(atan2f(cross, dot)); // radians, 0 = perfect yaw alignment
}

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

// TODO: implement this
static ControllerOutput getControllerOutput(const Controller *ctrl, float3 target, float deltaTime) {
	ControllerOutput output = {0};
	const float distanceLossWeight = 0.5f;

	float aileronStart = 0.5f; // neutral
	float aileronStep = 0.15f;
	bool aileronSide = true; // true = increase from neutral, false = decrease from neutral

	Plane baselinePlane = ctrl->plane;

	// get baseline loss at neutral aileron
	planeSetAileron01(&baselinePlane, aileronStart);
	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&baselinePlane, deltaTime, NULL);
	}
	float aileronBaselineLoss = rollLoss(&baselinePlane, target) + distanceToTarget(&baselinePlane, target) * distanceLossWeight;

	float bestAileronValue = aileronStart;
	float bestLoss = aileronBaselineLoss;

	for (int iter = 0; iter < ctrl->MaxIterationPerAxis; iter++) {
		Plane simulationPlane = ctrl->plane;
		float aileronValue = aileronStart;
		if (aileronSide) {
			aileronValue += aileronStep;
		} else {
			aileronValue -= aileronStep;
		}

		aileronValue = fmaxf(0.0f, fminf(1.0f, aileronValue)); // clamp to [0,1]

		planeSetAileron01(&simulationPlane, aileronValue);
		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlane, deltaTime, NULL);
		}
		float loss = rollLoss(&simulationPlane, target);
		float distLoss = distanceToTarget(&simulationPlane, target);
		loss += distLoss * distanceLossWeight;

		if (loss < bestLoss) {
			bestLoss = loss;
			bestAileronValue = aileronValue;
			aileronStart = aileronValue; // recenter here
			aileronStep *= 1.125f;
		} else {
			aileronStep /= 1.125f;
			aileronSide = !aileronSide;
		}
	}

	// printf("Best aileron value: %.3f with roll loss: %.4f\n", bestAileronValue, bestLoss);

	output.Aileron = bestAileronValue;
	output.AileronLoss = bestLoss;

	float pitchStart = 0.5f; // neutral
	float pitchStep = 0.15f;
	bool pitchSide = true; // true = increase from neutral, false = decrease from neutral

	Plane pitchBaselinePlane = ctrl->plane;

	// get baseline loss at neutral pitch
	planeSetElevator01(&pitchBaselinePlane, pitchStart);
	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&pitchBaselinePlane, deltaTime, NULL);
	}
	float pitchBaselineLoss = pitchLoss(&pitchBaselinePlane, target) + distanceToTarget(&pitchBaselinePlane, target) * distanceLossWeight;
	// printf("Baseline pitch loss at neutral elevator: %.4f\n", pitchBaselineLoss);

	float bestPitchValue = pitchStart;
	bestLoss = pitchBaselineLoss;

	for (int iter = 0; iter < ctrl->MaxIterationPerAxis; iter++) {
		Plane simulationPlane = ctrl->plane;
		float pitchValue = pitchStart;
		if (pitchSide) {
			pitchValue += pitchStep;
		} else {
			pitchValue -= pitchStep;
		}

		pitchValue = fmaxf(0.0f, fminf(1.0f, pitchValue)); // clamp to [0,1]

		planeSetElevator01(&simulationPlane, pitchValue);
		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlane, deltaTime, NULL);
		}
		float loss = pitchLoss(&simulationPlane, target);
		float distLoss = distanceToTarget(&simulationPlane, target);
		loss += distLoss * distanceLossWeight; // add a small penalty for distance to target
		// printf("Iter %d: Elevator %.3f, Loss %.4f\n", iter, pitchValue, loss);

		if (loss < bestLoss) {
			bestLoss = loss;
			bestPitchValue = pitchValue;
			pitchStart = pitchValue; // recenter here
			pitchStep *= 1.125f;
		} else {
			pitchStep /= 1.125f;
			pitchSide = !pitchSide;
		}
	}
	// printf("Best elevator value: %.3f with pitch loss: %.4f\n", bestPitchValue, bestLoss);

	output.Elevator = bestPitchValue;
	output.ElevatorLoss = bestLoss;

	float yawStart = 0.5f; // neutral
	float yawStep = 0.15f;
	bool yawSide = true; // true = increase from neutral, false = decrease from neutral

	Plane yawBaselinePlane = ctrl->plane;

	// get baseline loss at neutral yaw
	planeSetRudder01(&yawBaselinePlane, yawStart);
	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&yawBaselinePlane, deltaTime, NULL);
	}
	float yawBaselineLoss = yawLoss(&yawBaselinePlane, target) + distanceToTarget(&yawBaselinePlane, target) * distanceLossWeight;
	// printf("Baseline yaw loss at neutral rudder: %.4f\n", yawBaselineLoss);

	float bestYawValue = yawStart;
	bestLoss = yawBaselineLoss;

	for (int iter = 0; iter < ctrl->MaxIterationPerAxis; iter++) {
		Plane simulationPlane = ctrl->plane;
		float yawValue = yawStart;
		if (yawSide) {
			yawValue += yawStep;
		} else {
			yawValue -= yawStep;
		}

		yawValue = fmaxf(0.0f, fminf(1.0f, yawValue)); // clamp to [0,1]

		planeSetRudder01(&simulationPlane, yawValue);
		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlane, deltaTime, NULL);
		}
		float loss = yawLoss(&simulationPlane, target);
		float distLoss = distanceToTarget(&simulationPlane, target);
		loss += distLoss * distanceLossWeight; // add a small penalty for distance to target
		// printf("Iter %d: Rudder %.3f, Loss %.4f\n", iter, yawValue, loss);

		if (loss < bestLoss) {
			bestLoss = loss;
			bestYawValue = yawValue;
			yawStart = yawValue; // recenter here
			yawStep *= 1.125f;
		} else {
			yawStep /= 1.125f;
			yawSide = !yawSide;
		}
	}
	// printf("Best rudder value: %.3f with yaw loss: %.4f\n", bestYawValue, bestLoss);

	output.Rudder = bestYawValue;
	output.RudderLoss = bestLoss;

	return output;
}

static ControllerOutput getControllerOutputV3(const Controller *ctrl, float3 target, float deltaTime) {
	ControllerOutput output = {0};

	float yawStep = 0.35f;
	float pitchStep = 0.35f;
	float rollStep = 0.35f;

	float yawValue = 0.5f;
	float pitchValue = 0.5f;
	float rollValue = 0.5f;

	const int maxIterations = 128;
	float bestLossYaw = FLT_MAX;
	float bestLossPitch = FLT_MAX;
	float bestLossRoll = FLT_MAX;

	for (int iter = 0; iter < maxIterations * 3; iter++) {
		int action = iter % 3;

		Plane simulationPlanePositive = ctrl->plane;
		Plane simulationPlaneNegative = ctrl->plane;

		planeSetRudder01(&simulationPlanePositive, yawValue);
		planeSetElevator01(&simulationPlanePositive, pitchValue);
		planeSetAileron01(&simulationPlanePositive, rollValue);
		planeSetRudder01(&simulationPlaneNegative, yawValue);
		planeSetElevator01(&simulationPlaneNegative, pitchValue);
		planeSetAileron01(&simulationPlaneNegative, rollValue);

		if (action == 0) {
			planeSetRudder01(&simulationPlanePositive, fminf(1.0f, yawValue + yawStep));
			planeSetRudder01(&simulationPlaneNegative, fmaxf(0.0f, yawValue - yawStep));
		} else if (action == 1) {
			planeSetElevator01(&simulationPlanePositive, fminf(1.0f, pitchValue + pitchStep));
			planeSetElevator01(&simulationPlaneNegative, fmaxf(0.0f, pitchValue - pitchStep));
		} else {
			planeSetAileron01(&simulationPlanePositive, fminf(1.0f, rollValue + rollStep));
			planeSetAileron01(&simulationPlaneNegative, fmaxf(0.0f, rollValue - rollStep));
		}

		float runningAlignmentLossPositive = 0.0f;
		float runningAlignmentLossNegative = 0.0f;

		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlanePositive, deltaTime, NULL);
			runningAlignmentLossPositive += alignmentLoss(&simulationPlanePositive, target);
		}

		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlaneNegative, deltaTime, NULL);
			runningAlignmentLossNegative += alignmentLoss(&simulationPlaneNegative, target);
		}

		float distLossPositive = distanceToTarget(&simulationPlanePositive, target);
		float distLossNegative = distanceToTarget(&simulationPlaneNegative, target);

		float lossPositive = alignmentLoss(&simulationPlanePositive, target);
		float lossNegative = alignmentLoss(&simulationPlaneNegative, target);

		lossPositive += runningAlignmentLossPositive / (float)ctrl->LookaheadSteps;
		lossNegative += runningAlignmentLossNegative / (float)ctrl->LookaheadSteps;
		float currentDist = distanceToTarget(&ctrl->plane, target);
		lossPositive += distLossPositive - currentDist;
		lossNegative += distLossNegative - currentDist;

		if (action == 0) {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossYaw) {
					bestLossYaw = lossPositive;
					yawValue = fmaxf(0.0f, fminf(1.0f, yawValue + yawStep));
					output.Rudder = yawValue;
					yawStep *= 1.125f;
					printf("Yaw+ improved: %.4f\n", bestLossYaw);
				} else {
					yawStep *= 0.8f;
				}
			} else {
				if (lossNegative < bestLossYaw) {
					bestLossYaw = lossNegative;
					yawValue = fmaxf(0.0f, fminf(1.0f, yawValue - yawStep));
					output.Rudder = yawValue;
					yawStep *= 1.125f;
					printf("Yaw- improved: %.4f\n", bestLossYaw);
				} else {
					yawStep *= 0.8f;
				}
			}
		} else if (action == 1) {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossPitch) {
					bestLossPitch = lossPositive;
					pitchValue = fmaxf(0.0f, fminf(1.0f, pitchValue + pitchStep));
					output.Elevator = pitchValue;
					pitchStep *= 1.125f;
					printf("Pitch+ improved: %.4f\n", bestLossPitch);
				} else {
					pitchStep *= 0.8f;
				}
			} else {
				if (lossNegative < bestLossPitch) {
					bestLossPitch = lossNegative;
					pitchValue = fmaxf(0.0f, fminf(1.0f, pitchValue - pitchStep));
					output.Elevator = pitchValue;
					pitchStep *= 1.125f;
					printf("Pitch- improved: %.4f\n", bestLossPitch);
				} else {
					pitchStep *= 0.8f;
				}
			}
		} else {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossRoll) {
					bestLossRoll = lossPositive;
					rollValue = fmaxf(0.0f, fminf(1.0f, rollValue + rollStep));
					output.Aileron = rollValue;
					rollStep *= 1.125f;
					printf("Roll+ improved: %.4f\n", bestLossRoll);
				} else {
					rollStep *= 0.8f;
				}
			} else {
				if (lossNegative < bestLossRoll) {
					bestLossRoll = lossNegative;
					rollValue = fmaxf(0.0f, fminf(1.0f, rollValue - rollStep));
					output.Aileron = rollValue;
					rollStep *= 1.125f;
					printf("Roll- improved: %.4f\n", bestLossRoll);
				} else {
					rollStep *= 0.8f;
				}
			}
		}
	}

	float aileronLoss = rollLoss(&ctrl->plane, target);
	float elevatorLoss = pitchLoss(&ctrl->plane, target);
	float rudderLoss = yawLoss(&ctrl->plane, target);
	output.AileronLoss = bestLossRoll;
	output.ElevatorLoss = bestLossPitch;
	output.RudderLoss = bestLossYaw;

	return output;
}

static float evaluateLoss(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	float weightAlignment = 1.0f;
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float runningAlignmentLoss = 0.0f;
	float currentDist = distanceToTarget(&ctrl->plane, target);

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningAlignmentLoss += alignmentLoss(&simPlane, target);
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float loss = (weightAlignment * finalAlignment) + ((runningAlignmentLoss / (float)ctrl->LookaheadSteps) * weightAlignment) + (finalDist - currentDist);

	return loss;
}

static float evaluateLossV2(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float runningAlignmentLoss = 0.0f;
	float runningAlignmentLossVelocityVector = 0.0f;
	float currentDist = distanceToTarget(&ctrl->plane, target);

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningAlignmentLoss += alignmentLoss(&simPlane, target);
		runningAlignmentLossVelocityVector += alignmentLossVelocity(&simPlane, target);
	}

	float finalAlignment = alignmentLoss(&simPlane, target);
	float finalAlignmentVelocityVector = alignmentLossVelocity(&simPlane, target);
	float finalDist = distanceToTarget(&simPlane, target);

	float loss = finalAlignment + finalAlignmentVelocityVector + (runningAlignmentLoss / (float)ctrl->LookaheadSteps) + (runningAlignmentLossVelocityVector / (float)ctrl->LookaheadSteps) + (finalDist - currentDist);

	return loss;
}

// Closing-speed focus: extends V2 by rewarding high approach speed,
// not just direction alignment. Orientation terms give smooth gradient.
// TODO: doesn't work look into it
static float evaluateLossV3(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float runningLoss = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningLoss += alignmentLoss(&simPlane, target);
		runningLoss += alignmentLossVelocity(&simPlane, target);
		// Reward raw closing speed (m/s). At ~340 m/s this is ~340.
		// Divide by reference speed to keep scale comparable to alignment terms.
		float3 toTarget = Float3_Normalize(Float3_Sub(target, simPlane.position));
		float closingSpeed = Float3_Dot(simPlane.velocity, toTarget);
		runningLoss -= closingSpeed * (1.0f / 340.0f);
	}

	float finalDist = distanceToTarget(&simPlane, target);
	return (runningLoss / (float)ctrl->LookaheadSteps) + (finalDist - currentDist);
}

// Lateral-velocity focus: extends V2 by penalizing sideways drift.
// Orientation terms give smooth gradient; lateral term rewards coordinated turns.
// TODO: doesn't work look into it
static float evaluateLossV4(const Controller *ctrl, float values[3], float3 target, float deltaTime) {
	Plane simPlane = ctrl->plane;

	planeSetRudder01(&simPlane, values[0]);
	planeSetElevator01(&simPlane, values[1]);
	planeSetAileron01(&simPlane, values[2]);

	float currentDist = distanceToTarget(&ctrl->plane, target);
	float runningLoss = 0.0f;

	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&simPlane, deltaTime, NULL);
		runningLoss += alignmentLoss(&simPlane, target);
		runningLoss += alignmentLossVelocity(&simPlane, target);
		// Penalize lateral velocity component (normalized, 0..2 range).
		float3 toTarget = Float3_Normalize(Float3_Sub(target, simPlane.position));
		float3 velDir = Float3_Normalize(simPlane.velocity);
		float along = Float3_Dot(velDir, toTarget);
		float3 lateral = Float3_Sub(velDir, Float3_Scale(toTarget, along));
		runningLoss += Float3_Length(lateral);
	}

	float finalDist = distanceToTarget(&simPlane, target);
	return (runningLoss / (float)ctrl->LookaheadSteps) + (finalDist - currentDist);
}

typedef float (*LossFunction)(const Controller *ctrl, float values[3], float3 target, float deltaTime);

// TODO: test idea to change look ahead based on change of angle of loss
// TODO: test idea to use multiple look ahead periods for example 16 with 60 FPS, 8 with 30 FPS, 4 with 15 FPS, and 2 with 7.5 FPS, and then combine the losses from each of these look ahead periods to get a more robust loss evaluation
static ControllerOutput getControllerOutputV5(const Controller *ctrl, float3 target, float deltaTime, float *momentum, float *prevLoss, int maxIterations, LossFunction lossFunc) {
	ControllerOutput output = {0};

	float values[3] = {0.5f, 0.5f, 0.5f}; // yaw, pitch, roll
	float momentumCoefficient = 0.9f;	  // how much of the previous momentum to keep
	float learningRate = 0.05f;
	float epsilon = 0.025f; // for finite difference gradient

	float bestAxisLoss[3] = {FLT_MAX, FLT_MAX, FLT_MAX}; // best loss for yaw, pitch, roll

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

			float lossPos = lossFunc(ctrl, perturbedPositive, target, deltaTime);
			float lossNeg = lossFunc(ctrl, perturbedNegative, target, deltaTime);

			if (lossPos < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossPos;
			}
			if (lossNeg < bestAxisLoss[axis]) {
				bestAxisLoss[axis] = lossNeg;
			}

			// Central difference gradient
			gradient[axis] = (lossPos - lossNeg) / (2.0f * epsilon);
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

		// Adaptive learning rate decay
		learningRate *= 0.95f;
	}

	output.Rudder = values[0];
	output.Elevator = values[1];
	output.Aileron = values[2];

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

float lossPerSec(ControllerOutput *out, int steps, float dt) {
	float totalTime = steps * dt;
	if (totalTime <= 0.0f) return 0.0f;

	float invTime = 1.0f / totalTime;
	out->AileronLoss *= invTime;
	out->ElevatorLoss *= invTime;
	out->RudderLoss *= invTime;

	return out->AileronLoss + out->ElevatorLoss + out->RudderLoss;
}

typedef enum ControllerMode {
	Average,
	GreedyBest
} ControllerMode;

static ControllerOutput getControllerOutputV6(const Controller *ctrl, float3 target, float deltaTime, float *momentum, float *prevLoss, int iterations, int maxSimulationSteps, ControllerMode mode, LossFunction lossFunc) {
	float tempPrevLoss = *prevLoss;
	float tempMomentum[3] = {momentum[0], momentum[1], momentum[2]};
	Controller tempController = *ctrl;

	ControllerOutput out = getControllerOutputV5(&tempController, target, deltaTime, tempMomentum, &tempPrevLoss, maxSimulationSteps, lossFunc);
	float totalLoss = lossPerSec(&out, maxSimulationSteps, deltaTime);

	// Accumulators for weighted blending — controls and momentum
	float sumAileron = out.Aileron;
	float sumElevator = out.Elevator;
	float sumRudder = out.Rudder;
	float sumMomentum[3] = {tempMomentum[0], tempMomentum[1], tempMomentum[2]};
	float totalWeight = 1.0f; // baseline always has weight 1.0

	float modeTotalLoss[iterations + 1];
	ControllerOutput modelOutputs[iterations + 1];

	modeTotalLoss[0] = totalLoss;
	memcopy(&modelOutputs[0], &out);

	for (int iter = 1; iter < iterations + 1; iter++) {
		float iterPrevLoss = *prevLoss;
		float iterMomentum[3] = {momentum[0], momentum[1], momentum[2]};
		Controller iterController = *ctrl;

		float deltaTimeIter = deltaTime * iter * 4;
		int simulationSteps = maxSimulationSteps / 2;

		// V5 modifies iterMomentum in-place — capture it before it's lost
		ControllerOutput iterOut = getControllerOutputV5(&iterController, target, deltaTimeIter, iterMomentum, &iterPrevLoss, simulationSteps, lossFunc);
		float totalLoss = lossPerSec(&iterOut, simulationSteps, deltaTimeIter);
		modeTotalLoss[iter] = totalLoss;
		memcopy(&modelOutputs[iter], &iterOut);

		// Weight: lower per-second loss = higher weight
		// Clamp combinedLoss above -0.99 so denominator never goes to zero or negative
		float combinedLoss = iterOut.AileronLoss + iterOut.ElevatorLoss + iterOut.RudderLoss;
		if (combinedLoss < -0.99f) combinedLoss = -0.99f;
		float weight = 1.0f / (1.0f + combinedLoss);

		sumAileron += iterOut.Aileron * weight;
		sumElevator += iterOut.Elevator * weight;
		sumRudder += iterOut.Rudder * weight;
		sumMomentum[0] += iterMomentum[0] * weight;
		sumMomentum[1] += iterMomentum[1] * weight;
		sumMomentum[2] += iterMomentum[2] * weight;
		totalWeight += weight;
	}

	if (mode == GreedyBest) {
		int bestIdx = 0;
		for (int i = 1; i <= iterations; i++) {
			if (modeTotalLoss[i] < modeTotalLoss[bestIdx]) bestIdx = i;
		}
		out = modelOutputs[bestIdx];
	} else {
		// Normalized weighted blend
		out.Aileron = sumAileron / totalWeight;
		out.Elevator = sumElevator / totalWeight;
		out.Rudder = sumRudder / totalWeight;
		momentum[0] = sumMomentum[0] / totalWeight;
		momentum[1] = sumMomentum[1] / totalWeight;
		momentum[2] = sumMomentum[2] / totalWeight;
	}

	// Guard against NaN propagation
	if (isnan(out.Aileron)) out.Aileron = 0.5f;
	if (isnan(out.Elevator)) out.Elevator = 0.5f;
	if (isnan(out.Rudder)) out.Rudder = 0.5f;

	return out;
}

// version that reduces but increases steps
static ControllerOutput getControllerOutputV7(const Controller *ctrl, float3 target, float deltaTime, float *momentum, float *prevLoss, int iterations, int maxSimulationSteps, ControllerMode mode, LossFunction lossFunc) {
	float tempPrevLoss = *prevLoss;
	float tempMomentum[3] = {momentum[0], momentum[1], momentum[2]};
	Controller tempController = *ctrl;

	ControllerOutput out = getControllerOutputV5(&tempController, target, deltaTime, tempMomentum, &tempPrevLoss, maxSimulationSteps, lossFunc);
	float totalLoss = lossPerSec(&out, maxSimulationSteps, deltaTime);

	// Accumulators for weighted blending — controls and momentum
	float sumAileron = out.Aileron;
	float sumElevator = out.Elevator;
	float sumRudder = out.Rudder;
	float sumMomentum[3] = {tempMomentum[0], tempMomentum[1], tempMomentum[2]};
	float totalWeight = 1.0f; // baseline always has weight 1.0

	float modeTotalLoss[iterations + 1];
	ControllerOutput modelOutputs[iterations + 1];

	modeTotalLoss[0] = totalLoss;
	memcopy(&modelOutputs[0], &out);

	for (int iter = 1; iter < iterations + 1; iter++) {
		float iterPrevLoss = *prevLoss;
		float iterMomentum[3] = {momentum[0], momentum[1], momentum[2]};
		Controller iterController = *ctrl;

		float deltaTimeIter = deltaTime * iter / 2;
		int simulationSteps = maxSimulationSteps;

		// V5 modifies iterMomentum in-place — capture it before it's lost
		ControllerOutput iterOut = getControllerOutputV5(&iterController, target, deltaTimeIter, iterMomentum, &iterPrevLoss, simulationSteps, lossFunc);
		float totalLoss = lossPerSec(&iterOut, simulationSteps, deltaTimeIter);
		modeTotalLoss[iter] = totalLoss;
		memcopy(&modelOutputs[iter], &iterOut);

		// Weight: lower per-second loss = higher weight
		// Clamp combinedLoss above -0.99 so denominator never goes to zero or negative
		float combinedLoss = iterOut.AileronLoss + iterOut.ElevatorLoss + iterOut.RudderLoss;
		if (combinedLoss < -0.99f) combinedLoss = -0.99f;
		float weight = 1.0f / (1.0f + combinedLoss);

		sumAileron += iterOut.Aileron * weight;
		sumElevator += iterOut.Elevator * weight;
		sumRudder += iterOut.Rudder * weight;
		sumMomentum[0] += iterMomentum[0] * weight;
		sumMomentum[1] += iterMomentum[1] * weight;
		sumMomentum[2] += iterMomentum[2] * weight;
		totalWeight += weight;
	}

	if (mode == GreedyBest) {
		int bestIdx = 0;
		for (int i = 1; i <= iterations; i++) {
			if (modeTotalLoss[i] < modeTotalLoss[bestIdx]) bestIdx = i;
		}
		out = modelOutputs[bestIdx];
	} else {
		// Normalized weighted blend
		out.Aileron = sumAileron / totalWeight;
		out.Elevator = sumElevator / totalWeight;
		out.Rudder = sumRudder / totalWeight;
		momentum[0] = sumMomentum[0] / totalWeight;
		momentum[1] = sumMomentum[1] / totalWeight;
		momentum[2] = sumMomentum[2] / totalWeight;
	}

	// Guard against NaN propagation
	if (isnan(out.Aileron)) out.Aileron = 0.5f;
	if (isnan(out.Elevator)) out.Elevator = 0.5f;
	if (isnan(out.Rudder)) out.Rudder = 0.5f;

	return out;
}

static ControllerOutput getControllerOutputV4(const Controller *ctrl, float3 target, float deltaTime) {
	ControllerOutput output = {0};

	float values[3] = {0.5f, 0.5f, 0.5f};	// yaw, pitch, roll
	float momentum[3] = {0.0f, 0.0f, 0.0f}; // momentum for each axis
	float momentumCoefficient = 0.9f;		// how much of the previous momentum to keep
	float learningRate = 0.15f;
	float epsilon = 0.025f; // for finite difference gradient

	const int maxIterations = 512;

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

			float lossPos = evaluateLoss(ctrl, perturbedPositive, target, deltaTime);
			float lossNeg = evaluateLoss(ctrl, perturbedNegative, target, deltaTime);

			// Central difference gradient
			gradient[axis] = (lossPos - lossNeg) / (2.0f * epsilon);
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

		// Adaptive learning rate decay
		learningRate *= 0.95f;
	}

	output.Rudder = values[0];
	output.Elevator = values[1];
	output.Aileron = values[2];

	return output;
}

static ControllerOutput getControllerOutputV2(const Controller *ctrl, float3 target, float deltaTime) {
	ControllerOutput output = {0};

	float yawStep = 0.5f;
	float pitchStep = 0.5f;
	float rollStep = 0.5f;

	float yawValue = 0.5f;
	float pitchValue = 0.5f;
	float rollValue = 0.5f;

	const int maxIterations = 64;
	float bestLossYaw = FLT_MAX;
	float bestLossPitch = FLT_MAX;
	float bestLossRoll = FLT_MAX;

	for (int iter = 0; iter < maxIterations * 3; iter++) {
		int action = iter % 3;

		Plane simulationPlanePositive = ctrl->plane;
		Plane simulationPlaneNegative = ctrl->plane;

		planeSetRudder01(&simulationPlanePositive, yawValue);
		planeSetElevator01(&simulationPlanePositive, pitchValue);
		planeSetAileron01(&simulationPlanePositive, rollValue);
		planeSetRudder01(&simulationPlaneNegative, yawValue);
		planeSetElevator01(&simulationPlaneNegative, pitchValue);
		planeSetAileron01(&simulationPlaneNegative, rollValue);

		if (action == 0) {
			planeSetRudder01(&simulationPlanePositive, fminf(1.0f, yawValue + yawStep));
			planeSetRudder01(&simulationPlaneNegative, fmaxf(0.0f, yawValue - yawStep));
		} else if (action == 1) {
			planeSetElevator01(&simulationPlanePositive, fminf(1.0f, pitchValue + pitchStep));
			planeSetElevator01(&simulationPlaneNegative, fmaxf(0.0f, pitchValue - pitchStep));
		} else {
			planeSetAileron01(&simulationPlanePositive, fminf(1.0f, rollValue + rollStep));
			planeSetAileron01(&simulationPlaneNegative, fmaxf(0.0f, rollValue - rollStep));
		}

		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlaneNegative, deltaTime, NULL);
			float distToTarget = distanceToTarget(&simulationPlaneNegative, target);
			if (distToTarget < 10.0f) {
				break;
			}
		}

		for (int step = 0; step < ctrl->LookaheadSteps; step++) {
			updatePlane(&simulationPlanePositive, deltaTime, NULL);
			float distToTarget = distanceToTarget(&simulationPlanePositive, target);
			if (distToTarget < 10.0f) {
				break;
			}
		}

		float lossPositive = distanceToTarget(&simulationPlanePositive, target);
		float3 planeVelocityVectorPositive = Float3_Normalize(planeGetForwardVector(&simulationPlanePositive));
		// calculate misalignment between plane's forward vector and the vector to the target
		float3 targetVector = Float3_Normalize(Float3_Sub(target, simulationPlanePositive.position));
		// calculate dot product between plane's forward vector and the vector to the target
		float dotProduct = Float3_Dot(planeVelocityVectorPositive, targetVector);
		lossPositive += (1.0f - dotProduct) * 10.0f; // add a penalty for misalignment

		float lossNegative = distanceToTarget(&simulationPlaneNegative, target);
		float3 planeVelocityVectorNegative = Float3_Normalize(planeGetForwardVector(&simulationPlaneNegative));
		// calculate misalignment between plane's forward vector and the vector to the target
		float3 targetVectorNeg = Float3_Normalize(Float3_Sub(target, simulationPlaneNegative.position));
		// calculate dot product between plane's forward vector and the vector to the target
		float dotProductNeg = Float3_Dot(planeVelocityVectorNegative, targetVectorNeg);
		lossNegative += (1.0f - dotProductNeg) * 10.0f; // add a penalty for misalignment

		if (action == 0) {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossYaw) {
					bestLossYaw = lossPositive;
					yawValue = fmaxf(0.0f, fminf(1.0f, yawValue + yawStep));
					output.Rudder = yawValue;
					printf("Yaw+ improved: %.4f\n", bestLossYaw);
				}
			} else {
				if (lossNegative < bestLossYaw) {
					bestLossYaw = lossNegative;
					yawValue = fmaxf(0.0f, fminf(1.0f, yawValue - yawStep));
					output.Rudder = yawValue;
					printf("Yaw- improved: %.4f\n", bestLossYaw);
				}
			}
			yawStep *= 0.8f;
		} else if (action == 1) {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossPitch) {
					bestLossPitch = lossPositive;
					pitchValue = fmaxf(0.0f, fminf(1.0f, pitchValue + pitchStep));
					output.Elevator = pitchValue;
					printf("Pitch+ improved: %.4f\n", bestLossPitch);
				}
			} else {
				if (lossNegative < bestLossPitch) {
					bestLossPitch = lossNegative;
					pitchValue = fmaxf(0.0f, fminf(1.0f, pitchValue - pitchStep));
					output.Elevator = pitchValue;
					printf("Pitch- improved: %.4f\n", bestLossPitch);
				}
			}
			pitchStep *= 0.8f;
		} else {
			if (lossPositive < lossNegative) {
				if (lossPositive < bestLossRoll) {
					bestLossRoll = lossPositive;
					rollValue = fmaxf(0.0f, fminf(1.0f, rollValue + rollStep));
					output.Aileron = rollValue;
					printf("Roll+ improved: %.4f\n", bestLossRoll);
				}
			} else {
				if (lossNegative < bestLossRoll) {
					bestLossRoll = lossNegative;
					rollValue = fmaxf(0.0f, fminf(1.0f, rollValue - rollStep));
					output.Aileron = rollValue;
					printf("Roll- improved: %.4f\n", bestLossRoll);
				}
			}
			rollStep *= 0.8f;
		}
	}

	float aileronLoss = rollLoss(&ctrl->plane, target);
	float elevatorLoss = pitchLoss(&ctrl->plane, target);
	float rudderLoss = yawLoss(&ctrl->plane, target);
	output.AileronLoss = aileronLoss;
	output.ElevatorLoss = elevatorLoss;
	output.RudderLoss = rudderLoss;

	return output;
}

// TODO : create PID controller to control the plane to the target point in 3d space (distance loss)
// TODO : create PID controller to control the plane to the target point in 3d space (angle loss)

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

void saveLogsToCSV(const Logs *logs, const char *filename) {
	FILE *file = fopen(filename, "w");
	if (!file) {
		printf("Failed to open log file for writing: %s\n", filename);
		return;
	}
	fprintf(file, "Iteration,Aileron,AileronLoss,Elevator,ElevatorLoss,Rudder,RudderLoss,DistanceToTarget,PlanePositionX,PlanePositionY,PlanePositionZ,TargetPositionX,TargetPositionY,TargetPositionZ,PlaneVelocityX,PlaneVelocityY,PlaneVelocityZ,PlaneForwardX,PlaneForwardY,PlaneForwardZ,PlaneUpX,PlaneUpY,PlaneUpZ,PlaneRightX,PlaneRightY,PlaneRightZ,LossAngleChange\n");
	for (int i = 0; i < logs->len; i++) {
		const PlaneControlLogs *log = &logs->logs[i];
		fprintf(file, "%d,%.3f,%.4f,%.3f,%.4f,%.3f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.7f\n",
				log->iteration, log->aileronValue, log->aileronLoss, log->elevatorValue, log->elevatorLoss, log->rudderValue, log->rudderLoss, log->distanceToTarget, log->planePosition.x, log->planePosition.y, log->planePosition.z, log->targetPosition.x, log->targetPosition.y, log->targetPosition.z, log->planeVelocity.x, log->planeVelocity.y, log->planeVelocity.z, log->planeForward.x, log->planeForward.y, log->planeForward.z, log->planeUp.x, log->planeUp.y, log->planeUp.z, log->planeRight.x, log->planeRight.y, log->planeRight.z, log->lossAngle);
	}
	fclose(file);
}

// main testing loop to debug controller
int main() {
	// TODO - make research what helps with model accuracy (more iteration, lower learning rate, lower dt, higher dt)
	Plane plane;
	if (loadPlaneBin(&plane, "simulation/simModels/F-16C.bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 180.0f, 1.0f) != 0) {
		printf("Failed to load model: simulation/simModels/F-16C.bin\n");
		return 1;
	}

	Controller ctrl;
	initController(&ctrl, &plane);

	float3 target = Float3_Add(plane.position, (float3){1000.0f, 1000.0f, 1000.0f});

	const int simSteps = 1000;
	const float deltaTime = 1.0f / 60.0f; // 60 FPS

	Logs logs = {0};
	float momentum[3] = {0.0f, 0.0f, 0.0f}; // for getControllerOutputV5
	float prevLoss = 0.0f;

	for (int iter = 0; iter < simSteps; iter++) {
		// ControllerOutput out = getControllerOutputV5(&ctrl, target, deltaTime, momentum, &prevLoss, 128, evaluateLossV2);
		// ControllerOutput out = getControllerOutputV6(&ctrl, target, deltaTime, momentum, &prevLoss, 4, 128, GreedyBest, evaluateLossV2);
		// ControllerOutput out = getControllerOutputV6(&ctrl, target, deltaTime, momentum, &prevLoss, 8, 256, Average, evaluateLossV2);
		// ControllerOutput out = getControllerOutputV7(&ctrl, target, deltaTime, momentum, &prevLoss, 4, 128, Average, evaluateLossV2);
		ControllerOutput out = getControllerOutputV5(&ctrl, target, deltaTime, momentum, &prevLoss, 128, evaluateLossV3);
		printf("Sim Step %d: Aileron %.3f, Elevator %.3f, Rudder %.3f\n", iter, out.Aileron, out.Elevator, out.Rudder);

		planeSetAileron01(&ctrl.plane, out.Aileron);
		planeSetElevator01(&ctrl.plane, out.Elevator);
		planeSetRudder01(&ctrl.plane, out.Rudder);
		planeSetThrottle01(&ctrl.plane, 1.0f); // full throttle

		updatePlane(&ctrl.plane, deltaTime, NULL);

		float dist = distanceToTarget(&ctrl.plane, target);
		printf("[Step %i] Distance to target: %.2f\n", iter, dist);

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

	saveLogsToCSV(&logs, "simulation/cSim/flightControlLogs.csv");

	return 0;
}