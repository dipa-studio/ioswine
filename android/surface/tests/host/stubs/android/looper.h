/*
 * Host-test stub for <android/looper.h>.
 * android_main's looper is never driven in the resize test; the
 * symbols must only link.
 */
#ifndef TEST_STUB_ANDROID_LOOPER_H
#define TEST_STUB_ANDROID_LOOPER_H

#include <stdint.h>

typedef struct ALooper ALooper;

#define ALOOPER_POLL_WAKE -1
#define ALOOPER_POLL_CALLBACK -2
#define ALOOPER_POLL_TIMEOUT -3
#define ALOOPER_POLL_ERROR -4

struct ALooper {
    int unused;
};

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents,
                     void **outData);
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData);

#endif
