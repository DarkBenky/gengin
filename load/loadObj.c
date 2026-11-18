#include "loadObj.h"
#include "../object/object.h"
#include "../object/material/material.h"
#include <stdio.h>
#include <stdlib.h>
#include "../util/bbox.h"
#include "../render/color/color.h"
#include "../util/json.h"

// TODO: **Important** Update for support of FaceMap
void LoadObj(const char *filename, Object *obj, MaterialLib *lib) {
	if (filename == NULL || obj == NULL) {
		fprintf(stderr, "Error: Invalid filename or object pointer.\n");
		return;
	}

	Object_SetGeneratedName(obj, "__custom_obj_", filename);
	FILE *file = fopen(filename, "rb");
	if (file == NULL) {
		fprintf(stderr, "Error: Could not open file %s\n", filename);
		return;
	}

	// read header
	uint32 fileSize, triangleStructSize, triangleCountRead, _hasTextures, _textureSize;
	if (fread(&fileSize, sizeof(uint32), 1, file) != 1 ||
		fread(&triangleStructSize, sizeof(uint32), 1, file) != 1 ||
		fread(&triangleCountRead, sizeof(uint32), 1, file) != 1 ||
		fread(&_hasTextures, sizeof(uint32), 1, file) != 1 ||
		fread(&_textureSize, sizeof(uint32), 1, file) != 1) {
		fprintf(stderr, "Error: %s is not a model file.\n", filename);
		fclose(file);
		return;
	}

	// hasTextures bit 0 = textures present, bit 1 = cull backfaces
	obj->cullBackfaces = GetBit(_hasTextures, 1);
	bool hasTextures = GetBit(_hasTextures, 0);
	bool hasEmissionMap = GetBit(_hasTextures, 2);

	// if file has emission map, we need to get size of this section
	uint32 FileSizeEmissionMap = 0;
	if (hasEmissionMap) {
		uint64 position = 20; // 20 bytes for the header
		if (hasTextures) {
			position += (uint64)_textureSize * _textureSize * 9; // 9 floats per texel (color, normal, material)
		}
		position += (uint64)triangleCountRead * triangleStructSize; // 108 bytes per triangle

		long keep = ftell(file);
		if (fseek(file, position, SEEK_SET) != 0 ||
			fread(&FileSizeEmissionMap, sizeof(uint32), 1, file) != 1) {
			FileSizeEmissionMap = 0;
		}
		if (keep >= 0) fseek(file, keep, SEEK_SET);
	}

	uint32 triangleCount = triangleCountRead;

	// the header already carried textureSize; a file whose size equation does not hold is the
	// legacy 16 byte header, where that word was triangle data, and it always used a 4096 atlas
	uint32 textureSize = _textureSize;
	uint64 expected = 20ull + (uint64)triangleCount * triangleStructSize +
					  (hasTextures ? (uint64)textureSize * textureSize * 9 : 0);
	if (expected != fileSize || textureSize < 256 || textureSize > 16384) {
		textureSize = 4096;
		fseek(file, 16, SEEK_SET);
	}

	// the field reads below assume the writer's single record layout, so a file
	// that does not match it is corrupt
	long dataStart = ftell(file);
	long fileEnd = -1;
	if (dataStart >= 0 && fseek(file, 0, SEEK_END) == 0) fileEnd = ftell(file);
	uint64 recordSize = 5ull * sizeof(float3) + 3ull * sizeof(float) +
						6ull * sizeof(uint16) + sizeof(uint32);
	uint64 expectedSize = (uint64)dataStart +
						  (hasTextures ? (uint64)textureSize * textureSize * 9 : 0) +
						  (uint64)triangleCount * recordSize;
	// fileSize stops at the triangles; the face map follows them and carries its own size
	if (hasEmissionMap) expectedSize += FileSizeEmissionMap;
	if (triangleStructSize != recordSize || fileEnd < 0 || (uint64)fileEnd != expectedSize) {
		fprintf(stderr, "Error: %s is truncated or corrupt.\n", filename);
		fclose(file);
		return;
	}
	fseek(file, dataStart, SEEK_SET);

	// load textures if present
	Textures *tex = NULL;
	if (hasTextures) {
		obj->hasTexture = true;
		tex = Textures_LoadFromFile(file, textureSize);
		if (!tex) {
			fprintf(stderr, "Error: Failed to load textures from file.\n");
			fclose(file);
			exit(1);
		}
	} else {
		obj->hasTexture = false;
	}

	// printf("File size: %u bytes\n", fileSize);
	// printf("Triangle struct size: %u bytes\n", triangleStructSize);
	// printf("Triangle count: %u\n", triangleCount);

	obj->v1 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->v2 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->v3 = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->normals = (float3 *)malloc(triangleCount * sizeof(float3));
	obj->materialIds = (int *)malloc(triangleCount * sizeof(int));
	obj->uvs = (UvCords *)malloc(triangleCount * sizeof(UvCords));

	if (!obj->v1 || !obj->v2 || !obj->v3 || !obj->normals || !obj->materialIds || !obj->uvs) {
		fprintf(stderr, "Error: Could not allocate memory for triangles.\n");
		Object_Destroy(obj);
		fclose(file);
		return;
	}
	obj->triangleCount = triangleCount;

	float3 BBmin = {FLT_MAX, FLT_MAX, FLT_MAX};
	float3 BBmax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	for (uint32 i = 0; i < triangleCount; i++) {
		float3 v1, v2, v3;
		float3 normal, color;
		float Roughness, Metallic, Emission;
		uint16 uv1x, uv1y, uv2x, uv2y, uv3x, uv3y;

		fread(&v1, sizeof(float3), 1, file);
		fread(&v2, sizeof(float3), 1, file);
		fread(&v3, sizeof(float3), 1, file);

		UpdateBoundingBox(&BBmin, &BBmax, v1);
		UpdateBoundingBox(&BBmin, &BBmax, v2);
		UpdateBoundingBox(&BBmin, &BBmax, v3);

		fread(&normal, sizeof(float3), 1, file);
		fread(&Roughness, sizeof(float), 1, file);
		fread(&Metallic, sizeof(float), 1, file);
		fread(&Emission, sizeof(float), 1, file);
		fread(&color, sizeof(float3), 1, file);

		fread(&uv1x, sizeof(uint16), 1, file);
		fread(&uv1y, sizeof(uint16), 1, file);
		fread(&uv2x, sizeof(uint16), 1, file);
		fread(&uv2y, sizeof(uint16), 1, file);
		fread(&uv3x, sizeof(uint16), 1, file);
		fread(&uv3y, sizeof(uint16), 1, file);
		uint32 _uvPad;
		fread(&_uvPad, sizeof(uint32), 1, file); // 4-byte pad after UV block

		obj->v1[i] = v1;
		obj->v2[i] = v2;
		obj->v3[i] = v3;
		obj->normals[i] = normal;
		obj->uvs[i] = (UvCords){uv1x, uv1y, uv2x, uv2y, uv3x, uv3y};
		obj->materialIds[i] = MaterialLib_FindOrAdd(lib, Material_Make(color, Roughness, Metallic, Emission, tex));
	}
	obj->BBmin = BBmin;
	obj->BBmax = BBmax;

	if (hasEmissionMap) {
		fread(NULL, sizeof(uint32), 1, file); // skip the FileSizeEmissionMap field, we already read it
		fread(NULL, sizeof(uint32), 1, file); // skip the SizeOfTexture field, we already read it

		float3 *EmissionMap = (float3 *)malloc((size_t)textureSize * textureSize * sizeof(float3));
		float3 *EmissionMapOut = (float3 *)malloc((size_t)EMISSION_RESOLUTION * EMISSION_RESOLUTION * sizeof(float3));
		memset(EmissionMap, 0, (size_t)textureSize * textureSize * sizeof(float3));

		fread(EmissionMap, sizeof(float3), (size_t)textureSize * textureSize, file); // Read Front Side
		
		if (textureSize != EMISSION_RESOLUTION) {
		    // TODO: Create generic float3 function that can be use later use bilinear interpolation
		}
	}

	fclose(file);
}

