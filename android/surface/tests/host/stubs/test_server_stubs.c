/*
 * Implementations for the host-test Android stubs (see stubs/android/
 * and stubs/android_native_app_glue.h). Test-only: never compiled
 * into the APK.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android/log.h"
#include "android/native_window.h"
#include "android/configuration.h"
#include "android/native_activity.h"
#include "android/input.h"
#include "android/looper.h"
#include "android/choreographer.h"
#include "android_native_app_glue.h"

/* ------------------------------------------------------------------ */
/* Log capture                                                        */
/* ------------------------------------------------------------------ */

static char g_log_buf[65536];
static size_t g_log_len;

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    int n;
    (void)prio;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    fprintf(stderr, "[%s] %s\n", tag ? tag : "?", line);
    /* Keep a searchable copy (truncate, never overflow). */
    if (g_log_len < sizeof(g_log_buf) - 1) {
        size_t avail = sizeof(g_log_buf) - 1 - g_log_len;
        size_t w = (size_t)n < avail ? (size_t)n : avail;
        memcpy(g_log_buf + g_log_len, line, w);
        g_log_len += w;
        g_log_buf[g_log_len] = '\0';
        if (g_log_len + 1 < sizeof(g_log_buf)) {
            g_log_buf[g_log_len++] = '\n';
            g_log_buf[g_log_len] = '\0';
        }
    }
    return n;
}

void test_log_reset(void) {
    g_log_len = 0;
    g_log_buf[0] = '\0';
}

int test_log_contains(const char *substr) {
    return strstr(g_log_buf, substr) != NULL;
}

/* ------------------------------------------------------------------ */
/* Fake window                                                        */
/* ------------------------------------------------------------------ */

ANativeWindow *test_window_create(int32_t width, int32_t height) {
    ANativeWindow *win = (ANativeWindow *)calloc(1, sizeof(*win));
    if (!win) return NULL;
    win->width = width;
    win->height = height;
    win->buf_w = width;
    win->buf_h = height;
    win->format = WINDOW_FORMAT_RGBA_8888;
    win->refcount = 1;
    return win;
}

void test_window_resize(ANativeWindow *window, int32_t width, int32_t height) {
    if (!window) return;
    window->width = width;
    window->height = height;
}

void test_window_destroy(ANativeWindow *window) {
    if (!window) return;
    free(window->bits);
    free(window);
}

int32_t ANativeWindow_getWidth(ANativeWindow *window) {
    return window ? window->width : 0;
}

int32_t ANativeWindow_getHeight(ANativeWindow *window) {
    return window ? window->height : 0;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width,
                                         int32_t height, int32_t format) {
    if (!window) return -1;
    window->buf_w = width;
    window->buf_h = height;
    window->format = format;
    return 0;
}

int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *outBuffer,
                           void *inOutDirtyBounds) {
    size_t need;
    (void)inOutDirtyBounds;
    if (!window || !outBuffer) return -1;
    need = (size_t)(window->width > 0 ? window->width : 1) *
           (size_t)(window->height > 0 ? window->height : 1);
    if (need > window->bits_cap) {
        uint32_t *nb = (uint32_t *)realloc(window->bits, need * sizeof(uint32_t));
        if (!nb) return -1;
        window->bits = nb;
        window->bits_cap = need;
    }
    outBuffer->bits = window->bits;
    outBuffer->width = window->width;
    outBuffer->height = window->height;
    outBuffer->stride = window->width;
    outBuffer->format = WINDOW_FORMAT_RGBA_8888;
    return 0;
}

void ANativeWindow_unlockAndPost(ANativeWindow *window) {
    (void)window;
}

void ANativeWindow_acquire(ANativeWindow *window) {
    if (window) window->refcount++;
}

void ANativeWindow_release(ANativeWindow *window) {
    if (window && window->refcount > 0) window->refcount--;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

AConfiguration *test_config_create(int32_t orientation) {
    AConfiguration *cfg = (AConfiguration *)calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;
    cfg->orientation = orientation;
    return cfg;
}

int32_t AConfiguration_getOrientation(AConfiguration *config) {
    return config ? config->orientation : ACONFIGURATION_ORIENTATION_PORT;
}

/* ------------------------------------------------------------------ */
/* Activity / input / looper / choreographer (link-only stubs)        */
/* ------------------------------------------------------------------ */

void ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags) {
    (void)activity;
    (void)flags;
}

void ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags) {
    (void)activity;
    (void)flags;
}

int32_t AInputEvent_getType(const AInputEvent *event) {
    (void)event;
    return -1;
}

int32_t AKeyEvent_getAction(const AInputEvent *event) {
    (void)event;
    return -1;
}

int32_t AKeyEvent_getKeyCode(const AInputEvent *event) {
    (void)event;
    return -1;
}

int32_t AKeyEvent_getRepeatCount(const AInputEvent *event) {
    (void)event;
    return 0;
}

int32_t AMotionEvent_getAction(const AInputEvent *event) {
    (void)event;
    return -1;
}

size_t AMotionEvent_getPointerCount(const AInputEvent *event) {
    (void)event;
    return 0;
}

int32_t AMotionEvent_getPointerId(const AInputEvent *event, size_t pointerIndex) {
    (void)event;
    (void)pointerIndex;
    return -1;
}

float AMotionEvent_getX(const AInputEvent *event, size_t pointerIndex) {
    (void)event;
    (void)pointerIndex;
    return 0.0f;
}

float AMotionEvent_getY(const AInputEvent *event, size_t pointerIndex) {
    (void)event;
    (void)pointerIndex;
    return 0.0f;
}

float AMotionEvent_getPressure(const AInputEvent *event, size_t pointerIndex) {
    (void)event;
    (void)pointerIndex;
    return 0.0f;
}

size_t AMotionEvent_getHistorySize(const AInputEvent *event) {
    (void)event;
    return 0;
}

float AMotionEvent_getHistoricalX(const AInputEvent *event, size_t pointerIndex,
                                  size_t historyIndex) {
    (void)event;
    (void)pointerIndex;
    (void)historyIndex;
    return 0.0f;
}

float AMotionEvent_getHistoricalY(const AInputEvent *event, size_t pointerIndex,
                                  size_t historyIndex) {
    (void)event;
    (void)pointerIndex;
    (void)historyIndex;
    return 0.0f;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents,
                     void **outData) {
    (void)timeoutMillis;
    (void)outFd;
    (void)outEvents;
    (void)outData;
    return ALOOPER_POLL_ERROR;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
    (void)timeoutMillis;
    (void)outFd;
    (void)outEvents;
    (void)outData;
    return ALOOPER_POLL_ERROR;
}

static AChoreographer g_choreographer;

AChoreographer *AChoreographer_getInstance(void) {
    return &g_choreographer;
}

void AChoreographer_postFrameCallback64(AChoreographer *choreographer,
                                        AChoreographer_frameCallback64 callback,
                                        void *data) {
    (void)choreographer;
    (void)callback;
    (void)data;
}
