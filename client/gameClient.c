#include "client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../object/format.h"
#include "../object/scene.h"
#include "../load/loadObj.h"
#include "../hexDump/hexDump.h"

#define pathF16 "assets/models/f16.bin"
#define pathR27 "assets/models/r27.bin"

typedef enum {
	MODEL_F16 = 1,
	MODEL_R27 = 2,
} ModelType;

// upper 8 bits = ModelType, lower 24 bits = random instance (seeded per-process by PID)
static inline uint32 generateId(ModelType model) {
	return ((uint32)model << 24) | ((uint32)rand() & 0x00FFFFFF);
}

static inline ModelType idModel(uint32 id) {
	return (ModelType)(id >> 24);
}

static inline char *modelTypeToString(ModelType model) {
	switch (model) {
		case MODEL_F16: return "F16";
		case MODEL_R27: return "R27";
		default: return "Unknown";
	}
}

static inline char *modelTypeToPath(ModelType model) {
	switch (model) {
		case MODEL_F16: return pathF16;
		case MODEL_R27: return pathR27;
		default: return NULL;
	}
}

static inline uint32 idInstance(uint32 id) {
	return id & 0x00FFFFFF;
}

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
	Object **Objects;
	uint32 count;
	uint32 capacity;
} idRegister;

void idRegister_Init(idRegister *reg, uint32 initialCapacity) {
	reg->count = 0;
	reg->capacity = initialCapacity;
	reg->Ids = initialCapacity ? malloc(initialCapacity * sizeof(uint32)) : NULL;
	reg->Objects = initialCapacity ? malloc(initialCapacity * sizeof(Object *)) : NULL;
}

void idRegister_Add(idRegister *reg, uint32 Id, Object *obj) {
	if (reg->count >= reg->capacity) {
		reg->capacity = reg->capacity ? reg->capacity * 2 : 4;
		reg->Ids = realloc(reg->Ids, reg->capacity * sizeof(uint32));
		reg->Objects = realloc(reg->Objects, reg->capacity * sizeof(Object *));
	}
	reg->Ids[reg->count] = Id;
	reg->Objects[reg->count] = obj;
	reg->count++;
}

void idRegister_Remove(idRegister *reg, uint32 Id) {
	for (uint32 i = 0; i < reg->count; i++) {
		if (reg->Ids[i] == Id) {
			reg->Ids[i] = reg->Ids[reg->count - 1];
			reg->Objects[i] = reg->Objects[reg->count - 1];
			reg->count--;
			return;
		}
	}
}

void idRegister_Free(idRegister *reg) {
	free(reg->Ids);
	free(reg->Objects);
	reg->Ids = NULL;
	reg->Objects = NULL;
	reg->count = reg->capacity = 0;
}

static inline void idRegister_Clear(idRegister *reg) {
	reg->count = 0;
}

void RequestData_Init(RequestData *r, uint32 initialCap) {
	r->numOfObjects = 0;
	r->capacity = initialCap;
	r->objects = initialCap ? malloc(initialCap * sizeof(requestObject)) : NULL;
}

static inline void RequestData_Reset(RequestData *r) {
	r->numOfObjects = 0;
}

void RequestData_Free(RequestData *r) {
	free(r->objects);
	r->objects = NULL;
	r->numOfObjects = r->capacity = 0;
}

void addObjectToRequestData(RequestData *request, const Object *obj, uint32 Id) {
	if (request->numOfObjects >= request->capacity) {
		request->capacity = request->capacity ? request->capacity * 2 : 4;
		request->objects = realloc(request->objects, request->capacity * sizeof(requestObject));
	}

	requestObject *newObj = &request->objects[request->numOfObjects];
	newObj->Id = Id;
	newObj->TimeStamp = (uint32)time(NULL);
	newObj->Position = obj->position;
	newObj->Rotation = obj->rotation;
	newObj->Scale = obj->scale;
	request->numOfObjects++;
}

void addFromRegistry(RequestData *request, const idRegister *reg, uint32 Id) {
	for (uint32 i = 0; i < reg->count; i++) {
		if (reg->Ids[i] == Id) {
			addObjectToRequestData(request, reg->Objects[i], Id);
			return;
		}
	}
}

void addAllFromRegistry(RequestData *request, const idRegister *reg) {
	for (uint32 i = 0; i < reg->count; i++)
		addObjectToRequestData(request, reg->Objects[i], reg->Ids[i]);
}

void postObjects(const Client *c, const RequestData *request) {
	uint32 payloadSize = sizeof(uint32) + request->numOfObjects * sizeof(requestObject);
	char *payload = malloc(payloadSize);
	if (!payload) return;
	memcpy(payload, &request->numOfObjects, sizeof(uint32));
	memcpy(payload + sizeof(uint32), request->objects, request->numOfObjects * sizeof(requestObject));

	ClientResponse res = clientPost(c, payload, payloadSize);
	free(payload);
	printf("[client] POST response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	clientFreeResponse(&res);
}

void getObjects(const Client *c, )

int main(void) {
	srand((unsigned int)getpid());

	ObjectList scene;
	ObjectList_Init(&scene, 0);

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 0);

	idRegister objectRegistry;
	idRegister_Init(&objectRegistry, 16);

	Client c = {.host = "127.0.0.1", .port = 8080};

	Object *planeF16 = ObjectList_Add(&scene);
	uint32 f16Id = generateId(MODEL_F16);
	LoadObj(pathF16, planeF16, &matLib);
	idRegister_Add(&objectRegistry, f16Id, planeF16);
	RequestData request;
	RequestData_Init(&request, 8);
	addFromRegistry(&request, &objectRegistry, f16Id);
	postObjects(&c, &request);
	RequestData_Reset(&request);

	// update object position
	planeF16->position.x += 1.0f;
	addFromRegistry(&request, &objectRegistry, f16Id);
	sleep(1); // simulate time passing
	postObjects(&c, &request);
	RequestData_Reset(&request);

	ClientResponse res = clientGet(&c, "some data", strlen("some data") + 1);
	printf("[client] GET response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	hexDump(res.data, res.size);
	clientFreeResponse(&res);

	sleep(1);
	res = clientGet(&c, "some data", strlen("some data") + 1);
	printf("[client] GET response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	hexDump(res.data, res.size);

	// print response
	uint32 numObjects;
	memcpy(&numObjects, res.data, sizeof(uint32));
	printf("Number of objects in response: %u\n", numObjects);
	for (uint32 i = 0; i < numObjects; i++) {
		requestObject *obj = (requestObject *)(res.data + sizeof(uint32) + i * sizeof(requestObject));
		printf("Object %u: Id=%u, Position=(%.2f, %.2f, %.2f), Rotation=(%.2f, %.2f, %.2f), Scale=(%.2f, %.2f, %.2f)\n",
			i, obj->Id,
			obj->Position.x, obj->Position.y, obj->Position.z,
			obj->Rotation.x, obj->Rotation.y, obj->Rotation.z,
			obj->Scale.x, obj->Scale.y, obj->Scale.z);
		printf("  ModelType: %u, InstanceId: %u\n", idModel(obj->Id), idInstance(obj->Id));
		printf("  ModelType String: %s\n", modelTypeToString(idModel(obj->Id)));
		printf("  ModelType Path: %s\n", modelTypeToPath(idModel(obj->Id)));
	}

	clientFreeResponse(&res);

	RequestData_Free(&request);
	idRegister_Free(&objectRegistry);

    // TODO: handle on get request
    // - if new object that isn't in registry, add to registry and scene
    // - if existing object, update position/rotation/scale in registry and scene
    // - if object in registry but not in request, remove from registry and scene
}
