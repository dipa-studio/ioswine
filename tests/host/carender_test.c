/*
 * carender_test.c - Host test for the carender engine.
 *
 * Drives carender_step() with fake ops: a fake 60 Hz vsync clock,
 * a fake CA surface whose seed the test moves, a fake snapshot
 * renderer with a configurable fake duration, and a fake
 * forward_surface that can be made to fail (both buffers busy).
 * Simulates commit bursts (60 commits/s for 5 s), idle stretches,
 * slow renders (rate step-down and step-up), coalescing, damage
 * delivery, vsync timeouts and vsync errors.
 *
 * Plain cc + ASan/UBSan. Prints "N checks, F failures" last.
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "carender.h"
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

/* ------------------------------------------------------------------ */
/* Fake backend                                                       */
/* ------------------------------------------------------------------ */

#define FAKE_PERIOD_NS 16666667LL

static int64_t fake_time_ns;
static uint32_t fake_surface_id;   /* 0 = no CA surface */
static uint32_t fake_seed;
static int64_t fake_render_ns;     /* fake snapshot render duration */
static int64_t fake_forward_ns;    /* fake CA forward duration */
static int fake_forward_fail_left; /* fail this many forwards, then ok */
static int fake_vsync_mode;        /* 0 = vsync, 1 = timeout, -1 = error */

static uint64_t fake_forwards;
static uint64_t fake_renders;
static uint32_t fake_forwarded_seeds[1024];
static uint32_t fake_forwarded_seed_count;
static int32_t fake_last_damage_x;
static int32_t fake_last_damage_y;
static uint32_t fake_last_damage_w;
static uint32_t fake_last_damage_h;
static uint32_t fake_last_damage_count;
static int fake_last_damage_was_null;

static int fake_fps_lines;       /* [fps] lines logged */
static char fake_fps_line[96];   /* last [fps] line, NUL-terminated */
static int fake_frame_lines;     /* [carender] frame lines logged */
static char fake_frame_line[128]; /* last [carender] frame line */

/* Tick records: every on_vsync call appends its
 * timestamp, so the self-clocked test can measure the tick rate
 * and the longest stall. Recording is harmless to the older
 * tests, which never read these. */
static uint64_t fake_tick_count;
static int64_t fake_tick_ns[2048];
static uint64_t fake_tick_recorded;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
} fake_rect_t;

static int fake_render_fail;   /* when 1, fake_render_snapshot fails */

static int64_t fake_now_ns(void) {
    return fake_time_ns;
}

static void fake_log(const char *msg, size_t len) {
    /* Record [fps] and [carender] frame lines for tests; discard the rest. */
    if (msg && len >= 5 && msg[0] == '[' && msg[1] == 'f' &&
        msg[2] == 'p' && msg[3] == 's' && msg[4] == ']') {
        size_t copy = len < sizeof(fake_fps_line) - 1 ? len : sizeof(fake_fps_line) - 1;
        memcpy(fake_fps_line, msg, copy);
        fake_fps_line[copy] = '\0';
        fake_fps_lines++;
    } else if (msg && len >= 17 && strncmp(msg, "[carender] frame ", 17) == 0) {
        size_t copy = len < sizeof(fake_frame_line) - 1 ? len : sizeof(fake_frame_line) - 1;
        memcpy(fake_frame_line, msg, copy);
        fake_frame_line[copy] = '\0';
        fake_frame_lines++;
    }
}

static const char *fake_getenv(const char *name) {
    (void)name;
    return NULL;
}

static int fake_wait_vsync(int64_t *vsync_ns_out, int timeout_ms) {
    (void)timeout_ms;
    if (fake_vsync_mode == 1) return 1;
    if (fake_vsync_mode == -1) return -1;
    fake_time_ns += FAKE_PERIOD_NS;
    if (vsync_ns_out) *vsync_ns_out = fake_time_ns;
    return 0;
}

static void fake_on_vsync(int64_t vsync_ns) {
    fake_tick_count++;
    if (fake_tick_recorded < 2048) {
        fake_tick_ns[fake_tick_recorded++] = vsync_ns;
    }
}

