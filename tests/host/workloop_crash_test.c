/*
 * workloop_crash_test.c - Workloop crash regression test.
 *
 * The defect: kevent_get_next_ready() in libsystem_kernel/src/kevent.c
 * leaves *out_wlh stale when it returns 0. It sets *out_wlh = wlh right
 * after claiming a workloop busy, but the count == 0 path (nothing
 * deliverable for that workloop, e.g. only a sync-waiter knote was
 * active) releases the claim and keeps scanning WITHOUT resetting
 * *out_wlh, and the loop-end return 0 does not reset it either. The
 * park loop in lsk_workq_kernreturn (libsystem_kernel/src/workq.c)
 * dispatches on next_wlh != 0 alone, so it hands __pthread_wqthread a
 * WORKLOOP task with nevents == 0 and a stale event list.
 *
 * Why that crashes: __dispatch_workloop_worker_thread is the only
 * consumer of a WORKLOOP-flagged wq entry, and it assumes nevents >= 1
 * (dispatch_assume is live in release builds and calls _dispatch_bug,
 * which is a brk / SIGILL trap; the inlined _dispatch_adopt_wlh also
 * traps on a lingering DISPATCH_WLH_ANON). The observed device crash was exactly that:
 * SIGILL at __dispatch_workloop_worker_thread+272 after 36 frames.
 * If the trap did not fire first, the same defect livelocks the park
 * loop: the sync-waiter knote is never consumed, so every park-loop
 * iteration re-dispatches the same phantom 0-event workloop entry.
 *
 * Device sequence replayed here (test_phantom_workloop_entry):
 *   - workloop W = 0x50694c800, the workloop id from the crash log's
 *     wq-create line (tid 0x694d, prio 0x644005).
 *   - the thread-request knote for W was delivered and consumed during
 *     36 frames of draining, so it is inactive at crash time.
 *   - a client holds a SYNC_WAIT knote on W (sync IPC against that
 *     workloop); it is the only active knote for W.
 *   - the park loop calls kevent_get_next_ready and must get back
 *     "no work" (return 0, out_wlh == 0). Unpatched it gets
 *     return 0 with out_wlh == W: a phantom workloop entry.
 *
 * The test drives the REAL kevent.c (host build, -DKEVENT_HOST_TEST=1)
 * through the public path: register knotes with kevent_apply_changes,
 * then call kevent_get_next_ready. It FAILS on the unpatched tree
 * (tests 2 and 3) and PASSES on the patched tree. The workq_* stubs
 * mirror libsystem_kernel/src/workq.c exactly (busy table, 64 entries,
 * same return contract); workq_dispatch_kevent is controllable so a
 * test can leave a thread-request knote active (refuse) or consume it
 * (accept), matching what the real dispatcher does.
 *
 * Build (same shape as kevent_pset_test, but no ipc.c: the ipc externs
 * kevent.c needs are stubbed here because this test only registers
 * EVFILT_WORKLOOP knotes):
 *   cc -std=gnu11 -O1 -g -w -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DKEVENT_HOST_TEST=1 \
 *      -iquote $B/src -iquote tests/host \
 *      -o $B/workloop_crash_test tests/host/workloop_crash_test.c \
 *      $B/src/kevent.c -lpthread
 *   $B/workloop_crash_test
 *
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "workq.h"

/* ------------------------------------------------------------------ */
/* Stubs for symbols the real kevent.c needs.                          */
/* ------------------------------------------------------------------ */

/* Mach time: a counter that advances, so timeouts behave. */
static uint64_t g_now_ticks = 1000000;
uint64_t _mach_absolute_time(void) {
    g_now_ticks += 1000;
    return g_now_ticks;
}
int _mach_timebase_info(void *info) {
    uint32_t *p = (uint32_t *)info;
    p[0] = 1;
    p[1] = 1;
    return 0;
}
uint32_t _mach_thread_self(void) {
    return 0x303; /* main thread */
}

int g_ipc_trace = 0;
void set_errno(int err) { (void)err; }

/* ipc.c externs kevent.c references. This test only registers
 * EVFILT_WORKLOOP knotes, so the machport branches never run; the
 * stubs exist only to satisfy the linker. ipc_set_enqueue_hook is
 * called once by kevent_init; record the hook, do nothing with it. */
