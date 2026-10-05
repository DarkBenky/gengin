#include <string.h>

#include "../../object/format.h"
#include "../../object/object.h"
#include "../../object/scene.h"
#include "../../util/threadPool.h"
#include "../../render/color/color.h"

#define ROWS_PER_TASK 8
const float3 highlightColor = {1.75f, 0.75f, 1.81f}; // Multipliers for each color channel

enum EditorMode {
	MOVE_MODE, // default mode
	ROTATE_MODE,
	SCALE_MODE
};

typedef struct editorUi {
	ObjectList *scene;
	Camera *cam;
	Object *selectedObject; // if NULL, no object is selected
	enum EditorMode mode;
	bool editorActive;
	float moveSpeed;
	float rotationSpeed;
	float scaleSpeed;
} editorUi;

Error createEditorUi(ObjectList *objectList, editorUi *ui, Camera *cam) {
	Error err;
	if (ui == NULL || objectList == NULL || cam == NULL) {
		err.err = E_INVALID_ARG;
		strcpy(err.msg, "Invalid arguments");
		return err;
	}
	ui->scene = objectList;
	ui->cam = cam;
	ui->selectedObject = NULL;
	ui->mode = MOVE_MODE;
	ui->editorActive = false;
	ui->moveSpeed = 1.0f;
	ui->rotationSpeed = 1.0f;
	ui->scaleSpeed = 1.0f;
	err.err = nil;
	strcpy(err.msg, "OK");
	return err;
}

void destroyEditorUi(editorUi *ui) {
	free(ui);
}

Error activateEditorUi(editorUi *ui) {
	Error err;
	if (ui == NULL) {
		err.err = E_INVALID_ARG;
		strcpy(err.msg, "Invalid arguments");
		return err;
	}
	ui->editorActive = true;
	err.err = nil;
	strcpy(err.msg, "OK");
	return err;
}

Error deactivateEditorUi(editorUi *ui) {
	Error err;
	if (ui == NULL) {
		err.err = E_INVALID_ARG;
		strcpy(err.msg, "Invalid arguments");
		return err;
	}
	ui->selectedObject = NULL;
	ui->mode = MOVE_MODE;
	ui->editorActive = false;
	err.err = nil;
	strcpy(err.msg, "OK");
	return err;
}

void selectObject(editorUi *ui, int px, int py) {
	int hitIndex = -1;
	Object *hitObject = ObjectListGetObjectAtPixel(ui->cam, ui->scene, px, py, &hitIndex);
	ui->selectedObject = hitObject;
	ui->mode = MOVE_MODE;
}

typedef struct highlightTask {
	int rowStart;
	int rowEnd;
	int imageWidth;
    int imageHeight;
	int targetId;
	uint32 *framebuffer;
	int *objectIdBuffer;
} highlightTask;

void highlightTaskFunction(void *arg) {
	highlightTask *task = (highlightTask *)arg;
	int rowStart = task->rowStart;
	int rowEnd = task->rowEnd;
	int imageWidth = task->imageWidth;
	int targetId = task->targetId;
	uint32 *restrict framebuffer = task->framebuffer;
	int *restrict objectIdBuffer = task->objectIdBuffer;

	for (int row = rowStart; row < rowEnd; row++) {
		for (int col = 0; col < imageWidth; col++) {
			int objectId = objectIdBuffer[row * imageWidth + col];
			if (objectId == targetId) {
				float3 color = UnpackColor(framebuffer[row * imageWidth + col]);
				color.x *= highlightColor.x;
				color.y *= highlightColor.y;
				color.z *= highlightColor.z;
				// named apart from the loop counter: shadowing it indexed the
				// framebuffer by the packed color value
				Color tint = PackColor(color.x, color.y, color.z);
				framebuffer[row * imageWidth + col] = tint;
			}
		}
	}
}

void highlightTaskFunctionV2(void *arg) {
	highlightTask *task = (highlightTask *)arg;
	int rowStart = task->rowStart;
	int rowEnd = task->rowEnd;
	int imageWidth = task->imageWidth;
	int targetId = task->targetId;
	uint32 *restrict framebuffer = task->framebuffer;
	int *restrict objectIdBuffer = task->objectIdBuffer;

	__m256i vTarget = _mm256_set1_epi32(targetId);

	for (int row = rowStart; row < rowEnd; row++) {
		int base = row * imageWidth;
		int col = 0;
		for (; col + 8 <= imageWidth; col += 8) {
			__m256i ids = _mm256_loadu_si256((__m256i *)&objectIdBuffer[base + col]);
			__m256i eq = _mm256_cmpeq_epi32(ids, vTarget);
			int mask = _mm256_movemask_ps(_mm256_castsi256_ps(eq));
			if (!mask) continue; // none of the 8 match, skip

			// process only matching pixels
			while (mask) {
				int i = __builtin_ctz(mask);
				mask &= mask - 1;
				int idx = base + col + i;
				float3 c = UnpackColor(framebuffer[idx]);
				c.x *= highlightColor.x;
				c.y *= highlightColor.y;
				c.z *= highlightColor.z;
				framebuffer[idx] = PackColor(c.x, c.y, c.z);
			}
		}
		for (; col < imageWidth; col++) { // tail
			int idx = base + col;
			if (objectIdBuffer[idx] == targetId) {
				float3 c = UnpackColor(framebuffer[idx]);
				c.x *= highlightColor.x;
				c.y *= highlightColor.y;
				c.z *= highlightColor.z;
				framebuffer[idx] = PackColor(c.x, c.y, c.z);
			}
		}
	}
}

