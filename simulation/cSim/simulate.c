#include "simulate.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void atmosphere(float altitude, float *outDensity, float *outSpeedOfSound) {
	altitude = fmaxf(0.0f, fminf(altitude, 25000.0f));
	float T, pressure;
	if (altitude < 11000.0f) {
		T = 288.15f - 0.0065f * altitude;
		pressure = 101325.0f * powf(T / 288.15f, 5.2561f);
	} else {
		T = 216.65f;
		pressure = 22632.1f * expf(-0.0001577f * (altitude - 11000.0f));
	}
	*outDensity = pressure / (287.05f * T);
	*outSpeedOfSound = sqrtf(1.4f * 287.05f * T);
}

float calculateAirDensity(float altitude) {
	float density, sos;
	atmosphere(altitude, &density, &sos);
	return density;
}

void calculateSurfaceForces(const Surface *surface, float altitude, float airspeed, AerodynamicForces *forces) {
	float airDensity, speedOfSound;
	atmosphere(altitude, &airDensity, &speedOfSound);

	float mach = airspeed / speedOfSound;
	float dynamicPressure = 0.5f * airDensity * airspeed * airspeed;
	float aoaRad = surface->rotationAngle * (float)(M_PI / 180.0);

	float stallFactor = (fabsf(surface->rotationAngle) > surface->stallAngle) ? 0.3f : 1.0f;
	float baseCL = (surface->liftCoefficient * sinf(aoaRad) + 2.0f * (float)M_PI * surface->camber) * stallFactor;

	float compressibility;
	if (mach < 0.85f) {
		float beta = sqrtf(fmaxf(1.0f - mach * mach, 1e-4f));
		compressibility = 1.0f / beta;
	} else {
		compressibility = 1.0f / sqrtf(1.0f - 0.85f * 0.85f);
	}
	float effectiveCL = baseCL * compressibility;

	float parasiticCD = surface->dragCoefficient * cosf(aoaRad);
	float waveCd;
	if (mach < 0.8f) {
		waveCd = 0.0f;
	} else if (mach < 1.2f) {
		float t = (mach - 0.8f) / 0.4f;
		waveCd = 0.5f * t * t * t;
	} else {
		waveCd = 0.5f / sqrtf(fmaxf(mach * mach - 1.0f, 0.01f));
	}

	float parasiticDrag = dynamicPressure * surface->surfaceArea * (parasiticCD + waveCd);
	float liftMagnitude = dynamicPressure * surface->surfaceArea * effectiveCL;
	float inducedDrag = (liftMagnitude * liftMagnitude) /
						(dynamicPressure * (float)M_PI * surface->aspectRatio * surface->efficiency * surface->surfaceArea + 1e-6f);
	float dragMagnitude = parasiticDrag + inducedDrag;

	// lift dir = cross(rotationAxis, +X_forward) = (0, ax.z, -ax.y)
	float3 ax = surface->rotationAxis;
	float ly = ax.z;
	float lz = -ax.y;
	float llen = sqrtf(ly * ly + lz * lz);
	if (llen > 1e-6f) {
		ly /= llen;
		lz /= llen;
	}

	forces->Lift = (float3){0.0f, ly * liftMagnitude, lz * liftMagnitude, 0.0f};
	forces->Drag = (float3){-dragMagnitude, 0.0f, 0.0f, 0.0f};
	forces->CenterOfLift = surface->relativePos;
	forces->CenterOfDrag = surface->relativePos;
	forces->CenterOfMass = surface->relativePos;
}

