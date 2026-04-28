#include "dense.h"
#include "import.h"
#include "simulate.h"
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "client/client.h"
#include <time.h>

#define WANDB_HOST "127.0.0.1"
#define WANDB_PORT 6789

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

static int wandbCheckAvailable(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 0;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(WANDB_PORT);
	inet_pton(AF_INET, WANDB_HOST, &addr.sin_addr);
	int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	close(fd);
	return ok;
}

static void postStats(const Client *c, int gen, float avgLoss, float bestLoss) {
	struct {
		int gen;
		float avgLoss;
		float bestLoss;
	} payload = {gen, avgLoss, bestLoss};
	ClientResponse r = clientPost(c, (const char *)&payload, sizeof(payload));
	clientFreeResponse(&r);
}

static float3 randomPointOnSphere(void) {
	float z = (float)rand() / RAND_MAX * 2.0f - 1.0f;
	float t = (float)rand() / RAND_MAX * 2.0f * 3.14159265f;
	float r = sqrtf(1.0f - z * z);
	return (float3){r * cosf(t), r * sinf(t), z, 0.0f};
}

static void initPlane(Plane *p) {
	if (loadPlaneBin(p, "simulation/simModels/F-16C.bin",
					 (float3){1.0f, 0.0f, 0.0f, 0.0f},
					 (float3){0.0f, 5000.0f, 0.0f, 0.0f},
					 220.0f, 0.5f) != 0) {
		fprintf(stderr, "failed to load simulation/simModels/F-16C.bin\n");
		exit(1);
	}
}

#define MODEL_SIZE 256

static Model buildModel() {
	Model model;
	InitModel(&model, INPUT_SIZE, OUTPUT_SIZE);
	AddDenseLayer(&model, INPUT_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, OUTPUT_SIZE, TANH);
	return model;
}

#define POPULATION 256
#define ELITE_COUNT (POPULATION / 10)
#define GENERATIONS 10000
#define SIM_STEPS 256
#define N_EVAL_TARGETS 16
#define DT 0.1f
#define MUTATION_RATE_START 0.1f
#define MUTATION_RATE 0.01f
#define STAGNATION_GENS (GENERATIONS / 100)
#define STAGNATION_BOOST 4.0f

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

float scaleMutationRate(int generation) {
	float progress = (float)generation / GENERATIONS;
	return MUTATION_RATE_START * (1.0f - progress) + MUTATION_RATE * progress;
}

int main(void) {
	Client wandbClient = {WANDB_HOST, WANDB_PORT};
	int wandbEnabled = wandbCheckAvailable();
	if (wandbEnabled)
		printf("wandb logger connected on port %d\n", WANDB_PORT);
	else
		printf("wandb server not found, stats logged to stdout only\n");

	Plane basePlane;
	initPlane(&basePlane);

	Model population[POPULATION];
	for (int i = 0; i < POPULATION; i++)
		population[i] = buildModel();

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
	float bestLossEver = 2.0f;
	int stagnationCount = 0;

	// Fixed targets: same set every generation so selection pressure accumulates.
	float3 evalTargets[N_EVAL_TARGETS];
	for (int t = 0; t < N_EVAL_TARGETS; t++)
		evalTargets[t] = randomPointOnSphere();

	for (int gen = 0; gen < GENERATIONS; gen++) {
		float totalInferenceTime = 0.0f;
		int totalSteps = 0;
		for (int i = 0; i < POPULATION; i++) {
			float totalLoss = 0.0f;
			for (int t = 0; t < N_EVAL_TARGETS; t++) {
				Plane plane = basePlane;
				for (int s = 0; s < SIM_STEPS; s++) {
					clock_t start = clock();
					runInference(&population[i], &plane, &evalTargets[t]);
					clock_t end = clock();
					totalInferenceTime += (double)(end - start) / CLOCKS_PER_SEC;
					totalSteps++;
					updatePlane(&plane, DT, NULL);
				}
				totalLoss += lossFunc(&plane, &evalTargets[t]);
			}
			losses[i] = totalLoss / N_EVAL_TARGETS;
		}
		printf("avg inference time: %.4f ms\n", (totalInferenceTime / totalSteps) * 1000.0f);

		RankedModel ranked[POPULATION];
		for (int i = 0; i < POPULATION; i++) {
			ranked[i].loss = losses[i];
			ranked[i].idx = i;
		}
		qsort(ranked, POPULATION, sizeof(RankedModel), cmpRanked);

		int eliteIdx[ELITE_COUNT];
		int isElite[POPULATION];
		for (int i = 0; i < POPULATION; i++)
			isElite[i] = 0;
		for (int e = 0; e < ELITE_COUNT; e++) {
			eliteIdx[e] = ranked[e].idx;
			isElite[ranked[e].idx] = 1;
		}

		float totalLoss = 0.0f;
		for (int i = 0; i < POPULATION; i++)
			totalLoss += losses[i];
		float avgLoss = totalLoss / POPULATION;
		float bestLoss = losses[eliteIdx[0]];

		if (bestLoss < bestLossEver - 1e-5f) {
			bestLossEver = bestLoss;
			stagnationCount = 0;
		} else {
			stagnationCount++;
		}

		float mutationRate = scaleMutationRate(gen);
		if (stagnationCount >= STAGNATION_GENS) {
			mutationRate *= STAGNATION_BOOST;
			stagnationCount = 0;
		}

		printf("gen %4d  avg_loss %.4f  best_loss %.4f  mutation_rate %.7f  stagnation %d\n", gen, avgLoss, bestLoss, mutationRate, stagnationCount);
		if (wandbEnabled)
			postStats(&wandbClient, gen, avgLoss, bestLoss);

		if (SaveModel(&population[eliteIdx[0]], MODEL_PATH) != 0)
			fprintf(stderr, "warning: failed to save model to %s\n", MODEL_PATH);

		for (int i = 0; i < POPULATION; i++) {
			if (isElite[i]) continue;
			int parent = eliteIdx[rand() % ELITE_COUNT];
			CopyModel(&population[i], &population[parent]);
			MutateModel(&population[i], mutationRate);
		}
	}

	for (int i = 0; i < POPULATION; i++)
		FreeModel(&population[i]);

	return 0;
}
