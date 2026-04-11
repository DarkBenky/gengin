#include "server.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32 nowMs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

typedef struct {
	uint32 Id;
	uint32 TimeStamp;
	float3 Position;
	float3 Rotation;
	float3 Scale;
} Object;

// DATA Layout of POST request: [uint32 numOfObjects][Object1][Object2]...[ObjectN]
typedef struct {
	uint32 numOfObjects;
	Object *objects;
} RequestData;

typedef struct {
	Object *previousObjects;
	Object *currentObjects;
	uint32 objectCount;
	uint32 objectCapacity;
} ServerState;

void initServerState(ServerState *state) {
	state->objectCount = 0;
	state->objectCapacity = 16;
	state->previousObjects = malloc(state->objectCapacity * sizeof(Object));
	state->currentObjects = malloc(state->objectCapacity * sizeof(Object));
}

void addObject(ServerState *state, Object obj) {
	if (state->objectCount >= state->objectCapacity) {
		state->objectCapacity *= 2;
		state->previousObjects = realloc(state->previousObjects, state->objectCapacity * sizeof(Object));
		state->currentObjects = realloc(state->currentObjects, state->objectCapacity * sizeof(Object));
	}
	state->previousObjects[state->objectCount] = obj;
	state->currentObjects[state->objectCount++] = obj;
}

int findObjectIndex(const ServerState *state, uint32 id) {
	for (uint32 i = 0; i < state->objectCount; i++) {
		if (state->currentObjects[i].Id == id) {
			return i;
		}
	}
	return -1;
}

void freeServerState(ServerState *state) {
	free(state->previousObjects);
	free(state->currentObjects);
}

void removeOldObjects(ServerState *state) {
	uint32 now = nowMs();
	uint32 newCount = 0;
	for (uint32 i = 0; i < state->objectCount; i++) {
		if (now - state->currentObjects[i].TimeStamp <= 10000) {
			state->previousObjects[newCount] = state->previousObjects[i];
			state->currentObjects[newCount] = state->currentObjects[i];
			newCount++;
		}
	}
	state->objectCount = newCount;
}

void InterpolateObjectsNextPosition(const ServerState *state, uint32 id, Object *updatedObject) {
	int index = findObjectIndex(state, id);
	uint32 now = nowMs();
	updatedObject->Id = id;
	updatedObject->TimeStamp = now;

	if (index < 0) {
		updatedObject->Position.x = updatedObject->Position.y = updatedObject->Position.z = 0.0f;
		updatedObject->Rotation.x = updatedObject->Rotation.y = updatedObject->Rotation.z = 0.0f;
		updatedObject->Scale.x = updatedObject->Scale.y = updatedObject->Scale.z = 0.0f;
		return;
	}
	Object *prev = &state->previousObjects[index];
	Object *curr = &state->currentObjects[index];

	float deltaTime = (float)(curr->TimeStamp - prev->TimeStamp);
	if (deltaTime == 0.0f) {
		updatedObject->Position = curr->Position;
		updatedObject->Rotation = curr->Rotation;
		updatedObject->Scale = curr->Scale;
		return;
	}

	float3 deltaPos = {
		curr->Position.x - prev->Position.x,
		curr->Position.y - prev->Position.y,
		curr->Position.z - prev->Position.z};

	float3 positionChangeRate = {
		deltaPos.x / deltaTime,
		deltaPos.y / deltaTime,
		deltaPos.z / deltaTime};

	float3 deltaRotation = {
		curr->Rotation.x - prev->Rotation.x,
		curr->Rotation.y - prev->Rotation.y,
		curr->Rotation.z - prev->Rotation.z};

	float3 rotationChangeRate = {
		deltaRotation.x / deltaTime,
		deltaRotation.y / deltaTime,
		deltaRotation.z / deltaTime};

	float3 deltaScale = {
		curr->Scale.x - prev->Scale.x,
		curr->Scale.y - prev->Scale.y,
		curr->Scale.z - prev->Scale.z};

	float3 scaleChangeRate = {
		deltaScale.x / deltaTime,
		deltaScale.y / deltaTime,
		deltaScale.z / deltaTime};

	float timeSinceLastUpdate = (float)(now - curr->TimeStamp);

	updatedObject->Position.x = curr->Position.x + positionChangeRate.x * timeSinceLastUpdate;
	updatedObject->Position.y = curr->Position.y + positionChangeRate.y * timeSinceLastUpdate;
	updatedObject->Position.z = curr->Position.z + positionChangeRate.z * timeSinceLastUpdate;

	updatedObject->Rotation.x = curr->Rotation.x + rotationChangeRate.x * timeSinceLastUpdate;
	updatedObject->Rotation.y = curr->Rotation.y + rotationChangeRate.y * timeSinceLastUpdate;
	updatedObject->Rotation.z = curr->Rotation.z + rotationChangeRate.z * timeSinceLastUpdate;

	updatedObject->Scale.x = curr->Scale.x + scaleChangeRate.x * timeSinceLastUpdate;
	updatedObject->Scale.y = curr->Scale.y + scaleChangeRate.y * timeSinceLastUpdate;
	updatedObject->Scale.z = curr->Scale.z + scaleChangeRate.z * timeSinceLastUpdate;
}

static ServerState *gState;

static void onRequest(const Request *req, Response *res) {
	ServerState *state = gState;
	if (req->type == GET) {
		removeOldObjects(state);
		Object *out = malloc(state->objectCount * sizeof(Object) + sizeof(uint32));
		memcpy(out, &state->objectCount, sizeof(uint32));
		void *outObjects = (char *)out + sizeof(uint32);
		for (uint32 i = 0; i < state->objectCount; i++)
			InterpolateObjectsNextPosition(state, state->currentObjects[i].Id, &((Object *)outObjects)[i]);
		
		responseWrite(res, (char *)out, state->objectCount * sizeof(Object) + sizeof(uint32));
		free(out);
	} else if (req->type == POST) {
		// get object count and objects data from client and update server state
		uint32 numOfObjects;
		memcpy(&numOfObjects, req->data, sizeof(uint32));
		Object *objects = (Object *)(req->data + sizeof(uint32));

		for (uint32 i = 0; i < numOfObjects; i++) {
			int index = findObjectIndex(state, objects[i].Id);
			if (index >= 0) {
				memcpy(&state->previousObjects[index], &state->currentObjects[index], sizeof(Object));
				state->currentObjects[index] = objects[i];
				state->currentObjects[index].TimeStamp = nowMs();
			} else {
				objects[i].TimeStamp = nowMs();
				addObject(state, objects[i]);
			}
		}
		responseWrite(res, "Objects updated", 15);
	} else {
		responseWrite(res, "Invalid request type", 21);
	}
}

int main(void) {
	ServerState state;
	initServerState(&state);
	gState = &state;
	Server s;
	serverInit(&s, 8080);
	serverSetHandler(&s, onRequest);
	printf("[server] listening on port 8080\n");
	serverRun(&s);
	serverShutdown(&s);
	return 0;
}
