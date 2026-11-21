/*
 * Periodic UIWindow -> CALayer snapshot pump (fallback display path).
 *
 * Activated only when IOSWINE_SNAPSHOT=1.
 *
 * Runloop trigger rationale:
 *   snapshot_init() is called from __libkernel_init_late (step 15 of
 *   _libSystem_initializer), after _libdispatch_init (step 9), ___malloc_init
 *   (step 6), and ___pthread_late_init (step 8) have initialized libdispatch's
 *   _dispatch_main_q. Scheduling dispatch_after_f(+1000 ms) onto
 *   _dispatch_main_q guarantees execution on the main thread once
 *   UIApplicationMain enters CFRunLoopRunInMode, without requiring UIKit
 *   swizzling or CFRunLoopGetMain at init time. Each tick reschedules the
 *   next dispatch_after_f(+1000 ms) on _dispatch_main_q upon completion.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Initialize snapshot pump if IOSWINE_SNAPSHOT=1; strict no-op otherwise. */
void snapshot_init(void);

/* carender: render one frame on demand, on the
 * caller's thread. Does not arm or disarm the 1 s pump. The frame
 * is tagged src=ca. The carender engine itself no longer calls
 * this: it uses snapshot_request_frame_ca(), which
 * coalesces through the same ticks-owed counter as the pump, so a
 * stalled main queue can never stack snapshot ticks. */
void snapshot_render_now(void);

/* Request one frame on the main thread, coalesced: if a tick is
 * already queued or running, the request is remembered and served
 * by the next tick instead of queueing another, so a 60 Hz
 * display-link source can call this on every commit without ever
 * stacking ticks. Safe to call from any thread (CA commit hook,
 * display link, presenter event loop): it only touches atomic
 * flags and posts to the main queue; the render itself always runs
 * on the main thread. No-op when the pump is not started. This is
 * the entry point display-link rendering calls. The tick
 * it produces is tagged src=pump in the frame log. */
void snapshot_request_frame(void);

/* Same coalescing as snapshot_request_frame(), but the tick it
 * produces is tagged src=ca in the frame log: the carender vsync
 * engine calls this instead of dispatching its own
 * snapshot tick, so a stalled main queue can never accumulate
 * carender snapshot ticks: at most one tick is ever queued or
 * running, however many vsyncs arrive while the main thread is
 * busy. No-op when the pump is not started. */
void snapshot_request_frame_ca(void);

/* 1 if a tick is queued or running, 0 otherwise. */
int snapshot_frame_pending(void);

/* Number of frames produced so far (the N in "[snapshot] frame N").
 * Monotonic; the carender engine waits on this advancing to know a
 * coalesced snapshot tick it requested has rendered. */
uint64_t snapshot_frame_no(void);

/* 1 when snapshot_init() has armed the pump (or deferred it to
 * carender), 0 otherwise. Lets the carender snapshot fallback fail
 * fast instead of waiting out its 100 ms bound when there is no
 * pump to serve the tick. */
int snapshot_is_started(void);

/* Frames produced per source since init: pump ticks (periodic pump
 * and snapshot_request_frame()) and carender ticks
 * (snapshot_request_frame_ca() and snapshot_render_now()). Either
 * out pointer may be NULL. pump + ca always equals
 * snapshot_frame_no(). */
void snapshot_frame_counts(uint64_t *pump_frames, uint64_t *ca_frames);

/* Stop the pump, cancel any periodic reschedule, free the bitmap
 * buffer. A tick already queued before this call becomes a no-op.
 * Safe to call when not started. snapshot_init() may be called
 * again afterwards to re-arm. Does not touch the presenter: the
 * caller owns the presenter lifecycle (present_shutdown() /
 * ioswine_present_shutdown()). */
void snapshot_shutdown(void);

/*
 * Presenter entry point (provided by present.c when present).
 * Declared here with a weak local stub in snapshot.c returning -1.
 */
int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride);

#ifdef SNAPSHOT_HOST_TEST
typedef struct {
    void *(*dlsym_default)(const char *name);
    const char *(*getenv_fn)(const char *name);
    uint64_t (*now_ns)(void);
    void (*log_fn)(const char *msg, size_t len);
    void *(*alloc_buf)(size_t size);
    void (*free_buf)(void *ptr, size_t size);
} snapshot_test_ops_t;

void snapshot_test_configure(const snapshot_test_ops_t *ops);
void snapshot_test_reset(void);
void snapshot_test_tick(void);
#endif
