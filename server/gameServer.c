#include "server.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HISTORY_SIZE 4
// a raw second difference over three noisy samples amplifies jitter, hence the damp
#define ACCEL_DAMP 0.5f

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
	uint32 TimeStamp;
	float3 Position;
	float3 Rotation;
	float3 Scale;
} Sample;

typedef struct {
	uint32 Id;
	Sample history[HISTORY_SIZE];
	uint32 head;
	uint32 count;
} ObjectEntry;

typedef struct {
	ObjectEntry *objects;
	uint32 objectCount;
	uint32 objectCapacity;
} ServerState;

void initServerState(ServerState *state) {
	state->objectCount = 0;
	state->objectCapacity = 16;
	state->objects = malloc(state->objectCapacity * sizeof(ObjectEntry));
}

static void pushSample(ObjectEntry *e, const Sample *s) {
	e->history[e->head] = *s;
	e->head = (e->head + 1) % HISTORY_SIZE;
	if (e->count < HISTORY_SIZE)
		e->count++;
}

static const Sample *sampleBack(const ObjectEntry *e, uint32 back) {
	return &e->history[(e->head + HISTORY_SIZE - 1 - back) % HISTORY_SIZE];
}

void addObject(ServerState *state, uint32 id, const Sample *s) {
	if (state->objectCount >= state->objectCapacity) {
		state->objectCapacity *= 2;
		state->objects = realloc(state->objects, state->objectCapacity * sizeof(ObjectEntry));
	}
	ObjectEntry *e = &state->objects[state->objectCount++];
	e->Id = id;
	e->head = 0;
	e->count = 0;
	pushSample(e, s);
}

int findObjectIndex(const ServerState *state, uint32 id) {
	for (uint32 i = 0; i < state->objectCount; i++) {
		if (state->objects[i].Id == id) {
			return i;
		}
	}
	return -1;
}

void freeServerState(ServerState *state) {
	free(state->objects);
}

void removeOldObjects(ServerState *state) {
	uint32 now = nowMs();
	uint32 newCount = 0;
	for (uint32 i = 0; i < state->objectCount; i++) {
		if (now - sampleBack(&state->objects[i], 0)->TimeStamp <= 10000) {
			state->objects[newCount++] = state->objects[i];
		}
	}
	state->objectCount = newCount;
}

static float3 unwrapFloat3(float3 ref, float3 v) {
	while (v.x - ref.x > 180.0f)
		v.x -= 360.0f;
	while (ref.x - v.x > 180.0f)
		v.x += 360.0f;
	while (v.y - ref.y > 180.0f)
		v.y -= 360.0f;
	while (ref.y - v.y > 180.0f)
		v.y += 360.0f;
	while (v.z - ref.z > 180.0f)
		v.z -= 360.0f;
	while (ref.z - v.z > 180.0f)
		v.z += 360.0f;
	return v;
}

static float3 extrapLinearFloat3(float3 p1, uint32 t1, float3 p2, uint32 t2, uint32 now) {
	float h2 = (float)(int32)(t2 - t1);
	if (h2 <= 0.0f)
		return p2;
	float dt = (float)(int32)(now - t2);
	if (dt < 0.0f)
		dt = 0.0f;
	if (dt > h2)
		dt = h2;
	float3 out = {
		p2.x + (p2.x - p1.x) / h2 * dt,
		p2.y + (p2.y - p1.y) / h2 * dt,
		p2.z + (p2.z - p1.z) / h2 * dt};
	return out;
}

