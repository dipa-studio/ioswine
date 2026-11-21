/*
 * Host-test stub for <android/keycodes.h>.
 * Only the keycodes server.c's on_input compares against are
 * listed, with the NDK r28c values. The real NDK input.h includes
 * this header; the stub input.h does the same.
 */
#ifndef TEST_STUB_ANDROID_KEYCODES_H
#define TEST_STUB_ANDROID_KEYCODES_H

enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_VOLUME_UP = 24,
    AKEYCODE_VOLUME_DOWN = 25,
    AKEYCODE_TAB = 61,
    AKEYCODE_ENTER = 66,
    AKEYCODE_DEL = 67,
    AKEYCODE_ESCAPE = 111
};

#endif
