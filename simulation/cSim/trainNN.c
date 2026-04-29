#include "dense.h"
#include "import.h"
#include "simulate.h"
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "client/client.h"
#include <time.h>

#define WANDB_HOST "127.0.0.1"
#define WANDB_PORT 6789

#define GAME_SERVER_HOST "127.0.0.1"
#define GAME_SERVER_PORT 8080

#define MAX_SPEED    700.0f
#define MAX_ALTITUDE 25000.0f
#define MAX_DIST     20000.0f   // max distance used for normalization (meters)
#define TARGET_RADIUS 300.0f   // consider target reached within this radius
#define INPUT_SIZE  9
#define OUTPUT_SIZE 5

// IDs for the three visualization cubes posted to the game server.
// Upper 8 bits = ModelType (3=plane, 4=target, 5=start), lower 24 bits = fixed instance.
#define VIS_ID_PLANE  ((3u << 24) | 0x000001u)
#define VIS_ID_TARGET ((4u << 24) | 0x000001u)
#define VIS_ID_START  ((5u << 24) | 0x000001u)

typedef struct {
	float Speed;
	float Altitude;
	float3 currentForward;
	float3 dirToTarget; // normalized direction toward target position
	float distToTarget; // distance to target in meters
} Inputs;

typedef struct {
	float Aileron;
	float Elevator;
	float Rudder;
	float Flap;
	float Throttle;
} Outputs;

static void loadInputs(Inputs *inputs, const Plane *plane, const float3 *target) {
	inputs->Speed    = plane->currentSpeed;
	inputs->Altitude = plane->currentAltitude;
	inputs->currentForward = plane->forward;

	float dx = target->x - plane->position.x;
	float dy = target->y - plane->position.y;
	float dz = target->z - plane->position.z;
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	inputs->distToTarget = dist;
	if (dist > 1e-6f)
		inputs->dirToTarget = (float3){dx / dist, dy / dist, dz / dist, 0.0f};
	else
		inputs->dirToTarget = (float3){1.0f, 0.0f, 0.0f, 0.0f};
}

static void normalizeInputs(const Inputs *inputs, float buf[INPUT_SIZE]) {
	buf[0] = inputs->currentForward.x;
	buf[1] = inputs->currentForward.y;
	buf[2] = inputs->currentForward.z;
	buf[3] = inputs->dirToTarget.x;
	buf[4] = inputs->dirToTarget.y;
	buf[5] = inputs->dirToTarget.z;
	buf[6] = fminf(inputs->distToTarget / MAX_DIST, 1.0f) * 2.0f - 1.0f;
	buf[7] = (inputs->Speed / MAX_SPEED) * 2.0f - 1.0f;
	buf[8] = (inputs->Altitude / MAX_ALTITUDE) * 2.0f - 1.0f;
}

static void readOutputs(const float raw[OUTPUT_SIZE], Outputs *out) {
	out->Aileron   = raw[0];
	out->Elevator  = raw[1];
	out->Rudder    = raw[2];
	out->Flap      = raw[3];
	out->Throttle  = (raw[4] + 1.0f) * 0.5f;
}

static void applyOutputs(Plane *plane, const Outputs *out) {
	planeSetAileron(plane, out->Aileron);
	planeSetElevator(plane, out->Elevator);
	planeSetRudder(plane, out->Rudder);
	planeSetFlap(plane, out->Flap);
	planeSetThrottle(plane, out->Throttle);
}

void runInference(Model *model, Plane *plane, const float3 *target) {
	Inputs inputs;
	loadInputs(&inputs, plane, target);
	float inBuf[INPUT_SIZE];
	float outBuf[OUTPUT_SIZE];
	normalizeInputs(&inputs, inBuf);
	Forward(model, inBuf, outBuf);
	Outputs outputs;
	readOutputs(outBuf, &outputs);
	applyOutputs(plane, &outputs);
}

