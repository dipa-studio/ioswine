/*
 * hidevent_test.c - host test for hidevent.c.
 *
 * Build (plain, from this directory):
 *   cc -std=c11 -Wall -Wextra -O1 -g \
 *     -I../../.. -I../../src/common \
 *     hidevent_test.c ../../src/hidevent.c ../../src/common/protocol.c \
 *     -o hidevent_test -lm
 * Build (sanitizers, from this directory):
 *   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *     -fno-sanitize-recover=all \
 *     -I../../.. -I../../src/common \
 *     hidevent_test.c ../../src/hidevent.c ../../src/common/protocol.c \
 *     -o hidevent_test_asan -lm
 * Run:
 *   ./hidevent_test            (fuzz default: 20000 iterations)
 *   ./hidevent_test 100000     (fuzz iterations from argv[1])
 *
 * Golden vectors are byte-exact: every expected word is a literal
 * taken from captured HID event traces. The word
 * layout is documented in the comment above the HID_PARENT macro.
 * IOFixed values used below were computed by hand from the IEEE-754
 * float values (double multiply by 65536, add 0.5, truncate):
 *   0.0 -> 0x00000   0.1 -> 0x0199A   0.2 -> 0x03333
 *   0.25 -> 0x04000  0.3 -> 0x04CCD   0.4 -> 0x06666
 *   0.5 -> 0x08000   0.6 -> 0x0999A   0.7 -> 0x0B333
 *   0.8 -> 0x0CCCD   0.9 -> 0x0E666   1.0 -> 0x10000
 * (e.g. 0.3f is 0.30000001192092896; x65536 = 19660.80; +0.5 = 19661.30;
 *  truncate -> 19661 = 0x4CCD.)
 *
 * Coordinates in the tests are NORMALIZED [0,1], as the caller must
 * supply them; hidevent.c itself only clamps.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hidevent.h"

/* ------------------------------------------------------------------ */
/* Test harness                                                         */
/* ------------------------------------------------------------------ */

static long g_checks = 0;
static long g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Golden-vector word layout                                            */
/* ------------------------------------------------------------------ */

/*
 * One IOHIDDigitizerEventData is 104 bytes = 26 u32 words, little
 * endian. Word map:
 *   w0  size             w1  type (11 = Digitizer)  w2  options
 *   w3  depth (byte 12) + reserved[3]
 *   w4  position.x (IOFixed)  w5  position.y  w6  position.z
 *   w7  transducerIndex       w8  transducerType
 *   w9  identity              w10 eventMask   w11 childEventMask
 *   w12 buttonMask            w13 pressure (IOFixed)
 *   w14 auxPressure           w15 twist
 *   w16 orientationType (2 = Quality)
 *   w17 quality  w18 density  w19 irregularity
 *   w20 majorRadius  w21 minorRadius  w22 accuracy
 *   w23 generationCount  w24 willUpdateMask  w25 didUpdateMask
 *
 * Parent:
 *   size = total flat length, options 0x90003 (Absolute | Collection
 *   | range | displayIntegrated), transducerType 3, identity 0,
 *   eventMask == childEventMask == OR of children's masks,
 *   pressure 0, orientation Quality, radii 3.0 (0x30000),
 *   quality/density/irregularity 0.
 * Child down/move:
 *   size 104, options 0xB0001 (Absolute | range | touch |
 *   displayIntegrated), depth 1, transducerType 2, identity 2,
 *   mask 0x3 (down) or 0x4 (move), childEventMask 0, pressure 1.0
 *   (0x10000), orientation Quality, quality/density/irregularity
 *   1.0 (0x10000), radii 5.0 (0x50000).
 * Child up (Finger): options 0x80001 (Absolute | displayIntegrated),
 *   mask 0x3, pressure 0.0; everything else as down.
 * Child cancel (Finger): as up, but mask 0x83 (Cancel | Range | Touch).
 */
#define HID_PARENT(TOTAL, MASK) \
    (TOTAL), 11, 0x90003u, 0, \
    0, 0, 0, 0, 3, 0, (MASK), (MASK), 0, 0, 0, 0, 2, \
    0, 0, 0, 0x30000u, 0x30000u, 0, 0, 0, 0

