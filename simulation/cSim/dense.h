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
	float *preAct;		// z = Wx + b, cached for backward pass
	float *gradWeights; // accumulated dL/dW
	float *gradBiases;	// accumulated dL/dB
} DenseLayer;

typedef struct {
	uint32 numLayers;
	DenseLayer *layers;
	uint32 inputSize;
	uint32 outputSize;
	uint32 maxLayerSize;
	float *inputBuffer;	 // scratch for backward pass deltas
	float *outputBuffer; // scratch for backward pass deltas
	float **activations; // [numLayers+1]: model input + each layer's output, cached for backward
} Model;

typedef struct {
	float lr;
	float beta1;
	float beta2;
	float epsilon;
	uint32 step;
	uint32 numLayers;
	float **mWeights;
	float **mBiases;
	float **vWeights;
	float **vBiases;
} Optimizer;

void InitModel(Model *model, uint32 inputSize, uint32 outputSize);
void AddDenseLayer(Model *model, uint32 inputSize, uint32 outputSize, ActivationFunc activation);
void FreeModel(Model *model);
void Forward(Model *model, const float *input, float *output);
void CopyModel(Model *dest, const Model *src);
void MutateModel(Model *model, float mutationRate);
void CrossoverModels(Model *child, const Model *parentA, const Model *parentB, float mutationRate);

void InitOptimizer(Optimizer *opt, const Model *model, float lr, float beta1, float beta2);
void FreeOptimizer(Optimizer *opt);
void ZeroGradients(Model *model);
// outputGrad = dL/dOutput, externally computed from e.g. step-improvement or distance signal
void Backward(Model *model, const float *outputGrad);
void UpdateWeights(Model *model, Optimizer *opt);

// Returns 0 on success, non-zero on failure.
int SaveModel(const Model *model, const char *filename);
int LoadModel(Model *model, const char *filename);