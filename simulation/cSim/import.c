#include "import.h"
#include <string.h>

static int readF3(FILE *f, float3 *out) {
	float v[3];
	if (fread(v, sizeof(float), 3, f) != 3) return 0;
	out->x = v[0];
	out->y = v[1];
	out->z = v[2];
	out->w = 0.0f;
	return 1;
}

static int writeF3(FILE *f, float3 v) {
	float a[3] = {v.x, v.y, v.z};
	return fwrite(a, sizeof(float), 3, f) == 3;
}

static int readSurface(FILE *f, Surface *s) {
	if (!readF3(f, &s->relativePos)) return 0;
	if (!readF3(f, &s->rotationAxis)) return 0;
	if (fread(&s->rotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->rotationRate, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->maxRotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->minRotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->surfaceArea, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->liftCoefficient, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->dragCoefficient, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->aspectRatio, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->efficiency, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->stallAngle, sizeof(float), 1, f) != 1) return 0;
	if (fread(&s->camber, sizeof(float), 1, f) != 1) return 0;
	uint8 active;
	if (fread(&active, 1, 1, f) != 1) return 0;
	s->active = (bool)active;
	s->targetRotationAngle = s->rotationAngle;
	return 1;
}

static int writeSurface(FILE *f, const Surface *s) {
	if (!writeF3(f, s->relativePos)) return 0;
	if (!writeF3(f, s->rotationAxis)) return 0;
	if (fwrite(&s->rotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->rotationRate, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->maxRotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->minRotationAngle, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->surfaceArea, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->liftCoefficient, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->dragCoefficient, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->aspectRatio, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->efficiency, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->stallAngle, sizeof(float), 1, f) != 1) return 0;
	if (fwrite(&s->camber, sizeof(float), 1, f) != 1) return 0;
	uint8 active = (uint8)s->active;
	return fwrite(&active, 1, 1, f) == 1;
}

int loadPlaneBin(Plane *plane, const char *path, float3 forward, float3 position, float speed, float throttle) {
	FILE *f = fopen(path, "rb");
	if (!f) return 1;

	memset(plane, 0, sizeof(*plane));

	if (fread(plane->name, 1, 16, f) != 16) {
		fclose(f);
		return 2;
	}

	if (!readF3(f, &plane->position)) {
		fclose(f);
		return 3;
	}
	if (!readF3(f, &plane->forward)) {
		fclose(f);
		return 3;
	}
	if (!readF3(f, &plane->rotation)) {
		fclose(f);
		return 3;
	}

	Surface *surfaces[] = {
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
	for (int i = 0; i < 11; i++) {
		if (!readSurface(f, surfaces[i])) {
			fclose(f);
			return 4;
		}
	}

	float engine[7];
	if (fread(engine, sizeof(float), 7, f) != 7) {
		fclose(f);
		return 5;
	}
	plane->maxTrust = engine[0];
	plane->currentTrustPercentage = engine[1];
	plane->baseMass = engine[2];
	plane->fuelMass = engine[3];
	plane->currentFuelPercentage = engine[4];
	plane->burnRate = engine[5];
	plane->burnWithoutAfterburner = engine[6];
	plane->forward = forward;
	plane->position = position;
	plane->rotation = (float3){0.0f, 0.0f, 0.0f, 0.0f};
	plane->currentSpeed = speed;
	plane->currentAltitude = position.y;
	plane->currentTrustPercentage = throttle;

	fclose(f);
	return 0;
}

int savePlaneBin(const Plane *plane, const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) return 1;

	if (fwrite(plane->name, 1, 16, f) != 16) {
		fclose(f);
		return 2;
	}

	if (!writeF3(f, plane->position)) {
		fclose(f);
		return 3;
	}
	if (!writeF3(f, plane->forward)) {
		fclose(f);
		return 3;
	}
	if (!writeF3(f, plane->rotation)) {
		fclose(f);
		return 3;
	}

	const Surface *surfaces[] = {
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
	for (int i = 0; i < 11; i++) {
		if (!writeSurface(f, surfaces[i])) {
			fclose(f);
			return 4;
		}
	}

	float engine[7] = {
		plane->maxTrust,
		plane->currentTrustPercentage,
		plane->baseMass,
		plane->fuelMass,
		plane->currentFuelPercentage,
		plane->burnRate,
		plane->burnWithoutAfterburner,
	};
	if (fwrite(engine, sizeof(float), 7, f) != 7) {
		fclose(f);
		return 5;
	}

	fclose(f);
	return 0;
}