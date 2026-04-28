#include "dense.h"
#include "import.h"
#include "simulate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPEED 700.0f
#define MAX_ALTITUDE 25000.0f
#define INPUT_SIZE 8
#define OUTPUT_SIZE 5

typedef struct {
	float Speed;
	float Altitude;
	float3 currentForward;
	float3 targetForward;
} Inputs;

typedef struct {
	float Aileron;
	float Elevator;
	float Rudder;
	float Flap;
	float Throttle;
} Outputs;

static void loadInputs(Inputs *inputs, const Plane *plane, const float3 *targetForward) {
	inputs->Speed = plane->currentSpeed;
	inputs->Altitude = plane->currentAltitude;
	inputs->currentForward = plane->forward;
	inputs->targetForward = *targetForward;
}

static void normalizeInputs(const Inputs *inputs, float buf[INPUT_SIZE]) {
	buf[0] = inputs->currentForward.x;
	buf[1] = inputs->currentForward.y;
	buf[2] = inputs->currentForward.z;
	buf[3] = inputs->targetForward.x;
	buf[4] = inputs->targetForward.y;
	buf[5] = inputs->targetForward.z;
	buf[6] = (inputs->Speed / MAX_SPEED) * 2.0f - 1.0f;
	buf[7] = (inputs->Altitude / MAX_ALTITUDE) * 2.0f - 1.0f;
}

static void readOutputs(const float raw[OUTPUT_SIZE], Outputs *out) {
	out->Aileron = raw[0];
	out->Elevator = raw[1];
	out->Rudder = raw[2];
	out->Flap = raw[3];
	out->Throttle = (raw[4] + 1.0f) * 0.5f;
}

static void applyOutputs(Plane *plane, const Outputs *out) {
	planeSetAileron(plane, out->Aileron);
	planeSetElevator(plane, out->Elevator);
	planeSetRudder(plane, out->Rudder);
	planeSetFlap(plane, out->Flap);
	planeSetThrottle(plane, out->Throttle);
}

void runInference(Model *model, Plane *plane, const float3 *targetForward) {
	Inputs inputs;
	loadInputs(&inputs, plane, targetForward);
	float inBuf[INPUT_SIZE];
	float outBuf[OUTPUT_SIZE];
	normalizeInputs(&inputs, inBuf);
	Forward(model, inBuf, outBuf);
	Outputs outputs;
	readOutputs(outBuf, &outputs);
	applyOutputs(plane, &outputs);
}

