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
	case MODEL_F16:
		return "F16";
	case MODEL_R27:
		return "R27";
	default:
		return "Unknown";
	}
}

static inline char *modelTypeToPath(ModelType model) {
	switch (model) {
	case MODEL_F16:
		return pathF16;
	case MODEL_R27:
		return pathR27;
	default:
		return NULL;
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
	uint32 *Indices; // indices into ObjectList, stable across realloc
	uint32 count;
	uint32 capacity;
} idRegister;

void idRegister_Init(idRegister *reg, uint32 initialCapacity) {
	reg->count = 0;
	reg->capacity = initialCapacity;
	reg->Ids = initialCapacity ? malloc(initialCapacity * sizeof(uint32)) : NULL;
	reg->Indices = initialCapacity ? malloc(initialCapacity * sizeof(uint32)) : NULL;
}

void idRegister_Add(idRegister *reg, uint32 Id, uint32 sceneIndex) {
	if (reg->count >= reg->capacity) {
		reg->capacity = reg->capacity ? reg->capacity * 2 : 4;
		reg->Ids = realloc(reg->Ids, reg->capacity * sizeof(uint32));
		reg->Indices = realloc(reg->Indices, reg->capacity * sizeof(uint32));
	}
	reg->Ids[reg->count] = Id;
	reg->Indices[reg->count] = sceneIndex;
	reg->count++;
}

void idRegister_Remove(idRegister *reg, uint32 Id) {
	for (uint32 i = 0; i < reg->count; i++) {
		if (reg->Ids[i] == Id) {
			reg->Ids[i] = reg->Ids[reg->count - 1];
			reg->Indices[i] = reg->Indices[reg->count - 1];
			reg->count--;
			return;
		}
	}
}

bool idRegister_Contains(const idRegister *reg, uint32 Id) {
	for (uint32 i = 0; i < reg->count; i++) {
		if (reg->Ids[i] == Id) {
			return true;
		}
	}
	return false;
}

void idRegister_Free(idRegister *reg) {
	free(reg->Ids);
	free(reg->Indices);
	reg->Ids = NULL;
	reg->Indices = NULL;
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

void addFromRegistry(RequestData *request, const idRegister *reg, const ObjectList *scene, uint32 Id) {
	for (uint32 i = 0; i < reg->count; i++) {
		if (reg->Ids[i] == Id) {
			addObjectToRequestData(request, &scene->objects[reg->Indices[i]], Id);
			return;
		}
	}
}

void addAllFromRegistry(RequestData *request, const idRegister *reg, const ObjectList *scene) {
	for (uint32 i = 0; i < reg->count; i++)
		addObjectToRequestData(request, &scene->objects[reg->Indices[i]], reg->Ids[i]);
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

void getObjects(const Client *c, ObjectList *scene, MaterialLib *matLib, idRegister *reg) {
	ClientResponse res = clientGet(c, "get objects", strlen("get objects") + 1);
	uint32 numObjects;
	memcpy(&numObjects, res.data, sizeof(uint32));

	// track which registry entries were seen to detect removals
	bool *seen = calloc(reg->count, sizeof(bool));

	for (uint32 i = 0; i < numObjects; i++) {
		requestObject *obj = (requestObject *)(res.data + sizeof(uint32) + i * sizeof(requestObject));
		printf("Received object from server: Id=%u, Position=(%.2f, %.2f, %.2f), Rotation=(%.2f, %.2f, %.2f), Scale=(%.2f, %.2f, %.2f), ModelName=%s\n",
			   obj->Id,
			   obj->Position.x, obj->Position.y, obj->Position.z,
			   obj->Rotation.x, obj->Rotation.y, obj->Rotation.z,
			   obj->Scale.x, obj->Scale.y, obj->Scale.z,
			   modelTypeToString(idModel(obj->Id)));

		if (idRegister_Contains(reg, obj->Id)) {
			// update existing object transform — safe: indices never invalidate
			for (uint32 j = 0; j < reg->count; j++) {
				if (reg->Ids[j] == obj->Id) {
					Object *o = &scene->objects[reg->Indices[j]];
					o->position = obj->Position;
					o->rotation = obj->Rotation;
					o->scale = obj->Scale;
					seen[j] = true;
					break;
				}
			}
		} else {
			// new object — load model and add to scene and registry
			const char *path = modelTypeToPath(idModel(obj->Id));
			if (path) {
				uint32 sceneIndex = (uint32)scene->count;
				Object *newObj = ObjectList_Add(scene); // may realloc scene->objects
				LoadObj(path, newObj, matLib);
				newObj->position = obj->Position;
				newObj->rotation = obj->Rotation;
				newObj->scale = obj->Scale;
				idRegister_Add(reg, obj->Id, sceneIndex);
				seen = realloc(seen, reg->count * sizeof(bool));
				seen[reg->count - 1] = true;
			}
		}
	}

	// remove objects that were not in the server response
	for (uint32 i = reg->count; i-- > 0;) {
		if (!seen[i])
			idRegister_Remove(reg, reg->Ids[i]);
	}

	free(seen);
	clientFreeResponse(&res);
}

int main(void) {
	srand((unsigned int)getpid());

	ObjectList scene;
	ObjectList_Init(&scene, 0);

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 0);

	idRegister objectRegistry;
	idRegister_Init(&objectRegistry, 16);

	Client c = {.host = "127.0.0.1", .port = 8080};

	uint32 f16SceneIndex = (uint32)scene.count;
	Object *planeF16 = ObjectList_Add(&scene);
	uint32 f16Id = generateId(MODEL_F16);
	LoadObj(pathF16, planeF16, &matLib);
	idRegister_Add(&objectRegistry, f16Id, f16SceneIndex);
	RequestData request;
	RequestData_Init(&request, 8);
	addFromRegistry(&request, &objectRegistry, &scene, f16Id);
	postObjects(&c, &request);
	RequestData_Reset(&request);

	// update object position
	planeF16->position.x += 1.0f;
	addFromRegistry(&request, &objectRegistry, &scene, f16Id);
	sleep(1); // simulate time passing
	postObjects(&c, &request);
	RequestData_Reset(&request);

	ClientResponse res = clientGet(&c, "some data", strlen("some data") + 1);
	printf("[client] GET response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	hexDump(res.data, res.size);
	clientFreeResponse(&res);

	sleep(1);
	getObjects(&c, &scene, &matLib, &objectRegistry);
	sleep(1);
	getObjects(&c, &scene, &matLib, &objectRegistry);

	RequestData_Free(&request);
	idRegister_Free(&objectRegistry);

	// TODO: handle on get request
	// - if new object that isn't in registry, add to registry and scene
	// - if existing object, update position/rotation/scale in registry and scene
	// - if object in registry but not in request, remove from registry and scene
}
