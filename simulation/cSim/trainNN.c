// TODO: train model with genetic algorithm to predict control surface deflections for given forward vector
#include "dense.h"
#include "import.h"
#include "simulate.h"

#define MAX_SPEED 700.0f
#define MAX_ALTITUDE 25000.0f

#define INPUT_SIZE 8
#define OUTPUT_SIZE 5

typedef struct {
	float Speed;
	float Altitude;
	float3 currentForward;
	float3 targetForward;
} Inputs;

typedef struct {
	float Aileron;
	float Elevator;
	float Rudder;
	float Flap;
	float Throttle;
} Outputs;

static void loadInputs(Inputs *inputs, const Plane *plane, const float3 *targetForward) {
	inputs->Speed = plane->currentSpeed;
	inputs->Altitude = plane->currentAltitude;
	inputs->currentForward = plane->forward;
	inputs->targetForward = *targetForward;
}

static void normalizeInputs(const Inputs *inputs, float buf[INPUT_SIZE]) {
	buf[0] = inputs->currentForward.x;
	buf[1] = inputs->currentForward.y;
	buf[2] = inputs->currentForward.z;
	buf[3] = inputs->targetForward.x;
	buf[4] = inputs->targetForward.y;
	buf[5] = inputs->targetForward.z;
	buf[6] = (inputs->Speed / MAX_SPEED) * 2.0f - 1.0f;
	buf[7] = (inputs->Altitude / MAX_ALTITUDE) * 2.0f - 1.0f;
}

static void readOutputs(const float raw[OUTPUT_SIZE], Outputs *out) {
	out->Aileron = raw[0];
	out->Elevator = raw[1];
	out->Rudder = raw[2];
	out->Flap = raw[3];
	out->Throttle = (raw[4] + 1.0f) * 0.5f;
}

static void applyOutputs(Plane *plane, const Outputs *out) {
	planeSetAileron(plane, out->Aileron);
	planeSetElevator(plane, out->Elevator);
	planeSetRudder(plane, out->Rudder);
	planeSetFlap(plane, out->Flap);
	planeSetThrottle(plane, out->Throttle);
}

void runInference(Model *model, Plane *plane, const float3 *targetForward) {
	Inputs inputs;
	loadInputs(&inputs, plane, targetForward);

	float inBuf[INPUT_SIZE];
	float outBuf[OUTPUT_SIZE];
	normalizeInputs(&inputs, inBuf);
	Forward(model, inBuf, outBuf);

	Outputs outputs;
	readOutputs(outBuf, &outputs);
	applyOutputs(plane, &outputs);
}

float f3Dot(float3 a, float3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float lossFunc(const Plane *plane, const float3 *targetForward) {
	float3 fwd = plane->forward;
	float3 tgt = *targetForward;
	float dot = f3Dot(fwd, tgt);
	return 1.0f - dot; // want to maximize dot product (cosine similarity)
}

static Model buildModel() {
	Model model;
	InitModel(&model, INPUT_SIZE, OUTPUT_SIZE);
	AddDenseLayer(&model, INPUT_SIZE, 16, RELU);
	AddDenseLayer(&model, 16, 16, RELU);
	AddDenseLayer(&model, 16, 16, RELU);
	AddDenseLayer(&model, 16, OUTPUT_SIZE, TANH);
	return model;
}

float3 randomPointOnSphere() {
    float z = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    float t = (float)rand() / RAND_MAX * 2.0f * 3.14159265f;
    float r = sqrtf(1.0f - z * z);
    return (float3){r * cosf(t), r * sinf(t), z, 0.0f};
}

int main(void) {
	Plane plane;
	loadPlane(&plane, "../simModels/F-16C.bin");

	const populationSize = 32;
	Model Models[populationSize];
	for (int i = 0; i < populationSize; i++) {
		Models[i] = buildModel();
	}

    float losses[populationSize];
    for (int gen = 0; gen < 1000; gen++) {
        float3 target = randomPointOnSphere();
        for (int i = 0; i < populationSize; i++) {
            runInference(&Models[i], &plane, &target);
            for (int step = 0; step < 100; step++) {
                updatePlane(&plane, 0.1f, &target);
            }
            losses[i] = lossFunc(&plane, &target);
        }
        float averageLoss = 0.0f;
        for (int i = 0; i < populationSize; i++) {
            averageLoss += losses[i];
        }
        averageLoss /= populationSize;
        printf("Generation %d, Average Loss: %f\n", gen, averageLoss);
        // keep above average, rest set to best + mutation
        for (int i = 0; i < populationSize; i++) {
            if (losses[i] > averageLoss) {
                // copy from best
                int bestIdx = 0;
                for (int j = 1; j < populationSize; j++) {
                    if (losses[j] < losses[bestIdx]) {
                        bestIdx = j;
                    }
                }
                FreeModel(&Models[i]);
                Models[i] = buildModel();
        


    }






	FreeModel(&model);
	return 0;
}
