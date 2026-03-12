#ifndef KEYBOAR_H
#define KEYBOAR_H

#include <MiniFB.h>
#include <stdbool.h>

typedef struct {
	// keyboard — indexed by mfb_key (KB_KEY_*)
	bool keys[512];       // held this frame
	bool keysDown[512];   // pressed this frame (rising edge)
	bool keysUp[512];     // released this frame (falling edge)

	// mouse buttons — use MOUSE_LEFT / MOUSE_RIGHT / MOUSE_MIDDLE from MiniFB
	bool mouse[8];
	bool mouseDown[8];
	bool mouseUp[8];

	// cursor position and delta
	int mouseX, mouseY;
	int mouseDX, mouseDY;

	// scroll
	float scrollX, scrollY;
} Input;

// Call once per frame before mfb_update/wait_sync
void Input_Poll(Input *inp, struct mfb_window *win);

static inline bool Input_KeyHeld  (const Input *inp, mfb_key k)        { return inp->keys[k]; }
static inline bool Input_KeyDown  (const Input *inp, mfb_key k)        { return inp->keysDown[k]; }
static inline bool Input_KeyUp    (const Input *inp, mfb_key k)        { return inp->keysUp[k]; }

static inline bool Input_MouseHeld(const Input *inp, mfb_mouse_button b) { return inp->mouse[b]; }
static inline bool Input_MouseDown(const Input *inp, mfb_mouse_button b) { return inp->mouseDown[b]; }
static inline bool Input_MouseUp  (const Input *inp, mfb_mouse_button b) { return inp->mouseUp[b]; }

#endif // KEYBOAR_H