static float3 f3Add(float3 a, float3 b) {
	return (float3){a.x + b.x, a.y + b.y, a.z + b.z, 0.0f};
}
static float3 f3Scale(float3 v, float s) {
	return (float3){v.x * s, v.y * s, v.z * s, 0.0f};
}
static float3 f3Cross(float3 a, float3 b) {
	return (float3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
}
static float f3Len(float3 v) {
	return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}
static float3 f3Norm(float3 v) {
	float l = f3Len(v);
	return l > 1e-6f ? f3Scale(v, 1.0f / l) : (float3){1.0f, 0.0f, 0.0f, 0.0f};
}

static float3 localToWorld(float3 local, float3 fwd) {
	float3 ref = (fabsf(fwd.y) < 0.99f) ? (float3){0.0f, 1.0f, 0.0f, 0.0f} : (float3){1.0f, 0.0f, 0.0f, 0.0f};
	float3 right = f3Norm(f3Cross(fwd, ref));
	float3 up = f3Cross(right, fwd);
	float3 r = f3Scale(fwd, local.x);
	r = f3Add(r, f3Scale(up, local.y));
	r = f3Add(r, f3Scale(right, local.z));
	return r;
}

static void setSurfaceTarget(Surface *surface, float norm) {
	if (!surface->active) return;
	norm = fmaxf(-1.0f, fminf(1.0f, norm));
	float t = (norm + 1.0f) * 0.5f;
	surface->targetRotationAngle = surface->minRotationAngle + t * (surface->maxRotationAngle - surface->minRotationAngle);
}

static void stepSurface(Surface *s, float dt) {
	if (!s->active) return;
	float diff = s->targetRotationAngle - s->rotationAngle;
	float step = s->rotationRate * dt;
	if (fabsf(diff) <= step)
		s->rotationAngle = s->targetRotationAngle;
	else
		s->rotationAngle += (diff > 0.0f ? step : -step);
}

void planeSetThrottle(Plane *plane, float pct) {
	plane->currentTrustPercentage = fmaxf(0.0f, fminf(1.0f, pct));
}

void planeSetAileron(Plane *plane, float norm) {
	setSurfaceTarget(&plane->leftAileron, -norm);
	setSurfaceTarget(&plane->rightAileron, norm);
}

void planeSetElevator(Plane *plane, float norm) {
	setSurfaceTarget(&plane->leftElevator, norm);
	setSurfaceTarget(&plane->rightElevator, norm);
}

void planeSetRudder(Plane *plane, float norm) {
	setSurfaceTarget(&plane->rudder, norm);
}

void planeSetFlap(Plane *plane, float norm) {
	setSurfaceTarget(&plane->leftFlap, norm);
	setSurfaceTarget(&plane->rightFlap, norm);
}

void updatePlane(Plane *plane, float deltaTime, float3 *newForwardDirection) {
	float totalMass = plane->baseMass + plane->fuelMass * plane->currentFuelPercentage;

	Surface *surfaces[] = {
		&plane->leftWing,
		&plane->rightWing,
		&plane->verticalStabilizer,
		&plane->horizontalStabilizer,
		&plane->leftAileron,
		&plane->rightAileron,
		&plane->rudder,
		&plane->leftFlap,
		&plane->rightFlap,
		&plane->leftElevator,
		&plane->rightElevator,
	};
	int surfaceCount = (int)(sizeof(surfaces) / sizeof(surfaces[0]));

	for (int i = 0; i < surfaceCount; i++)
		stepSurface(surfaces[i], deltaTime);

	float3 localForce = {0.0f, 0.0f, 0.0f, 0.0f};
	AerodynamicForces sf;
	for (int i = 0; i < surfaceCount; i++) {
		calculateSurfaceForces(surfaces[i], plane->currentAltitude, plane->currentSpeed, &sf);
		localForce = f3Add(localForce, sf.Lift);
		localForce = f3Add(localForce, sf.Drag);
	}

	float3 worldForce = localToWorld(localForce, plane->forward);
	float effectiveThrust = (plane->currentFuelPercentage > 0.0f)
								? plane->maxTrust * plane->currentTrustPercentage
								: 0.0f;
	worldForce = f3Add(worldForce, f3Scale(plane->forward, effectiveThrust));
	worldForce.y -= 9.81f * totalMass;

	if (plane->currentTrustPercentage > 0.0f && plane->currentFuelPercentage > 0.0f) {
		float t = plane->currentTrustPercentage;
		float burn = (t > 0.8f)
						 ? plane->burnWithoutAfterburner + (plane->burnRate - plane->burnWithoutAfterburner) * ((t - 0.8f) / 0.2f)
						 : plane->burnWithoutAfterburner * (t / 0.8f);
		float remaining = fmaxf(0.0f, plane->fuelMass * plane->currentFuelPercentage - burn * deltaTime);
		plane->currentFuelPercentage = remaining / plane->fuelMass;
	}

	float3 accel = f3Scale(worldForce, 1.0f / totalMass);
	float3 velocity = f3Scale(plane->forward, plane->currentSpeed);
	float3 newVel = f3Add(velocity, f3Scale(accel, deltaTime));
	float newSpeed = f3Len(newVel);

	plane->position = f3Add(plane->position, f3Scale(velocity, deltaTime));
	plane->currentAltitude = plane->position.y;

	if (newSpeed > 0.1f) {
		plane->forward = f3Norm(newVel);
		plane->currentSpeed = newSpeed;
	}

	if (newForwardDirection)
		*newForwardDirection = plane->forward;
}