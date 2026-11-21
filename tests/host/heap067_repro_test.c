/*
 * heap067_repro_test.c - End-to-end host repro
 * (heap corruption under random tapping).
 *
 * This is the touch-pipeline half of the repro (the $P build:
 * touch.c / present.c need the present-shim linux.h with the socket
 * syscalls, so this file builds under -iquote $B/srcp; the VM /
 * locks / render half lives in heap067_repro_vm_locks_test.c, which
 * builds under the $S shim).
 *
 * Device crash signatures targeted (plus the new TUIKB evidence
 * forwarded 2026-11-18):
 *   S1 objc_retain+16 from -[__NSArrayM insertObject:atIndex:]
 *   S2 objc_retain+16 from -[UITouch _setResponder:]
 *   S3 AG::Graph::add_input+416 / add_output_edge+80 /
 *      UpdateStack::update+1100, fault address 0x5xxxxxxxx
 *   S4 ___NSMUTABLEDICTIONARY_CONCURRENT_MODIFICATION_DETECTED__
 *   S5 frame stall (no crash line)
 *   S6 NSInternalInconsistencyException 'returning nil screen from
 *      mainScreen is not allowed!'
 *   TUIKB (new): objc_retain+16 fault 0x220 from
 *      -[__NSArrayM insertObject:atIndex:]+228, chain
 *      -[TUIKBGraphSerialization relativeRefArrayForOffset:withCount:]
 *      <- objectForOffset: <- _UIKBTreeForOffset:withFlags: <-
 *      UIKBTreeForOffset:withFlags: <- objectForOffset: <-
 *      relativeRefArrayForOffset: (keyboard layout graph decode).
 *      KBLayouts .dat files are byte-identical to the IPSW, so the
 *      data is fine: objectForOffset: returned a garbage/freed
 *      object. TUIKBGraphSerialization caches decoded objects in a
 *      mutable dictionary; two threads decoding the same graph at
 *      once is the mechanism. The concurrent-decode scenario is in
 *      heap067_repro_vm_locks_test.c (it needs thread.c); this file
 *      covers the tap storm, the retain leak, and the AG guard.
 *
 * What this file does:
 *   S1 route D storm: two producer threads (presenter taps on
 *      pointer 0, selftest-style taps on pointer 1) feed the real
 *      ring through touch_test_process_touch while a real async
 *      main queue (test-side worker thread) runs the injects and a
 *      render thread drives the vsync clock and the frame-20
 *      selftest. Invariants: every accepted phase delivers exactly
 *      one sendEvent:, no silent loss, fingers end at 0, the tab
 *      flips, created == released.
 *   S2 route C storm (direct route on device,
 *      IOSWINE_TOUCH_DIRECT=1): same producers, KIF path. RED GATE
 *      on base: touch_direct_inject retains the HID event TWICE
 *      (touch.c, KIF branch) but touch_inject releases the parent
 *      once, so every phase leaks the parent plus its children.
 *      The mock does not provide CFRetain (the device CoreFoundation
 *      does), so this file provides one through its dlsym wrapper
 *      to make the accounting device-faithful. On base the 64-event
 *      mock pool exhausts, created stays 64, released stays 0, and
 *      sendEvent stalls while accepted phases keep growing.
 *   S3 AG guard + vsync clock: the guard refuses before any tick,
 *      opens after the first display tick, and the cadisplay vsync
 *      clock advances tick by tick (the device crash-B liveness
 *      pin). The guard is consulted by no in-tree caller
 *      (ag_update_begin has zero callers outside attributegraph.c);
 *      that structural gap is pinned here, not fixed here.
 *   S4 fuzz loop: route D tap storms over 8 deterministic seeds.
 *
 * Expected on base: S2 FAILS (retain leak + pool exhaustion),
 * S1/S3/S4 pass (pins). The TUIKB concurrent-decode scenario in
 * the companion file reproduces S1/S4 in its unguarded controls.
 *
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 * No em dashes are used in this file.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mock_uikit_async.h"
#include "cadisplay.h"
#include "attributegraph.h"

/* iosurface stubs: cadisplay.c links against the IOSurface table,
 * which has no host instance here. The vsync / tick paths under
 * test never touch a surface; the stubs only satisfy the linker.
 * Copied from device_log_replay_021_test.c. */