static float f3Dot(float3 a, float3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float lossFunc(const Plane *plane, const float3 *targetForward) {
	return 1.0f - f3Dot(plane->forward, *targetForward);
}

static float3 randomPointOnSphere(void) {
	float z = (float)rand() / RAND_MAX * 2.0f - 1.0f;
	float t = (float)rand() / RAND_MAX * 2.0f * 3.14159265f;
	float r = sqrtf(1.0f - z * z);
	return (float3){r * cosf(t), r * sinf(t), z, 0.0f};
}

static Surface makeSurface(float3 pos, float3 axis, float area, float cl, float cd,
                            float ar, float eff, float stall, float camber,
                            float minAng, float maxAng, float rate, bool active) {
	Surface s;
	memset(&s, 0, sizeof(s));
	s.relativePos = pos;
	s.rotationAxis = axis;
	s.surfaceArea = area;
	s.liftCoefficient = cl;
	s.dragCoefficient = cd;
	s.aspectRatio = ar;
	s.efficiency = eff;
	s.stallAngle = stall;
	s.camber = camber;
	s.minRotationAngle = minAng;
	s.maxRotationAngle = maxAng;
	s.rotationRate = rate;
	s.active = active;
	s.rotationAngle = 0.0f;
	s.targetRotationAngle = 0.0f;
	return s;
}

static void initPlane(Plane *p) {
	memset(p, 0, sizeof(*p));
	strncpy(p->name, "F-16C", sizeof(p->name) - 1);

	p->forward           = (float3){1.0f, 0.0f, 0.0f, 0.0f};
	p->position          = (float3){0.0f, 5000.0f, 0.0f, 0.0f};
	p->rotation          = (float3){0.0f, 0.0f, 0.0f, 0.0f};
	p->currentSpeed      = 220.0f;   // m/s, ~0.65 Mach at 5000m
	p->currentAltitude   = 5000.0f;
	p->baseMass          = 9207.0f;  // kg, F-16 empty weight
	p->fuelMass          = 3162.0f;  // kg, internal fuel
	p->currentFuelPercentage = 1.0f;
	p->maxTrust          = 129000.0f; // N, F110-GE-129 with afterburner
	p->currentTrustPercentage = 0.5f;
	p->burnRate          = 2.8f;     // kg/s at full afterburner
	p->burnWithoutAfterburner = 0.9f;

	float3 yAxis = {0.0f, 1.0f, 0.0f, 0.0f};
	float3 zAxis = {0.0f, 0.0f, 1.0f, 0.0f};

	// Wings (large, fixed, provide main lift)
	p->leftWing  = makeSurface((float3){0.0f,  0.0f, -3.5f, 0.0f}, yAxis,
	                           13.9f, 1.6f, 0.02f, 3.0f, 0.85f, 18.0f, 0.04f,  0.0f, 0.0f, 0.0f, false);
	p->rightWing = makeSurface((float3){0.0f,  0.0f,  3.5f, 0.0f}, yAxis,
	                           13.9f, 1.6f, 0.02f, 3.0f, 0.85f, 18.0f, 0.04f,  0.0f, 0.0f, 0.0f, false);

	// Horizontal stabilizer (fixed pitch trim surface)
	p->horizontalStabilizer = makeSurface((float3){-4.5f, 0.0f, 0.0f, 0.0f}, yAxis,
	                                      3.9f, 1.2f, 0.02f, 2.5f, 0.80f, 20.0f, 0.0f,  0.0f, 0.0f, 0.0f, false);

	// Vertical stabilizer (fixed yaw stability)
	p->verticalStabilizer   = makeSurface((float3){-4.5f, 0.5f, 0.0f, 0.0f}, zAxis,
	                                      5.1f, 1.0f, 0.02f, 1.5f, 0.75f, 20.0f, 0.0f,  0.0f, 0.0f, 0.0f, false);

	// Ailerons (roll control)
	p->leftAileron  = makeSurface((float3){-0.5f, 0.0f, -4.5f, 0.0f}, yAxis,
	                              1.5f, 1.2f, 0.025f, 4.0f, 0.82f, 25.0f, 0.0f, -25.0f, 25.0f, 60.0f, true);
	p->rightAileron = makeSurface((float3){-0.5f, 0.0f,  4.5f, 0.0f}, yAxis,
	                              1.5f, 1.2f, 0.025f, 4.0f, 0.82f, 25.0f, 0.0f, -25.0f, 25.0f, 60.0f, true);

	// Elevators (pitch control)
	p->leftElevator  = makeSurface((float3){-4.5f, 0.0f, -1.8f, 0.0f}, yAxis,
	                               1.95f, 1.4f, 0.02f, 2.5f, 0.80f, 25.0f, 0.0f, -25.0f, 25.0f, 80.0f, true);
	p->rightElevator = makeSurface((float3){-4.5f, 0.0f,  1.8f, 0.0f}, yAxis,
	                               1.95f, 1.4f, 0.02f, 2.5f, 0.80f, 25.0f, 0.0f, -25.0f, 25.0f, 80.0f, true);

	// Rudder (yaw control)
	p->rudder = makeSurface((float3){-4.8f, 1.0f, 0.0f, 0.0f}, zAxis,
	                        2.0f, 1.0f, 0.025f, 1.5f, 0.75f, 25.0f, 0.0f, -30.0f, 30.0f, 60.0f, true);

	// Flaps (lift augmentation)
	p->leftFlap  = makeSurface((float3){-0.2f, 0.0f, -2.5f, 0.0f}, yAxis,
	                           1.5f, 1.8f, 0.04f, 2.5f, 0.78f, 15.0f, 0.06f, 0.0f, 40.0f, 30.0f, true);
	p->rightFlap = makeSurface((float3){-0.2f, 0.0f,  2.5f, 0.0f}, yAxis,
	                           1.5f, 1.8f, 0.04f, 2.5f, 0.78f, 15.0f, 0.06f, 0.0f, 40.0f, 30.0f, true);
}


static Model buildModel(void) {
	Model model;
	InitModel(&model, INPUT_SIZE, OUTPUT_SIZE);
	AddDenseLayer(&model, INPUT_SIZE, 16, RELU);
	AddDenseLayer(&model, 16, 16, RELU);
	AddDenseLayer(&model, 16, 16, RELU);
	AddDenseLayer(&model, 16, OUTPUT_SIZE, TANH);
	return model;
}

#define POPULATION 512
#define ELITE_COUNT (POPULATION / 10)
#define GENERATIONS 10000
#define SIM_STEPS 64
#define DT 0.1f
#define MUTATION_RATE 0.05f

typedef struct {
	float loss;
	int idx;
} RankedModel;

static int cmpRanked(const void *a, const void *b) {
	float la = ((const RankedModel *)a)->loss;
	float lb = ((const RankedModel *)b)->loss;
	return (la > lb) - (la < lb);
}

#define MODEL_PATH "simulation/model.bin"

int main(void) {
	Plane basePlane;
	initPlane(&basePlane);

	Model population[POPULATION];
	for (int i = 0; i < POPULATION; i++)
		population[i] = buildModel();

	// Try to seed population from a saved checkpoint
	Model seed = {0};
	if (LoadModel(&seed, MODEL_PATH) == 0) {
		printf("Loaded checkpoint from %s\n", MODEL_PATH);
		for (int i = 0; i < POPULATION; i++) {
			CopyModel(&population[i], &seed);
			if (i > 0)
				MutateModel(&population[i], MUTATION_RATE);
		}
		FreeModel(&seed);
	}

	float losses[POPULATION];

	for (int gen = 0; gen < GENERATIONS; gen++) {
		float3 target = randomPointOnSphere();

		for (int i = 0; i < POPULATION; i++) {
			Plane plane = basePlane;
			for (int s = 0; s < SIM_STEPS; s++) {
				runInference(&population[i], &plane, &target);
				updatePlane(&plane, DT, NULL);
			}
			losses[i] = lossFunc(&plane, &target);
		}

		RankedModel ranked[POPULATION];
		for (int i = 0; i < POPULATION; i++) {
			ranked[i].loss = losses[i];
			ranked[i].idx = i;
		}
		qsort(ranked, POPULATION, sizeof(RankedModel), cmpRanked);

		int eliteIdx[ELITE_COUNT];
		int isElite[POPULATION];
		for (int i = 0; i < POPULATION; i++) isElite[i] = 0;
		for (int e = 0; e < ELITE_COUNT; e++) {
			eliteIdx[e] = ranked[e].idx;
			isElite[ranked[e].idx] = 1;
		}

		float totalLoss = 0.0f;
		for (int i = 0; i < POPULATION; i++) totalLoss += losses[i];
		printf("gen %4d  avg_loss %.4f  best_loss %.4f\n",
			   gen, totalLoss / POPULATION, losses[eliteIdx[0]]);

		if (SaveModel(&population[eliteIdx[0]], MODEL_PATH) != 0)
			fprintf(stderr, "warning: failed to save model to %s\n", MODEL_PATH);

		for (int i = 0; i < POPULATION; i++) {
			if (isElite[i]) continue;
			int parent = eliteIdx[rand() % ELITE_COUNT];
			CopyModel(&population[i], &population[parent]);
			MutateModel(&population[i], MUTATION_RATE);
		}
	}

	for (int i = 0; i < POPULATION; i++)
		FreeModel(&population[i]);

	return 0;
}
