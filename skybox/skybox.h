#ifndef SKYBOX_H
#define SKYBOX_H

#include "../object/format.h"

typedef struct Skybox {
    uint32 *front;
    uint32 *back;
    uint32 *left;
    uint32 *right;
    uint32 *top;
    uint32 *bottom;
    int imageWidth;
    int imageHeight;
} Skybox;


void LoadSkybox(Skybox *skybox, const char *directory);
void DestroySkybox(Skybox *skybox);
Color SampleSkybox(const Skybox *skybox, const float3 rayDir);

#endif // SKYBOX_H