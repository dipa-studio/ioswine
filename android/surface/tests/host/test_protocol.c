/*
 * Host unit test for the ioswine-surface protocol (protocol.c).
 * Plain `cc`, no NDK, no Android headers:
 *   cc -std=c11 -Wall -Wextra -I ../../src/common test_protocol.c \
 *      ../../src/common/protocol.c -o test_protocol -lm && ./test_protocol
 *
 * Covers: LE helpers, header encode/decode, round-trip of every message
 * type, malformed-input rejection, HELLO validation, memfd size,
 * coordinate mapping (screen<->client, letterbox), damage clipping.
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

static void test_le_helpers(void) {
    uint8_t buf[16];
    isws_wr_u16le(buf, 0x1234);
    CHECK(buf[0] == 0x34 && buf[1] == 0x12);
    CHECK(isws_rd_u16le(buf) == 0x1234);
    isws_wr_u32le(buf, 0xdeadbeefu);
    CHECK(buf[0] == 0xef && buf[3] == 0xde);
    CHECK(isws_rd_u32le(buf) == 0xdeadbeefu);
    isws_wr_u64le(buf, 0x0123456789abcdefULL);
    CHECK(buf[0] == 0xef && buf[7] == 0x01);
    CHECK(isws_rd_u64le(buf) == 0x0123456789abcdefULL);
    isws_wr_i64le(buf, -123456789012345LL);
    CHECK(isws_rd_i64le(buf) == -123456789012345LL);
    isws_wr_f32le(buf, 3.5f);
    CHECK_FLOAT_EQ(isws_rd_f32le(buf), 3.5f);
    /* Unaligned access must work (buf+1). */
    isws_wr_u32le(buf + 1, 0x11223344u);
    CHECK(isws_rd_u32le(buf + 1) == 0x11223344u);
}

static void test_header(void) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    CHECK(isws_encode_header(hdr, ISWS_MSG_PRESENT, 0, 123) == ISWS_HEADER_SIZE);
    uint16_t type, flags;
    uint32_t len;
    CHECK(isws_decode_header(hdr, sizeof(hdr), &type, &flags, &len) == 0);
    CHECK(type == ISWS_MSG_PRESENT && flags == 0 && len == 123);
    /* Bad magic. */
    hdr[0] ^= 0xff;
    CHECK(isws_decode_header(hdr, sizeof(hdr), &type, &flags, &len) == -1);
    hdr[0] ^= 0xff;
    /* Length over the cap. */
    isws_wr_u32le(hdr + 8, ISWS_MAX_PAYLOAD + 1);
    CHECK(isws_decode_header(hdr, sizeof(hdr), &type, &flags, &len) == -2);
    /* Short buffer. */
    CHECK(isws_decode_header(hdr, 5, &type, &flags, &len) == -1);
}

static void test_hello(void) {
    isws_hello_t h = { .version = ISWS_VERSION, .width = 1080,
                       .height = 2400, .stride = 1080 * 4,
                       .format = ISWS_FORMAT_BGRA8888_PREMUL, .flags = 0 };
    uint8_t buf[64];
    size_t n = isws_encode_hello(buf, sizeof(buf), &h);
    CHECK(n == ISWS_HELLO_PAYLOAD_SIZE);
    isws_hello_t h2;
    CHECK(isws_decode_hello(buf, n, &h2) == 0);
    CHECK(memcmp(&h, &h2, sizeof(h)) == 0);
    CHECK(isws_validate_hello(&h) == ISWS_ERR_NONE);
    CHECK(isws_memfd_size(&h) == (uint64_t)1080 * 4 * 2400 * 2);
    /* Wrong payload length rejected. */
    CHECK(isws_decode_hello(buf, n - 1, &h2) != 0);
    CHECK(isws_decode_hello(buf, n + 1, &h2) != 0);
    /* Too small output buffer. */
    CHECK(isws_encode_hello(buf, 10, &h) == 0);
    /* Validation failures. */
    isws_hello_t bad = h;
    bad.version = 99;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_VERSION);
    bad = h; bad.width = 0;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.width = ISWS_MAX_WIDTH + 1;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.height = 0;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.stride = h.width * 4 - 4; /* smaller than width*4 */
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.stride = h.width * 4 + 2; /* not a multiple of 4 */
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.format = 2;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.flags = 1;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    bad = h; bad.stride = ISWS_MAX_STRIDE + 4;
    CHECK(isws_validate_hello(&bad) == ISWS_ERR_BAD_HELLO);
    CHECK(isws_memfd_size(&bad) == 0);
}

