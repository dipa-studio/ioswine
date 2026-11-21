/*
 * attributegraph_test.c - host test for attributegraph.{c,h}
 *
 *
 * The device fault this pins (measured 2026-11-17):
 *   [lsk CRASH] sig=0xb pc=0x1b6cecb70 lr=0x1b6cec914
 *   x0=1 x8=0 x9=0x20
 * i.e. SIGSEGV in AG::Graph::UpdateStack::update, x8=0: the update
 * dereferences a nil display/time source. It fired right after a
 * tap on a SwiftUI cell
 * (_TtGC7SwiftUI15CellHostingViewGVS_15ModifiedContentVS_14_ViewList_ViewVS_26CollectionViewCellModifier__)
 * reached sendEvent: on the main queue. The update is driven by
 * SwiftUI's display-link cycle (ViewRendererHost.render advances
 * the host's time and flushes graph transactions); our runtime has
 * no CADisplay and no firing display link, so the first animated
 * transaction flushes with no display/time source.
 *
 * The mock below models that faulting load: mock_ag_update() reads
 * the display/time source the way UpdateStack::update does, but
 * detects the nil and reports FAULT instead of dereferencing it,
 * so the host test can assert the pattern without crashing. The
 * guard (ag_update_begin) is what our code consults before forcing
 * a graph update; the test proves it refuses exactly in the device
 * precondition (no display tick ever recorded) and allows the
 * update once a tick has advanced host time.
 *
 * Build (plain):
 *   cc -std=c11 -Wall -Wextra -Werror -DAG_HOST_TEST=1
 *      -I libsystem_kernel/src tests/host/attributegraph_test.c
 *      libsystem_kernel/src/attributegraph.c -o /tmp/attributegraph_test
 * Build (ASan+UBSan): add -fsanitize=address,undefined -g.
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdint.h>

#include "attributegraph.h"

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
/* Mock of the AG update entry                                         */
/* ------------------------------------------------------------------ */

/* The display/time source UpdateStack::update dereferences. On
 * device this is nil (no CADisplay, no display tick ever fired);
 * the mock makes the nil explicit instead of faulting. */
typedef struct {
    int64_t host_time_ns;
} mock_display_t;

#define MOCK_AG_OK 0
#define MOCK_AG_FAULT -1

static int mock_update_invocations;
static int64_t mock_update_observed_ns;

static void mock_reset(void) {
    mock_update_invocations = 0;
    mock_update_observed_ns = -1;
}

/* Models AG::Graph::UpdateStack::update's load of the host-time
 * source. The real function faults (SIGSEGV, x8=0) on a nil source;
 * the mock reports MOCK_AG_FAULT instead of dereferencing it. */
static int mock_ag_update(mock_display_t *disp) {
    mock_update_invocations++;
    if (disp == NULL) return MOCK_AG_FAULT;
    mock_update_observed_ns = disp->host_time_ns;
    return MOCK_AG_OK;
}

/* Guarded driver: the only way our code forces a graph update. It
 * consults ag_update_begin() first and never invokes the update
 * when the guard refuses. */
static int drive_update(mock_display_t *disp) {
    if (ag_update_begin() != 0) return -1;
    return mock_ag_update(disp);
}

/* The device's touch-up wire_ts for the crashing tap
 * (live-taps-20261117.txt, the CellHostingView up phase). Used as
 * the first display-tick timestamp so the replay uses a real
 * device value. */
#define DEVICE_TAP_UP_TS 1075488407609LL

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_initial_state(void) {
    ag_test_reset();
    CHECK(ag_update_cycle_ready() == 0);
    CHECK_EQ_INT(ag_tick_count(), 0);
    CHECK_EQ_INT(ag_last_tick_ns(), -1);
    CHECK(ag_update_begin() == -1);
}

