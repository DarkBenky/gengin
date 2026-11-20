#ifndef LOADOBJ_H
#define LOADOBJ_H

#include "../object/format.h"

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


// TODO: **Important** load the FaceMap from same bin file
void LoadObj(const char *filename, Object *obj, MaterialLib *lib);

// TODO: **Important** save the FaceMap to same bin file as the Object
Error SaveObj(const char *filename, Object *obj, MaterialLib *lib, float boost);

#endif // LOADOBJ_H