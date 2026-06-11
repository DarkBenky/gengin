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
	// printf("Baseline roll loss at neutral aileron: %.4f\n", baselineLoss);

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
		// printf("Iter %d: Aileron %.3f, Loss %.4f\n", iter, aileronValue, loss);

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
	float pitchBaselineLoss = pitchLoss(&pitchBaselinePlane, target);
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
	float yawBaselineLoss = yawLoss(&yawBaselinePlane, target);
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

float distanceToTarget(const Plane *plane, float3 target) {
	return Float3_Length(Float3_Sub(target, plane->position));
}

typedef struct {
	int iteration;
	float aileronValue;
	float aileronLoss;
	float elevatorValue;
	float elevatorLoss;
	float rudderValue;
	float rudderLoss;
	float distanceToTarget;
	float3 planePosition;
	float3 targetPosition;
	float3 planeVelocity;

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
	fprintf(file, "Iteration,Aileron,AileronLoss,Elevator,ElevatorLoss,Rudder,RudderLoss,DistanceToTarget,PlanePositionX,PlanePositionY,PlanePositionZ,TargetPositionX,TargetPositionY,TargetPositionZ,PlaneVelocityX,PlaneVelocityY,PlaneVelocityZ\n");
	for (int i = 0; i < logs->len; i++) {
		const PlaneControlLogs *log = &logs->logs[i];
		fprintf(file, "%d,%.3f,%.4f,%.3f,%.4f,%.3f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
			log->iteration, log->aileronValue, log->aileronLoss, log->elevatorValue, log->elevatorLoss, log->rudderValue, log->rudderLoss, log->distanceToTarget, log->planePosition.x, log->planePosition.y, log->planePosition.z, log->targetPosition.x, log->targetPosition.y, log->targetPosition.z, log->planeVelocity.x, log->planeVelocity.y, log->planeVelocity.z);
	}
	fclose(file);
}


// main testing loop to debug controller
int main() {
	Plane plane;
	if (loadPlaneBin(&plane, "simulation/simModels/F-16C.bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 180.0f, 1.0f) != 0) {
		printf("Failed to load model: simulation/simModels/F-16C.bin\n");
		return 1;
	}

	Controller ctrl;
	initController(&ctrl, &plane);

	float3 target = Float3_Add(plane.position, (float3){1000.0f, 1000.0f, 1000.0f});

	const int simSteps = 2000;
	const float deltaTime = 1.0f / 60.0f; // 60 FPS

	Logs logs = {0};

	for (int iter = 0; iter < simSteps; iter++) {
		ControllerOutput out = getControllerOutput(&ctrl, target, deltaTime);
		printf("Sim Step %d: Aileron %.3f, Elevator %.3f, Rudder %.3f\n", iter, out.Aileron, out.Elevator, out.Rudder);

		planeSetAileron01(&ctrl.plane, out.Aileron);
		planeSetElevator01(&ctrl.plane, out.Elevator);
		planeSetRudder01(&ctrl.plane, out.Rudder);
		planeSetThrottle01(&ctrl.plane, 1.0f); // full throttle

		updatePlane(&ctrl.plane, deltaTime, NULL);

		float dist = distanceToTarget(&ctrl.plane, target);
		printf("[Step %i] Distance to target: %.2f\n", iter, dist);

		PlaneControlLogs log = {
			.iteration = iter,
			.aileronValue = out.Aileron,
			.aileronLoss = out.AileronLoss,
			.elevatorValue = out.Elevator,
			.elevatorLoss = out.ElevatorLoss,
			.rudderValue = out.Rudder,
			.rudderLoss = out.RudderLoss,
			.distanceToTarget = dist,
			.planePosition = ctrl.plane.position,
			.targetPosition = target,
			.planeVelocity = ctrl.plane.velocity
		};
		addLog(&logs, log);
	}

	saveLogsToCSV(&logs, "simulation/cSim/flightControlLogs.csv");

	return 0;
}