static void test_device_fault_pattern(void) {
    ag_test_reset();
    mock_reset();
    /* Device precondition: no display tick ever recorded, so the
     * guard refuses the update. */
    CHECK(ag_update_begin() == -1);
    /* The unguarded update with the device's nil display/time
     * source faults: this is the live-taps line 211 SIGSEGV
     * (x8=0), modeled as a detected nil instead of a real fault. */
    CHECK(mock_ag_update(NULL) == MOCK_AG_FAULT);
    /* The guarded driver refuses and never invokes the update. */
    CHECK(drive_update(NULL) == -1);
    CHECK_EQ_INT(mock_update_invocations, 1);
}

static void test_first_tick(void) {
    mock_display_t disp;
    ag_test_reset();
    mock_reset();
    ag_note_display_tick(DEVICE_TAP_UP_TS);
    CHECK(ag_update_cycle_ready() == 1);
    CHECK_EQ_INT(ag_tick_count(), 1);
    CHECK_EQ_INT(ag_last_tick_ns(), DEVICE_TAP_UP_TS);
    CHECK(ag_update_begin() == 0);
    disp.host_time_ns = DEVICE_TAP_UP_TS;
    CHECK(drive_update(&disp) == MOCK_AG_OK);
    CHECK_EQ_INT(mock_update_invocations, 1);
    CHECK_EQ_INT(mock_update_observed_ns, DEVICE_TAP_UP_TS);
}

static void test_tick_monotonic(void) {
    ag_test_reset();
    ag_note_display_tick(DEVICE_TAP_UP_TS);
    /* A backwards tick is ignored: host time never runs back. */
    ag_note_display_tick(DEVICE_TAP_UP_TS - 1);
    CHECK_EQ_INT(ag_tick_count(), 1);
    CHECK_EQ_INT(ag_last_tick_ns(), DEVICE_TAP_UP_TS);
    /* An equal tick is ignored: it advances nothing. */
    ag_note_display_tick(DEVICE_TAP_UP_TS);
    CHECK_EQ_INT(ag_tick_count(), 1);
    /* A negative tick is ignored. */
    ag_note_display_tick(-5);
    CHECK_EQ_INT(ag_tick_count(), 1);
    CHECK_EQ_INT(ag_last_tick_ns(), DEVICE_TAP_UP_TS);
    /* The next frame (16.6 ms later) advances the cycle. */
    ag_note_display_tick(DEVICE_TAP_UP_TS + 16666666LL);
    CHECK_EQ_INT(ag_tick_count(), 2);
    CHECK_EQ_INT(ag_last_tick_ns(), DEVICE_TAP_UP_TS + 16666666LL);
    CHECK(ag_update_cycle_ready() == 1);
}

static void test_reset(void) {
    ag_test_reset();
    ag_note_display_tick(DEVICE_TAP_UP_TS);
    ag_test_reset();
    CHECK(ag_update_cycle_ready() == 0);
    CHECK_EQ_INT(ag_tick_count(), 0);
    CHECK_EQ_INT(ag_last_tick_ns(), -1);
    CHECK(ag_update_begin() == -1);
}

static void test_device_replay(void) {
    mock_display_t disp;
    ag_test_reset();
    mock_reset();
    /* Replay the crashing tap: state change, flush, no display tick
     * ever recorded. The guard refuses; no update runs, no fault. */
    CHECK(drive_update(NULL) == -1);
    CHECK_EQ_INT(mock_update_invocations, 0);
    /* The display link fires once. The same flush now
     * has its time source and completes. */
    ag_note_display_tick(DEVICE_TAP_UP_TS);
    disp.host_time_ns = DEVICE_TAP_UP_TS;
    CHECK(drive_update(&disp) == MOCK_AG_OK);
    CHECK_EQ_INT(mock_update_invocations, 1);
    CHECK_EQ_INT(mock_update_observed_ns, DEVICE_TAP_UP_TS);
}

int main(void) {
    test_initial_state();
    test_device_fault_pattern();
    test_first_tick();
    test_tick_monotonic();
    test_reset();
    test_device_replay();
    printf("attributegraph_test: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
