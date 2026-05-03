#include <MiniFB.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "object/format.h"
#include "object/object.h"
#include "object/scene.h"
#include "render/render.h"
#include "render/cpu/font.h"
#include "render/cpu/ray.h"
#include "render/cpu/ssr.h"
#include "render/color/color.h"
#include "load/loadObj.h"
#include "math/vector3.h"
#include "skybox/skybox.h"
#include "util/threadPool.h"
#include "util/bench.h"
#include "keyboar/keyboar.h"
#include "render/gpu/kernels/cloadrendering/cload.h"
#include "client/gameClient.h"
#include "simulation/cSim/import.h"
#include "simulation/cSim/simulate.h"

#define WNOW(ts) clock_gettime(CLOCK_MONOTONIC, &(ts))
#define WDIFF(a, b) ((double)((b).tv_sec - (a).tv_sec) + (double)((b).tv_nsec - (a).tv_nsec) * 1e-9)

#define ACCUMULATE_STATS 1024
#define GRID_COLS 32
#define GRID_ROWS 32

// update render object to sim object and move camera to follow the plane
void SimObjToRenderObj(Plane *simPlane, Object *renderObj, Camera *camera, Input *input, struct mfb_window *window) {
	float3 forward;
	updatePlane(simPlane, 1.0f / 160.0f, &forward);
	printf("Sim plane position: (%.2f, %.2f, %.2f), speed: %.2f m/s\n", simPlane->position.x, simPlane->position.y, simPlane->position.z, simPlane->currentSpeed);

	renderObj->position = simPlane->position;
	renderObj->rotation = simPlane->rotation;
	Object_UpdateWorldBounds(renderObj);

	float3 planeFwd = Float3_Normalize(simPlane->forward);
	// compute plane's banked up vector
	float3 worldRef = (fabsf(planeFwd.y) < 0.99f) ? (float3){0.0f, 1.0f, 0.0f, 0.0f} : (float3){1.0f, 0.0f, 0.0f, 0.0f};
	float3 planeRight = Float3_Normalize(Float3_Cross(planeFwd, worldRef));
	float3 planeUp = Float3_Cross(planeRight, planeFwd);
	float bank = simPlane->bankAngle;
	float3 bankedUp = Float3_Add(Float3_Scale(planeUp, cosf(bank)), Float3_Scale(planeRight, sinf(bank)));

	float3 camOffset = Float3_Add(Float3_Scale(planeFwd, -12.0f), Float3_Scale(bankedUp, 3.0f));
	camera->position = Float3_Add(simPlane->position, camOffset);
	camera->forward = Float3_Normalize(Float3_Sub(simPlane->position, camera->position));

	Input_Poll(input, window);
	float AileronPct = planeGetAileronPct(simPlane);
	float elevatorPct = planeGetElevatorPct(simPlane);
	float rudderPct = planeGetRudderPct(simPlane);
	float flapPct = planeGetFlapPct(simPlane);

	if (input->keys[KB_KEY_S])
		planeSetElevatorPct(simPlane, fminf(100.0f, elevatorPct + 10.0f));
	else if (input->keys[KB_KEY_W])
		planeSetElevatorPct(simPlane, fmaxf(0.0f, elevatorPct - 10.0f));
	else
		planeSetElevatorPct(simPlane, 50.0f);

	if (input->keys[KB_KEY_A])
		planeSetAileronPct(simPlane, fmaxf(0.0f, AileronPct - 10.0f));
	else if (input->keys[KB_KEY_D])
		planeSetAileronPct(simPlane, fminf(100.0f, AileronPct + 10.0f));
	else
		planeSetAileronPct(simPlane, 50.0f);

	if (input->keys[KB_KEY_Q])
		planeSetRudderPct(simPlane, fminf(100.0f, rudderPct + 2.0f));
	else if (input->keys[KB_KEY_E])
		planeSetRudderPct(simPlane, fmaxf(0.0f, rudderPct - 2.0f));
	else
		planeSetRudderPct(simPlane, 50.0f);
}

