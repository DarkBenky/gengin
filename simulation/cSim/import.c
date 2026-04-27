#include "import.h"

void loadPlane(Plane *plane, const char *filename) {
	if (filename == NULL || plane == NULL) {
		fprintf(stderr, "Error: Invalid filename or plane pointer.\n");
		return;
	}

	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		fprintf(stderr, "Error: Could not open file %s\n", filename);
		return;
	}

	fread(plane, (sizeof(Plane) - sizeof(float) * 2), 1, file);
	plane->currentSpeed = 0.0f;
	plane->currentAltitude = 2000.0f;

	Surface *ss[] = {
		&plane->leftWing,
		&plane->rightWing,
		&plane->verticalStabilizer,
		&plane->horizontalStabilizer,
		&plane->leftAileron,
		&plane->rightAileron,
		&plane->rudder,
		&plane->leftFlap,
		&plane->rightFlap,
		&plane->leftElevator,
		&plane->rightElevator,
	};
	for (int i = 0; i < 11; i++)
		ss[i]->targetRotationAngle = ss[i]->rotationAngle;

	fclose(file);
}

void savePlane(const Plane *plane, const char *filename) {
	if (filename == NULL || plane == NULL) {
		fprintf(stderr, "Error: Invalid filename or plane pointer.\n");
		return;
	}

	FILE *file = fopen(filename, "w");
	if (file == NULL) {
		fprintf(stderr, "Error: Could not open file %s for writing\n", filename);
		return;
	}

	fwrite(plane, (sizeof(Plane) - sizeof(float) * 2), 1, file);
	fclose(file);
}