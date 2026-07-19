#include "radarScreen.h"
#include <time.h>

int main(void) {
	const int width = 800;
	const int height = 600;
	uint32 *fb = (uint32 *)malloc(sizeof(uint32) * width * height);

	radarScreenUi radar;
	initRadarUi(fb, width, height, 90, 60, 1000, 50, 50, 700, 500, &radar);

	struct Alphabet alphabet;
	LoadAlphabet(&alphabet, "assets/chars");

	float3 radarPos = {0.0f, 0.0f, 0.0f, 0.0f};
	float3 radarForward = {0.0f, 0.0f, 1.0f, 0.0f};
	float3 radarLeft = {1.0f, 0.0f, 0.0f, 0.0f};
	float3 radarUp = {0.0f, 1.0f, 0.0f, 0.0f};

	float3 targets[] = {
		{50.0f, 0.0f, 200.0f, 0.0f},
		{-30.0f, 20.0f, 150.0f, 0.0f},
		{10.0f, 40.0f, 300.0f, 0.0f},
		{-80.0f, -10.0f, 400.0f, 0.0f},
		{60.0f, -5.0f, 100.0f, 0.0f},
		{0.0f, 0.0f, 500.0f, 0.0f},
	};
	const int numTargets = sizeof(targets) / sizeof(targets[0]);

	drawRadarScreen(&radar, &alphabet);

	for (int i = 0; i < numTargets; i++) {
		addRadarTarget(&radar, targets[i], radarPos, radarForward, radarLeft, radarUp);
	}

	drawRadarScreen(&radar, &alphabet);

	Camera cam = {0};
	cam.framebuffer = fb;
	cam.screenWidth = width;
	cam.screenHeight = height;
	SaveImage("radarScreen/radar_test.bmp", &cam);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	const int iterations = 10000;
	for (int i = 0; i < iterations; i++) {
		drawRadarScreen(&radar, &alphabet);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
	printf("Average time: %.3f ms, %.3f fps\n", elapsed * 1000.0 / iterations, iterations / elapsed);

	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	free(fb);
	return 0;
}
