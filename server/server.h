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

typedef struct {
	uint32 size;
	RequestType type;
	char *data;
} Request;

typedef struct {
	uint32 size;
	char *data;
} Response;

typedef void (*RequestHandler)(const Request *req, Response *res);

typedef struct {
	int server_fd;
	struct sockaddr_in address;
	int addrlen;
	Request currentRequest;
	Response currentResponse;
	RequestHandler handler;
} Server;

void serverSetHandler(Server *s, RequestHandler handler);
void responseWrite(Response *res, const char *data, uint32 size);

void serverInit(Server *s, uint16 port);
void serverRun(Server *s);
void serverShutdown(Server *s);

#endif // SERVER_H