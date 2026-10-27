#include "missile.h"
#include "flightControl.h"
#include "../../math/vector3.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MISSILE_CL_MAX 0.9f
#define MISSILE_OSWALD 0.6f
#define MISSILE_FIN_AR 1.0f
#define MISSILE_MIN_SPEED 50.0f

static void missileAtmosphere(float altitude, float *outDensity, float *outSpeedOfSound) {
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

static float missileWaveDrag(float mach) {
	if (mach < 0.8f) return 0.0f;
	if (mach < 1.2f) {
		float t = (mach - 0.8f) / 0.4f;
		return 0.5f * t * t * t;
	}
	return 0.3317f / sqrtf(fmaxf(mach * mach - 1.0f, 0.01f));
}

static float missileBurnTime(const MissileSpec *spec) {
	float total = 0.0f;
	for (int i = 0; i < spec->stageCount; i++)
		total += spec->stages[i].burnTime;
	return total > 0.0f ? total : 1.0f;
}

float missileFinAccelPerDeflection(const Missile *m) {
	float density, speedOfSound;
	missileAtmosphere(m->position.y, &density, &speedOfSound);
	float speed = Float3_Length(m->velocity);
	float mass = m->spec.emptyMass + m->fuelLeft;
	float areaDeg = m->spec.frontArea * m->spec.frontMaxDeg + m->spec.backArea * m->spec.backMaxDeg;
	return 0.5f * density * speed * speed * m->spec.cnAlpha * 0.01745f * areaDeg / mass;
}

static float slewTo(float current, float target, float maxStep) {
	float diff = target - current;
	if (diff > maxStep) return current + maxStep;
	if (diff < -maxStep) return current - maxStep;
	return target;
}

static float finCl(float cnAlpha, float deflection, float maxDeg) {
	float cl = cnAlpha * deflection * maxDeg * 0.01745f;
	return fmaxf(-MISSILE_CL_MAX, fminf(MISSILE_CL_MAX, cl));
}

int missileLoadBin(Missile *m, const char *path, float3 position, float3 velocity) {
	MissileSpec spec;
	int err = loadMissileBin(&spec, path);
	if (err) return err;

	memset(m, 0, sizeof(*m));
	m->spec = spec;
	m->position = position;
	m->velocity = velocity;
	m->forward = Float3_Normalize(velocity);
	m->fuelLeft = spec.propellantMass;
	m->phase = MISSILE_BOOST;
	return 0;
}

int missileUpdate(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime) {
	if (m->phase == MISSILE_DEAD) return 0;
	missileFlightControl(m, targetPosition, targetVelocity, deltaTime);
	return missileStep(m, targetPosition, targetVelocity, deltaTime);
}

int missileStep(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime) {
	if (m->phase == MISSILE_DEAD) return 0;

	m->timeAlive += deltaTime;

	float3 rel = Float3_Sub(targetPosition, m->position);
	float range = Float3_Length(rel);
	if (range <= m->spec.fuseRadius) {
		m->hit = 1;
		m->phase = MISSILE_DEAD;
		return 0;
	}

	float speed = Float3_Length(m->velocity);
	if (m->timeAlive >= m->spec.lifetime || speed < MISSILE_MIN_SPEED) {
		m->phase = MISSILE_DEAD;
		return 0;
	}

	// Normalized rate: a full deflection spans +/-maxDeg.
	m->frontPitchAct = slewTo(m->frontPitchAct, m->frontPitch, m->spec.frontRateDeg / fmaxf(m->spec.frontMaxDeg, 1.0f) * deltaTime);
	m->frontYawAct = slewTo(m->frontYawAct, m->frontYaw, m->spec.frontRateDeg / fmaxf(m->spec.frontMaxDeg, 1.0f) * deltaTime);
	m->backPitchAct = slewTo(m->backPitchAct, m->backPitch, m->spec.backRateDeg / fmaxf(m->spec.backMaxDeg, 1.0f) * deltaTime);
	m->backYawAct = slewTo(m->backYawAct, m->backYaw, m->spec.backRateDeg / fmaxf(m->spec.backMaxDeg, 1.0f) * deltaTime);
	m->thrustPitchAct = slewTo(m->thrustPitchAct, m->thrustPitch, m->spec.tvcRateDeg / fmaxf(m->spec.tvcMaxDeg, 1.0f) * deltaTime);
	m->thrustYawAct = slewTo(m->thrustYawAct, m->thrustYaw, m->spec.tvcRateDeg / fmaxf(m->spec.tvcMaxDeg, 1.0f) * deltaTime);

	// Propellant burns evenly over the total motor time; stages schedule thrust.
	while (m->stage < m->spec.stageCount && m->stageTime >= m->spec.stages[m->stage].burnTime) {
		m->stageTime -= m->spec.stages[m->stage].burnTime;
		m->stage++;
	}
	float thrust = 0.0f;
	if (m->fuelLeft > 0.0f && m->stage < m->spec.stageCount) {
		thrust = m->spec.stages[m->stage].thrust;
		float flow = m->spec.propellantMass / missileBurnTime(&m->spec);
		m->fuelLeft = fmaxf(0.0f, m->fuelLeft - flow * deltaTime);
		m->stageTime += deltaTime;
		m->phase = (m->stage == 0) ? MISSILE_BOOST : MISSILE_SUSTAIN;
	} else {
		m->phase = MISSILE_COAST;
	}
	float mass = m->spec.emptyMass + m->fuelLeft;

	// Fin force = q * area * CL per axis; both surfaces command nose-up positive.
	float3 fwd = Float3_Normalize(m->velocity);
	float3 rightRaw = Float3_Cross(fwd, (float3){0.0f, 1.0f, 0.0f, 0.0f});
	float3 right = (Float3_Length(rightRaw) < 1e-4f)
					   ? (float3){1.0f, 0.0f, 0.0f, 0.0f}
					   : Float3_Normalize(rightRaw);
	float3 up = Float3_Normalize(Float3_Cross(right, fwd));

	float altitude = m->position.y;
	float airDensity, speedOfSound;
	missileAtmosphere(altitude, &airDensity, &speedOfSound);
	float q = 0.5f * airDensity * speed * speed;
	float mach = speed / (speedOfSound + 1e-6f);

	float radius = 0.5f * m->spec.diameter;
	float refArea = (float)M_PI * radius * radius;
	m->forward = fwd;

	float clFP = finCl(m->spec.cnAlpha, m->frontPitchAct, m->spec.frontMaxDeg);
	float clBP = finCl(m->spec.cnAlpha, m->backPitchAct, m->spec.backMaxDeg);
	float clFY = finCl(m->spec.cnAlpha, m->frontYawAct, m->spec.frontMaxDeg);
	float clBY = finCl(m->spec.cnAlpha, m->backYawAct, m->spec.backMaxDeg);
	float nPitch = q * (m->spec.frontArea * clFP + m->spec.backArea * clBP);
	float nYaw = q * (m->spec.frontArea * clFY + m->spec.backArea * clBY);

	float3 accel = Float3_Add(Float3_Scale(up, nPitch / mass), Float3_Scale(right, nYaw / mass));
	float tvcRad = m->spec.tvcMaxDeg * 0.01745f;
	accel = Float3_Add(accel, Float3_Scale(up, thrust * m->thrustPitchAct * tvcRad / mass));
	accel = Float3_Add(accel, Float3_Scale(right, thrust * m->thrustYawAct * tvcRad / mass));

	float accelMag = Float3_Length(accel);
	float structLimit = m->spec.maxG * 9.81f;
	if (accelMag > structLimit && accelMag > 1e-6f) {
		accel = Float3_Scale(accel, structLimit / accelMag);
		accelMag = structLimit;
	}

	// Profile/wave drag + fin-induced drag, priced in fin-area terms.
	float cdInduced = (m->spec.frontArea * (clFP * clFP + clFY * clFY) +
					   m->spec.backArea * (clBP * clBP + clBY * clBY)) /
					  ((float)M_PI * MISSILE_FIN_AR * MISSILE_OSWALD);
	float dragMag = q * (refArea * (m->spec.cd0 + missileWaveDrag(mach)) + cdInduced);

	float3 force = Float3_Scale(fwd, thrust - dragMag);
	force = Float3_Add(force, Float3_Scale(accel, mass));
	force.y -= 9.81f * mass;

	m->velocity = Float3_Add(m->velocity, Float3_Scale(force, deltaTime / mass));
	m->position = Float3_Add(m->position, Float3_Scale(m->velocity, deltaTime));

	return 1;
}

void missileFlightControl(Missile *m, float3 targetPosition, float3 targetVelocity, float deltaTime) {
	MissileControllerOutput out = getControllerOutputMissile(m, targetPosition, targetVelocity, deltaTime);
	missileSetFrontPitch01(m, out.FrontPitch);
	missileSetFrontYaw01(m, out.FrontYaw);
	missileSetBackPitch01(m, out.BackPitch);
	missileSetBackYaw01(m, out.BackYaw);
	missileSetThrustPitch01(m, out.ThrustPitch);
	missileSetThrustYaw01(m, out.ThrustYaw);
}
