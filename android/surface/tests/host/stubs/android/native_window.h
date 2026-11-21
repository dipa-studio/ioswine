/*
 * Host-test stub for <android/native_window.h>.
 *
 * A fake ANativeWindow is a plain struct holding the window size;
 * the test changes width/height to simulate a resize, exactly what
 * the real window does when the device rotates or the window grows.
 * ANativeWindow_lock hands out a scratch RGBA buffer sized to the
 * current window so draw_launcher_screen / redraw_last_frame can
 * run on host. Test-only: never compiled into the APK.
 */
#ifndef TEST_STUB_ANDROID_NATIVE_WINDOW_H
#define TEST_STUB_ANDROID_NATIVE_WINDOW_H

#include <stdint.h>

#define WINDOW_FORMAT_RGBA_8888 1

typedef struct ANativeWindow ANativeWindow;
typedef struct ANativeWindow_Buffer ANativeWindow_Buffer;

struct ANativeWindow_Buffer {
    void *bits;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
};

/* Test-visible fake window. width/height are the window size
 * ANativeWindow_getWidth/Height report; buf_w/buf_h are the last
 * size passed to ANativeWindow_setBuffersGeometry; bits is the
 * scratch buffer ANativeWindow_lock returns. */
struct ANativeWindow {
    int32_t width;
    int32_t height;
    int32_t buf_w;
    int32_t buf_h;
    int32_t format;
    uint32_t *bits;
    size_t bits_cap;
    int refcount;
};

int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width,
                                         int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *outBuffer,
                           void *inOutDirtyBounds);
void ANativeWindow_unlockAndPost(ANativeWindow *window);
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);

/* Test helpers (implemented in test_server_stubs.c). */
ANativeWindow *test_window_create(int32_t width, int32_t height);
void test_window_resize(ANativeWindow *window, int32_t width, int32_t height);
void test_window_destroy(ANativeWindow *window);

#endif
