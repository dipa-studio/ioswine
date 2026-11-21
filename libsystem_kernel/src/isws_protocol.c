/* Presenter wire protocol (shared with android/surface), compiled into libsystem_kernel.
 * Freestanding: block the host <string.h> and declare what protocol.c uses. */
#include <stddef.h>
#define _STRING_H 1
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#include "../../android/surface/src/common/protocol.c"
