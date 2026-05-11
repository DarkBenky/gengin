#include "simulate.h"
#include "import.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
#include <math.h>
#include <time.h>
#include "../../math/vector3.h"
#include "dense.h"
#include "../../client/client.h"

typedef struct {
	float3 startPosition;
	float3 targetPosition;
	float3 prevPosition;
	int iterationCount;
	int currentIteration;
	Model *models;
	float *losses;			 // total epoch loss for each model
	uint32 *modelLossOrders; // sorted indexes of models by loss for selection
	float3 *paths;			 // model count * iteration count -> commit to python to visualize it
	float3 *epochLosses;	 // model count * iteration count -> commit to python to visualize it
	int numModels;
	float startMutationRate;
	float endMutationRate;
	int epochs;
	int currentEpoch;
	Client client;			// for sending training data to python for visualization
	int eliteReachedTarget; // count of elite models that reached target this epoch
	int epochsSinceLastTarget;
} ModelTrainer;
typedef struct {
	float3 toTargetDir;		// normalized (targetPosition - currentPosition) direction vector
	float3 toTarget;		// (targetPosition - currentPosition) / initialDist  [-1..1 approx]
	float3 currentVelocity; // velocity / 500                                      [-1..1 approx]
	float altitudeError;	// (targetAltitude - currentAltitude) / 500            [-1..1 approx]
	float verticalVelocity; // velocity.y / 100                                    [-1..1 approx]
	float bankAngle;		// radians / PI                                        [-1..1]
	float bankRate;			// rad/s / 3                                           [-1..1 approx]
	float pitchRate;		// rad/s / 3                                           [-1..1 approx]
	float speed;			// currentSpeed / 500                                  [ 0..1 approx]
	float Throttle;			// 0.0 to 1.0
	float Aileron;			// 0.0 to 1.0
	float Elevator;			// 0.0 to 1.0
	float Rudder;			// 0.0 to 1.0
} ModelInput;

typedef struct {
	float Throttle; // 0.0 to 1.0 (mapped from tanh via (out+1)/2)
	float Aileron;	// -1.0 to 1.0
	float Elevator; // -1.0 to 1.0
	float Rudder;	// -1.0 to 1.0
} ModelOutput;

float calculateMutationRate(float startRate, float endRate, int currentEpoch, int totalEpochs) {
	if (currentEpoch >= totalEpochs) return endRate;
	// geometric (exponential) interpolation: reaches midpoint in log-space at half the epochs
	// e.g. 0.01->0.0005 over 1000 epochs: at epoch 100 rate=0.00363, at epoch 500 rate=0.00158
	float t = (float)currentEpoch / (float)totalEpochs;
	return startRate * powf(endRate / startRate, t);
}

void generatePath(ModelTrainer *p, Plane plane, float minDistance, float maxDistance, int IterationCount) {
	p->startPosition = plane.position;
	p->prevPosition = plane.position;
	p->iterationCount = IterationCount;
	p->currentIteration = 0;

	float maxDivergenceAngleRadians = 45.0f * (M_PI / 180.0f);
	float randomAngle = ((float)rand() / RAND_MAX) * maxDivergenceAngleRadians - (maxDivergenceAngleRadians / 2.0f);

	// project forward onto horizontal plane so target is never behind or above/below a cliff
	float3 hFwd = {plane.forward.x, 0.0f, plane.forward.z, 0.0f};
	float hLen = sqrtf(hFwd.x * hFwd.x + hFwd.z * hFwd.z);
	if (hLen < 1e-4f)
		hFwd = (float3){0.0f, 0.0f, 1.0f, 0.0f}; // plane pointing straight up/down — fall back to world Z
	else
		hFwd = Float3_Scale(hFwd, 1.0f / hLen);
	float3 right = Float3_Normalize(Float3_Cross(hFwd, (float3){0.0f, 1.0f, 0.0f, 0.0f}));

	float distance = minDistance + ((float)rand() / RAND_MAX) * (maxDistance - minDistance);
	float3 horizontal = Float3_Add(Float3_Scale(hFwd, distance), Float3_Scale(right, tanf(randomAngle) * distance));
	// bell-curve altitude bias: average 3 uniform samples so the distribution peaks at 0
	// and extreme climbs/dives are rare — keeps most targets near the current altitude
	float r0 = ((float)rand() / RAND_MAX) - 0.5f;
	float r1 = ((float)rand() / RAND_MAX) - 0.5f;
	float r2 = ((float)rand() / RAND_MAX) - 0.5f;
	float altOffset = ((r0 + r1 + r2) / 3.0f) * 1000.0f;
	p->targetPosition = (float3){plane.position.x + horizontal.x, plane.position.y + altOffset, plane.position.z + horizontal.z, 0.0f};
}

