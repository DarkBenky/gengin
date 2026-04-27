#include "dense.h"
#include "import.h"
#include "simulate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
	Plane basePlane;
	loadPlane(&basePlane, "simulation/simModels/F-16C.bin");

	Model population[POPULATION];
	for (int i = 0; i < POPULATION; i++)
		population[i] = buildModel();

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
