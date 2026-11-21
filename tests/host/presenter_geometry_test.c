/*
 * Host unit test for the 019 presenter geometry fixes (protocol.c).
 * Plain `cc`, no NDK, no Android headers:
 *   cc -std=c11 -Wall -Wextra -I <tree>/android/surface/src/common \
 *      presenter_geometry_test.c <tree>/android/surface/src/common/protocol.c \
 *      -o presenter_geometry_test && ./presenter_geometry_test
 *
 * Covers: isws_compute_mapping double-precision exact fit (1440x3200
 * client on a 1440x3200 screen must have scale 1.0 and offsets
 * exactly 0), isws_dest_rect (single integer rect for blit and bars,
 * symmetric letterbox within 1 px, never outside the screen),
 * isws_nn_index (exact identity at 1:1, exact 4x upscale, clamped),
 * isws_bgra_to_rgba_opaque (opaque identity, transparent to white,
 * premultiplied half-alpha blend), screen/client round-trip, and a
 * 10000-iteration fuzz over random screen/client sizes asserting the
 * dest-rect invariants.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "protocol.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_FLOAT_EQ(a, b) CHECK(fabsf((a) - (b)) < 1e-4f)

static void test_exact_fit(void) {
    isws_info_t m;
    int32_t x0, y0, x1, y1;

    /* The 019 target: 1440x3200 frame on a 1440x3200 window. */
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 0.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);

    /* Old geometry: 1080x2400 frame on a 1440x3200 window fills it. */
    CHECK(isws_compute_mapping(1440, 3200, 1080, 2400, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1440.0f / 1080.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 0.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);

    /* The observed broken frame: 360x800 on a 1440x3200 window. */
    CHECK(isws_compute_mapping(1440, 3200, 360, 800, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 4.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 0.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);

    /* Odd size: 720x1280 on a 1440x3200 window, scale by width. */
    CHECK(isws_compute_mapping(1440, 3200, 720, 1280, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 2.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 320.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 320 && x1 == 1440 && y1 == 2880);

    /* Rotation is recorded, not applied: same mapping for 90/180/270. */
    CHECK(isws_compute_mapping(1440, 3200, 720, 1280, 90, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 2.0f);
    CHECK_FLOAT_EQ(m.offset_y, 320.0f);
    CHECK(m.rotation == 90u);
    CHECK(isws_compute_mapping(1440, 3200, 720, 1280, 270, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 2.0f);

    /* Rejections. */
    CHECK(isws_compute_mapping(0, 3200, 1440, 3200, 0, &m) != 0);
    CHECK(isws_compute_mapping(1440, 3200, 1440, 0, 0, &m) != 0);
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 45, &m) != 0);
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, NULL) != 0);
}