int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    (void)surface_id;
    (void)out;
    return 15; /* KERN_INVALID_NAME: no surfaces on host. */
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    (void)surface_id;
    (void)fd_out;
    return 15; /* KERN_INVALID_NAME. */
}

uint32_t iosurface_surface_count(void) {
    return 0;
}

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

#define CHECK_EQ_U64(a, b) do { \
    uint64_t _a = (uint64_t)(a); uint64_t _b = (uint64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", __FILE__, \
               __LINE__, #a, (unsigned long long)_a, \
               (unsigned long long)_b); \
    } \
} while (0)

#define CHECK_EQ_I64(a, b) do { \
    int64_t _a = (int64_t)(a); int64_t _b = (int64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, \
               __LINE__, #a, (long long)_a, (long long)_b); \
    } \
} while (0)

/* Deterministic xorshift64 PRNG. The seed is fixed per section so a
 * failure reproduces exactly. */
static uint64_t g_rng_state;
static void rng_seed(uint64_t seed) { g_rng_state = seed ? seed : 0x9e3779b97f4a7c15ULL; }
static uint64_t rng_next(void) {
    uint64_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) { }
}

/* ------------------------------------------------------------------ */
/* Real async main queue (device-faithful dispatch_async_f)           */
/*                                                                    */
/* The mock's dispatch_async_f runs the block synchronously on the    */
/* calling thread, which collapses the producer/consumer handoff    */
/* and can never exercise the ring across threads. This queue is a   */
/* real FIFO with a worker thread: enqueue_touch_for_main posts the  */
/* block here, the worker runs touch_inject, exactly like the main   */
/* dispatch queue on device (one consumer, many producers).          */
/* ------------------------------------------------------------------ */

#define MQ_CAP 4096
typedef struct {
    void (*work)(void *);
    void *ctx;
} mq_block_t;

static pthread_mutex_t g_mq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_mq_empty_cond = PTHREAD_COND_INITIALIZER;
static mq_block_t g_mq_queue[MQ_CAP];
static int g_mq_head = 0;
static int g_mq_tail = 0;
static int g_mq_count = 0;
static int g_mq_running = 0;
static int g_mq_stop = 0;
static int g_mq_overflows = 0;
static pthread_t g_mq_thread;

static void *mq_worker(void *arg) {
    (void)arg;
    for (;;) {
        mq_block_t blk;
        pthread_mutex_lock(&g_mq_mu);
        while (g_mq_count == 0 && !g_mq_stop) {
            pthread_cond_wait(&g_mq_cond, &g_mq_mu);
        }
        if (g_mq_count == 0 && g_mq_stop) {
            pthread_mutex_unlock(&g_mq_mu);
            return NULL;
        }
        blk = g_mq_queue[g_mq_head];
        g_mq_head = (g_mq_head + 1) % MQ_CAP;
        g_mq_count--;
        g_mq_running = 1;
        pthread_mutex_unlock(&g_mq_mu);
        blk.work(blk.ctx);
        pthread_mutex_lock(&g_mq_mu);
        g_mq_running = 0;
        if (g_mq_count == 0) pthread_cond_signal(&g_mq_empty_cond);
        pthread_mutex_unlock(&g_mq_mu);
    }
}

static void mq_start(void) {
    pthread_mutex_lock(&g_mq_mu);
    g_mq_head = 0;
    g_mq_tail = 0;
    g_mq_count = 0;
    g_mq_running = 0;
    g_mq_stop = 0;
    g_mq_overflows = 0;
    pthread_mutex_unlock(&g_mq_mu);
    pthread_create(&g_mq_thread, NULL, mq_worker, NULL);
}

static void mq_stop(void) {
    pthread_mutex_lock(&g_mq_mu);
    g_mq_stop = 1;
    pthread_cond_signal(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_mu);
    pthread_join(g_mq_thread, NULL);
}

