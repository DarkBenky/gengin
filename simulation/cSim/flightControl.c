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

	return atan2f(cross, dot); // radians, 0 = perfect pitch alignment
}

float yawLoss(const Plane *plane, float3 target) {
	float3 toTarget = Float3_Normalize(Float3_Sub(target, plane->position));
	float3 forward = planeGetForwardVector(plane);
	float3 up = planeGetUpVector(plane);

	float3 idealForward = Float3_Normalize(Float3_Sub(toTarget, Float3_Scale(up, Float3_Dot(toTarget, up))));

	float cross = Float3_Dot(Float3_Cross(forward, idealForward), up);
	float dot = Float3_Dot(forward, idealForward);

	return atan2f(cross, dot); // radians, 0 = perfect yaw alignment
}

// TODO: implement this
static ControllerOutput getControllerOutput(const Controller *ctrl, float3 target, float deltaTime) {
	ControllerOutput output = {0};

	float aileronStart = 0.5f; // neutral
	float aileronStep = 0.15f;
	bool aileronSide = true; // true = increase from neutral, false = decrease from neutral

	Plane baselinePlane = ctrl->plane;

	// get baseline loss at neutral aileron
	planeSetAileron01(&baselinePlane, aileronStart);
	for (int step = 0; step < ctrl->LookaheadSteps; step++) {
		updatePlane(&baselinePlane, deltaTime, NULL);
	}
	float baselineLoss = rollLoss(&baselinePlane, target);
	printf("Baseline roll loss at neutral aileron: %.4f\n", baselineLoss);

	float bestAileronValue = aileronStart;
	float bestLoss = baselineLoss;

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
		printf("Iter %d: Aileron %.3f, Loss %.4f\n", iter, aileronValue, loss);

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

	printf("Best aileron value: %.3f with roll loss: %.4f\n", bestAileronValue, bestLoss);

	output.Aileron = bestAileronValue;

	// TODO: loop to minimize pitchLoss

	// TODO: loop to minimize yawLoss

	return output;
}

// main testing loop to debug controller
int main() {
	Plane plane;
	if (loadPlaneBin(&plane, "simulation/simModels/F-16C.bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 340.0f, 1.0f) != 0) {
		printf("Failed to load model: simulation/simModels/F-16C.bin\n");
		return 1;
	}

	Controller ctrl;
	initController(&ctrl, &plane);

	float3 target = Float3_Add(plane.position, (float3){1000.0f, 1000.0f, 1000.0f});

	getControllerOutput(&ctrl, target, 1.0f / 60.0f);

	return 0;
}
