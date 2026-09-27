#include "loadMNIST.h"
#include "../kernelGen.h"
#include "../weightsJson.h"
#include <string.h>
#include <stdlib.h>
#include <float.h>
#include <time.h>

#define CL_PATH "../ccnKernel2d.cl"
#ifndef STEPS
#define STEPS 0
#endif
#define BATCH 32
#define LEARNING_RATE 0.01f

static double nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

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

// weights.json checkpoint: the file format lives in ../weightsJson.h; this is the layer
// table for the current net plus the thin glue that reads/applies the typed layers
#define WEIGHTS_JSON_PATH "weights.json"

static const KgwtLayerSpec kWeightsSpecs[4] = {
    {"conv1", "conv", {"w", "h", "c", "f", "n"}, {28, 28, 1, 3, 16}, 5, KGEN_W28_H28_C1_F3_N16_WEIGHT_FLOATS, KGEN_W28_H28_C1_F3_N16_BIAS_FLOATS},
    {"conv2", "conv", {"w", "h", "c", "f", "n"}, {14, 14, 16, 3, 32}, 5, KGEN_W14_H14_C16_F3_N32_WEIGHT_FLOATS, KGEN_W14_H14_C16_F3_N32_BIAS_FLOATS},
    {"dense1", "dense", {"in", "out"}, {1568, 128}, 2, KGENDENSE_I1568_O128_WEIGHT_FLOATS, KGENDENSE_I1568_O128_BIAS_FLOATS},
    {"dense2", "dense", {"in", "out"}, {128, 10}, 2, KGENDENSE_I128_O10_WEIGHT_FLOATS, KGENDENSE_I128_O10_BIAS_FLOATS},
};

static int saveWeights(const char *path, CL_Context *ctx, KGenConvLayer *conv, KGenConvLayer *conv2,
                       KGenDenseLayer *dense, KGenDenseLayer *dense2) {
    float convBias[KGEN_W28_H28_C1_F3_N16_BIAS_FLOATS];
    float conv2Bias[KGEN_W14_H14_C16_F3_N32_BIAS_FLOATS];
    float denseBias[KGENDENSE_I1568_O128_BIAS_FLOATS];
    float dense2Bias[KGENDENSE_I128_O10_BIAS_FLOATS];
    // bias has no host copy, read it back from the GPU
    CL_Buffer_Read(ctx, &conv->bias, convBias, sizeof(convBias));
    CL_Buffer_Read(ctx, &conv2->bias, conv2Bias, sizeof(conv2Bias));
    CL_Buffer_Read(ctx, &dense->bias, denseBias, sizeof(denseBias));
    CL_Buffer_Read(ctx, &dense2->bias, dense2Bias, sizeof(dense2Bias));

    const float *weights[4] = {conv->hostWeights, conv2->hostWeights, dense->hostWeights, dense2->hostWeights};
    const float *biases[4] = {convBias, conv2Bias, denseBias, dense2Bias};
    return Kgwt_SaveJson(path, 4, kWeightsSpecs, weights, biases);
}

static int loadWeights(const char *path, CL_Context *ctx,
                       KGenConvLayer *conv, KGenConvLayer *conv2,
                       KGenDenseLayer *dense, KGenDenseLayer *dense2) {
    // static: dense1 alone is 200k floats, no need to push that through the stack
    static float wConv[KGEN_W28_H28_C1_F3_N16_WEIGHT_FLOATS];
    static float wConv2[KGEN_W14_H14_C16_F3_N32_WEIGHT_FLOATS];
    static float wDense[KGENDENSE_I1568_O128_WEIGHT_FLOATS];
    static float wDense2[KGENDENSE_I128_O10_WEIGHT_FLOATS];
    float bConv[KGEN_W28_H28_C1_F3_N16_BIAS_FLOATS];
    float bConv2[KGEN_W14_H14_C16_F3_N32_BIAS_FLOATS];
    float bDense[KGENDENSE_I1568_O128_BIAS_FLOATS];
    float bDense2[KGENDENSE_I128_O10_BIAS_FLOATS];

    float *weights[4] = {wConv, wConv2, wDense, wDense2};
    float *biases[4] = {bConv, bConv2, bDense, bDense2};
    if (!Kgwt_LoadJson(path, 4, kWeightsSpecs, weights, biases)) return 0;

    KGen_w28_h28_c1_f3_n16_SetWeights(ctx, conv, wConv, bConv);
    KGen_w14_h14_c16_f3_n32_SetWeights(ctx, conv2, wConv2, bConv2);
    KGenDense_i1568_o128_SetWeights(ctx, dense, wDense, bDense);
    KGenDense_i128_o10_SetWeights(ctx, dense2, wDense2, bDense2);
    return 1;
}

