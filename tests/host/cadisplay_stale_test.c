/* cadisplay_stale_test.c - host test for the cadisplay
 * copy-path fix: a surface shorter than the display, or narrower
 * than the present stride, must not leave the previous frame's
 * pixels in the uncovered region. Also checks the stride
 * translation for the real geometries (1080: surface 4352 vs
 * present 4320, no shear; 1440: both 5760).
 *
 * Mocks present_* and iosurface_* (same shape as cadisplay_test.c).
 * Build like host_tests.sh builds cadisplay_test, with
 * tests/host/cadisplay-mock on the include path.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cadisplay.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_I64(a, b) do { \
    int64_t _a = (int64_t)(a); int64_t _b = (int64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, \
               __LINE__, #a, (long long)_a, (long long)_b); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    uint64_t _a = (uint64_t)(a); uint64_t _b = (uint64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", __FILE__, \
               __LINE__, #a, (unsigned long long)_a, \
               (unsigned long long)_b); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Mocks                                                              */
/* ------------------------------------------------------------------ */

#define MOCK_FB_W 1440u
#define MOCK_FB_H 3200u

static uint8_t *g_fb;
static uint32_t g_fb_stride;
static int g_present_count;
static int g_present_last_index = -1;

int present_connected(void) { return 1; }

uint8_t *present_framebuffer(int index) {
    if (index < 0 || index > 1) return NULL;
    return g_fb + (size_t)index * g_fb_stride * MOCK_FB_H;
}

uint32_t present_stride(void) { return g_fb_stride; }

uint32_t present_height(void) { return MOCK_FB_H; }

int present_present(int index, const isws_rect_t *damage,
                    uint32_t damage_count) {
    (void)damage;
    (void)damage_count;
    g_present_count++;
    g_present_last_index = index;
    return 0;
}

int64_t present_last_vsync_ns(void) { return -1; }

uint64_t present_last_frame_id(void) { return (uint64_t)-1; }

#define MOCK_MAX_SURFACES 4

static iosurface_surface mock_surfaces[MOCK_MAX_SURFACES];
static uint32_t mock_surface_count;

static void mock_surface_reset(void) {
    memset(mock_surfaces, 0, sizeof(mock_surfaces));
    mock_surface_count = 0;
}

/* Fill surface id with a pattern: pixel (x, y) = 0xFF000000 | (y*4096+x). */
static uint32_t mock_surface_add(uint32_t w, uint32_t h,
                                 uint32_t bytes_per_row) {
    iosurface_surface *s;
    uint32_t y, x;
    if (mock_surface_count >= MOCK_MAX_SURFACES) return 0;
    s = &mock_surfaces[mock_surface_count];
    s->in_use = 1;
    s->id = mock_surface_count + 1;
    s->width = w;
    s->height = h;
    s->bytes_per_row = bytes_per_row;
    s->pixel_format = IOSURFACE_PIXEL_FORMAT_BGRA;
    s->bytes_per_element = 4;
    s->lock_count = 0;
    s->seed = 0;
    s->use_count = 1;
    s->fd = -1;
    s->addr = malloc((size_t)bytes_per_row * h);
    s->map_size = (uint64_t)bytes_per_row * h;
    s->plane_count = 1;
    s->planes[0].width = w;
    s->planes[0].height = h;
    s->planes[0].bytes_per_row = bytes_per_row;
    s->planes[0].offset = 0;
    s->planes[0].size = bytes_per_row * h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t v = 0xFF000000u | (y * 4096u + x);
            memcpy((uint8_t *)s->addr + (size_t)y * bytes_per_row +
                       (size_t)x * 4,
                   &v, sizeof(v));
        }
    }
    mock_surface_count++;
    return s->id;
}

static void mock_surface_free_all(void) {
    uint32_t i;
    for (i = 0; i < mock_surface_count; i++) {
        free(mock_surfaces[i].addr);
        mock_surfaces[i].addr = NULL;
    }
    mock_surface_count = 0;
}

int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    uint32_t i;
    if (!out) return KERN_INVALID_ARGUMENT;
    for (i = 0; i < mock_surface_count; i++) {
        if (mock_surfaces[i].id == surface_id) {
            *out = mock_surfaces[i];
            return KERN_SUCCESS;
        }
    }
    return KERN_INVALID_NAME;
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    uint32_t i;
    if (!fd_out) return KERN_INVALID_ARGUMENT;
    for (i = 0; i < mock_surface_count; i++) {
        if (mock_surfaces[i].id == surface_id) {
            *fd_out = mock_surfaces[i].fd;
            return KERN_SUCCESS;
        }
    }
    return KERN_INVALID_NAME;
}

uint32_t iosurface_surface_count(void) {
    return mock_surface_count;
}

static uint32_t fb_pixel(int buf, uint32_t x, uint32_t y) {
    uint32_t v = 0;
    memcpy(&v, g_fb + (size_t)buf * g_fb_stride * MOCK_FB_H +
               (size_t)y * g_fb_stride + (size_t)x * 4,
           sizeof(v));
    return v;
}

