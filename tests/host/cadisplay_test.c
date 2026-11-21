/*
 * cadisplay_test.c - host test for cadisplay.{c,h}.
 *
 * Mocks for exactly the present and iosurface functions the contract
 * allows cadisplay.c to call (present_connected, present_framebuffer,
 * present_stride, present_present, present_last_vsync_ns,
 * present_last_frame_id, present_last_info, iosurface_get_surface,
 * iosurface_fd, iosurface_surface_count). No real socket, no real
 * memfd. The mock surface table is test-controlled; backing buffers
 * are malloc'd and filled with a deterministic pattern.
 *
 * protocol.h and ipc.h in this directory are verbatim copies from
 * the tree (android/surface/src/common/protocol.h and
 * libsystem_kernel/src/ipc.h) so the host build is self-contained;
 * the tree is not modified.
 *
 * Build (plain):
 *   cc -std=c11 -Wall -Wextra -Werror -I src -I tests/host \
 *      tests/host/cadisplay_test.c src/cadisplay.c -o /tmp/cadisplay_test
 * Build (ASan+UBSan):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
 *      -I src -I tests/host tests/host/cadisplay_test.c src/cadisplay.c \
 *      -o /tmp/cadisplay_test_asan
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cadisplay.h"

/* ------------------------------------------------------------------ */
/* Check harness                                                        */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    g_checks++; \
    { long long _a = (long long)(a); long long _b = (long long)(b); \
      if (_a != _b) { \
          g_failures++; \
          fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n", \
                  __FILE__, __LINE__, #a, _a, _b); \
      } } \
} while (0)

/* ------------------------------------------------------------------ */
/* Mock present.c                                                       */
/* ------------------------------------------------------------------ */

/* One 1080x2400 BGRA buffer, per present buffer. Static BSS; the
 * test only touches the part its geometry needs. */
#define MOCK_FB_BYTES (4320u * 2400u)
static uint8_t mock_fb0[MOCK_FB_BYTES];
static uint8_t mock_fb1[MOCK_FB_BYTES];

static int mock_connected = 1;
static uint32_t mock_stride;
static int mock_present_count;
static int mock_present_last_index = -1;
static uint32_t mock_present_last_damage_count;
static int mock_present_fail;
static int64_t mock_last_vsync_ns = -1;
static uint64_t mock_last_frame_id = (uint64_t)-1;
static isws_info_t mock_info;

int present_connected(void) {
    return mock_connected;
}

uint8_t *present_framebuffer(int index) {
    if (!mock_connected) return NULL;
    if (index == 0) return mock_fb0;
    if (index == 1) return mock_fb1;
    return NULL;
}

uint32_t present_stride(void) {
    return mock_stride;
}

int present_present(int index, const isws_rect_t *damage,
                    uint32_t damage_count) {
    (void)damage;
    if (index < 0 || index > 1) return -1;
    mock_present_count++;
    mock_present_last_index = index;
    mock_present_last_damage_count = damage_count;
    if (mock_present_fail) return -1;
    return 0;
}

int64_t present_last_vsync_ns(void) {
    return mock_last_vsync_ns;
}

uint64_t present_last_frame_id(void) {
    return mock_last_frame_id;
}

isws_info_t present_last_info(void) {
    return mock_info;
}

/* ------------------------------------------------------------------ */
/* Mock iosurface.c                                                     */
/* ------------------------------------------------------------------ */

#define MOCK_MAX_SURFACES 16
static iosurface_surface mock_surfaces[MOCK_MAX_SURFACES];
static uint32_t mock_surface_n;

static int mock_surface_find(uint32_t surface_id) {
    uint32_t i;
    for (i = 0; i < mock_surface_n; i++) {
        if (mock_surfaces[i].id == surface_id) return (int)i;
    }
    return -1;
}

/* Add a mock surface. Backing is malloc'd and filled with a
 * deterministic pattern (byte i = (i + id) & 0xff). Returns the
 * backing buffer, or NULL if the table is full or malloc fails. */
static uint8_t *mock_surface_add(uint32_t id, uint32_t width,
                                 uint32_t height, uint32_t bytes_per_row,
                                 uint32_t pixel_format, uint32_t lock_count,
                                 uint32_t seed, int fd) {
    iosurface_surface *s;
    uint8_t *backing;
    uint64_t nbytes;
    uint64_t i;
    if (mock_surface_n >= MOCK_MAX_SURFACES) return NULL;
    nbytes = (uint64_t)bytes_per_row * height;
    backing = (uint8_t *)malloc((size_t)nbytes);
    if (backing == NULL) return NULL;
    for (i = 0; i < nbytes; i++) backing[i] = (uint8_t)((i + id) & 0xffu);
    s = &mock_surfaces[mock_surface_n];
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    s->id = id;
    s->width = width;
    s->height = height;
    s->bytes_per_row = bytes_per_row;
    s->pixel_format = pixel_format;
    s->bytes_per_element = 4;
    s->alloc_size = nbytes;
    s->lock_count = lock_count;
    s->seed = seed;
    s->use_count = 1;
    s->fd = fd;
    s->addr = backing;
    s->map_size = nbytes;
    mock_surface_n++;
    return backing;
}

static void mock_surface_clear(void) {
    uint32_t i;
    for (i = 0; i < mock_surface_n; i++) {
        free(mock_surfaces[i].addr);
        mock_surfaces[i].addr = NULL;
    }
    mock_surface_n = 0;
}

static int mock_surface_set_seed(uint32_t id, uint32_t seed) {
    int idx = mock_surface_find(id);
    if (idx < 0) return -1;
    mock_surfaces[idx].seed = seed;
    return 0;
}

int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    int idx;
    if (out == NULL) return KERN_INVALID_ARGUMENT;
    idx = mock_surface_find(surface_id);
    if (idx < 0) return KERN_INVALID_NAME;
    *out = mock_surfaces[idx];
    return KERN_SUCCESS;
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    int idx;
    if (fd_out == NULL) return KERN_INVALID_ARGUMENT;
    idx = mock_surface_find(surface_id);
    if (idx < 0) return KERN_INVALID_NAME;
    *fd_out = mock_surfaces[idx].fd;
    return KERN_SUCCESS;
}

uint32_t iosurface_surface_count(void) {
    return mock_surface_n;
}

/* ------------------------------------------------------------------ */
/* Mock reset                                                           */
/* ------------------------------------------------------------------ */

static void mock_reset_all(void) {
    mock_connected = 1;
    mock_stride = 0;
    mock_present_count = 0;
    mock_present_last_index = -1;
    mock_present_last_damage_count = 0;
    mock_present_fail = 0;
    mock_last_vsync_ns = -1;
    mock_last_frame_id = (uint64_t)-1;
    memset(&mock_info, 0, sizeof(mock_info));
    mock_surface_clear();
    memset(mock_fb0, 0xAA, sizeof(mock_fb0));
    memset(mock_fb1, 0xAA, sizeof(mock_fb1));
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                            */
/* ------------------------------------------------------------------ */

/* Small display geometry used by most scenarios: 32x16, scale 1,
 * stride 128. */
#define SMALL_W 32u
#define SMALL_H 16u
#define SMALL_STRIDE 128u

static void test_init_shutdown(void) {
    cadisplay_info_t info;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(0, SMALL_H, 1), -1);
    CHECK_EQ_INT(cadisplay_init(SMALL_W, 0, 1), -1);
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 0), -1);
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    CHECK_EQ_INT(cadisplay_connected(), 1);
    mock_connected = 0;
    CHECK_EQ_INT(cadisplay_connected(), 0);
    mock_connected = 1;
    CHECK_EQ_INT(cadisplay_connected(), 1);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.width, SMALL_W);
    CHECK_EQ_INT(info.height, SMALL_H);
    CHECK_EQ_INT(info.scale, 1u);
    CHECK_EQ_INT(info.refresh_hz, 60u);
    CHECK_EQ_INT(info.last_vsync_ns, -1);
    CHECK_EQ_INT(info.last_frame_id, (uint64_t)-1);
    CHECK_EQ_INT(info.current_surface_id, 0u);
    CHECK_EQ_INT(info.frames_presented, 0u);
    CHECK_EQ_INT(info.frames_zero_copy, 0u);
    CHECK_EQ_INT(info.frames_copied, 0u);
    CHECK_EQ_INT(cadisplay_info(NULL), -1);
    /* Calls before init fail cleanly. */
    cadisplay_shutdown();
    CHECK_EQ_INT(cadisplay_connected(), 0);
    CHECK_EQ_INT(cadisplay_info(&info), -1);
    CHECK_EQ_INT(cadisplay_on_vsync(1000), -1);
    CHECK_EQ_INT(cadisplay_next_vsync_ns(), -1);
    CHECK_EQ_INT(cadisplay_on_frame(1), -1);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 0u);
    /* Re-init resets state. */
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 0u);
}

