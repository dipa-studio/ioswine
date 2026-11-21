/*
 * carender.c - CA-commit / display-link driven render engine.
 *
 * See carender.h for the
 * design. The engine thread keeps its own tick boundary at the
 * configured rate (self-clocked, because FRAME_DONE
 * is only ever sent as the ack of a PRESENT, so waiting for an
 * ack before the first tick deadlocks) and only produces a frame
 * when something changed: either the CA display surface seed
 * moved (the local render server composited a frame into our
 * IOSurface) or a CATransaction commit marked the layer tree
 * dirty (the snapshot fallback re-renders the windows with
 * renderInContext on the main thread). Idle vsyncs do no work at
 * all, which is the whole point: a static Library screen costs
 * zero renders, and a scrolling list renders at most once per
 * vsync.
 *
 * Rate adaptation: the engine measures each render/forward
 * duration and steps the divisor 1 -> 2 -> 3 (60 -> 30 -> 20 Hz)
 * after 8 consecutive slow samples, and back down after 8
 * consecutive fast samples. The divisor only skips work, it
 * never queues it: a skipped vsync keeps its dirty flag, so the
 * next acted vsync renders the latest state.
 *
 * Freestanding C11: no libc, no malloc, no __thread, no static
 * function-pointer tables. ObjC/CF symbols are resolved with
 * dlsym + lsk_sign_fn, following snapshot.c and touch.c.
 *
 * No em dashes are used in this file.
 */
#include "carender.h"

#ifndef CARENDER_HOST_TEST
#include "linux.h"
#include "present.h"
#include "cadisplay.h"
#include "snapshot.h"
extern const char *lsk_getenv(const char *name);
#endif

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache.
 * Same addresses snapshot.c and touch.c use. */
#define CA_GAPIS_ADDR         0x1ec954040ULL
#define CA_GAPIS_OBJ_DISC     0x63faULL
#define CA_SLOT_DLSYM         16ULL
#define CA_DIV_DLSYM          0x839dULL

/* isws_rect_t layout (protocol.h): x, y as int32, w, h as uint32. */
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
} carender_isws_rect_t;

typedef struct {
    const carender_ops_t *ops;
    int configured;
    int armed;
    uint32_t hz;
    uint32_t divisor;
    int64_t period_ns;
    cadamage_t damage;
    double scale;
    int dirty;
    volatile int commit_seen;
    uint32_t last_surface;
    uint32_t last_seed;
    int have_last;
    int64_t render_ema_ns;
    uint32_t slow_streak;
    uint32_t fast_streak;
    int send_damage;
    carender_stats_t st;
    /* [fps] log window: start time of the
     * current one-second window and the frame count at its start.
     * fps_window_start_ns == 0 means no window is open yet. */
    int64_t fps_window_start_ns;
    uint64_t fps_window_frames;
    /* Self-clocked tick: the next tick boundary on
     * the engine clock. 0 = unprimed: the first tick is immediate,
     * so the first render also bootstraps the presenter session
     * through present_init, which is lazy on the first present. */
    int64_t next_tick_ns;
    /* Start of the current run of consecutive vsync-wait errors,
     * or -1 when the last wait succeeded. A session that is not
     * live yet (engine starts before main) must not kill the
     * engine; only a session dead for CARENDER_WAIT_ERROR_DEATH_NS
     * stops the step with -1. */
    int64_t wait_error_since_ns;
    /* First timer tick already logged (once per engine lifetime). */
    int timer_tick_logged;
} carender_t;

static carender_t g_e;

/* Backend dispatch. On device the engine calls the ca_dev_*
 * backends directly through the if-chain below; under
 * CARENDER_HOST_TEST it calls the ops table installed by
 * carender_test_configure. The dispatch is an if-chain, not
 * a static function-pointer table: the arm64e linker rejects
 * signed pointers in static data. The ops struct remains the
 * host-test seam only; no production code builds one. */
#ifndef CARENDER_HOST_TEST
static int64_t ca_dev_now_ns(void);
static void ca_dev_log(const char *msg, size_t len);
static int ca_dev_wait_vsync(int64_t *vsync_ns_out, int timeout_ms);
static void ca_dev_on_vsync(int64_t vsync_ns);
static uint32_t ca_dev_find_surface(void);
static int ca_dev_surface_seed(uint32_t surface_id, uint32_t *seed_out);
static int ca_dev_forward_surface(uint32_t surface_id, const void *damage,
                                  uint32_t damage_count);
static int ca_dev_render_snapshot(void);
static void ca_dev_sleep_ms(int ms);
#endif

static void ca_ops_log(const char *msg, size_t len) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->log_fn) g_e.ops->log_fn(msg, len);
#else
    ca_dev_log(msg, len);
#endif
}

static int64_t ca_ops_now_ns(void) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->now_ns) return g_e.ops->now_ns();
    return 0;
#else
    return ca_dev_now_ns();
#endif
}

static int ca_ops_wait_vsync(int64_t *vsync_ns_out, int timeout_ms) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->wait_vsync) {
        return g_e.ops->wait_vsync(vsync_ns_out, timeout_ms);
    }
    return -1;
