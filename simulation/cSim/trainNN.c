#include "trainNN.h"
#include <stdio.h>
#include "../../util/threadPool.h"
#include "../../util/fmt.h"

#define MAX_FLOAT 3.402823466e+38F
#define MODEL_NAME "F-16C"

typedef struct {
	ModelTrainer *trainer;
	Plane *plane;
	float *currentTop10PercentLoss;
} FunctionArgs;

void epochTask(void *args) {
	epoch(((FunctionArgs *)args)->trainer, ((FunctionArgs *)args)->plane, ((FunctionArgs *)args)->currentTop10PercentLoss);
}

#define NUM_THREADS 8

int main() {
	ModelTrainer trainers[NUM_THREADS];
	Plane planes[NUM_THREADS];
	float top10PercentLosses[NUM_THREADS];

	int epochs = 10000;
	int iterationCount = 2048;
	int ModelCount = 1024;
	uint16 port = 5174;

	// init trainers and planes
	for (int i = 0; i < NUM_THREADS; i++) {
		initModelTrainer(&trainers[i], ModelCount, epochs, iterationCount, 0.01f, 0.0005f, 4, 64, port + i);
		if (loadPlaneBin(&planes[i], "simulation/simModels/" MODEL_NAME ".bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 250.0f, 1.0f) != 0) {
			printf("Failed to load model: simulation/simModels/" MODEL_NAME ".bin\n");
			return 1;
		}
		top10PercentLosses[i] = MAX_FLOAT;
	}

	ThreadPool *pool = poolCreate(NUM_THREADS, NUM_THREADS);

	// load the best model if it exists
	Model seedModel = {0};
	int hasSeed = LoadModel(&seedModel, "simulation/best_model_" MODEL_NAME ".bin") == 0;
	if (hasSeed) {
		printf("Loaded existing best model — seeding all trainers\n");
		for (int i = 0; i < NUM_THREADS; i++)
			CopyModel(&trainers[i].models[0], &seedModel);
		FreeModel(&seedModel);
	}

	// load best backprop model if it exists
	Model seedBackpropModel = {0};
	int hasBackpropSeed = LoadModel(&seedBackpropModel, "simulation/best_backprop_model_" MODEL_NAME ".bin") == 0;
	if (hasBackpropSeed) {
		printf("Loaded existing best backprop model — seeding all trainers\n");
		for (int i = 0; i < NUM_THREADS; i++)
			CopyModel(&trainers[i].backpropModel, &seedBackpropModel);
		FreeModel(&seedBackpropModel);
	}

	float globalBestLoss = MAX_FLOAT;
	float globalBestBackpropLoss = MAX_FLOAT;

	int crossMutateInterval = 10; // epochs
	int crossMutateCount = 50;	  // bottom N models replaced per trainer per migration
	for (int epochIdx = 0; epochIdx < epochs; epochIdx++) {
		FunctionArgs args[NUM_THREADS];
		for (int i = 0; i < NUM_THREADS; i++) {
			args[i].trainer = &trainers[i];
			args[i].plane = &planes[i];
			args[i].currentTop10PercentLoss = &top10PercentLosses[i];
			poolAdd(pool, epochTask, &args[i]);
		}
		poolWait(pool);

		// reset per-trainer loss baseline on new scenario so stale easy-scenario
		// losses don't block saves after a harder scenario is introduced
		for (int i = 0; i < NUM_THREADS; i++) {
			if (trainers[i].epochsSinceLastTarget == 1) {
				top10PercentLosses[i] = MAX_FLOAT;
				globalBestLoss = MAX_FLOAT;
			}
		}

		// find the trainer with the best loss this epoch and save if improved
		int bestTrainer = 0;
		for (int i = 1; i < NUM_THREADS; i++) {
			if (top10PercentLosses[i] < top10PercentLosses[bestTrainer])
				bestTrainer = i;
		}
		if (top10PercentLosses[bestTrainer] < globalBestLoss) {
			globalBestLoss = top10PercentLosses[bestTrainer];
			uint32 bestModelIdx = trainers[bestTrainer].modelLossOrders[0];
			printf("Saving model with loss: %f (trainer %d, epoch %d)\n", globalBestLoss, bestTrainer, epochIdx + 1);
			SaveModel(&trainers[bestTrainer].models[bestModelIdx], "simulation/best_model_" MODEL_NAME ".bin");
		}

		// find trainer with best backprop EMA loss and save if improved
		int bestBackpropTrainer = 0;
		for (int i = 1; i < NUM_THREADS; i++) {
			if (trainers[i].backpropLossEMA < trainers[bestBackpropTrainer].backpropLossEMA)
				bestBackpropTrainer = i;
		}
		if (trainers[bestBackpropTrainer].backpropLossEMA < globalBestBackpropLoss) {
			globalBestBackpropLoss = trainers[bestBackpropTrainer].backpropLossEMA;
			printf("Saving backprop model with loss EMA: %f (trainer %d, epoch %d)\n", globalBestBackpropLoss, bestBackpropTrainer, epochIdx + 1);
			SaveModel(&trainers[bestBackpropTrainer].backpropModel, "simulation/best_backprop_model_" MODEL_NAME ".bin");
		}
		// cross mutate between trainers
		if ((epochIdx + 1) % crossMutateInterval == 0) {
			uint32 eliteIdx[NUM_THREADS];
			for (int i = 0; i < NUM_THREADS; i++)
				eliteIdx[i] = trainers[i].modelLossOrders[0];

			for (int i = 0; i < NUM_THREADS; i++) {
				int numModels = trainers[i].numModels;
				for (int n = 0; n < crossMutateCount; n++) {
					int parentAIdx = rand() % NUM_THREADS;
					int parentBIdx = rand() % NUM_THREADS;
					while (parentBIdx == parentAIdx)
						parentBIdx = rand() % NUM_THREADS;
					uint32 targetSlot = trainers[i].modelLossOrders[numModels - 1 - n];
					float crossMutationRate = calculateMutationRate(trainers[i].startMutationRate, trainers[i].endMutationRate, epochIdx, epochs);
					CrossoverModels(
						&trainers[i].models[targetSlot],
						&trainers[parentAIdx].models[eliteIdx[parentAIdx]],
						&trainers[parentBIdx].models[eliteIdx[parentBIdx]],
						crossMutationRate);
				}
			}
		}
		int completedEpochs = epochIdx + 1;
		int completedMigrations = completedEpochs / crossMutateInterval;
		long totalMutations = (long)NUM_THREADS * trainers[0].numModels * completedEpochs + (long)NUM_THREADS * crossMutateCount * completedMigrations;
		printf("Epoch %d | Total Mutations Explored: %s\n", completedEpochs, fmtInt(totalMutations));
	}

	// Single-threaded version:
	// ModelTrainer trainer;
	// int epochs = 10000;
	// int iterationCount = 2048;
	// uint16 port = 5173;
	// initModelTrainer(&trainer, 1024, epochs, iterationCount, 0.01f, 0.0005f, 2, 48, port);

	// Plane plane;
	// if (loadPlaneBin(&plane, "simulation/simModels/" MODEL_NAME ".bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 250.0f, 1.0f) != 0) {
	// 	printf("Failed to load model: simulation/simModels/" MODEL_NAME ".bin\n");
	// 	return 1;
	// }

	// float top10PercentLoss = MAX_FLOAT;
	// for (int epochIdx = 0; epochIdx < trainer.epochs; epochIdx++) {
	// 	float currentTop10PercentLoss;
	// 	epoch(&trainer, &plane, &currentTop10PercentLoss);
	// 	// reset baseline whenever a new target was generated so the save threshold is always relative to the current scenario, not a stale easier one
	// 	if (trainer.epochsSinceLastTarget == 1) {
	// 		top10PercentLoss = MAX_FLOAT;
	// 	}
	// 	if (currentTop10PercentLoss < top10PercentLoss) {
	// 		top10PercentLoss = currentTop10PercentLoss;
	// 		printf("Saving model with loss : %f\n", top10PercentLoss);
	// 		SaveModel(&trainer.models[trainer.modelLossOrders[0]], "simulation/best_model_" MODEL_NAME ".bin");
	// 	}
	// }
}