static uint32_t fake_find_surface(void) {
    return fake_surface_id;
}

static int fake_surface_seed(uint32_t surface_id, uint32_t *seed_out) {
    if (surface_id == 0 || surface_id != fake_surface_id) return -1;
    if (seed_out) *seed_out = fake_seed;
    return 0;
}

static int fake_forward_surface(uint32_t surface_id, const void *damage,
                                uint32_t damage_count) {
    const fake_rect_t *r;
    (void)surface_id;
    fake_time_ns += fake_forward_ns;
    if (fake_forward_fail_left > 0) {
        fake_forward_fail_left--;
        return -1;
    }
    fake_forwards++;
    if (fake_forwarded_seed_count < 1024) {
        fake_forwarded_seeds[fake_forwarded_seed_count++] = fake_seed;
    }
    fake_last_damage_count = damage_count;
    fake_last_damage_was_null = (damage == NULL) ? 1 : 0;
    if (damage != NULL && damage_count > 0) {
        r = (const fake_rect_t *)damage;
        fake_last_damage_x = r[0].x;
        fake_last_damage_y = r[0].y;
        fake_last_damage_w = r[0].w;
        fake_last_damage_h = r[0].h;
    }
    return 0;
}

static int fake_render_snapshot(void) {
    fake_time_ns += fake_render_ns;
    if (fake_render_fail) return -1;
    fake_renders++;
    return 0;
}

static const carender_ops_t g_ops = {
    fake_now_ns,
    fake_log,
    fake_getenv,
    fake_wait_vsync,
    fake_on_vsync,
    fake_find_surface,
    fake_surface_seed,
    fake_forward_surface,
    fake_render_snapshot
};

/* ------------------------------------------------------------------ */
/* Fake coalesced snapshot backend                      */
/* ------------------------------------------------------------------ */

/* Models the real snapshot.c coalescer: snapshot_request() records
 * the request and dispatches a tick only when none is queued; the
 * queued tick runs when the mock main queue releases it (after
 * fake_snap_release_after sleep_ms calls, or by hand through
 * fake_snap_run_queued()). fake_time_ns is the clock the wait
 * loop reads, so the 100 ms bound is deterministic. */
static uint64_t fake_snap_frame_no_v;
static int fake_snap_queued;
static uint64_t fake_snap_requests;
static uint64_t fake_snap_dispatches;
static uint64_t fake_snap_renders;
static int64_t fake_snap_release_after; /* -1 = the queue never releases */
static uint64_t fake_snap_sleep_calls;
static uint64_t fake_snap_wait_start;   /* sleep calls at dispatch time */

static void fake_snap_request(void) {
    fake_snap_requests++;
    if (!fake_snap_queued) {
        fake_snap_queued = 1;
        fake_snap_dispatches++;
        fake_snap_wait_start = fake_snap_sleep_calls;
    }
}

static uint64_t fake_snap_frame_no(void) {
    return fake_snap_frame_no_v;
}

static void fake_snap_run_queued(void) {
    if (!fake_snap_queued) return;
    fake_snap_queued = 0;
    fake_snap_frame_no_v++;
    fake_snap_renders++;
}

static void fake_snap_sleep_ms(int ms) {
    int i;
    for (i = 0; i < ms; i++) {
        fake_time_ns += 1000000LL;
        fake_snap_sleep_calls++;
        if (fake_snap_queued && fake_snap_release_after >= 0 &&
            (int64_t)(fake_snap_sleep_calls - fake_snap_wait_start) >=
                fake_snap_release_after) {
            fake_snap_run_queued();
        }
    }
}

/* Same nine backends as g_ops, plus the coalesced snapshot trio.
 * render_snapshot stays wired so the test can prove the engine
 * prefers the async path (fake_renders must stay 0 here). */