#define HID_CHILD_DOWN(X, Y, IDX) \
    104, 11, 0xB0001u, 1, \
    (X), (Y), 0, (IDX), 2, 2, 0x3u, 0, 0, 0x10000u, 0, 0, 2, \
    0x10000u, 0x10000u, 0x10000u, 0x50000u, 0x50000u, 0, 0, 0, 0

#define HID_CHILD_MOVE(X, Y, IDX) \
    104, 11, 0xB0001u, 1, \
    (X), (Y), 0, (IDX), 2, 2, 0x4u, 0, 0, 0x10000u, 0, 0, 2, \
    0x10000u, 0x10000u, 0x10000u, 0x50000u, 0x50000u, 0, 0, 0, 0

#define HID_CHILD_UP(X, Y, IDX) \
    104, 11, 0x80001u, 1, \
    (X), (Y), 0, (IDX), 2, 2, 0x3u, 0, 0, 0, 0, 0, 2, \
    0x10000u, 0x10000u, 0x10000u, 0x50000u, 0x50000u, 0, 0, 0, 0

#define HID_CHILD_CANCEL(X, Y, IDX) \
    104, 11, 0x80001u, 1, \
    (X), (Y), 0, (IDX), 2, 2, 0x83u, 0, 0, 0, 0, 0, 2, \
    0x10000u, 0x10000u, 0x10000u, 0x50000u, 0x50000u, 0, 0, 0, 0

/* Compare the emitted blob against expected words, one CHECK per word. */
static void expect_blob(const uint8_t *got, int n,
                        const uint32_t *want, int nwords,
                        const char *label)
{
    int i;
    CHECK(n == nwords * 4);
    if (n != nwords * 4) {
        printf("FAIL %s: length %d, expected %d bytes\n",
               label, n, nwords * 4);
        return;
    }
    for (i = 0; i < nwords; i++) {
        uint32_t g = isws_rd_u32le(got + i * 4);
        if (g != want[i]) {
            g_checks++;
            g_failures++;
            printf("FAIL %s: word %d got 0x%08x want 0x%08x\n",
                   label, i, g, want[i]);
        } else {
            g_checks++;
        }
    }
}

static isws_touch_t touch(uint32_t action, uint32_t id, uint32_t count,
                          float x, float y)
{
    isws_touch_t t;
    memset(&t, 0, sizeof(t));
    t.action = action;
    t.pointer_id = id;
    t.pointer_count = count;
    t.x = x;
    t.y = y;
    t.timestamp_ns = 1000;
    return t;
}

/* ------------------------------------------------------------------ */
/* Golden vectors                                                       */
/* ------------------------------------------------------------------ */

static void test_tap(void)
{
    uint8_t out[1144];
    isws_touch_t t;
    int r;

    /* Single tap, down/move/up. */
    hidevent_reset();

    /* DOWN finger 0 at (0.5, 0.25): parent size 208 (parent + 1
     * child), parent masks 0x3; child down, position IOFixed
     * (0x8000, 0x4000), transducerIndex 0, identity 2. */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, 0.25f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 1);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x3u),
            HID_CHILD_DOWN(0x8000u, 0x4000u, 0),
        };
        expect_blob(out, r, want, 52, "tap down");
    }

    /* MOVE finger 0 to (0.5, 0.3): parent mask 0x4 (Position);
     * child mask 0x4, position (0x8000, 0x4CCD). */
    t = touch(ISWS_TOUCH_MOVE, 0, 1, 0.5f, 0.3f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 1);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x4u),
            HID_CHILD_MOVE(0x8000u, 0x4CCDu, 0),
        };
        expect_blob(out, r, want, 52, "tap move");
    }

    /* UP finger 0 at (0.5, 0.3): parent mask 0x3; child up, options
     * 0x80001 (range/touch bits clear), pressure 0. */
    t = touch(ISWS_TOUCH_UP, 0, 1, 0.5f, 0.3f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 0);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x3u),
            HID_CHILD_UP(0x8000u, 0x4CCDu, 0),
        };
        expect_blob(out, r, want, 52, "tap up");
    }
}

