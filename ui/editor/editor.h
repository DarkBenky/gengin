#include <math.h>
#include <string.h>

#include "../../math/transform.h"
#include "../../math/vector3.h"
#include "../../object/format.h"
#include "../../object/object.h"
#include "../../object/scene.h"
#include "../../util/threadPool.h"
#include "../../keyboard/keyboard.h"
#include "../../render/cpu/font.h"
#include "MiniFB_enums.h"
#include <MiniFB.h>

#define ROWS_PER_TASK 8
#define DEG2RAD 0.017453292519943295f // engine stores rotations in radians
#define RAD2DEG 57.29577951308232f
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
const int uniformScaleKey = KB_KEY_KP_1;

const float snapToGridSize = 1.0f;
const float snapToAngleSize = 15.0f; // degrees

typedef struct editorUi {
	ObjectList *scene;
	Camera *cam;
	struct Alphabet *alphabet;
	Input *input;			// TODO: integrate Input into Editor
	Object *selectedObject; // if NULL, no object is selected
	enum EditorMode mode;
	bool editorActive;
	bool snapToGrid;
	bool uniformScale;
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
	ui->moveSpeed = 5.0f;
	ui->rotationSpeed = 90.0f; // degrees per second
	ui->scaleSpeed = 0.1f;
	ui->snapToGrid = false;
	ui->uniformScale = false;
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

static const char *modeName(enum EditorMode mode) {
	switch (mode) {
	case ROTATE_MODE:
		return "rotate";
	case SCALE_MODE:
		return "scale";
	default:
		return "move";
	}
}

static float3 typeNameColor(const char *typeName) {
	if (strncmp(typeName, "__buildin_", 10) == 0) {
		float3 cyan = {0.45f, 0.85f, 1.0f};
		return cyan;
	}
	if (strncmp(typeName, "__custom_obj_", 13) == 0) {
		float3 green = {0.35f, 1.0f, 0.35f};
		return green;
	}
	float3 amber = {1.0f, 0.75f, 0.3f}; // merged or generated objects
	return amber;
}

// cuts the tail of text until it fits maxWidth, marking the cut with ".."
static void trimTextToWidth(struct Alphabet *alphabet, char *text, float scale, int maxWidth) {
	int len = (int)strlen(text);
	bool trimmed = false;
	while (len > 0 && MeasureTextWidth(alphabet, text, scale) > maxWidth) {
		len--;
		text[len] = '\0';
		trimmed = true;
	}
	if (!trimmed || len <= 2) return;
	int ellipsisWidth = MeasureTextWidth(alphabet, "..", scale);
	while (len > 2 && MeasureTextWidth(alphabet, text, scale) + ellipsisWidth > maxWidth) {
		len--;
		text[len] = '\0';
	}
	strcat(text, "..");
}

static void dimRect(float3 *framebuffer, int fbWidth, int fbHeight, int left, int top, int right, int bottom, float factor) {
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > fbWidth) right = fbWidth;
	if (bottom > fbHeight) bottom = fbHeight;
	for (int row = top; row < bottom; row++) {
		float3 *line = framebuffer + row * fbWidth;
		for (int col = left; col < right; col++) {
			line[col].x *= factor;
			line[col].y *= factor;
			line[col].z *= factor;
		}
	}
}

