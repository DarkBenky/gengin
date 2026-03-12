#include "keyboar.h"

static bool prevKeys[512];
static bool prevMouse[8];
static int prevMouseX, prevMouseY;

void Input_Poll(Input *inp, struct mfb_window *win) {
	// keyboard — key buffer is one byte per key: 1 = pressed, 0 = released
	const uint8_t *keyBuf = mfb_get_key_buffer(win);
	for (int i = 0; i < 512; i++) {
		bool cur = keyBuf[i] != 0;
		inp->keysDown[i] = cur && !prevKeys[i];
		inp->keysUp[i] = !cur && prevKeys[i];
		inp->keys[i] = cur;
		prevKeys[i] = cur;
	}

	// mouse buttons — buffer indexed by mfb_mouse_button enum
	const uint8_t *btnBuf = mfb_get_mouse_button_buffer(win);
	for (int i = 0; i < 8; i++) {
		bool cur = btnBuf[i] != 0;
		inp->mouseDown[i] = cur && !prevMouse[i];
		inp->mouseUp[i] = !cur && prevMouse[i];
		inp->mouse[i] = cur;
		prevMouse[i] = cur;
	}

	// cursor
	int mx = mfb_get_mouse_x(win);
	int my = mfb_get_mouse_y(win);
	inp->mouseDX = mx - prevMouseX;
	inp->mouseDY = my - prevMouseY;
	inp->mouseX = mx;
	inp->mouseY = my;
	prevMouseX = mx;
	prevMouseY = my;

	// scroll (MiniFB resets these after each call)
	inp->scrollX = mfb_get_mouse_scroll_x(win);
	inp->scrollY = mfb_get_mouse_scroll_y(win);
}
