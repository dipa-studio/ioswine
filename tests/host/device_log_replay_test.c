/*
 * device_log_replay_test.c - Replay device-round logs.
 *
 * Part 1 pins the exact failure signatures from the two device logs
 * (verbatim excerpts).
 * Part 2 replays the logged touch inputs through the REAL touch.c
 * pipeline with the device-faithful async mock (mock_uikit_async.h).
 * It asserts the shared invariants (wrapper count, normalized
 * coords, context/sender id, no drops) in both states, then
 * branches on whether the drain already fired: on the unpatched
 * tree it asserts the device outcome (queued via _receiveHIDEvent:,
 * filtered stays 0, sendevent=0, tab after=0) and proves the pump
 * delivers; on a fixed tree it asserts the synchronous
 * drain already delivered everything. Branching is what lets this
 * test go fully green after the fix instead of pinning the broken
 * behavior forever.
 * Part 3 is the gate: the IOSWINE_TOUCH_SELFTEST=1 tap must reach
 * sendEvent: and flip the tab (after=1, sendevent>=1). It FAILS on
 * the unpatched tree (route A never drains) and PASSES once
 * touch_inject triggers the drain synchronously after queueing
 * (Internal + signal + drain_events, the combined route).
 * Part 4 is diagnostic only: pumping the fetch thread + runloop
 * source by hand proves the queued events were well-formed and
 * would deliver if the drain fired.
 *
 * Dependency: touch route. If fixed
 * purely by making the fetcher-thread workloop run (no synchronous
 * drain in touch_inject), part 3 stays red: this mock has no
 * workloop to pump, by design.
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

/* ------------------------------------------------------------------ */
/* Part 1: verbatim device-log excerpts + failure signatures          */
/* ------------------------------------------------------------------ */

/* touch-019-selftest.txt lines 1-13, verbatim (devq
 * aidoku-touch-selftest, first two taps). */
static const char kSelftestLog[] =
    "[touch] reader started\n"
    "[touch] sender id 2660229795 ctx=2660229795\n"
    "[touch] fetcher thread present? 1\n"
    "[touch] down x=77 y=787 id=0\n"
    "[touch] hid-built down nx=213 ny=984 id=0 wire_ts=35255469114101\n"
    "[touch] queued via _receiveHIDEvent:\n"
    "[touch] queue filtered=0 raw=-\n"
    "[touch] sender id 2660229795 ctx=2660229795\n"
    "[touch] fetcher thread present? 1\n"
    "[touch] up x=77 y=787 id=0\n"
    "[touch] hid-built up nx=213 ny=984 id=0 wire_ts=35255505986289\n"
    "[touch] queued via _receiveHIDEvent:\n"
    "[touch] queue filtered=0 raw=-\n";

/* touch-019-interactive.txt: wq-create lines 2 and 9, the UIKit
 * misconfiguration line 23, the selftest lines 24-25, and the crash
 * line 423 (devq aidoku-interactive, IPC trace on). The
 * wq_trampoline line is quoted from a trace: it does not appear
 * in the saved log file (interleaved output). */
static const char kInteractiveLog[] =
    "[thread] wq-create tid=0x693d prio=0x644006 workloop id=0x5014b8680 pthread=0x7d9431e000 kport=0x3903\n"
    "[thread] wq-create tid=0x694d prio=0x644005 workloop id=0x50694c800 pthread=0x7d87a94000 kport=0x5d4b\n"
    "2026-11-17 18:03:01.997 Aidoku[26919:26939] Misconfigured Device:  Default Event Timing Profile is missing (/System/Library/EventTimingProfiles/D47.Default.plist). Create empty file for a quick temporary workaround. File radar on UIKit for a permanent fix.\n"
    "[touch] selftest tab before=0 after=0\n"
    "[touch] selftest sendevent=0\n"
    "[lsk CRASH] sig=0x0000000000000004 pc=0x00000001be0214ec lr=0x00302801e2e933b0 sp=0x0000007d9453da80 fp=0x0000007d9453db20 x0=0x0000000000000000 x8=0x0000000000020012 x9=0x0000000000000001 x16=0x00000001ecc9cfc0\n";