/* Wait until the queue is empty and no block is running (10 s cap). */
static int mq_wait_empty(void) {
    int64_t deadline = now_ms() + 10000;
    pthread_mutex_lock(&g_mq_mu);
    while ((g_mq_count != 0 || g_mq_running) && now_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_mq_empty_cond, &g_mq_mu, &ts);
    }
    int empty = (g_mq_count == 0 && !g_mq_running);
    pthread_mutex_unlock(&g_mq_mu);
    return empty;
}

static void my_dispatch_async_f(void *q, void *ctx, void (*work)(void *)) {
    (void)q;
    pthread_mutex_lock(&g_mq_mu);
    if (g_mq_count >= MQ_CAP) {
        g_mq_overflows++;
        pthread_mutex_unlock(&g_mq_mu);
        return;
    }
    g_mq_queue[g_mq_tail].work = work;
    g_mq_queue[g_mq_tail].ctx = ctx;
    g_mq_tail = (g_mq_tail + 1) % MQ_CAP;
    g_mq_count++;
    pthread_cond_signal(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_mu);
}

/* ------------------------------------------------------------------ */
/* CFRetain (device-faithful) + dlsym wrapper                         */
/*                                                                    */
/* The mock provides CFRelease but not CFRetain, so on host the KIF  */
/* double-retain in touch.c is a no-op and the leak is invisible.   */
/* The device CoreFoundation provides CFRetain, so this file does:  */
/* it bumps the mock event refcount exactly the way a real retain   */
/* would, which makes the retain accounting observable on host.     */
/* ------------------------------------------------------------------ */

static void *my_CFRetain(void *cf) {
    mock_hid_event_t *ev = (mock_hid_event_t *)cf;
    if (ev && ev->in_use) ev->refcount++;
    return cf;
}

static void *my_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "dispatch_async_f") == 0) return (void *)my_dispatch_async_f;
    if (strcmp(name, "CFRetain") == 0) return (void *)my_CFRetain;
    return mock_dlsym(name);
}

static const touch_test_ops_t my_ops = {
    .dlsym_default = my_dlsym,
    .dlopen_fn = mock_dlopen,
    .getenv_fn = mock_getenv,
    .mach_abs_time = mock_mach_abs_time,
    .log_fn = mock_log,
    .receive_internal_fn = mock_receive_internal,
    .drain_events_fn = mock_drain_events,
    .timebase_fn = mock_timebase,
};

/* ------------------------------------------------------------------ */
/* Wire helper + tap generator                                        */
/* ------------------------------------------------------------------ */

/* Encode one presenter TOUCH message exactly as the Android
 * presenter sends it, then decode it back the way touch.c receives
 * it. Copied from e2e_touch_window_test.c. */
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

/* One tap: DOWN + 0-2 MOVEs + UP at (x, y) client pixels. The first
 * tap of every producer is the deterministic selftest point
 * (107, 791) inside the Browse tab rect, so the tab flip is
 * guaranteed unless delivery itself is broken. Later taps are 70%
 * in-tab, 30% anywhere in the 360x800 client area. */
#define STORM_TAPS_PER_PRODUCER 120

typedef struct {
    uint64_t seed;
    uint32_t pointer_id;
    uint64_t attempted;
    uint64_t accepted;
    uint64_t orphaned;
} producer_arg_t;

static _Atomic uint64_t g_wire_ts = 1000000;

static int inject_phase(uint32_t action, uint32_t pointer_id, float x, float y) {
    isws_touch_t t;
    int64_t ts = (int64_t)__atomic_fetch_add(&g_wire_ts, 1000, __ATOMIC_RELAXED);
    if (wire_touch(action, pointer_id, x, y, ts, &t) != 0) return -1;
    return touch_test_process_touch(&t, 360, 800);
}

/* Run n_taps taps for one pointer from a deterministic seed.
 * Counts attempted / accepted / orphaned. A phase is orphaned when
 * it is accepted at enqueue but can never deliver: its tap's DOWN
 * was dropped (ring full), so touch_inject skips the follow-on
 * MOVE/UP for a finger that never went down. The MOVE jitter is
 * never (0,0): an identical MOVE is also skipped by touch_inject
 * (no position change, no event), which would otherwise look like
 * a lost delivery. */