static const carender_ops_t g_ops_async = {
    fake_now_ns,
    fake_log,
    fake_getenv,
    fake_wait_vsync,
    fake_on_vsync,
    fake_find_surface,
    fake_surface_seed,
    fake_forward_surface,
    fake_render_snapshot,
    fake_snap_request,
    fake_snap_frame_no,
    fake_snap_sleep_ms
};

static void fake_reset(void) {
    fake_time_ns = 0;
    fake_surface_id = 0;
    fake_seed = 0;
    fake_render_ns = 5000000LL;   /* 5 ms: fast */
    fake_forward_ns = 1000000LL;  /* 1 ms */
    fake_forward_fail_left = 0;
    fake_vsync_mode = 0;
    fake_render_fail = 0;
    fake_forwards = 0;
    fake_renders = 0;
    fake_forwarded_seed_count = 0;
    fake_last_damage_x = 0;
    fake_last_damage_y = 0;
    fake_last_damage_w = 0;
    fake_last_damage_h = 0;
    fake_last_damage_count = 0;
    fake_last_damage_was_null = 1;
    fake_fps_lines = 0;
    fake_fps_line[0] = '\0';
    fake_frame_lines = 0;
    fake_frame_line[0] = '\0';
    fake_tick_count = 0;
    fake_tick_recorded = 0;
    carender_test_configure(&g_ops);
    carender_test_set_frame(1080, 2400, 3.0);
}

/* Clock sleep for the self-clocked tests: advances
 * the fake clock by the requested milliseconds, with no snapshot
 * side effects (unlike fake_snap_sleep_ms). */
static void fake_clock_sleep_ms(int ms) {
    if (ms > 0) fake_time_ns += (int64_t)ms * 1000000LL;
}

/* Same backends as g_ops, plus the clock sleep the wait-error
 * path needs. The snapshot trio stays NULL, so the engine uses
 * the legacy render_snapshot path. */
static const carender_ops_t g_ops_vsync = {
    fake_now_ns,
    fake_log,
    fake_getenv,
    fake_wait_vsync,
    fake_on_vsync,
    fake_find_surface,
    fake_surface_seed,
    fake_forward_surface,
    fake_render_snapshot,
    NULL,
    NULL,
    fake_clock_sleep_ms
};

static void fake_reset_vsync(void) {
    fake_reset();
    carender_test_configure(&g_ops_vsync);
    carender_test_set_frame(1080, 2400, 3.0);
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                          */
/* ------------------------------------------------------------------ */

/* A: 60 commits/s for 5 s, fast renders: one frame per vsync. */
static void test_burst(void) {
    int i;
    carender_stats_t st;
    fake_reset();
    for (i = 0; i < 300; i++) {
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(fake_renders == 300);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 300);
    CHECK(st.snap_frames == 300);
    CHECK(st.ca_frames == 0);
    CHECK(st.coalesced == 0);
    CHECK(st.divisor == 1);
    CHECK(carender_current_hz() == 60);
}

/* B: idle stretch: no commits, no surface change, no work. */
static void test_idle(void) {
    int i;
    carender_stats_t st;
    fake_reset();
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(fake_renders == 1);
    for (i = 0; i < 120; i++) {
        CHECK(carender_step() == 0);
    }
    CHECK(fake_renders == 1);
    CHECK(fake_forwards == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 1);
    CHECK(st.idle_vsyncs == 120);
}

/* C: slow renders step the rate down to 30 Hz, fast renders step
 * it back up to 60 Hz. */
static void test_rate_adapt(void) {
    int i;
    carender_stats_t st;
    fake_reset();
    fake_render_ns = 20000000LL; /* 20 ms: slow */
    for (i = 0; i < 120; i++) {
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(carender_stats(&st) == 0);
    CHECK(st.divisor == 2);
    CHECK(carender_current_hz() == 30);
    /* 8 acted at divisor 1, then every other vsync: about 64. */
    CHECK(st.frames >= 55 && st.frames <= 70);
    /* Fast again: 8 consecutive fast samples step back up. */
    fake_render_ns = 2000000LL; /* 2 ms: fast */
    for (i = 0; i < 40; i++) {
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(carender_stats(&st) == 0);
    CHECK(st.divisor == 1);
    CHECK(carender_current_hz() == 60);
}

/* D: CA path: the seed moves, the surface is forwarded; seeds are
 * forwarded in strictly increasing order, and a busy presenter
 * coalesces instead of queueing. */
static void test_ca_path(void) {
    int i;
    carender_stats_t st;
    fake_reset();
    fake_surface_id = 7;
    fake_seed = 100;
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 1);
    CHECK(fake_forwarded_seeds[0] == 100);
    /* Same seed again: nothing to do. */
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 1);
    /* Seed moves every vsync for 10 vsyncs. */
    for (i = 0; i < 10; i++) {
        fake_seed++;
        CHECK(carender_step() == 0);
    }
    CHECK(fake_forwards == 11);
    for (i = 1; i < 11; i++) {
        CHECK(fake_forwarded_seeds[i] == fake_forwarded_seeds[i - 1] + 1);
    }
    /* Busy presenter: 5 forwards fail, then it recovers. */
    fake_forward_fail_left = 5;
    for (i = 0; i < 5; i++) {
        fake_seed++;
        CHECK(carender_step() == 0);
    }
    CHECK(fake_forwards == 11);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.coalesced == 5);
    fake_seed++;
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 12);
    /* The recovered forward carries the latest seed, not a stale one. */
    CHECK(fake_forwarded_seeds[11] == fake_seed);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.ca_frames == 12);
    CHECK(fake_frame_lines == 12);
    CHECK(strncmp(fake_frame_line, "[carender] frame 12 src=ca surface=7 seed=116", 44) == 0);
}