static void test_present(void) {
    isws_present_t p;
    memset(&p, 0, sizeof(p));
    p.frame_id = 0x1122334455667788ULL;
    p.buffer_index = 1;
    p.damage_count = 2;
    p.damage[0] = (isws_rect_t){ .x = 10, .y = 20, .w = 100, .h = 50 };
    p.damage[1] = (isws_rect_t){ .x = -5, .y = -5, .w = 30, .h = 30 };
    uint8_t buf[512];
    size_t n = isws_encode_present(buf, sizeof(buf), &p);
    CHECK(n == 16 + 16 * 2);
    isws_present_t p2;
    CHECK(isws_decode_present(buf, n, &p2) == 0);
    CHECK(p2.frame_id == p.frame_id && p2.buffer_index == 1 && p2.damage_count == 2);
    CHECK(p2.damage[0].x == 10 && p2.damage[0].w == 100);
    CHECK(p2.damage[1].x == -5 && p2.damage[1].h == 30);
    /* Zero damage = whole frame. */
    p.damage_count = 0;
    n = isws_encode_present(buf, sizeof(buf), &p);
    CHECK(n == 16);
    CHECK(isws_decode_present(buf, n, &p2) == 0 && p2.damage_count == 0);
    /* Malformed: truncated, extra bytes, too many rects, bad buffer index. */
    CHECK(isws_decode_present(buf, 15, &p2) != 0);
    CHECK(isws_decode_present(buf, n + 1, &p2) != 0);
    isws_wr_u32le(buf + 8, ISWS_NUM_BUFFERS);
    CHECK(isws_decode_present(buf, n, &p2) != 0);
    isws_wr_u32le(buf + 8, 0);
    uint8_t bad[64];
    memset(bad, 0, sizeof(bad));
    isws_wr_u64le(bad, 1);
    isws_wr_u32le(bad + 12, ISWS_MAX_DAMAGE + 1);
    CHECK(isws_decode_present(bad, sizeof(bad), &p2) != 0);
    /* Encode rejects too many rects / small buffer. */
    p.damage_count = ISWS_MAX_DAMAGE + 1;
    CHECK(isws_encode_present(buf, sizeof(buf), &p) == 0);
    p.damage_count = 2;
    CHECK(isws_encode_present(buf, 10, &p) == 0);
}

