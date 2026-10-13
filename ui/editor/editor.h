#include <string.h>

#include "../../object/format.h"
#include "../../object/object.h"
#include "../../object/scene.h"
#include "../../util/threadPool.h"
#include "../../keyboard/keyboard.h"
#include "../../render/cpu/font.h"
#include "MiniFB_enums.h"
#include <MiniFB.h>

#define ROWS_PER_TASK 8
const float3 highlightColor = {1.75f, 0.75f, 1.81f}; // Multipliers for each color channel

enum EditorMode {
	MOVE_MODE = KB_KEY_1,
	ROTATE_MODE = KB_KEY_2,
	SCALE_MODE = KB_KEY_3
};

const int ToggleEditor = KB_KEY_TAB;
const int ForwardKey = KB_KEY_UP;
const int BackwardKey = KB_KEY_DOWN;
const int LeftKey = KB_KEY_LEFT;
const int RightKey = KB_KEY_RIGHT;
const int UpKey = KB_KEY_RIGHT_SHIFT;
const int DownKey = KB_KEY_RIGHT_CONTROL;
const int SnapKey = KB_KEY_KP_0;

typedef struct editorUi {
	ObjectList *scene;
	Camera *cam;
	struct Alphabet *alphabet;
	Input *input;			// TODO: integrate Input into Editor
	Object *selectedObject; // if NULL, no object is selected
	enum EditorMode mode;
	bool editorActive;
	bool snapToGrid;
	float moveSpeed;
	float rotationSpeed;
	float scaleSpeed;
} editorUi;

Error createEditorUi(ObjectList *objectList, editorUi *ui, Camera *cam, Input *input, struct Alphabet *alphabet) {
	Error err;
	if (ui == NULL || objectList == NULL || cam == NULL || input == NULL || alphabet == NULL) {
		err.err = E_INVALID_ARG;
		strcpy(err.msg, "Invalid arguments");
		return err;
	}
	ui->scene = objectList;
	ui->cam = cam;
	ui->input = input;
	ui->alphabet = alphabet;
	ui->selectedObject = NULL;
	ui->mode = MOVE_MODE;
	ui->editorActive = false;
	ui->moveSpeed = 1.0f;
	ui->rotationSpeed = 1.0f;
	ui->scaleSpeed = 1.0f;
	ui->snapToGrid = false;
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
	float3 *framebufferHdr;
	int *objectIdBuffer;
} highlightTask;

