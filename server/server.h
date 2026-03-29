#ifndef SERVER_H
#define SERVER_H

#include "../object/format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

typedef enum {
	GET,
	POST
} RequestType;

typedef struct ServerObject {
	uint32 id;
	uint32 timeStemp;
	uint32 prevTimeStemp;
	float3 position;
	float3 prevPosition;
	float3 rotation;
	float3 prevRotation;
	float3 scale;
	char filename[256];
	bool persistent; // true = global (map, shared world), false = per-player
} ServerObject;

typedef struct {
	RequestType type;
	uint32 id;
	uint32 timeStemp;
	float3 position;
	float3 rotation;
	float3 scale;
	char filename[256];
} Response;

// for object that should be created once use id = 0 and for objects that should be updated use id > 0, server will assign id to new objects and return it in response so client can use it for future updates, if client wants to delete an object it can send POST with id and empty filename and server will remove the object with that id
typedef Response Request;

void UpdateObjectState(ServerObject *object, float3 newPosition, float3 newRotation, float3 newScale, const char *filename);
void GetObjectPosition(ServerObject *object, Response *response);

typedef struct Server {
	int port;
	int serverFd;
	ServerObject *objects;
	int length;	  // number of objects
	int capacity; // capacity of objects array
} Server;

void ServerInit(Server *server, int port);
void ServerStart(Server *server);
void ServerStop(Server *server);

uint32 AddObject(Server *server, float3 position, float3 rotation, float3 scale, const char *filename, bool persistent);
void RemoveObject(Server *server, uint32 id);

#endif // SERVER_H