static void test_simple_messages(void) {
    uint8_t buf[256];
    /* READY */
    isws_ready_t r = { .version = 1, .flags = 0, .max_in_flight = 2 }, r2;
    size_t n = isws_encode_ready(buf, sizeof(buf), &r);
    CHECK(n == ISWS_READY_PAYLOAD_SIZE);
    CHECK(isws_decode_ready(buf, n, &r2) == 0);
    CHECK(r2.version == 1 && r2.max_in_flight == 2);
    CHECK(isws_decode_ready(buf, n - 1, &r2) != 0);
    /* FRAME_DONE */
    isws_frame_done_t d = { .frame_id = 42, .vsync_ns = 123456789, .status = 0 }, d2;
    n = isws_encode_frame_done(buf, sizeof(buf), &d);
    CHECK(n == ISWS_FRAME_DONE_PAYLOAD_SIZE);
    CHECK(isws_decode_frame_done(buf, n, &d2) == 0);
    CHECK(d2.frame_id == 42 && d2.vsync_ns == 123456789 && d2.status == 0);
    CHECK(isws_decode_frame_done(buf, 10, &d2) != 0);
    isws_wr_u32le(buf + 16, 2); /* bad status */
    CHECK(isws_decode_frame_done(buf, n, &d2) != 0);
    isws_wr_u32le(buf + 16, 0);
    /* TOUCH */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 3,
                       .pointer_count = 2, .x = 123.5f, .y = 456.25f,
                       .timestamp_ns = -7 }, t2;
    n = isws_encode_touch(buf, sizeof(buf), &t);
    CHECK(n == ISWS_TOUCH_PAYLOAD_SIZE);
    CHECK(isws_decode_touch(buf, n, &t2) == 0);
    CHECK(t2.action == ISWS_TOUCH_DOWN && t2.pointer_id == 3);
    CHECK_FLOAT_EQ(t2.x, 123.5f);
    CHECK_FLOAT_EQ(t2.y, 456.25f);
    CHECK(t2.timestamp_ns == -7);
    /* TOUCH rejects bad action / pointer id / count. */
    isws_wr_u32le(buf + 0, 99);
    CHECK(isws_decode_touch(buf, n, &t2) != 0);
    isws_wr_u32le(buf + 0, ISWS_TOUCH_MOVE);
    isws_wr_u32le(buf + 4, ISWS_MAX_POINTERS);
    CHECK(isws_decode_touch(buf, n, &t2) != 0);
    isws_wr_u32le(buf + 4, 0);
    isws_wr_u32le(buf + 8, 0); /* pointer_count 0 rejected */
    CHECK(isws_decode_touch(buf, n, &t2) != 0);
    isws_wr_u32le(buf + 8, ISWS_MAX_POINTERS + 1);
    CHECK(isws_decode_touch(buf, n, &t2) != 0);
    /* KEY */
    isws_key_t k = { .keycode = ISWS_KEY_BACK, .action = ISWS_KEY_ACTION_UP,
                     .timestamp_ns = 999 }, k2;
    n = isws_encode_key(buf, sizeof(buf), &k);
    CHECK(n == ISWS_KEY_PAYLOAD_SIZE);
    CHECK(isws_decode_key(buf, n, &k2) == 0);
    CHECK(k2.keycode == ISWS_KEY_BACK && k2.action == ISWS_KEY_ACTION_UP);
    isws_wr_u32le(buf + 4, 7);
    CHECK(isws_decode_key(buf, n, &k2) != 0);
    /* INFO */
    isws_info_t info = { .screen_w = 1080, .screen_h = 2400, .client_w = 1080,
                         .client_h = 2400, .scale = 1.0f, .offset_x = 0,
                         .offset_y = 0, .rotation = 0 }, info2;
    n = isws_encode_info(buf, sizeof(buf), &info);
    CHECK(n == ISWS_INFO_PAYLOAD_SIZE);
    CHECK(isws_decode_info(buf, n, &info2) == 0);
    CHECK(info2.screen_w == 1080 && info2.rotation == 0);
    CHECK_FLOAT_EQ(info2.scale, 1.0f);
    isws_wr_f32le(buf + 16, 0.0f); /* scale 0 */
    CHECK(isws_decode_info(buf, n, &info2) != 0);
    isws_wr_f32le(buf + 16, 1.0f);
    isws_wr_u32le(buf + 0, 0); /* screen_w 0 */
    CHECK(isws_decode_info(buf, n, &info2) != 0);
    isws_wr_u32le(buf + 0, 1080);
    isws_wr_u32le(buf + 28, 45); /* bad rotation */
    CHECK(isws_decode_info(buf, n, &info2) != 0);
    /* FOCUS */
    isws_focus_t f = { .focused = 1, .reserved = 0 }, f2;
    n = isws_encode_focus(buf, sizeof(buf), &f);
    CHECK(n == ISWS_FOCUS_PAYLOAD_SIZE);
    CHECK(isws_decode_focus(buf, n, &f2) == 0 && f2.focused == 1);
    isws_wr_u32le(buf + 0, 2);
    CHECK(isws_decode_focus(buf, n, &f2) != 0);
    /* ERROR */
    const char *msg = "something broke";
    n = isws_encode_error(buf, sizeof(buf), ISWS_ERR_BAD_HELLO, msg, strlen(msg));
    CHECK(n == 4 + strlen(msg));
    uint32_t code;
    const uint8_t *mout;
    size_t mlen;
    CHECK(isws_decode_error(buf, n, &code, &mout, &mlen) == 0);
    CHECK(code == ISWS_ERR_BAD_HELLO && mlen == strlen(msg));
    CHECK(memcmp(mout, msg, mlen) == 0);
    CHECK(isws_decode_error(buf, 3, &code, &mout, &mlen) != 0);
    /* ERROR with an empty message. */
    n = isws_encode_error(buf, sizeof(buf), ISWS_ERR_NONE, "", 0);
    CHECK(n == 4);
    CHECK(isws_decode_error(buf, n, &code, &mout, &mlen) == 0 && mlen == 0);
}