void drawStats(editorUi *ui, int x, int y, int width, int height) {
	const int characterSize = 8;
	const int panelPad = 5;
	float3 white = {1.0f, 1.0f, 1.0f};
	float3 yellow = {1.0f, 1.0f, 0.0f};

	// editor inactive: only advertise how to open it
	if (!ui->editorActive) {
		const float hintScale = 1.0f;
		int hintWidth = MeasureTextWidth(ui->alphabet, "Tab", hintScale) + MeasureTextWidth(ui->alphabet, " ", hintScale) + MeasureTextWidth(ui->alphabet, "open editor", hintScale);
		dimRect(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, x - panelPad, y - panelPad, x + hintWidth + panelPad, y + (int)(characterSize * hintScale) + panelPad, 0.25f);

		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, "Tab", x, y, hintScale, yellow);
		int descX = x + MeasureTextWidth(ui->alphabet, "Tab", hintScale) + MeasureTextWidth(ui->alphabet, " ", hintScale);
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, "open editor", descX, y, hintScale, white);
		return;
	}

	const Object *selected = ui->selectedObject;

	char lines[22][32];
	int n = 0;

	snprintf(lines[n++], 32, "mode: %s  snap: %s", modeName(ui->mode), ui->snapToGrid ? "on" : "off");

	// ObjectPathOrType is zero initialized: cut at the first NUL so the
	char typeText[sizeof(ui->selectedObject->ObjectPathOrType) + 4];
	int typeRow = -1;
	if (selected != NULL) {
		int typeLen = 0;
		while (typeLen < (int)sizeof(selected->ObjectPathOrType) && selected->ObjectPathOrType[typeLen] != '\0') typeLen++;
		memcpy(typeText, selected->ObjectPathOrType, typeLen);
		typeText[typeLen] = '\0';
		typeRow = n;
		lines[n++][0] = '\0';
	}

	if (selected != NULL) {
		float3 pos = selected->position;
		float3 rot = selected->rotation;
		float3 scl = selected->scale;
		int triCount = selected->triangleCount;

		snprintf(lines[n++], 32, "position");
		snprintf(lines[n++], 32, "    x: %.2f", pos.x);
		snprintf(lines[n++], 32, "    y: %.2f", pos.y);
		snprintf(lines[n++], 32, "    z: %.2f", pos.z);
		snprintf(lines[n++], 32, "rotation deg");
		snprintf(lines[n++], 32, "    x: %.2f", rot.x * RAD2DEG);
		snprintf(lines[n++], 32, "    y: %.2f", rot.y * RAD2DEG);
		snprintf(lines[n++], 32, "    z: %.2f", rot.z * RAD2DEG);
		snprintf(lines[n++], 32, "scale");
		snprintf(lines[n++], 32, "    x: %.2f", scl.x);
		snprintf(lines[n++], 32, "    y: %.2f", scl.y);
		snprintf(lines[n++], 32, "    z: %.2f", scl.z);
		snprintf(lines[n++], 32, "triangles");
		fmtTriangles(lines[n++], 32, triCount);
	}

	lines[n][0] = '\0';
	n++;

	char snapHint[32], uniformHint[32];
	snprintf(snapHint, 32, "snap %s", ui->snapToGrid ? "off" : "on");
	snprintf(uniformHint, 32, "uniform %s", ui->uniformScale ? "off" : "on");

	// key token renders yellow, its description white
	const char *hintKeys[] = {"keys", "    click", "    1/2/3", "    arrows", "    RShift/RCtrl", "    KP0", "    KP1", "    Tab"};
	const char *hintDescs[] = {"", "select", "move rotate scale", "X/Z", "Y", snapHint, uniformHint, "exit editor"};
	const int hintCount = (int)(sizeof(hintKeys) / sizeof(hintKeys[0]));

	int maxChars = 0;
	for (int i = 0; i < n; i++) {
		int len = (int)strlen(lines[i]);
		if (len > maxChars) maxChars = len;
	}
	for (int i = 0; i < hintCount; i++) {
		int len = (int)strlen(hintKeys[i]) + 1 + (int)strlen(hintDescs[i]);
		if (len > maxChars) maxChars = len;
	}

	int totalRows = n + hintCount;
	float scaleX = (float)width / (maxChars * characterSize);
	float scaleY = (float)height / (totalRows * characterSize);
	float scale = scaleX < scaleY ? scaleX : scaleY;
	if (scale >= 1.0f) scale = (float)(int)scale;

	int lineHeight = (int)(characterSize * scale);

	// type line: keep the panel's font size and cut the tail, readable first
	const char *typeLabel = "type: ";
	int typeLabelWidth = 0;
	int typeTextWidth = 0;
	if (typeRow >= 0) {
		typeLabelWidth = MeasureTextWidth(ui->alphabet, typeLabel, scale);
		trimTextToWidth(ui->alphabet, typeText, scale, width - typeLabelWidth);
		typeTextWidth = MeasureTextWidth(ui->alphabet, typeText, scale);
	}

	int panelWidth = (int)(maxChars * characterSize * scale);
	if (typeRow >= 0 && typeLabelWidth + typeTextWidth > panelWidth) panelWidth = typeLabelWidth + typeTextWidth;
	int panelHeight = totalRows * lineHeight;
	dimRect(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, x - panelPad, y - panelPad, x + panelWidth + panelPad, y + panelHeight + panelPad, 0.25f);

	float3 cyan = {0.45f, 0.85f, 1.0f};
	float3 green = {0.35f, 1.0f, 0.35f};
	float3 red = {1.0f, 0.3f, 0.3f};

	int topX = x;
	const char *modeLabel = "mode: ";
	RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, modeLabel, topX, y, scale, white);
	topX += MeasureTextWidth(ui->alphabet, modeLabel, scale);
	RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, modeName(ui->mode), topX, y, scale, cyan);
	topX += MeasureTextWidth(ui->alphabet, modeName(ui->mode), scale);
	const char *snapLabel = "  snap: ";
	RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, snapLabel, topX, y, scale, white);
	topX += MeasureTextWidth(ui->alphabet, snapLabel, scale);
	RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, ui->snapToGrid ? "on" : "off", topX, y, scale, ui->snapToGrid ? green : red);

	for (int i = 1; i < n; i++) {
		if (i == typeRow) continue;
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, lines[i], x, y + i * lineHeight, scale, white);
	}

	if (typeRow >= 0) {
		int rowY = y + typeRow * lineHeight;
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, typeLabel, x, rowY, scale, white);
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, typeText, x + typeLabelWidth, rowY, scale, typeNameColor(typeText));
	}

	for (int i = 0; i < hintCount; i++) {
		int rowY = y + (n + i) * lineHeight;
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, hintKeys[i], x, rowY, scale, yellow);
		int descX = x + MeasureTextWidth(ui->alphabet, hintKeys[i], scale) + MeasureTextWidth(ui->alphabet, " ", scale);
		RenderTextHdr(ui->cam->framebufferHdr, ui->cam->screenWidth, ui->cam->screenHeight, ui->alphabet, hintDescs[i], descX, rowY, scale, white);
	}
}

