#include "simulate.h"
#include "import.h"
#include <stddef.h>
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
    float *losses; // total epoch loss for each model
    uint32 *modelLossOrders; // sorted indexes of models by loss for selection
    float3 *paths; // model count * iteration count -> commit to python to visualize it
    float3 *epochLosses; // model count * iteration count -> commit to python to visualize it
    int numModels;
    float startMutationRate;
    float endMutationRate;
    int epochs;
    int currentEpoch;
    Client client; // for sending training data to python for visualization
} ModelTrainer;
typedef struct {
    float3 toTarget;        // (targetPosition - currentPosition) / 5000  [-1..1 approx]
    float3 currentVelocity; // velocity / 500                              [-1..1 approx]
    float Throttle; // 0.0 to 1.0
    float Aileron;  // 0.0 to 1.0
    float Elevator; // 0.0 to 1.0
    float Rudder;   // 0.0 to 1.0
} ModelInput;
    
typedef struct {
    float Throttle; // 0.0 to 1.0 (mapped from tanh via (out+1)/2)
    float Aileron;  // -1.0 to 1.0
    float Elevator; // -1.0 to 1.0
    float Rudder;   // -1.0 to 1.0
} ModelOutput;

float calculateMutationRate(float startRate, float endRate, int currentEpoch, int totalEpochs) {
    if (currentEpoch >= totalEpochs) return endRate;
    float progress = (float)currentEpoch / (float)totalEpochs;
    return startRate + progress * (endRate - startRate);
}

void generatePath(ModelTrainer *p, Plane plane, float minDistance, float maxDistance, int IterationCount) 
{
    p->startPosition = plane.position;
    p->prevPosition = plane.position;
    p->iterationCount = IterationCount;
    p->currentIteration = 0;

    float maxDivergenceAngleRadians = 45.0f * (M_PI / 180.0f);
    float randomAngle = ((float)rand() / RAND_MAX) * maxDivergenceAngleRadians - (maxDivergenceAngleRadians / 2.0f);
    float3 forward = Float3_Normalize(plane.forward);
    float3 right = Float3_Normalize(Float3_Cross(forward, (float3){0.0f, 1.0f, 0.0f, 0.0f}));

    float distance = minDistance + ((float)rand() / RAND_MAX) * (maxDistance - minDistance);
    p->targetPosition = Float3_Add(plane.position, Float3_Add(Float3_Scale(forward, distance), Float3_Scale(right, tanf(randomAngle) * distance)));
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
            uint32 inSize  = (j == 0)              ? p->models[i].inputSize  : (uint32)layerSize;
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

trainingStats* serializeTrainStats(ModelTrainer *p, int *size) {
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
    memcpy(base,                        p->losses,      lossSize);
    memcpy(base + lossSize,             p->paths,       pathsSize);
    memcpy(base + lossSize + pathsSize, p->epochLosses, epochLossesSize);
    *size = sizeof(trainingStats) + lossSize + pathsSize + epochLossesSize;
    return stats;
}

void epoch(ModelTrainer *p, Plane *plane, float *top10PercentLoss) {
    float3 startPos = plane->position;
    float3 modelOrientation = plane->forward;
    float3 startVel = plane->velocity;
    float startBankAngle = plane->bankAngle;
    float startBankRate  = plane->bankRate;
    float startPitchRate = plane->pitchRate;
    float startYawRate   = plane->yawRate;

    // target 20-60% of the distance the plane can cruise in the allotted steps,
    // so the plane has enough time to reach it but also isn't trivially close.
    float simTime = p->iterationCount * (1.0f / 24.0f);
    float cruiseRange = simTime * 250.0f;
    generatePath(p, *plane, cruiseRange * 0.2f, cruiseRange * 0.6f, p->iterationCount);
    float initialDist = Float3_Length(Float3_Sub(startPos, p->targetPosition));
    if (initialDist < 1.0f) initialDist = 1.0f;

    for (int modelIdx = 0; modelIdx < p->numModels; modelIdx++) {
        // reset plane to start of epoch
        float totalLoss = 0.0f;
        plane->position  = startPos;
        plane->forward   = modelOrientation;
        plane->velocity  = startVel;
        plane->bankAngle = startBankAngle;
        plane->bankRate  = startBankRate;
        plane->pitchRate = startPitchRate;
        plane->yawRate   = startYawRate;

        for (int step = 0; step < p->iterationCount; step++) {
            // normalize by initialDist: magnitude=1 at start, ~0 at target — always well-conditioned
            float3 toTarget = Float3_Scale(Float3_Sub(p->targetPosition, plane->position), 1.0f / initialDist);
            float3 velNorm  = Float3_Scale(plane->velocity, 1.0f / 500.0f);
            ModelInput input = {
                .toTarget        = toTarget,
                .currentVelocity = velNorm,
                .Throttle = planeGetThrottle01(plane),
                .Aileron  = planeGetAileron01(plane),
                .Elevator = planeGetElevator01(plane),
                .Rudder   = planeGetRudder01(plane)
            };
            ModelOutput output;
            Forward(&p->models[modelIdx], (float *)&input, (float *)&output);
            // tanh output: surfaces in [-1,1], throttle mapped from [-1,1] to [0,1]
            planeSetAileron(plane,   output.Aileron);
            planeSetElevator(plane,  output.Elevator);
            planeSetRudder(plane,    output.Rudder);
            planeSetThrottle(plane,  (output.Throttle + 1.0f) * 0.5f);
            float3 newForward;
            updatePlane(plane, 1.0f / 24.0f, &newForward);

            float distanceToTarget = Float3_Length(Float3_Sub(plane->position, p->targetPosition));
            float controlEffortLoss = (output.Throttle * output.Throttle) + (output.Aileron * output.Aileron)
                                    + (output.Elevator * output.Elevator) + (output.Rudder * output.Rudder);
            // normalize by initial distance so losses are comparable across epochs regardless of target distance
            totalLoss += distanceToTarget / initialDist;

            p->paths[modelIdx * p->iterationCount + step] = plane->position;
            p->epochLosses[modelIdx * p->iterationCount + step] = (float3){totalLoss, distanceToTarget, controlEffortLoss};
        }
        p->losses[modelIdx] = totalLoss;
    }

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

// TODO: train real model