static float evaluateAccuracy(CL_Context *ctx, KGenConvLayer *conv, KGenPoolLayer *pool,
                              KGenConvLayer *conv2, KGenPoolLayer *pool2,
                              KGenDenseLayer *dense, KGenDenseLayer *dense2,
                              KGenSoftmaxLayer *softmax, MnistDataset *ds,
                              float *convOut, float *poolOut, float *conv2Out, float *pool2Out,
                              float *hidden, float *scores, float *logits, int count) {
    int correct = 0;
    for (int i = 0; i < count; i++) {
        MnistSample sample = nextSample(ds);
        KGen_w28_h28_c1_f3_n16_Run(ctx, conv, sample.image, convOut, KGEN_RELU, 0);
        KGenPool_w28_h28_c16_p2_Run(ctx, pool, convOut, poolOut);
        KGen_w14_h14_c16_f3_n32_Run(ctx, conv2, poolOut, conv2Out, KGEN_RELU, 0);
        KGenPool_w14_h14_c32_p2_Run(ctx, pool2, conv2Out, pool2Out);
        KGenDense_i1568_o128_Run(ctx, dense, pool2Out, hidden, KGEN_RELU, 0);
        KGenDense_i128_o10_Run(ctx, dense2, hidden, scores, KGEN_NONE, 0);
        KGenSoftmax_n10_Run(ctx, softmax, scores, logits);
        correct += (computeAccuracy(logits, sample.label) > 0.5f);
    }
    return (float)correct / (float)count;
}

// runs the eval loop and reports accuracy plus how fast the forward pass runs
static float reportAccuracy(CL_Context *ctx, KGenConvLayer *conv, KGenPoolLayer *pool,
                            KGenConvLayer *conv2, KGenPoolLayer *pool2,
                            KGenDenseLayer *dense, KGenDenseLayer *dense2,
                            KGenSoftmaxLayer *softmax, MnistDataset *ds,
                            float *convOut, float *poolOut, float *conv2Out, float *pool2Out,
                            float *hidden, float *scores, float *logits, int count) {
    const double startMs = nowMs();
    const float accuracy = evaluateAccuracy(ctx, conv, pool, conv2, pool2, dense, dense2, softmax, ds,
                                            convOut, poolOut, conv2Out, pool2Out, hidden, scores, logits, count);
    const double elapsedMs = nowMs() - startMs;
    printf("Test accuracy over %d samples: %f\n", count, accuracy);
    printf("Inference: %.3f ms/image, %.1f images/s (%d samples in %.1f ms)\n",
           elapsedMs / count, count * 1000.0 / elapsedMs, count, elapsedMs);
    return accuracy;
}

static void destroyLayers(CL_Context *ctx, KGenConvLayer *conv, KGenPoolLayer *pool,
                          KGenConvLayer *conv2, KGenPoolLayer *pool2,
                          KGenDenseLayer *dense, KGenDenseLayer *dense2,
                          KGenSoftmaxLayer *softmax) {
    KGen_w28_h28_c1_f3_n16_Destroy(conv);
    KGenPool_w28_h28_c16_p2_Destroy(pool);
    KGen_w14_h14_c16_f3_n32_Destroy(conv2);
    KGenPool_w14_h14_c32_p2_Destroy(pool2);
    KGenDense_i1568_o128_Destroy(dense);
    KGenDense_i128_o10_Destroy(dense2);
    KGenSoftmax_n10_Destroy(softmax);
    CL_Context_Destroy(ctx);
}

