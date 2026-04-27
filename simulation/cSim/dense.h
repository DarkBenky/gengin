#pragma once
#include "import.h"

typedef enum {
	RELU,
	SIGMOID,
	TANH,
} ActivationFunc;

typedef struct {
	uint32 inputSize;
	uint32 outputSize;
	ActivationFunc activation;
	float *weights;
	float *biases;
} DenseLayer;

typedef struct {
	uint32 numLayers;
	DenseLayer *layers;
	uint32 inputSize;
	uint32 outputSize;
	uint32 maxLayerSize;
	float *inputBuffer;
	float *outputBuffer;
} Model;

void InitModel(Model *model, uint32 inputSize, uint32 outputSize);
void AddDenseLayer(Model *model, uint32 inputSize, uint32 outputSize, ActivationFunc activation);
void FreeModel(Model *model);
void Forward(Model *model, const float *input, float *output);
void CopyModel(Model *dest, const Model *src);
void MutateModel(Model *model, float mutationRate);