static void (*g_enqueue_hook)(uint32_t name, uint32_t msgh_size) = NULL;
void ipc_set_enqueue_hook(void (*hook)(uint32_t name, uint32_t msgh_size)) {
    g_enqueue_hook = hook;
}
uint32_t ipc_port_get_pset_name(uint32_t name) {
    (void)name;
    return 0;
}
int ipc_port_has_receive_right(uint32_t name) {
    (void)name;
    return 1;
}
uint32_t ipc_port_peek_size(uint32_t name) {
    (void)name;
    return 0;
}

/* workq_dispatch_kevent stub: controllable. When g_dispatch_ok is 1 the
 * delivery is accepted and recorded (the real dispatcher spawns or
 * wakes a worker); when 0 it is refused (no free slot / busy), which
 * leaves the knote active, exactly as the real dispatcher does. */
static int g_dispatch_ok = 1;
static int g_dispatched_count = 0;
static struct kevent_qos_s g_last_dispatched;
int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos) {
    (void)wlh;
    (void)qos;
    if (!g_dispatch_ok) return 0;
    if (nevents > 0) g_last_dispatched = ev[0];
    g_dispatched_count += nevents;
    return 1;
}

void workq_lock(void) {}
void workq_unlock(void) {}

/* Busy table: mirrors libsystem_kernel/src/workq.c (MAX_BUSY_WLH 64,
 * wlh == 0 always succeeds, full table drops the claim and returns 0). */
#define TEST_MAX_BUSY_WLH 64
static uint64_t g_busy_wlh[TEST_MAX_BUSY_WLH];
static int g_busy_wlh_count = 0;
int workq_is_wlh_busy(uint64_t wlh) {
    for (int i = 0; i < g_busy_wlh_count; i++) {
        if (g_busy_wlh[i] == wlh) return 1;
    }
    return 0;
}
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    if (wlh == 0) return 1;
    if (busy) {
        if (workq_is_wlh_busy(wlh)) return 1;
        if (g_busy_wlh_count < TEST_MAX_BUSY_WLH) {
            g_busy_wlh[g_busy_wlh_count++] = wlh;
            return 1;
        }
        return 0;
    }
    for (int i = 0; i < g_busy_wlh_count; i++) {
        if (g_busy_wlh[i] == wlh) {
            g_busy_wlh[i] = g_busy_wlh[--g_busy_wlh_count];
            break;
        }
    }
    return 1;
}
static void busy_reset(void) {
    g_busy_wlh_count = 0;
}

/* ------------------------------------------------------------------ */
/* Small harness.                                                      */
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

/* Workloop ids taken from the device log's wq-create lines. */
#define W_CRASH 0x50694c800ULL /* tid 0x694d, the crashing workloop */
#define W_OTHER 0x50681e000ULL /* tid 0x694f */
#define W_FIRST 0x5014b8680ULL /* tid 0x693d */

static int apply_change(uint64_t wlh, uint64_t ident, int16_t filter,
                        uint16_t flags, uint32_t fflags, uint64_t udata) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = ident;
    ch.filter = filter;
    ch.flags = flags;
    ch.qos = 4;
    ch.fflags = fflags;
    ch.udata = udata;
    return kevent_apply_changes(wlh, &ch, 1, NULL, 0, 0);
}

/* The park-loop dispatch decision in lsk_workq_kernreturn, as written
 * before the fix: dispatch whenever next_wlh != 0. The test asserts the
 * decision is false (no dispatch) for the no-work cases. */
