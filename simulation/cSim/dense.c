#include "dense.h"
#include <math.h>
#include <string.h>

void InitModel(Model *model, uint32 inputSize, uint32 outputSize) {
	model->numLayers = 0;
	model->layers = NULL;
	model->inputSize = inputSize;
	model->outputSize = outputSize;
	uint32 maxSize = inputSize > outputSize ? inputSize : outputSize;
	model->maxLayerSize = maxSize;
	model->inputBuffer = malloc(maxSize * sizeof(float));
	model->outputBuffer = malloc(maxSize * sizeof(float));
	model->activations = malloc(sizeof(float *));
	model->activations[0] = malloc(inputSize * sizeof(float));
}

void AddDenseLayer(Model *model, uint32 inputSize, uint32 outputSize, ActivationFunc activation) {
	DenseLayer *newLayers = realloc(model->layers, (model->numLayers + 1) * sizeof(DenseLayer));
	if (!newLayers)
		return;
	model->layers = newLayers;

	DenseLayer *layer = &model->layers[model->numLayers++];
	layer->inputSize = inputSize;
	layer->outputSize = outputSize;
	layer->activation = activation;
	layer->weights = malloc(inputSize * outputSize * sizeof(float));
	layer->biases = malloc(outputSize * sizeof(float));
	layer->preAct = malloc(outputSize * sizeof(float));
	layer->gradWeights = calloc(inputSize * outputSize, sizeof(float));
	layer->gradBiases = calloc(outputSize, sizeof(float));

	float scale = sqrtf(2.0f / (float)inputSize);
	for (uint32 i = 0; i < inputSize * outputSize; i++)
		layer->weights[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f * scale;
	for (uint32 i = 0; i < outputSize; i++)
		layer->biases[i] = 0.0f;

	float **newActivations = realloc(model->activations, (model->numLayers + 1) * sizeof(float *));
	if (!newActivations) return;
	model->activations = newActivations;
	model->activations[model->numLayers] = malloc(outputSize * sizeof(float));

	uint32 maxSize = inputSize > outputSize ? inputSize : outputSize;
	if (maxSize > model->maxLayerSize) {
		model->maxLayerSize = maxSize;
		free(model->inputBuffer);
		free(model->outputBuffer);
		model->inputBuffer = malloc(maxSize * sizeof(float));
		model->outputBuffer = malloc(maxSize * sizeof(float));
	}
}

void FreeModel(Model *model) {
	for (uint32 i = 0; i < model->numLayers; i++) {
		free(model->layers[i].weights);
		free(model->layers[i].biases);
		free(model->layers[i].preAct);
		free(model->layers[i].gradWeights);
		free(model->layers[i].gradBiases);
	}
	free(model->layers);
	free(model->inputBuffer);
	free(model->outputBuffer);
	if (model->activations) {
		for (uint32 i = 0; i <= model->numLayers; i++)
			free(model->activations[i]);
		free(model->activations);
	}
	memset(model, 0, sizeof(*model));
}

void Forward(Model *model, const float *input, float *output) {
	memcpy(model->activations[0], input, model->inputSize * sizeof(float));

	for (uint32 i = 0; i < model->numLayers; i++) {
		DenseLayer *layer = &model->layers[i];
		float *in = model->activations[i];
		float *out = model->activations[i + 1];

		for (uint32 j = 0; j < layer->outputSize; j++) {
			float z = layer->biases[j];
			for (uint32 k = 0; k < layer->inputSize; k++)
				z += layer->weights[j * layer->inputSize + k] * in[k];
			layer->preAct[j] = z;
			switch (layer->activation) {
			case RELU:
				out[j] = fmaxf(0.0f, z);
				break;
			case SIGMOID:
				out[j] = 1.0f / (1.0f + expf(-z));
				break;
			case TANH:
				out[j] = tanhf(z);
				break;
			}
		}
	}

	memcpy(output, model->activations[model->numLayers], model->outputSize * sizeof(float));
}

void CopyModel(Model *dest, const Model *src) {
	FreeModel(dest);
	InitModel(dest, src->inputSize, src->outputSize);
	for (uint32 i = 0; i < src->numLayers; i++) {
		DenseLayer *sl = &src->layers[i];
		AddDenseLayer(dest, sl->inputSize, sl->outputSize, sl->activation);
		DenseLayer *dl = &dest->layers[i];
		memcpy(dl->weights, sl->weights, sl->inputSize * sl->outputSize * sizeof(float));
		memcpy(dl->biases, sl->biases, sl->outputSize * sizeof(float));
	}
}

void MutateModel(Model *model, float mutationRate) {
	for (uint32 i = 0; i < model->numLayers; i++) {
		DenseLayer *layer = &model->layers[i];
		uint32 wCount = layer->inputSize * layer->outputSize;
		for (uint32 j = 0; j < wCount; j++) {
			if ((float)rand() / RAND_MAX < mutationRate)
				layer->weights[j] += ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
		}
		for (uint32 j = 0; j < layer->outputSize; j++) {
			if ((float)rand() / RAND_MAX < mutationRate)
				layer->biases[j] += ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
		}
	}
}

#define MODEL_MAGIC 0x4D4E4E44u
#define MODEL_VERSION 1u

int SaveModel(const Model *model, const char *filename) {
	FILE *f = fopen(filename, "wb");
	if (!f) return 1;

	uint32 magic = MODEL_MAGIC;
	uint32 version = MODEL_VERSION;
	fwrite(&magic, sizeof(uint32), 1, f);
	fwrite(&version, sizeof(uint32), 1, f);
	fwrite(&model->inputSize, sizeof(uint32), 1, f);
	fwrite(&model->outputSize, sizeof(uint32), 1, f);
	fwrite(&model->numLayers, sizeof(uint32), 1, f);

	for (uint32 i = 0; i < model->numLayers; i++) {
		const DenseLayer *layer = &model->layers[i];
		fwrite(&layer->inputSize, sizeof(uint32), 1, f);
		fwrite(&layer->outputSize, sizeof(uint32), 1, f);
		fwrite(&layer->activation, sizeof(ActivationFunc), 1, f);
		fwrite(layer->weights, sizeof(float), layer->inputSize * layer->outputSize, f);
		fwrite(layer->biases, sizeof(float), layer->outputSize, f);
	}

	fclose(f);
	return 0;
}

int LoadModel(Model *model, const char *filename) {
	FILE *f = fopen(filename, "rb");
	if (!f) return 1;

	uint32 magic, version, inputSize, outputSize, numLayers;
	if (fread(&magic, sizeof(uint32), 1, f) != 1 || magic != MODEL_MAGIC) {
		fclose(f);
		return 2;
	}
	if (fread(&version, sizeof(uint32), 1, f) != 1 || version != MODEL_VERSION) {
		fclose(f);
		return 3;
	}
	if (fread(&inputSize, sizeof(uint32), 1, f) != 1) {
		fclose(f);
		return 4;
	}
	if (fread(&outputSize, sizeof(uint32), 1, f) != 1) {
		fclose(f);
		return 5;
	}
	if (fread(&numLayers, sizeof(uint32), 1, f) != 1) {
		fclose(f);
		return 6;
	}

	// Zero stale state so FreeModel is safe on uninitialized models.
	// If model was previously initialized, call FreeModel before LoadModel.
	memset(model, 0, sizeof(*model));
	InitModel(model, inputSize, outputSize);

	for (uint32 i = 0; i < numLayers; i++) {
		uint32 lIn, lOut;
		ActivationFunc act;
		if (fread(&lIn, sizeof(uint32), 1, f) != 1) {
			FreeModel(model);
			fclose(f);
			return 7;
		}
		if (fread(&lOut, sizeof(uint32), 1, f) != 1) {
			FreeModel(model);
			fclose(f);
			return 7;
		}
		if (fread(&act, sizeof(ActivationFunc), 1, f) != 1) {
			FreeModel(model);
			fclose(f);
			return 7;
		}

		AddDenseLayer(model, lIn, lOut, act);
		DenseLayer *layer = &model->layers[model->numLayers - 1];

		if (fread(layer->weights, sizeof(float), lIn * lOut, f) != lIn * lOut) {
			FreeModel(model);
			fclose(f);
			return 8;
		}
		if (fread(layer->biases, sizeof(float), lOut, f) != lOut) {
			FreeModel(model);
			fclose(f);
			return 8;
		}
	}

	fclose(f);
	return 0;
}

void CrossoverModels(Model *child, const Model *parentA, const Model *parentB, float mutationRate) {
	if (parentA->numLayers != parentB->numLayers || parentA->inputSize != parentB->inputSize || parentA->outputSize != parentB->outputSize)
		return;

	FreeModel(child);
	InitModel(child, parentA->inputSize, parentA->outputSize);

	for (uint32 i = 0; i < parentA->numLayers; i++) {
		const DenseLayer *layerA = &parentA->layers[i];
		const DenseLayer *layerB = &parentB->layers[i];
		if (layerA->inputSize != layerB->inputSize || layerA->outputSize != layerB->outputSize) {
			FreeModel(child);
			return;
		}
		AddDenseLayer(child, layerA->inputSize, layerA->outputSize, layerA->activation);
		DenseLayer *childLayer = &child->layers[i];

		for (uint32 j = 0; j < layerA->inputSize * layerA->outputSize; j++) {
			childLayer->weights[j] = (rand() % 2 == 0) ? layerA->weights[j] : layerB->weights[j];
			if ((float)rand() / RAND_MAX < mutationRate)
				childLayer->weights[j] += ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
		}
		for (uint32 j = 0; j < layerA->outputSize; j++) {
			childLayer->biases[j] = (rand() % 2 == 0) ? layerA->biases[j] : layerB->biases[j];
			if ((float)rand() / RAND_MAX < mutationRate)
				childLayer->biases[j] += ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
		}
	}
}

static float activationDerivative(ActivationFunc act, float preAct, float postAct) {
	switch (act) {
	case RELU:
		return preAct > 0.0f ? 1.0f : 0.0f;
	case SIGMOID:
		return postAct * (1.0f - postAct);
	case TANH:
		return 1.0f - postAct * postAct;
	}
	return 1.0f;
}

void ZeroGradients(Model *model) {
	for (uint32 i = 0; i < model->numLayers; i++) {
		DenseLayer *layer = &model->layers[i];
		memset(layer->gradWeights, 0, layer->inputSize * layer->outputSize * sizeof(float));
		memset(layer->gradBiases, 0, layer->outputSize * sizeof(float));
	}
}

// outputGrad is dL/dOutput (size: model->outputSize), externally computed from
// e.g. step-improvement or distance-reduction signal. Accumulates into gradWeights/gradBiases.
void Backward(Model *model, const float *outputGrad) {
	float *currentDelta = model->inputBuffer;
	float *prevDelta = model->outputBuffer;

	memcpy(currentDelta, outputGrad, model->outputSize * sizeof(float));

	for (int i = (int)model->numLayers - 1; i >= 0; i--) {
		DenseLayer *layer = &model->layers[i];
		float *layerInput = model->activations[i];
		float *layerOutput = model->activations[i + 1];

		for (uint32 j = 0; j < layer->outputSize; j++)
			currentDelta[j] *= activationDerivative(layer->activation, layer->preAct[j], layerOutput[j]);

		for (uint32 j = 0; j < layer->outputSize; j++) {
			layer->gradBiases[j] += currentDelta[j];
			for (uint32 k = 0; k < layer->inputSize; k++)
				layer->gradWeights[j * layer->inputSize + k] += currentDelta[j] * layerInput[k];
		}

		if (i > 0) {
			memset(prevDelta, 0, layer->inputSize * sizeof(float));
			for (uint32 j = 0; j < layer->outputSize; j++)
				for (uint32 k = 0; k < layer->inputSize; k++)
					prevDelta[k] += layer->weights[j * layer->inputSize + k] * currentDelta[j];
			float *tmp = currentDelta;
			currentDelta = prevDelta;
			prevDelta = tmp;
		}
	}
}

void InitOptimizer(Optimizer *opt, const Model *model, float lr, float beta1, float beta2) {
	opt->lr = lr;
	opt->beta1 = beta1;
	opt->beta2 = beta2;
	opt->epsilon = 1e-8f;
	opt->step = 0;
	opt->numLayers = model->numLayers;
	opt->mWeights = malloc(model->numLayers * sizeof(float *));
	opt->mBiases = malloc(model->numLayers * sizeof(float *));
	opt->vWeights = malloc(model->numLayers * sizeof(float *));
	opt->vBiases = malloc(model->numLayers * sizeof(float *));
	for (uint32 i = 0; i < model->numLayers; i++) {
		uint32 wSize = model->layers[i].inputSize * model->layers[i].outputSize;
		uint32 bSize = model->layers[i].outputSize;
		opt->mWeights[i] = calloc(wSize, sizeof(float));
		opt->mBiases[i] = calloc(bSize, sizeof(float));
		opt->vWeights[i] = calloc(wSize, sizeof(float));
		opt->vBiases[i] = calloc(bSize, sizeof(float));
	}
}

void FreeOptimizer(Optimizer *opt) {
	for (uint32 i = 0; i < opt->numLayers; i++) {
		free(opt->mWeights[i]);
		free(opt->mBiases[i]);
		free(opt->vWeights[i]);
		free(opt->vBiases[i]);
	}
	free(opt->mWeights);
	free(opt->mBiases);
	free(opt->vWeights);
	free(opt->vBiases);
	memset(opt, 0, sizeof(*opt));
}

// Adam optimizer update. Call after Backward, then ZeroGradients before next step.
void UpdateWeights(Model *model, Optimizer *opt) {
	opt->step++;
	float bc1 = 1.0f - powf(opt->beta1, (float)opt->step);
	float bc2 = 1.0f - powf(opt->beta2, (float)opt->step);

	for (uint32 i = 0; i < model->numLayers; i++) {
		DenseLayer *layer = &model->layers[i];
		uint32 wSize = layer->inputSize * layer->outputSize;

		for (uint32 j = 0; j < wSize; j++) {
			opt->mWeights[i][j] = opt->beta1 * opt->mWeights[i][j] + (1.0f - opt->beta1) * layer->gradWeights[j];
			opt->vWeights[i][j] = opt->beta2 * opt->vWeights[i][j] + (1.0f - opt->beta2) * layer->gradWeights[j] * layer->gradWeights[j];
			float mHat = opt->mWeights[i][j] / bc1;
			float vHat = opt->vWeights[i][j] / bc2;
			layer->weights[j] -= opt->lr * mHat / (sqrtf(vHat) + opt->epsilon);
		}

		for (uint32 j = 0; j < layer->outputSize; j++) {
			opt->mBiases[i][j] = opt->beta1 * opt->mBiases[i][j] + (1.0f - opt->beta1) * layer->gradBiases[j];
			opt->vBiases[i][j] = opt->beta2 * opt->vBiases[i][j] + (1.0f - opt->beta2) * layer->gradBiases[j] * layer->gradBiases[j];
			float mHat = opt->mBiases[i][j] / bc1;
			float vHat = opt->vBiases[i][j] / bc2;
			layer->biases[j] -= opt->lr * mHat / (sqrtf(vHat) + opt->epsilon);
		}
	}
}