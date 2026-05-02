#include "simulate.h"
#include "import.h"
#include <math.h>
#include <time.h>
#include "../../math/vector3.h"
#include "../../object/scene.h"
#include "dense.h"

typedef struct {
    float3 startPosition;
    float3 targetPosition;
    float3 prevPosition;
    int iterationCount;
    int currentIteration;
    int ObjectIds[16];
} Path;

void generatePath(Path *p, Plane plane, float maxDivergenceAngleDegrees, float maxDistance, ObjectList *scene, int IterationCount) 
{
    p->startPosition = plane.position;
    p->prevPosition = plane.position;
    p->iterationCount = IterationCount;
    p->currentIteration = 0;

    float maxDivergenceAngleRadians = maxDivergenceAngleDegrees * (M_PI / 180.0f);
    float randomAngle = ((float)rand() / RAND_MAX) * maxDivergenceAngleRadians - (maxDivergenceAngleRadians / 2.0f);
    float3 forward = Float3_Normalize(plane.forward);
    float3 right = Float3_Normalize(Float3_Cross(forward, (float3){0.0f, 1.0f, 0.0f, 0.0f}));
    float3 up = Float3_Cross(right, forward);

    float distance = ((float)rand() / RAND_MAX) * maxDistance;
    p->targetPosition = Float3_Add(plane.position, Float3_Add(Float3_Scale(forward, distance), Float3_Scale(right, tanf(randomAngle) * distance)));

    float3 pathDirection = Float3_Normalize(Float3_Sub(p->targetPosition, p->startPosition));

    // free old path objects
    const int numPathObjects = 16;
    for (int i = 0; i < numPathObjects; i++) {
        if (p->ObjectIds[i] >= 0 && p->ObjectIds[i] < scene->count) {
            ObjectList_Remove(scene, p->ObjectIds[i]);
            p->ObjectIds[i] = -1;
        }
    }

    // generate the objects in path to visualize the path
    for (int i = 0; i < numPathObjects; i++) {
        Object *obj = ObjectList_Add(scene);
        CreateCube(obj, Float3_Add(p->startPosition, Float3_Scale(pathDirection, (distance / numPathObjects) * i)), (float3){0.0f, 0.0f, 0.0f}, (float3){1.0f, 1.0f, 1.0f}, (float3){1.0f, 1.0f, 0.0f}, NULL, 0.0f, 0.5f, 0.5f);
        Object_UpdateWorldBounds(obj);
        p->ObjectIds[i] = scene->count - 1;
    }
}

void createModel(Model *model, int numHiddenLayers, int hiddenLayerSize) {
    model->inputSize = sizeof(ModelInput) / sizeof(float);
    model->outputSize = sizeof(ModelOutput) / sizeof(float);
    // TODO : implement model and integrate into training loop
    
}

float loss(Path *p, Plane *plane, ObjectList *scene, Model *model) {
    if (p->currentIteration >= p->iterationCount) {
        generatePath(p, *plane, 90.0f, 5000.0f, scene, p->iterationCount);
    }
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
    Forward(model, (float *)&input, (float *)&output);
    planeSetAileron01(plane, output.Aileron);
    planeSetElevator01(plane, output.Elevator);
    planeSetRudder01(plane, output.Rudder);
    planeSetThrottle01(plane, output.Throttle);
    float3 newForward;
    updatePlane(plane, 1.0f / 60.0f, &newForward);

    float distanceToTarget = Float3_Length(Float3_Sub(plane->position, p->targetPosition));
    float prevDistanceToTarget = Float3_Length(Float3_Sub(prevPos, p->targetPosition));
    float progressLoss = distanceToTarget / prevDistanceToTarget; // want this to be < 1.0, meaning we're getting closer to the target
    float controlEffortLoss = (output.Throttle * output.Throttle) + (output.Aileron * output.Aileron) + (output.Elevator * output.Elevator) + (output.Rudder * output.Rudder); // want this to be small to encourage efficient controls
    float totalLoss = progressLoss + 0.0125f * controlEffortLoss;
    p->currentIteration++;
    return totalLoss;
}

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
