/*
 * fuzz_touch_damage_utf8_test.c - Fuzz loops for touch, damage, and UTF-8.
 *
 * Drives the REAL protocol.c and the REAL touch.c (TOUCH_HOST_TEST,
 * touch_map_coords only; no ObjC symbols needed) with deterministic
 * pseudo-random inputs plus explicit edge tables:
 *   1. Touch coordinates: 20000 fuzz iterations over raw float bit
 *      patterns (NaN, Inf, -0, huge, subnormal), client sizes (0, 1,
 *      huge) and window sizes. touch_map_coords must never crash and
 *      must return -1 for NaN/Inf/zero-size input and clamp valid
 *      input into [0,1] / [0,window].
 *   2. Damage rects: 20000 fuzz iterations over isws_present_t with
 *      damage_count 0..20 and wild rects. isws_encode_present must
 *      refuse damage_count > 16; isws_decode_present must refuse bad
 *      buffer index / count / length; isws_expand_damage must return
 *      at most 16 rects, all inside the frame.
 *   3. Key messages: 20000 fuzz iterations over isws_key_t.
 *      isws_decode_key rejects action > 1; keycode is NOT validated
 *      by the decoder (documented here: unknown keycodes pass the
 *      wire check and must be filtered by the consumer).
 *   4. UTF-8 text input (harness level): the v1 protocol has NO text
 *      payload (isws_key_t carries a keycode only), so keyboard text
 *      needs a new message type. This section validates the
 *      reference UTF-8 checker a text payload must satisfy: it
 *      accepts valid 1-4 byte sequences (including Hungarian
 *      accented letters), rejects overlong forms, surrogates,
 *      code points above U+10FFFF, truncated sequences, NUL bytes,
 *      and inputs longer than 4096 bytes. 20000 fuzz iterations over
 *      random byte strings plus an explicit edge table. The checker
 *      is test-local; the wire format must match its rules.
 *
 * All fuzzing is deterministic (fixed LCG seed) so failures reproduce.
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "touch.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Deterministic LCG (same sequence on every run and platform). */
static uint64_t g_rng = 0x123456789abcdefULL;
static uint64_t rng_next(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_rng >> 11;
}
static uint32_t rng_u32(void) { return (uint32_t)rng_next(); }
static float rng_f32_bits(void) {
    uint32_t b = rng_u32();
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

/* ------------------------------------------------------------------ */
/* 1. Touch coordinate fuzz (real touch_map_coords)                    */
/* ------------------------------------------------------------------ */

static int finite_f32(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return ((u >> 23) & 0xffu) != 0xffu;
}

static void test_touch_edge_table(void) {
    double nx, ny, wx, wy;
    isws_touch_t t;
    /* Exact edges of a 720x1600 client on a 360x800 pt window. */
    t.action = ISWS_TOUCH_DOWN; t.pointer_id = 0; t.pointer_count = 1;
    t.x = 0.0f; t.y = 0.0f; t.timestamp_ns = 0;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK(nx == 0.0 && ny == 0.0 && wx == 0.0 && wy == 0.0);
    t.x = 720.0f; t.y = 1600.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK(nx == 1.0 && ny == 1.0 && wx == 360.0 && wy == 800.0);
    /* Negative and beyond-screen clamp into range. */
    t.x = -100.0f; t.y = 5000.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK(nx == 0.0 && ny == 1.0);
    /* INT32 extremes as floats clamp, they do not overflow. */
    t.x = 2147483647.0f; t.y = -2147483648.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK(nx == 1.0 && ny == 0.0);
    /* NaN / Inf are rejected. */
    t.x = NAN; t.y = 100.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    t.x = 100.0f; t.y = INFINITY;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    t.x = -INFINITY; t.y = 100.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    /* Zero / negative sizes are rejected. */
    t.x = 100.0f; t.y = 100.0f;
    CHECK(touch_map_coords(&t, 0, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 720, 0, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 720, 1600, 0.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, -800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 720, 1600, NAN, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(NULL, 720, 1600, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    /* NULL outputs are allowed: the return code is the contract. */
    t.x = 360.0f; t.y = 800.0f;
    CHECK(touch_map_coords(&t, 720, 1600, 360.0, 800.0, NULL, NULL, NULL, NULL) == 0);
}

static void test_touch_fuzz(void) {
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 20000; i++) {
        isws_touch_t t;
        t.action = ISWS_TOUCH_DOWN;
        t.pointer_id = 0;
        t.pointer_count = 1;
        t.x = rng_f32_bits();
        t.y = rng_f32_bits();
        t.timestamp_ns = 0;
        uint32_t cw = rng_u32() % 8193;
        uint32_t ch = rng_u32() % 8193;
        double win_w = (double)(rng_u32() % 16385);
        double win_h = (double)(rng_u32() % 16385);
        /* Sprinkle in exact edge values. */
        if ((i % 97) == 0) { t.x = 0.0f; t.y = 0.0f; }
        if ((i % 89) == 0) { cw = 0; }
        if ((i % 83) == 0) { win_w = 0.0; }
        double nx = -99.0, ny = -99.0, wx = -99.0, wy = -99.0;
        int rc = touch_map_coords(&t, cw, ch, win_w, win_h, &nx, &ny, &wx, &wy);
        int expect_bad = !finite_f32(t.x) || !finite_f32(t.y) ||
                         cw == 0 || ch == 0 ||
                         !isfinite(win_w) || !isfinite(win_h) ||
                         win_w <= 0.0 || win_h <= 0.0;
        if (expect_bad) {
            rejected++;
            CHECK(rc == -1);
        } else {
            accepted++;
            CHECK(rc == 0);
            CHECK(nx >= 0.0 && nx <= 1.0);
            CHECK(ny >= 0.0 && ny <= 1.0);
            CHECK(wx >= 0.0 && wx <= win_w);
            CHECK(wy >= 0.0 && wy <= win_h);
        }
    }
    CHECK(accepted > 10000);
    CHECK(rejected > 100);
    printf("touch fuzz: 20000 iterations, %d accepted, %d rejected\n", accepted, rejected);
}

/* ------------------------------------------------------------------ */
/* 2. Damage rect fuzz (real protocol.c)                               */
/* ------------------------------------------------------------------ */

static void test_damage_fuzz(void) {
    int encode_refused = 0, decode_refused = 0, expanded = 0;
    for (int i = 0; i < 20000; i++) {
        isws_present_t p;
        p.frame_id = rng_next();
        p.buffer_index = rng_u32() % 4;
        p.damage_count = rng_u32() % 21;
        for (uint32_t j = 0; j < ISWS_MAX_DAMAGE; j++) {
            p.damage[j].x = (int32_t)(rng_u32() % 2000) - 500;
            p.damage[j].y = (int32_t)(rng_u32() % 4000) - 500;
            p.damage[j].w = rng_u32() % 2000;
            p.damage[j].h = rng_u32() % 2000;
        }
        uint8_t buf[16 + 16 * ISWS_MAX_DAMAGE];
        size_t n = isws_encode_present(buf, sizeof(buf), &p);
        if (p.damage_count > ISWS_MAX_DAMAGE) {
            encode_refused++;
            CHECK(n == 0);
            continue;
        }
        CHECK(n == 16u + 16u * p.damage_count);
        isws_present_t out;
        /* Truncated length must be refused. */
        if (p.damage_count > 0) {
            CHECK(isws_decode_present(buf, n - 1, &out) == -1);
        }
        int rc = isws_decode_present(buf, n, &out);
        if (p.buffer_index >= ISWS_NUM_BUFFERS) {
            decode_refused++;
            CHECK(rc == -1);
        } else {
            CHECK(rc == 0);
            CHECK(out.damage_count == p.damage_count);
            isws_rect_t expanded_rects[ISWS_MAX_DAMAGE];
            size_t ne = isws_expand_damage(&out, 720, 1600, expanded_rects);
            CHECK(ne <= ISWS_MAX_DAMAGE);
            for (size_t k = 0; k < ne; k++) {
                CHECK(expanded_rects[k].x >= 0 && expanded_rects[k].y >= 0);
                CHECK((uint32_t)expanded_rects[k].x + expanded_rects[k].w <= 720);
                CHECK((uint32_t)expanded_rects[k].y + expanded_rects[k].h <= 1600);
                CHECK(expanded_rects[k].w > 0 && expanded_rects[k].h > 0);
            }
            expanded++;
        }
    }
    CHECK(encode_refused > 100);
    CHECK(decode_refused > 100);
    CHECK(expanded > 1000);
    printf("damage fuzz: 20000 iterations, %d encode-refused, %d decode-refused, %d expanded\n",
           encode_refused, decode_refused, expanded);
}

static void test_damage_edge_table(void) {
    isws_present_t p;
    isws_rect_t out[ISWS_MAX_DAMAGE];
    /* damage_count 0 means the whole frame. */
    p.frame_id = 0; p.buffer_index = 0; p.damage_count = 0;
    size_t ne = isws_expand_damage(&p, 720, 1600, out);
    CHECK(ne == 1);
    CHECK(out[0].x == 0 && out[0].y == 0 && out[0].w == 720 && out[0].h == 1600);
    /* A fully outside rect expands to nothing. */
    p.damage_count = 1;
    p.damage[0].x = 900; p.damage[0].y = 0; p.damage[0].w = 10; p.damage[0].h = 10;
    CHECK(isws_expand_damage(&p, 720, 1600, out) == 0);
    /* Negative origin clips. */
    p.damage[0].x = -10; p.damage[0].y = -10; p.damage[0].w = 30; p.damage[0].h = 30;
    CHECK(isws_expand_damage(&p, 720, 1600, out) == 1);
    CHECK(out[0].x == 0 && out[0].y == 0 && out[0].w == 20 && out[0].h == 20);
    /* INT32 extremes do not overflow the clip math. */
    p.damage[0].x = -2147483647; p.damage[0].y = -2147483647;
    p.damage[0].w = 0xffffffffu; p.damage[0].h = 0xffffffffu;
    CHECK(isws_expand_damage(&p, 720, 1600, out) == 1);
    CHECK(out[0].x == 0 && out[0].y == 0 && out[0].w == 720 && out[0].h == 1600);
    /* isws_clip_damage rejects an empty rect. */
    isws_rect_t r = { .x = 10, .y = 10, .w = 0, .h = 5 };
    CHECK(isws_clip_damage(&r, 720, 1600, &out[0]) == -1);
}

/* ------------------------------------------------------------------ */
/* 3. Key message fuzz (real protocol.c)                               */
/* ------------------------------------------------------------------ */

static void test_key_fuzz(void) {
    int roundtripped = 0, action_refused = 0, len_refused = 0;
    for (int i = 0; i < 20000; i++) {
        isws_key_t k;
        k.keycode = rng_u32();
        k.action = rng_u32() % 4;
        k.timestamp_ns = (int64_t)rng_next();
        uint8_t buf[ISWS_KEY_PAYLOAD_SIZE];
        CHECK(isws_encode_key(buf, sizeof(buf), &k) == ISWS_KEY_PAYLOAD_SIZE);
        isws_key_t out;
        if (k.action > ISWS_KEY_ACTION_UP) {
            action_refused++;
            CHECK(isws_decode_key(buf, sizeof(buf), &out) == -1);
            continue;
        }
        CHECK(isws_decode_key(buf, sizeof(buf), &out) == 0);
        CHECK(out.keycode == k.keycode);
        CHECK(out.action == k.action);
        CHECK(out.timestamp_ns == k.timestamp_ns);
        roundtripped++;
        /* Short and long lengths are refused. */
        CHECK(isws_decode_key(buf, sizeof(buf) - 1, &out) == -1);
        len_refused++;
    }
    CHECK(roundtripped > 5000);
    CHECK(action_refused > 1000);
    CHECK(len_refused == roundtripped);
    /* Documented: the decoder does NOT validate keycode. An unknown
     * keycode round-trips fine; the consumer must filter it. */
    isws_key_t k = { .keycode = 0xDEADBEEFu, .action = ISWS_KEY_ACTION_DOWN, .timestamp_ns = 0 };
    uint8_t buf[ISWS_KEY_PAYLOAD_SIZE];
    isws_encode_key(buf, sizeof(buf), &k);
    isws_key_t out;
    CHECK(isws_decode_key(buf, sizeof(buf), &out) == 0);
    CHECK(out.keycode == 0xDEADBEEFu);
    /* Only ISWS_KEY_BACK is defined in v1; BACK down/up round-trips. */
    k.keycode = ISWS_KEY_BACK;
    isws_encode_key(buf, sizeof(buf), &k);
    CHECK(isws_decode_key(buf, sizeof(buf), &out) == 0);
    CHECK(out.keycode == ISWS_KEY_BACK);
    printf("key fuzz: 20000 iterations, %d roundtripped, %d action-refused\n",
           roundtripped, action_refused);
}

/* ------------------------------------------------------------------ */
/* 4. UTF-8 reference checker (harness level, test-local)             */
/*                                                                     */
/* Rules a future text payload must satisfy. Returns 0 for valid      */
/* UTF-8, -1 otherwise. Rejects: overlong forms, surrogates, code     */
/* points above U+10FFFF, truncated sequences, NUL bytes, control     */
/* characters other than \n and \t, inputs longer than 4096 bytes.    */
/* ------------------------------------------------------------------ */

static int utf8_valid_text(const uint8_t *s, size_t len) {
    if (len > 4096) return -1;
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c == 0) return -1;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\t') return -1;
            if (c == 0x7F) return -1;
            i++;
            continue;
        }
        uint32_t cp = 0;
        size_t extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; if (cp < 2) return -1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; if (cp > 4) return -1; }
        else return -1;
        if (i + extra >= len) return -1;
        for (size_t j = 1; j <= extra; j++) {
            uint8_t cc = s[i + j];
            if ((cc & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return -1;
        if (extra == 2 && cp < 0x800) return -1;
        if (extra == 3 && cp < 0x10000) return -1;
        if (cp > 0x10FFFF) return -1;
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
        i += extra + 1;
    }
    return 0;
}

static void test_utf8_edge_table(void) {
    /* Plain ASCII passes. */
    CHECK(utf8_valid_text((const uint8_t *)"hello", 5) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"", 0) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"a\nb\tc", 5) == 0);
    /* Hungarian accented letters: e-acute, o-double-acute, u-double-acute. */
    CHECK(utf8_valid_text((const uint8_t *)"\xC3\xA9", 2) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"\xC5\x91", 2) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"\xC5\xB1", 2) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"h\xC3\xA9t", 4) == 0);
    /* 3-byte and 4-byte sequences pass. */
    CHECK(utf8_valid_text((const uint8_t *)"\xE2\x82\xAC", 3) == 0);
    CHECK(utf8_valid_text((const uint8_t *)"\xF0\x9F\x98\x80", 4) == 0);
    /* Overlong forms are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\xC0\xAF", 2) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xC1\xBF", 2) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xE0\x80\xAF", 3) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xF0\x80\x80\xAF", 4) == -1);
    /* Surrogates are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\xED\xA0\x80", 3) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xED\xBF\xBF", 3) == -1);
    /* Above U+10FFFF is rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\xF4\x90\x80\x80", 4) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xF5\x80\x80\x80", 4) == -1);
    /* Truncated sequences are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\xC3", 1) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xE2\x82", 2) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xF0\x9F\x98", 3) == -1);
    /* Bad continuation bytes are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\xC3\x28", 2) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xE2\x28\xAC", 3) == -1);
    /* Lone continuation bytes and 0xFF/0xFE are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"\x80", 1) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xFF", 1) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"\xFE", 1) == -1);
    /* NUL and control characters are rejected. */
    CHECK(utf8_valid_text((const uint8_t *)"a\0b", 3) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"a\x01" "b", 3) == -1);
    CHECK(utf8_valid_text((const uint8_t *)"a\x7F" "b", 3) == -1);
    /* Length cap: 4096 passes, 4097 does not. */
    static uint8_t big[4097];
    memset(big, 'a', sizeof(big));
    CHECK(utf8_valid_text(big, 4096) == 0);
    CHECK(utf8_valid_text(big, 4097) == -1);
}

