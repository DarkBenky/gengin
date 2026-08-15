// C-only build of minifb: the C backends call release_cpp_stub() which lives in
// MiniFB_cpp.cpp (C++ API). This project never uses the C++ API, so provide a
// no-op C stub instead of pulling in the C++ runtime.
struct mfb_window;

void release_cpp_stub(struct mfb_window *window) {
	(void)window;
}
