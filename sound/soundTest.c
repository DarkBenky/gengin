#define CUTE_SOUND_IMPLEMENTATION
#include "sound.h"
#include <stdio.h>
#include <unistd.h>

int main() {
	cs_error_t err = cs_init(44100, NULL);
	if (err != CUTE_SOUND_ERROR_NONE) {
		printf("cs_init failed: %s\n", cs_error_as_string(err));
		return 1;
	}

	cs_audio_source_t *audio = cs_load_wav("sound/samples/planeInside.wav", &err);
	if (!audio) {
		printf("cs_load_wav failed: %s\n", cs_error_as_string(err));
		cs_shutdown();
		return 1;
	}

	printf("Loaded: %d Hz, %d samples, %d channels\n",
		   cs_get_sample_rate(audio),
		   cs_get_sample_count(audio),
		   cs_get_channel_count(audio));

	// Play both channels (center pan)
	cs_sound_params_t params = cs_sound_params_default();
	params.volume = 0.75f;
	params.pan = 0.5f;
	params.looped = true;
	cs_playing_sound_t snd = cs_play_sound(audio, params);

	printf("Playing both channels (pan=0.5) for 3 seconds...\n");
	for (int i = 0; i < 300; i++) {
		cs_update(0.01f);
		usleep(10000);
	}

	// Right channel only
	cs_sound_set_pan(snd, 1.0f);
	printf("Playing right channel only (pan=1.0) for 3 seconds...\n");
	for (int i = 0; i < 300; i++) {
		cs_update(0.01f);
		usleep(10000);
	}

	// Left channel only
	cs_sound_set_pan(snd, 0.0f);
	printf("Playing left channel only (pan=0.0) for 3 seconds...\n");
	for (int i = 0; i < 300; i++) {
		cs_update(0.01f);
		usleep(10000);
	}

    // Fade out over 3 seconds
    printf("Fading out over 3 seconds...\n");
    cs_sound_set_pan(snd, 0.5f);
    for (int i = 0; i < 300; i++) {
        float volume = 0.75f * (1.0f - (float)i / 300.0f);
        cs_sound_set_volume(snd, volume);
        cs_update(0.01f);
        usleep(10000);
    }

	cs_sound_stop(snd);
	printf("Stopped.\n");

	cs_free_audio_source(audio);
	cs_shutdown();
	printf("Done.\n");
	return 0;
}