const float boostSatMax = 1.8;
const float boostBrightMax = 1.35;

#define SAVE_TEXEL_CHUNK 4096 // texels per buffered write, keeps fwrite calls out of the inner loop

static inline float2 boostFactor(float boost) {
	float2 result;
	result.x = 1.0f + (boostSatMax - 1.0f) * boost;
	result.y = 1.0f + (boostBrightMax - 1.0f) * boost;
	return result;
}

// One to one implementation of writeFile in parseObj.go
Error SaveObj(const char *filename, Object *obj, MaterialLib *lib, float boost) {
	if (filename == NULL || obj == NULL || lib == NULL) {
		fprintf(stderr, "Error: Invalid filename, object or material lib.\n");
		return Error_Make(E_INVALID_ARG, "Invalid arguments");
	}

	FILE *file = fopen(filename, "wb");
	if (file == NULL) {
		fprintf(stderr, "Error: Could not open file %s\n", filename);
		return Error_Make(E_FILE_OPEN_FAILED, "Could not open file");
	}

	// Binary layout:
	// Header (20 bytes):
	//   fileSize           uint32  — File size ignores FaceMap
	//   triangleStructSize uint32  — 108 bytes
	//   triangleCount      uint32
	//   hasTextures        uint32  — bit0: textures present, bit1: cull backfaces, bit2: has emission map
	//   textureSize        uint32  — texture edge length
	//
	// Texture data (if hasTextures):
	//   ColorMap     textureSize*textureSize*4 bytes (RGBA uint8)
	//   NormalMap    textureSize*textureSize*3 bytes (RGB uint8)
	//   MaterialMap  textureSize*textureSize*2 bytes ([roughness, metallic] uint8)
	//
	// Per triangle (108 bytes):
	//   v1 v2 v3 normal   4 × float3 padded to float4 = 4×16 = 64
	//   roughness metallic emission                    = 12
	//   color float3 padded                            = 16
	//   uv1[2] uv2[2] uv3[2] + 4 byte pad              = 16
	//
	// Face Map Layout if hasEmissionMap:
	//  FileSizeEmissionMap 	uint32
	//  SizeOfTexture 			uint32 —  based on the EMISSION_RESOLUTION defined in object.h
	//  FrontTexture  			float3 *SizeOfTexture * SizeOfTexture
	//  BackTexture  			float3 *SizeOfTexture * SizeOfTexture
	//  RightTexture  			float3 *SizeOfTexture * SizeOfTexture
	//  LeftTexture  			float3 *SizeOfTexture * SizeOfTexture
	//  TopTexture   			float3 *SizeOfTexture * SizeOfTexture
	//  BottomTexture 			float3 *SizeOfTexture * SizeOfTexture

	const uint32 triangleStructSize = 108;

	uint32 triangleCount = obj->triangleCount;
	uint32 hasTextures = SetBit(0, 0, obj->hasTexture);
	bool cullBackfaces = obj->cullBackfaces;
	uint32 textureSize = 4096; // default texture size
	uint32 textureBytes = 0;
	bool hasEmissionMap = obj->hasEmission ? 1 : 0;

	if (hasTextures) {
		Material mat = lib->entries[obj->materialIds[0]];
		Textures *tex = mat.textures;
		textureSize = tex ? tex->size : 4096;
		textureBytes = textureSize * textureSize * 9;
	}

	uint32 fileSize = 20 + triangleCount * triangleStructSize + (hasTextures ? textureBytes : 0);
	hasTextures = SetBit(hasTextures, 1, cullBackfaces);
	hasTextures = SetBit(hasTextures, 2, hasEmissionMap);

	fwrite(&fileSize, sizeof(uint32), 1, file);
	fwrite(&triangleStructSize, sizeof(uint32), 1, file);
	fwrite(&triangleCount, sizeof(uint32), 1, file);
	fwrite(&hasTextures, sizeof(uint32), 1, file);
	fwrite(&textureSize, sizeof(uint32), 1, file);

	if (obj->hasTexture) {
		const float2 satBrightBoost = boostFactor(boost);
		Material mat = lib->entries[obj->materialIds[0]];
		for (int i = 0; i < textureSize * textureSize; i++) {
			Color col = mat.textures->colorMap[i];
			col = BoostColor(col, satBrightBoost.x, satBrightBoost.y);
			fwrite(&col, sizeof(Color), 1, file);
		}
		for (int i = 0; i < textureSize * textureSize; i++) {
			Color col = mat.textures->normalMap[i];
			uint8 *px = (uint8 *)&col;
			fwrite(px, 3, 1, file); // file stores RGB only, alpha is dropped
		}
		for (int i = 0; i < textureSize * textureSize; i++) {
			uint16 matProps = mat.textures->MaterialMap[i];
			fwrite(&matProps, sizeof(uint16), 1, file);
		}
	}

	char zeroPad[4] = {0, 0, 0, 0};
	UvCords zeroUv = {0};

	Material mat;
	for (int i = 0; i < triangleCount; i++) {
		mat = lib->entries[obj->materialIds[i]];
		fwrite(&obj->v1[i], sizeof(float3), 1, file);
		fwrite(&obj->v2[i], sizeof(float3), 1, file);
		fwrite(&obj->v3[i], sizeof(float3), 1, file);
		fwrite(&obj->normals[i], sizeof(float3), 1, file);
		fwrite(&mat.roughness, sizeof(float), 1, file);
		fwrite(&mat.metallic, sizeof(float), 1, file);
		fwrite(&mat.emission, sizeof(float), 1, file);
		fwrite(&mat.color, sizeof(float3), 1, file);
		const UvCords *uv = obj->uvs ? &obj->uvs[i] : &zeroUv;
		fwrite(&uv->uv1x, sizeof(uint16), 1, file);
		fwrite(&uv->uv1y, sizeof(uint16), 1, file);
		fwrite(&uv->uv2x, sizeof(uint16), 1, file);
		fwrite(&uv->uv2y, sizeof(uint16), 1, file);
		fwrite(&uv->uv3x, sizeof(uint16), 1, file);
		fwrite(&uv->uv3y, sizeof(uint16), 1, file);
		fwrite(zeroPad, sizeof(char), 4, file); // 4-byte pad after UV block
	}

	if (hasEmissionMap) {
		// uint32 FileSize + uint32 SizeOfTexture + 6 * SizeOfTexture * SizeOfTexture * sizeof(float3_no_pad)
		uint32 fileSizeFaceMap = sizeof(uint32) * 2 + EMISSION_RESOLUTION * EMISSION_RESOLUTION * sizeof(float3) * 6;
		uint32 sizeOfTexture = EMISSION_RESOLUTION;

		fwrite(&fileSizeFaceMap, sizeof(uint32), 1, file);
		fwrite(&sizeOfTexture, sizeof(uint32), 1, file);

		fwrite(obj->frontFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
		fwrite(obj->backFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
		fwrite(obj->rightFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
		fwrite(obj->leftFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
		fwrite(obj->topFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
		fwrite(obj->bottomFaceEmission.emissionMap, sizeof(float3), EMISSION_RESOLUTION * EMISSION_RESOLUTION, file);
	}

	fclose(file);
	return Error_Ok();
};

// TODO: **Important** Implement the function to save a scene to a JSON file
Error SaveSceneToJson(const ObjectList *scene, const char *filename) {
	for (int i = 0; i < scene->count; i++) {
		// TODO: **Important** Process each object in the scene
		Object *obj = &scene->objects[i];
		jsonObject *objJson = newJsonObject();

		bool isShape = false;
		if (HasPrefix(obj->ObjectPathOrType, "__buildin")) {
			addJsonField(objJson, "type", "shape", JSON_TYPE_STRING);
			isShape = true;
		} else {
			addJsonField(objJson, "type", "object", JSON_TYPE_STRING);
		}

		if (isShape) {
			// extract the shape type from the ObjectPathOrType string
			char shapeType[64] = {0};
			int index = 0;
			int offset = strlen("__buildin_");
			for (int j = offset; j < strlen(obj->ObjectPathOrType); j++) {
				if (obj->ObjectPathOrType[j] == '_') {
					shapeType[index] = '\0';
					break;
				}
				shapeType[index++] = obj->ObjectPathOrType[j];
			}
			addJsonField(objJson, "shapeType", shapeType, JSON_TYPE_STRING);
		}

		// Save position, rotation, scale, color as arrays
		char *posArray = newJsonNumberArray((float *)&obj->position, 3);
		char *rotArray = newJsonNumberArray((float *)&obj->rotation, 3);
		char *scaleArray = newJsonNumberArray((float *)&obj->scale, 3);
		float3 color = UnpackColor(obj->_temp);
		char *colorArray = newJsonNumberArray(&color.x, 3);

		addJsonField(objJson, "position", posArray, JSON_TYPE_ARRAY);
		addJsonField(objJson, "rotation", rotArray, JSON_TYPE_ARRAY);
		addJsonField(objJson, "scale", scaleArray, JSON_TYPE_ARRAY);
		addJsonField(objJson, "color", colorArray, JSON_TYPE_ARRAY);

		// TODO: **Important** Save emission maps needs to be implemented it needs to be designed

		// Free all allocations
	}
}