void highlightTaskFunctionEdge(void *arg) {
	highlightTask *task = (highlightTask *)arg;
	int rowStart = task->rowStart;
	int rowEnd = task->rowEnd;
	int imageWidth = task->imageWidth;
	int imageHeight = task->imageHeight;
	int targetId = task->targetId;
	uint32 *restrict framebuffer = task->framebuffer;
	int *restrict objectIdBuffer = task->objectIdBuffer;

	const uint32 white = PackColor(1.0f, 1.0f, 1.0f);

	for (int row = rowStart; row < rowEnd; row++) {
		for (int col = 0; col < imageWidth; col++) {
			int idx = row * imageWidth + col;
			if (objectIdBuffer[idx] != targetId) continue;

			// out-of-image counts as "not target", so the object gets
			// outlined at screen edges too
			bool edge =
				col == 0               || objectIdBuffer[idx - 1]          != targetId ||
				col == imageWidth - 1  || objectIdBuffer[idx + 1]          != targetId ||
				row == 0               || objectIdBuffer[idx - imageWidth] != targetId ||
				row == imageHeight - 1 || objectIdBuffer[idx + imageWidth] != targetId;

			if (edge) {
				framebuffer[idx] = white;
			} else {
				float3 color = UnpackColor(framebuffer[idx]);
				color.x *= highlightColor.x;
				color.y *= highlightColor.y;
				color.z *= highlightColor.z;
				framebuffer[idx] = PackColor(color.x, color.y, color.z);
			}
		}
	}
}

#define CURSOR_ARM 6

void applyHighlight(editorUi *ui, ThreadPool *threadPool, int px, int py) {
	Camera *cam = ui->cam;
	int width = cam->screenWidth;
	int height = cam->screenHeight;

	// if selected object not Null tint pixel in frame buffer with highlight color
	if (ui->selectedObject != NULL && cam->objectIdBuffer != NULL) {
		int targetId = (int)(ui->selectedObject - ui->scene->objects);
		if (targetId >= 0 && targetId < ui->scene->count) {
			highlightTask tasks[(HEIGHT + ROWS_PER_TASK - 1) / ROWS_PER_TASK];
			int taskCount = 0;
			for (int rowStart = 0; rowStart < height; rowStart += ROWS_PER_TASK) {
				int rowEnd = rowStart + ROWS_PER_TASK;
				if (rowEnd > height) rowEnd = height;
				highlightTask *task = &tasks[taskCount++];
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebuffer, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunction, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	if (px >= 0 && py >= 0 && px < width && py < height) {
		Color cursorColor = PackColor(1.0f, 1.0f, 1.0f);
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebuffer[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebuffer[y * width + px] = cursorColor;
		}
	}
}

void applyHighlightV2(editorUi *ui, ThreadPool *threadPool, int px, int py) {
	Camera *cam = ui->cam;
	int width = cam->screenWidth;
	int height = cam->screenHeight;

	// if selected object not Null tint pixel in frame buffer with highlight color
	if (ui->selectedObject != NULL && cam->objectIdBuffer != NULL) {
		int targetId = (int)(ui->selectedObject - ui->scene->objects);
		if (targetId >= 0 && targetId < ui->scene->count) {
			highlightTask tasks[(HEIGHT + ROWS_PER_TASK - 1) / ROWS_PER_TASK];
			int taskCount = 0;
			for (int rowStart = 0; rowStart < height; rowStart += ROWS_PER_TASK) {
				int rowEnd = rowStart + ROWS_PER_TASK;
				if (rowEnd > height) rowEnd = height;
				highlightTask *task = &tasks[taskCount++];
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebuffer, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunctionV2, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	if (px >= 0 && py >= 0 && px < width && py < height) {
		Color cursorColor = PackColor(1.0f, 1.0f, 1.0f);
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebuffer[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebuffer[y * width + px] = cursorColor;
		}
	}
}

void applyHighlightEdge(editorUi *ui, ThreadPool *threadPool, int px, int py) {
	Camera *cam = ui->cam;
	int width = cam->screenWidth;
	int height = cam->screenHeight;

	// if selected object not Null tint pixel in frame buffer with highlight color
	if (ui->selectedObject != NULL && cam->objectIdBuffer != NULL) {
		int targetId = (int)(ui->selectedObject - ui->scene->objects);
		if (targetId >= 0 && targetId < ui->scene->count) {
			highlightTask tasks[(HEIGHT + ROWS_PER_TASK - 1) / ROWS_PER_TASK];
			int taskCount = 0;
			for (int rowStart = 0; rowStart < height; rowStart += ROWS_PER_TASK) {
				int rowEnd = rowStart + ROWS_PER_TASK;
				if (rowEnd > height) rowEnd = height;
				highlightTask *task = &tasks[taskCount++];
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebuffer, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunctionEdge, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	if (px >= 0 && py >= 0 && px < width && py < height) {
		Color cursorColor = PackColor(1.0f, 1.0f, 1.0f);
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebuffer[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebuffer[y * width + px] = cursorColor;
		}
	}
}