static float3 extrapFloat3(float3 p0, uint32 t0, float3 p1, uint32 t1, float3 p2, uint32 t2, uint32 now) {
	float h1 = (float)(int32)(t1 - t0);
	float h2 = (float)(int32)(t2 - t1);
	if (h1 <= 0.0f || h2 <= 0.0f)
		return p2;
	float dt = (float)(int32)(now - t2);
	if (dt < 0.0f)
		dt = 0.0f;
	if (dt > h2)
		dt = h2;
	float3 v1 = {
		(p1.x - p0.x) / h1,
		(p1.y - p0.y) / h1,
		(p1.z - p0.z) / h1};
	float3 v2 = {
		(p2.x - p1.x) / h2,
		(p2.y - p1.y) / h2,
		(p2.z - p1.z) / h2};
	float3 accel = {
		(v2.x - v1.x) * ACCEL_DAMP / (h1 + h2),
		(v2.y - v1.y) * ACCEL_DAMP / (h1 + h2),
		(v2.z - v1.z) * ACCEL_DAMP / (h1 + h2)};
	float3 out = {
		p2.x + v2.x * dt + 0.5f * accel.x * dt * dt,
		p2.y + v2.y * dt + 0.5f * accel.y * dt * dt,
		p2.z + v2.z * dt + 0.5f * accel.z * dt * dt};
	return out;
}

void extrapolateObjectNext(const ServerState *state, uint32 id, Object *updatedObject) {
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
	const ObjectEntry *e = &state->objects[index];
	const Sample *s2 = sampleBack(e, 0);
	if (e->count < 2) {
		updatedObject->Position = s2->Position;
		updatedObject->Rotation = s2->Rotation;
		updatedObject->Scale = s2->Scale;
		return;
	}
	const Sample *s1 = sampleBack(e, 1);
	if (e->count == 2) {
		updatedObject->Position = extrapLinearFloat3(s1->Position, s1->TimeStamp, s2->Position, s2->TimeStamp, now);
		updatedObject->Rotation = extrapLinearFloat3(unwrapFloat3(s2->Rotation, s1->Rotation), s1->TimeStamp, s2->Rotation, s2->TimeStamp, now);
		updatedObject->Scale = extrapLinearFloat3(s1->Scale, s1->TimeStamp, s2->Scale, s2->TimeStamp, now);
		return;
	}
	const Sample *s0 = sampleBack(e, 2);
	updatedObject->Position = extrapFloat3(s0->Position, s0->TimeStamp, s1->Position, s1->TimeStamp, s2->Position, s2->TimeStamp, now);
	updatedObject->Rotation = extrapFloat3(unwrapFloat3(s2->Rotation, s0->Rotation), s0->TimeStamp, unwrapFloat3(s2->Rotation, s1->Rotation), s1->TimeStamp, s2->Rotation, s2->TimeStamp, now);
	updatedObject->Scale = extrapFloat3(s0->Scale, s0->TimeStamp, s1->Scale, s1->TimeStamp, s2->Scale, s2->TimeStamp, now);
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
			extrapolateObjectNext(state, state->objects[i].Id, &((Object *)outObjects)[i]);

		responseWrite(res, (char *)out, state->objectCount * sizeof(Object) + sizeof(uint32));
		free(out);
	} else if (req->type == POST) {
		// get object count and objects data from client and update server state
		uint32 numOfObjects;
		if (req->size < sizeof(uint32)) {
			responseWrite(res, "Invalid request size", 20);
			return;
		}
		memcpy(&numOfObjects, req->data, sizeof(uint32));
		if (numOfObjects > (req->size - sizeof(uint32)) / sizeof(Object)) {
			responseWrite(res, "Invalid request size", 20);
			return;
		}

		for (uint32 i = 0; i < numOfObjects; i++) {
			Object obj;
			memcpy(&obj, req->data + sizeof(uint32) + i * sizeof(Object), sizeof(Object));
			obj.TimeStamp = nowMs();
			Sample s;
			s.TimeStamp = obj.TimeStamp;
			s.Position = obj.Position;
			s.Rotation = obj.Rotation;
			s.Scale = obj.Scale;
			int index = findObjectIndex(state, obj.Id);
			if (index >= 0) {
				pushSample(&state->objects[index], &s);
			} else {
				addObject(state, obj.Id, &s);
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
	serverInit(&s, 8081);
	serverSetHandler(&s, onRequest);
	printf("[server] listening on port 8081\n");
	serverRun(&s);
	serverShutdown(&s);
	return 0;
}
