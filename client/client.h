#ifndef CLIENT_H
#define CLIENT_H

#include "../object/format.h"
#include <stddef.h>

typedef struct {
	char *data;
	uint32 size;
} ClientResponse;

typedef struct {
	const char *host;
	uint16 port;
} Client;

// Returns response; res.data is NULL on failure. Caller must call clientFreeResponse.
ClientResponse clientGet(const Client *c, const char *data, uint32 size);
ClientResponse clientPost(const Client *c, const char *data, uint32 size);

void clientFreeResponse(ClientResponse *res);

#endif // CLIENT_H
