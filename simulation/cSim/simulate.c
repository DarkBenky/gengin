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
static float f3Dot(float3 a, float3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static float f3Len(float3 v) {
	return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}
static float3 f3Norm(float3 v) {
	float l = f3Len(v);
	return l > 1e-6f ? f3Scale(v, 1.0f / l) : (float3){1.0f, 0.0f, 0.0f, 0.0f};
}

// Build right and up vectors from forward direction and bank angle.
static void buildFrame(float3 fwd, float bank, float3 *outRight, float3 *outUp) {
	float3 ref = (fabsf(fwd.y) < 0.99f) ? (float3){0.0f, 1.0f, 0.0f, 0.0f} : (float3){1.0f, 0.0f, 0.0f, 0.0f};
	float3 wRight = f3Norm(f3Cross(fwd, ref));
	float3 wUp = f3Cross(wRight, fwd);
	float cb = cosf(bank), sb = sinf(bank);
	*outRight = f3Add(f3Scale(wRight, cb), f3Scale(wUp, sb));
	*outUp = f3Add(f3Scale(wUp, cb), f3Scale(f3Scale(wRight, -1.0f), sb));
}

// Lift and drag magnitudes from real effective AoA (body AoA + surface deflection, in degrees).
// Stall is evaluated on the total effective AoA instead of just the surface deflection.
static void calcForceMagnitudes(const Surface *s, float altitude, float airspeed,
								float effectiveAoaDeg, float *outLift, float *outDrag) {
	if (!s->active) {
		*outLift = 0.0f;
		*outDrag = 0.0f;
		return;
	}
	float airDensity, speedOfSound;
	atmosphere(altitude, &airDensity, &speedOfSound);
	float mach = airspeed / (speedOfSound + 1e-6f);
	float q = 0.5f * airDensity * airspeed * airspeed;
	float aoaRad = effectiveAoaDeg * (float)(M_PI / 180.0);
	float stallFactor = (fabsf(effectiveAoaDeg) > s->stallAngle) ? 0.3f : 1.0f;
	float baseCL = (s->liftCoefficient * sinf(aoaRad) + 2.0f * (float)M_PI * s->camber) * stallFactor;
	float compressibility = (mach < 0.85f)
								? 1.0f / sqrtf(fmaxf(1.0f - mach * mach, 1e-4f))
								: 1.0f / sqrtf(1.0f - 0.85f * 0.85f);
	float CL = baseCL * compressibility;
	float waveCd;
	if (mach < 0.8f)
		waveCd = 0.0f;
	else if (mach < 1.2f) {
		float t = (mach - 0.8f) / 0.4f;
		waveCd = 0.5f * t * t * t;
	} else
		waveCd = 0.5f / sqrtf(fmaxf(mach * mach - 1.0f, 0.01f));
	float liftMag = q * s->surfaceArea * CL;
	float parasiticDrag = q * s->surfaceArea * (s->dragCoefficient * cosf(aoaRad) + waveCd);
	float inducedDrag = (liftMag * liftMag) /
						(q * (float)M_PI * s->aspectRatio * s->efficiency * s->surfaceArea + 1e-6f);
	*outLift = liftMag;
	*outDrag = parasiticDrag + fabsf(inducedDrag);
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
	// Lazy-init velocity — handles base plane copies and old structs without velocity field.
	if (f3Len(plane->velocity) < 1.0f && plane->currentSpeed > 1.0f)
		plane->velocity = f3Scale(f3Norm(plane->forward), plane->currentSpeed);

	float totalMass = plane->baseMass + plane->fuelMass * plane->currentFuelPercentage;

	Surface *allSurfaces[] = {
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
	for (int i = 0; i < 11; i++)
		stepSurface(allSurfaces[i], deltaTime);

	// Orientation frame from nose direction + bank angle.
	float3 fwd = f3Norm(plane->forward);
	float3 right_banked, up_banked;
	buildFrame(fwd, plane->bankAngle, &right_banked, &up_banked);

	// Real AoA and sideslip: angle between velocity vector and nose direction.
	float speed = f3Len(plane->velocity);
	float3 velNorm = speed > 1.0f ? f3Scale(plane->velocity, 1.0f / speed) : fwd;
	float velFwd = f3Dot(velNorm, fwd);
	float velUp = f3Dot(velNorm, up_banked);
	float velRight = f3Dot(velNorm, right_banked);
	// aoa_rad > 0 means nose is above the flight path (classic positive AoA).
	float aoa_rad = atan2f(-velUp, fmaxf(velFwd, 0.01f));
	// sideslip_rad > 0 means velocity comes from the right.
	float sideslip_rad = atan2f(velRight, fmaxf(velFwd, 0.01f));
	float aoa_deg = aoa_rad * (180.0f / (float)M_PI);
	float sideslip_deg = sideslip_rad * (180.0f / (float)M_PI);

	// Roll: aileron differential drives bankRate.
#define MAX_ROLL_RATE 4.5f
#define ROLL_DAMP 1.4f
	if (plane->leftAileron.active && plane->leftAileron.maxRotationAngle > 0.0f) {
		float ailCmd = plane->leftAileron.rotationAngle / plane->leftAileron.maxRotationAngle;
		plane->bankRate += (ailCmd * MAX_ROLL_RATE - plane->bankRate) * (deltaTime / 0.12f);
	}
	plane->bankRate -= ROLL_DAMP * plane->bankRate * deltaTime;
	plane->bankAngle += plane->bankRate * deltaTime;
	plane->bankAngle -= 0.1f * plane->bankAngle * deltaTime; // mild dihedral restoring

	// Pitch rate: elevator command + aerodynamic pitch stability (AoA restoring moment).
#define MAX_PITCH_RATE 1.8f // rad/s at full elevator
#define PITCH_CMD_TC 0.15f	// elevator response time constant
#define PITCH_DAMP 2.0f		// s^-1
#define PITCH_STAB 4.0f		// rad/s^2 per rad of AoA — static pitch stability
	if (plane->leftElevator.active && plane->leftElevator.maxRotationAngle != 0.0f) {
		float elevCmd = 0.5f * (plane->leftElevator.rotationAngle + plane->rightElevator.rotationAngle) / plane->leftElevator.maxRotationAngle;
		plane->pitchRate += (elevCmd * MAX_PITCH_RATE - plane->pitchRate) * (deltaTime / PITCH_CMD_TC);
	}
	plane->pitchRate -= PITCH_DAMP * plane->pitchRate * deltaTime;
	plane->pitchRate -= PITCH_STAB * aoa_rad * deltaTime; // AoA drives nose back toward velocity

	// Yaw rate: rudder command + weathervane stability (sideslip restoring).
#define MAX_YAW_RATE 0.8f
#define YAW_CMD_TC 0.15f
#define YAW_DAMP 2.0f
#define YAW_STAB 3.0f // rad/s^2 per rad of sideslip
	if (plane->rudder.active && plane->rudder.maxRotationAngle != 0.0f) {
		float rudCmd = plane->rudder.rotationAngle / plane->rudder.maxRotationAngle;
		plane->yawRate += (rudCmd * MAX_YAW_RATE - plane->yawRate) * (deltaTime / YAW_CMD_TC);
	}
	plane->yawRate -= YAW_DAMP * plane->yawRate * deltaTime;
	plane->yawRate -= YAW_STAB * sideslip_rad * deltaTime; // weathervane restoring

	// Rotate nose direction by pitch and yaw rates, then rebuild the frame.
	fwd = f3Norm(f3Add(fwd, f3Scale(up_banked, plane->pitchRate * deltaTime)));
	fwd = f3Norm(f3Add(fwd, f3Scale(right_banked, plane->yawRate * deltaTime)));
	plane->forward = fwd;
	buildFrame(fwd, plane->bankAngle, &right_banked, &up_banked);

	// Aerodynamic forces — lift perpendicular to velocity (in banked-up dir), drag opposing velocity.
	float totalLift = 0.0f, totalDrag = 0.0f, totalLateral = 0.0f;

	// Horizontal surfaces: body pitch AoA + surface deflection angle.
	Surface *hSurf[] = {
		&plane->leftWing,
		&plane->rightWing,
		&plane->horizontalStabilizer,
		&plane->leftFlap,
		&plane->rightFlap,
		&plane->leftElevator,
		&plane->rightElevator,
	};
	for (int i = 0; i < 7; i++) {
		float lift, drag;
		calcForceMagnitudes(hSurf[i], plane->currentAltitude, speed,
							aoa_deg + hSurf[i]->rotationAngle, &lift, &drag);
		totalLift += lift;
		totalDrag += drag;
	}

	// Vertical surfaces: sideslip + surface deflection angle.
	Surface *vSurf[] = {&plane->verticalStabilizer, &plane->rudder};
	for (int i = 0; i < 2; i++) {
		float lift, drag;
		calcForceMagnitudes(vSurf[i], plane->currentAltitude, speed,
							sideslip_deg + vSurf[i]->rotationAngle, &lift, &drag);
		totalLateral -= lift; // opposes sideslip (weathervane force)
		totalDrag += drag;
	}

	// Ailerons: differential so net lift ~cancels; count their drag only.
	{
		float lift, drag;
		calcForceMagnitudes(&plane->leftAileron, plane->currentAltitude, speed,
							aoa_deg + plane->leftAileron.rotationAngle, &lift, &drag);
		totalDrag += drag * 0.5f;
		calcForceMagnitudes(&plane->rightAileron, plane->currentAltitude, speed,
							aoa_deg + plane->rightAileron.rotationAngle, &lift, &drag);
		totalDrag += drag * 0.5f;
	}

	float3 worldForce = {0.0f, 0.0f, 0.0f, 0.0f};
	worldForce = f3Add(worldForce, f3Scale(up_banked, totalLift));		 // lift in banked-up direction
	worldForce = f3Add(worldForce, f3Scale(velNorm, -totalDrag));		 // drag opposing velocity
	worldForce = f3Add(worldForce, f3Scale(right_banked, totalLateral)); // lateral aero

	// Thrust along nose.
	float effectiveThrust = (plane->currentFuelPercentage > 0.0f)
								? plane->maxTrust * plane->currentTrustPercentage
								: 0.0f;
	worldForce = f3Add(worldForce, f3Scale(fwd, effectiveThrust));
	worldForce.y -= 9.81f * totalMass; // gravity

	// Fuel burn.
	if (plane->currentTrustPercentage > 0.0f && plane->currentFuelPercentage > 0.0f) {
		float t = plane->currentTrustPercentage;
		float burn = (t > 0.8f)
						 ? plane->burnWithoutAfterburner + (plane->burnRate - plane->burnWithoutAfterburner) * ((t - 0.8f) / 0.2f)
						 : plane->burnWithoutAfterburner * (t / 0.8f);
		plane->currentFuelPercentage = fmaxf(0.0f,
											 (plane->fuelMass * plane->currentFuelPercentage - burn * deltaTime) / plane->fuelMass);
	}

	// Integrate velocity with net force.
	float3 accel = f3Scale(worldForce, 1.0f / totalMass);
	plane->velocity = f3Add(plane->velocity, f3Scale(accel, deltaTime));
	plane->position = f3Add(plane->position, f3Scale(plane->velocity, deltaTime));
	plane->currentAltitude = plane->position.y;
	plane->currentSpeed = f3Len(plane->velocity);

	if (newForwardDirection)
		*newForwardDirection = plane->forward;

	// Sync Euler angles for renderer.
	plane->rotation.y = atan2f(plane->forward.x, plane->forward.z);
	plane->rotation.x = asinf(-fmaxf(-1.0f, fminf(1.0f, plane->forward.y)));
	plane->rotation.z = plane->bankAngle;
}