#else
    return ca_dev_wait_vsync(vsync_ns_out, timeout_ms);
#endif
}

static void ca_ops_on_vsync(int64_t vsync_ns) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->on_vsync) g_e.ops->on_vsync(vsync_ns);
#else
    ca_dev_on_vsync(vsync_ns);
#endif
}

static uint32_t ca_ops_find_surface(void) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->find_surface) return g_e.ops->find_surface();
    return 0;
#else
    return ca_dev_find_surface();
#endif
}

static int ca_ops_surface_seed(uint32_t surface_id, uint32_t *seed_out) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->surface_seed) {
        return g_e.ops->surface_seed(surface_id, seed_out);
    }
    return -1;
#else
    return ca_dev_surface_seed(surface_id, seed_out);
#endif
}

static int ca_ops_forward_surface(uint32_t surface_id, const void *damage,
                                  uint32_t damage_count) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->forward_surface) {
        return g_e.ops->forward_surface(surface_id, damage, damage_count);
    }
    return -1;
#else
    return ca_dev_forward_surface(surface_id, damage, damage_count);
#endif
}

static int64_t ca_now_ns(void);

/* Coalesced snapshot render. Request one tick
 * through the snapshot coalescer and wait (bounded, 100 ms) for
 * the frame counter to advance. The coalescer caps queued ticks
 * at one pending plus one running, so this can never stack ticks
 * on the main queue no matter how long the main thread stalls:
 * while the queue is stalled every call here just re-requests
 * (folded into the one pending tick) and times out, and when the
 * main thread resumes exactly one tick renders. The wait is
 * bounded by ca_now_ns() + 100 ms; sleep_ms_fn advances the clock
 * the test controls, so the bound is deterministic under the host
 * test. Returns 0 when a frame rendered, -1 on timeout or when
 * any primitive is missing. Shared by the device backend and the
 * host-test branch of ca_ops_render_snapshot below, so the wait
 * logic is tested, not just the device path. */
static int ca_snapshot_render_coalesced(void (*request)(void),
                                        uint64_t (*frame_no)(void),
                                        void (*sleep_ms_fn)(int)) {
    uint64_t before;
    int64_t deadline;
    if (!request || !frame_no || !sleep_ms_fn) return -1;
    before = frame_no();
    request();
    deadline = ca_now_ns() + 100000000LL; /* 100 ms bound */
    while (frame_no() == before) {
        if (ca_now_ns() >= deadline) return -1;
        sleep_ms_fn(1);
    }
    return 0;
}

static int ca_ops_render_snapshot(void) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->snapshot_request && g_e.ops->snapshot_frame_no &&
        g_e.ops->sleep_ms) {
        return ca_snapshot_render_coalesced(g_e.ops->snapshot_request,
                                            g_e.ops->snapshot_frame_no,
                                            g_e.ops->sleep_ms);
    }
    if (g_e.ops && g_e.ops->render_snapshot) {
        return g_e.ops->render_snapshot();
    }
    return -1;
#else
    return ca_dev_render_snapshot();
#endif
}

/* Sleep helper for the wait-error path in carender_step: paces the
 * engine loop to the tick boundary when the session is not live
 * (present_wait_vsync then returns -1 instantly, so without this
 * the thread would busy-spin). Host test: the ops sleep_ms when
 * present, a no-op when NULL. */