static void test_letterbox_symmetry(void) {
    isws_info_t m;
    int32_t x0, y0, x1, y1;
    int32_t top, bottom, left, right;

    /* iPhone-ish 1170x2532 client on a 1440x3200 screen. */
    CHECK(isws_compute_mapping(1440, 3200, 1170, 2532, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1440.0f / 1170.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK(m.offset_y > 0.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && x1 == 1440);
    CHECK(y0 == 42 && y1 == 3158);
    top = y0;
    bottom = 3200 - y1;
    CHECK(top == 42 && bottom == 42);
    CHECK(top - bottom == -1 || bottom - top == 1 || top == bottom);

    /* Wide client on a tall screen: bars top/bottom only. */
    CHECK(isws_compute_mapping(1080, 2400, 2000, 1000, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1080.0f / 2000.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 930.0f);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 930 && x1 == 1080 && y1 == 1470);

    /* Tall client on a wide screen: bars left/right only, symmetric. */
    CHECK(isws_compute_mapping(2400, 1080, 800, 2000, 0, &m) == 0);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(y0 == 0 && y1 == 1080);
    left = x0;
    right = 2400 - x1;
    CHECK(left >= 0 && right >= 0);
    CHECK(left - right <= 1 && right - left <= 1);

    /* NULL map and NULL outputs are safe. */
    isws_dest_rect(NULL, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0);
    isws_dest_rect(&m, NULL, NULL, NULL, NULL);
}

static void test_nn_index(void) {
    int32_t i;
    /* 1:1 identity over a whole row. */
    for (i = 0; i < 100; i++) {
        CHECK(isws_nn_index(i, 0, 100, 0, 100) == i);
    }
    /* 4x upscale: each source pixel covers exactly 4 dest pixels. */
    CHECK(isws_nn_index(0, 0, 1440, 0, 360) == 0);
    CHECK(isws_nn_index(3, 0, 1440, 0, 360) == 0);
    CHECK(isws_nn_index(4, 0, 1440, 0, 360) == 1);
    CHECK(isws_nn_index(1439, 0, 1440, 0, 360) == 359);
    /* 2x downscale. */
    CHECK(isws_nn_index(0, 0, 540, 0, 1080) == 0);
    CHECK(isws_nn_index(1, 0, 540, 0, 1080) == 2);
    CHECK(isws_nn_index(539, 0, 540, 0, 1080) == 1078);
    /* Offset ranges (damage rect sub-span). */
    CHECK(isws_nn_index(10, 10, 20, 50, 10) == 50);
    CHECK(isws_nn_index(29, 10, 20, 50, 10) == 59);
    /* Clamping and degenerate lengths. */
    CHECK(isws_nn_index(-5, 0, 100, 0, 100) == 0);
    CHECK(isws_nn_index(500, 0, 100, 0, 100) == 99);
    CHECK(isws_nn_index(0, 0, 0, 7, 100) == 7);
    CHECK(isws_nn_index(0, 0, 100, 7, 0) == 7);
}

static void test_bgra_to_rgba(void) {
    /* Opaque pixel: plain swizzle, bit-identical to the old code. */
    CHECK(isws_bgra_to_rgba_opaque(0xff112233u) == 0xff332211u);
    CHECK(isws_bgra_to_rgba_opaque(0xffffffffu) == 0xffffffffu);
    CHECK(isws_bgra_to_rgba_opaque(0xff000000u) == 0xff000000u);
    /* Transparent pixel: white, not black. */
    CHECK(isws_bgra_to_rgba_opaque(0x00000000u) == 0xffffffffu);
    /* Half alpha, premultiplied: BGRA (B=0x20,G=0x40,R=0x80,A=0x80)
     * over white gives R=255, G=191, B=159, word 0xff9fbfff. */
    CHECK(isws_bgra_to_rgba_opaque(0x80804020u) == 0xff9fbfffu);
    /* Non-premultiplied input is clamped, never wraps. */
    CHECK(isws_bgra_to_rgba_opaque(0x00102030u) == 0xffffffffu);
    /* Low alpha dark pixel becomes nearly white (254, not 255). */
    CHECK(isws_bgra_to_rgba_opaque(0x01000000u) == 0xfffefefeu);
}

static void test_round_trip(void) {
    isws_info_t m;
    float sx, sy, cx, cy;
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &m) == 0);
    /* Corners and centre map identically at 1:1. */
    isws_client_to_screen(&m, 0.0f, 0.0f, &sx, &sy);
    CHECK_FLOAT_EQ(sx, 0.0f);
    CHECK_FLOAT_EQ(sy, 0.0f);
    CHECK(isws_screen_to_client(&m, sx, sy, &cx, &cy) == 0);
    CHECK_FLOAT_EQ(cx, 0.0f);
    CHECK_FLOAT_EQ(cy, 0.0f);
    isws_client_to_screen(&m, 1439.0f, 3199.0f, &sx, &sy);
    CHECK(isws_screen_to_client(&m, sx, sy, &cx, &cy) == 0);
    CHECK(fabsf(cx - 1439.0f) < 1.0f && fabsf(cy - 3199.0f) < 1.0f);
    isws_client_to_screen(&m, 720.0f, 1600.0f, &sx, &sy);
    CHECK_FLOAT_EQ(sx, 720.0f);
    CHECK_FLOAT_EQ(sy, 1600.0f);
}

