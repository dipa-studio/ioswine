/*
 * cadamage_test.c - Host test for cadamage.{c,h}.
 *
 * Plain cc + ASan/UBSan. Deterministic (fixed-seed xorshift fuzz).
 * Prints "N checks, F failures" as the last line; exits nonzero
 * when any check fails.
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "cadamage.h"

static int g_checks;
static int g_failures;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

static uint32_t rng_state = 0x12345678u;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rect_within(const cadamage_rect_t *r, uint32_t w, uint32_t h) {
    if (r->w == 0 || r->h == 0) return 0;
    if (r->x >= w || r->y >= h) return 0;
    if (r->x + r->w > w || r->y + r->h > h) return 0;
    return 1;
}

static int point_covered(const cadamage_rect_t *rects, int n,
                         uint32_t x, uint32_t y) {
    int i;
    for (i = 0; i < n; i++) {
        if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
            y >= rects[i].y && y < rects[i].y + rects[i].h) {
            return 1;
        }
    }
    return 0;
}

static void test_basics(void) {
    cadamage_t d;
    cadamage_rect_t out[16];
    cadamage_rect_t b;
    int n;

    CHECK(cadamage_init(NULL, 100, 100) == -1);
    CHECK(cadamage_init(&d, 0, 100) == -1);
    CHECK(cadamage_init(&d, 100, 0) == -1);
    CHECK(cadamage_init(&d, 1080, 2400) == 0);
    CHECK(cadamage_empty(&d) == 1);
    CHECK(cadamage_is_full(&d) == 0);
    CHECK(cadamage_get(&d, out, 16) == 0);
    CHECK(cadamage_bounding(&d, &b) == -1);

    /* Zero-area and fully-outside rects are ignored. */
    cadamage_add(&d, 10, 10, 0, 50);
    cadamage_add(&d, 10, 10, 50, 0);
    cadamage_add(&d, 2000, 10, 50, 50);
    cadamage_add(&d, 10, 3000, 50, 50);
    CHECK(cadamage_empty(&d) == 1);

    /* Clamping to the frame. */
    cadamage_add(&d, 1000, 2300, 500, 500);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 1);
    CHECK(out[0].x == 1000 && out[0].y == 2300);
    CHECK(out[0].w == 80 && out[0].h == 100);
    CHECK(rect_within(&out[0], 1080, 2400) == 1);
    cadamage_reset(&d);
    CHECK(cadamage_empty(&d) == 1);

    /* Full frame. */
    cadamage_add_full(&d);
    CHECK(cadamage_is_full(&d) == 1);
    CHECK(cadamage_empty(&d) == 0);
    CHECK(cadamage_get(&d, out, 16) == -1);
    CHECK(cadamage_bounding(&d, &b) == 0);
    CHECK(b.x == 0 && b.y == 0 && b.w == 1080 && b.h == 2400);
    cadamage_reset(&d);

    /* Points conversion with scale 3.0, rounded outward. */
    cadamage_add_points(&d, 10.0, 20.0, 100.0, 50.0, 3.0);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 1);
    CHECK(out[0].x == 30 && out[0].y == 60);
    CHECK(out[0].w == 300 && out[0].h == 150);
    cadamage_reset(&d);
    cadamage_add_points(&d, 10.2, 20.7, 5.1, 5.1, 3.0);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 1);
    /* x: floor(30.6)=30, far edge ceil(46.0)=46 -> w=16. */
    CHECK(out[0].x == 30 && out[0].w == 16);
    /* y: floor(62.1)=62, far edge ceil(77.4)=78 -> h=16. */
    CHECK(out[0].y == 62 && out[0].h == 16);
    cadamage_reset(&d);
    cadamage_add_points(&d, 0.0, 0.0, 10.0, 10.0, 0.0);
    CHECK(cadamage_empty(&d) == 1);
    cadamage_add_points(&d, 0.0, 0.0, 10.0, 10.0, -1.0);
    CHECK(cadamage_empty(&d) == 1);
}

