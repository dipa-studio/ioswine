/*
 * e2e_touch_window_test.c - End-to-end touch: presenter wire bytes to
 * tab switch.
 *
 * Drives the REAL protocol.c + touch.c through the device-faithful
 * async mock (mock_uikit_async.h):
 *   presenter screen tap (1440x3200 px) -> isws_screen_to_client
 *   -> ISWS_MSG_TOUCH wire bytes (encode) -> decode -> inject
 *   (touch_test_process_touch, client 360x800) -> fetcher queue
 *   -> (Internal + signal + drain) -> sendEvent:
 *   -> window hit-test by context id -> tab switch.
 *
 * The gate (after=1, sendevent>=1) requires that touch_inject trigger
 * the drain synchronously after queueing; see device_log_replay_test.c
 * for the dependency note. The other sub-cases branch on whether the
 * drain already fired: the multi-touch case pins the delivered total
 * (pump + already delivered == 4), and the context-id sub-case
 * reproduces the stale-context symptom at the mock level only when
 * drain has not yet occurred (app sendEvent: fires, window sendEvent:
 * does not, tab stays 0); under a synchronous drain that symptom
 * cannot occur, and the sub-case asserts the immediate delivery instead.
 *
 * Plain cc + ASan/UBSan on Linux host. No Apple code, no device.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_uikit_async.h"

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

/* Encode one presenter TOUCH message exactly as the Android
 * presenter sends it, then decode it back the way touch.c receives
 * it. Returns 0 and fills *out on success. */
