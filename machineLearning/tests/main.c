#include "loadMNIST.h"
#include "../kernelGen.h"
#include <string.h>
#include <float.h>

#define CL_PATH "../ccnKernel2d.cl"
#define STEPS 25000
#define BATCH 8
#define LEARNING_RATE 0.001f

float computeLoss(const float *logits, int label) {
    float loss = 0.0f;
    for (int i = 0; i < 10; i++) {
        float target = (i == label) ? 1.0f : 0.0f;
        float diff = logits[i] - target;
        loss += diff * diff; // Mean Squared Error
    }
    return loss / 10.0f;
}

float computeAccuracy(const float *logits, int label) {
    float maxLogit = -FLT_MAX;
    int maxIndex = -1;
    for (int i = 0; i < 10; i++) {
        if (logits[i] > maxLogit) {
            maxLogit = logits[i];
            maxIndex = i;
        }
    }
    return (maxIndex == label) ? 1.0f : 0.0f;
}

int main() {
    srand(1234);

    MnistDataset* ds = loadMnist("data/mnist.bin");
    if (!ds) {
        fprintf(stderr, "failed to load data/mnist.bin\n");
        return 1;
    }
    printf("Loaded MNIST dataset with %d samples\n", ds->sampleCount);
    
    MnistSample sample = nextSample(ds);
    printSample(&sample);

    CL_Context ctx = CL_Context_Create();

    KGenConvLayer  conv  = KGen_w28_h28_c1_f3_Init(&ctx, CL_PATH, NULL, 0.0f);
    KGenPoolLayer  pool  = KGenPool_w28_h28_c1_p2_Init(&ctx, CL_PATH);
    KGenConvLayer  conv2 = KGen_w14_h14_c1_f3_Init(&ctx, CL_PATH, NULL, 0.0f);
    KGenPoolLayer  pool2 = KGenPool_w14_h14_c1_p2_Init(&ctx, CL_PATH);
    KGenDenseLayer dense = KGenDense_i49_o10_Init(&ctx, CL_PATH, NULL, NULL);

    const float *image = sample.image;
    float convOut[KGEN_W28_H28_C1_F3_OUT_FLOATS];
    float poolOut[KGENPOOL_W28_H28_C1_P2_OUT_FLOATS];
    float conv2Out[KGEN_W14_H14_C1_F3_OUT_FLOATS];
    float pool2Out[KGENPOOL_W14_H14_C1_P2_OUT_FLOATS];
    float logits[KGENDENSE_I49_O10_OUT_FLOATS];

    // get current loss and accuracy
    float loss = 0.0f;
    float accuracy = 0.0f;
    for (int j = 0; j < BATCH; j++) {
        sample = nextSample(ds);
        image = sample.image;
        KGen_w28_h28_c1_f3_Run(&ctx, &conv, image, convOut, KGEN_RELU, 0);
        KGenPool_w28_h28_c1_p2_Run(&ctx, &pool, convOut, poolOut);
        KGen_w14_h14_c1_f3_Run(&ctx, &conv2, poolOut, conv2Out, KGEN_RELU, 0);
        KGenPool_w14_h14_c1_p2_Run(&ctx, &pool2, conv2Out, pool2Out);
        KGenDense_i49_o10_Run(&ctx, &dense, pool2Out, logits, KGEN_SIGMOID, 0);
        // compute loss and accuracy
        loss += computeLoss(logits, sample.label);
        accuracy += computeAccuracy(logits, sample.label);
    }
    float initialLoss = loss / BATCH;
    float initialAccuracy = accuracy / BATCH;
    float bestLoss = initialLoss;
    float bestAccuracy = initialAccuracy;
    printf("Initial loss: %f accuracy: %f\n", initialLoss, initialAccuracy);
    
    for (int i = 0; i < STEPS; i++) {
        // save current weights
        float convWeights[KGEN_W28_H28_C1_F3_WEIGHT_FLOATS];
        memcpy(convWeights, conv.hostWeights, KGEN_W28_H28_C1_F3_WEIGHT_FLOATS * sizeof(float));
        float conv2Weights[KGEN_W14_H14_C1_F3_WEIGHT_FLOATS];
        memcpy(conv2Weights, conv2.hostWeights, KGEN_W14_H14_C1_F3_WEIGHT_FLOATS * sizeof(float));
        float denseWeights[KGENDENSE_I49_O10_WEIGHT_FLOATS];
        memcpy(denseWeights, dense.hostWeights, KGENDENSE_I49_O10_WEIGHT_FLOATS * sizeof(float));

        // mutate weights
        KGen_w28_h28_c1_f3_Mutate(&ctx, &conv, LEARNING_RATE);
        KGen_w14_h14_c1_f3_Mutate(&ctx, &conv2, LEARNING_RATE);
        KGenDense_i49_o10_Mutate(&ctx, &dense, LEARNING_RATE);

        // compute loss and accuracy after mutation on a batch of samples
        loss = 0.0f;
        accuracy = 0.0f;
        for (int j = 0; j < BATCH; j++) {
            sample = nextSample(ds);
            image = sample.image;
            KGen_w28_h28_c1_f3_Run(&ctx, &conv, image, convOut, KGEN_RELU, 0);
            KGenPool_w28_h28_c1_p2_Run(&ctx, &pool, convOut, poolOut);
            KGen_w14_h14_c1_f3_Run(&ctx, &conv2, poolOut, conv2Out, KGEN_RELU, 0);
            KGenPool_w14_h14_c1_p2_Run(&ctx, &pool2, conv2Out, pool2Out);
            KGenDense_i49_o10_Run(&ctx, &dense, pool2Out, logits, KGEN_SIGMOID, 0);
            // compute loss and accuracy
            loss += computeLoss(logits, sample.label);
            accuracy += computeAccuracy(logits, sample.label);
        }
        float candidateLoss = loss / BATCH;
        float candidateAccuracy = accuracy / BATCH;
        printf("Loss after mutation step %d: %f accuracy: %f\n", i, candidateLoss, candidateAccuracy);

        // keep the mutation only if it beats the best loss so far
        if (candidateLoss < bestLoss) {
            bestLoss = candidateLoss;
            bestAccuracy = candidateAccuracy;
        } else {
            memcpy(conv.hostWeights, convWeights, KGEN_W28_H28_C1_F3_WEIGHT_FLOATS * sizeof(float));
            memcpy(conv2.hostWeights, conv2Weights, KGEN_W14_H14_C1_F3_WEIGHT_FLOATS * sizeof(float));
            memcpy(dense.hostWeights, denseWeights, KGENDENSE_I49_O10_WEIGHT_FLOATS * sizeof(float));
        }
    }

    printf("Final loss: %f\n", bestLoss);
    printf("Final accuracy: %f\n", bestAccuracy);
    printf("Loss improvement: %f\n", initialLoss - bestLoss);
    printf("Accuracy improvement: %f\n", bestAccuracy - initialAccuracy);

    KGen_w28_h28_c1_f3_Destroy(&conv);
    KGenPool_w28_h28_c1_p2_Destroy(&pool);
    KGen_w14_h14_c1_f3_Destroy(&conv2);
    KGenPool_w14_h14_c1_p2_Destroy(&pool2);
    KGenDense_i49_o10_Destroy(&dense);
    CL_Context_Destroy(&ctx);
    freeMnist(ds);
    return 0;
}