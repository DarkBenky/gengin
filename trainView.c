#include <MiniFB.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "object/format.h"
#include "object/object.h"
#include "object/scene.h"
#include "render/render.h"
#include "render/cpu/ray.h"
#include "render/color/color.h"
#include "math/vector3.h"
#include "util/threadPool.h"
#include "keyboar/keyboar.h"
#include "client/gameClient.h"

// Camera movement speed in meters per frame — training coords are real-world scale.
#define CAM_SPEED 50.0f

int main(void) {
	srand((uint32)getpid());

	Input input;
	Camera camera;
	// Start well above the plane's initial altitude (5000 m) and slightly behind,
	// looking toward the expected flight volume.
	initCamera(&camera, WIDTH, HEIGHT, 90.0f,
			   (float3){0.0f, 7000.0f, -3000.0f, 0.0f},
			   (float3){0.0f, -0.3f, 1.0f, 0.0f},
			   (float3){0.0f, 1000.0f, 0.0f, 0.0f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 64);

	ObjectList scene;
	ObjectList_Init(&scene, 8);

	// A flat ground quad so the scene is not empty while waiting for the training script.
	Object *ground = ObjectList_Add(&scene);
	CreateCube(ground,
			   (float3){0.0f, -50.0f, 5000.0f, 0.0f},
			   (float3){0.0f, 0.0f, 0.0f, 0.0f},
			   (float3){30000.0f, 10.0f, 30000.0f, 0.0f},
			   (float3){0.35f, 0.40f, 0.35f, 0.0f},
			   &matLib, 0.0f, 0.9f, 0.0f);
	Object_UpdateWorldBounds(ground);

	Client c = {.host = "127.0.0.1", .port = 8080};

	idRegister objectRegistry;
	idRegister_Init(&objectRegistry, 8);

	struct mfb_window *window = mfb_open_ex("Training View", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		ObjectList_Destroy(&scene);
		destroyCamera(&camera);
		return 1;
	}
	mfb_set_target_fps(0);

	ThreadPool *threadPool = poolCreate(32, HEIGHT);
	RayTraceTaskQueue rayTaskQueue;

	int frame = 0;

	while (1) {
		// Sync with training script — get cube positions from game server.
		getObjects(&c, &scene, &matLib, &objectRegistry);

		clearBuffers(&camera);
		const float2 jitterPattern[4] = {
			{1.5f, 1.5f}, {-1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
		camera.jitter = jitterPattern[frame & 3];
		camera.seed   = frame * (int)35527 << 16 | (int)11369;
		frame++;

		Input_Poll(&input, window);
		if (input.keysDown[KB_KEY_ESCAPE]) break;
		if (input.keys[KB_KEY_W]) CameraMoveForward(&camera,  CAM_SPEED);
		if (input.keys[KB_KEY_S]) CameraMoveForward(&camera, -CAM_SPEED);
		if (input.keys[KB_KEY_A]) CameraMoveRight(&camera,   -CAM_SPEED);
		if (input.keys[KB_KEY_D]) CameraMoveRight(&camera,    CAM_SPEED);
		if (input.keys[KB_KEY_Q]) CameraMoveUp(&camera,       CAM_SPEED);
		if (input.keys[KB_KEY_E]) CameraMoveUp(&camera,      -CAM_SPEED);
		if (input.mouse[MOUSE_LEFT])
			CameraRotate(&camera, input.mouseDY * 0.005f, -input.mouseDX * 0.005f);

		RenderSetup(scene.objects, scene.count, &camera);
		RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, threadPool, NULL);

		if (mfb_update(window, camera.framebuffer) != STATE_OK) break;
	}

	mfb_close(window);
	poolDestroy(threadPool);
	ObjectList_Destroy(&scene);
	MaterialLib_Destroy(&matLib);
	idRegister_Free(&objectRegistry);
	destroyCamera(&camera);
	return 0;
}