/* Deterministic LCG fuzz: dest-rect invariants over random sizes. */
static uint32_t g_rng = 0x12345678u;
static uint32_t next_rand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static void test_effective_mapping(void) {
    isws_info_t stale, eff;
    int32_t x0, y0, x1, y1;

    /* The 020 device case: the mapping was computed when the window
     * was 1080x2400 (client 1080x2400, scale 1.0, offsets 0), but
     * the locked window buffer is 1440x3200. Using the stale map
     * as-is paints only [0,1080) x [0,2400): a 360 px right margin
     * and an 800 px bottom bar stay black. The effective mapping
     * must recompute from the buffer size so the dest rect is the
     * full 1440x3200. */
    CHECK(isws_compute_mapping(1080, 2400, 1080, 2400, 0, &stale) == 0);
    isws_dest_rect(&stale, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1080 && y1 == 2400);
    CHECK(isws_effective_mapping(&stale, 1440, 3200, &eff) == 0);
    CHECK(eff.screen_w == 1440 && eff.screen_h == 3200);
    CHECK(eff.client_w == 1080 && eff.client_h == 2400);
    CHECK_FLOAT_EQ(eff.scale, 1440.0f / 1080.0f);
    CHECK_FLOAT_EQ(eff.offset_x, 0.0f);
    CHECK_FLOAT_EQ(eff.offset_y, 0.0f);
    isws_dest_rect(&eff, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);

    /* Same stale window, but the client already converged to
     * 1440x3200: the effective mapping is 1:1, offsets exactly 0. */
    CHECK(isws_compute_mapping(1080, 2400, 1440, 3200, 0, &stale) == 0);
    CHECK_FLOAT_EQ(stale.scale, 0.75f);
    CHECK(isws_effective_mapping(&stale, 1440, 3200, &eff) == 0);
    CHECK_FLOAT_EQ(eff.scale, 1.0f);
    CHECK_FLOAT_EQ(eff.offset_x, 0.0f);
    CHECK_FLOAT_EQ(eff.offset_y, 0.0f);
    isws_dest_rect(&eff, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);

    /* Same-size buffer: the mapping is returned unchanged. */
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &stale) == 0);
    CHECK(isws_effective_mapping(&stale, 1440, 3200, &eff) == 0);
    CHECK(eff.screen_w == 1440 && eff.screen_h == 3200);
    CHECK_FLOAT_EQ(eff.scale, 1.0f);
    CHECK_FLOAT_EQ(eff.offset_x, 0.0f);
    CHECK_FLOAT_EQ(eff.offset_y, 0.0f);

    /* Window shrank: recompute downscales, dest still fills it. */
    CHECK(isws_effective_mapping(&stale, 1080, 2400, &eff) == 0);
    CHECK_FLOAT_EQ(eff.scale, 0.75f);
    isws_dest_rect(&eff, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1080 && y1 == 2400);

    /* Rotation is preserved through the recompute. */
    CHECK(isws_compute_mapping(1440, 3200, 720, 1280, 90, &stale) == 0);
    CHECK(isws_effective_mapping(&stale, 1080, 2400, &eff) == 0);
    CHECK(eff.rotation == 90u);
    CHECK_FLOAT_EQ(eff.scale, 1.5f);

    /* Invalid inputs fail and leave the output untouched. */
    eff.screen_w = 777;
    CHECK(isws_effective_mapping(NULL, 1440, 3200, &eff) == -1);
    CHECK(eff.screen_w == 777);
    CHECK(isws_effective_mapping(&stale, 0, 3200, &eff) == -1);
    CHECK(isws_effective_mapping(&stale, 1440, 0, &eff) == -1);
    CHECK(isws_effective_mapping(&stale, 1440, 3200, NULL) == -1);

    /* Fuzz: a recomputed mapping never paints outside the buffer,
     * and when buffer and client aspects match it fills exactly. */
    {
        int iter;
        for (iter = 0; iter < 2000; iter++) {
            uint32_t sw = 1 + next_rand() % 4000;
            uint32_t sh = 1 + next_rand() % 4000;
            uint32_t cw = 1 + next_rand() % 4000;
            uint32_t ch = 1 + next_rand() % 4000;
            uint32_t bw = 1 + next_rand() % 4000;
            uint32_t bh = 1 + next_rand() % 4000;
            CHECK(isws_compute_mapping(sw, sh, cw, ch, 0, &stale) == 0);
            CHECK(isws_effective_mapping(&stale, bw, bh, &eff) == 0);
            CHECK(eff.screen_w == bw && eff.screen_h == bh);
            CHECK(eff.client_w == cw && eff.client_h == ch);
            isws_dest_rect(&eff, &x0, &y0, &x1, &y1);
            CHECK(x0 >= 0 && y0 >= 0);
            CHECK(x1 <= (int32_t)bw && y1 <= (int32_t)bh);
            CHECK(x1 > x0 && y1 > y0);
            if ((uint64_t)bw * ch == (uint64_t)bh * cw) {
                CHECK(x0 == 0 && y0 == 0 && x1 == (int32_t)bw &&
                      y1 == (int32_t)bh);
            }
        }
    }
}

