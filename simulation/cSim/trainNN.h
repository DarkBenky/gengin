#include "simulate.h"
#include "import.h"
#include <cstddef>
#include <cstring>
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
    float3 currentPosition;
    float3 currentVelocity;
    float3 targetPosition;
    float Throttle; // 0.0 to 1.0
    float Aileron;  // 0.0 to 1.0
    float Elevator; // 0.0 to 1.0
    float Rudder;   // 0.0 to 1.0
} ModelInput;
    
typedef struct {
    float Throttle; // 0.0 to 1.0
    float Aileron;  // 0.0 to 1.0
    float Elevator; // 0.0 to 1.0
    float Rudder;   // 0.0 to 1.0
} ModelOutput;

float calculateMutationRate(float startRate, float endRate, int currentEpoch, int totalEpochs) {
    if (currentEpoch >= totalEpochs) return endRate;
    float progress = (float)currentEpoch / (float)totalEpochs;
    return startRate + progress * (endRate - startRate);
}

void generatePath(ModelTrainer *p, Plane plane, float maxDivergenceAngleDegrees, float maxDistance, int IterationCount) 
{
    p->startPosition = plane.position;
    p->prevPosition = plane.position;
    p->iterationCount = IterationCount;
    p->currentIteration = 0;

    float maxDivergenceAngleRadians = maxDivergenceAngleDegrees * (M_PI / 180.0f);
    float randomAngle = ((float)rand() / RAND_MAX) * maxDivergenceAngleRadians - (maxDivergenceAngleRadians / 2.0f);
    float3 forward = Float3_Normalize(plane.forward);
    float3 right = Float3_Normalize(Float3_Cross(forward, (float3){0.0f, 1.0f, 0.0f, 0.0f}));

    float distance = ((float)rand() / RAND_MAX) * maxDistance;
    p->targetPosition = Float3_Add(plane.position, Float3_Add(Float3_Scale(forward, distance), Float3_Scale(right, tanf(randomAngle) * distance)));
}

void initModelTrainer(ModelTrainer *p, int numModels, int epochs, int iterationCount, float startMutationRate, float endMutationRate, int layerCount, int layerSize) {
    p->numModels = numModels;
    p->epochs = epochs;
    p->iterationCount = iterationCount;
    p->startMutationRate = startMutationRate;
    p->endMutationRate = endMutationRate;
    p->currentEpoch = 0;
    p->currentIteration = 0;
    Client c = {.host = "127.0.0.1", .port = 5173};
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
            ActivationFunc act = (j == layerCount - 1) ? ACTIVATION_SIGMOID : ACTIVATION_RELU;
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
    uint8_t *base = (uint8_t *)(stats + 1);
    memcpy(base,                        p->losses,      lossSize);
    memcpy(base + lossSize,             p->paths,       pathsSize);
    memcpy(base + lossSize + pathsSize, p->epochLosses, epochLossesSize);
    *size = sizeof(trainingStats) + lossSize + pathsSize + epochLossesSize;
    return stats;
}

void epoch(ModelTrainer *p, Plane *plane) {
    float3 startPos = plane->position;
    float3 modelOrientation = plane->forward;
    float3 startVel = plane->velocity;

    generatePath(p, *plane, 90.0f, 5000.0f, p->iterationCount);

    for (int modelIdx = 0; modelIdx < p->numModels; modelIdx++) {
        // reset plane to start of epoch
        float totalLoss = 0.0f;
        plane->position = startPos;
        plane->forward = modelOrientation;
        plane->velocity = startVel;

        for (int step = 0; step < p->iterationCount; step++) {
            float3 prevPos = plane->position;
            ModelInput input = {
                .currentPosition = plane->position,
                .currentVelocity = plane->velocity,
                .targetPosition = p->targetPosition,
                .Throttle = planeGetThrottle01(plane),
                .Aileron = planeGetAileron01(plane),
                .Elevator = planeGetElevator01(plane),
                .Rudder = planeGetRudder01(plane)
            };
            ModelOutput output;
            Forward(&p->models[modelIdx], (float *)&input, (float *)&output);
            planeSetAileron01(plane, output.Aileron);
            planeSetElevator01(plane, output.Elevator);
            planeSetRudder01(plane, output.Rudder);
            planeSetThrottle01(plane, output.Throttle);
            float3 newForward;
            updatePlane(plane, 1.0f / 120.0f, &newForward);

            // loss calculation
            float distanceToTarget = Float3_Length(Float3_Sub(plane->position, p->targetPosition));
            float prevDistanceToTarget = Float3_Length(Float3_Sub(prevPos, p->targetPosition));
            float progressLoss = distanceToTarget / prevDistanceToTarget;
            float controlEffortLoss = (output.Throttle * output.Throttle) + (output.Aileron * output.Aileron) + (output.Elevator * output.Elevator) + (output.Rudder * output.Rudder);
            totalLoss += progressLoss + 0.0125f * controlEffortLoss;

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
    int eliteCount = p->numModels / 5;
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

    int statsSize;
    trainingStats *stats = serializeTrainStats(p, &statsSize);
    clientPost(p->client, (const char *)stats, statsSize);
    free(stats);
}

// TODO: train real model