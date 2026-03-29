#include "server.h"

void UpdateObjectState(ServerObject *object, float3 newPosition, float3 newRotation, float3 newScale, const char *filename) {
	if (object == NULL) {
		fprintf(stderr, "Error: object pointer is NULL\n");
		return;
	}

	object->prevPosition = object->position;
	object->prevRotation = object->rotation;
	object->prevTimeStemp = object->timeStemp;

	object->position = newPosition;
	object->rotation = newRotation;
	object->scale = newScale;
	strncpy(object->filename, filename, sizeof(object->filename) - 1);
	object->filename[sizeof(object->filename) - 1] = '\0';
	object->timeStemp = (uint32)time(NULL);
}

void GetObjectPosition(ServerObject *object, Response *response) {
	// interpolate position, rotation, and scale based on timeStemp
	if (object == NULL || response == NULL) {
		fprintf(stderr, "Error: object or response pointer is NULL\n");
		return;
	}

	uint32 currentTime = (uint32)time(NULL);
	float t = (float)(currentTime - object->prevTimeStemp) / ((float)(object->timeStemp - object->prevTimeStemp) + 1e-6f);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;

	// interpolate position, rotation, and scale
	response->position.x = object->prevPosition.x + t * (object->position.x - object->prevPosition.x);
	response->position.y = object->prevPosition.y + t * (object->position.y - object->prevPosition.y);
	response->position.z = object->prevPosition.z + t * (object->position.z - object->prevPosition.z);

	response->rotation.x = object->prevRotation.x + t * (object->rotation.x - object->prevRotation.x);
	response->rotation.y = object->prevRotation.y + t * (object->rotation.y - object->prevRotation.y);
	response->rotation.z = object->prevRotation.z + t * (object->rotation.z - object->prevRotation.z);

	response->scale.x = object->scale.x;
	response->scale.y = object->scale.y;
	response->scale.z = object->scale.z;

	strncpy(response->filename, object->filename, sizeof(response->filename) - 1);
	response->filename[sizeof(response->filename) - 1] = '\0';
	response->timeStemp = currentTime;
}

void ServerInit(Server *server, int port) {
	server->port = port;
	server->serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (server->serverFd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(server->serverFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind");
		close(server->serverFd);
		exit(EXIT_FAILURE);
	}

	const int initCapacity = 32;
	server->objects = malloc(sizeof(ServerObject) * initCapacity); // initial capacity for 32 objects
	server->length = 0;
	server->capacity = initCapacity;
}

static ServerObject *findObjectById(Server *server, uint32 id) {
	for (int i = 0; i < server->length; i++) {
		if (server->objects[i].id == id) return &server->objects[i];
	}
	return NULL;
}

uint32 AddObject(Server *server, float3 position, float3 rotation, float3 scale, const char *filename, bool persistent) {
	if (server->length >= server->capacity) {
		server->capacity *= 2;
		server->objects = realloc(server->objects, sizeof(ServerObject) * server->capacity);
		if (!server->objects) {
			perror("realloc");
			exit(EXIT_FAILURE);
		}
	}
	static uint32 nextId = 1;
	ServerObject *obj = &server->objects[server->length++];
	obj->id = nextId++;
	obj->position = position;
	obj->prevPosition = position;
	obj->rotation = rotation;
	obj->prevRotation = rotation;
	obj->scale = scale;
	strncpy(obj->filename, filename, sizeof(obj->filename) - 1);
	obj->filename[sizeof(obj->filename) - 1] = '\0';
	obj->timeStemp = (uint32)time(NULL);
	obj->prevTimeStemp = obj->timeStemp;
	obj->persistent = persistent;
	return obj->id;
}

void RemoveObject(Server *server, uint32 id) {
	for (int i = 0; i < server->length; i++) {
		if (server->objects[i].id == id) {
			server->objects[i] = server->objects[--server->length];
			return;
		}
	}
}

void RemoveOldObjects(Server *server, uint32 maxAge) {
	uint32 currentTime = (uint32)time(NULL);
	for (int i = server->length - 1; i >= 0; i--) {
		if (!server->objects[i].persistent && (currentTime - server->objects[i].timeStemp) > maxAge) {
			server->objects[i] = server->objects[--server->length];
		}
	}
}

bool idExists(Server *server, uint32 id) {
	return findObjectById(server, id) != NULL;
}

void ServerStart(Server *server) {
	if (listen(server->serverFd, 3) < 0) {
		perror("listen");
		close(server->serverFd);
		exit(EXIT_FAILURE);
	}

	time_t tpsTimer = time(NULL);
	int counter = 0;
	while (true) {
		int addrlen = sizeof(struct sockaddr_in);
		struct sockaddr_in clientAddress;

		int newSocket = accept(server->serverFd, (struct sockaddr *)&clientAddress, (socklen_t *)&addrlen);
		if (newSocket < 0) {
			perror("accept");
			close(server->serverFd);
			exit(EXIT_FAILURE);
		}

		Request request;
		int bytesRead = read(newSocket, &request, sizeof(Request));
		if (bytesRead != (int)sizeof(Request)) {
			if (bytesRead < 0) perror("read");
			close(newSocket);
			continue;
		}

		if (request.type == GET) {
			int count = 0;
			for (int i = 0; i < server->length; i++) {
				if (!server->objects[i].persistent) count++;
			}
			int bufSize = (int)(sizeof(uint32) + count * sizeof(Response));
			char *responses = malloc(bufSize);
			if (!responses) {
				close(newSocket);
				continue;
			}
			uint32 objectCount = (uint32)count;
			memcpy(responses, &objectCount, sizeof(uint32));
			int idx = 0;
			for (int i = 0; i < server->length; i++) {
				if (server->objects[i].persistent) continue;
				Response response;
				response.id = server->objects[i].id;
				GetObjectPosition(&server->objects[i], &response);
				memcpy(responses + sizeof(uint32) + idx * sizeof(Response), &response, sizeof(Response));
				idx++;
			}
			write(newSocket, responses, bufSize);
			free(responses);
		} else if (request.type == POST) {
			uint32 responseId = 0;
			ServerObject *obj = findObjectById(server, request.id);
			if (obj) {
				if (request.filename[0] == '\0') {
					RemoveObject(server, request.id);
				} else {
					UpdateObjectState(obj, request.position, request.rotation, request.scale, request.filename);
				}
			} else {
				// id == 0 or unknown: create new per-player object
				responseId = AddObject(server, request.position, request.rotation, request.scale, request.filename, false);
			}
			write(newSocket, &responseId, sizeof(uint32));
		}
		close(newSocket);
		counter++;
		if (counter % 256 == 0) {
			time_t now = time(NULL);
			double elapsed = difftime(now, tpsTimer) + 1e-6;
			printf("TPS: %.2f\n", 256.0 / elapsed);
			tpsTimer = now;
			counter = 0;
		}
		RemoveOldObjects(server, 10);
	}
}

void ServerStop(Server *server) {
	close(server->serverFd);
	free(server->objects);
}