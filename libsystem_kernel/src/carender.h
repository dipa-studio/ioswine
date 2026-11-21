/*
 * carender.h - CA-commit / display-link driven render engine.
 *
 * Self-clocked vsync loop: the engine keeps its own
 * tick boundary at the configured rate and ticks when the boundary
 * arrives, whether or not a presenter FRAME_DONE ack came. An ack
 * is an early wakeup, never the tick itself: FRAME_DONE is only
 * ever sent as the ack of a PRESENT, so waiting for an ack before
 * the first tick deadlocks (no PRESENT without a tick, no ack
 * without a PRESENT). The first tick is immediate, so the first
 * render also bootstraps the presenter session through
 * present_init, which is lazy on the first present. A wait that
 * errors continuously for CARENDER_WAIT_ERROR_DEATH_NS stops the
 * step with -1; shorter error runs are survived.
 *   - wait for the next tick boundary (present_wait_vsync, bounded
 *     by the time to the boundary)
 *   - if the CA display surface seed changed, forward it with
 *     cadisplay_on_frame_damage (the CA path)
 *   - else if a CATransaction commit marked the tree dirty, render
 *     the windows with renderInContext on the main thread (the
 *     snapshot fallback path: one coalesced tick requested through
 *     snapshot_request_frame_ca, waited on bounded 100 ms)
 *   - else do nothing: render only on change
 * At most 2 frames are ever in flight (the presenter's limit); a
 * vsync whose buffers are both busy is coalesced, never queued.
 * The effective rate adapts to the measured render duration:
 * 60 Hz while renders are fast, 30 Hz, then 20 Hz as they slow.
 * One on_vsync call per acted tick drives the CADisplayLink clock
 * (lsk_cadisplay_vsync_tick, once per tick).
 *
 * Damage: the engine tracks damage rects from -[CALayer
 * setNeedsDisplayInRect:] (see cadamage.h). PRESENT damage is
 * sent only when IOSWINE_CARENDER_DAMAGE=1, because the presenter
 * uploads only the listed rects and a wrong rect leaves stale
 * pixels on screen; the layer-coordinate to framebuffer-pixel
 * mapping is approximate (full-screen layers at origin are exact).
 * Default is full-frame damage (damage_count 0), which is always
 * correct.
 *
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc,
 * no malloc, no __thread, no static function-pointer tables).
 * Host-test build (-DCARENDER_HOST_TEST) replaces the device
 * backends with the ops table below.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cadamage.h"

/* Default target rate. 60 [guess: the display is 1440x3200 @4,
 * 60 Hz per cadisplay.h]. */
#define CARENDER_DEFAULT_HZ 60u
/* Slowest adaptive rate: divisor 3 of 60 Hz = 20 Hz. */
#define CARENDER_MAX_DIVISOR 3u
/* A vsync wait that errors continuously for this long stops the
 * step with -1 (the engine thread then exits). Shorter error runs
 * are survived: the engine starts before main, while the
 * presenter session is still not live, and the first tick's render
 * bootstraps the session through present_init. 30 s. */
#define CARENDER_WAIT_ERROR_DEATH_NS 30000000000LL
/* Rate adaptation is relative to the current frame budget
 * (period / divisor): a render is slow when it takes more than
 * three quarters of the budget, fast when it takes less than a
 * quarter. Hysteresis comes from requiring 8 consecutive samples
 * in one direction (see carender.c). */

typedef struct {
    uint64_t vsyncs;        /* FRAME_DONE vsyncs seen */
    uint64_t frames;        /* frames forwarded or rendered */
    uint64_t ca_frames;     /* frames via the CA surface path */
    uint64_t snap_frames;   /* frames via the snapshot fallback path */
    uint64_t idle_vsyncs;   /* vsyncs with nothing to do */
    uint64_t coalesced;     /* vsyncs skipped because both buffers busy */
    uint64_t timeouts;      /* vsync waits that timed out */
    uint64_t commits;       /* CATransaction commits observed */
    uint64_t damage_rects;  /* damage rects recorded */
    uint32_t divisor;       /* current rate divisor (1, 2 or 3) */
    int64_t render_ema_ns;  /* exponential average render duration */
    uint32_t last_surface;  /* last forwarded CA surface id */
    uint32_t last_seed;     /* its seed at forward time */
} carender_stats_t;

