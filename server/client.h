#include "../object/format.h"
#include "../object/object.h"
#include "../object/scene.h"
#include "../load/loadObj.h"
#include <curl/curl.h>

typedef struct {
    char ip[16]; // e.g., "127.0.0.1"
    char port[6]; // e.g., "8080"
} ServerAddress;

typedef struct {
    uint32 id;
    uint32 timeStamp;
    char fileName[32];
    float3 position;
    float3 rotation;
    float3 scale;
} ObjectSyncData;

typedef struct {
    CURL* curl;
    ObjectSyncData playerObject; // The player's own object
    ServerAddress address;
    ObjectSyncData* Objects; // array of Objects
    int objectCount; // number of Objects
    int capacity; // capacity of the Objects array
} LocalState;

// Returns 0 on success, non-zero on transport error.
// If responseJson is non-NULL, a heap string is returned and must be freed with freeHttpResponseJson.
int curlGetJson(CURL* curl, const char* url, char** responseJson, long* httpStatusCode);
int curlPostJson(CURL* curl, const char* url, const char* jsonData, char** responseJson, long* httpStatusCode);
void freeHttpResponseJson(char* responseJson);

void initLocalState(LocalState* state, const char* serverIp, const char* serverPort, Object* playerObject, char* modelFileName);
void postLocalState(LocalState* state, Object *playerObject);
// pulls and updates local state
// if object with id is not in local state, crete object and load object data from local assets
void getServerState(LocalState* state, ObjectList* objectList, MaterialLib* matLib); 