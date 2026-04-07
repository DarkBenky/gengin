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

int main(void) {
	srand((unsigned int)getpid());

	ObjectList scene;
	ObjectList_Init(&scene, 0);

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 0);

	Client c = {.host = "127.0.0.1", .port = 8080};

	Object *planeF16 = ObjectList_Add(&scene);
	uint32 f16Id = generateId(MODEL_F16);
	LoadObj(pathF16, planeF16, &matLib);

	RequestData request;
	RequestData_Init(&request, 8);
	addObjectToRequestData(&request, planeF16, f16Id);
	postObjects(&c, &request);
	RequestData_Reset(&request);

	// update object position
	planeF16->position.x += 1.0f;
	addObjectToRequestData(&request, planeF16, f16Id);
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
    clientFreeResponse(&res);


	RequestData_Free(&request);
}