void initModelTrainer(ModelTrainer *p, int numModels, int epochs, int iterationCount, float startMutationRate, float endMutationRate, int layerCount, int layerSize, uint16 port) {
	p->numModels = numModels;
	p->epochs = epochs;
	p->iterationCount = iterationCount;
	p->startMutationRate = startMutationRate;
	p->endMutationRate = endMutationRate;
	p->currentEpoch = 0;
	p->currentIteration = 0;
	Client c = {.host = "127.0.0.1", .port = port};
	p->client = c;
	p->eliteReachedTarget = 0;

	p->models = (Model *)malloc(sizeof(Model) * numModels);
	p->losses = (float *)malloc(sizeof(float) * numModels);
	p->paths = (float3 *)malloc(sizeof(float3) * numModels * p->iterationCount);
	p->epochLosses = (float3 *)malloc(sizeof(float3) * numModels * p->iterationCount);
	p->modelLossOrders = (uint32 *)malloc(sizeof(uint32) * numModels);
	memset(p->paths, 0, sizeof(float3) * numModels * p->iterationCount);
	memset(p->epochLosses, 0, sizeof(float3) * numModels * p->iterationCount);
	for (int i = 0; i < numModels; i++) {
		Model model;
		InitModel(&model, sizeof(ModelInput) / sizeof(float), sizeof(ModelOutput) / sizeof(float));
		p->models[i] = model;
		for (int j = 0; j < layerCount; j++) {
			uint32 inSize = (j == 0) ? p->models[i].inputSize : (uint32)layerSize;
			uint32 outSize = (j == layerCount - 1) ? p->models[i].outputSize : (uint32)layerSize;
			ActivationFunc act = (j == layerCount - 1) ? TANH : RELU;
			AddDenseLayer(&p->models[i], inSize, outSize, act);
		}
		MutateModel(&p->models[i], startMutationRate);
		p->losses[i] = 0.0f;
		p->modelLossOrders[i] = i;
	}
}

typedef struct {
	uint32 modelCount;
	uint32 iterationCount;
	float startPosition[3];
	float targetPosition[3];
	// wire layout after this header:
	//   float[modelCount]                   losses
	//   float3[modelCount * iterationCount] paths
	//   float3[modelCount * iterationCount] epochLosses (x=totalLoss, y=distanceToTarget, z=controlEffortLoss)
} trainingStats;

trainingStats *serializeTrainStats(ModelTrainer *p, int *size) {
	int lossSize = sizeof(float) * p->numModels;
	int pathsSize = sizeof(float3) * p->numModels * p->iterationCount;
	int epochLossesSize = sizeof(float3) * p->numModels * p->iterationCount;
	trainingStats *stats = (trainingStats *)malloc(sizeof(trainingStats) + lossSize + pathsSize + epochLossesSize);
	stats->modelCount = p->numModels;
	stats->iterationCount = p->iterationCount;
	stats->startPosition[0] = p->startPosition.x;
	stats->startPosition[1] = p->startPosition.y;
	stats->startPosition[2] = p->startPosition.z;
	stats->targetPosition[0] = p->targetPosition.x;
	stats->targetPosition[1] = p->targetPosition.y;
	stats->targetPosition[2] = p->targetPosition.z;
	uint8_t *base = (uint8_t *)(stats + 1);
	memcpy(base, p->losses, lossSize);
	memcpy(base + lossSize, p->paths, pathsSize);
	memcpy(base + lossSize + pathsSize, p->epochLosses, epochLossesSize);
	*size = sizeof(trainingStats) + lossSize + pathsSize + epochLossesSize;
	return stats;
}

