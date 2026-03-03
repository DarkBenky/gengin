#ifndef LIFT_H
#define LIFT_H

#include "../math/vector3.h"
#include "../object/format.h"

// Returns the lift force vector in world space, given the velocity, forward and up directions eg. wing orientation, and lift/drag coefficients. Lift is perpendicular to velocity and forward, in the plane defined by forward and up.
float3 calculateLift(float3 velocity, float3 forward, float3 up, float liftCoefficient, float dragCoefficient);
float3 calculateDrag(float3 velocity, float dragCoefficient);    

#endif // LIFT_H