void editorStep(editorUi *ui, struct mfb_window *window, float dt, int x, int y, int width, int height) {
	(void)window; // input is polled once per frame by the caller
	if (dt < 0.0f) dt = 0.0f; // speeds are per second: 1.0 = one unit per second

	drawStats(ui, x, y, width, height);

	Input *input = ui->input;
	if (input->keysDown[ToggleEditor]) {
		if (ui->editorActive) {
			deactivateEditorUi(ui);
			return;
		} else {
			activateEditorUi(ui);
		}
	}

	if (ui->editorActive) {
		if (input->keysDown[SnapKey]) {
			ui->snapToGrid = !ui->snapToGrid;
		}
		if (input->keysDown[uniformScaleKey]) {
			ui->uniformScale = !ui->uniformScale;
		}

		if (input->keysDown[MOVE_MODE]) {
			ui->mode = MOVE_MODE;
		} else if (input->keysDown[ROTATE_MODE]) {
			ui->mode = ROTATE_MODE;
		} else if (input->keysDown[SCALE_MODE]) {
			ui->mode = SCALE_MODE;
		}

		if (ui->selectedObject == NULL) return;

		switch (ui->mode) {
			case MOVE_MODE: {
				float step = ui->moveSpeed * dt;
				bool moving = input->keys[ForwardKey] || input->keys[BackwardKey] || input->keys[LeftKey] || input->keys[RightKey] || input->keys[UpKey] || input->keys[DownKey];
				// Y is up in this engine: arrows move on the X/Z ground plane, Shift/Ctrl move on Y
				float yaw = ui->selectedObject->rotation.y;
				float sy = sinf(yaw), cy = cosf(yaw);
				if (input->keys[ForwardKey]) { // local +Z
					ui->selectedObject->position.x += step * sy;
					ui->selectedObject->position.z += step * cy;
				} else if (input->keys[BackwardKey]) {
					ui->selectedObject->position.x -= step * sy;
					ui->selectedObject->position.z -= step * cy;
				} else if (input->keys[LeftKey]) { // local -X
					ui->selectedObject->position.x -= step * cy;
					ui->selectedObject->position.z += step * sy;
				} else if (input->keys[RightKey]) { // local +X
					ui->selectedObject->position.x += step * cy;
					ui->selectedObject->position.z -= step * sy;
				} else if (input->keys[UpKey]) {
					ui->selectedObject->position.y += step;
				} else if (input->keys[DownKey]) {
					ui->selectedObject->position.y -= step;
				}

				if (ui->snapToGrid && !moving) {
					ui->selectedObject->position.x = roundf(ui->selectedObject->position.x / snapToGridSize) * snapToGridSize;
					ui->selectedObject->position.y = roundf(ui->selectedObject->position.y / snapToGridSize) * snapToGridSize;
					ui->selectedObject->position.z = roundf(ui->selectedObject->position.z / snapToGridSize) * snapToGridSize;
				}

				Object_UpdateWorldBounds(ui->selectedObject);

				break;
			}
			case ROTATE_MODE: {
				float step = ui->rotationSpeed * dt * DEG2RAD; // deg/s -> rad/s
				bool rotating = input->keys[ForwardKey] || input->keys[BackwardKey] || input->keys[LeftKey] || input->keys[RightKey] || input->keys[UpKey] || input->keys[DownKey];

				// spin around the local bbox center instead of the model origin
				float3 localCenter = {
					0.5f * (ui->selectedObject->BBmin.x + ui->selectedObject->BBmax.x),
					0.5f * (ui->selectedObject->BBmin.y + ui->selectedObject->BBmax.y),
					0.5f * (ui->selectedObject->BBmin.z + ui->selectedObject->BBmax.z)};
				float3 centerOffset = Float3_Mul(localCenter, ui->selectedObject->scale);
				float3 oldCenter = RotateXYZ(centerOffset, ui->selectedObject->rotation);

				if (input->keys[ForwardKey]) {
					ui->selectedObject->rotation.y += step;
				} else if (input->keys[BackwardKey]) {
					ui->selectedObject->rotation.y -= step;
				} else if (input->keys[LeftKey]) {
					ui->selectedObject->rotation.x += step;
				} else if (input->keys[RightKey]) {
					ui->selectedObject->rotation.x -= step;
				} else if (input->keys[UpKey]) {
					ui->selectedObject->rotation.z += step;
				} else if (input->keys[DownKey]) {
					ui->selectedObject->rotation.z -= step;
				}

				if (ui->snapToGrid && !rotating) {
					const float snapRad = snapToAngleSize * DEG2RAD;
					ui->selectedObject->rotation.x = roundf(ui->selectedObject->rotation.x / snapRad) * snapRad;
					ui->selectedObject->rotation.y = roundf(ui->selectedObject->rotation.y / snapRad) * snapRad;
					ui->selectedObject->rotation.z = roundf(ui->selectedObject->rotation.z / snapRad) * snapRad;
				}

				// keep the bbox center where it was so the object spins in place
				float3 newCenter = RotateXYZ(centerOffset, ui->selectedObject->rotation);
				ui->selectedObject->position = Float3_Add(ui->selectedObject->position, Float3_Sub(oldCenter, newCenter));

				Object_UpdateWorldBounds(ui->selectedObject);

				break;
			}
			case SCALE_MODE: {
				float step = ui->scaleSpeed * dt;
				bool scaling = input->keys[ForwardKey] || input->keys[BackwardKey] || input->keys[LeftKey] || input->keys[RightKey] || input->keys[UpKey] || input->keys[DownKey];
				if (ui->uniformScale) {
					if (input->keys[ForwardKey] || input->keys[BackwardKey] || input->keys[LeftKey] || input->keys[RightKey] || input->keys[UpKey] || input->keys[DownKey]) {
						float scaleAmount = input->keys[ForwardKey] || input->keys[BackwardKey] ? step : input->keys[LeftKey] || input->keys[RightKey] ? -step
																								  : input->keys[UpKey] || input->keys[DownKey] ? -step
																																	: step;
						ui->selectedObject->scale.x += scaleAmount;
						ui->selectedObject->scale.y += scaleAmount;
						ui->selectedObject->scale.z += scaleAmount;
					}
				} else {
					if (input->keys[ForwardKey]) {
						ui->selectedObject->scale.x += step;
					} else if (input->keys[BackwardKey]) {
						ui->selectedObject->scale.x -= step;
					} else if (input->keys[LeftKey]) {
						ui->selectedObject->scale.y -= step;
					} else if (input->keys[RightKey]) {
						ui->selectedObject->scale.y += step;
					} else if (input->keys[UpKey]) {
						ui->selectedObject->scale.z += step;
					} else if (input->keys[DownKey]) {
						ui->selectedObject->scale.z -= step;
					}
				}

				if (ui->snapToGrid && !scaling) {
					ui->selectedObject->scale.x = roundf(ui->selectedObject->scale.x / snapToGridSize) * snapToGridSize;
					ui->selectedObject->scale.y = roundf(ui->selectedObject->scale.y / snapToGridSize) * snapToGridSize;
					ui->selectedObject->scale.z = roundf(ui->selectedObject->scale.z / snapToGridSize) * snapToGridSize;
				}

				Object_UpdateWorldBounds(ui->selectedObject);

				break;
			}
			default: {
				printf("Unknown editor mode\n");
				ui->mode = MOVE_MODE;
				break;
			}
		}
	}
}