static const char kTrampolineLine[] =
    "wq_trampoline: slot=0 wlh=0x50694c800 flags=0x464005 nevents=1";

static void test_log_signatures(void) {
    /* Selftest log: every event queued, nothing drained. */
    CHECK(strstr(kSelftestLog, "[touch] reader started\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] sender id 2660229795 ctx=2660229795\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] fetcher thread present? 1\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] down x=77 y=787 id=0\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] hid-built down nx=213 ny=984 id=0 wire_ts=35255469114101\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] queued via _receiveHIDEvent:\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] queue filtered=0 raw=-\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] up x=77 y=787 id=0\n") != NULL);
    CHECK(strstr(kSelftestLog, "[touch] hid-built up nx=213 ny=984 id=0 wire_ts=35255505986289\n") != NULL);
    /* The selftest summary lines live in the interactive log, not in
     * the selftest log (the selftest log ends mid-event at line 400). */
    CHECK(strstr(kInteractiveLog, "[touch] selftest tab before=0 after=0\n") != NULL);
    CHECK(strstr(kInteractiveLog, "[touch] selftest sendevent=0\n") != NULL);
    /* Crash signature: SIGILL (sig=4) at
     * __dispatch_workloop_worker_thread+272, pc 0x1be0214ec. */
    CHECK(strstr(kInteractiveLog, "[lsk CRASH] sig=0x0000000000000004 pc=0x00000001be0214ec") != NULL);
    /* The crashing workloop was created just before: tid 0x694d,
     * first-entry trampoline flags 0x644005 (WORKLOOP | TSD_BASE_SET
     * | NEWSPI | PRIO_QOS | qos 5). */
    CHECK(strstr(kInteractiveLog, "wq-create tid=0x694d prio=0x644005 workloop id=0x50694c800") != NULL);
    /* Task-doc trampoline line for the same workloop: reuse entry
     * (WORKLOOP | NEWSPI | REUSE | PRIO_QOS | qos 5), one event. */
    CHECK(strstr(kTrampolineLine, "slot=0 wlh=0x50694c800 flags=0x464005 nevents=1") != NULL);
    /* UIKit misconfiguration line. */
    CHECK(strstr(kInteractiveLog, "Misconfigured Device:  Default Event Timing Profile is missing (/System/Library/EventTimingProfiles/D47.Default.plist)") != NULL);
}

/* ------------------------------------------------------------------ */
/* Part 2: replay the logged inputs through real touch.c              */
/* ------------------------------------------------------------------ */

/* First three DOWN/UP pairs from touch-019-selftest.txt, as presenter
 * client pixels (360x800 client, same as window points here). The
 * hid-built nx/ny in the log are the normalized coords times 1000:
 * (77, 787) -> (213, 984); (101, 774) -> (280, 968);
 * (100, 776) -> (279, 970). */
static const struct {
    uint32_t action;
    float x;
    float y;
    int64_t ts;
} kLoggedTaps[] = {
    { ISWS_TOUCH_DOWN, 77.0f, 787.0f, 35255469114101LL },
    { ISWS_TOUCH_UP, 77.0f, 787.0f, 35255505986289LL },
    { ISWS_TOUCH_DOWN, 101.0f, 774.0f, 35255628932382LL },
    { ISWS_TOUCH_UP, 101.0f, 774.0f, 35255687761445LL },
    { ISWS_TOUCH_DOWN, 100.0f, 776.0f, 35255779263007LL },
    { ISWS_TOUCH_UP, 100.0f, 776.0f, 35255840822070LL },
};

