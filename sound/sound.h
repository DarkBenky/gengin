#include "../deps/cute_headers/cute_sound.h"
#include <stdio.h>
#include "../object/format.h"

// typedef struct SoundObject {
//     ...
// } SoundObject;

typedef struct SoundObject {
    cs_audio_source_t *audio;
    cs_playing_sound_t sound;
    float3 position; // position in 3D space
    float3 velocity; // used for changing sound based on movement
    float volume; // 0 to 1
    float pan; // 0 (left) to 1 (right)
    bool looped;
} SoundObject;
typedef struct SoundSystem {
    SoundObject *sounds;
    int soundCount;
    int soundCapacity;
} SoundSystem;

void initSoundSystem(SoundSystem *system) {
    cs_error_t err = cs_init(44100, NULL);
    if (err != CUTE_SOUND_ERROR_NONE) {
        printf("cs_init failed: %s\n", cs_error_as_string(err));
        // Handle error appropriately
    }
}