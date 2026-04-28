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

	float scale = sqrtf(2.0f / (float)inputSize);
	for (uint32 i = 0; i < inputSize * outputSize; i++)
		layer->weights[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f * scale;
	for (uint32 i = 0; i < outputSize; i++)
		layer->biases[i] = 0.0f;

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
	}
	free(model->layers);
	free(model->inputBuffer);
	free(model->outputBuffer);

	model->layers = NULL;
	model->inputBuffer = NULL;
	model->outputBuffer = NULL;
	model->numLayers = 0;
	model->inputSize = 0;
	model->outputSize = 0;
	model->maxLayerSize = 0;
}

void Forward(Model *model, const float *input, float *output) {
	memcpy(model->inputBuffer, input, model->inputSize * sizeof(float));

	for (uint32 i = 0; i < model->numLayers; i++) {
		DenseLayer *layer = &model->layers[i];

		for (uint32 j = 0; j < layer->outputSize; j++) {
			float sum = layer->biases[j];
			for (uint32 k = 0; k < layer->inputSize; k++)
				sum += layer->weights[j * layer->inputSize + k] * model->inputBuffer[k];

			switch (layer->activation) {
			case RELU:
				model->outputBuffer[j] = fmaxf(0.0f, sum);
				break;
			case SIGMOID:
				model->outputBuffer[j] = 1.0f / (1.0f + expf(-sum));
				break;
			case TANH:
				model->outputBuffer[j] = tanhf(sum);
				break;
			}
		}

		float *temp = model->inputBuffer;
		model->inputBuffer = model->outputBuffer;
		model->outputBuffer = temp;
	}

	memcpy(output, model->inputBuffer, model->outputSize * sizeof(float));
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

#define MODEL_MAGIC   0x4D4E4E44u
#define MODEL_VERSION 1u

int SaveModel(const Model *model, const char *filename) {
	FILE *f = fopen(filename, "wb");
	if (!f) return 1;

	uint32 magic   = MODEL_MAGIC;
	uint32 version = MODEL_VERSION;
	fwrite(&magic,            sizeof(uint32), 1, f);
	fwrite(&version,          sizeof(uint32), 1, f);
	fwrite(&model->inputSize, sizeof(uint32), 1, f);
	fwrite(&model->outputSize,sizeof(uint32), 1, f);
	fwrite(&model->numLayers, sizeof(uint32), 1, f);

	for (uint32 i = 0; i < model->numLayers; i++) {
		const DenseLayer *layer = &model->layers[i];
		fwrite(&layer->inputSize,  sizeof(uint32),        1,                             f);
		fwrite(&layer->outputSize, sizeof(uint32),        1,                             f);
		fwrite(&layer->activation, sizeof(ActivationFunc),1,                             f);
		fwrite(layer->weights,     sizeof(float), layer->inputSize * layer->outputSize,  f);
		fwrite(layer->biases,      sizeof(float), layer->outputSize,                     f);
	}

	fclose(f);
	return 0;
}

int LoadModel(Model *model, const char *filename) {
	FILE *f = fopen(filename, "rb");
	if (!f) return 1;

	uint32 magic, version, inputSize, outputSize, numLayers;
	if (fread(&magic,      sizeof(uint32), 1, f) != 1 || magic   != MODEL_MAGIC)   { fclose(f); return 2; }
	if (fread(&version,    sizeof(uint32), 1, f) != 1 || version != MODEL_VERSION) { fclose(f); return 3; }
	if (fread(&inputSize,  sizeof(uint32), 1, f) != 1) { fclose(f); return 4; }
	if (fread(&outputSize, sizeof(uint32), 1, f) != 1) { fclose(f); return 5; }
	if (fread(&numLayers,  sizeof(uint32), 1, f) != 1) { fclose(f); return 6; }

	// Zero stale state so FreeModel is safe on uninitialized models.
	// If model was previously initialized, call FreeModel before LoadModel.
	memset(model, 0, sizeof(*model));
	InitModel(model, inputSize, outputSize);

	for (uint32 i = 0; i < numLayers; i++) {
		uint32 lIn, lOut;
		ActivationFunc act;
		if (fread(&lIn,  sizeof(uint32),         1, f) != 1) { FreeModel(model); fclose(f); return 7; }
		if (fread(&lOut, sizeof(uint32),         1, f) != 1) { FreeModel(model); fclose(f); return 7; }
		if (fread(&act,  sizeof(ActivationFunc), 1, f) != 1) { FreeModel(model); fclose(f); return 7; }

		AddDenseLayer(model, lIn, lOut, act);
		DenseLayer *layer = &model->layers[model->numLayers - 1];

		if (fread(layer->weights, sizeof(float), lIn * lOut, f) != lIn * lOut) { FreeModel(model); fclose(f); return 8; }
		if (fread(layer->biases,  sizeof(float), lOut,       f) != lOut)       { FreeModel(model); fclose(f); return 8; }
	}

	fclose(f);
	return 0;
}