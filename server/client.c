#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	char *data;
	size_t size;
} ResponseBuffer;

static size_t writeResponse(void *ptr, size_t size, size_t nmemb, void *userdata) {
	ResponseBuffer *buffer = (ResponseBuffer *)userdata;
	size_t chunkSize = size * nmemb;
	char *newData = realloc(buffer->data, buffer->size + chunkSize + 1);
	if (newData == NULL) {
		return 0;
	}
	buffer->data = newData;
	memcpy(buffer->data + buffer->size, ptr, chunkSize);
	buffer->size += chunkSize;
	buffer->data[buffer->size] = '\0';
	return chunkSize;
}

static int gCurlGlobalInitialized = 0;

static int runHttpJsonRequest(CURL *curl, const char *url, const char *jsonData, int isPost, char **responseJson, long *httpStatusCode) {
	if (curl == NULL || url == NULL) {
		return -1;
	}

	if (responseJson != NULL) {
		*responseJson = NULL;
	}
	if (httpStatusCode != NULL) {
		*httpStatusCode = 0;
	}

	ResponseBuffer buffer;
	buffer.data = malloc(1);
	buffer.size = 0;
	if (buffer.data == NULL) {
		return -1;
	}
	buffer.data[0] = '\0';

	struct curl_slist *headers = NULL;

	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	if (isPost) {
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData != NULL ? jsonData : "{}");
	}

	CURLcode res = curl_easy_perform(curl);
	if (res == CURLE_OK && httpStatusCode != NULL) {
		(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpStatusCode);
	}

	if (headers != NULL) {
		curl_slist_free_all(headers);
	}

	if (res != CURLE_OK) {
		free(buffer.data);
		return -1;
	}

	if (responseJson != NULL) {
		*responseJson = buffer.data;
	} else {
		free(buffer.data);
	}

	return 0;
}

int curlGetJson(CURL *curl, const char *url, char **responseJson, long *httpStatusCode) {
	return runHttpJsonRequest(curl, url, NULL, 0, responseJson, httpStatusCode);
}

int curlPostJson(CURL *curl, const char *url, const char *jsonData, char **responseJson, long *httpStatusCode) {
	return runHttpJsonRequest(curl, url, jsonData, 1, responseJson, httpStatusCode);
}

void freeHttpResponseJson(char *responseJson) {
	free(responseJson);
}

void initLocalState(LocalState *state, const char *serverIp, const char *serverPort, Object *playerObject, char *modelFileName) {
	if (state == NULL || playerObject == NULL || modelFileName == NULL) {
		return;
	}

	if (!gCurlGlobalInitialized) {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
			gCurlGlobalInitialized = 1;
		}
	}

	memset(state, 0, sizeof(*state));

	if (serverIp != NULL) {
		strncpy(state->address.ip, serverIp, sizeof(state->address.ip) - 1);
		state->address.ip[sizeof(state->address.ip) - 1] = '\0';
	}
	if (serverPort != NULL) {
		strncpy(state->address.port, serverPort, sizeof(state->address.port) - 1);
		state->address.port[sizeof(state->address.port) - 1] = '\0';
	}

	state->objectCount = 0;
	state->capacity = 16;
	state->Objects = malloc(sizeof(ObjectSyncData) * state->capacity);
	if (state->Objects == NULL) {
		state->capacity = 0;
	}

	state->playerObject.id = playerObject->id;
	state->playerObject.timeStamp = (uint32)time(NULL);
	strncpy(state->playerObject.fileName, modelFileName, sizeof(state->playerObject.fileName) - 1);
	state->playerObject.fileName[sizeof(state->playerObject.fileName) - 1] = '\0';
	state->playerObject.position = playerObject->position;
	state->playerObject.rotation = playerObject->rotation;
	state->playerObject.scale = playerObject->scale;

	state->curl = curl_easy_init();
}

void postLocalState(LocalState *state, Object *playerObject) {
	if (state == NULL || playerObject == NULL || state->curl == NULL) {
		return;
	}

	state->playerObject.position = playerObject->position;
	state->playerObject.rotation = playerObject->rotation;
	state->playerObject.scale = playerObject->scale;
	state->playerObject.timeStamp = (uint32)time(NULL);

	char url[64];
	int urlLen = snprintf(url, sizeof(url), "http://%s:%s/addObject", state->address.ip, state->address.port);
	if (urlLen <= 0 || urlLen >= (int)sizeof(url)) {
		return;
	}

	char payload[512];
	int payloadLen = snprintf(
		payload,
		sizeof(payload),
		"{\"id\":%u,\"time_stamp\":%u,\"file_name\":\"%s\","
		"\"position\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
		"\"rotation\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
		"\"scale\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}}",
		(unsigned int)state->playerObject.id,
		(unsigned int)state->playerObject.timeStamp,
		state->playerObject.fileName,
		state->playerObject.position.x,
		state->playerObject.position.y,
		state->playerObject.position.z,
		state->playerObject.rotation.x,
		state->playerObject.rotation.y,
		state->playerObject.rotation.z,
		state->playerObject.scale.x,
		state->playerObject.scale.y,
		state->playerObject.scale.z);
	if (payloadLen <= 0 || payloadLen >= (int)sizeof(payload)) {
		return;
	}

	(void)curlPostJson(state->curl, url, payload, NULL, NULL);
}
