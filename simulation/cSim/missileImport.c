#include "missileImport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int isMissileBin(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	char magic[4];
	size_t n = fread(magic, 1, 4, f);
	fclose(f);
	return n == 4 && memcmp(magic, MISSILE_BIN_MAGIC, 4) == 0;
}

int loadMissileBin(MissileSpec *spec, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return 1;
	memset(spec, 0, sizeof(*spec));

	char magic[4];
	uint32_t version;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MISSILE_BIN_MAGIC, 4) != 0) {
		fclose(f);
		return 2;
	}
	if (fread(&version, sizeof(version), 1, f) != 1 || version != MISSILE_BIN_VERSION) {
		fclose(f);
		return 3;
	}
	if (fread(spec->name, 1, 16, f) != 16) {
		fclose(f);
		return 4;
	}

#define READ_F(field)                                \
	if (fread(&(field), sizeof(float), 1, f) != 1) { \
		fclose(f);                                   \
		return 5;                                    \
	}
	READ_F(spec->length);
	READ_F(spec->diameter);
	READ_F(spec->finSpanFront);
	READ_F(spec->finSpanBack);
	READ_F(spec->emptyMass);
	READ_F(spec->propellantMass);
	READ_F(spec->inertiaRoll);
	READ_F(spec->inertiaPitch);
	READ_F(spec->inertiaYaw);
	READ_F(spec->cnAlpha);
	READ_F(spec->cd0);
	READ_F(spec->dampingRoll);
	READ_F(spec->dampingPitch);
	READ_F(spec->dampingYaw);
	READ_F(spec->stallAngle);
	READ_F(spec->maxG);
	READ_F(spec->frontArea);
	READ_F(spec->frontMaxDeg);
	READ_F(spec->frontRateDeg);
	READ_F(spec->backArea);
	READ_F(spec->backMaxDeg);
	READ_F(spec->backRateDeg);
	READ_F(spec->tvcMaxDeg);
	READ_F(spec->tvcRateDeg);

	uint32_t stageCount;
	if (fread(&stageCount, sizeof(stageCount), 1, f) != 1 || stageCount < 1 || stageCount > 3) {
		fclose(f);
		return 6;
	}
	spec->stageCount = (int)stageCount;
	for (int i = 0; i < 3; i++) {
		READ_F(spec->stages[i].thrust);
		READ_F(spec->stages[i].burnTime);
	}
	READ_F(spec->pnGain);
	READ_F(spec->fuseRadius);
	READ_F(spec->lifetime);
#undef READ_F

	fclose(f);
	return 0;
}