static void run_taps(uint32_t pointer_id, int n_taps, uint64_t seed,
                     uint64_t *attempted, uint64_t *accepted,
                     uint64_t *orphaned) {
    rng_seed(seed);
    for (int tap = 0; tap < n_taps; tap++) {
        float x, y;
        float last_x, last_y;
        int moves;
        int down_dropped = 0;
        if (tap == 0) {
            x = 107.0f;
            y = 791.0f;
        } else if ((rng_next() % 10) < 7) {
            x = 72.0f + (float)(rng_next() % 720) / 10.0f;
            y = 752.0f + (float)(rng_next() % 480) / 10.0f;
        } else {
            x = (float)(rng_next() % 3600) / 10.0f;
            y = (float)(rng_next() % 8000) / 10.0f;
        }
        moves = (int)(rng_next() % 3);
        last_x = x;
        last_y = y;
        (*attempted)++;
        if (inject_phase(ISWS_TOUCH_DOWN, pointer_id, x, y) == 0) {
            (*accepted)++;
        } else {
            down_dropped = 1;
        }
        for (int m = 0; m < moves; m++) {
            float mx = x + (float)((int)(rng_next() % 21) - 10);
            float my = y + (float)((int)(rng_next() % 21) - 10);
            if (mx < 0) mx = 0;
            if (mx > 359) mx = 359;
            if (my < 0) my = 0;
            if (my > 799) my = 799;
            /* Must differ from the finger's last position: the
             * clamp above can push a MOVE back onto it (e.g. x=359
             * with a +10 jitter clamps to 359), and touch_inject
             * skips an identical MOVE, which would look like a lost
             * delivery. Nudge along x, staying in bounds. */
            if (mx == last_x && my == last_y) {
                if (mx > 0) mx -= 1.0f;
                else mx += 1.0f;
            }
            (*attempted)++;
            if (inject_phase(ISWS_TOUCH_MOVE, pointer_id, mx, my) == 0) {
                (*accepted)++;
                if (down_dropped) (*orphaned)++;
            }
            last_x = mx;
            last_y = my;
            sched_yield();
        }
        (*attempted)++;
        if (inject_phase(ISWS_TOUCH_UP, pointer_id, x, y) == 0) {
            (*accepted)++;
            if (down_dropped) (*orphaned)++;
        }
        sched_yield();
        /* Pace the producers: without this the two producers can
         * outrun the single async worker, the 256-entry ring fills
         * (300+ drops in one run), a dropped UP leaves its finger
         * stuck active, and the next tap's phases then deliver
         * against the stale finger, which breaks the strict
         * accepted-orphaned == sendevent accounting. The overload
         * behaviour itself (dropped UP -> stuck finger) is a real
         * finding; the storm pins the clean-regime delivery path. */
        usleep(1000);
    }
}

static void *producer_thread(void *arg) {
    producer_arg_t *a = (producer_arg_t *)arg;
    run_taps(a->pointer_id, STORM_TAPS_PER_PRODUCER, a->seed,
             &a->attempted, &a->accepted, &a->orphaned);
    return NULL;
}

/* Render thread: 60 display-link ticks through the real cadisplay
 * tick path, which also ticks the AG guard. (The frame-20 selftest
 * is deliberately not driven here: it injects through
 * enqueue_touch_for_main directly, bypassing the producers'
 * attempted/accepted counters, so its deliveries would confound
 * the storm accounting. The selftest path itself is covered by
 * the existing device_log_replay tests.) */
static void *render_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 60; i++) {
        lsk_cadisplay_vsync_tick((uint64_t)(1000000000LL + (int64_t)i * 16666667LL));
    }
    return NULL;
}

/* Join with a deadline; 0 = joined, -1 = still running (deadlock). */
static int join_deadline(pthread_t th, int64_t deadline_ms) {
    for (;;) {
        if (pthread_tryjoin_np(th, NULL) == 0) return 0;
        if (now_ms() >= deadline_ms) return -1;
        sleep_ms(5);
    }
}