/* E: damage delivery: with the flag on, a point rect arrives in
 * pixels; with the flag off, the forward carries no damage
 * (whole frame); overflow degrades to whole frame. */
static void test_damage(void) {
    fake_reset();
    fake_surface_id = 3;
    fake_seed = 1;
    carender_test_set_send_damage(1);
    carender_damage_add_points(10.0, 20.0, 100.0, 50.0);
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 1);
    CHECK(fake_last_damage_was_null == 0);
    CHECK(fake_last_damage_count == 1);
    CHECK(fake_last_damage_x == 30 && fake_last_damage_y == 60);
    CHECK(fake_last_damage_w == 300 && fake_last_damage_h == 150);

    /* Flag off: whole frame. */
    fake_reset();
    fake_surface_id = 3;
    fake_seed = 2;
    carender_damage_add_points(10.0, 20.0, 100.0, 50.0);
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 1);
    CHECK(fake_last_damage_count == 0);
    CHECK(fake_last_damage_was_null == 1);

    /* Overflow: 17 disjoint point rects degrade to whole frame. */
    fake_reset();
    fake_surface_id = 3;
    fake_seed = 3;
    carender_test_set_send_damage(1);
    {
        int i;
        for (i = 0; i < 17; i++) {
            carender_damage_add_points((double)(i * 20), 0.0, 5.0, 5.0);
        }
    }
    CHECK(carender_step() == 0);
    CHECK(fake_forwards == 1);
    CHECK(fake_last_damage_count == 0);
}

/* F: vsync timeouts are counted, not fatal; a single vsync error
 * no longer stops the step (the engine survives a
 * session that is not live yet and still ticks). Only 30
 * continuous seconds of wait errors stop the step with -1. */
static void test_vsync_faults(void) {
    carender_stats_t st;
    fake_reset();
    fake_vsync_mode = 1;
    CHECK(carender_step() == 0);
    CHECK(carender_step() == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.timeouts == 2);
    fake_vsync_mode = 0;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(fake_renders == 1);
    fake_vsync_mode = -1;
    CHECK(carender_step() == 0);
    fake_time_ns += 31000000000LL; /* 31 s of dead session */
    CHECK(carender_step() == -1);
}

/* G: snapshot render failure coalesces like a busy presenter,
 * and the dirty flag survives so the next vsync retries. */
