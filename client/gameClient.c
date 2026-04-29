#include "gameClient.h"
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

// upper 8 bits = ModelType, lower 24 bits = random instance (seeded per-process by PID)
uint32 generateId(ModelType model) {
	return ((uint32)model << 24) | ((uint32)rand() & 0x00FFFFFF);
}

static inline ModelType idModel(uint32 id) {
	return (ModelType)(id >> 24);
}

static inline char *modelTypeToString(ModelType model) {
	switch (model) {
	case MODEL_F16:          return "F16";
	case MODEL_R27:          return "R27";
	case MODEL_CUBE_PLANE:   return "CubePlane";
	case MODEL_CUBE_TARGET:  return "CubeTarget";
	case MODEL_CUBE_START:   return "CubeStart";
	default:                 return "Unknown";
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

void idRegister_Clear(idRegister *reg) {
	reg->count = 0;
}

void RequestData_Init(RequestData *r, uint32 initialCap) {
	r->numOfObjects = 0;
	r->capacity = initialCap;
	r->objects = initialCap ? malloc(initialCap * sizeof(requestObject)) : NULL;
}

void RequestData_Reset(RequestData *r) {
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
	printf("[client] GET response (%u bytes)\n", res.size);
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
					float alpha = 0.2f;
					o->position.x += (obj->Position.x - o->position.x) * alpha;
					o->position.y += (obj->Position.y - o->position.y) * alpha;
					o->position.z += (obj->Position.z - o->position.z) * alpha;
					o->rotation.x += (obj->Rotation.x - o->rotation.x) * alpha;
					o->rotation.y += (obj->Rotation.y - o->rotation.y) * alpha;
					o->rotation.z += (obj->Rotation.z - o->rotation.z) * alpha;
					o->scale.x += (obj->Scale.x - o->scale.x) * alpha;
					o->scale.y += (obj->Scale.y - o->scale.y) * alpha;
					o->scale.z += (obj->Scale.z - o->scale.z) * alpha;
					Object_UpdateWorldBounds(o);
					seen[j] = true;
					break;
				}
			}
		} else {
			// new object — load model or create cube and add to scene and registry
			printf("New object detected with Id=%u, loading model...\n", obj->Id);
			ModelType mtype = idModel(obj->Id);
			const char *path = modelTypeToPath(mtype);
			uint32 sceneIndex = (uint32)scene->count;
			Object *newObj = ObjectList_Add(scene);

			if (path) {
				LoadObj(path, newObj, matLib);
				CreateObjectBVH(newObj, &newObj->bvh);
				newObj->position = obj->Position;
				newObj->rotation = obj->Rotation;
				newObj->scale = obj->Scale;
				Object_UpdateWorldBounds(newObj);
				idRegister_Add(reg, obj->Id, sceneIndex);
				seen = realloc(seen, reg->count * sizeof(bool));
				seen[reg->count - 1] = true;
			} else {
				float3 color = {0.5f, 0.5f, 0.5f, 0.0f};
				if (mtype == MODEL_CUBE_PLANE)  color = (float3){0.1f, 0.4f, 0.9f, 0.0f};
				if (mtype == MODEL_CUBE_TARGET) color = (float3){0.9f, 0.1f, 0.1f, 0.0f};
				if (mtype == MODEL_CUBE_START)  color = (float3){0.9f, 0.9f, 0.9f, 0.0f};
				CreateCube(newObj, obj->Position, obj->Rotation, obj->Scale, color, matLib, 0.0f, 0.6f, 0.0f);
				Object_UpdateWorldBounds(newObj);
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

#ifdef GAME_CLIENT_STANDALONE
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

	// simulate a second client posting a new R27 object
	Client c2 = {.host = "127.0.0.1", .port = 8080};

	ObjectList scene2;
	ObjectList_Init(&scene2, 0);
	MaterialLib matLib2;
	MaterialLib_Init(&matLib2, 0);

	Object *planeR27 = ObjectList_Add(&scene2);
	uint32 r27Id = generateId(MODEL_R27);
	LoadObj(pathR27, planeR27, &matLib2);

	idRegister c2Registry;
	idRegister_Init(&c2Registry, 4);
	idRegister_Add(&c2Registry, r27Id, 0);

	RequestData c2Request;
	RequestData_Init(&c2Request, 4);
	addFromRegistry(&c2Request, &c2Registry, &scene2, r27Id);
	postObjects(&c2, &c2Request);

	RequestData_Free(&c2Request);
	idRegister_Free(&c2Registry);

	sleep(1);
	getObjects(&c, &scene, &matLib, &objectRegistry);
	printf("[client] Scene objects count: %u\n", scene.count);
	for (uint32 i = 0; i < scene.count; i++) {
		Object *o = &scene.objects[i];
		// check if bvh and triangles were loaded
		if (o->bvh.nodes != nil && o->bvh.nodeCount > 0 && o->v1 != nil && o->v2 != nil && o->v3 != nil && o->triangleCount > 0) {
			printf("[SUCCESS] %d Triangles loaded\n", o->triangleCount);
		} else {
			printf("[FAIL] Object %u has invalid geometry data\n", i);
			if (o->bvh.nodes == nil || o->bvh.nodeCount == 0)
				printf("  BVH not loaded\n");
			if (o->v1 == nil || o->v2 == nil || o->v3 == nil || o->triangleCount == 0)
				printf("  Triangle data not loaded\n");
		}
		// check if materials are loaded
		if (matLib.count > 0) {
			printf("[SUCCESS] MaterialLib has %d materials\n", matLib.count);
		} else {
			printf("[FAIL] MaterialLib is empty\n");
		}
	}

	// short test how many GET/POST we can do in 30sec
	float totalTime = 0.0f;
	int iterations = 0;
	while (totalTime < 30.0f) {
		clock_t start = clock();
		getObjects(&c, &scene, &matLib, &objectRegistry);
		RequestData_Reset(&request);
		for (uint32 i = 0; i < scene.count; i++) {
			addObjectToRequestData(&request, &scene.objects[i], objectRegistry.Ids[i]);
		}
		postObjects(&c, &request);
		clock_t end = clock();
		float elapsed = (float)(end - start) / CLOCKS_PER_SEC;
		totalTime += elapsed;
		iterations++;
	}
	printf("[client] Completed %d GET/POST iterations average iteration time: %.2fms\n", iterations, (totalTime / iterations) * 1000.0f);

	sleep(15);
	getObjects(&c, &scene, &matLib, &objectRegistry);
	printf("[client] Scene objects count: %u\n", scene.count);

	RequestData_Free(&request);
	idRegister_Free(&objectRegistry);
}
#endif // GAME_CLIENT_STANDALONE
