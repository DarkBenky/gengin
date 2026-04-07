#include "client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Wire protocol:
//   send: uint32 total_size (type byte + data), uint8 type, data
//   recv: uint32 size, data

static ClientResponse sendRequest(const Client *c, uint8 type, const char *data, uint32 size) {
	ClientResponse res = {NULL, 0};

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return res;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(c->port);
	if (inet_pton(AF_INET, c->host, &addr.sin_addr) <= 0) {
		perror("inet_pton");
		close(fd);
		return res;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(fd);
		return res;
	}

	uint32 total = 1 + size;
	send(fd, &total, sizeof(total), 0);
	send(fd, &type, sizeof(type), 0);
	if (size > 0) send(fd, data, size, 0);

	uint32 rsize = 0;
	ssize_t nr = recv(fd, &rsize, sizeof(rsize), MSG_WAITALL);
	if (nr == (ssize_t)sizeof(rsize) && rsize > 0) {
		res.data = malloc(rsize);
		if (res.data) {
			nr = recv(fd, res.data, rsize, MSG_WAITALL);
			res.size = (uint32)nr;
		}
	}

	close(fd);
	return res;
}

ClientResponse clientGet(const Client *c, const char *data, uint32 size) {
	return sendRequest(c, 0, data, size);
}

ClientResponse clientPost(const Client *c, const char *data, uint32 size) {
	return sendRequest(c, 1, data, size);
}

void clientFreeResponse(ClientResponse *res) {
	free(res->data);
	res->data = NULL;
	res->size = 0;
}