static float f3Dist(float3 a, float3 b) {
	float dx = a.x - b.x;
	float dy = a.y - b.y;
	float dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Loss = average distance per step + time penalty for not reaching the target.
// avgDist is in meters; the time penalty is multiplied by MAX_DIST * 0.1 (~2000 m)
// so both terms are on the same order of magnitude when the plane is mid-range.
static float evaluateEpisode(Model *model, Plane *plane, const float3 *target, int simSteps, float dt) {
	float totalDist = 0.0f;
	int reachedStep = simSteps; // step at which plane first entered TARGET_RADIUS

	for (int s = 0; s < simSteps; s++) {
		runInference(model, plane, target);
		updatePlane(plane, dt, NULL);
		float dist = f3Dist(plane->position, *target);
		totalDist += dist;
		if (reachedStep == simSteps && dist < TARGET_RADIUS)
			reachedStep = s + 1;
	}

	float avgDist     = totalDist / simSteps;
	float timePenalty = (float)reachedStep / simSteps; // 0 = reached immediately, 1 = never
	return avgDist + MAX_DIST * 0.1f * timePenalty;
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

static void postStats(const Client *c, int gen, float avgLoss, float bestLoss, float bestEver, float avgDist, float bestDist) {
	struct {
		int gen;
		float avgLoss;
		float bestLoss;
		float bestEver;
		float avgDist;
		float bestDist;
	} payload = {gen, avgLoss, bestLoss, bestEver, avgDist, bestDist};
	ClientResponse r = clientPost(c, (const char *)&payload, sizeof(payload));
	clientFreeResponse(&r);
}

// Generates a random 3-D target position in a realistic flight envelope.
// The plane starts at altitude 5000 m, so targets span a wide volume.
static float3 randomTargetPosition(void) {
	float x = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 8000.0f;
	float y = 1500.0f + (float)rand() / RAND_MAX * 7000.0f;
	float z = 1000.0f + (float)rand() / RAND_MAX * 12000.0f;
	return (float3){x, y, z, 0.0f};
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

#define MODEL_SIZE 32

static Model buildModel() {
	Model model;
	InitModel(&model, INPUT_SIZE, OUTPUT_SIZE);
	AddDenseLayer(&model, INPUT_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, MODEL_SIZE, RELU);
	AddDenseLayer(&model, MODEL_SIZE, OUTPUT_SIZE, TANH);
	return model;
}

#define POPULATION         256
#define ELITE_COUNT        (POPULATION / 10)
#define GENERATIONS        10000
#define SIM_STEPS          256
#define N_EVAL_TARGETS     16
#define N_VAL_TARGETS      64
#define DT                 0.1f
#define MUTATION_RATE_START 0.05f
#define MUTATION_RATE       0.001f
#define STAGNATION_GENS    (GENERATIONS / 100)
#define STAGNATION_BOOST   4.0f

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

// requestObject layout must match the game server's Object struct.
typedef struct {
	uint32 Id;
	uint32 TimeStamp;
	float3 Position;
	float3 Rotation;
	float3 Scale;
} VisObject;

static void postVisState(const Client *gameClient,
						 float3 planePos, float3 targetPos, float3 startPos) {
	VisObject objs[3] = {
		{VIS_ID_PLANE,  0, planePos,  {0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 200.0f, 200.0f, 0.0f}},
		{VIS_ID_TARGET, 0, targetPos, {0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 200.0f, 200.0f, 0.0f}},
		{VIS_ID_START,  0, startPos,  {0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 200.0f, 200.0f, 0.0f}},
	};
	uint32 n = 3;
	size_t sz = sizeof(uint32) + sizeof(objs);
	char *buf = malloc(sz);
	if (!buf) {
		fprintf(stderr, "[vis] postVisState: out of memory\n");
		return;
	}
	memcpy(buf, &n, sizeof(uint32));
	memcpy(buf + sizeof(uint32), objs, sizeof(objs));
	ClientResponse r = clientPost(gameClient, buf, (uint32)sz);
	free(buf);
	clientFreeResponse(&r);
}

static int gameServerAvailable(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 0;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(GAME_SERVER_PORT);
	inet_pton(AF_INET, GAME_SERVER_HOST, &addr.sin_addr);
	int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	close(fd);
	return ok;
}

int main(void) {
	Client wandbClient = {WANDB_HOST, WANDB_PORT};
	int wandbEnabled = wandbCheckAvailable();
	if (wandbEnabled)
		printf("wandb logger connected on port %d\n", WANDB_PORT);
	else
		printf("wandb server not found, stats logged to stdout only\n");

	Client gameClient = {GAME_SERVER_HOST, GAME_SERVER_PORT};
	int visEnabled = gameServerAvailable();
	if (visEnabled)
		printf("game server connected on port %d — visualization enabled\n", GAME_SERVER_PORT);
	else
		printf("game server not found — visualization disabled\n");

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
	float bestValLoss = FLT_MAX;
	int stagnationCount = 0;

	// Fixed validation set — never resampled, used only for saving decisions.
	float3 valTargets[N_VAL_TARGETS];
	for (int t = 0; t < N_VAL_TARGETS; t++)
		valTargets[t] = randomTargetPosition();

	float3 evalTargets[N_EVAL_TARGETS];

	for (int gen = 0; gen < GENERATIONS; gen++) {
		// Resample targets every generation to force generalization.
		for (int t = 0; t < N_EVAL_TARGETS; t++)
			evalTargets[t] = randomTargetPosition();

		float totalInferenceTime = 0.0f;
		int totalSteps = 0;
		for (int i = 0; i < POPULATION; i++) {
			float totalLoss = 0.0f;
			for (int t = 0; t < N_EVAL_TARGETS; t++) {
				Plane plane = basePlane;
				clock_t start = clock();
				float epLoss = evaluateEpisode(&population[i], &plane, &evalTargets[t], SIM_STEPS, DT);
				clock_t end = clock();
				totalInferenceTime += (double)(end - start) / CLOCKS_PER_SEC;
				totalSteps += SIM_STEPS;
				totalLoss += epLoss;
			}
			losses[i] = totalLoss / N_EVAL_TARGETS;
		}
		printf("avg inference time: %.4f ms\n", (totalInferenceTime / totalSteps) * 1000.0f);

		RankedModel ranked[POPULATION];
		for (int i = 0; i < POPULATION; i++) {
			ranked[i].loss = losses[i];
			ranked[i].idx  = i;
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
		float avgLoss  = totalLoss / POPULATION;
		float bestLoss = losses[eliteIdx[0]];

		// Evaluate best model on the fixed validation set.
		float valLoss = 0.0f;
		for (int t = 0; t < N_VAL_TARGETS; t++) {
			Plane plane = basePlane;
			float epLoss = evaluateEpisode(&population[eliteIdx[0]], &plane, &valTargets[t], SIM_STEPS, DT);
			valLoss += epLoss;
		}
		valLoss /= N_VAL_TARGETS;

		if (valLoss < bestValLoss - 1e-5f) {
			bestValLoss = valLoss;
			stagnationCount = 0;
			if (SaveModel(&population[eliteIdx[0]], MODEL_PATH) != 0)
				fprintf(stderr, "warning: failed to save model to %s\n", MODEL_PATH);
		} else {
			stagnationCount++;
		}

		// Post visualization state: replay best model on first validation target.
		if (visEnabled) {
			Plane vizPlane = basePlane;
			float3 vizTarget = valTargets[0];
			float3 startPos  = vizPlane.position;
			for (int s = 0; s < SIM_STEPS; s++) {
				runInference(&population[eliteIdx[0]], &vizPlane, &vizTarget);
				updatePlane(&vizPlane, DT, NULL);
			}
			postVisState(&gameClient, vizPlane.position, vizTarget, startPos);
		}

		// avgMetric/bestMetric include both the distance and time-penalty components.
		float avgMetric  = avgLoss;
		float bestMetric = bestLoss;

		float mutationRate = scaleMutationRate(gen);
		int boosted = 0;
		if (stagnationCount >= STAGNATION_GENS) {
			mutationRate *= STAGNATION_BOOST;
			stagnationCount = 0;
			boosted = 1;
		}

		printf("gen %4d  avgLoss=%.1f  bestLoss=%.1f  val=%.1f  stagnation=%d%s\n",
			   gen, avgMetric, bestMetric, bestValLoss, stagnationCount, boosted ? " [BOOST]" : "");
		if (wandbEnabled)
			postStats(&wandbClient, gen, avgLoss, bestLoss, bestValLoss, avgMetric, bestMetric);

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