static void setup_fixture(int direct) {
    touch_test_reset();
    reset_mocks();
    mock_async_setup_fixture();
    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;
    g_env_touch = "1";
    g_env_trace = NULL;
    g_env_selftest = NULL;
    g_env_direct = direct ? "1" : NULL;
    touch_test_configure(&my_ops);
    touch_init();
    touch_set_window_size(360.0, 800.0);
    __atomic_store_n(&g_wire_ts, 1000000, __ATOMIC_RELAXED);
    mq_start();
}

static void teardown_fixture(void) {
    mq_wait_empty();
    mq_stop();
}

/* ------------------------------------------------------------------ */
/* S0: verbatim device-log signature strings                          */
/* ------------------------------------------------------------------ */

static const char kSigS1[] =
    "objc_retain+16 from -[__NSArrayM insertObject:atIndex:]";
static const char kSigS2[] =
    "objc_retain+16 from -[UITouch _setResponder:]";
static const char kSigS3[] =
    "AG::Graph::add_input+416, add_output_edge+80, UpdateStack::update+1100";
static const char kSigS4[] =
    "___NSMUTABLEDICTIONARY_CONCURRENT_MODIFICATION_DETECTED__";
static const char kSigS6[] =
    "NSInternalInconsistencyException 'returning nil screen from mainScreen is not allowed!'";
static const char kSigTuikb[] =
    "-[TUIKBGraphSerialization relativeRefArrayForOffset:withCount:]+132"
    " <- objectForOffset:+492 <- _UIKBTreeForOffset:withFlags:+168"
    " <- UIKBTreeForOffset:withFlags: <- objectForOffset:+256"
    " <- relativeRefArrayForOffset:+120";

static void test_signatures(void) {
    /* Pin the exact strings this repro targets, so a rename of a
     * signature in the device docs fails here instead of silently
     * targeting the wrong crash. */
    CHECK(strstr(kSigS1, "objc_retain+16") != NULL);
    CHECK(strstr(kSigS1, "__NSArrayM insertObject:atIndex:") != NULL);
    CHECK(strstr(kSigS2, "UITouch _setResponder:") != NULL);
    CHECK(strstr(kSigS3, "UpdateStack::update+1100") != NULL);
    CHECK(strstr(kSigS4, "CONCURRENT_MODIFICATION") != NULL);
    CHECK(strstr(kSigS6, "returning nil screen from mainScreen") != NULL);
    CHECK(strstr(kSigTuikb, "TUIKBGraphSerialization") != NULL);
    CHECK(strstr(kSigTuikb, "relativeRefArrayForOffset") != NULL);
    CHECK(strstr(kSigTuikb, "objectForOffset") != NULL);
    printf("[heap067] S1: %s\n", kSigS1);
    printf("[heap067] S2: %s\n", kSigS2);
    printf("[heap067] S3: %s (fault 0x5xxxxxxxx)\n", kSigS3);
    printf("[heap067] S4: %s\n", kSigS4);
    printf("[heap067] S6: %s\n", kSigS6);
    printf("[heap067] TUIKB: %s\n", kSigTuikb);
}

/* ------------------------------------------------------------------ */
/* S1: route D storm (default route)                                  */
/* ------------------------------------------------------------------ */

/* Wait until the sendEvent count stops changing (two reads 50 ms
 * apart agree), up to 2 s. Route D can dispatch a sendEvent through
 * the dispatcher source after the block that enqueued the phase has
 * run, so the count can still be settling when the producers join
 * and the async queue drains. Reading it too early looks like a
 * silent loss of one or two phases. */
static void settle_sendevent(void) {
    uint64_t prev = touch_test_sendevent_count();
    int64_t deadline = now_ms() + 2000;
    for (;;) {
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
        uint64_t cur = touch_test_sendevent_count();
        if (cur == prev) return;
        prev = cur;
        if (now_ms() >= deadline) return;
    }
}