static void test_replay_logged_inputs(void) {
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

    for (size_t i = 0; i < sizeof(kLoggedTaps) / sizeof(kLoggedTaps[0]); i++) {
        isws_touch_t t;
        t.action = kLoggedTaps[i].action;
        t.pointer_id = 0;
        t.pointer_count = 1;
        t.x = kLoggedTaps[i].x;
        t.y = kLoggedTaps[i].y;
        t.timestamp_ns = kLoggedTaps[i].ts;
        CHECK(touch_test_process_touch(&t, 360, 800) == 0);
    }

    /* Shared invariants, drain or no drain: every event was
     * accepted through _receiveHIDEventInternal: (route D, the
     * default) with NORMALIZED coords (finding: last
     * DOWN was (100, 776) -> (0.2778, 0.97)), the context id is
     * stamped via BKSHIDEventSetDigitizerInfo (the
     * guessed 0xb0005 field is NOT written by default), the sender
     * id follows it, and no drop counters fired. */
    CHECK(g_direct_internal_count == 6);
    CHECK(strstr(g_log_buf, "[touch] internal receive\n") != NULL);
    CHECK(g_last_children[0].index == 0);
    CHECK_NEAR(g_last_children[0].x, 100.0 / 360.0, 0.001);
    CHECK_NEAR(g_last_children[0].y, 776.0 / 800.0, 0.001);
    CHECK(g_last_received.bks_context_id == (int64_t)0x54321u);
    CHECK(g_last_received.context_id == 0);
    CHECK(g_last_received.sender_id == 0x54321u);
    {
        uint64_t rf = 0, iv = 0, st = 0, ca = 0;
        touch_test_drop_counts(&rf, &iv, &st, &ca);
        CHECK(rf == 0 && iv == 0 && st == 0 && ca == 0);
    }

    if (touch_test_sendevent_count() == 0) {
        /* Unpatched tree: the device outcome, reproduced on host.
         * Every event queued, filtered stays 0, nothing reaches
         * sendEvent:, the tab does not move. */
        CHECK(g_raw_queue_count == 6);
        CHECK(g_filtered_queue_count == 0);
        CHECK(g_tab_selected_index == 0);
        CHECK(g_window_send_event_count == 0);
        CHECK(strstr(g_log_buf, "[touch] queue filtered=0 raw=6\n") != NULL);
        /* Diagnostic only: pump the fetch thread and the runloop
         * source by hand. The queued events are well-formed: they
         * deliver, the first tap (77, 787) is inside the Browse tab
         * rect, the tab flips. This proves the only missing piece
         * on the device was the drain trigger, not the events. */
        {
            int pumped = mock_run_fetch_thread();
            CHECK(pumped == 6);
            CHECK(g_filtered_queue_count == 6);
            CHECK(g_raw_queue_count == 0);
            mock_fire_main_runloop_source();
            CHECK(touch_test_sendevent_count() >= 1);
            CHECK(g_window_send_event_count >= 1);
            CHECK(g_tab_selected_index == 1);
            CHECK(strstr(g_log_buf, "[touch] sendEvent: reached with touch count 1\n") != NULL);
        }
    } else {
        /* Fixed tree: the synchronous drain in touch_inject
         * already delivered all six events (raw 0, tab flipped). */
        CHECK(g_raw_queue_count == 0);
        CHECK(g_filtered_queue_count == 0);
        CHECK(g_window_send_event_count >= 1);
        CHECK(g_tab_selected_index == 1);
        CHECK(mock_run_fetch_thread() == 0);
    }
}

/* ------------------------------------------------------------------ */
/* Part 3: the gate. Selftest tap must reach sendEvent: + tab.       */
/* ------------------------------------------------------------------ */

static void test_selftest_gate(void) {
    touch_test_reset();
    reset_mocks();
    mock_async_setup_fixture();

    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;

    touch_test_configure(&mock_async_ops_base);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_selftest = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    touch_on_frame(19);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);
    touch_on_frame(21);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);

    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] down x=107 y=791 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] up x=107 y=791 id=0\n") != NULL);
    /* GATE: fails on the unpatched tree (after=0, sendevent=0),
     * passes once the drain is triggered in touch_inject. */
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1\n") != NULL);
    CHECK(touch_test_sendevent_count() >= 1);
    CHECK(g_window_send_event_count >= 1);

    /* Repeated frame 20 does not re-run. */
    {
        size_t len_before = g_log_len;
        touch_on_frame(20);
        CHECK(g_log_len == len_before);
    }
}

int main(void) {
    test_log_signatures();
    test_replay_logged_inputs();
    test_selftest_gate();
    printf("device_log_replay_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
