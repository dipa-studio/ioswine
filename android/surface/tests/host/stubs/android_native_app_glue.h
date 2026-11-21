/*
 * Host-test stub for <android_native_app_glue.h>.
 *
 * Only the surface server.c needs is stubbed: the android_app
 * struct fields it touches (window, config, activity, onAppCmd,
 * onInputEvent, destroyRequested) and the APP_CMD_* enum, copied in
 * the NDK r28c order so the values match the real build exactly.
 * Test-only: never compiled into the APK (the real glue header is
 * used there).
 */
#ifndef TEST_STUB_ANDROID_NATIVE_APP_GLUE_H
#define TEST_STUB_ANDROID_NATIVE_APP_GLUE_H

#include <stdint.h>
#include <stddef.h>
#include "android/native_window.h"
#include "android/configuration.h"
#include "android/native_activity.h"
#include "android/looper.h"
#include "android/input.h"

#ifdef __cplusplus
extern "C" {
#endif

struct android_app;

struct android_poll_source {
    int32_t id;
    struct android_app *app;
    void (*process)(struct android_app *app,
                    struct android_poll_source *source);
};

enum {
    APP_CMD_INPUT_CHANGED,
    APP_CMD_INIT_WINDOW,
    APP_CMD_TERM_WINDOW,
    APP_CMD_WINDOW_RESIZED,
    APP_CMD_WINDOW_REDRAW_NEEDED,
    APP_CMD_CONTENT_RECT_CHANGED,
    APP_CMD_GAINED_FOCUS,
    APP_CMD_LOST_FOCUS,
    APP_CMD_CONFIG_CHANGED,
    APP_CMD_LOW_MEMORY,
    APP_CMD_START,
    APP_CMD_RESUME,
    APP_CMD_SAVE_STATE,
    APP_CMD_PAUSE,
    APP_CMD_STOP,
    APP_CMD_DESTROY
};

struct android_app {
    void *userData;
    void (*onAppCmd)(struct android_app *app, int32_t cmd);
    int32_t (*onInputEvent)(struct android_app *app, AInputEvent *event);
    ANativeActivity *activity;
    AConfiguration *config;
    void *savedState;
    size_t savedStateSize;
    ALooper *looper;
    ANativeWindow *window;
    int destroyRequested;
};

void android_main(struct android_app *app);

#ifdef __cplusplus
}
#endif

#endif
