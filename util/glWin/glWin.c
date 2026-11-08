#include "glWin.h"
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <CL/cl_gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CL_GL_CONTEXT_KHR
#define CL_GL_CONTEXT_KHR 0x2008
#endif
#ifndef CL_GLX_DISPLAY_KHR
#define CL_GLX_DISPLAY_KHR 0x200A
#endif

struct mfb_window {
	Display *display;
	Window window;
	Atom wmDelete;
	GLXContext glContext;
	GLXFBConfig fbConfig;
	int width;
	int height;
	int bufferWidth;
	int bufferHeight;
	GLuint texture;
	GLuint pbo;
	uint8_t keyBuffer[512];
	uint8_t mouseButtonBuffer[8];
	int mouseX;
	int mouseY;
	float scrollX;
	float scrollY;
	int closed;
	CL_Context cl;
	CL_Buffer presentBuf;
};

static int translateKeysym(KeySym ks) {
	switch (ks) {
		case XK_Escape: return KB_KEY_ESCAPE;
		case XK_Return: return KB_KEY_ENTER;
		case XK_Tab: return KB_KEY_TAB;
		case XK_BackSpace: return KB_KEY_BACKSPACE;
		case XK_Insert: return KB_KEY_INSERT;
		case XK_Delete: return KB_KEY_DELETE;
		case XK_Right: return KB_KEY_RIGHT;
		case XK_Left: return KB_KEY_LEFT;
		case XK_Down: return KB_KEY_DOWN;
		case XK_Up: return KB_KEY_UP;
		case XK_Prior: return KB_KEY_PAGE_UP;
		case XK_Next: return KB_KEY_PAGE_DOWN;
		case XK_Home: return KB_KEY_HOME;
		case XK_End: return KB_KEY_END;
		case XK_Caps_Lock: return KB_KEY_CAPS_LOCK;
		case XK_Scroll_Lock: return KB_KEY_SCROLL_LOCK;
		case XK_Num_Lock: return KB_KEY_NUM_LOCK;
		case XK_Print: return KB_KEY_PRINT_SCREEN;
		case XK_Pause: return KB_KEY_PAUSE;
		case XK_Shift_L: return KB_KEY_LEFT_SHIFT;
		case XK_Shift_R: return KB_KEY_RIGHT_SHIFT;
		case XK_Control_L: return KB_KEY_LEFT_CONTROL;
		case XK_Control_R: return KB_KEY_RIGHT_CONTROL;
		case XK_Alt_L: return KB_KEY_LEFT_ALT;
		case XK_Alt_R: return KB_KEY_RIGHT_ALT;
		case XK_Super_L: return KB_KEY_LEFT_SUPER;
		case XK_Super_R: return KB_KEY_RIGHT_SUPER;
		case XK_Menu: return KB_KEY_MENU;
		case XK_KP_Enter: return KB_KEY_KP_ENTER;
		case XK_KP_Add: return KB_KEY_KP_ADD;
		case XK_KP_Subtract: return KB_KEY_KP_SUBTRACT;
		case XK_KP_Multiply: return KB_KEY_KP_MULTIPLY;
		case XK_KP_Divide: return KB_KEY_KP_DIVIDE;
		case XK_KP_Equal: return KB_KEY_KP_EQUAL;
		case XK_KP_Decimal: return KB_KEY_KP_DECIMAL;
		case XK_KP_Space: return KB_KEY_SPACE;
	}
	if (ks >= XK_KP_0 && ks <= XK_KP_9) return KB_KEY_KP_0 + (int)(ks - XK_KP_0);
	if (ks >= XK_F1 && ks <= XK_F24) return KB_KEY_F1 + (int)(ks - XK_F1);
	if (ks >= XK_a && ks <= XK_z) return KB_KEY_A + (int)(ks - XK_a);
	if (ks >= XK_space && ks <= XK_grave) return (int)ks;
	return KB_KEY_UNKNOWN;
}

