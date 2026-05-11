#include "simulate.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Moments of inertia (kg*m^2) — approximated for a fighter-sized aircraft.
// Ixx = roll, Iyy = pitch, Izz = yaw.
#define I_ROLL 40000.0f
#define I_PITCH 80000.0f
#define I_YAW 90000.0f

// Aerodynamic damping coefficients (N*m per rad/s).
// Model airframe rotational resistance proportional to angular velocity (Clp, Cmq, Cnr derivatives).
// Values derived from equilibrium roll rate: at cruise q=17821 (220m/s, 5000m), max aileron torque
// ~306kN*m must give ~45 deg/s (0.78 rad/s) → DAMP_ROLL = torque / (rate * dampScale) ≈ 80000.
// Gives decay times: ~0.4s roll, ~0.6s pitch, ~0.9s yaw at cruise speed.
#define DAMP_ROLL 80000.0f
#define DAMP_PITCH 130000.0f
#define DAMP_YAW 100000.0f

// Lever arms (m) from CG to surface aerodynamic center.
#define LEVER_AILERON 5.0f
#define LEVER_ELEVATOR 7.0f
#define LEVER_RUDDER 7.0f

// Wing-leveling stability coefficient (N*m). Multiplied by sin(bankAngle) to
// produce a restoring roll torque, modelling the combined dihedral effect and
// pendular stability (CG below wing aerodynamic center).
#define BANK_RESTORE_COEFF 80000.0f

// Dihedral coupling: roll torque per (rad * q * m^2) from sideslip.
// Models the tendency of a swept/dihedral wing to roll into a sideslip.
#define DIHEDRAL_EFFECT_COEFF 0.1f

// Weathervane stability: yaw torque per (rad * q) from sideslip.
// Represents the combined fin + fuselage directional stability derivative (Cnβ).
#define WEATHERVANE_COEFF 6.0f

// Adverse yaw scaling: multiplies aileron drag to produce differential yaw torque.
#define ADVERSE_YAW_FACTOR 4.0f

// Minimum surface area (m^2) below which no aerodynamic force is computed.
#define MIN_SURFACE_AREA 1e-6f

// Minimum control half-range (deg) below which deflection is treated as zero.
#define MIN_CONTROL_RANGE 1e-4f

// Wave drag coefficient constant for the supersonic branch (Mach >= 1.2).
// Derived from the cubic formula at Mach 1.2 (= 0.5) times sqrt(1.2^2 - 1) ≈ 0.3317
// to ensure continuity with the transonic cubic at the boundary.
#define WAVE_CD_SUPERSONIC 0.3317f

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
// ref x fwd gives the true body-right vector; fwd x right gives body-up.
// Bank rotation: positive bank = right wing down (D-key convention).
// outUp  = wUp*cos + wRight*sin  -> tilts right at positive bank (lift curves right)
// outRight = wRight*cos - wUp*sin -> tilts down-right at positive bank
static void buildFrame(float3 fwd, float bank, float3 *outRight, float3 *outUp) {
	float3 ref = (fabsf(fwd.y) < 0.99f) ? (float3){0.0f, 1.0f, 0.0f, 0.0f} : (float3){1.0f, 0.0f, 0.0f, 0.0f};
	float3 wRight = f3Norm(f3Cross(ref, fwd));
	float3 wUp = f3Cross(fwd, wRight);
	float cb = cosf(bank), sb = sinf(bank);
	*outRight = f3Add(f3Scale(wRight, cb), f3Scale(f3Scale(wUp, -1.0f), sb));
	*outUp = f3Add(f3Scale(wUp, cb), f3Scale(wRight, sb));
}

