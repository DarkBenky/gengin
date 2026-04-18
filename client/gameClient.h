#ifndef GAME_CLIENT_H
#define GAME_CLIENT_H

#include "client.h"
#include "../object/scene.h"
#include "../object/material/material.h"

typedef enum {
	MODEL_F16 = 1,
	MODEL_R27 = 2,
} ModelType;

typedef struct {
	uint32 Id;
	uint32 TimeStamp;
	float3 Position;
	float3 Rotation;
	float3 Scale;
} requestObject;

typedef struct {
	uint32 numOfObjects;
	uint32 capacity;
	requestObject *objects;
} RequestData;

typedef struct {
	uint32 *Ids;
	uint32 *Indices;
	uint32 count;
	uint32 capacity;
} idRegister;

void idRegister_Init(idRegister *reg, uint32 initialCapacity);
void idRegister_Add(idRegister *reg, uint32 Id, uint32 sceneIndex);
void idRegister_Remove(idRegister *reg, uint32 Id);
bool idRegister_Contains(const idRegister *reg, uint32 Id);
void idRegister_Free(idRegister *reg);
void idRegister_Clear(idRegister *reg);

void RequestData_Init(RequestData *r, uint32 initialCap);
void RequestData_Reset(RequestData *r);
void RequestData_Free(RequestData *r);

void addObjectToRequestData(RequestData *request, const Object *obj, uint32 Id);
void addFromRegistry(RequestData *request, const idRegister *reg, const ObjectList *scene, uint32 Id);
void addAllFromRegistry(RequestData *request, const idRegister *reg, const ObjectList *scene);

void postObjects(const Client *c, const RequestData *request);
void getObjects(const Client *c, ObjectList *scene, MaterialLib *matLib, idRegister *reg);

#endif // GAME_CLIENT_H