static void test_vsync(void) {
    cadisplay_info_t info;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    CHECK_EQ_INT(cadisplay_next_vsync_ns(), -1);
    mock_last_frame_id = 7;
    CHECK_EQ_INT(cadisplay_on_vsync(1000000000LL), 0);
    /* Period is 1e9/60 = 16666666 ns. */
    CHECK_EQ_INT(cadisplay_next_vsync_ns(), 1016666666LL);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.last_vsync_ns, 1000000000LL);
    CHECK_EQ_INT(info.last_frame_id, 7u);
    mock_last_frame_id = 8;
    CHECK_EQ_INT(cadisplay_on_vsync(1016666666LL), 0);
    CHECK_EQ_INT(cadisplay_next_vsync_ns(), 1033333332LL);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.last_vsync_ns, 1016666666LL);
    CHECK_EQ_INT(info.last_frame_id, 8u);
}

static void test_copy_path(void) {
    cadisplay_info_t info;
    uint8_t *backing;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    backing = mock_surface_add(1, SMALL_W, SMALL_H, SMALL_STRIDE,
                               IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1);
    CHECK(backing != NULL);
    CHECK_EQ_INT(cadisplay_on_frame(1), 0);
    CHECK_EQ_INT(mock_present_count, 1);
    CHECK_EQ_INT(mock_present_last_index, 0);
    CHECK_EQ_INT(mock_present_last_damage_count, 0u);
    /* Pixels landed in framebuffer 0: row 0 and row 15. */
    CHECK(memcmp(mock_fb0, backing, SMALL_STRIDE) == 0);
    CHECK(memcmp(mock_fb0 + 15u * SMALL_STRIDE,
                 backing + 15u * SMALL_STRIDE, SMALL_STRIDE) == 0);
    /* Framebuffer 1 untouched (still sentinel). */
    CHECK(mock_fb1[0] == 0xAA);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 1u);
    CHECK_EQ_INT(info.frames_copied, 1u);
    CHECK_EQ_INT(info.frames_zero_copy, 0u);
}

