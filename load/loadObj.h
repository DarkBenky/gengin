#ifndef LOADOBJ_H
#define LOADOBJ_H

#include "../object/format.h"

typedef struct Object Object;
typedef struct MaterialLib MaterialLib;


typedef struct FaceMap {
    uint32 Magic;
    uint32 Version;
    uint32 FileSize;
    uint32 SizeOfTexture;

    float3_no_pad *FrontTexture; // SizeOfTexture * SizeOfTexture * 3 floats
    float3_no_pad *BackTexture;
    float3_no_pad *RightTexture;
    float3_no_pad *LeftTexture;
    float3_no_pad *TopTexture;
    float3_no_pad *BottomTexture;

} FaceMap;

Error LoadFaceMap(const char *filename, Object *obj);

void LoadObj(const char *filename, Object *obj, MaterialLib *lib);

// TODO: **Important** save the FaceMap to same bin file as the Object
Error SaveObj(const char *filename, Object *obj, MaterialLib *lib, float boost);

#endif // LOADOBJ_H