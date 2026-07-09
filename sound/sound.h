#include "../deps/cute_headers/cute_sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../object/format.h"
#include "../math/vector3.h"
#include <time.h>
#include <math.h>

typedef struct SoundObject {
    cs_audio_source_t *audio;
    cs_playing_sound_t sound;
    float3 position; // position in 3D space
    float volume; // 0 to 1
    float pan; // 0 (left) to 1 (right)
    bool looped;
} SoundObject;

typedef struct SoundSystem {
    uint64_t lastCallMs;
    SoundObject *sounds;
    int soundCount;
    int soundCapacity;
} SoundSystem;

static int initSoundSystem(SoundSystem *system, int initialCapacity) {
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

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    system->lastCallMs = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

static int addSoundObject(SoundSystem *system, const char *filePath, float3 position, float volume, float pan, bool looped) {
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

static inline float clamp(float value, float min, float max) {
    return fmaxf(min, fminf(max, value));
}

static void compute3dSound(SoundObject *obj, float3 listenerPosition, float3 listenerForward, float3 listenerUp, float3 listenerRight) {
    float distance = Float3_Length(Float3_Sub(obj->position, listenerPosition));
    float3 direction = Float3_Normalize(Float3_Sub(obj->position, listenerPosition));
    const float referenceDistance = 500.0f; // Reference distance for volume attenuation

    float right = Float3_Dot(direction, listenerRight);
    float forward = Float3_Dot(direction, listenerForward);
    float up = Float3_Dot(direction, listenerUp);
    float azimuth = atan2f(right, forward);
    float pan = clamp(azimuth / M_PI, -1.0f, 1.0f);
    float theta = (pan + 1.0f) * (M_PI / 4.0f);
    float leftVolume = cosf(theta);
    float rightVolume = sinf(theta);

    float behindAmount = clamp(-forward, 0.0f, 1.0f);
    float belowAmount = clamp(-up, 0.0f, 1.0f);
    float directivity = 1.0f - 0.35f * behindAmount - 0.15f * belowAmount;
    float volume = obj->volume * (referenceDistance / (referenceDistance + distance));
    volume *= directivity;

    cs_sound_set_volume(obj->sound, volume);
    cs_sound_set_pan_ex(obj->sound, leftVolume, rightVolume);

    // TODO: Implement Doppler effect based on relative velocity between listener and sound source
}


static void playSounds(SoundSystem *system, float3 listenerPosition, float3 listenerForward,float3 listenerUp, float3 listenerRight) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t nowMs = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    float dt = (float)(nowMs - system->lastCallMs) / 1000.0f;
    system->lastCallMs = nowMs;

    for (int i = 0; i < system->soundCount; i++) {
        compute3dSound(&system->sounds[i], listenerPosition, listenerForward,
                       listenerUp, listenerRight);
    }

    cs_update(dt);
}