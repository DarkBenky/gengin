#include "trainNN.h"
#include "dense.h"
#include <stdio.h>

#define MAX_FLOAT 3.402823466e+38F
#define MODEL_NAME "F-16C"

int main() {
	ModelTrainer trainer;
	int epochs = 10000;
	int iterationCount = 2048;
	uint16 port = 5173;
	initModelTrainer(&trainer, 1024, epochs, iterationCount, 0.01f, 0.0005f, 2, 48, port);

	Plane plane;
	if (loadPlaneBin(&plane, "simulation/simModels/" MODEL_NAME ".bin", (float3){0.0f, 0.0f, 1.0f, 0.0f}, (float3){0.0f, 1000.0f, 0.0f, 1.0f}, 250.0f, 1.0f) != 0) {
		printf("Failed to load model: simulation/simModels/" MODEL_NAME ".bin\n");
		return 1;
	}

	float top10PercentLoss = MAX_FLOAT;
	for (int epochIdx = 0; epochIdx < trainer.epochs; epochIdx++) {
		// TODO : spawn the training on each thread then cross mutate between them
		float currentTop10PercentLoss;
		epoch(&trainer, &plane, &currentTop10PercentLoss);
		// reset baseline whenever a new target was generated so the save threshold is always relative to the current scenario, not a stale easier one
		if (trainer.epochsSinceLastTarget == 1) {
			top10PercentLoss = MAX_FLOAT;
		}
		if (currentTop10PercentLoss < top10PercentLoss) {
			top10PercentLoss = currentTop10PercentLoss;
			printf("Saving model with loss : %f\n", top10PercentLoss);
			SaveModel(&trainer.models[trainer.modelLossOrders[0]], "simulation/best_model_" MODEL_NAME ".bin");
		}
	}
}