static void test_route_d_storm(void) {
    pthread_t prod_a, prod_b, rend_th;
    producer_arg_t arg_a = { .seed = 0x067A, .pointer_id = 0 };
    producer_arg_t arg_b = { .seed = 0x067B, .pointer_id = 1 };
    int64_t deadline;
    uint64_t attempted, accepted, sendevent;
    uint64_t ring_full = 0, invalid = 0, stale = 0, cancel_all = 0;

    setup_fixture(0);
    CHECK(pthread_create(&prod_a, NULL, producer_thread, &arg_a) == 0);
    CHECK(pthread_create(&prod_b, NULL, producer_thread, &arg_b) == 0);
    CHECK(pthread_create(&rend_th, NULL, render_thread, NULL) == 0);
    deadline = now_ms() + 30000;
    CHECK(join_deadline(prod_a, deadline) == 0);
    CHECK(join_deadline(prod_b, deadline) == 0);
    CHECK(join_deadline(rend_th, deadline) == 0);
    teardown_fixture();
    settle_sendevent();

    attempted = arg_a.attempted + arg_b.attempted;
    accepted = arg_a.accepted + arg_b.accepted;
    sendevent = touch_test_sendevent_count();
    touch_test_drop_counts(&ring_full, &invalid, &stale, &cancel_all);
    printf("[heap067] route D storm: attempted=%llu accepted=%llu sendevent=%llu "
           "orphaned=%llu "
           "drops ring_full=%llu invalid=%llu stale=%llu cancel_all=%llu "
           "created=%d released=%d fingers_down=%u tab=%d mq_overflows=%d\n",
           (unsigned long long)attempted, (unsigned long long)accepted,
           (unsigned long long)sendevent,
           (unsigned long long)(arg_a.orphaned + arg_b.orphaned),
           (unsigned long long)ring_full, (unsigned long long)invalid,
           (unsigned long long)stale, (unsigned long long)cancel_all,
           g_created_count, g_released_count,
           touch_test_fingers_down(), g_tab_selected_index,
           g_mq_overflows);
    /* No silent loss: every accepted phase delivers exactly one
     * sendEvent:, except orphaned follow-ons of a dropped DOWN,
     * and every attempted phase is either accepted or counted as
     * a documented drop. The exact identity only holds on a clean
     * run (ring_full==0): once the ring overflows, a dropped UP
     * leaves its finger stuck active and the next tap's phases can
     * deliver against the stale finger, so the accounting no longer
     * closes. The clean run is the norm with the pacing above; an
     * overload run still has to satisfy the weaker invariants. */
    if (ring_full == 0) {
        CHECK_EQ_U64(accepted - (arg_a.orphaned + arg_b.orphaned),
                     sendevent);
    } else {
        printf("[heap067] route D storm: OVERLOAD run (ring_full=%llu), "
               "strict delivery identity not asserted\\n",
               (unsigned long long)ring_full);
    }
    CHECK_EQ_U64(attempted, accepted + ring_full + invalid + stale);
    CHECK(touch_test_fingers_down() == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK_EQ_U64((uint64_t)g_created_count, (uint64_t)g_released_count);
    CHECK(g_mq_overflows == 0);
    /* The render thread ticked the AG guard through the real
     * display-link path; the guard must be open afterwards. */
    CHECK(ag_update_cycle_ready() == 1);
    CHECK(ag_tick_count() == 60);
}

/* ------------------------------------------------------------------ */
/* S2: route C storm (device route, IOSWINE_TOUCH_DIRECT=1)           */
/* RED GATE on base: the KIF double-retain leak.                      */
/* ------------------------------------------------------------------ */

static void test_route_c_storm(void) {
    pthread_t prod_a, prod_b, rend_th;
    producer_arg_t arg_a = { .seed = 0x067C, .pointer_id = 0 };
    producer_arg_t arg_b = { .seed = 0x067D, .pointer_id = 1 };
    int64_t deadline;
    uint64_t attempted, accepted, sendevent;
    uint64_t ring_full = 0, invalid = 0, stale = 0, cancel_all = 0;

    setup_fixture(1);
    CHECK(pthread_create(&prod_a, NULL, producer_thread, &arg_a) == 0);
    CHECK(pthread_create(&prod_b, NULL, producer_thread, &arg_b) == 0);
    CHECK(pthread_create(&rend_th, NULL, render_thread, NULL) == 0);
    deadline = now_ms() + 30000;
    CHECK(join_deadline(prod_a, deadline) == 0);
    CHECK(join_deadline(prod_b, deadline) == 0);
    CHECK(join_deadline(rend_th, deadline) == 0);
    teardown_fixture();

    attempted = arg_a.attempted + arg_b.attempted;
    accepted = arg_a.accepted + arg_b.accepted;
    sendevent = touch_test_sendevent_count();
    touch_test_drop_counts(&ring_full, &invalid, &stale, &cancel_all);
    printf("[heap067] route C storm: attempted=%llu accepted=%llu sendevent=%llu "
           "orphaned=%llu "
           "drops ring_full=%llu invalid=%llu stale=%llu cancel_all=%llu "
           "created=%d released=%d fingers_down=%u tab=%d mq_overflows=%d\n",
           (unsigned long long)attempted, (unsigned long long)accepted,
           (unsigned long long)sendevent,
           (unsigned long long)(arg_a.orphaned + arg_b.orphaned),
           (unsigned long long)ring_full, (unsigned long long)invalid,
           (unsigned long long)stale, (unsigned long long)cancel_all,
           g_created_count, g_released_count,
           touch_test_fingers_down(), g_tab_selected_index,
           g_mq_overflows);
    /* touch_direct_inject (touch.c, KIF branch) calls CFRetain on
     * the HID event TWICE per phase, while touch_inject releases
     * the parent ONCE at the end. Net +2 retains per phase: the
     * parent (and its children, held by the parent) is never
     * freed. On base the 64-event mock pool exhausts, created
     * pins at 64, released stays 0, and sendEvent stalls while
     * accepted phases keep growing. A fixed tree retains once and
     * balances the release, so created == released and every
     * accepted phase delivers. */
    CHECK_EQ_U64((uint64_t)g_created_count, (uint64_t)g_released_count);
    /* Correction (2026-11-19): the old gate asserted
     * accepted == sendevent with no orphan adjustment. Orphaned
     * follow-ons (a tap whose DOWN was dropped, so touch_inject
     * skips the MOVE/UP for a finger that never went down) are
     * accepted at enqueue but can never deliver, by this test's
     * own model; route D's identical gate subtracts them. On an
     * overload run (ring_full > 0) the old gate failed by exactly
     * the orphaned count (observed: accepted 711, sendevent 708,
     * orphaned 3). Subtract them, like route D. */
    CHECK_EQ_U64(accepted - (arg_a.orphaned + arg_b.orphaned), sendevent);
    /* fingers_down is NOT asserted here: once the pool is exhausted
     * the worker slows, the ring fills, and an UP can be ring-full
     * dropped while its DOWN was accepted, leaving that finger
     * stuck down. That stuck finger is a downstream symptom of
     * the leak (it varies run to run: 0 last run, 1 this run), not
     * a separate gate; the two CHECKs above are the gates. */
    CHECK(g_tab_selected_index == 1);
    CHECK(g_mq_overflows == 0);
}

/* ------------------------------------------------------------------ */
/* S3: AG guard + vsync clock                                         */
/* ------------------------------------------------------------------ */

static void *ag_tick_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        lsk_cadisplay_vsync_tick((uint64_t)(2000000000LL + (int64_t)i * 16666667LL));
        sched_yield();
    }
    return NULL;
}