static void test_zero_copy(void) {
    cadisplay_info_t info;
    uint8_t *backing;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    backing = mock_surface_add(5, SMALL_W, SMALL_H, SMALL_STRIDE,
                               IOSURFACE_PIXEL_FORMAT_BGRA, 0, 3, 42);
    CHECK(backing != NULL);
    CHECK_EQ_INT(cadisplay_is_zero_copy(5), 1);
    CHECK_EQ_INT(cadisplay_set_present_fd(0, 42), 0);
    CHECK_EQ_INT(cadisplay_on_frame(5), 0);
    CHECK_EQ_INT(mock_present_count, 1);
    CHECK_EQ_INT(mock_present_last_index, 0);
    /* No copy: framebuffer 0 untouched (still sentinel). */
    CHECK(mock_fb0[0] == 0xAA);
    CHECK(mock_fb0[SMALL_STRIDE * SMALL_H - 1] == 0xAA);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 1u);
    CHECK_EQ_INT(info.frames_zero_copy, 1u);
    CHECK_EQ_INT(info.frames_copied, 0u);

    /* Zero-copy on index 1: ack index 0 first, then match fd 43. */
    mock_last_frame_id = 1;
    CHECK_EQ_INT(cadisplay_on_vsync(1000000000LL), 0);
    backing = mock_surface_add(6, SMALL_W, SMALL_H, SMALL_STRIDE,
                               IOSURFACE_PIXEL_FORMAT_BGRA, 0, 1, 43);
    CHECK(backing != NULL);
    CHECK_EQ_INT(cadisplay_set_present_fd(1, 43), 0);
    CHECK_EQ_INT(cadisplay_on_frame(6), 0);
    CHECK_EQ_INT(mock_present_last_index, 1);
    CHECK(mock_fb1[0] == 0xAA);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 2u);
    CHECK_EQ_INT(info.frames_zero_copy, 2u);

    /* Ack index 1, then present surface 5 zero-copy on index 0. */
    mock_last_frame_id = 2;
    CHECK_EQ_INT(cadisplay_on_vsync(1016666666LL), 0);
    CHECK_EQ_INT(cadisplay_on_frame(5), 0);
    CHECK_EQ_INT(mock_present_last_index, 0);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_zero_copy, 3u);
    /* Index 0 is in flight now, so surface 5 falls back to the
     * copy path on index 1 (free). */
    CHECK_EQ_INT(cadisplay_on_frame(5), 0);
    CHECK_EQ_INT(mock_present_last_index, 1);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_copied, 1u);
    CHECK_EQ_INT(info.frames_zero_copy, 3u);
    /* Both in flight now: the next on_frame has nowhere to go. */
    CHECK_EQ_INT(cadisplay_on_frame(5), -1);
}