static void test_two_finger(void)
{
    uint8_t out[1144];
    isws_touch_t t;
    int r;

    hidevent_reset();

    /* DOWN finger 0 at (0.1, 0.2). */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.1f, 0.2f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 1);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x3u),
            HID_CHILD_DOWN(0x199Au, 0x3333u, 0),
        };
        expect_blob(out, r, want, 52, "two-finger down0");
    }

    /* DOWN finger 1 at (0.7, 0.8): 312 bytes. Finger 0 is re-sent
     * as a hold (mask 0x3, range/touch 1, last position), finger 1
     * as the down child. Children in ascending pointer_id order. */
    t = touch(ISWS_TOUCH_DOWN, 1, 2, 0.7f, 0.8f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    CHECK(hidevent_fingers_down() == 2);
    {
        static const uint32_t want[] = {
            HID_PARENT(312, 0x3u),
            HID_CHILD_DOWN(0x199Au, 0x3333u, 0),
            HID_CHILD_DOWN(0xB333u, 0xCCCDu, 1),
        };
        expect_blob(out, r, want, 78, "two-finger down1");
    }

    /* MOVE finger 0 to (0.2, 0.2): parent mask 0x7 (Position from
     * the move OR Range|Touch from the hold); child 0 is the move,
     * child 1 is the hold at (0.7, 0.8). */
    t = touch(ISWS_TOUCH_MOVE, 0, 2, 0.2f, 0.2f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    CHECK(hidevent_fingers_down() == 2);
    {
        static const uint32_t want[] = {
            HID_PARENT(312, 0x7u),
            HID_CHILD_MOVE(0x3333u, 0x3333u, 0),
            HID_CHILD_DOWN(0xB333u, 0xCCCDu, 1),
        };
        expect_blob(out, r, want, 78, "two-finger move0");
    }

    /* UP finger 1 at (0.7, 0.8): child 0 hold, child 1 up.
     * Parent mask 0x3 (both children carry Range|Touch). */
    t = touch(ISWS_TOUCH_UP, 1, 2, 0.7f, 0.8f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    CHECK(hidevent_fingers_down() == 1);
    {
        static const uint32_t want[] = {
            HID_PARENT(312, 0x3u),
            HID_CHILD_DOWN(0x3333u, 0x3333u, 0),
            HID_CHILD_UP(0xB333u, 0xCCCDu, 1),
        };
        expect_blob(out, r, want, 78, "two-finger up1");
    }

    /* UP finger 0 at (0.2, 0.2): back to a single child. */
    t = touch(ISWS_TOUCH_UP, 0, 1, 0.2f, 0.2f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 0);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x3u),
            HID_CHILD_UP(0x3333u, 0x3333u, 0),
        };
        expect_blob(out, r, want, 52, "two-finger up0");
    }
}

static void test_cancel(void)
{
    uint8_t out[1144];
    isws_touch_t t;
    int r;

    hidevent_reset();

    /* DOWN finger 3 at (0.4, 0.6). */
    t = touch(ISWS_TOUCH_DOWN, 3, 1, 0.4f, 0.6f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 1);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x3u),
            HID_CHILD_DOWN(0x6666u, 0x999Au, 3),
        };
        expect_blob(out, r, want, 52, "cancel down");
    }

    /* CANCEL finger 3: parent mask 0x83, child mask 0x83
     * (Cancel | Range | Touch), options 0x80001, pressure 0.
     * This mask is a [guess]. */
    t = touch(ISWS_TOUCH_CANCEL, 3, 1, 0.4f, 0.6f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(hidevent_fingers_down() == 0);
    {
        static const uint32_t want[] = {
            HID_PARENT(208, 0x83u),
            HID_CHILD_CANCEL(0x6666u, 0x999Au, 3),
        };
        expect_blob(out, r, want, 52, "cancel");
    }
}

/* ------------------------------------------------------------------ */
/* Edge cases                                                           */
/* ------------------------------------------------------------------ */

