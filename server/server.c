#include "server.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

void serverSetHandler(Server *s, RequestHandler handler) {
	s->handler = handler;
}

void responseWrite(Response *res, const char *data, uint32 size) {
	free(res->data);
	res->data = malloc(size);
	if (!res->data) return;
	memcpy(res->data, data, size);
	res->size = size;
}

void serverInit(Server *s, uint16 port) {
	s->handler = NULL;
	s->currentRequest.data = NULL;
	s->currentRequest.size = 0;
	s->currentResponse.data = NULL;
	s->currentResponse.size = 0;

	s->server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->server_fd < 0) {
		perror("socket");
		return;
	}

	int opt = 1;
	setsockopt(s->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	s->addrlen = sizeof(s->address);
	s->address.sin_family = AF_INET;
	s->address.sin_addr.s_addr = INADDR_ANY;
	s->address.sin_port = htons(port);

	if (bind(s->server_fd, (struct sockaddr *)&s->address, s->addrlen) < 0) {
		perror("bind");
		close(s->server_fd);
		s->server_fd = -1;
		return;
	}

	if (listen(s->server_fd, 8) < 0) {
		perror("listen");
		close(s->server_fd);
		s->server_fd = -1;
	}
}

void serverRun(Server *s) {
	if (s->server_fd < 0) return;

	while (1) {
		int client_fd = accept(s->server_fd, (struct sockaddr *)&s->address, (socklen_t *)&s->addrlen);
		if (client_fd < 0) {
			if (errno == EINTR) continue;
			perror("accept");
			break;
		}

		// read request size header first
		uint32 incoming_size = 0;
		ssize_t nr = recv(client_fd, &incoming_size, sizeof(incoming_size), MSG_WAITALL);
		if (nr == sizeof(incoming_size) && incoming_size > 0) {
			free(s->currentRequest.data);
			s->currentRequest.data = malloc(incoming_size);
			if (s->currentRequest.data) {
				nr = recv(client_fd, s->currentRequest.data, incoming_size, MSG_WAITALL);
				s->currentRequest.size = (uint32)nr;
				// first byte encodes request type, strip it from data the handler sees
				if (s->currentRequest.size > 0) {
					s->currentRequest.type = (s->currentRequest.data[0] == 1) ? POST : GET;
					s->currentRequest.data++;
					s->currentRequest.size--;
				}
			}
		}

		if (s->handler) {
			s->handler(&s->currentRequest, &s->currentResponse);
		}

		// restore pointer before freeing
		if (s->currentRequest.type == POST || s->currentRequest.type == GET) {
			s->currentRequest.data--;
		}

		if (s->currentResponse.data && s->currentResponse.size > 0) {
			send(client_fd, &s->currentResponse.size, sizeof(s->currentResponse.size), 0);
			send(client_fd, s->currentResponse.data, s->currentResponse.size, 0);
		}

		close(client_fd);
	}
}

void serverShutdown(Server *s) {
	if (s->server_fd >= 0) {
		close(s->server_fd);
		s->server_fd = -1;
	}
	free(s->currentRequest.data);
	s->currentRequest.data = NULL;
	s->currentRequest.size = 0;
	free(s->currentResponse.data);
	s->currentResponse.data = NULL;
	s->currentResponse.size = 0;
}