static int wire_touch(uint32_t action, uint32_t pointer_id, float x, float y,
                      int64_t ts, isws_touch_t *out) {
    uint8_t msg[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
    isws_touch_t in;
    uint16_t type = 0, flags = 0;
    uint32_t plen = 0;
    in.action = action;
    in.pointer_id = pointer_id;
    in.pointer_count = 1;
    in.x = x;
    in.y = y;
    in.timestamp_ns = ts;
    if (isws_encode_header(msg, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE) != ISWS_HEADER_SIZE) return -1;
    if (isws_encode_touch(msg + ISWS_HEADER_SIZE, ISWS_TOUCH_PAYLOAD_SIZE, &in) != ISWS_TOUCH_PAYLOAD_SIZE) return -1;
    if (isws_decode_header(msg, sizeof(msg), &type, &flags, &plen) != 0) return -1;
    if (type != ISWS_MSG_TOUCH || plen != ISWS_TOUCH_PAYLOAD_SIZE) return -1;
    if (isws_decode_touch(msg + ISWS_HEADER_SIZE, plen, out) != 0) return -1;
    return 0;
}

static void test_geometry_mapping(void) {
    isws_info_t info;
    float cx = -1.0f, cy = -1.0f;
    int32_t x0 = -1, y0 = -1, x1 = -1, y1 = -1;
    /* Geometry: 1440x3200 phone, 360x800 pt window at
     * scale 4.0 is an exact fit, no margin, no bar. */
    CHECK(isws_compute_mapping(1440, 3200, 360, 800, 0, &info) == 0);
    CHECK(info.scale == 4.0f);
    CHECK(info.offset_x == 0.0f);
    CHECK(info.offset_y == 0.0f);
    isws_dest_rect(&info, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);
    /* Screen tap on the Browse tab (428, 3164) maps to client
     * (107, 791), the selftest tap point. */
    CHECK(isws_screen_to_client(&info, 428.0f, 3164.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 107.0, 0.001);
    CHECK_NEAR(cy, 791.0, 0.001);
    /* A tap in the black top bar of a non-exact-fit frame would map
     * outside the client frame; with the exact-fit mapping there is
     * no bar, so every screen point maps inside. */
    CHECK(isws_screen_to_client(&info, 720.0f, 1600.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 180.0, 0.001);
    CHECK_NEAR(cy, 400.0, 0.001);
}

/* Async-mock fixture; the default route is D, so these tests exercise
 * the synchronous Internal path, not route A. */
static void setup_async(void) {
    touch_test_reset();
    reset_mocks();
    mock_async_setup_fixture();
    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;
    touch_test_configure(&mock_async_ops_base);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_selftest = NULL;
    touch_init();
    touch_set_window_size(360.0, 800.0);
}

static void test_wire_tap_tab_switch(void) {
    isws_touch_t t;
    setup_async();

    /* DOWN (screen 428,3164 -> client 107,791), MOVE, UP: the
     * selftest tap as presenter wire bytes. */
    CHECK(wire_touch(ISWS_TOUCH_DOWN, 0, 107.0f, 791.0f, 1000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(wire_touch(ISWS_TOUCH_MOVE, 0, 110.0f, 791.0f, 2000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(wire_touch(ISWS_TOUCH_UP, 0, 107.0f, 791.0f, 3000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);

    /* Three events through _receiveHIDEventInternal: (route D).
     * If queued through the wrapper without synchronous drain,
     * none is drained yet; with synchronous drain enabled,
     * it has already delivered them. */
    CHECK(g_direct_internal_count == 3);
    if (touch_test_sendevent_count() == 0) {
        CHECK(g_raw_queue_count == 3);
        CHECK(g_filtered_queue_count == 0);
        CHECK(g_tab_selected_index == 0);
    }

    /* GATE: requires synchronous drain. */
    CHECK(touch_test_sendevent_count() >= 1);
    CHECK(g_window_send_event_count >= 1);
    CHECK(g_tab_selected_index == 1);
}

static void test_wire_multitouch(void) {
    isws_touch_t t;
    setup_async();

    /* Finger 0 taps the tab; finger 1 taps outside it (300, 100).
     * The tab flips once, from finger 0's UP; finger 1 must not
     * disturb it, and both fingers must be tracked independently. */
    CHECK(wire_touch(ISWS_TOUCH_DOWN, 0, 107.0f, 791.0f, 1000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(wire_touch(ISWS_TOUCH_DOWN, 1, 300.0f, 100.0f, 1100, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(touch_test_fingers_down() == 2);
    CHECK(wire_touch(ISWS_TOUCH_UP, 1, 300.0f, 100.0f, 2000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(touch_test_fingers_down() == 1);
    CHECK(wire_touch(ISWS_TOUCH_UP, 0, 107.0f, 791.0f, 3000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(touch_test_fingers_down() == 0);

    /* Diagnostic pump: both taps deliver, the tab flips exactly
     * once, from finger 0. With synchronous drain enabled, the
     * drain already delivered them and the pump is a no-op; the
     * total is what is pinned. */
    {
        int before = (int)touch_test_sendevent_count();
        int pumped = mock_run_fetch_thread();
        CHECK(pumped + before == 4);
    }
    mock_fire_main_runloop_source();
    CHECK(touch_test_sendevent_count() == 4);
    CHECK(g_tab_selected_index == 1);
}

static void test_context_id_mismatch(void) {
    isws_touch_t t;
    setup_async();

    /* The events carry the window context id at inject time... */
    CHECK(wire_touch(ISWS_TOUCH_DOWN, 0, 107.0f, 791.0f, 1000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(wire_touch(ISWS_TOUCH_UP, 0, 107.0f, 791.0f, 2000, &t) == 0);
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    CHECK(g_last_received.bks_context_id == (int64_t)0x54321u);
    CHECK(g_last_received.context_id == 0);
    /* ...but by drain time the key window has a different context
     * id (window recreated / stale event). Stale-context symptom:
     * -[UIApplication sendEvent:] fires, -[UIWindow sendEvent:]
     * never runs, the tab does not switch. */
    if (touch_test_sendevent_count() == 0) {
        /* Asynchronous drain: the events drain after the window
         * changed context, so -[UIApplication sendEvent:] fires
         * but -[UIWindow sendEvent:] never runs and the tab does
         * not switch. */
        g_fake_context_id = 0x99999u;
        CHECK(mock_run_fetch_thread() == 2);
        mock_fire_main_runloop_source();
        CHECK(touch_test_sendevent_count() >= 1);
        CHECK(g_window_send_event_count == 0);
        CHECK(g_tab_selected_index == 0);
    } else {
        /* Synchronous drain: delivered both events while the
         * context still matched, so the stale-context symptom
         * cannot occur. */
        CHECK(g_window_send_event_count >= 1);
        CHECK(g_tab_selected_index == 1);
    }
}

static void test_wire_validation(void) {
    isws_touch_t out;
    uint8_t msg[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
    /* A truncated wire message must not decode or inject. */
    CHECK(wire_touch(ISWS_TOUCH_DOWN, 0, 107.0f, 791.0f, 1000, &out) == 0);
    memset(msg, 0, sizeof(msg));
    CHECK(isws_decode_touch(msg, ISWS_TOUCH_PAYLOAD_SIZE - 1, &out) == -1);
    /* Unknown action encodes (the encoder does not validate) but
     * must not decode. */
    {
        isws_touch_t bad;
        bad.action = 99;
        bad.pointer_id = 0;
        bad.pointer_count = 1;
        bad.x = 107.0f;
        bad.y = 791.0f;
        bad.timestamp_ns = 1000;
        CHECK(isws_encode_touch(msg, sizeof(msg), &bad) == ISWS_TOUCH_PAYLOAD_SIZE);
        CHECK(isws_decode_touch(msg, ISWS_TOUCH_PAYLOAD_SIZE, &out) == -1);
    }
}

int main(void) {
    test_geometry_mapping();
    test_wire_tap_tab_switch();
    test_wire_multitouch();
    test_context_id_mismatch();
    test_wire_validation();
    printf("e2e_touch_window_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
