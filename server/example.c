#include "server.h"
#include <stdio.h>
#include <string.h>

static void onRequest(const Request *req, Response *res) {
	const char *msg = "Hello from gengin server!";
	responseWrite(res, msg, (uint32)strlen(msg) + 1);
	printf("[server] received %u bytes, sent reply\n", req->size);
    printf("[server] request data: %s\n", req->data);
}

int main(void) {
	Server s;
	serverInit(&s, 8080);
	serverSetHandler(&s, onRequest);
	printf("[server] listening on port 8080\n");
	serverRun(&s);
	serverShutdown(&s);
	return 0;
}
