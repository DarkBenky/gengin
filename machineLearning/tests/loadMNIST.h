#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int32_t sampleCount;
    float* images;
    int32_t* labels;
} MnistDataset;

typedef struct {
    const float* image;
    int32_t label;
} MnistSample;

MnistDataset* loadMnist(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }

    MnistDataset* ds = malloc(sizeof(MnistDataset));

    if (fread(&ds->sampleCount, sizeof(int32_t), 1, f) != 1) {
        fclose(f); free(ds); return NULL;
    }

    const int imageSize = 28 * 28;
    ds->images = malloc(sizeof(float) * imageSize * ds->sampleCount);
    ds->labels = malloc(sizeof(int32_t) * ds->sampleCount);

    for (int32_t i = 0; i < ds->sampleCount; i++) {
        if (fread(&ds->images[i * imageSize], sizeof(float), imageSize, f) != (size_t)imageSize ||
            fread(&ds->labels[i], sizeof(int32_t), 1, f) != 1) {
            fprintf(stderr, "Failed to read sample %d\n", i);
            free(ds->images); free(ds->labels); free(ds);
            fclose(f);
            return NULL;
        }
    }

    fclose(f);
    return ds;
}

void freeMnist(MnistDataset* ds) {
    if (!ds) return;
    free(ds->images);
    free(ds->labels);
    free(ds);
}

void initSampler(void) {
    srand((unsigned int)time(NULL));
}

void printSample(const MnistSample* sample) {
    static const char ramp[] = " .:-=+*#%@";
    const int width = 28;
    const int height = 28;

    printf("Label: %d\n", sample->label);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int level = (int)(sample->image[y * width + x] * 9.0f + 0.5f);
            if (level < 0) level = 0;
            if (level > 9) level = 9;
            putchar(ramp[level]);
        }
        putchar('\n');
    }
}

MnistSample nextSample(const MnistDataset* ds) {
    const int imageSize = 28 * 28;
    int32_t idx = rand() % ds->sampleCount;

    MnistSample sample;
    sample.image = &ds->images[idx * imageSize];
    sample.label = ds->labels[idx];
    return sample;
}