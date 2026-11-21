/* Private memcpy/memset/memcmp for libsystem_kernel code that uses the standard names (present.c, the
 * shared presenter protocol). Hidden so they never shadow libsystem_platform's exports for Apple code. */
#include <stddef.h>
__attribute__((visibility("hidden"))) void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
__attribute__((visibility("hidden"))) void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}
__attribute__((visibility("hidden"))) int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