void highlightTaskFunction(void *arg) {
	highlightTask *task = (highlightTask *)arg;
	int rowStart = task->rowStart;
	int rowEnd = task->rowEnd;
	int imageWidth = task->imageWidth;
	int targetId = task->targetId;
	float3 *restrict framebuffer = task->framebufferHdr;
	int *restrict objectIdBuffer = task->objectIdBuffer;

	for (int row = rowStart; row < rowEnd; row++) {
		for (int col = 0; col < imageWidth; col++) {
			int objectId = objectIdBuffer[row * imageWidth + col];
			if (objectId == targetId) {
				float3 color = framebuffer[row * imageWidth + col];
				color.x *= highlightColor.x;
				color.y *= highlightColor.y;
				color.z *= highlightColor.z;
				// named apart from the loop counter: shadowing it indexed the
				// framebuffer by the packed color value
				framebuffer[row * imageWidth + col] = color;
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
	float3 *restrict framebuffer = task->framebufferHdr;
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
				float3 c = framebuffer[idx];
				c.x *= highlightColor.x;
				c.y *= highlightColor.y;
				c.z *= highlightColor.z;
				framebuffer[idx] = c;
			}
		}
		for (; col < imageWidth; col++) { // tail
			int idx = base + col;
			if (objectIdBuffer[idx] == targetId) {
				float3 c = framebuffer[idx];
				c.x *= highlightColor.x;
				c.y *= highlightColor.y;
				c.z *= highlightColor.z;
				framebuffer[idx] = c;
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
	float3 *restrict framebuffer = task->framebufferHdr;
	int *restrict objectIdBuffer = task->objectIdBuffer;

	const float3 white = {1.0f, 1.0f, 1.0f};

	for (int row = rowStart; row < rowEnd; row++) {
		for (int col = 0; col < imageWidth; col++) {
			int idx = row * imageWidth + col;
			if (objectIdBuffer[idx] != targetId) continue;

			// out-of-image counts as "not target", so the object gets
			// outlined at screen edges too
			bool edge =
				col == 0 || objectIdBuffer[idx - 1] != targetId ||
				col == imageWidth - 1 || objectIdBuffer[idx + 1] != targetId ||
				row == 0 || objectIdBuffer[idx - imageWidth] != targetId ||
				row == imageHeight - 1 || objectIdBuffer[idx + imageWidth] != targetId;

			if (edge) {
				framebuffer[idx] = white;
			} else {
				float3 color = framebuffer[idx];
				color.x *= highlightColor.x;
				color.y *= highlightColor.y;
				color.z *= highlightColor.z;
				framebuffer[idx] = color;
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
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebufferHdr, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunction, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	const float3 cursorColor = {1.0f, 1.0f, 1.0f};
	if (px >= 0 && py >= 0 && px < width && py < height) {
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebufferHdr[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebufferHdr[y * width + px] = cursorColor;
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
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebufferHdr, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunctionV2, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	const float3 cursorColor = {1.0f, 1.0f, 1.0f};
	if (px >= 0 && py >= 0 && px < width && py < height) {
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebufferHdr[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebufferHdr[y * width + px] = cursorColor;
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
				*task = (highlightTask){rowStart, rowEnd, width, height, targetId, cam->framebufferHdr, cam->objectIdBuffer};
				poolAdd(threadPool, highlightTaskFunctionEdge, task);
			}
			poolWait(threadPool);
		}
	}

	// draw cursor at px, py
	const float3 cursorColor = {1.0f, 1.0f, 1.0f};
	if (px >= 0 && py >= 0 && px < width && py < height) {
		for (int i = -CURSOR_ARM; i <= CURSOR_ARM; i++) {
			int x = px + i;
			if (x >= 0 && x < width) cam->framebufferHdr[py * width + x] = cursorColor;
			int y = py + i;
			if (y >= 0 && y < height) cam->framebufferHdr[y * width + px] = cursorColor;
		}
	}
}

static int fmtTriangles(char *buf, int size, int count) {
	if (count >= 1000) return snprintf(buf, size, "    c: %dk", count / 1000);
	return snprintf(buf, size, "    c: %d", count);
}

void drawStats(editorUi *ui, int x, int y, int width, int height) {
	const int characterSize = 8;
	const Object *selected = ui->selectedObject;
	if (selected == NULL) return;
	float3 pos = selected->position;
	float3 rot = selected->rotation;
	float3 scl = selected->scale;
	int triCount = selected->triangleCount;

	char lines[14][32];
	int n = 0;
	snprintf(lines[n++], 32, "position");
	snprintf(lines[n++], 32, "    x: %.2f", pos.x);
	snprintf(lines[n++], 32, "    y: %.2f", pos.y);
	snprintf(lines[n++], 32, "    z: %.2f", pos.z);
	snprintf(lines[n++], 32, "rotation");
	snprintf(lines[n++], 32, "    x: %.2f", rot.x);
	snprintf(lines[n++], 32, "    y: %.2f", rot.y);
	snprintf(lines[n++], 32, "    z: %.2f", rot.z);
	snprintf(lines[n++], 32, "scale");
	snprintf(lines[n++], 32, "    x: %.2f", scl.x);
	snprintf(lines[n++], 32, "    y: %.2f", scl.y);
	snprintf(lines[n++], 32, "    z: %.2f", scl.z);
	snprintf(lines[n++], 32, "triangles");
	fmtTriangles(lines[n++], 32, triCount);

	int maxChars = 0;
	for (int i = 0; i < n; i++) {
		int len = (int)strlen(lines[i]);
		if (len > maxChars) maxChars = len;
	}

	float scaleX = (float)width / (maxChars * characterSize);
	float scaleY = (float)height / (n * characterSize);
	float scale = scaleX < scaleY ? scaleX : scaleY;
	if (scale >= 1.0f) scale = (float)(int)scale;

	int lineHeight = (int)(characterSize * scale);
	float3 white = {1.0f, 1.0f, 1.0f};

	for (int i = 0; i < n; i++) {
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, lines[i], x, y + i * lineHeight, scale, white);
	}
}

void editorStep(editorUi *ui, struct mfb_window *window) {
	Input *input = ui->input;
	Input_Poll(input, window);
	if (input->keys[ToggleEditor]) {
		if (ui->editorActive) {
			deactivateEditorUi(ui);
			return;
		} else {
			activateEditorUi(ui);
		}
	}

	if (ui->editorActive) {
		if (input->keys[SnapKey]) {
			ui->snapToGrid = !ui->snapToGrid;
		}

		if (input->keys[MOVE_MODE]) {
			ui->mode = MOVE_MODE;
		} else if (input->keys[ROTATE_MODE]) {
			ui->mode = ROTATE_MODE;
		} else if (input->keys[SCALE_MODE]) {
			ui->mode = SCALE_MODE;
		}

		switch (ui->mode) {
		// NOTE: when this is applied we need to rebuild objects data
		case MOVE_MODE: {
			// TODO: Move selected object
			break;
		}
		case ROTATE_MODE: {
			// TODO: Rotate selected object
			break;
		}
		case SCALE_MODE: {
			// TODO: Scale selected object
			break;
		}
		}
	}
}