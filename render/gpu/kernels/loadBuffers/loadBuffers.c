#include "loadBuffers.h"

Arena createArena(size_t objectSize, int capacity) {
	Arena arena;
	arena.memory = malloc(objectSize * capacity);
	arena.objectSize = objectSize;
	arena.len = 0;
	arena.capacity = capacity;
	return arena;
}

void addToArena(Arena *arena, void *object) {
	if (arena->len >= arena->capacity) {
		int newCap = arena->capacity * 2;
		void *resized = realloc(arena->memory, arena->objectSize * newCap);
		if (!resized) {
			fprintf(stderr, "Error: Could not grow Arena.\n");
			return;
		}
		arena->memory = resized;
		arena->capacity = newCap;
	}
	memcpy((char *)arena->memory + arena->len * arena->objectSize, object, arena->objectSize);
	arena->len++;
}

void clearArena(Arena *arena) {
	arena->len = 0;
}

void setObjectSize(Arena *arena, size_t newSize) {
	if (newSize == arena->objectSize) return;
	void *resized = realloc(arena->memory, newSize * arena->capacity);
	if (!resized) {
		fprintf(stderr, "Error: Could not resize Arena.\n");
		return;
	}
	arena->memory = resized;
	arena->objectSize = newSize;
}

void resetArena(Arena *arena, size_t objectSize) {
	setObjectSize(arena, objectSize);
	clearArena(arena);
}

void destroyArena(Arena *arena) {
	free(arena->memory);
	arena->memory = NULL;
	arena->objectSize = 0;
	arena->len = arena->capacity = 0;
}

CL_Buffer loadObjectIds(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));

	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &i);
	}
}

CL_Buffer loadObjectIdOffsets(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
	if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
	resetArena(arena, sizeof(int));

	int offset = 0;
	for (int i = 0; i < objectCount; i++) {
		addToArena(arena, &offset);
		offset += objects[i].triangleCount;
	}
}

CL_Buffer loadObjectPositions(Object *objects, int objectCount, CL_Context *ctx, cl_mem_flags flag, Arena *arena) {
    if (!objects || objectCount <= 0 || !ctx || !arena) exit(1);
    resetArena(arena, sizeof(float3));

    for (int i = 0; i < objectCount; i++) {
        addToArena(arena, &objects[i].position);
    }
}