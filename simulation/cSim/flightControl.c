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

	return atan2f(cross, dot); // radians, 0 = perfect roll alignment
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
static ControllerOutput getControllerOutput(const Controller *ctrl, float3 target) {
	// TODO: loop to minimize rollLoss

    // TODO: loop to minimize pitchLoss

    // TODO: loop to minimize yawLoss
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
}
