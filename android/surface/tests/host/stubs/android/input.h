/*
 * Host-test stub for <android/input.h>.
 * server.c's on_input reads these; the resize test never delivers
 * input, but the symbols must link. All accessors return 0 / -1
 * (no pointer, no key, no motion) unless a test overrides them.
 */
#ifndef TEST_STUB_ANDROID_INPUT_H
#define TEST_STUB_ANDROID_INPUT_H

#include <stdint.h>

#include <android/keycodes.h>

typedef struct AInputEvent AInputEvent;

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

#define AMOTION_EVENT_ACTION_MASK 0xff
#define AMOTION_EVENT_ACTION_POINTER_INDEX_MASK 0xff00
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2
#define AMOTION_EVENT_ACTION_CANCEL 3
#define AMOTION_EVENT_ACTION_POINTER_DOWN 5
#define AMOTION_EVENT_ACTION_POINTER_UP 6

struct AInputEvent {
    int unused;
};

int32_t AInputEvent_getType(const AInputEvent *event);
int32_t AKeyEvent_getAction(const AInputEvent *event);
int32_t AKeyEvent_getKeyCode(const AInputEvent *event);
int32_t AKeyEvent_getRepeatCount(const AInputEvent *event);
int32_t AMotionEvent_getAction(const AInputEvent *event);
size_t AMotionEvent_getPointerCount(const AInputEvent *event);
int32_t AMotionEvent_getPointerId(const AInputEvent *event, size_t pointerIndex);
float AMotionEvent_getX(const AInputEvent *event, size_t pointerIndex);
float AMotionEvent_getY(const AInputEvent *event, size_t pointerIndex);
float AMotionEvent_getPressure(const AInputEvent *event, size_t pointerIndex);
size_t AMotionEvent_getHistorySize(const AInputEvent *event);
float AMotionEvent_getHistoricalX(const AInputEvent *event, size_t pointerIndex,
                                  size_t historyIndex);
float AMotionEvent_getHistoricalY(const AInputEvent *event, size_t pointerIndex,
                                  size_t historyIndex);

#endif
