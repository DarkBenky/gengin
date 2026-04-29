#include "dense.h"
#include "import.h"
#include "simulate.h"
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "client/client.h"

#define WANDB_HOST "127.0.0.1"
#define WANDB_PORT 6789

#define GAME_SERVER_HOST "127.0.0.1"
#define GAME_SERVER_PORT 8080

#define MAX_SPEED 700.0f
#define MAX_ALTITUDE 25000.0f
#define MAX_DIST 20000.0f	 // max distance used for normalization (meters)
#define TARGET_RADIUS 300.0f // consider target reached within this radius
#define INPUT_SIZE 10
#define OUTPUT_SIZE 5

// IDs for the three visualization cubes posted to the game server.
// Upper 8 bits = ModelType (3=plane, 4=target, 5=start), lower 24 bits = fixed instance.
#define VIS_ID_PLANE ((3u << 24) | 0x000001u)
#define VIS_ID_TARGET ((4u << 24) | 0x000001u)
#define VIS_ID_START ((5u << 24) | 0x000001u)

// Replay animation: post every VIS_STRIDE steps, wait VIS_FRAME_US between frames.
#define VIS_STRIDE 8
#define VIS_FRAME_US 40000

typedef struct {
	float Speed;
	float Altitude;
	float bankAngle;
	float3 currentForward;
	float3 dirToTarget;
	float distToTarget;
} Inputs;

typedef struct {
	float Aileron;
	float Elevator;
	float Rudder;
	float Flap;
	float Throttle;
} Outputs;

