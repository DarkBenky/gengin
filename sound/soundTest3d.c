#define CUTE_SOUND_IMPLEMENTATION
#include "sound.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

static volatile sig_atomic_t keepRunning = 1;

static void onSigint(int sig) {
	keepRunning = 0;
}

#define COMPASS_SIZE 21 // odd number, center cell = listener
#define COMPASS_RADIUS (COMPASS_SIZE / 2)

// Draws a top-down radar: N = forward (+Z), E = right (+X).
// Each source is placed by azimuth/distance and labeled with its index.
// Elevation is shown as a separate up/down glyph next to the label line.
static void drawCompass(SoundSystem *system, float3 listenerPos,
						float3 listenerForward, float3 listenerUp, float3 listenerRight,
						float orbitRadius, float elapsed) {
	static char grid[COMPASS_SIZE][COMPASS_SIZE];
	for (int y = 0; y < COMPASS_SIZE; y++)
		for (int x = 0; x < COMPASS_SIZE; x++)
			grid[y][x] = (x == COMPASS_RADIUS && y == COMPASS_RADIUS) ? '+' : '.';

	char elevLine[64] = {0};

	for (int i = 0; i < system->soundCount; i++) {
		float3 toSrc = Float3_Sub(system->sounds[i].position, listenerPos);
		float distance = Float3_Length(toSrc);
		float3 dir = Float3_Normalize(toSrc);

		float right = Float3_Dot(dir, listenerRight);
		float forward = Float3_Dot(dir, listenerForward);
		float up = Float3_Dot(dir, listenerUp);

		float azimuth = atan2f(right, forward); // 0 = ahead, +pi/2 = right

		float norm = distance / (orbitRadius * 1.3f);
		if (norm > 1.0f) norm = 1.0f;

		int gx = COMPASS_RADIUS + (int)roundf(sinf(azimuth) * norm * COMPASS_RADIUS);
		int gy = COMPASS_RADIUS - (int)roundf(cosf(azimuth) * norm * COMPASS_RADIUS);

		if (gx >= 0 && gx < COMPASS_SIZE && gy >= 0 && gy < COMPASS_SIZE) {
			grid[gy][gx] = '0' + (i % 10);
		}

		char elevGlyph = (up > 0.3f) ? '^' : (up < -0.3f) ? 'v'
														  : '-';
		char buf[16];
		snprintf(buf, sizeof(buf), "%d:%c ", i, elevGlyph);
		strncat(elevLine, buf, sizeof(elevLine) - strlen(elevLine) - 1);
	}

	// \033[H moves cursor to top-left without clearing scrollback -> no flicker
	printf("\033[H");
	printf("t=%6.1fs   N=forward  ^=above  v=below                        \n\n", elapsed);
	for (int y = 0; y < COMPASS_SIZE; y++) {
		printf("  ");
		for (int x = 0; x < COMPASS_SIZE; x++)
			putchar(grid[y][x]);
		putchar('\n');
	}
	printf("\nElevation: %-60s\n", elevLine);
	fflush(stdout);
}

int main() {
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, onSigint);
	printf("\033[2J");

	printf("Number of sound sources (1-32): ");
	int numSources;
	if (scanf("%d", &numSources) != 1) numSources = 3;
	if (numSources < 1) numSources = 1;
	if (numSources > 32) numSources = 32;

	printf("Orbit radius (50-5000): ");
	float orbitRadius;
	if (scanf("%f", &orbitRadius) != 1) orbitRadius = 500.0f;
	if (orbitRadius < 50.0f) orbitRadius = 50.0f;
	if (orbitRadius > 5000.0f) orbitRadius = 5000.0f;

	SoundSystem system;
	if (initSoundSystem(&system, numSources) != 0) return 1;

	for (int i = 0; i < numSources; i++) {
		float angle = (float)i / numSources * 2.0f * M_PI;
		float r = orbitRadius * (0.5f + 0.5f * (float)i / (numSources > 1 ? numSources - 1 : 1));
		float3 pos = {cosf(angle) * r, (i % 3 - 1) * 40.0f, sinf(angle) * r};
		if (addSoundObject(&system, "sound/samples/pitch.wav", pos, 1.0f, 0.5f, true) != 0) {
			printf("Failed to add sound %d\n", i);
		}
	}

	float3 listenerPos = {0.0f, 0.0f, 0.0f};
	float3 listenerForward = {0.0f, 0.0f, 1.0f};
	float3 listenerUp = {0.0f, 1.0f, 0.0f};
	float3 listenerRight = {1.0f, 0.0f, 0.0f};

	float elapsed = 0.0f;
	const float frameTime = 0.016f;
	const float orbitSpeed = 0.8f;

	for (int frame = 0; keepRunning; frame++) {
		elapsed += frameTime;

		for (int i = 0; i < system.soundCount; i++) {
			float speed = orbitSpeed * (1.0f + i * 0.3f);
			float r = orbitRadius * (0.5f + 0.5f * (float)i / (numSources > 1 ? numSources - 1 : 1));
			float angle = elapsed * speed + (float)i / numSources * 2.0f * M_PI;
			float distMod = r + sinf(elapsed * 0.4f + i) * r * 0.15f;
			system.sounds[i].position.x = cosf(angle) * distMod;
			system.sounds[i].position.z = sinf(angle) * distMod;
			system.sounds[i].position.y = sinf(elapsed * 0.7f + i * 1.3f) * r * 0.2f;
		}

		playSounds(&system, listenerPos, listenerForward, listenerUp, listenerRight);

		// redraw every frame (~60Hz) instead of every 60th -> continuous motion
		drawCompass(&system, listenerPos, listenerForward, listenerUp, listenerRight,
					orbitRadius, elapsed);

		usleep((useconds_t)(frameTime * 1000000));
	}

	cs_shutdown();
	return 0;
}