static void setMouseButton(struct mfb_window *win, unsigned int button, int value) {
	switch (button) {
		case Button1: win->mouseButtonBuffer[MFB_MOUSE_BTN_1] = value; break;
		case Button2: win->mouseButtonBuffer[MFB_MOUSE_BTN_3] = value; break;
		case Button3: win->mouseButtonBuffer[MFB_MOUSE_BTN_2] = value; break;
		case 6: if (value) win->scrollX += 1.0f; break;
		case 7: if (value) win->scrollX -= 1.0f; break;
		case 8: win->mouseButtonBuffer[MFB_MOUSE_BTN_4] = value; break;
		case 9: win->mouseButtonBuffer[MFB_MOUSE_BTN_5] = value; break;
		default: break;
	}
}

static void pollEvents(struct mfb_window *win) {
	while (XPending(win->display)) {
		XEvent ev;
		XNextEvent(win->display, &ev);
		switch (ev.type) {
			case KeyPress: {
				int key = translateKeysym(XLookupKeysym(&ev.xkey, 0));
				if (key >= 0 && key < 512) win->keyBuffer[key] = 1;
				break;
			}
			case KeyRelease: {
				if (XPending(win->display)) {
					XEvent next;
					XPeekEvent(win->display, &next);
					if (next.type == KeyPress && next.xkey.keycode == ev.xkey.keycode && next.xkey.time == ev.xkey.time) {
						XNextEvent(win->display, &next);
						break;
					}
				}
				int key = translateKeysym(XLookupKeysym(&ev.xkey, 0));
				if (key >= 0 && key < 512) win->keyBuffer[key] = 0;
				break;
			}
			case ButtonPress:
				win->mouseX = ev.xbutton.x;
				win->mouseY = ev.xbutton.y;
				if (ev.xbutton.button == Button4) {
					win->scrollY += 1.0f;
				} else if (ev.xbutton.button == Button5) {
					win->scrollY -= 1.0f;
				} else {
					setMouseButton(win, ev.xbutton.button, 1);
				}
				break;
			case ButtonRelease:
				win->mouseX = ev.xbutton.x;
				win->mouseY = ev.xbutton.y;
				setMouseButton(win, ev.xbutton.button, 0);
				break;
			case MotionNotify:
				win->mouseX = ev.xmotion.x;
				win->mouseY = ev.xmotion.y;
				break;
			case ConfigureNotify:
				win->width = ev.xconfigure.width;
				win->height = ev.xconfigure.height;
				break;
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == win->wmDelete) win->closed = 1;
				break;
			case DestroyNotify:
				win->closed = 1;
				break;
		}
	}
}

static void drawFrame(struct mfb_window *win) {
	glViewport(0, 0, win->width, win->height);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, win->texture);
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2f(-1.0f, 1.0f);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2f(1.0f, 1.0f);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2f(1.0f, -1.0f);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2f(-1.0f, -1.0f);
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glXSwapBuffers(win->display, win->window);
}

static int createClContext(struct mfb_window *win) {
	cl_platform_id platforms[16];
	cl_uint platformCount = 0;
	if (clGetPlatformIDs(16, platforms, &platformCount) != CL_SUCCESS) return 0;

	for (cl_uint p = 0; p < platformCount; p++) {
		cl_device_id devices[16];
		cl_uint deviceCount = 0;
		if (clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 16, devices, &deviceCount) != CL_SUCCESS) {
			if (clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, 16, devices, &deviceCount) != CL_SUCCESS) continue;
		}
		for (cl_uint d = 0; d < deviceCount; d++) {
			char ext[2048] = {0};
			clGetDeviceInfo(devices[d], CL_DEVICE_EXTENSIONS, sizeof(ext) - 1, ext, NULL);
			if (!strstr(ext, "cl_khr_gl_sharing")) continue;

			cl_context_properties props[] = {
				CL_CONTEXT_PLATFORM, (cl_context_properties)platforms[p],
				CL_GL_CONTEXT_KHR, (cl_context_properties)win->glContext,
				CL_GLX_DISPLAY_KHR, (cl_context_properties)win->display,
				0};
			cl_int err;
			cl_context ctx = clCreateContext(props, 1, &devices[d], NULL, NULL, &err);
			if (err != CL_SUCCESS || ctx == NULL) continue;

			win->cl.platform = platforms[p];
			win->cl.device = devices[d];
			win->cl.context = ctx;
			cl_queue_properties qprops[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
			win->cl.queue = clCreateCommandQueueWithProperties(ctx, devices[d], qprops, &err);
			if (err != CL_SUCCESS || win->cl.queue == NULL) return 0;
			return 1;
		}
	}
	return 0;
}