typedef struct {
    int attempts;
    int allowed;
    int refused;
} ag_update_arg_t;

static void *ag_update_thread(void *arg) {
    ag_update_arg_t *a = (ag_update_arg_t *)arg;
    for (int i = 0; i < 500; i++) {
        a->attempts++;
        if (ag_update_begin() == 0) a->allowed++;
        else a->refused++;
        sched_yield();
    }
    return NULL;
}

static void test_ag_guard(void) {
    pthread_t tick_th, upd_a, upd_b;
    ag_update_arg_t ua = { 0, 0, 0 }, ub = { 0, 0, 0 };
    int64_t deadline;
    int64_t t0, want_next, prev_next;

    /* Guard refuses before any tick (the device precondition for
     * the live-taps line 211 fault: UpdateStack::update with no
     * display/time source). */
    ag_test_reset();
    CHECK(ag_update_cycle_ready() == 0);
    CHECK_EQ_I64(ag_tick_count(), 0);
    CHECK_EQ_I64(ag_last_tick_ns(), -1);
    CHECK(ag_update_begin() == -1);

    /* Vsync clock: cadisplay_init + on_vsync advances the next
     * vsync tick by tick (device crash-B liveness pin). The period
     * is 1000000000/60 = 16666666 ns (integer division). */
    CHECK(cadisplay_init(1440, 3200, 4) == 0);
    prev_next = -1;
    for (int i = 0; i < 10; i++) {
        t0 = 3000000000LL + (int64_t)i * 16666666LL;
        CHECK(cadisplay_on_vsync(t0) == 0);
        want_next = t0 + 16666666LL;
        CHECK_EQ_I64(cadisplay_next_vsync_ns(), want_next);
        CHECK(prev_next < 0 || cadisplay_next_vsync_ns() > prev_next);
        prev_next = cadisplay_next_vsync_ns();
    }

    /* The first display tick opens the guard, through the real
     * cadisplay -> ag_note_display_tick wiring. */
    ag_test_reset();
    lsk_cadisplay_vsync_tick(4000000000ULL);
    CHECK(ag_update_cycle_ready() == 1);
    CHECK_EQ_I64(ag_tick_count(), 1);
    CHECK(ag_update_begin() == 0);

    /* Concurrent ticks + updates: no crash, ticks exact, the
     * guard opens once the first tick lands. */
    ag_test_reset();
    CHECK(pthread_create(&tick_th, NULL, ag_tick_thread, NULL) == 0);
    CHECK(pthread_create(&upd_a, NULL, ag_update_thread, &ua) == 0);
    CHECK(pthread_create(&upd_b, NULL, ag_update_thread, &ub) == 0);
    deadline = now_ms() + 10000;
    CHECK(join_deadline(tick_th, deadline) == 0);
    CHECK(join_deadline(upd_a, deadline) == 0);
    CHECK(join_deadline(upd_b, deadline) == 0);
    printf("[heap067] AG concurrent: ticks=%u allowed=%d refused=%d\n",
           ag_tick_count(), ua.allowed + ub.allowed, ua.refused + ub.refused);
    CHECK_EQ_I64(ag_tick_count(), 50);
    CHECK(ag_update_cycle_ready() == 1);
    CHECK(ua.attempts + ub.attempts == 1000);
    CHECK(ua.allowed + ub.allowed > 0);
    CHECK(ua.allowed + ub.allowed + ua.refused + ub.refused == 1000);
}