int main(int argc, char **argv) {
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

    KGenConvLayer  conv  = KGen_w28_h28_c1_f3_n16_Init(&ctx, CL_PATH, NULL, NULL);
    KGenPoolLayer  pool  = KGenPool_w28_h28_c16_p2_Init(&ctx, CL_PATH);
    KGenConvLayer  conv2 = KGen_w14_h14_c16_f3_n32_Init(&ctx, CL_PATH, NULL, NULL);
    KGenPoolLayer  pool2 = KGenPool_w14_h14_c32_p2_Init(&ctx, CL_PATH);
    KGenDenseLayer dense = KGenDense_i1568_o128_Init(&ctx, CL_PATH, NULL, NULL);
    KGenDenseLayer dense2 = KGenDense_i128_o10_Init(&ctx, CL_PATH, NULL, NULL);
    KGenSoftmaxLayer softmax = KGenSoftmax_n10_Init(&ctx, CL_PATH);

    const float *image = sample.image;
    float convOut[KGEN_W28_H28_C1_F3_N16_OUT_FLOATS];
    float poolOut[KGENPOOL_W28_H28_C16_P2_OUT_FLOATS];
    float conv2Out[KGEN_W14_H14_C16_F3_N32_OUT_FLOATS];
    float pool2Out[KGENPOOL_W14_H14_C32_P2_OUT_FLOATS];
    float hidden[KGENDENSE_I1568_O128_OUT_FLOATS];
    float scores[KGENDENSE_I128_O10_OUT_FLOATS];
    float logits[KGENSOFTMAX_N10_COUNT];

    const int evalOnly = (argc > 1 && strcmp(argv[1], "--eval") == 0);
    const int loaded = loadWeights(WEIGHTS_JSON_PATH, &ctx, &conv, &conv2, &dense, &dense2);
    if (loaded) {
        printf("Loaded weights from %s\n", WEIGHTS_JSON_PATH);
    }
    if (evalOnly) {
        if (!loaded) {
            fprintf(stderr, "--eval: %s not found\n", WEIGHTS_JSON_PATH);
            destroyLayers(&ctx, &conv, &pool, &conv2, &pool2, &dense, &dense2, &softmax);
            freeMnist(ds);
            return 1;
        }
        reportAccuracy(&ctx, &conv, &pool, &conv2, &pool2, &dense, &dense2, &softmax, ds,
                       convOut, poolOut, conv2Out, pool2Out, hidden, scores, logits, 512);
        destroyLayers(&ctx, &conv, &pool, &conv2, &pool2, &dense, &dense2, &softmax);
        freeMnist(ds);
        return 0;
    }
    if (!loaded) {
        printf("No %s yet, starting from random weights\n", WEIGHTS_JSON_PATH);
    }

    // get current loss and accuracy
    float loss = 0.0f;
    float accuracy = 0.0f;
    for (int j = 0; j < BATCH; j++) {
        sample = nextSample(ds);
        image = sample.image;
        KGen_w28_h28_c1_f3_n16_Run(&ctx, &conv, image, convOut, KGEN_RELU, 0);
        KGenPool_w28_h28_c16_p2_Run(&ctx, &pool, convOut, poolOut);
        KGen_w14_h14_c16_f3_n32_Run(&ctx, &conv2, poolOut, conv2Out, KGEN_RELU, 0);
        KGenPool_w14_h14_c32_p2_Run(&ctx, &pool2, conv2Out, pool2Out);
        KGenDense_i1568_o128_Run(&ctx, &dense, pool2Out, hidden, KGEN_RELU, 0);
        KGenDense_i128_o10_Run(&ctx, &dense2, hidden, scores, KGEN_NONE, 0);
        KGenSoftmax_n10_Run(&ctx, &softmax, scores, logits);
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
        float convWeights[KGEN_W28_H28_C1_F3_N16_WEIGHT_FLOATS];
        memcpy(convWeights, conv.hostWeights, KGEN_W28_H28_C1_F3_N16_WEIGHT_FLOATS * sizeof(float));
        float conv2Weights[KGEN_W14_H14_C16_F3_N32_WEIGHT_FLOATS];
        memcpy(conv2Weights, conv2.hostWeights, KGEN_W14_H14_C16_F3_N32_WEIGHT_FLOATS * sizeof(float));
        // dense1 alone is 800 KB, keep it off the stack
        static float denseWeights[KGENDENSE_I1568_O128_WEIGHT_FLOATS];
        memcpy(denseWeights, dense.hostWeights, KGENDENSE_I1568_O128_WEIGHT_FLOATS * sizeof(float));
        float dense2Weights[KGENDENSE_I128_O10_WEIGHT_FLOATS];
        memcpy(dense2Weights, dense2.hostWeights, KGENDENSE_I128_O10_WEIGHT_FLOATS * sizeof(float));

        // mutate weights
        KGen_w28_h28_c1_f3_n16_Mutate(&ctx, &conv, LEARNING_RATE);
        KGen_w14_h14_c16_f3_n32_Mutate(&ctx, &conv2, LEARNING_RATE);
        KGenDense_i1568_o128_Mutate(&ctx, &dense, LEARNING_RATE);
        KGenDense_i128_o10_Mutate(&ctx, &dense2, LEARNING_RATE);

        // compute loss and accuracy after mutation on a batch of samples
        loss = 0.0f;
        accuracy = 0.0f;
        for (int j = 0; j < BATCH; j++) {
            sample = nextSample(ds);
            image = sample.image;
            KGen_w28_h28_c1_f3_n16_Run(&ctx, &conv, image, convOut, KGEN_RELU, 0);
            KGenPool_w28_h28_c16_p2_Run(&ctx, &pool, convOut, poolOut);
            KGen_w14_h14_c16_f3_n32_Run(&ctx, &conv2, poolOut, conv2Out, KGEN_RELU, 0);
            KGenPool_w14_h14_c32_p2_Run(&ctx, &pool2, conv2Out, pool2Out);
            KGenDense_i1568_o128_Run(&ctx, &dense, pool2Out, hidden, KGEN_RELU, 0);
            KGenDense_i128_o10_Run(&ctx, &dense2, hidden, scores, KGEN_NONE, 0);
            KGenSoftmax_n10_Run(&ctx, &softmax, scores, logits);
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
            // revert host weights AND re-upload, otherwise the GPU keeps the rejected mutation
            KGen_w28_h28_c1_f3_n16_SetWeights(&ctx, &conv, convWeights, NULL);
            KGen_w14_h14_c16_f3_n32_SetWeights(&ctx, &conv2, conv2Weights, NULL);
            KGenDense_i1568_o128_SetWeights(&ctx, &dense, denseWeights, NULL);
            KGenDense_i128_o10_SetWeights(&ctx, &dense2, dense2Weights, NULL);
        }
    }

    printf("Best batch loss: %f\n", bestLoss);
    printf("Best batch accuracy (selected %d-sample batch): %f\n", BATCH, bestAccuracy);
    printf("Loss improvement: %f\n", initialLoss - bestLoss);
    printf("Accuracy improvement: %f\n", bestAccuracy - initialAccuracy);

    // Test the final weights on few samples
    for (int i = 0; i < 5; i++) {
        sample = nextSample(ds);
        image = sample.image;
        KGen_w28_h28_c1_f3_n16_Run(&ctx, &conv, image, convOut, KGEN_RELU, 0);
        KGenPool_w28_h28_c16_p2_Run(&ctx, &pool, convOut, poolOut);
        KGen_w14_h14_c16_f3_n32_Run(&ctx, &conv2, poolOut, conv2Out, KGEN_RELU, 0);
        KGenPool_w14_h14_c32_p2_Run(&ctx, &pool2, conv2Out, pool2Out);
        KGenDense_i1568_o128_Run(&ctx, &dense, pool2Out, hidden, KGEN_RELU, 0);
        KGenDense_i128_o10_Run(&ctx, &dense2, hidden, scores, KGEN_NONE, 0);
        KGenSoftmax_n10_Run(&ctx, &softmax, scores, logits);
        
        // loss
        float loss = computeLoss(logits, sample.label);
        printf("Sample %d loss: %f\n", i, loss);
        // accuracy
        float accuracy = computeAccuracy(logits, sample.label);
        printf("Sample %d accuracy: %f\n", i, accuracy);

        printSample(&sample);
        float sum = 0.0f;
        for (int j = 0; j < 10; j++) {
            printf("logits[%d] = %f\n", j, logits[j]);
            sum += logits[j];
        }
        printf("sum = %f\n", sum);
        printf("-------------------------\n");
    }

    // stable accuracy: 5 samples is too noisy to read anything from
    const int evalCount = 512;
    reportAccuracy(&ctx, &conv, &pool, &conv2, &pool2, &dense, &dense2, &softmax, ds,
                   convOut, poolOut, conv2Out, pool2Out, hidden, scores, logits, evalCount);

    if (saveWeights(WEIGHTS_JSON_PATH, &ctx, &conv, &conv2, &dense, &dense2)) {
        printf("Saved weights to %s\n", WEIGHTS_JSON_PATH);
    }

    destroyLayers(&ctx, &conv, &pool, &conv2, &pool2, &dense, &dense2, &softmax);
    freeMnist(ds);
    return 0;
}