static void destroyCl(struct mfb_window *win) {
	if (win->presentBuf.buf) {
		clReleaseMemObject(win->presentBuf.buf);
		win->presentBuf.buf = NULL;
	}
	if (win->cl.queue) clReleaseCommandQueue(win->cl.queue);
	if (win->cl.context) clReleaseContext(win->cl.context);
	memset(&win->cl, 0, sizeof(win->cl));
}

struct mfb_window *mfb_open_ex(const char *title, unsigned width, unsigned height, unsigned flags) {
	(void)flags;

	struct mfb_window *win = calloc(1, sizeof(struct mfb_window));
	if (!win) return NULL;
	win->bufferWidth = (int)width;
	win->bufferHeight = (int)height;
	win->width = (int)width;
	win->height = (int)height;

	win->display = XOpenDisplay(NULL);
	if (!win->display) {
		printf("[glWin] XOpenDisplay failed\n");
		free(win);
		return NULL;
	}

	int attribs[] = {
		GLX_X_RENDERABLE, True,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_DOUBLEBUFFER, True,
		GLX_RED_SIZE, 8,
		GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8,
		None};
	int configCount = 0;
	GLXFBConfig *configs = glXChooseFBConfig(win->display, DefaultScreen(win->display), attribs, &configCount);
	if (!configs || configCount == 0) {
		printf("[glWin] no GLX framebuffer config\n");
		mfb_close(win);
		return NULL;
	}
	win->fbConfig = configs[0];

	XVisualInfo *visual = glXGetVisualFromFBConfig(win->display, win->fbConfig);
	if (!visual) {
		printf("[glWin] no GLX visual\n");
		XFree(configs);
		mfb_close(win);
		return NULL;
	}