/* Host-test backend operations. The host test supplies fakes
 * through this struct (carender_test_configure); on device the
 * engine calls the static ca_dev_* functions in carender.c
 * directly through an if-chain dispatch, and the ops pointer is
 * NULL. There is no static ops table in production code: the
 * arm64e linker rejects function pointers in static data.
 * wait_vsync returns 0 on a new vsync (fills *vsync_ns_out), 1 on
 * timeout, -1 on error. forward_surface returns 0 on success, -1
 * when the frame could not be presented (both buffers busy counts
 * as -1). render_snapshot returns 0 when a frame was rendered,
 * -1 on failure. damage rects use isws_rect_t layout
 * (x, y, w, h as int32/uint32); the engine passes cadamage rects
 * converted to that layout.
 * Coalesced snapshot fallback: when snapshot_request,
 * snapshot_frame_no and sleep_ms are all present, the engine
 * renders the snapshot fallback by requesting one coalesced tick
 * and waiting (bounded, 100 ms) for the frame counter to advance,
 * instead of calling render_snapshot() directly. The tick goes
 * through the snapshot ticks-owed coalescer, so at most one
 * snapshot tick is ever queued or running no matter how long the
 * main thread stalls: a stalled main queue can never accumulate a
 * burst of redundant full renders. When any of the three is NULL
 * the engine falls back to render_snapshot() (the legacy fallback
 * path). On device the three are the
 * snapshot_request_frame_ca / snapshot_frame_no / nanosleep
 * backends in carender.c. */
typedef struct {
    int64_t (*now_ns)(void);
    void (*log_fn)(const char *msg, size_t len);
    const char *(*getenv_fn)(const char *name);
    int (*wait_vsync)(int64_t *vsync_ns_out, int timeout_ms);
    void (*on_vsync)(int64_t vsync_ns);
    uint32_t (*find_surface)(void);
    int (*surface_seed)(uint32_t surface_id, uint32_t *seed_out);
    int (*forward_surface)(uint32_t surface_id, const void *damage,
                           uint32_t damage_count);
    int (*render_snapshot)(void);
    void (*snapshot_request)(void);
    uint64_t (*snapshot_frame_no)(void);
    void (*sleep_ms)(int ms);
} carender_ops_t;

/* Device entry point: read IOSWINE_CARENDER and friends, install
 * the commit/damage hooks, start the engine thread. Strict no-op
 * unless IOSWINE_CARENDER=1. Called once from init.c. */
void carender_init(void);

/* Called by the +[CATransaction commit] hook (device) or directly
 * by the host test. Marks the layer tree dirty. Safe to call from
 * any thread. */
void carender_commit_notify(void);

/* Called by the -[CALayer setNeedsDisplayInRect:] hook (device)
 * or directly by the host test. Records damage in UIKit points;
 * the engine converts with the display scale. */
void carender_damage_add_points(double x, double y, double w, double h);

/* One engine iteration: wait for the next tick boundary, then
 * forward or render if there is work. Returns 0 on success
 * (including timeout and coalesced), -1 only after
 * CARENDER_WAIT_ERROR_DEATH_NS of continuous vsync-wait errors.
 * The device thread loops on this; the host test calls it directly. */
int carender_step(void);

/* Fill *out with the current engine statistics. Returns 0, or -1
 * when the engine was never configured. */
int carender_stats(carender_stats_t *out);

/* Current target rate in Hz (60 / divisor), 0 when unconfigured. */
uint32_t carender_current_hz(void);

#ifdef CARENDER_HOST_TEST
/* Host test only: install the ops table and reset all state. The
 * frame geometry comes from the ops (the test sets the damage
 * tracker size through carender_test_set_frame). */
void carender_test_configure(const carender_ops_t *ops);
void carender_test_set_frame(uint32_t w, uint32_t h, double scale);
void carender_test_set_send_damage(int on);
void carender_test_reset(void);
#endif