static void loadInputs(Inputs *inputs, const Plane *plane, const float3 *target) {
	inputs->Speed = plane->currentSpeed;
	inputs->Altitude = plane->currentAltitude;
	inputs->bankAngle = plane->bankAngle;
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
	buf[9] = inputs->bankAngle / (float)M_PI; // normalized to [-1, 1] for 180-deg range
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

// Returns a loss that rewards reaching the target AND reaching it quickly.
// Early arrival → loss near 0. Never arriving → final distance (>> TARGET_RADIUS).
static float evaluateEpisode(Model *model, Plane *plane, const float3 *target, int simSteps, float dt) {
	for (int s = 0; s < simSteps; s++) {
		runInference(model, plane, target);
		updatePlane(plane, dt, NULL);
		if (f3Dist(plane->position, *target) < TARGET_RADIUS)
			return TARGET_RADIUS * (float)(s + 1) / simSteps;
	}
	return f3Dist(plane->position, *target);
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

static void postStats(const Client *c, int gen, float avgDist, float bestDist, float valDist) {
	struct {
		int gen;
		float avgDist;
		float bestDist;
		float valDist;
	} payload = {gen, avgDist, bestDist, valDist};
	ClientResponse r = clientPost(c, (const char *)&payload, sizeof(payload));
	clientFreeResponse(&r);
}

static float3 randomTargetPosition(void) {
	float x = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 8000.0f;
	float y = 1500.0f + (float)rand() / RAND_MAX * 7000.0f;
	float z = 1000.0f + (float)rand() / RAND_MAX * 12000.0f;
	return (float3){x, y, z, 0.0f};
}

static float3 randomStartPosition(void) {
	float x = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 4000.0f;
	float y = 3000.0f + (float)rand() / RAND_MAX * 4000.0f;
	float z = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * 4000.0f;
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

#define POPULATION 256
#define ELITE_COUNT (POPULATION / 10)
#define GENERATIONS 10000
#define SIM_STEPS 1024
#define N_EVAL_TARGETS 16
#define N_VAL_TARGETS 64
#define DT 0.1f
#define MUTATION_RATE_START 0.05f
#define MUTATION_RATE 0.001f
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

static void postVisState(const Client *, float3, float3, float3, float3);

typedef struct {
	Client client;
	float3 targetPos;
	float3 startPos;
	uint32 frameCount;
	float3 *positions; // [frameCount]
	float3 *rotations; // [frameCount]
} ReplayThreadArg;

static void *replayThread(void *arg) {
	ReplayThreadArg *r = arg;
	for (uint32 i = 0; i < r->frameCount; i++) {
		postVisState(&r->client, r->positions[i], r->rotations[i], r->targetPos, r->startPos);
		usleep(VIS_FRAME_US);
	}
	free(r->positions);
	free(r->rotations);
	free(r);
	return NULL;
}

static void spawnReplayAsync(const Client *gameClient,
							 float3 *positions, float3 *rotations, uint32 frameCount,
							 float3 targetPos, float3 startPos) {
	ReplayThreadArg *arg = malloc(sizeof(ReplayThreadArg));
	if (!arg) {
		free(positions);
		free(rotations);
		return;
	}
	arg->client = *gameClient;
	arg->targetPos = targetPos;
	arg->startPos = startPos;
	arg->frameCount = frameCount;
	arg->positions = positions;
	arg->rotations = rotations;
	pthread_t tid;
	if (pthread_create(&tid, NULL, replayThread, arg) != 0) {
		free(positions);
		free(rotations);
		free(arg);
		return;
	}
	pthread_detach(tid);
}

typedef struct {
	uint32 Id;
	uint32 TimeStamp;
	float3 Position;
	float3 Rotation;
	float3 Scale;
} VisObject;

static void postVisState(const Client *gameClient,
						 float3 planePos, float3 planeRot, float3 targetPos, float3 startPos) {
	VisObject objs[3] = {
		{VIS_ID_PLANE, 0, planePos, planeRot, {200.0f, 200.0f, 200.0f, 0.0f}},
		{VIS_ID_TARGET, 0, targetPos, {0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 200.0f, 200.0f, 0.0f}},
		{VIS_ID_START, 0, startPos, {0.0f, 0.0f, 0.0f, 0.0f}, {200.0f, 200.0f, 200.0f, 0.0f}},
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
	if (gameServerAvailable())
		printf("game server connected on port %d — visualization enabled\n", GAME_SERVER_PORT);
	else
		printf("game server not found — will retry each generation\n");

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

	float3 valTargets[N_VAL_TARGETS];
	float3 valStarts[N_VAL_TARGETS];
	for (int t = 0; t < N_VAL_TARGETS; t++) {
		valTargets[t] = randomTargetPosition();
		valStarts[t] = randomStartPosition();
	}

	float3 evalTargets[N_EVAL_TARGETS];
	float3 evalStarts[N_EVAL_TARGETS];

	for (int gen = 0; gen < GENERATIONS; gen++) {
		for (int t = 0; t < N_EVAL_TARGETS; t++) {
			evalTargets[t] = randomTargetPosition();
			evalStarts[t] = randomStartPosition();
		}

		for (int i = 0; i < POPULATION; i++) {
			float totalLoss = 0.0f;
			for (int t = 0; t < N_EVAL_TARGETS; t++) {
				Plane plane = basePlane;
				plane.position = evalStarts[t];
				plane.currentAltitude = evalStarts[t].y;
				totalLoss += evaluateEpisode(&population[i], &plane, &evalTargets[t], SIM_STEPS, DT);
			}
			losses[i] = totalLoss / N_EVAL_TARGETS;
			// Post current best during training so objects stay alive in the viewer.
			if (i % (POPULATION / 4) == 0) {
				Plane vizPlane = basePlane;
				vizPlane.position = evalStarts[0];
				vizPlane.currentAltitude = evalStarts[0].y;
				postVisState(&gameClient, vizPlane.position, vizPlane.rotation, evalTargets[0], evalStarts[0]);
			}
		}

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

		// Evaluate best model on the fixed validation set.
		float valLoss = 0.0f;
		for (int t = 0; t < N_VAL_TARGETS; t++) {
			Plane plane = basePlane;
			plane.position = valStarts[t];
			plane.currentAltitude = valStarts[t].y;
			valLoss += evaluateEpisode(&population[eliteIdx[0]], &plane, &valTargets[t], SIM_STEPS, DT);
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

		// Simulate the full replay, collect frames, then hand off to a background thread.
		// Training continues immediately while the thread posts frames at VIS_FRAME_US pace.
		{
			Plane vizPlane = basePlane;
			vizPlane.position = valStarts[0];
			vizPlane.currentAltitude = valStarts[0].y;
			float3 vizTarget = valTargets[0];
			float3 startPos = vizPlane.position;
			uint32 frameCount = SIM_STEPS / VIS_STRIDE;
			float3 *positions = malloc(frameCount * sizeof(float3));
			float3 *rotations = malloc(frameCount * sizeof(float3));
			uint32 fi = 0;
			if (positions && rotations) {
				for (int s = 0; s < SIM_STEPS; s++) {
					runInference(&population[eliteIdx[0]], &vizPlane, &vizTarget);
					updatePlane(&vizPlane, DT, NULL);
					if ((s + 1) % VIS_STRIDE == 0)
						positions[fi] = vizPlane.position,
						rotations[fi++] = vizPlane.rotation;
				}
				spawnReplayAsync(&gameClient, positions, rotations, fi, vizTarget, startPos);
			} else {
				free(positions);
				free(rotations);
			}
		}

		float mutationRate = scaleMutationRate(gen);
		int boosted = 0;
		if (stagnationCount >= STAGNATION_GENS) {
			mutationRate *= STAGNATION_BOOST;
			stagnationCount = 0;
			boosted = 1;
		}

		printf("gen %4d  avg=%.0fm  best=%.0fm  val=%.0fm  stagnation=%d%s\n",
			   gen, avgLoss, bestLoss, bestValLoss, stagnationCount, boosted ? " [BOOST]" : "");
		if (wandbEnabled)
			postStats(&wandbClient, gen, avgLoss, bestLoss, bestValLoss);

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