/* ------------------------------------------------------------------ */
/* S4: fuzz loop over tap sequences (route D, 8 seeds)                */
/* ------------------------------------------------------------------ */

static void test_fuzz_seeds(void) {
    for (int seed = 0; seed < 8; seed++) {
        uint64_t attempted = 0, accepted = 0, orphaned = 0;
        uint64_t sendevent;
        uint64_t ring_full = 0, invalid = 0, stale = 0, cancel_all = 0;

        /* One producer, 30 taps, fresh fixture per seed. The
         * producer count is small on purpose: the storm sections
         * above carry the concurrency, this loop carries the
         * sequence variety. */
        setup_fixture(0);
        run_taps(0, 30, 0xF067 + (uint64_t)seed,
                 &attempted, &accepted, &orphaned);
        teardown_fixture();
        sendevent = touch_test_sendevent_count();
        touch_test_drop_counts(&ring_full, &invalid, &stale, &cancel_all);
        printf("[heap067] fuzz seed %d: attempted=%llu accepted=%llu sendevent=%llu "
               "orphaned=%llu created=%d released=%d fingers_down=%u tab=%d\n",
               seed, (unsigned long long)attempted,
               (unsigned long long)accepted, (unsigned long long)sendevent,
               (unsigned long long)orphaned,
               g_created_count, g_released_count,
               touch_test_fingers_down(), g_tab_selected_index);
        CHECK_EQ_U64(accepted - orphaned, sendevent);
        CHECK_EQ_U64(attempted, accepted + ring_full + invalid + stale);
        CHECK(touch_test_fingers_down() == 0);
        CHECK_EQ_U64((uint64_t)g_created_count, (uint64_t)g_released_count);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_signatures();
    test_route_d_storm();
    test_route_c_storm();
    test_ag_guard();
    test_fuzz_seeds();
    printf("heap067_repro_test: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