static int park_loop_would_dispatch(uint64_t next_wlh, int nev) {
    return next_wlh != 0;
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/* Empty kq: no knotes at all. Must report no work and no wlh. */
static void test_empty_kq(void) {
    struct kevent_qos_s ev[16];
    uint64_t out_wlh = 0;
    int qos = -1;
    int nev = kevent_get_next_ready(&out_wlh, ev, 16, &qos);
    CHECK(nev == 0);
    CHECK(out_wlh == 0);
    CHECK(!park_loop_would_dispatch(out_wlh, nev));
    CHECK(g_busy_wlh_count == 0);
}

/* Primary RED test: the exact device shape. W_CRASH had its
 * thread-request knote delivered and consumed (36 frames of draining),
 * and now only a SYNC_WAIT knote is active for it. get_next_ready must
 * report no work (return 0, out_wlh == 0). Unpatched it returns 0
 * with out_wlh == W_CRASH: the park loop would dispatch a WORKLOOP
 * task with nevents == 0 into __dispatch_workloop_worker_thread. */
static void test_phantom_workloop_entry(void) {
    busy_reset();
    g_dispatch_ok = 1;
    g_dispatched_count = 0;

    /* Thread-request knote for W_CRASH, delivered and consumed. */
    CHECK(apply_change(W_CRASH, W_CRASH, EVFILT_WORKLOOP, EV_ADD,
                       NOTE_WL_THREAD_REQUEST, W_CRASH) == 0);
    CHECK(g_dispatched_count == 1);
    CHECK(g_last_dispatched.ident == W_CRASH);
    CHECK(g_last_dispatched.filter == EVFILT_WORKLOOP);
    CHECK((g_last_dispatched.fflags & NOTE_WL_THREAD_REQUEST) != 0);

    /* A client holds a SYNC_WAIT knote on W_CRASH (ident is the
     * waiter's Mach port, not the workloop id, not this thread). */
    CHECK(apply_change(W_CRASH, 0x1234, EVFILT_WORKLOOP, EV_ADD,
                       NOTE_WL_SYNC_WAIT, 0) == 0);
    CHECK(g_dispatched_count == 1); /* waiter registration dispatches nothing */

    struct kevent_qos_s ev[16];
    uint64_t out_wlh = 0;
    int qos = -1;
    int nev = kevent_get_next_ready(&out_wlh, ev, 16, &qos);
    CHECK(nev == 0);
    CHECK(out_wlh == 0);
    CHECK(!park_loop_would_dispatch(out_wlh, nev));
    /* The claim taken while scanning must have been released. */
    CHECK(workq_is_wlh_busy(W_CRASH) == 0);
    CHECK(g_busy_wlh_count == 0);
}

/* Same defect, no thread-request knote at all: only a sync waiter is
 * registered for the workloop. Also must report no work. */
static void test_waiter_only_no_thread_request(void) {
    busy_reset();
    CHECK(apply_change(W_OTHER, 0x5678, EVFILT_WORKLOOP, EV_ADD,
                       NOTE_WL_SYNC_WAIT, 0) == 0);
    struct kevent_qos_s ev[16];
    uint64_t out_wlh = 0;
    int qos = -1;
    int nev = kevent_get_next_ready(&out_wlh, ev, 16, &qos);
    CHECK(nev == 0);
    CHECK(out_wlh == 0);
    CHECK(!park_loop_would_dispatch(out_wlh, nev));
    CHECK(workq_is_wlh_busy(W_OTHER) == 0);
}

/* Normal case, guard against over-fixing: a live thread-request knote
 * must still be delivered first, with the workloop claimed busy.
 * The dispatcher refuses here (g_dispatch_ok = 0) so the knote stays
 * active after registration, as it is when the park loop finds it.
 * Note this also proves get_next_ready keeps scanning past the stale
 * waiter knotes left active by the earlier tests and finds real work. */
static void test_thread_request_still_delivered(void) {
    busy_reset();
    g_dispatch_ok = 0;
    g_dispatched_count = 0;
    CHECK(apply_change(W_FIRST, W_FIRST, EVFILT_WORKLOOP, EV_ADD,
                       NOTE_WL_THREAD_REQUEST, W_FIRST) == 0);
    CHECK(g_dispatched_count == 0); /* refused: knote stays active */

    struct kevent_qos_s ev[16];
    uint64_t out_wlh = 0;
    int qos = -1;
    int nev = kevent_get_next_ready(&out_wlh, ev, 16, &qos);
    CHECK(nev == 1);
    CHECK(out_wlh == W_FIRST);
    CHECK(park_loop_would_dispatch(out_wlh, nev));
    CHECK(ev[0].ident == W_FIRST);
    CHECK(ev[0].filter == EVFILT_WORKLOOP);
    CHECK((ev[0].fflags & NOTE_WL_THREAD_REQUEST) != 0);
    CHECK(ev[0].udata == W_FIRST);
    CHECK(qos == 4);
    /* The claim is held on success: the caller owns it until the
     * workloop return clears it. */
    CHECK(workq_is_wlh_busy(W_FIRST) == 1);
    workq_set_wlh_busy(W_FIRST, 0);
    g_dispatch_ok = 1;
}

int main(void) {
    /* Unbuffered stdout: CHECK failures must survive a sanitizer abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_empty_kq();
    test_phantom_workloop_entry();
    test_waiter_only_no_thread_request();
    test_thread_request_still_delivered();
    printf("workloop_crash_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