static void ca_ops_sleep_ms(int ms) {
#ifdef CARENDER_HOST_TEST
    if (g_e.ops && g_e.ops->sleep_ms) g_e.ops->sleep_ms(ms);
#else
    ca_dev_sleep_ms(ms);
#endif
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Device-only plain logger: every caller lives in the device
 * sections below, so the host-test build would warn it unused.
 * ca_strlen is its only caller, so it moves inside the guard too. */
#ifndef CARENDER_HOST_TEST
static size_t ca_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void ca_log(const char *s) {
    ca_ops_log(s, ca_strlen(s));
}
#endif

/* Plain-string logger usable in both builds (ca_log is device
 * only). Routes through the ops log_fn under CARENDER_HOST_TEST
 * and through ca_dev_log on device. */
static void ca_log_str(const char *s) {
    size_t n = 0;
    if (!s) return;
    while (s[n]) n++;
    ca_ops_log(s, n);
}

static void ca_log_u64(const char *prefix, uint64_t v) {
    char buf[96];
    char num[24];
    size_t pos = 0;
    size_t n = 0;
    uint64_t t = v;
    if (v == 0) {
        num[n++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (t > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (rn > 0) num[n++] = rev[--rn];
    }
    while (prefix[pos] && pos < sizeof(buf) - 32) {
        buf[pos] = prefix[pos];
        pos++;
    }
    for (size_t i = 0; i < n && pos < sizeof(buf) - 2; i++) buf[pos++] = num[i];
    buf[pos++] = '\n';
    ca_ops_log(buf, pos);
}

static size_t ca_fmt_u64(char *dst, size_t cap, uint64_t v) {
    char rev[24];
    size_t rn = 0;
    size_t n = 0;
    if (cap == 0) return 0;
    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v > 0 && rn < sizeof(rev)) {
        rev[rn++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (rn > 0 && n < cap) dst[n++] = rev[--rn];
    return n;
}

static void ca_log_frame(uint64_t frame_no, uint32_t surface, uint32_t seed) {
    char buf[128];
    size_t pos = 0;
    const char *p1 = "[carender] frame ";
    const char *p2 = " src=ca surface=";
    const char *p3 = " seed=";
    for (size_t i = 0; p1[i] && pos < sizeof(buf) - 48; i++) buf[pos++] = p1[i];
    pos += ca_fmt_u64(buf + pos, sizeof(buf) - pos - 32, frame_no);
    for (size_t i = 0; p2[i] && pos < sizeof(buf) - 24; i++) buf[pos++] = p2[i];
    pos += ca_fmt_u64(buf + pos, sizeof(buf) - pos - 16, (uint64_t)surface);
    for (size_t i = 0; p3[i] && pos < sizeof(buf) - 12; i++) buf[pos++] = p3[i];
    pos += ca_fmt_u64(buf + pos, sizeof(buf) - pos - 2, (uint64_t)seed);
    buf[pos++] = '\n';
    ca_ops_log(buf, pos);
}

/* [fps] log line: "[fps] frames=<N> fps=<F>"
 * once per second while the engine runs, where N is the frames
 * forwarded or rendered in the last second. A 0 line is meaningful:
 * it proves the engine is alive but idle, and the acceptance check
 * fails a run with any 0 second while the app is foreground. */
static void ca_log_fps(uint64_t frames) {
    char buf[96];
    char num[24];
    size_t pos = 0;
    size_t n = 0;
    uint64_t t = frames;
    const char *p1 = "[fps] frames=";
    const char *p2 = " fps=";
    if (t == 0) {
        num[n++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (t > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (rn > 0) num[n++] = rev[--rn];
    }
    while (p1[pos] && pos < sizeof(buf) - 40) {
        buf[pos] = p1[pos];
        pos++;
    }
    for (size_t i = 0; i < n && pos < sizeof(buf) - 32; i++) buf[pos++] = num[i];
    for (size_t i = 0; p2[i] && pos < sizeof(buf) - 32; i++) buf[pos++] = p2[i];
    for (size_t i = 0; i < n && pos < sizeof(buf) - 2; i++) buf[pos++] = num[i];
    buf[pos++] = '\n';
    ca_ops_log(buf, pos);
}

static int64_t ca_now_ns(void) {
    return ca_ops_now_ns();
}

/* ------------------------------------------------------------------ */
/* Rate adaptation                                                    */
/* ------------------------------------------------------------------ */

static void ca_adapt_rate(int64_t duration_ns) {
    int64_t budget;
    int64_t slow_ns;
    int64_t fast_ns;
    if (g_e.render_ema_ns == 0) {
        g_e.render_ema_ns = duration_ns;
    } else {
        /* EMA with alpha 1/4, integer only. */
        g_e.render_ema_ns = (g_e.render_ema_ns * 3 + duration_ns) / 4;
    }
    /* Budget for one frame at the current divisor: the period
     * times the divisor. A 20 ms render is slow at 60 Hz
     * (16.6 ms budget) but fits at 30 Hz (33.3 ms budget), so
     * the rate settles at 30 Hz instead of stepping down again. */
    budget = g_e.period_ns * (int64_t)g_e.divisor;
    slow_ns = budget * 3 / 4;
    fast_ns = budget / 4;
    if (duration_ns >= slow_ns) {
        g_e.slow_streak++;
        g_e.fast_streak = 0;
        if (g_e.slow_streak >= 8 && g_e.divisor < CARENDER_MAX_DIVISOR) {
            g_e.divisor++;
            g_e.slow_streak = 0;
            ca_log_u64("[carender] rate step down, divisor=", g_e.divisor);
        }
    } else if (duration_ns <= fast_ns) {
        g_e.fast_streak++;
        g_e.slow_streak = 0;
        if (g_e.fast_streak >= 8 && g_e.divisor > 1) {
            g_e.divisor--;
            g_e.fast_streak = 0;
            ca_log_u64("[carender] rate step up, divisor=", g_e.divisor);
        }
    } else {
        g_e.slow_streak = 0;
        g_e.fast_streak = 0;
    }
    g_e.st.divisor = g_e.divisor;
    g_e.st.render_ema_ns = g_e.render_ema_ns;
}

/* ------------------------------------------------------------------ */
/* Public notifications (commit hook, damage hook)                    */
/* ------------------------------------------------------------------ */

void carender_commit_notify(void) {
    g_e.commit_seen = 1;
    g_e.dirty = 1;
    g_e.st.commits++;
}

void carender_damage_add_points(double x, double y, double w, double h) {
    if (!g_e.configured) return;
    cadamage_add_points(&g_e.damage, x, y, w, h, g_e.scale);
    g_e.st.damage_rects++;
    g_e.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* One engine iteration                                               */
/* ------------------------------------------------------------------ */

int carender_step(void) {
    int64_t vsync_ns = 0;
    int r;
    int64_t timeout_ms;
    uint32_t surf;
    uint32_t seed = 0;
    int seed_ok = 0;
    int surf_changed = 0;

    if (!g_e.configured || !g_e.armed) return 0;
#ifdef CARENDER_HOST_TEST
    if (g_e.ops == NULL || g_e.ops->wait_vsync == NULL) return 0;
#endif

    /* Self-clocked tick. The old code waited for a
     * FRAME_DONE ack and did nothing on timeout. That deadlocks
     * with the snapshot pump deferred: FRAME_DONE is only ever
     * sent as the ack of a PRESENT (server.c), and no PRESENT is
     * ever sent without a tick, so the engine spun on 16 ms
     * timeouts forever and produced zero frames. The engine now
     * keeps its own tick boundary: it waits only until the next
     * boundary, and ticks when the boundary arrives whether or
     * not an ack came. An ack is an early wakeup, never the tick
     * itself. The first tick is immediate (boundary primed to
     * now), so the first render also bootstraps the presenter
     * session through present_init, which is lazy on the first
     * present. */
    {
        int64_t now = ca_now_ns();
        int64_t period_ms = g_e.period_ns / 1000000LL;
        int64_t remain_ns;
        if (period_ms <= 0) period_ms = 16;
        if (g_e.next_tick_ns == 0) g_e.next_tick_ns = now;
        remain_ns = g_e.next_tick_ns - now;
        if (remain_ns <= 0) {
            timeout_ms = 0;
        } else {
            timeout_ms = remain_ns / 1000000LL;
            if (timeout_ms > period_ms) timeout_ms = period_ms;
        }
    }
    r = ca_ops_wait_vsync(&vsync_ns, (int)timeout_ms);
    if (r == 0) {
        /* Ack arrived: an early wakeup. The error run is over. */
        g_e.wait_error_since_ns = -1;
    } else {
        g_e.st.timeouts++;
        if (r < 0) {
            /* Session not live yet (the engine starts before main,
             * the session is lazy on the first present) or dead.
             * Pace the loop by sleeping to the tick boundary
             * instead of spinning on instant -1 returns, and
             * survive: only a session dead for
             * CARENDER_WAIT_ERROR_DEATH_NS stops the step with -1.
             * The old code died on the first -1, which killed the
             * engine thread at startup. */
            int64_t now2 = ca_now_ns();
            if (g_e.wait_error_since_ns < 0) {
                g_e.wait_error_since_ns = now2;
                ca_log_str("[carender] vsync wait error, session not live yet (retrying)\n");
            } else if (g_e.st.frames > 0 &&
                       now2 - g_e.wait_error_since_ns >=
                       CARENDER_WAIT_ERROR_DEATH_NS) {
                return -1;
            }
            {
                int64_t rem_ns = g_e.next_tick_ns - ca_now_ns();
                int sleep_ms = (int)(rem_ns / 1000000LL);
                if (sleep_ms < 1) sleep_ms = 1;
                ca_ops_sleep_ms(sleep_ms);
            }
        } else {
            g_e.wait_error_since_ns = -1;
        }
    }
    {
        int64_t now = ca_now_ns();
        int64_t tick_ns;
        if (now < g_e.next_tick_ns) return 0; /* ack arrived early */
        /* Acted tick. Advance the boundary by one period; if we
         * are more than a period behind, re-sync instead of firing
         * a catch-up burst. */
        if (now - g_e.next_tick_ns < g_e.period_ns) {
            g_e.next_tick_ns += g_e.period_ns;
        } else {
            g_e.next_tick_ns = now + g_e.period_ns;
        }
        tick_ns = (r == 0) ? vsync_ns : now;
        if (r != 0 && !g_e.timer_tick_logged) {
            g_e.timer_tick_logged = 1;
            ca_log_str("[carender] vsync on timer, no FRAME_DONE ack\n");
        }
        /* One on_vsync per acted tick: the ack timestamp when the
         * tick rode an ack, the engine clock otherwise. This is
         * also the display-link tick: the link
         * fires at the acted-tick rate, never faster. */
        ca_ops_on_vsync(tick_ns);
        g_e.st.vsyncs++;
    }

    /* [fps] window: once per second, log the
     * frames forwarded or rendered in the last second. The window
     * opens on the first acted tick and closes on the first acted
     * tick at or past one second later; a window with 0 frames
     * still logs, so a stalled engine is visible instead of silent. */
    {
        int64_t now = ca_now_ns();
        if (g_e.fps_window_start_ns == 0) {
            g_e.fps_window_start_ns = now;
            g_e.fps_window_frames = g_e.st.frames;
        } else if (now - g_e.fps_window_start_ns >= 1000000000LL) {
            ca_log_fps(g_e.st.frames - g_e.fps_window_frames);
            g_e.fps_window_start_ns = now;
            g_e.fps_window_frames = g_e.st.frames;
        }
    }

    /* Rate divisor: act on every divisor-th vsync. Skipped vsyncs
     * keep their dirty flag, so no content is lost, only delayed. */
    if ((g_e.st.vsyncs % g_e.divisor) != 0) return 0;

    /* A commit that landed between steps must be picked up even
     * if the flag was set before this step started. */
    if (g_e.commit_seen) {
        g_e.commit_seen = 0;
        g_e.dirty = 1;
    }

    surf = ca_ops_find_surface();
    if (surf != 0) {
        seed_ok = (ca_ops_surface_seed(surf, &seed) == 0);
    }
    if (seed_ok) {
        if (!g_e.have_last || surf != g_e.last_surface || seed != g_e.last_seed) {
            surf_changed = 1;
        }
    }

    if (surf_changed) {
        int64_t t0 = ca_now_ns();
        int64_t dt;
        int rc;
        carender_isws_rect_t rects[CADAMAGE_MAX_RECTS];
        cadamage_rect_t drects[CADAMAGE_MAX_RECTS];
        int n = -1;
        const void *damage_ptr = NULL;
        uint32_t damage_count = 0;
        if (g_e.send_damage) {
            n = cadamage_get(&g_e.damage, drects, CADAMAGE_MAX_RECTS);
            if (n > 0) {
                int i;
                for (i = 0; i < n; i++) {
                    rects[i].x = (int32_t)drects[i].x;
                    rects[i].y = (int32_t)drects[i].y;
                    rects[i].w = drects[i].w;
                    rects[i].h = drects[i].h;
                }
                damage_ptr = rects;
                damage_count = (uint32_t)n;
            }
        }
        rc = ca_ops_forward_surface(surf, damage_ptr, damage_count);
        dt = ca_now_ns() - t0;
        if (rc == 0) {
            g_e.last_surface = surf;
            g_e.last_seed = seed;
            g_e.have_last = 1;
            g_e.st.last_surface = surf;
            g_e.st.last_seed = seed;
            g_e.st.frames++;
            g_e.st.ca_frames++;
            ca_log_frame(g_e.st.frames, surf, seed);
            cadamage_reset(&g_e.damage);
            g_e.dirty = 0;
            ca_adapt_rate(dt);
        } else {
            /* Both present buffers busy: coalesce. Keep the damage
             * and the dirty flag; the next acted vsync retries with
             * the latest state. */
            g_e.st.coalesced++;
        }
        return 0;
    }

    if (g_e.dirty) {
        int64_t t0 = ca_now_ns();
        int64_t dt;
        int rc = ca_ops_render_snapshot();
        dt = ca_now_ns() - t0;
        if (rc == 0) {
            g_e.dirty = 0;
            cadamage_reset(&g_e.damage);
            g_e.st.frames++;
            g_e.st.snap_frames++;
            ca_adapt_rate(dt);
        } else {
            g_e.st.coalesced++;
        }
        return 0;
    }

    g_e.st.idle_vsyncs++;
    return 0;
}

int carender_stats(carender_stats_t *out) {
    if (out == NULL || !g_e.configured) return -1;
    *out = g_e.st;
    return 0;
}

uint32_t carender_current_hz(void) {
    if (!g_e.configured || g_e.divisor == 0) return 0;
    return g_e.hz / g_e.divisor;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

static void carender_configure(const carender_ops_t *ops, uint32_t frame_w,
                               uint32_t frame_h, double scale, uint32_t hz,
                               int send_damage) {
    g_e.ops = ops;
    g_e.configured = 1;
    g_e.armed = 1;
    g_e.hz = (hz == 0) ? CARENDER_DEFAULT_HZ : hz;
    g_e.divisor = 1;
    g_e.period_ns = 1000000000LL / (int64_t)g_e.hz;
    g_e.scale = (scale > 0.0) ? scale : 3.0;
    g_e.send_damage = send_damage ? 1 : 0;
    g_e.dirty = 0;
    g_e.commit_seen = 0;
    g_e.last_surface = 0;
    g_e.last_seed = 0;
    g_e.have_last = 0;
    g_e.render_ema_ns = 0;
    g_e.slow_streak = 0;
    g_e.fast_streak = 0;
    cadamage_init(&g_e.damage, frame_w, frame_h);
    g_e.st.vsyncs = 0;
    g_e.st.frames = 0;
    g_e.st.ca_frames = 0;
    g_e.st.snap_frames = 0;
    g_e.st.idle_vsyncs = 0;
    g_e.st.coalesced = 0;
    g_e.st.timeouts = 0;
    g_e.st.commits = 0;
    g_e.st.damage_rects = 0;
    g_e.st.divisor = 1;
    g_e.st.render_ema_ns = 0;
    g_e.st.last_surface = 0;
    g_e.st.last_seed = 0;
    g_e.fps_window_start_ns = 0;
    g_e.fps_window_frames = 0;
    g_e.next_tick_ns = 0;
    g_e.wait_error_since_ns = -1;
    g_e.timer_tick_logged = 0;
}

#ifdef CARENDER_HOST_TEST

void carender_test_configure(const carender_ops_t *ops) {
    carender_configure(ops, 1080, 2400, 3.0, CARENDER_DEFAULT_HZ, 0);
}

void carender_test_set_frame(uint32_t w, uint32_t h, double scale) {
    int send_damage = g_e.send_damage;
    uint32_t hz = g_e.hz;
    carender_stats_t keep = g_e.st;
    carender_configure(g_e.ops, w, h, scale, hz, send_damage);
    g_e.st = keep;
    g_e.st.divisor = g_e.divisor;
}

void carender_test_set_send_damage(int on) {
    g_e.send_damage = on ? 1 : 0;
}

void carender_test_reset(void) {
    const carender_ops_t *ops = g_e.ops;
    carender_configure(ops, 1080, 2400, 3.0, CARENDER_DEFAULT_HZ, 0);
}

#endif /* CARENDER_HOST_TEST */

#ifndef CARENDER_HOST_TEST

/* ------------------------------------------------------------------ */
/* Device backends                                                    */
/* ------------------------------------------------------------------ */

static void *ca_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__)
    uint64_t v = (uint64_t)p;
    __asm__ volatile(
        "xpaci %0\n"
        "paciza %0\n"
        : "+r"(v)
    );
    return (void *)v;
#else
    return p;
#endif
}

#if defined(__aarch64__)
static __attribute__((noinline)) void *ca_gapis_call2(uint64_t slot, uint64_t div,
                                                       uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)CA_GAPIS_ADDR;
    uint64_t vt;
    uint64_t obj_div;
    uint64_t slot_addr;
    uint64_t fn;
    uint64_t fn_div;
    if (!obj) return NULL;
    vt = *(volatile uint64_t *)obj;
    obj_div = (obj & 0x0000ffffffffffffULL) | (CA_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    slot_addr = vt + slot * 8ULL;
    fn = *(volatile uint64_t *)slot_addr;
    fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

    register uint64_t x0 __asm__("x0") = obj;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x16 __asm__("x16") = fn;
    register uint64_t x17 __asm__("x17") = fn_div;
    __asm__ volatile(
        "blraa x16, x17"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "+r"(x17)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "cc", "memory"
    );
    return (void *)x0;
}
#endif

static void *ca_dlsym(const char *name) {
    if (!name) return NULL;
#if defined(__aarch64__)
    return ca_gapis_call2(CA_SLOT_DLSYM, CA_DIV_DLSYM,
                          (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static int64_t ca_dev_now_ns(void) {
    int64_t ts[2] = { 0, 0 };
    if (sys2(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)ts) < 0) {
        return 0;
    }
    return ts[0] * 1000000000LL + ts[1];
}

static void ca_dev_log(const char *msg, size_t len) {
    if (msg && len > 0) sys_write(2, msg, len);
}

static int ca_dev_wait_vsync(int64_t *vsync_ns_out, int timeout_ms) {
    return present_wait_vsync(vsync_ns_out, timeout_ms);
}

/* lsk_cadisplay_vsync_tick() is declared in cadisplay.h, which
 * this file already includes above (non-host branch); there is
 * no local extern anymore. */

static void ca_dev_on_vsync(int64_t vsync_ns) {
    (void)cadisplay_on_vsync(vsync_ns);
    /* One display-link tick per acted engine tick:
     * the engine is self-clocked, so the link fires at the
     * acted-tick rate whether the tick rode a FRAME_DONE ack or
     * the local timer. */
    lsk_cadisplay_vsync_tick((uint64_t)vsync_ns);
}

static uint32_t ca_dev_find_surface(void) {
    return cadisplay_find_display_surface();
}

static int ca_dev_surface_seed(uint32_t surface_id, uint32_t *seed_out) {
    return cadisplay_surface_seed(surface_id, seed_out);
}

static int ca_dev_forward_surface(uint32_t surface_id, const void *damage,
                                  uint32_t damage_count) {
    /* cadisplay_on_frame_damage is declared in cadisplay.h.
     * The damage pointer is isws_rect_t layout. */
    return cadisplay_on_frame_damage(surface_id,
                                     (const isws_rect_t *)damage,
                                     damage_count);
}

/* Snapshot fallback on the main thread. The engine
 * requests one tick through the snapshot coalescer
 * (snapshot_request_frame_ca) and waits (bounded, 100 ms) for the
 * frame counter to advance. The old code dispatched its own
 * snapshot_render_now tick to the main queue on every dirty vsync
 * and waited on a done flag: while the main thread was stalled
 * those ticks accumulated unboundedly on the main queue, and when
 * it resumed a burst of redundant full renders ran back to back.
 * The coalescer caps queued ticks at one pending plus one running,
 * so the burst cannot form. When carender is armed the pump is
 * deferred, so the only tick producer is this function and a frame
 * counter advance means this request (or one folded into it)
 * rendered. renderInContext and UIKit still run on the main
 * thread; the engine thread only waits. */
static void ca_dev_sleep_ms(int ms) {
    struct { long s, ns; } ts = { 0, 0 };
    ts.s = ms / 1000;
    ts.ns = (long)(ms % 1000) * 1000000L;
    sys2(SYS_nanosleep, (long)&ts, 0);
}

static int ca_dev_render_snapshot(void) {
    if (!snapshot_is_started()) return -1;
    return ca_snapshot_render_coalesced(snapshot_request_frame_ca,
                                        snapshot_frame_no,
                                        ca_dev_sleep_ms);
}

/* ------------------------------------------------------------------ */
/* Commit / damage hooks (method_setImplementation, snapshot.c style) */
/* ------------------------------------------------------------------ */

typedef struct {
    double x;
    double y;
    double w;
    double h;
} ca_cgrect_t;

typedef void (*ca_commit_fn)(void *, void *);
static ca_commit_fn g_commit_orig;
static void ca_commit_hook(void *cls, void *sel) {
    if (g_commit_orig) g_commit_orig(cls, sel);
    carender_commit_notify();
}

typedef void (*ca_needs_fn)(void *, void *, ca_cgrect_t);
static ca_needs_fn g_needs_orig;
static void ca_needs_hook(void *self, void *sel, ca_cgrect_t rect) {
    if (g_needs_orig) g_needs_orig(self, sel, rect);
    carender_damage_add_points(rect.x, rect.y, rect.w, rect.h);
}

static void ca_install_hooks(void) {
    void *(*class_getClassMethod)(void *, void *) =
        (void *(*)(void *, void *))ca_sign_fn(ca_dlsym("class_getClassMethod"));
    void *(*class_getInstanceMethod)(void *, void *) =
        (void *(*)(void *, void *))ca_sign_fn(ca_dlsym("class_getInstanceMethod"));
    void *(*method_setImplementation)(void *, void *) =
        (void *(*)(void *, void *))ca_sign_fn(ca_dlsym("method_setImplementation"));
    void *(*objc_getClass)(const char *) =
        (void *(*)(const char *))ca_sign_fn(ca_dlsym("objc_getClass"));
    void *(*sel_registerName)(const char *) =
        (void *(*)(const char *))ca_sign_fn(ca_dlsym("sel_registerName"));
    void *m;
    if (!class_getClassMethod || !class_getInstanceMethod ||
        !method_setImplementation || !objc_getClass || !sel_registerName) {
        ca_log("[carender] hooks unavailable (missing runtime symbols)\n");
        return;
    }
    m = NULL;
    {
        void *cls = objc_getClass("CATransaction");
        if (cls) m = class_getClassMethod(cls, sel_registerName("commit"));
        if (m) {
            g_commit_orig = (ca_commit_fn)method_setImplementation(m, (void *)ca_commit_hook);
            ca_log("[carender] hooked +[CATransaction commit]\n");
        } else {
            ca_log("[carender] +[CATransaction commit] not found, commit wake off\n");
        }
    }
    m = NULL;
    {
        void *cls = objc_getClass("CALayer");
        if (cls) m = class_getInstanceMethod(cls, sel_registerName("setNeedsDisplayInRect:"));
        if (m) {
            g_needs_orig = (ca_needs_fn)method_setImplementation(m, (void *)ca_needs_hook);
            ca_log("[carender] hooked -[CALayer setNeedsDisplayInRect:]\n");
        } else {
            ca_log("[carender] -[CALayer setNeedsDisplayInRect:] not found, damage off\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Optional experiment: start the local render server thread.        */
/*                                                                     */
/* ca-local-server-loop.md hypothesis 1:                               */
/* CA_FORCE_LOCAL_SERVER=1 creates the server port and port set but   */
/* nothing ever calls _CARenderServerStart, so no thread receives     */
/* on the server port and _CASGetDisplays hangs. Calling the          */
/* exported _CARenderServerStart once is the direct test of that      */
/* hypothesis. Gated on IOSWINE_CA_START_SERVER=1, runs once from    */
/* the engine thread after UIKit is up. [guess]                       */
/* ------------------------------------------------------------------ */

static void ca_maybe_start_server(void) {
    const char *en = lsk_getenv("IOSWINE_CA_START_SERVER");
    void *fn;
    if (!en || en[0] != '1') return;
    fn = ca_sign_fn(ca_dlsym("_CARenderServerStart"));
    if (!fn) {
        ca_log("[carender] _CARenderServerStart not found\n");
        return;
    }
    {
        int (*start)(void) = (int (*)(void))fn;
        int rc = start();
        if (rc == 1) {
            ca_log("[carender] _CARenderServerStart ok (thread started)\n");
        } else {
            ca_log("[carender] _CARenderServerStart returned non-1\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Optional experiment: CALocalDisplay registration.                  */
/*                                                                     */
/* ca-local.md section 2: with CA_ENABLE_LOCAL_DISPLAY=1,             */
/* +[CALocalDisplay displayWithDisplayId:name:deviceName:update:      */
/* timings:] registers a local display whose update block fires on    */
/* every composite. The block signature is unverified, so the block  */
/* here only logs its first argument and notifies the engine that    */
/* content changed; the surface is still found by the seed scan.     */
/* Gated on IOSWINE_CA_LOCAL_DISPLAY=1. [guess]                       */
/* ------------------------------------------------------------------ */

typedef struct {
    void *isa;
    int flags;
    int reserved;
    void *invoke;
    void *descriptor;
} ca_block_t;

static void ca_local_display_update(void *block, void *a0, void *a1, void *a2) {
    (void)block;
    (void)a0;
    (void)a1;
    (void)a2;
    /* Notification only: the surface seed scan finds the frame.
     * Marking dirty also wakes the snapshot fallback path. */
    carender_commit_notify();
}

static ca_block_t g_ca_update_block;

static void ca_maybe_register_local_display(void) {
    const char *en = lsk_getenv("IOSWINE_CA_LOCAL_DISPLAY");
    void *(*objc_getClass)(const char *) =
        (void *(*)(const char *))ca_sign_fn(ca_dlsym("objc_getClass"));
    void *(*sel_registerName)(const char *) =
        (void *(*)(const char *))ca_sign_fn(ca_dlsym("sel_registerName"));
    void *objc_msgSend;
    void *ns_concrete_stack_block;
    void *cls;
    void *sel;
    if (!en || en[0] != '1') return;
    if (!objc_getClass || !sel_registerName) {
        ca_log("[carender] local display unavailable (missing symbols)\n");
        return;
    }
    objc_msgSend = ca_sign_fn(ca_dlsym("objc_msgSend"));
    ns_concrete_stack_block = ca_dlsym("_NSConcreteStackBlock");
    if (!objc_msgSend || !ns_concrete_stack_block) {
        ca_log("[carender] local display unavailable (no block isa)\n");
        return;
    }
    cls = objc_getClass("CALocalDisplay");
    if (!cls) {
        ca_log("[carender] CALocalDisplay class not found\n");
        return;
    }
    sel = sel_registerName("displayWithDisplayId:name:deviceName:update:timings:");
    if (!sel) {
        ca_log("[carender] CALocalDisplay factory selector missing\n");
        return;
    }
    g_ca_update_block.isa = ns_concrete_stack_block;
    g_ca_update_block.flags = 0;
    g_ca_update_block.reserved = 0;
    g_ca_update_block.invoke = (void *)ca_local_display_update;
    g_ca_update_block.descriptor = NULL;
    {
        void *(*fn)(void *, void *, uint64_t, void *, void *, void *, void *) =
            (void *(*)(void *, void *, uint64_t, void *, void *, void *, void *))objc_msgSend;
        void *disp = fn(cls, sel, 1, NULL, NULL, &g_ca_update_block, NULL);
        if (disp) {
            ca_log("[carender] CALocalDisplay registered (displayId 1)\n");
        } else {
            ca_log("[carender] CALocalDisplay factory returned nil\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Engine thread and entry point                                      */
/* ------------------------------------------------------------------ */

static void *ca_engine_thread(void *arg) {
    (void)arg;
    ca_maybe_start_server();
    ca_maybe_register_local_display();
    for (;;) {
        if (carender_step() != 0) {
            ca_log("[carender] vsync error, engine stopping\n");
            return NULL;
        }
    }
    return NULL;
}

void carender_init(void) {
    const char *en;
    const char *hz_s;
    const char *dmg_s;
    uint32_t hz = CARENDER_DEFAULT_HZ;
    int send_damage = 0;
    int (*pc)(void *, void *, void *(*)(void *), void *);

    en = lsk_getenv("IOSWINE_CARENDER");
    if (!en || en[0] != '1') return;

    /* Once only: the ipc lazy arm (ipc.c) and
     * __libkernel_init_late (init.c) are both call sites, and a
     * second call would spawn a second engine thread and re-swizzle
     * the hooks, losing the real original IMPs. init_late never runs
     * under ioswine today, but the guard makes the two sites safe to
     * coexist if its gates ever open. */
    {
        static int once;
        if (once) return;
        once = 1;
    }

    hz_s = lsk_getenv("IOSWINE_CARENDER_HZ");
    if (hz_s && hz_s[0] == '3' && hz_s[1] == '0') hz = 30;
    dmg_s = lsk_getenv("IOSWINE_CARENDER_DAMAGE");
    if (dmg_s && dmg_s[0] == '1') send_damage = 1;

    /* Frame geometry comes from the BKS display values, 1440x3200
     * @4, the same values cadisplay_init uses. The damage tracker
     * is re-sized on the first forwarded frame if they disagree;
     * until then these are the documented display values.
     * Device backends are the ca_dev_* functions, reached through
     * the if-chain dispatch; the ops pointer is NULL on device
     * (it carries the host-test ops table only). The snapshot
     * fallback needs no main-queue handle of its own:
     * it requests a coalesced tick through snapshot.c, which
     * owns the main-queue dispatch. */
    carender_configure(NULL, 1440, 3200, 4.0, hz, send_damage);

    ca_install_hooks();

    pc = (int (*)(void *, void *, void *(*)(void *), void *))
        ca_sign_fn(ca_dlsym("pthread_create"));
    if (pc) {
        uint64_t th = 0;
        if (pc(&th, 0, ca_engine_thread, 0) == 0) {
            ca_log("[carender] engine armed (vsync driven)\n");
            return;
        }
    }
    ca_log("[carender] engine thread failed to start\n");
    g_e.armed = 0;
}

#endif /* CARENDER_HOST_TEST */
