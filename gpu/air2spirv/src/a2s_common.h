/* a2s_common.h - shared freestanding helpers for the air2spirv sources.
 *
 * No libc here: only <stdint.h> and <stddef.h>, which are freestanding-safe.
 * The runtime (loader/rt.c) already provides memcpy/memset/memcmp/strlen
 * under their plain names; these helpers use a distinct a2s_ prefix so the
 * air2spirv objects never collide with the loader runtime at link time.
 * All helpers are static inline and take explicit lengths, so they cannot
 * read past a caller-supplied bound.
 */
#ifndef IOSWINE_A2S_COMMON_H
#define IOSWINE_A2S_COMMON_H

#include <stdint.h>
#include <stddef.h>

static inline void *a2s_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dst = (uint8_t *)d;
    const uint8_t *src = (const uint8_t *)s;
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    return d;
}

static inline void *a2s_memset(void *d, int c, size_t n)
{
    uint8_t *dst = (uint8_t *)d;
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)c;
    return d;
}

static inline int a2s_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

static inline size_t a2s_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static inline int a2s_strcmp(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == '\0')
            return 0;
        i++;
    }
}

/* Little-endian readers. The caller guarantees the bytes are in bounds;
 * these never dereference outside the supplied pointer. */
static inline uint16_t a2s_rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t a2s_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t a2s_rd64le(const uint8_t *p)
{
    return (uint64_t)a2s_rd32le(p) | ((uint64_t)a2s_rd32le(p + 4) << 32);
}

static inline void a2s_wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void a2s_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void a2s_wr64le(uint8_t *p, uint64_t v)
{
    a2s_wr32le(p, (uint32_t)(v & 0xffffffffu));
    a2s_wr32le(p + 4, (uint32_t)(v >> 32));
}

#endif /* IOSWINE_A2S_COMMON_H */