static void test_snap_failure(void) {
    carender_stats_t st;
    fake_reset();
    fake_render_fail = 1;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.coalesced == 1);
    CHECK(st.frames == 0);
    CHECK(fake_renders == 0);
    fake_render_fail = 0;
    CHECK(carender_step() == 0);
    CHECK(fake_renders == 1);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 1);
}

/* H: [fps] log line: once per second, form
 * "[fps] frames=<N> fps=<N>". Idle engine still logs (0 frames),
 * so a stalled engine is visible instead of silent. */
static void test_fps_log(void) {
    int i;
    carender_stats_t st;
    fake_reset();
    /* No commits: idle engine. 61 vsyncs at ~16.7 ms = just over 1 s. */
    for (i = 0; i < 61; i++) {
        CHECK(carender_step() == 0);
    }
    CHECK(fake_fps_lines >= 1);
    CHECK(strcmp(fake_fps_line, "[fps] frames=0 fps=0\n") == 0);
    /* Busy engine: one commit per vsync for another second. */
    fake_fps_lines = 0;
    for (i = 0; i < 61; i++) {
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(fake_fps_lines >= 1);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames > 0);
    /* The line reports the frames in the last second, both numbers
     * equal and positive on a busy engine. */
    {
        unsigned long long n1 = 0, n2 = 0;
        CHECK(sscanf(fake_fps_line, "[fps] frames=%llu fps=%llu", &n1, &n2) == 2);
        CHECK(n1 == n2);
        CHECK(n1 > 0);
    }
    CHECK(strncmp(fake_fps_line, "[fps] frames=", 13) == 0);
    CHECK(strstr(fake_fps_line, " fps=") != NULL);
}

/* I: coalesced snapshot fallback: the engine
 * requests one tick through the snapshot coalescer and waits
 * bounded 100 ms for the frame counter. While the main queue is
 * stalled, repeated dirty vsyncs issue requests but dispatch only
 * one tick, each wait times out at exactly 100 fake ms, and no
 * frame is counted. When the queue releases, the next dirty
 * vsync renders exactly once. */
static void test_snap_coalesced(void) {
    carender_stats_t st;
    int i;
    fake_reset();
    fake_snap_frame_no_v = 0;
    fake_snap_queued = 0;
    fake_snap_requests = 0;
    fake_snap_dispatches = 0;
    fake_snap_renders = 0;
    fake_snap_release_after = -1;
    fake_snap_sleep_calls = 0;
    fake_snap_wait_start = 0;
    carender_test_configure(&g_ops_async);
    carender_test_set_frame(1080, 2400, 3.0);

    /* Stalled main queue: 5 dirty vsyncs, 5 requests, 1 dispatch. */
    for (i = 0; i < 5; i++) {
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(fake_snap_requests == 5);
    CHECK(fake_snap_dispatches == 1);
    CHECK(fake_snap_renders == 0);
    CHECK(fake_snap_sleep_calls == 500);
    CHECK(fake_renders == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 0);
    CHECK(st.snap_frames == 0);
    CHECK(st.coalesced == 5);

    /* Release the one queued tick by hand, then a dirty vsync with
     * a 30 ms main-queue delay renders exactly once. */
    fake_snap_run_queued();
    CHECK(fake_snap_renders == 1);
    fake_snap_release_after = 30;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(fake_snap_requests == 6);
    CHECK(fake_snap_dispatches == 2);
    CHECK(fake_snap_renders == 2);
    CHECK(fake_snap_sleep_calls == 530);
    CHECK(fake_renders == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 1);
    CHECK(st.snap_frames == 1);
    CHECK(st.coalesced == 5);

    /* Immediate tick: one more dirty vsync renders at once. */
    fake_snap_release_after = 1;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(fake_snap_requests == 7);
    CHECK(fake_snap_dispatches == 3);
    CHECK(fake_snap_renders == 3);
    CHECK(fake_snap_sleep_calls == 531);
    CHECK(fake_renders == 0);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames == 2);
    CHECK(st.snap_frames == 2);
}