// Lift and drag magnitudes from real effective AoA (body AoA + surface deflection, in degrees).
// Stall is evaluated on the total effective AoA instead of just the surface deflection.
static void calcForceMagnitudes(const Surface *s, float q, float mach,
								float effectiveAoaDeg, float *outLift, float *outDrag) {
	// active=false means the surface is structurally fixed, not servo-driven.
	// It still generates aerodynamic force based on its fixed angle and area.
	// Zero-area surfaces (e.g. unused slots) produce zero force naturally.
	if (s->surfaceArea < MIN_SURFACE_AREA) {
		*outLift = 0.0f;
		*outDrag = 0.0f;
		return;
	}
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
		waveCd = WAVE_CD_SUPERSONIC / sqrtf(fmaxf(mach * mach - 1.0f, 0.01f));
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

static float getSurfacePct(const Surface *surface) {
	float range = surface->maxRotationAngle - surface->minRotationAngle;
	if (range < 1e-6f) return 50.0f;
	return (surface->rotationAngle - surface->minRotationAngle) / range * 100.0f;
}

static float getSurface01(const Surface *surface) {
	float range = surface->maxRotationAngle - surface->minRotationAngle;
	if (range < 1e-6f) return 0.5f;
	return (surface->rotationAngle - surface->minRotationAngle) / range;
}

static float getSurfaceNorm(const Surface *surface) {
	return getSurface01(surface) * 2.0f - 1.0f;
}

float3 planeGetForwardVector(const Plane *plane) {
	return plane->forward;
}

float3 planeGetRightVector(const Plane *plane) {
	float3 right, up;
	buildFrame(plane->forward, plane->bankAngle, &right, &up);
	return right;
}

float3 planeGetUpVector(const Plane *plane) {
	float3 right, up;
	buildFrame(plane->forward, plane->bankAngle, &right, &up);
	return up;
}

void planeSetThrottle(Plane *plane, float pct) {
	plane->currentTrustPercentage = fmaxf(0.0f, fminf(1.0f, pct));
}

float planeGetThrottlePct(const Plane *plane) {
	return plane->currentTrustPercentage * 100.0f;
}
float planeGetAileronPct(const Plane *plane) {
	return getSurfacePct(&plane->rightAileron);
}
float planeGetElevatorPct(const Plane *plane) {
	return getSurfacePct(&plane->rightElevator);
}
float planeGetRudderPct(const Plane *plane) {
	return getSurfacePct(&plane->rudder);
}
float planeGetFlapPct(const Plane *plane) {
	return getSurfacePct(&plane->rightFlap);
}

float planeGetThrottle01(const Plane *plane) {
	return plane->currentTrustPercentage;
}
float planeGetAileron01(const Plane *plane) {
	return getSurface01(&plane->rightAileron);
}
float planeGetElevator01(const Plane *plane) {
	return getSurface01(&plane->rightElevator);
}
float planeGetRudder01(const Plane *plane) {
	return getSurface01(&plane->rudder);
}
float planeGetFlap01(const Plane *plane) {
	return getSurface01(&plane->rightFlap);
}

float planeGetAileronNorm(const Plane *plane) {
	return getSurfaceNorm(&plane->rightAileron);
}
float planeGetElevatorNorm(const Plane *plane) {
	return getSurfaceNorm(&plane->rightElevator);
}
float planeGetRudderNorm(const Plane *plane) {
	return getSurfaceNorm(&plane->rudder);
}
float planeGetFlapNorm(const Plane *plane) {
	return getSurfaceNorm(&plane->rightFlap);
}

static float pctToNorm(float pct) {
	return pct / 50.0f - 1.0f;
}

static float norm01ToNorm(float v) {
	return v * 2.0f - 1.0f;
}

void planeSetThrottlePct(Plane *plane, float pct) {
	planeSetThrottle(plane, pct / 100.0f);
}
void planeSetAileronPct(Plane *plane, float pct) {
	planeSetAileron(plane, pctToNorm(pct));
}
void planeSetElevatorPct(Plane *plane, float pct) {
	planeSetElevator(plane, pctToNorm(pct));
}
void planeSetRudderPct(Plane *plane, float pct) {
	planeSetRudder(plane, pctToNorm(pct));
}
void planeSetFlapPct(Plane *plane, float pct) {
	planeSetFlap(plane, pctToNorm(pct));
}

void planeSetThrottle01(Plane *plane, float v) {
	plane->currentTrustPercentage = fmaxf(0.0f, fminf(1.0f, v));
}
void planeSetAileron01(Plane *plane, float v) {
	planeSetAileron(plane, norm01ToNorm(v));
}
void planeSetElevator01(Plane *plane, float v) {
	planeSetElevator(plane, norm01ToNorm(v));
}
void planeSetRudder01(Plane *plane, float v) {
	planeSetRudder(plane, norm01ToNorm(v));
}
void planeSetFlap01(Plane *plane, float v) {
	planeSetFlap(plane, norm01ToNorm(v));
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
	// sideslip_rad > 0 means the velocity vector points to the right of the nose
	// (aircraft crabbing right, or equivalently: nose is left of the velocity direction).
	// Weathervane: positive sideslip → yaw nose right (toward velocity) → yawTorque += K*sideslip.
	float sideslip_rad = atan2f(velRight, fmaxf(velFwd, 0.01f));
	float aoa_deg = aoa_rad * (180.0f / (float)M_PI);
	float sideslip_deg = sideslip_rad * (180.0f / (float)M_PI);

	// Atmosphere state for this frame — computed once, shared across all surface force calcs.
	float airDensity, speedOfSound;
	atmosphere(plane->currentAltitude, &airDensity, &speedOfSound);
	float mach = speed / (speedOfSound + 1e-6f);
	float q = 0.5f * airDensity * speed * speed;

	float controlScale = fminf(1.0f, q / 5000.0f);
	// Separate uncapped scale for damping so it keeps growing with speed.
	// This prevents the instability where control forces grow with q^2 but
	// damping stays constant once controlScale hits its 1.0 cap.
	float dampScale = q / 5000.0f;

	// Resolve current surface deflections once — needed across multiple torque terms.
	float ailDefl = 0.0f, elevDefl = 0.0f, rudDefl = 0.0f;
	if (plane->rightAileron.active) {
		float neutral = (plane->rightAileron.minRotationAngle + plane->rightAileron.maxRotationAngle) * 0.5f;
		float halfRange = (plane->rightAileron.maxRotationAngle - plane->rightAileron.minRotationAngle) * 0.5f;
		if (halfRange > MIN_CONTROL_RANGE) ailDefl = (plane->rightAileron.rotationAngle - neutral) / halfRange;
	}
	if (plane->rightElevator.active) {
		float neutral = (plane->rightElevator.minRotationAngle + plane->rightElevator.maxRotationAngle) * 0.5f;
		float halfRange = (plane->rightElevator.maxRotationAngle - plane->rightElevator.minRotationAngle) * 0.5f;
		if (halfRange > MIN_CONTROL_RANGE) elevDefl = (plane->rightElevator.rotationAngle - neutral) / halfRange;
	}
	if (plane->rudder.active) {
		float neutral = (plane->rudder.minRotationAngle + plane->rudder.maxRotationAngle) * 0.5f;
		float halfRange = (plane->rudder.maxRotationAngle - plane->rudder.minRotationAngle) * 0.5f;
		if (halfRange > MIN_CONTROL_RANGE) rudDefl = (plane->rudder.rotationAngle - neutral) / halfRange;
	}

	// Roll: aileron differential lift + dihedral stability (sideslip rolls wings level).
	// Dihedral effect: right sideslip (sideslip_rad > 0) exposes right wing to more airflow →
	// right wing generates more lift → aircraft rolls LEFT (negative torque). Using -= here.
	float rollTorque = ailDefl * q * plane->rightAileron.surfaceArea * plane->rightAileron.liftCoefficient * LEVER_AILERON * controlScale;
	rollTorque -= sideslip_rad * q * plane->rightWing.surfaceArea * DIHEDRAL_EFFECT_COEFF * LEVER_AILERON * controlScale;
	// Wing-leveling: combined dihedral and pendular stability produces a restoring
	// roll torque when banked, giving natural tendency to return to wings-level.
	rollTorque -= sinf(plane->bankAngle) * BANK_RESTORE_COEFF;

	// Pitch: elevator + tail AoA restoring moment.
	float pitchTorque = -elevDefl * q * plane->rightElevator.surfaceArea * plane->rightElevator.liftCoefficient * LEVER_ELEVATOR * controlScale;
	pitchTorque -= aoa_rad * q * 8.0f * controlScale;

	// Yaw: rudder + weathervane stability + adverse yaw from aileron differential drag.
	float yawTorque = rudDefl * q * plane->rudder.surfaceArea * plane->rudder.liftCoefficient * LEVER_RUDDER * controlScale;
	yawTorque += sideslip_rad * q * WEATHERVANE_COEFF * controlScale;
	yawTorque -= ailDefl * q * plane->rightAileron.surfaceArea * plane->rightAileron.dragCoefficient * LEVER_AILERON * ADVERSE_YAW_FACTOR * controlScale;

	// Engine gyroscopic precession: spinning turbine resists attitude changes.
	// Pitching creates yaw, yawing creates pitch (CW engine from pilot view).
	// Reduced from 6000 to avoid excessive cross-coupling that destabilises the model.
	float H_engine = 1500.0f * plane->currentTrustPercentage;
	pitchTorque -= plane->yawRate * H_engine;
	yawTorque += plane->pitchRate * H_engine;

	// Aerodynamic damping opposes rotation. Uses uncapped dampScale so damping
	// keeps growing with airspeed — matching the physical q-dependence of the
	// control forces and preventing instability at high speed.
	rollTorque -= plane->bankRate * DAMP_ROLL * dampScale;
	pitchTorque -= plane->pitchRate * DAMP_PITCH * dampScale;
	yawTorque -= plane->yawRate * DAMP_YAW * dampScale;

	// Integrate angular acceleration.
	plane->bankRate += (rollTorque / I_ROLL) * deltaTime;
	plane->pitchRate += (pitchTorque / I_PITCH) * deltaTime;
	plane->yawRate += (yawTorque / I_YAW) * deltaTime;

	fwd = f3Norm(f3Add(fwd, f3Scale(up_banked, plane->pitchRate * deltaTime)));
	fwd = f3Norm(f3Add(fwd, f3Scale(right_banked, plane->yawRate * deltaTime)));
	plane->bankAngle += plane->bankRate * deltaTime;

	// Banked-turn coupling: gravity component turns heading when banked.
	float clampedBank = fmaxf(-1.2f, fminf(1.2f, plane->bankAngle));
	float turnYaw = (9.81f / fmaxf(speed, 50.0f)) * tanf(clampedBank) * deltaTime;
	fwd = f3Norm(f3Add(fwd, f3Scale(right_banked, turnYaw)));

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
		calcForceMagnitudes(hSurf[i], q, mach, aoa_deg + hSurf[i]->rotationAngle, &lift, &drag);
		totalLift += lift;
		totalDrag += drag;
	}

	// Vertical surfaces: sideslip + surface deflection angle.
	Surface *vSurf[] = {&plane->verticalStabilizer, &plane->rudder};
	for (int i = 0; i < 2; i++) {
		float lift, drag;
		calcForceMagnitudes(vSurf[i], q, mach, sideslip_deg + vSurf[i]->rotationAngle, &lift, &drag);
		totalLateral -= lift;
		totalDrag += drag;
	}

	// Ailerons: rolling moment is already captured in rollTorque via ailDefl.
	// Add their combined lift (sum of both sides) and average drag to force totals.
	// Using 0.5 drag factor per side matches the intent of the previous drag-only code.
	{
		float liftL, dragL, liftR, dragR;
		calcForceMagnitudes(&plane->leftAileron, q, mach, aoa_deg + plane->leftAileron.rotationAngle, &liftL, &dragL);
		calcForceMagnitudes(&plane->rightAileron, q, mach, aoa_deg + plane->rightAileron.rotationAngle, &liftR, &dragR);
		totalLift += liftL + liftR;
		totalDrag += dragL * 0.5f + dragR * 0.5f;
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