static void test_edge(void)
{
    uint8_t out[1144];
    isws_touch_t t;
    int r;

    hidevent_reset();

    /* NULL arguments. */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(NULL, out, sizeof(out)) == -1);
    CHECK(hidevent_touch(&t, NULL, sizeof(out)) == -1);
    CHECK(hidevent_fingers_down() == 0);

    /* Bad pointer id. */
    t = touch(ISWS_TOUCH_DOWN, 10, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    t = touch(ISWS_TOUCH_DOWN, 99, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);

    /* pointer_count == 0 is never valid (the count includes the
     * finger in the message, protocol.h). */
    t = touch(ISWS_TOUCH_DOWN, 0, 0, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);

    /* Unknown action. */
    t = touch(4, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    t = touch(0xFFFFFFFFu, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    CHECK(hidevent_fingers_down() == 0);

    /* Non-finite coordinates. */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, NAN, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, NAN);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    t = touch(ISWS_TOUCH_DOWN, 0, 1, INFINITY, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, -INFINITY);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == -1);
    CHECK(hidevent_fingers_down() == 0);

    /* Out of range (finite) coordinates are clamped, not rejected. */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 1.5f, -0.5f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    CHECK(isws_rd_u32le(out + 104 + 16) == 0x10000u); /* x = 1.0 */
    CHECK(isws_rd_u32le(out + 104 + 20) == 0x00000u); /* y = 0.0 */
    CHECK(hidevent_fingers_down() == 1);
    t = touch(ISWS_TOUCH_UP, 0, 1, 1.5f, -0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 208);
    CHECK(hidevent_fingers_down() == 0);

    /* Buffer too small: one byte short of a 208-byte event. The
     * failed call must not change state. */
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, 207) == -1);
    CHECK(hidevent_fingers_down() == 0);
    CHECK(hidevent_touch(&t, out, 208) == 208);
    CHECK(hidevent_fingers_down() == 1);

    /* Buffer too small for a two-finger event (needs 312). */
    t = touch(ISWS_TOUCH_DOWN, 1, 2, 0.6f, 0.6f);
    CHECK(hidevent_touch(&t, out, 311) == -1);
    CHECK(hidevent_fingers_down() == 1);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 312);
    CHECK(hidevent_fingers_down() == 2);

    /* Duplicate move: bit-identical position, no state change. */
    t = touch(ISWS_TOUCH_MOVE, 1, 2, 0.6f, 0.6f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    CHECK(hidevent_fingers_down() == 2);

    /* Duplicate DOWN at the identical position: also no event. */
    t = touch(ISWS_TOUCH_DOWN, 1, 2, 0.6f, 0.6f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);

    /* DOWN at a new position for an already-down finger refreshes
     * it and emits normally. */
    t = touch(ISWS_TOUCH_DOWN, 1, 2, 0.7f, 0.6f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    CHECK(isws_rd_u32le(out + 208 + 16) == 0xB333u); /* child 1 x */
    CHECK(isws_rd_u32le(out + 208 + 20) == 0x999Au); /* child 1 y */

    /* MOVE / UP / CANCEL for a finger that is not down: ignored. */
    t = touch(ISWS_TOUCH_MOVE, 5, 2, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    t = touch(ISWS_TOUCH_UP, 5, 2, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    t = touch(ISWS_TOUCH_CANCEL, 5, 2, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    CHECK(hidevent_fingers_down() == 2);

    /* hidevent_reset clears state; a later UP for a forgotten
     * finger is ignored. */
    hidevent_reset();
    CHECK(hidevent_fingers_down() == 0);
    t = touch(ISWS_TOUCH_UP, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    t = touch(ISWS_TOUCH_DOWN, 0, 1, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 208);
    CHECK(hidevent_fingers_down() == 1);
    hidevent_reset();
}

static void test_ten_fingers(void)
{
    uint8_t out[1144];
    isws_touch_t t;
    int r;
    uint32_t i;

    hidevent_reset();

    /* All ten fingers down: the worst case is 104 * 11 = 1144 bytes. */
    for (i = 0; i < 10; i++) {
        t = touch(ISWS_TOUCH_DOWN, i, i + 1,
                  (float)i * 0.1f, 0.5f);
        r = hidevent_touch(&t, out, sizeof(out));
        CHECK(r == (int)(104 * (2 + i)));
        CHECK(hidevent_fingers_down() == i + 1);
    }
    CHECK(r == 1144);
    /* Parent size is the total flat length; children are in
     * ascending pointer_id order with depth 1 and type 11. */
    CHECK(isws_rd_u32le(out + 0) == 1144u);
    CHECK(isws_rd_u32le(out + 4) == 11u);
    for (i = 0; i < 10; i++) {
        const uint8_t *c = out + 104 * (1 + i);
        CHECK(isws_rd_u32le(c + 0) == 104u);
        CHECK(isws_rd_u32le(c + 4) == 11u);
        CHECK(c[12] == 1); /* depth */
        CHECK(isws_rd_u32le(c + 28) == i); /* transducerIndex */
    }

    /* A tenth finger does not fit in a 1143-byte buffer, and the
     * failed call leaves the table alone. */
    hidevent_reset();
    for (i = 0; i < 9; i++) {
        t = touch(ISWS_TOUCH_DOWN, i, i + 1, 0.5f, 0.5f);
        r = hidevent_touch(&t, out, sizeof(out));
        CHECK(r == (int)(104 * (2 + i)));
    }
    CHECK(hidevent_fingers_down() == 9);
    t = touch(ISWS_TOUCH_DOWN, 9, 10, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, 1143) == -1);
    CHECK(hidevent_fingers_down() == 9);
    CHECK(hidevent_touch(&t, out, 1144) == 1144);
    CHECK(hidevent_fingers_down() == 10);
    hidevent_reset();
}

/* ------------------------------------------------------------------ */
/* Fuzz                                                                 */
/* ------------------------------------------------------------------ */

static uint32_t g_rng = 0x12345678u;

static uint32_t rnd32(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static float rnd_float_bits(void)
{
    union { uint32_t u; float f; } v;
    v.u = rnd32();
    return v.f;
}

static void test_fuzz(long iterations)
{
    uint8_t out[1144];
    long i;

    hidevent_reset();
    for (i = 0; i < iterations; i++) {
        isws_touch_t t;
        int r;
        uint32_t kind = rnd32() % 4;

        memset(&t, 0, sizeof(t));
        /* action 0..5: 4 and 5 are invalid and must be rejected. */
        t.action = rnd32() % 6;
        /* pointer_id 0..12: 10..12 are invalid. */
        t.pointer_id = rnd32() % 13;
        /* pointer_count 0..3: 0 is invalid. */
        t.pointer_count = rnd32() % 4;
        if (kind == 0) {
            /* Sane normalized coordinates. */
            t.x = (float)(rnd32() % 10001) / 10000.0f;
            t.y = (float)(rnd32() % 10001) / 10000.0f;
        } else if (kind == 1) {
            /* Out-of-range finite coordinates (clamp path). */
            t.x = (float)((int)(rnd32() % 400) - 200);
            t.y = (float)((int)(rnd32() % 400) - 200);
        } else if (kind == 2) {
            /* Arbitrary bit patterns: NaN, infinities, denormals. */
            t.x = rnd_float_bits();
            t.y = rnd_float_bits();
        } else {
            /* Extreme magnitudes. */
            t.x = (rnd32() & 1) ? 1.0e30f : -1.0e30f;
            t.y = (rnd32() & 1) ? 3.4e38f : -3.4e38f;
        }
        t.timestamp_ns = (int64_t)rnd32();

        r = hidevent_touch(&t, out, sizeof(out));
        CHECK(r >= -1 && r <= 1144);
        CHECK(hidevent_fingers_down() <= 10);
        if (r > 0) {
            uint32_t j;
            uint32_t nchild;
            /* Length is a whole number of 104-byte structs. */
            CHECK(r % 104 == 0);
            CHECK(r >= 208);
            nchild = (uint32_t)r / 104 - 1;
            CHECK(nchild >= 1 && nchild <= 10);
            /* Parent size field is the total flat length. */
            CHECK(isws_rd_u32le(out + 0) == (uint32_t)r);
            CHECK(isws_rd_u32le(out + 4) == 11u);
            CHECK(out[12] == 0); /* parent depth */
            for (j = 0; j < nchild; j++) {
                const uint8_t *c = out + 104 * (1 + j);
                CHECK(isws_rd_u32le(c + 0) == 104u);
                CHECK(isws_rd_u32le(c + 4) == 11u);
                CHECK(c[12] == 1); /* child depth */
                CHECK(isws_rd_u32le(c + 28) < 10u); /* index */
                CHECK(isws_rd_u32le(c + 32) == 2u); /* Finger */
            }
        }
        if (i % 1000 == 999)
            hidevent_reset();
    }
    hidevent_reset();
    printf("fuzz: %ld iterations done\n", iterations);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    long fuzz_n = 20000;

    if (argc > 1)
        fuzz_n = atol(argv[1]);

    test_tap();
    test_two_finger();
    test_cancel();
    test_edge();
    test_ten_fingers();
    test_fuzz(fuzz_n);

    if (g_failures == 0) {
        printf("PASS: %ld checks, 0 failures\n", g_checks);
        return 0;
    }
    printf("FAIL: %ld checks, %ld failures\n", g_checks, g_failures);
    return 1;
}