/* J: self-clocked vsync: with FRAME_DONE acks never
 * arriving (fake_vsync_mode = 1, the device reality under
 * IOSWINE_CARENDER=1), the engine still ticks on its own clock
 * and sustains 30+ fps over a simulated 10 s window with no
 * stall longer than 100 ms. The loop advances the fake clock by
 * 1 ms per iteration (10,000 iterations); renders add their 5 ms
 * on top, so the window is at least 10 s of simulated wall time.
 * On the unpatched tree this fails with zero frames: every step
 * just counts a timeout. */
static void test_vsync_self_clocked(void) {
    int i;
    carender_stats_t st;
    int64_t max_gap_ns = 0;
    fake_reset_vsync();
    fake_vsync_mode = 1; /* acks never arrive */
    fake_render_ns = 5000000LL; /* 5 ms: fast */
    for (i = 0; i < 10000; i++) {
        fake_time_ns += 1000000LL; /* 1 ms of wall time passes */
        carender_commit_notify();
        CHECK(carender_step() == 0);
    }
    CHECK(carender_stats(&st) == 0);
    CHECK(st.frames >= 300);
    CHECK(st.timeouts > 0);
    CHECK(fake_tick_count == st.vsyncs);
    CHECK(fake_tick_count >= 300);
    /* First tick is immediate (primed), no stall over 100 ms. */
    CHECK(fake_tick_recorded > 0);
    CHECK(fake_tick_ns[0] <= 100000000LL);
    for (i = 1; i < (int)fake_tick_recorded; i++) {
        int64_t gap = fake_tick_ns[i] - fake_tick_ns[i - 1];
        if (gap > max_gap_ns) max_gap_ns = gap;
    }
    CHECK(max_gap_ns <= 100000000LL);
    /* [fps] lines report the sustained rate. */
    CHECK(fake_fps_lines >= 5);
    {
        unsigned long long n1 = 0, n2 = 0;
        CHECK(sscanf(fake_fps_line, "[fps] frames=%llu fps=%llu", &n1, &n2) == 2);
        CHECK(n1 >= 30);
        CHECK(n1 == n2);
    }
}

/* K: vsync wait errors: a session that is not live
 * yet (the engine starts before main, the presenter session is
 * lazy) must not kill the engine. One error still ticks and
 * renders, so the first tick bootstraps the session through
 * present_init on device; a good wait resets the error run, so
 * 31 s of errors after a recovery must not stop the step. Only
 * 31 s of continuous errors stop it with -1. */
static void test_vsync_wait_error(void) {
    carender_stats_t st;
    fake_reset_vsync();
    fake_vsync_mode = -1; /* session not live */
    fake_render_ns = 5000000LL;
    /* One error: the step survives and still renders. */
    carender_commit_notify();
    CHECK(carender_step() == 0);
    CHECK(fake_renders == 1);
    CHECK(carender_stats(&st) == 0);
    CHECK(st.timeouts >= 1);
    /* Continuous errors for 31 s: stops with -1. */
    fake_time_ns += 31000000000LL;
    CHECK(carender_step() == -1);
    /* Recovery resets the run: one error, one good wait, then
     * 31 s of errors must not stop the step. */
    fake_reset_vsync();
    fake_vsync_mode = -1;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    fake_vsync_mode = 0;
    carender_commit_notify();
    CHECK(carender_step() == 0);
    fake_time_ns += 31000000000LL;
    fake_vsync_mode = -1;
    CHECK(carender_step() == 0);

    /* Before any frame is presented (boot wait), > 30 s of vsync errors
     * must not kill the engine. */
    fake_reset_vsync();
    fake_vsync_mode = -1;
    CHECK(carender_step() == 0);
    fake_time_ns += 60000000000LL;
    CHECK(carender_step() == 0);
}

int main(void) {
    test_burst();
    test_idle();
    test_rate_adapt();
    test_ca_path();
    test_damage();
    test_vsync_faults();
    test_snap_failure();
    test_fps_log();
    test_snap_coalesced();
    test_vsync_self_clocked();
    test_vsync_wait_error();
    printf("carender_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
