#include "client.h"
#include <stdio.h>
#include <string.h>

int main(void) {
	Client c = {.host = "127.0.0.1", .port = 8080};

	// GET example
	const char *getPayload = "hello server";
	ClientResponse res = clientGet(&c, getPayload, (uint32)strlen(getPayload) + 1);
	printf("[client] GET response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	clientFreeResponse(&res);

	// POST example
	const char *postPayload = "some post data";
	res = clientPost(&c, postPayload, (uint32)strlen(postPayload) + 1);
	printf("[client] POST response (%u bytes): %s\n", res.size, res.data ? res.data : "(empty)");
	clientFreeResponse(&res);

	return 0;
}