static void test_fuzz(void) {
    static const uint32_t rotations[4] = { 0, 90, 180, 270 };
    int iter;
    for (iter = 0; iter < 10000; iter++) {
        uint32_t sw = 1 + next_rand() % 4000;
        uint32_t sh = 1 + next_rand() % 4000;
        uint32_t cw = 1 + next_rand() % 4000;
        uint32_t ch = 1 + next_rand() % 4000;
        uint32_t rot = rotations[next_rand() % 4];
        isws_info_t m;
        int32_t x0, y0, x1, y1;
        int64_t dw, dh;
        CHECK(isws_compute_mapping(sw, sh, cw, ch, rot, &m) == 0);
        CHECK(m.scale > 0.0f);
        CHECK(m.offset_x >= 0.0f && m.offset_y >= 0.0f);
        isws_dest_rect(&m, &x0, &y0, &x1, &y1);
        CHECK(x0 >= 0 && y0 >= 0);
        CHECK(x1 <= (int32_t)sw && y1 <= (int32_t)sh);
        CHECK(x1 > x0 && y1 > y0);
        dw = (int64_t)x1 - x0;
        dh = (int64_t)y1 - y0;
        /* Dest size is client * scale rounded, within 1 px of the
         * float computation, and never larger than the screen. */
        CHECK(dw <= (int64_t)sw && dh <= (int64_t)sh);
        CHECK(dw >= (int64_t)((double)cw * (double)m.scale) - 1);
        CHECK(dh >= (int64_t)((double)ch * (double)m.scale) - 1);
        /* Letterbox bars symmetric within 2 px (1 px rounding each
         * side, plus at most 1 px float/double rounding disagreement). */
        CHECK((int64_t)y0 - ((int64_t)sh - y1) <= 2 &&
              ((int64_t)sh - y1) - (int64_t)y0 <= 2);
        CHECK((int64_t)x0 - ((int64_t)sw - x1) <= 2 &&
              ((int64_t)sw - x1) - (int64_t)x0 <= 2);
        /* Equal aspects fill the screen exactly. */
        if ((uint64_t)sw * ch == (uint64_t)sh * cw) {
            CHECK(x0 == 0 && y0 == 0 && x1 == (int32_t)sw && y1 == (int32_t)sh);
        }
        /* Nearest-neighbour index stays in range across the rect.
         * The last dest pixel samples the last source pixel when the
         * dest is at least as wide as the source (upscale / 1:1);
         * when downscaling it samples the last *sampled* source
         * pixel, which may be up to cw/dw short of the last one. */
        CHECK(isws_nn_index(x0, x0, (int32_t)dw, 0, (int32_t)cw) == 0);
        {
            int32_t lastx = isws_nn_index(x1 - 1, x0, (int32_t)dw, 0, (int32_t)cw);
            int32_t lasty = isws_nn_index(y1 - 1, y0, (int32_t)dh, 0, (int32_t)ch);
            CHECK(lastx >= 0 && lastx <= (int32_t)cw - 1);
            CHECK(lasty >= 0 && lasty <= (int32_t)ch - 1);
            if (dw >= (int64_t)cw) CHECK(lastx == (int32_t)cw - 1);
            if (dh >= (int64_t)ch) CHECK(lasty == (int32_t)ch - 1);
            if (dw < (int64_t)cw) {
                CHECK(lastx >= (int32_t)cw - 1 - (int32_t)((int64_t)cw / dw) - 1);
            }
            if (dh < (int64_t)ch) {
                CHECK(lasty >= (int32_t)ch - 1 - (int32_t)((int64_t)ch / dh) - 1);
            }
        }
    }
}

int main(void) {
    test_exact_fit();
    test_letterbox_symmetry();
    test_nn_index();
    test_bgra_to_rgba();
    test_round_trip();
    test_effective_mapping();
    test_fuzz();
    printf("presenter_geometry_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