static void test_merge(void) {
    cadamage_t d;
    cadamage_rect_t out[16];
    int n;

    CHECK(cadamage_init(&d, 1000, 1000) == 0);

    /* Overlapping rects merge into one. */
    cadamage_add(&d, 0, 0, 100, 100);
    cadamage_add(&d, 50, 50, 100, 100);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 1);
    CHECK(out[0].x == 0 && out[0].y == 0 && out[0].w == 150 && out[0].h == 150);
    cadamage_reset(&d);

    /* Edge-touching rects merge. */
    cadamage_add(&d, 0, 0, 100, 100);
    cadamage_add(&d, 100, 0, 100, 100);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 1);
    CHECK(out[0].w == 200 && out[0].h == 100);
    cadamage_reset(&d);

    /* Disjoint rects stay separate. */
    cadamage_add(&d, 0, 0, 100, 100);
    cadamage_add(&d, 300, 300, 100, 100);
    cadamage_add(&d, 600, 0, 50, 50);
    n = cadamage_get(&d, out, 16);
    CHECK(n == 3);
    cadamage_reset(&d);

    /* Bounding rect covers all. */
    {
        cadamage_rect_t b;
        cadamage_add(&d, 10, 20, 30, 40);
        cadamage_add(&d, 500, 600, 70, 80);
        CHECK(cadamage_bounding(&d, &b) == 0);
        CHECK(b.x == 10 && b.y == 20 && b.w == 560 && b.h == 660);
    }
    cadamage_reset(&d);

    /* 17 disjoint rects overflow the 16-slot list: full frame. */
    {
        int i;
        for (i = 0; i < 17; i++) {
            cadamage_add(&d, (uint32_t)(i * 50), 0, 10, 10);
        }
        CHECK(cadamage_is_full(&d) == 1);
        CHECK(cadamage_get(&d, out, 16) == -1);
    }
    cadamage_reset(&d);

    /* get with a smaller output buffer returns at most max_out. */
    cadamage_add(&d, 0, 0, 10, 10);
    cadamage_add(&d, 100, 0, 10, 10);
    cadamage_add(&d, 200, 0, 10, 10);
    n = cadamage_get(&d, out, 2);
    CHECK(n == 2);
}

static void test_fuzz(void) {
    int iter;
    for (iter = 0; iter < 10000; iter++) {
        cadamage_t d;
        cadamage_rect_t out[16];
        cadamage_rect_t added[32];
        int n_added = 0;
        int k;
        int n;
        uint32_t fw = 1080;
        uint32_t fh = 2400;
        CHECK(cadamage_init(&d, fw, fh) == 0);
        for (k = 0; k < 5; k++) {
            uint32_t x = rng_next() % 1200;
            uint32_t y = rng_next() % 2600;
            uint32_t w = (rng_next() % 400) + 1;
            uint32_t h = (rng_next() % 400) + 1;
            added[n_added].x = x;
            added[n_added].y = y;
            added[n_added].w = w;
            added[n_added].h = h;
            n_added++;
            cadamage_add(&d, x, y, w, h);
        }
        if (cadamage_is_full(&d)) {
            CHECK(cadamage_get(&d, out, 16) == -1);
            continue;
        }
        n = cadamage_get(&d, out, 16);
        CHECK(n >= 1 && n <= 16);
        for (k = 0; k < n; k++) {
            CHECK(rect_within(&out[k], fw, fh) == 1);
        }
        /* Every added rect (clamped to the frame) must be covered
         * by the union of the returned rects: sample its corners
         * and centre. */
        for (k = 0; k < n_added; k++) {
            uint32_t cx = added[k].x;
            uint32_t cy = added[k].y;
            uint32_t cw = added[k].w;
            uint32_t ch = added[k].h;
            uint32_t x2;
            uint32_t y2;
            if (cx >= fw || cy >= fh) continue;
            if (cx + cw > fw) cw = fw - cx;
            if (cy + ch > fh) ch = fh - cy;
            if (cw == 0 || ch == 0) continue;
            x2 = cx + cw - 1;
            y2 = cy + ch - 1;
            CHECK(point_covered(out, n, cx, cy) == 1);
            CHECK(point_covered(out, n, x2, cy) == 1);
            CHECK(point_covered(out, n, cx, y2) == 1);
            CHECK(point_covered(out, n, x2, y2) == 1);
            CHECK(point_covered(out, n, cx + cw / 2, cy + ch / 2) == 1);
        }
    }
}

int main(void) {
    test_basics();
    test_merge();
    test_fuzz();
    printf("cadamage_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
