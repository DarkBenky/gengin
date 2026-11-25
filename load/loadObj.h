#ifndef LOADOBJ_H
#define LOADOBJ_H

#include "../object/format.h"

#include "../object/scene.h"
#include "../object/material/material.h"


typedef struct Object Object;
typedef struct MaterialLib MaterialLib;


typedef struct FaceMap {
    uint32 FileSize;
    uint32 SizeOfTexture;

    float3 *FrontTexture; // SizeOfTexture * SizeOfTexture * 3 floats
    float3 *BackTexture;
    float3 *RightTexture;
    float3 *LeftTexture;
    float3 *TopTexture;
    float3 *BottomTexture;

} FaceMap;


void LoadObj(const char *filename, Object *obj, MaterialLib *lib);
Error SaveObj(const char *filename, Object *obj, MaterialLib *lib, float boost);

Error SaveSceneToJson(const ObjectList *scene, const char *filename);

#endif // LOADOBJ_H