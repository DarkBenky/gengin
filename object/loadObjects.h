/* Example input
{
  "version": 1,
  "objects": [
    {
      "type": "shape",
      "shape": "cube",
      "position": [0, 1, 0],
      "rotation": [0, 45, 0],
      "scale": [1, 1, 1],
      "color": [1.0, 0.2, 0.2],
      "emission": 0.0,
      "roughness": 0.5,
      "metallic": 0.0
    },
    {
      "type": "shape",
      "shape": "sphere",
      "position": [3, 1, 0],
      "rotation": [0, 0, 0],
      "scale": [1, 1, 1],
      "color": [1.0, 1.0, 1.0],
      "emission": 10.0,
      "roughness": 1.0,
      "metallic": 0.0
    },
    {
      "type": "object",
      "path": "models/tree.obj",
      "position": [-4, 0, 2],
      "rotation": [0, 90, 0],
      "scale": [2, 2, 2]
    }
  ]
}
*/

#ifndef LOADOBJECTS_H
#define LOADOBJECTS_H

#include "object.h"

typedef enum {
    OBJ_SHAPE = 0,
    OBJ_MODEL = 1,
} ObjType;

typedef enum {
    SHAPE_CUBE, SHAPE_SPHERE, SHAPE_SPHERE_HIGH, SHAPE_PLANE,
    SHAPE_CONE, SHAPE_CAPSULE, SHAPE_TORUS, SHAPE_DISK,
    SHAPE_PYRAMID, SHAPE_PRISM, SHAPE_HEMISPHERE, SHAPE_TUBE,
    SHAPE_QUAD, SHAPE_UV_SPHERE, SHAPE_ICOSPHERE, SHAPE_CYLINDER,
    SHAPE_COUNT
} ShapeKind;

typedef struct {
    float3 color;
    float emission;
    float roughness;
    float metallic;
} ObjectMaterial;

typedef struct {
    ObjType type;
    float3 position;
    float3 rotation;  // degrees, XYZ order
    float3 scale;
    union {
        ShapeKind shape;   // OBJ_SHAPE
        char path[256];    // OBJ_MODEL, relative to the scene file
    };
    ObjectMaterial material;     // only used for OBJ_SHAPE
} ObjectRecord;

typedef void (*CreateFn)(Object *, float3, float3, float3, float3,
                         MaterialLib *, float, float, float);

static const struct { const char *name; CreateFn fn; } shapeTable[SHAPE_COUNT] = {
    [SHAPE_CUBE]        = { "cube",        CreateCube },
    [SHAPE_SPHERE]      = { "sphere",      CreateSphere },
    [SHAPE_SPHERE_HIGH] = { "sphereHigh",  CreateSphereHighResolution },
    [SHAPE_PLANE]       = { "plane",       CreatePlane },
    [SHAPE_CONE]        = { "cone",        CreateCone },
    [SHAPE_CAPSULE]     = { "capsule",     CreateCapsule },
    [SHAPE_TORUS]       = { "torus",       CreateTorus },
    [SHAPE_DISK]        = { "disk",        CreateDisk },
    [SHAPE_PYRAMID]     = { "pyramid",     CreatePyramid },
    [SHAPE_PRISM]       = { "prism",       CreatePrism },
    [SHAPE_HEMISPHERE]  = { "hemisphere",  CreateHemisphere },
    [SHAPE_TUBE]        = { "tube",        CreateTube },
    [SHAPE_QUAD]        = { "quad",        CreateQuad },
    [SHAPE_UV_SPHERE]   = { "uvSphere",    CreateUVSphere },
    [SHAPE_ICOSPHERE]   = { "icosphere",   CreateIcosphere },
    [SHAPE_CYLINDER]    = { "cylinder",    CreateCylinder },
};

static inline int findShape(const char *name) {
    for (int i = 0; i < SHAPE_COUNT; i++)
        if (strcmp(shapeTable[i].name, name) == 0) return i;
    return -1;
}

static inline void spawnShape(Object *obj, const ObjectRecord *r, MaterialLib *lib) {
    if ((unsigned)r->shape >= SHAPE_COUNT) return;
    const ObjectMaterial *m = &r->material;
    shapeTable[r->shape].fn(obj, r->position, r->rotation, r->scale,
                            m->color, lib, m->emission, m->roughness, m->metallic);
    Object_UpdateWorldBounds(obj);
}

// TODO: implement parsing of the json input
// TODO: implement saving of the scene to json file

#endif