	Window root = RootWindow(win->display, visual->screen);
	Colormap colormap = XCreateColormap(win->display, root, visual->visual, AllocNone);
	XSetWindowAttributes swa = {0};
	swa.colormap = colormap;
	swa.border_pixel = 0;
	swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
					 PointerMotionMask | StructureNotifyMask;
	win->window = XCreateWindow(win->display, root, 0, 0, width, height, 0, visual->depth,
								InputOutput, visual->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XFree(visual);
	XFree(configs);

	win->wmDelete = XInternAtom(win->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(win->display, win->window, &win->wmDelete, 1);
	XStoreName(win->display, win->window, title ? title : "gengin");
	XMapWindow(win->display, win->window);
	XAutoRepeatOff(win->display);
	XSync(win->display, False);

	win->glContext = glXCreateNewContext(win->display, win->fbConfig, GLX_RGBA_TYPE, NULL, True);
	if (!win->glContext) {
		printf("[glWin] glXCreateNewContext failed\n");
		mfb_close(win);
		return NULL;
	}
	if (!glXMakeCurrent(win->display, win->window, win->glContext)) {
		printf("[glWin] glXMakeCurrent failed\n");
		mfb_close(win);
		return NULL;
	}

	typedef void (*swapIntervalExt)(Display *, GLXDrawable, int);
	swapIntervalExt setSwapInterval = (swapIntervalExt)glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
	if (setSwapInterval) setSwapInterval(win->display, win->window, 0);

	glGenTextures(1, &win->texture);
	glBindTexture(GL_TEXTURE_2D, win->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	size_t pixelBytes = (size_t)width * height * 4;
	glGenBuffers(1, &win->pbo);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, win->pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, pixelBytes, NULL, GL_STREAM_DRAW);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	if (createClContext(win)) {
		cl_int err;
		win->presentBuf.size = pixelBytes;
		win->presentBuf.flags = CL_MEM_WRITE_ONLY;
		win->presentBuf.buf = clCreateFromGLBuffer(win->cl.context, CL_MEM_WRITE_ONLY, win->pbo, &err);
		if (err != CL_SUCCESS || win->presentBuf.buf == NULL) {
			printf("[glWin] clCreateFromGLBuffer failed (err %d), mfb_update_gpu disabled\n", err);
			destroyCl(win);
		}
	} else {
		printf("[glWin] no OpenCL device with CL/GL sharing, mfb_update_gpu disabled\n");
	}

	return win;
}

struct mfb_window *mfb_open(const char *title, unsigned width, unsigned height) {
	return mfb_open_ex(title, width, height, 0);
}

mfb_update_state mfb_update(struct mfb_window *window, void *buffer) {
	if (!window) return STATE_INVALID_WINDOW;
	if (!buffer) return STATE_INVALID_BUFFER;
	pollEvents(window);
	if (window->closed) return STATE_EXIT;

	glXMakeCurrent(window->display, window->window, window->glContext);
	glBindTexture(GL_TEXTURE_2D, window->texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, window->bufferWidth, window->bufferHeight, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
	glBindTexture(GL_TEXTURE_2D, 0);
	drawFrame(window);
	return STATE_OK;
}

mfb_update_state mfb_update_gpu(struct mfb_window *window, CL_Buffer *buffer) {
	if (!window) return STATE_INVALID_WINDOW;
	if (!buffer || buffer->buf == NULL) return STATE_INVALID_BUFFER;
	if (window->cl.context == NULL || window->presentBuf.buf == NULL) return STATE_INTERNAL_ERROR;
	pollEvents(window);
	if (window->closed) return STATE_EXIT;

	cl_int err;
	cl_mem mem = window->presentBuf.buf;
	err = clEnqueueAcquireGLObjects(window->cl.queue, 1, &mem, 0, NULL, NULL);
	if (err != CL_SUCCESS) return STATE_INTERNAL_ERROR;
	err = clEnqueueCopyBuffer(window->cl.queue, buffer->buf, mem, 0, 0, buffer->size, 0, NULL, NULL);
	if (err == CL_SUCCESS) err = clEnqueueReleaseGLObjects(window->cl.queue, 1, &mem, 0, NULL, NULL);
	clFinish(window->cl.queue);
	if (err != CL_SUCCESS) return STATE_INTERNAL_ERROR;

	glXMakeCurrent(window->display, window->window, window->glContext);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, window->pbo);
	glBindTexture(GL_TEXTURE_2D, window->texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, window->bufferWidth, window->bufferHeight, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	drawFrame(window);
	return STATE_OK;
}

CL_Context *mfb_get_cl_context(struct mfb_window *window) {
	if (!window || window->cl.context == NULL) return NULL;
	return &window->cl;
}

void mfb_set_target_fps(unsigned fps) {
	(void)fps;
}

unsigned mfb_get_window_width(struct mfb_window *window) {
	if (!window) return 0;
	return (unsigned)window->width;
}

unsigned mfb_get_window_height(struct mfb_window *window) {
	if (!window) return 0;
	return (unsigned)window->height;
}

const uint8_t *mfb_get_key_buffer(struct mfb_window *window) {
	if (!window) return NULL;
	return window->keyBuffer;
}

const uint8_t *mfb_get_mouse_button_buffer(struct mfb_window *window) {
	if (!window) return NULL;
	return window->mouseButtonBuffer;
}

int mfb_get_mouse_x(struct mfb_window *window) {
	if (!window) return 0;
	return window->mouseX;
}

int mfb_get_mouse_y(struct mfb_window *window) {
	if (!window) return 0;
	return window->mouseY;
}

float mfb_get_mouse_scroll_x(struct mfb_window *window) {
	if (!window) return 0.0f;
	float v = window->scrollX;
	window->scrollX = 0.0f;
	return v;
}

float mfb_get_mouse_scroll_y(struct mfb_window *window) {
	if (!window) return 0.0f;
	float v = window->scrollY;
	window->scrollY = 0.0f;
	return v;
}

void mfb_close(struct mfb_window *window) {
	if (!window) return;
	destroyCl(window);
	if (window->glContext) {
		glXMakeCurrent(window->display, window->window, window->glContext);
		if (window->pbo) glDeleteBuffers(1, &window->pbo);
		if (window->texture) glDeleteTextures(1, &window->texture);
		glXMakeCurrent(window->display, None, NULL);
		glXDestroyContext(window->display, window->glContext);
	}
	if (window->display) {
		XAutoRepeatOn(window->display);
		XCloseDisplay(window->display);
	}
	free(window);
}