static void test_utf8_fuzz(void) {
    int valid = 0, invalid = 0;
    for (int i = 0; i < 20000; i++) {
        size_t len = (size_t)(rng_next() % 65);
        uint8_t buf[64];
        for (size_t j = 0; j < len; j++) {
            /* Bias toward ASCII and toward UTF-8 lead/continuation bytes. */
            uint32_t r = rng_u32() % 10;
            if (r < 7) buf[j] = (uint8_t)(rng_u32() % 0x80);
            else if (r < 9) buf[j] = (uint8_t)(0xC0 + (rng_u32() % 0x40));
            else buf[j] = (uint8_t)rng_u32();
        }
        int rc = utf8_valid_text(buf, len);
        CHECK(rc == 0 || rc == -1);
        if (rc == 0) valid++;
        else invalid++;
        /* Pure ASCII without NUL/control always passes. */
        if (len > 0) {
            int all_plain = 1;
            for (size_t j = 0; j < len; j++) {
                if (buf[j] < 0x20 || buf[j] >= 0x7F) { all_plain = 0; break; }
            }
            if (all_plain) CHECK(rc == 0);
        }
    }
    CHECK(valid > 500);
    CHECK(invalid > 1000);
    printf("utf8 fuzz: 20000 iterations, %d valid, %d invalid\n", valid, invalid);
}

int main(void) {
    /* Unbuffered stdout: CHECK failures must survive a sanitizer abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_touch_edge_table();
    test_touch_fuzz();
    test_damage_edge_table();
    test_damage_fuzz();
    test_key_fuzz();
    test_utf8_edge_table();
    test_utf8_fuzz();
    printf("fuzz_touch_damage_utf8_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