#define ACCEPTABLE_DISTANCE_TO_TARGET 250.0f
void epoch(ModelTrainer *p, Plane *plane, float *top10PercentLoss) {
	float3 startPos = plane->position;
	float3 modelOrientation = plane->forward;
	float3 startVel = plane->velocity;
	float startBankAngle = plane->bankAngle;
	float startBankRate = plane->bankRate;
	float startPitchRate = plane->pitchRate;
	float startYawRate = plane->yawRate;
	float startFuelPct = plane->currentFuelPercentage;

	// target 20-60% of the distance the plane can cruise in the allotted steps,
	// so the plane has enough time to reach it but also isn't trivially close.
	float simTime = p->iterationCount * (1.0f / 24.0f);
	float cruiseRange = simTime * 250.0f;
	// generate a new path on the very first epoch, when at least one elite model reached the target
	// last epoch, or when the target has gone 100 epochs unreached (likely unreachable — pick a fresh one)
	if (p->currentEpoch == 0 || p->eliteReachedTarget > 0 || p->epochsSinceLastTarget >= 100) {
		generatePath(p, *plane, cruiseRange * 0.2f, cruiseRange * 0.6f, p->iterationCount);
		p->epochsSinceLastTarget = 0;
	}
	printf("Target position: x: %f, y: %f, z: %f | Plane position: x: %f, y: %f, z: %f\n",
		   p->targetPosition.x, p->targetPosition.y, p->targetPosition.z,
		   startPos.x, startPos.y, startPos.z);
	p->eliteReachedTarget = 0;
	float initialDist = Float3_Length(Float3_Sub(startPos, p->targetPosition));
	if (initialDist < 1.0f) initialDist = 1.0f;

	// precompute which model indices are top 10% from last epoch's sort
	int eliteCheckCount = p->numModels / 10;
	if (eliteCheckCount < 1) eliteCheckCount = 1;
	uint8_t *isElite = calloc(p->numModels, 1);
	for (int i = 0; i < eliteCheckCount; i++)
		isElite[p->modelLossOrders[i]] = 1;

	for (int modelIdx = 0; modelIdx < p->numModels; modelIdx++) {
		// reset plane to start of epoch
		float totalLoss = 0.0f;
		plane->position = startPos;
		plane->forward = modelOrientation;
		plane->velocity = startVel;
		plane->bankAngle = startBankAngle;
		plane->bankRate = startBankRate;
		plane->pitchRate = startPitchRate;
		plane->yawRate = startYawRate;
		plane->currentFuelPercentage = startFuelPct;

		for (int step = 0; step < p->iterationCount; step++) {
			// normalize by initialDist: magnitude=1 at start, ~0 at target — always well-conditioned
			float3 toTarget = Float3_Scale(Float3_Sub(p->targetPosition, plane->position), 1.0f / initialDist);
			float3 velNorm = Float3_Scale(plane->velocity, 1.0f / 500.0f);
			float altErr = (p->targetPosition.y - plane->position.y) / 500.0f;
			float vertVel = plane->velocity.y / 100.0f;
			ModelInput input = {
				.toTargetDir = Float3_Normalize(toTarget),
				.toTarget = toTarget,
				.currentVelocity = velNorm,
				.altitudeError = altErr,
				.verticalVelocity = vertVel,
				.bankAngle = plane->bankAngle / (float)M_PI,
				.bankRate = plane->bankRate / 3.0f,
				.pitchRate = plane->pitchRate / 3.0f,
				.speed = plane->currentSpeed / 500.0f,
				.Throttle = planeGetThrottle01(plane),
				.Aileron = planeGetAileron01(plane),
				.Elevator = planeGetElevator01(plane),
				.Rudder = planeGetRudder01(plane)};
			ModelOutput output;
			Forward(&p->models[modelIdx], (float *)&input, (float *)&output);
			// tanh output: surfaces in [-1,1], throttle mapped from [-1,1] to [0,1]
			planeSetAileron(plane, output.Aileron);
			planeSetElevator(plane, output.Elevator);
			planeSetRudder(plane, output.Rudder);
			planeSetThrottle(plane, (output.Throttle + 1.0f) * 0.5f);
			float3 newForward;
			updatePlane(plane, 1.0f / 24.0f, &newForward);

			float distanceToTarget = Float3_Length(Float3_Sub(plane->position, p->targetPosition));
			if (distanceToTarget < ACCEPTABLE_DISTANCE_TO_TARGET) {
				printf("Model: %d reached target\n", modelIdx);
				// only count elite models (already sorted from last epoch) to avoid noise from lucky randoms
				if (isElite[modelIdx])
					p->eliteReachedTarget++;
				// clear remaining path slots so visualization doesn't show stale data
				memset(&p->paths[modelIdx * p->iterationCount + step + 1], 0,
					   sizeof(float3) * (p->iterationCount - step - 1));
				break;
			}

			float controlEffortLoss = (output.Throttle * output.Throttle) + (output.Aileron * output.Aileron) + (output.Elevator * output.Elevator) + (output.Rudder * output.Rudder);
			// normalize by initial distance so losses are comparable across epochs regardless of target distance
			// small control-effort penalty encourages smooth inputs and breaks ties between equal-distance models
			totalLoss += distanceToTarget / initialDist + 0.05f * controlEffortLoss;

			p->paths[modelIdx * p->iterationCount + step] = plane->position;
			p->epochLosses[modelIdx * p->iterationCount + step] = (float3){totalLoss, distanceToTarget, controlEffortLoss};
		}
		p->losses[modelIdx] = totalLoss;
	}
	free(isElite);

	// restore plane to its pre-epoch state so the next epoch captures the correct startPos
	plane->position = startPos;
	plane->forward = modelOrientation;
	plane->velocity = startVel;
	plane->bankAngle = startBankAngle;
	plane->bankRate = startBankRate;
	plane->pitchRate = startPitchRate;
	plane->yawRate = startYawRate;

	// sort ascending by loss: index 0 = best (lowest loss)
	for (int i = 1; i < p->numModels; i++) {
		uint32 key = p->modelLossOrders[i];
		int j = i - 1;
		while (j >= 0 && p->losses[p->modelLossOrders[j]] > p->losses[key]) {
			p->modelLossOrders[j + 1] = p->modelLossOrders[j];
			j--;
		}
		p->modelLossOrders[j + 1] = key;
	}

	// elitism + crossover
	int eliteCount = p->numModels / 5; // top 20% are parents for next generation
	if (eliteCount < 1) eliteCount = 1;

	float mutationRate = calculateMutationRate(p->startMutationRate, p->endMutationRate, p->currentEpoch, p->epochs);

	Model *nextGen = (Model *)malloc(sizeof(Model) * (p->numModels - eliteCount));
	for (int i = 0; i < p->numModels - eliteCount; i++) {
		memset(&nextGen[i], 0, sizeof(Model));
		int parentAIdx = p->modelLossOrders[rand() % eliteCount];
		int parentBIdx = p->modelLossOrders[rand() % eliteCount];
		CrossoverModels(&nextGen[i], &p->models[parentAIdx], &p->models[parentBIdx], mutationRate);
	}

	for (int i = eliteCount; i < p->numModels; i++) {
		FreeModel(&p->models[p->modelLossOrders[i]]);
		p->models[p->modelLossOrders[i]] = nextGen[i - eliteCount];
	}
	free(nextGen);

	p->currentEpoch++;
	p->epochsSinceLastTarget++;

	float minEpochLoss = p->losses[p->modelLossOrders[0]];
	float maxEpochLoss = p->losses[p->modelLossOrders[p->numModels - 1]];
	float medianEpochLoss = p->losses[p->modelLossOrders[p->numModels / 2]];
	printf("Epoch %d: min loss=%.6f, median loss=%.6f, max loss=%.6f, mutation rate=%.6f\n", p->currentEpoch, minEpochLoss, medianEpochLoss, maxEpochLoss, mutationRate);

	// calculate top 10 percent average loss for model saving criteria
	int top10Count = p->numModels / 10;
	if (top10Count < 1) top10Count = 1;
	float top10LossSum = 0.0f;
	for (int i = 0; i < top10Count; i++) {
		top10LossSum += p->losses[p->modelLossOrders[i]];
	}
	*top10PercentLoss = top10LossSum / (float)top10Count;

	int statsSize;
	trainingStats *stats = serializeTrainStats(p, &statsSize);
	clientPost(&p->client, (const char *)stats, statsSize);
	free(stats);
}