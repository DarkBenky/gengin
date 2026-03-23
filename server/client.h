#include "../object/format.h"
#include "../object/object.h"
#include "../object/scene.h"
#include "../load/loadObj.h"

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
} Object;

typedef struct {
    Object playerObject; // The player's own object
    ServerAddress address;
    Object* Objects; // array of Objects
    int objectCount; // number of Objects
    int capacity; // capacity of the Objects array
} LocalState;

void initLocalState(LocalState* state, const char* ip, const char* port);
void postLocalState(LocalState* state);
// pulls and updates local state
// if object with id is not in local state, crete object and load object data from local assets
void getServerState(LocalState* state, ObjectList* objectList, MaterialLib* matLib); 