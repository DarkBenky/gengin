#include "../deps/cute_headers/cute_sound.h"
#include "../util/threadPool.h"
#include <stdio.h>
#include <stdlib.h>
#include "../object/format.h"
#include <time.h>

// typedef struct SoundObject {
//     ...
// } SoundObject;

typedef struct SoundObject {
    cs_audio_source_t *audio;
    cs_playing_sound_t sound;
    float3 position; // position in 3D space
    float volume; // 0 to 1
    float pan; // 0 (left) to 1 (right)
    bool looped;
} SoundObject;
typedef struct SoundSystem {
    uint32 timeSinceLastCall;
    ThreadPool *threadPool;
    SoundObject *sounds;
    int soundCount;
    int soundCapacity;
} SoundSystem;

int initSoundSystem(SoundSystem *system, int initialCapacity) {
    cs_error_t err = cs_init(44100, NULL);
    if (err != CUTE_SOUND_ERROR_NONE) {
        printf("cs_init failed: %s\n", cs_error_as_string(err));
        return 1;
    }
    system->sounds = (SoundObject *)malloc(initialCapacity * sizeof(SoundObject));
    if (!system->sounds) {
        printf("Failed to allocate sound array\n");
        cs_shutdown();
        return 1;
    }
    system->soundCount = 0;
    system->soundCapacity = initialCapacity;
    system->threadPool = poolCreate(4, 16);
    system->timeSinceLastCall = time(NULL);
    return 0;
}

int addSoundObject(SoundSystem *system, const char *filePath, float3 position, float volume, float pan, bool looped) {
    if (system->soundCount >= system->soundCapacity) {
        int newCapacity = system->soundCapacity * 2;
        SoundObject *newSounds = (SoundObject *)realloc(system->sounds, newCapacity * sizeof(SoundObject));
        if (!newSounds) {
            printf("Failed to reallocate sound array\n");
            return -1;
        }
        system->sounds = newSounds;
        system->soundCapacity = newCapacity;
    }

    cs_error_t err;
    cs_audio_source_t *audio = cs_load_wav(filePath, &err);
    if (!audio) {
        printf("cs_load_wav failed: %s\n", cs_error_as_string(err));
        return -1;
    }

    cs_sound_params_t params = cs_sound_params_default();
    params.volume = volume;
    params.pan = pan;
    params.looped = looped;

    cs_playing_sound_t sound = cs_play_sound(audio, params);

    SoundObject *obj = &system->sounds[system->soundCount++];
    obj->audio = audio;
    obj->sound = sound;
    obj->position = position;
    obj->volume = volume;
    obj->pan = pan;
    obj->looped = looped;

    return 0; // success
}