static void test_unknown_locked(void) {
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    CHECK_EQ_INT(cadisplay_on_frame(999), -1);
    CHECK_EQ_INT(cadisplay_is_zero_copy(999), 0);
    CHECK(mock_surface_add(7, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 1, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_on_frame(7), -1);
    CHECK_EQ_INT(cadisplay_is_zero_copy(7), 0);
    /* A surface with no backing cannot be copied. Free the backing
     * first so the leak checker stays clean, then null the addr. */
    CHECK(mock_surface_add(8, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    {
        int idx = mock_surface_find(8);
        free(mock_surfaces[idx].addr);
        mock_surfaces[idx].addr = NULL;
    }
    CHECK_EQ_INT(cadisplay_on_frame(8), -1);
}

static void test_is_zero_copy(void) {
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    CHECK_EQ_INT(cadisplay_is_zero_copy(999), 0);
    CHECK(mock_surface_add(8, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_420V, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_is_zero_copy(8), 0);
    CHECK(mock_surface_add(9, SMALL_W, SMALL_H, 256,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_is_zero_copy(9), 0);
    CHECK(mock_surface_add(10, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_is_zero_copy(10), 1);
    /* Stride 0 (presenter not connected) matches nothing real. */
    mock_stride = 0;
    CHECK_EQ_INT(cadisplay_is_zero_copy(10), 0);
    mock_stride = SMALL_STRIDE;
    CHECK_EQ_INT(cadisplay_is_zero_copy(10), 1);
}

static void test_find_display_surface(void) {
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    /* Seed-aware preference: id 5 has the highest seed, so it wins
     * even though id 9 is more recently created. */
    CHECK(mock_surface_add(5, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 10, -1) != NULL);
    CHECK(mock_surface_add(9, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 3, -1) != NULL);
    /* Wrong size and wrong format never match, whatever the seed. */
    CHECK(mock_surface_add(3, 64, SMALL_H, 256,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 99, -1) != NULL);
    CHECK(mock_surface_add(12, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_420V, 0, 50, -1) != NULL);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 5u);
    /* Seed tie: raise id 9 to seed 10; the tiebreak is
     * most-recently-created, so id 9 wins. */
    CHECK_EQ_INT(mock_surface_set_seed(9, 10), 0);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 9u);
    /* All seeds equal: highest id wins. */
    CHECK_EQ_INT(mock_surface_set_seed(5, 0), 0);
    CHECK_EQ_INT(mock_surface_set_seed(9, 0), 0);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 9u);
    /* A lower id with a higher seed beats a higher id again. */
    CHECK_EQ_INT(mock_surface_set_seed(5, 7), 0);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 5u);
    /* No match: only wrong-size / wrong-format surfaces. */
    mock_surface_clear();
    CHECK(mock_surface_add(20, 64, SMALL_H, 256,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK(mock_surface_add(21, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_420V, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 0u);
    /* Empty table. */
    mock_surface_clear();
    CHECK_EQ_INT(cadisplay_find_display_surface(), 0u);
    /* Unknown seed update fails cleanly. */
    CHECK_EQ_INT(mock_surface_set_seed(999, 1), -1);
}

static void test_set_present_fd(void) {
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    CHECK_EQ_INT(cadisplay_set_present_fd(2, 5), -1);
    CHECK_EQ_INT(cadisplay_set_present_fd(-1, 5), -1);
    CHECK_EQ_INT(cadisplay_set_present_fd(0, 42), 0);
    CHECK_EQ_INT(cadisplay_set_present_fd(1, -1), 0);
    /* present_last_info is part of the allowed mock set; exercise
     * it so the mock is not dead code. */
    mock_info.client_w = SMALL_W;
    mock_info.client_h = SMALL_H;
    CHECK_EQ_INT(present_last_info().client_w, SMALL_W);
    CHECK_EQ_INT(present_last_info().client_h, SMALL_H);
}

static void test_info_counters(void) {
    cadisplay_info_t info;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    CHECK(mock_surface_add(1, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK(mock_surface_add(2, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, 42) != NULL);
    CHECK_EQ_INT(cadisplay_set_present_fd(0, 42), 0);
    /* Copy path on buffer 0. */
    CHECK_EQ_INT(cadisplay_on_frame(1), 0);
    /* Zero-copy on buffer 0 is busy now, so surface 2 falls back
     * to the copy path on buffer 1. */
    CHECK_EQ_INT(cadisplay_on_frame(2), 0);
    CHECK_EQ_INT(mock_present_last_index, 1);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 2u);
    CHECK_EQ_INT(info.frames_copied, 2u);
    CHECK_EQ_INT(info.frames_zero_copy, 0u);
    /* Ack buffer 0, then surface 2 goes zero-copy on it. */
    mock_last_frame_id = 1;
    CHECK_EQ_INT(cadisplay_on_vsync(1000000000LL), 0);
    CHECK_EQ_INT(cadisplay_on_frame(2), 0);
    CHECK_EQ_INT(mock_present_last_index, 0);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 3u);
    CHECK_EQ_INT(info.frames_copied, 2u);
    CHECK_EQ_INT(info.frames_zero_copy, 1u);
    /* current_surface tracking. */
    CHECK_EQ_INT(cadisplay_current_surface(), 0u);
    cadisplay_set_current_surface(2);
    CHECK_EQ_INT(cadisplay_current_surface(), 2u);
    cadisplay_set_current_surface(0);
    CHECK_EQ_INT(cadisplay_current_surface(), 0u);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.current_surface_id, 0u);
}

static void test_both_in_flight(void) {
    cadisplay_info_t info;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    CHECK(mock_surface_add(1, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_on_frame(1), 0);
    CHECK_EQ_INT(mock_present_last_index, 0);
    CHECK_EQ_INT(cadisplay_on_frame(1), 0);
    CHECK_EQ_INT(mock_present_last_index, 1);
    /* Both buffers in flight: no present possible. */
    CHECK_EQ_INT(cadisplay_on_frame(1), -1);
    CHECK_EQ_INT(mock_present_count, 2);
    /* One vsync frees the oldest (index 0). */
    mock_last_frame_id = 1;
    CHECK_EQ_INT(cadisplay_on_vsync(2000000000LL), 0);
    CHECK_EQ_INT(cadisplay_on_frame(1), 0);
    CHECK_EQ_INT(mock_present_last_index, 0);
    /* present_present failing surfaces as -1. */
    mock_present_fail = 1;
    mock_last_frame_id = 2;
    CHECK_EQ_INT(cadisplay_on_vsync(2016666666LL), 0);
    CHECK_EQ_INT(cadisplay_on_frame(1), -1);
    mock_present_fail = 0;
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 3u);
}

static void test_surface_fd_seed(void) {
    int fd = -999;
    uint32_t seed = 0;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    CHECK(mock_surface_add(11, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 42, 77) != NULL);
    CHECK_EQ_INT(cadisplay_surface_fd(11, &fd), 0);
    CHECK_EQ_INT(fd, 77);
    CHECK_EQ_INT(cadisplay_surface_seed(11, &seed), 0);
    CHECK_EQ_INT(seed, 42u);
    CHECK_EQ_INT(cadisplay_surface_fd(999, &fd), -1);
    CHECK_EQ_INT(cadisplay_surface_seed(999, &seed), -1);
    CHECK_EQ_INT(cadisplay_surface_fd(11, NULL), -1);
    CHECK_EQ_INT(cadisplay_surface_seed(11, NULL), -1);
    /* The IOKit table (merged at integration): 6 services. */
    {
        uint32_t count = 99;
        const cadisplay_iokit_service_t *tab;
        tab = cadisplay_iokit_services(&count);
        CHECK(tab != NULL);
        CHECK_EQ_INT(count, 6u);
        CHECK(cadisplay_iokit_services(NULL) != NULL);
        CHECK(strcmp(tab[0].service_name, "IOMobileFramebuffer") == 0);
        CHECK(strcmp(tab[0].class_name, "IOMobileFramebuffer") == 0);
        CHECK(strcmp(tab[2].service_name, "IODisplay") == 0);
        CHECK(strcmp(tab[5].service_name, "AppleARMBacklight") == 0);
        CHECK(tab[0].properties != NULL);
        CHECK(tab[5].properties != NULL);
    }
}

static void test_real_geometry(void) {
    cadisplay_info_t info;
    uint8_t *backing;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(1080, 2400, 3), 0);
    mock_stride = 4320;
    /* The real iosurface-aligned bytes_per_row (4352) for a
     * 1080-wide surface. */
    backing = mock_surface_add(100, 1080, 2400, 4352,
                               IOSURFACE_PIXEL_FORMAT_BGRA, 0, 5, -1);
    CHECK(backing != NULL);
    /* Stride mismatch, so zero-copy is 0 and the copy path runs. */
    CHECK_EQ_INT(cadisplay_is_zero_copy(100), 0);
    CHECK_EQ_INT(cadisplay_find_display_surface(), 100u);
    CHECK_EQ_INT(cadisplay_on_frame(100), 0);
    CHECK_EQ_INT(mock_present_last_index, 0);
    CHECK_EQ_INT(cadisplay_info(&info), 0);
    CHECK_EQ_INT(info.frames_presented, 1u);
    CHECK_EQ_INT(info.frames_copied, 1u);
    CHECK_EQ_INT(info.frames_zero_copy, 0u);
    /* Row 0 and row 2399 copied with the right strides. */
    CHECK(memcmp(mock_fb0, backing, 4320) == 0);
    CHECK(memcmp(mock_fb0 + 2399u * 4320u,
                 backing + 2399u * 4352u, 4320) == 0);
}

/* Deterministic LCG, fixed seed, so the fuzz is reproducible. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 16;
}

static void test_fuzz(void) {
    int i;
    mock_reset_all();
    CHECK_EQ_INT(cadisplay_init(SMALL_W, SMALL_H, 1), 0);
    mock_stride = SMALL_STRIDE;
    CHECK(mock_surface_add(1, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, 42) != NULL);
    CHECK(mock_surface_add(2, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 1, 5, -1) != NULL);
    CHECK(mock_surface_add(3, SMALL_W, SMALL_H, SMALL_STRIDE,
                           IOSURFACE_PIXEL_FORMAT_420V, 0, 0, -1) != NULL);
    CHECK(mock_surface_add(4, SMALL_W, SMALL_H, 256,
                           IOSURFACE_PIXEL_FORMAT_BGRA, 0, 0, -1) != NULL);
    CHECK_EQ_INT(cadisplay_set_present_fd(0, 42), 0);
    for (i = 0; i < 100000; i++) {
        uint32_t sid = rng_next() % 200u;
        int64_t ts = (int64_t)(rng_next() % 1000000u) * 1000LL;
        int r1 = cadisplay_on_frame(sid);
        int r2 = cadisplay_is_zero_copy(sid);
        uint32_t f = cadisplay_find_display_surface();
        int r3 = cadisplay_on_vsync(ts);
        int r4 = cadisplay_set_present_fd((int)(rng_next() % 3u) - 1,
                                          (int)(rng_next() % 100u));
        int fd;
        int r5 = cadisplay_surface_fd(sid, &fd);
        uint32_t seed;
        int r6 = cadisplay_surface_seed(sid, &seed);
        CHECK(r1 == 0 || r1 == -1);
        CHECK(r2 == 0 || r2 == 1);
        CHECK(r3 == 0 || r3 == -1);
        CHECK(r4 == 0 || r4 == -1);
        CHECK(r5 == 0 || r5 == -1);
        CHECK(r6 == 0 || r6 == -1);
        if (f != 0) {
            iosurface_surface s;
            CHECK(iosurface_get_surface(f, &s) == KERN_SUCCESS);
            CHECK(s.width == SMALL_W && s.height == SMALL_H &&
                  s.pixel_format == IOSURFACE_PIXEL_FORMAT_BGRA);
        }
    }
}

int main(void) {
    test_init_shutdown();
    test_vsync();
    test_copy_path();
    test_zero_copy();
    test_unknown_locked();
    test_is_zero_copy();
    test_find_display_surface();
    test_set_present_fd();
    test_info_counters();
    test_both_in_flight();
    test_surface_fd_seed();
    test_real_geometry();
    test_fuzz();
    mock_surface_clear();
    printf("cadisplay_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