int main(void) {
    g_fb_stride = MOCK_FB_W * 4u;
    g_fb = malloc((size_t)g_fb_stride * MOCK_FB_H * 2);
    if (!g_fb) {
        printf("cadisplay_stale_test: malloc failed\n");
        return 1;
    }

    /* 1. Full-size 1080x2400 surface, 4352 stride, into a 1080x2400
     * display: rows copy exactly, no shear at row boundaries. */
    g_fb_stride = 1080u * 4u;
    memset(g_fb, 0, (size_t)g_fb_stride * MOCK_FB_H * 2);
    mock_surface_reset();
    {
        uint32_t id = mock_surface_add(1080, 2400, 4352);
        CHECK(id != 0);
        CHECK(cadisplay_init(1080, 2400, 3) == 0);
        CHECK(cadisplay_on_frame(id) == 0);
        CHECK_EQ_I64(g_present_count, 1);
        CHECK_EQ_U64(fb_pixel(0, 0, 0), 0xFF000000u);
        CHECK_EQ_U64(fb_pixel(0, 1079, 0), 0xFF000000u | 1079u);
        CHECK_EQ_U64(fb_pixel(0, 0, 1), 0xFF000000u | 4096u);
        CHECK_EQ_U64(fb_pixel(0, 1079, 2399), 0xFF000000u | (2399u * 4096u + 1079u));
        CHECK_EQ_U64(fb_pixel(0, 500, 1234), 0xFF000000u | (1234u * 4096u + 500u));
        CHECK(cadisplay_is_zero_copy(id) == 0);
        cadisplay_shutdown();
        mock_surface_free_all();
    }

    /* 2. Short surface (1080x1000) into a 1080x2400 display whose
     * buffer was pre-filled with garbage: rows below the surface
     * must be zero, not stale. */
    g_fb_stride = 1080u * 4u;
    memset(g_fb, 0xEE, (size_t)g_fb_stride * MOCK_FB_H * 2);
    mock_surface_reset();
    {
        uint32_t id = mock_surface_add(1080, 1000, 4352);
        CHECK(id != 0);
        CHECK(cadisplay_init(1080, 2400, 3) == 0);
        g_present_count = 0;
        CHECK(cadisplay_on_frame(id) == 0);
        CHECK_EQ_I64(g_present_count, 1);
        CHECK_EQ_U64(fb_pixel(0, 0, 0), 0xFF000000u);
        CHECK_EQ_U64(fb_pixel(0, 1079, 999), 0xFF000000u | (999u * 4096u + 1079u));
        CHECK_EQ_U64(fb_pixel(0, 0, 1000), 0u);
        CHECK_EQ_U64(fb_pixel(0, 1079, 2399), 0u);
        CHECK_EQ_U64(fb_pixel(0, 500, 2000), 0u);
        cadisplay_shutdown();
        mock_surface_free_all();
    }

    /* 3. Narrow surface (1000 px wide, 4000 stride) into a
     * 1080x2400 display pre-filled with garbage: the right-hand
     * strip must be zero, not stale. */
    g_fb_stride = 1080u * 4u;
    memset(g_fb, 0xEE, (size_t)g_fb_stride * MOCK_FB_H * 2);
    mock_surface_reset();
    {
        uint32_t id = mock_surface_add(1000, 2400, 4000);
        CHECK(id != 0);
        CHECK(cadisplay_init(1080, 2400, 3) == 0);
        g_present_count = 0;
        CHECK(cadisplay_on_frame(id) == 0);
        CHECK_EQ_I64(g_present_count, 1);
        CHECK_EQ_U64(fb_pixel(0, 0, 0), 0xFF000000u);
        CHECK_EQ_U64(fb_pixel(0, 999, 0), 0xFF000000u | 999u);
        CHECK_EQ_U64(fb_pixel(0, 1000, 0), 0u);
        CHECK_EQ_U64(fb_pixel(0, 1079, 2399), 0u);
        CHECK_EQ_U64(fb_pixel(0, 500, 1234), 0xFF000000u | (1234u * 4096u + 500u));
        cadisplay_shutdown();
        mock_surface_free_all();
    }

    /* 4. 1440x3200 surface, 5760 stride (== present stride): exact
     * copy, and zero-copy eligibility by stride alone holds. */
    g_fb_stride = 1440u * 4u;
    memset(g_fb, 0, (size_t)g_fb_stride * MOCK_FB_H * 2);
    mock_surface_reset();
    {
        uint32_t id = mock_surface_add(1440, 3200, 5760);
        CHECK(id != 0);
        CHECK(cadisplay_init(1440, 3200, 4) == 0);
        g_present_count = 0;
        CHECK(cadisplay_on_frame(id) == 0);
        CHECK_EQ_I64(g_present_count, 1);
        CHECK_EQ_U64(fb_pixel(0, 0, 0), 0xFF000000u);
        CHECK_EQ_U64(fb_pixel(0, 1439, 3199), 0xFF000000u | (3199u * 4096u + 1439u));
        CHECK_EQ_U64(fb_pixel(0, 1439, 0), 0xFF000000u | 1439u);
        CHECK_EQ_U64(fb_pixel(0, 0, 1), 0xFF000000u | 4096u);
        cadisplay_shutdown();
        mock_surface_free_all();
    }

    free(g_fb);
    if (g_failures == 0) {
        printf("cadisplay_stale_test: all %d checks passed\n", g_checks);
        return 0;
    }
    printf("cadisplay_stale_test: %d checks, %d FAILURES\n", g_checks,
           g_failures);
    return 1;
}
