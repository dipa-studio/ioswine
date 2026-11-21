/*
 * Host-test stub for <android/native_activity.h>.
 * server.c only passes app->activity to the show/hide soft-input
 * calls; the activity is opaque here.
 */
#ifndef TEST_STUB_ANDROID_NATIVE_ACTIVITY_H
#define TEST_STUB_ANDROID_NATIVE_ACTIVITY_H

#include <stdint.h>

typedef struct ANativeActivity ANativeActivity;

struct ANativeActivity {
    int unused;
};

#define ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT 0x0001
#define ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED 0x0002
#define ANATIVEACTIVITY_HIDE_SOFT_INPUT_IMPLICIT_ONLY 0x0001
#define ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS 0x0002

void ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags);
void ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags);

#endif
