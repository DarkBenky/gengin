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

#include "../../object/format.h"
#include "../../math/vector3.h"
#include "simulate.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_ITERATION_PER_AXIS 128
#define LOOKAHEAD_STEPS 16

typedef struct {
    Plane plane;

    int LookaheadSteps;

    // Target motion between controller calls, finite-differenced from the last
    // two target positions: the caller supplies only a position per frame.
    float3 prevTarget;
    float3 targetVelocity;
    int hasPrevTarget;
} Controller;

static void initController(Controller *ctrl, const Plane *plane) {
    ctrl->plane = *plane;
    ctrl->LookaheadSteps = LOOKAHEAD_STEPS;
    ctrl->prevTarget = (float3){0.0f, 0.0f, 0.0f};
    ctrl->targetVelocity = (float3){0.0f, 0.0f, 0.0f};
    ctrl->hasPrevTarget = 0;
}

typedef struct {
    float Elevator;
    float ElevatorLoss;
    float Aileron;
    float AileronLoss;
    float Rudder;
    float RudderLoss;
    float LossAngle;
} ControllerOutput; 