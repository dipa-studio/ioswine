/*
 * Host-test stub for <android/choreographer.h>.
 * android_main posts the vsync callback; the resize test never
 * calls android_main, so the symbols must only link.
 */
#ifndef TEST_STUB_ANDROID_CHOREOGRAPHER_H
#define TEST_STUB_ANDROID_CHOREOGRAPHER_H

#include <stdint.h>

typedef struct AChoreographer AChoreographer;

struct AChoreographer {
    int unused;
};

typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos,
                                               void *data);

AChoreographer *AChoreographer_getInstance(void);
void AChoreographer_postFrameCallback64(AChoreographer *choreographer,
                                        AChoreographer_frameCallback64 callback,
                                        void *data);

#endif