int main() {
	srand((uint32)getpid());

	Input input;
	Camera camera;
	initCamera(&camera, WIDTH, HEIGHT, 90.0f, (float3){0.0f, 2.0f, -7.0f}, (float3){0.0f, -0.15f, 1.0f}, (float3){0.0f, 80.0f, -60.0f});

	MaterialLib matLib;
	MaterialLib_Init(&matLib, 256);

	ObjectList scene;
	ObjectList_Init(&scene, 1);

	Client c = {.host = "127.0.0.1", .port = 8080};

	idRegister objectRegistry;
	idRegister_Init(&objectRegistry, 1);

	RequestData request;
	RequestData_Init(&request, 1);

	// build checkerboard grid into a temporary list, then merge into one object
	ObjectList grid;
	ObjectList_Init(&grid, GRID_COLS * GRID_ROWS);
	for (int row = 0; row < GRID_ROWS; row++) {
		for (int col = 0; col < GRID_COLS; col++) {
			Object *obj = ObjectList_Add(&grid);
			static const struct {
				float3 color;
				float roughness;
				float metallic;
			} palette[] = {
				{{0.90f, 0.78f, 0.08f}, 0.85f, 0.00f}, // yellow  - rough matte
				{{0.55f, 0.08f, 0.85f}, 0.10f, 0.05f}, // purple  - smooth
				{{0.85f, 0.60f, 0.05f}, 0.20f, 0.90f}, // gold    - metallic
				{{0.80f, 0.12f, 0.12f}, 0.75f, 0.05f}, // red     - rough
				{{0.08f, 0.75f, 0.85f}, 0.15f, 0.00f}, // cyan    - smooth
				{{0.88f, 0.88f, 0.88f}, 0.05f, 0.90f}, // silver  - mirror
				{{0.10f, 0.50f, 0.12f}, 0.90f, 0.00f}, // green   - rough matte
				{{0.90f, 0.38f, 0.05f}, 0.50f, 0.20f}, // orange  - semi-rough
			};
			int pIdx = ((col * 7) ^ (row * 3) ^ (col + row * 5)) % 8;
			float emission = (row == GRID_ROWS - 1) ? 0.5f : 0.0f;
			float roughness = palette[pIdx].roughness;
			float metallic = palette[pIdx].metallic;
			float3 tileColor = palette[pIdx].color;
			CreateCube(obj, (float3){(col - GRID_COLS / 2) * 7.0f, -0.09f, 5.0f + row * 7.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){7.0f, 0.1f, 7.0f}, tileColor, &matLib, emission, roughness, metallic);
			Object_UpdateWorldBounds(obj);
		}
	}
	ObjectList_Merge(&grid, &scene);
	free(grid.objects);

	Object *cube = ObjectList_Add(&scene);
	CreateCube(cube, (float3){0.0f, 1.0f, 10.0f}, (float3){0.0f, 0.5f, 0.0f}, (float3){7.0f, 7.0f, 7.0f}, (float3){0.9f, 0.0f, 0.0f}, &matLib, 100.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube);

	Object *cube2 = ObjectList_Add(&scene);
	CreateCube(cube2, (float3){-10.0f, 1.0f, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){7.0f, 7.0f, 7.0f}, (float3){0.0f, 0.9f, 0.0f}, &matLib, 100.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube2);

	Object *cube3 = ObjectList_Add(&scene);
	CreateCube(cube3, (float3){10.0f, 1.0f, 15.0f}, (float3){0.0f, 0.0f, 0.0f}, (float3){7.0f, 7.0f, 7.0f}, (float3){0.0f, 0.0f, 0.9f}, &matLib, 100.0f, 0.99f, 0.0f);
	Object_UpdateWorldBounds(cube3);

	uint32 f16SceneIndex = (uint32)scene.count;
	Object *plane = ObjectList_Add(&scene);
	uint32 f16Id = generateId(MODEL_F16);
	LoadObj("assets/models/f16.bin", plane, &matLib);
	idRegister_Add(&objectRegistry, f16Id, f16SceneIndex);
	plane->position = (float3){0.0f, 10.0f, 20.0f};
	plane->rotation = (float3){0.0f, 0.0f, 0.0f};
	plane->scale = (float3){2.0f, 2.0f, 2.0f};
	CreateObjectBVH(plane, &plane->bvh);
	Object_UpdateWorldBounds(plane);

	Plane simPlane;
	loadPlaneBin(&simPlane, "./simulation/simModels/F-16C.bin", (float3){0.0f, 0.0f, 1.0f}, (float3){0.0f, 10.0f, 20.0f}, 100.0f, 1.0f);

	addFromRegistry(&request, &objectRegistry, &scene, f16Id);
	postObjects(&c, &request);
	RequestData_Reset(&request);

	struct mfb_window *window = mfb_open_ex("my display", WIDTH, HEIGHT, WF_RESIZABLE);
	if (!window) {
		fprintf(stderr, "Failed to create window\n");
		ObjectList_Destroy(&scene);
		destroyCamera(&camera);
		return 1;
	}
	mfb_set_target_fps(0);

	struct Alphabet alphabet;
	LoadAlphabet(&alphabet, "assets/chars");

	Skybox skybox;
	LoadSkybox(&skybox, "skybox");

	ThreadPool *threadPool = poolCreate(32, HEIGHT);
	RayTraceTaskQueue rayTaskQueue;
	SSRTask *ssrTasks = malloc(sizeof(SSRTask) * ((HEIGHT + 3) / 4));

	printf("Demo scene loaded. Total Tris: %d\n", ObjectList_CountTriangles(&scene));

	Volume cloudVol;
	LoadVolume(&cloudVol, "assets/models/cloud.bin",
			   (float3){0.0f, 70.0f, 13.0f, 0.0f},
			   (float3){0.0f, 0.0f, 0.0f, 0.0f},
			   (float3){180.0f, 18.0f, 180.0f, 0.0f},
			   VOLUME_CLOUD);

	CloudRenderer cloudRenderer;
	CloudRenderer_Init(&cloudRenderer, WIDTH, HEIGHT,
					   "render/gpu/kernels/cloadrendering/render.cl");
	UploadVolumeToGpu(&cloudVol, &cloudRenderer.ctx);
	int frame = 0;
	int shadowResolution = 4;
	double frameTimes[4] = {0};
	double accumRenderTime = 0.0;
	double accumSetupTime = 0.0;
	double accumShadowTime = 0.0;
	double accumSSRTime = 0.0;
	double accumCompositeTime = 0.0;
	double accumSyncTime = 0.0;
	double accumPresentTime = 0.0;
	int accumFrames = 0;

	struct timespec wSyncStart, wA, wB;
	WNOW(wSyncStart);

	Bench bench;
	benchInit(&bench);

	while (1) {
		// get current scene state from server
		getObjects(&c, &scene, &matLib, &objectRegistry);

		benchFrameStart(&bench);
		WNOW(wA);
		accumSyncTime += WDIFF(wSyncStart, wA);

		frame++;
		WNOW(wA); // setup timer covers clear + input + RenderSetup
		clearBuffers(&camera);
		const float2 jitterPattern[4] = {
			{1.5f, 1.5f}, {-1.5f, 1.5f}, {-1.5f, -1.5f}, {1.5f, -1.5f}};
		camera.jitter = jitterPattern[frame & 3];
		camera.seed = frame * (int)35527.0f << 16 | (int)11369.0f;

		// Input_Poll(&input, window);
		// if (input.keysDown[KB_KEY_ESCAPE]) break;
		// if (input.keys[KB_KEY_W]) CameraMoveForward(&camera, 0.2f);
		// if (input.keys[KB_KEY_S]) CameraMoveForward(&camera, -0.2f);
		// if (input.keys[KB_KEY_A]) CameraMoveRight(&camera, -0.2f);
		// if (input.keys[KB_KEY_D]) CameraMoveRight(&camera, 0.2f);
		// if (input.keys[KB_KEY_Q]) CameraMoveUp(&camera, 0.2f);
		// if (input.keys[KB_KEY_E]) CameraMoveUp(&camera, -0.2f);
		// if (input.mouse[MOUSE_LEFT]) CameraRotate(&camera, input.mouseDY * 0.005f, -input.mouseDX * 0.005f);

		// // Keep plane in front of camera, facing the same direction, offset slightly below view center
		// float3 fwd = Float3_Normalize(camera.forward);
		// plane->position = Float3_Add(
		// 	Float3_Add(camera.position, Float3_Scale(fwd, 10.0f)),
		// 	Float3_Scale(Float3_Normalize(camera.up), -2.5f));
		// plane->rotation = (float3){asinf(-fwd.y), atan2f(fwd.x, fwd.z), 0.0f};
		// Object_UpdateWorldBounds(plane);

		SimObjToRenderObj(&simPlane, plane, &camera, &input, window);

		// post current scene
		addAllFromRegistry(&request, &objectRegistry, &scene);
		postObjects(&c, &request);
		RequestData_Reset(&request);

		RenderSetup(scene.objects, scene.count, &camera);
		WNOW(wB);
		double setupTime = WDIFF(wA, wB);
		accumSetupTime += setupTime;

		WNOW(wA);
		RayTraceScene(scene.objects, scene.count, &camera, &matLib, &rayTaskQueue, threadPool, &skybox);

		// RASTERIZE
		// for (int i = 0; i < OBJECT_COUNT; i++) {
		// 	RenderObject(&objects[i], &camera, &matLib);
		// }
		WNOW(wB);
		double frameRenderTime = setupTime + WDIFF(wA, wB);
		frameTimes[frame & 3] = frameRenderTime;
		accumRenderTime += frameRenderTime;

		WNOW(wA);
		// ShadowPostProcess(objects, OBJECT_COUNT, &camera, shadowResolution, 64);
		// DitherPostProcess(&camera, frame);
		// DitherOrderedPostProcess(&camera, frame);
		WNOW(wB);
		accumShadowTime += WDIFF(wA, wB);

		WNOW(wA);
		// SSRPostProcess(&camera, threadPool, ssrTasks, 4);
		CloudRenderer_Render(&cloudRenderer, &cloudVol, &camera, (CloudParams){
																	 .baseColor = {1.0f, 1.0f, 1.0f, 0.0f},
																	 .extinctionScale = 11.0f,	// high = thick opaque cloud
																	 .shadowExtinction = 0.03f, // low = less internal darkening
																	 .scatterG = 0.1f,
																	 .shadowDist = 1.0f,
																	 .ambientLight = 0.8f, // high = shadowed parts stay bright white
																	 .godRays = 1,
																	 .godRayColor = {1.0f, 0.95f, 0.8f, 0.0f},
																	 .godRayIntensity = 0.6f,
																	 .godRayDecay = 0.95f,
																 });
		WNOW(wB);
		accumSSRTime += WDIFF(wA, wB);

		benchCaptureFrame(&bench, camera.framebuffer, WIDTH * HEIGHT);

		WNOW(wA);
		CloudRenderer_Composite(&cloudRenderer, &camera);
		WNOW(wB);
		accumCompositeTime += WDIFF(wA, wB);

#ifndef BENCH_MODE
		Color c = PackColorF((float3){1.0f, 0.5f, 0.2f});
		char text[64];
		double avgFrameTime = (frameTimes[0] + frameTimes[1] + frameTimes[2] + frameTimes[3]) * 0.25;
		snprintf(text, sizeof(text), "FPS: %.1f", avgFrameTime > 0.0 ? 1.0 / avgFrameTime : 0.0);
		RenderText(camera.framebuffer, WIDTH, HEIGHT, &alphabet, text, 20, 20, 1.75f, c);
#endif

		WNOW(wA);
		if (mfb_update(window, camera.framebuffer) != STATE_OK) break;
		WNOW(wB);
		accumPresentTime += WDIFF(wA, wB);

		accumFrames++;

		if ((frame % ACCUMULATE_STATS) == 0) {
			double avgRender = accumRenderTime / accumFrames * 1000.0;
			double avgSetup = accumSetupTime / accumFrames * 1000.0;
			double avgRasterize = avgRender - avgSetup;
			double avgShadow = accumShadowTime / accumFrames * 1000.0;
			double avgSSR = accumSSRTime / accumFrames * 1000.0;
			double avgComposite = accumCompositeTime / accumFrames * 1000.0;
			double avgSync = accumSyncTime / accumFrames * 1000.0;
			double avgPresent = accumPresentTime / accumFrames * 1000.0;
			double avgTotal = avgRender + avgShadow + avgSSR + avgComposite + avgSync + avgPresent;
			double avgFps = 1000.0 / avgTotal;
			double targetFrameTime = 1.0 / 60.0;
			int maxTriangles60 = (int)(ObjectList_CountTriangles(&scene) * ((targetFrameTime * 1000.0) / avgRasterize));
			printf("Frame %d  setup: %.2f ms  raster: %.2f ms  shadow: %.2f ms  ssr: %.2f ms  composite: %.2f ms  sync: %.2f ms  present: %.2f ms  total: %.2f ms  FPS: %.1f  Est Tris@60: %d  ShadowRes: %d\n",
				   frame, avgSetup, avgRasterize, avgShadow, avgSSR, avgComposite, avgSync, avgPresent, avgTotal, avgFps, maxTriangles60, shadowResolution);
			accumRenderTime = accumSetupTime = accumShadowTime = accumSSRTime = accumCompositeTime = accumSyncTime = accumPresentTime = 0.0;
			accumFrames = 0;
		}

		WNOW(wSyncStart);
		if (benchFrameEnd(&bench)) break;
#ifdef PGO_MAX_FRAMES
		if (frame >= PGO_MAX_FRAMES) break;
#endif
	}

	mfb_close(window);
	benchReport(&bench);
	benchFree(&bench);

	for (int i = 0; i < 256; i++) {
		if (alphabet.letters[i].tile.pixels) {
			free((void *)alphabet.letters[i].tile.pixels);
		}
	}
	ObjectList_Destroy(&scene);
	CloudRenderer_Destroy(&cloudRenderer);
	free(cloudVol.density);
	DestroySkybox(&skybox);
	poolDestroy(threadPool);
	free(ssrTasks);
	MaterialLib_Destroy(&matLib);
	destroyCamera(&camera);
	return 0;
}