static void test_mapping(void) {
    isws_info_t m;
    /* Exact fit: 1080x2400 client on a 1080x2400 screen. */
    CHECK(isws_compute_mapping(1080, 2400, 1080, 2400, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, 0.0f);
    /* iPhone-ish 1170x2532 client on a 1440x3200 screen: scale by width. */
    CHECK(isws_compute_mapping(1440, 3200, 1170, 2532, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1440.0f / 1170.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK(m.offset_y > 0.0f); /* letterbox top/bottom */
    /* Wide client on a tall screen: scale by width (0.54), the frame
     * exactly fills the screen width, bars top/bottom. */
    CHECK(isws_compute_mapping(1080, 2400, 2000, 1000, 0, &m) == 0);
    CHECK_FLOAT_EQ(m.scale, 1080.0f / 2000.0f);
    CHECK_FLOAT_EQ(m.offset_x, 0.0f);
    CHECK_FLOAT_EQ(m.offset_y, (2400.0f - 1000.0f * 0.54f) * 0.5f);
    /* Zero dimensions / bad rotation rejected. */
    CHECK(isws_compute_mapping(0, 2400, 1080, 2400, 0, &m) != 0);
    CHECK(isws_compute_mapping(1080, 2400, 1080, 0, 0, &m) != 0);
    CHECK(isws_compute_mapping(1080, 2400, 1080, 2400, 45, &m) != 0);

    /* Round-trip: client -> screen -> client. The painted origin is
     * the integer dest rect, not the float offset: offset_y here is
     * 41.846 (pre-rounding) while the blit starts at the rounded row
     * 42, so comparing the screen point against m.offset_y can never
     * be exact and a 1 px tolerance there would hide a real off-by-one
     * in the rect. The contract is exact through the dest rect, so
     * the origin is asserted against isws_dest_rect itself, and the
     * round-trip/centre checks below use a 0.01 px tolerance (float
     * round-trip error here is 0.0002 px at most, measured). */
    CHECK(isws_compute_mapping(1440, 3200, 1170, 2532, 0, &m) == 0);
    float sx, sy, cx, cy;
    int32_t x0, y0, x1, y1;
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 42 && x1 == 1440 && y1 == 3158);
    isws_client_to_screen(&m, 0.0f, 0.0f, &sx, &sy);
    CHECK_FLOAT_EQ(sx, (float)x0);
    CHECK_FLOAT_EQ(sy, (float)y0);
    CHECK(isws_screen_to_client(&m, sx, sy, &cx, &cy) == 0);
    CHECK_FLOAT_EQ(cx, 0.0f);
    CHECK_FLOAT_EQ(cy, 0.0f);
    isws_client_to_screen(&m, 1169.0f, 2531.0f, &sx, &sy);
    CHECK(isws_screen_to_client(&m, sx, sy, &cx, &cy) == 0);
    CHECK(fabsf(cx - 1169.0f) < 0.01f && fabsf(cy - 2531.0f) < 0.01f);
    /* A point in the letterbox bar is reported outside and clamped. */
    CHECK(isws_screen_to_client(&m, 720.0f, 2.0f, &cx, &cy) == -1);
    CHECK_FLOAT_EQ(cy, 0.0f);
    /* Centre of the screen maps to the centre of the client frame. */
    CHECK(isws_screen_to_client(&m, 720.0f, 1600.0f, &cx, &cy) == 0);
    CHECK(fabsf(cx - 585.0f) < 0.01f);
    CHECK(fabsf(cy - 1266.0f) < 0.01f);
}

static void test_damage(void) {
    isws_rect_t out;
    /* Fully inside: unchanged. */
    CHECK(isws_clip_damage(&(isws_rect_t){ 10, 20, 100, 50 }, 1080, 2400, &out) == 0);
    CHECK(out.x == 10 && out.y == 20 && out.w == 100 && out.h == 50);
    /* Negative origin clipped. */
    CHECK(isws_clip_damage(&(isws_rect_t){ -10, -20, 30, 40 }, 1080, 2400, &out) == 0);
    CHECK(out.x == 0 && out.y == 0 && out.w == 20 && out.h == 20);
    /* Overflowing the right/bottom edge clipped. */
    CHECK(isws_clip_damage(&(isws_rect_t){ 1000, 2300, 200, 200 }, 1080, 2400, &out) == 0);
    CHECK(out.w == 80 && out.h == 100);
    /* Fully outside / empty rejected. */
    CHECK(isws_clip_damage(&(isws_rect_t){ 2000, 0, 10, 10 }, 1080, 2400, &out) != 0);
    CHECK(isws_clip_damage(&(isws_rect_t){ 0, 0, 0, 10 }, 1080, 2400, &out) != 0);
    CHECK(isws_clip_damage(&(isws_rect_t){ -50, -50, 10, 10 }, 1080, 2400, &out) != 0);

    /* expand_damage: count 0 -> whole frame. */
    isws_present_t p;
    memset(&p, 0, sizeof(p));
    isws_rect_t rects[ISWS_MAX_DAMAGE];
    CHECK(isws_expand_damage(&p, 1080, 2400, rects) == 1);
    CHECK(rects[0].w == 1080 && rects[0].h == 2400);
    /* Mixed list: one good, one empty, one partially clipped. */
    p.damage_count = 3;
    p.damage[0] = (isws_rect_t){ 0, 0, 10, 10 };
    p.damage[1] = (isws_rect_t){ 5000, 5000, 10, 10 }; /* dropped */
    p.damage[2] = (isws_rect_t){ -5, 100, 20, 10 };    /* clipped to 15x10 */
    size_t n = isws_expand_damage(&p, 1080, 2400, rects);
    CHECK(n == 2);
    CHECK(rects[0].w == 10 && rects[0].h == 10);
    CHECK(rects[1].x == 0 && rects[1].w == 15 && rects[1].h == 10);
}

int main(void) {
    test_le_helpers();
    test_header();
    test_hello();
    test_present();
    test_simple_messages();
    test_mapping();
    test_damage();
    printf("test_protocol: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
