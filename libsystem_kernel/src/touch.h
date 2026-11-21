/*
 * touch.h - Presenter touch reader and UIKit IOHIDEvent injector.
 *
 * Activated only when IOSWINE_TOUCH=1 (and IOSWINE_SNAPSHOT=1).
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc, no malloc,
 * no __thread, no static function-pointer tables).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "present.h"

/* Start the background touch reader thread if IOSWINE_TOUCH=1; no-op otherwise. */
void touch_init(void);

/* Update the UIKit window size in points (e.g. 360.0 x 800.0) reported by snapshot.c. */
void touch_set_window_size(double width_pts, double height_pts);

/* Normalize a presenter client-pixel touch event to [0,1] and map to window points.
 * Returns 0 on success, -1 on invalid input (NULL, NaN/Inf, non-positive sizes). */
int touch_map_coords(const isws_touch_t *t, uint32_t client_w, uint32_t client_h,
                     double win_w, double win_h,
                     double *norm_x_out, double *norm_y_out,
                     double *win_x_out, double *win_y_out);

/* Build an IOHIDEvent digitizer event and dispatch to UIKit.
 * Called on the main dispatch queue via dispatch_async_f. */
void touch_inject(void *ctx);

/* Handle one event dequeued from the presenter (TOUCH / FOCUS / INFO /
 * KEY). This is the reader-side dispatch, factored out of the reader
 * thread so host tests can drive it without a socket. TOUCH events are
 * checked for staleness per pointer and enqueued for the main queue;
 * FOCUS lost cancels every active finger; INFO and KEY are no-ops here.
 * Returns 0 when accepted (or a no-op), -1 when dropped. */
int touch_handle_present_event(const present_event_t *ev);

/* Notification called after each presented frame; handles IOSWINE_TOUCH_SELFTEST=1. */
void touch_on_frame(uint64_t frame_no);

#ifdef TOUCH_HOST_TEST
typedef struct {
    void *(*dlsym_default)(const char *name);
    void *(*dlopen_fn)(const char *path);
    const char *(*getenv_fn)(const char *name);
    uint64_t (*mach_abs_time)(void);
    void (*log_fn)(const char *msg, size_t len);
    void (*receive_internal_fn)(void *fetcher, void *event);
    void (*drain_events_fn)(void *fetcher, void *environment);
    /* Direct -[UIEventEnvironment processEventQueue:] replacement:
     * called by the combined route after the drain, with the limit
     * touch.c passes (4). */
    void (*process_event_queue_fn)(void *environment, uint32_t limit);
    /* mach_timebase_info replacement for the host test: fills numer/denom,
     * returns 0 on success. Used to convert mach_absolute_time to seconds
     * for UITouch timestamps on the direct path. */
    int (*timebase_fn)(uint32_t *numer, uint32_t *denom);
} touch_test_ops_t;

void touch_test_configure(const touch_test_ops_t *ops);
void touch_test_reset(void);
int touch_test_process_touch(const isws_touch_t *t, uint32_t client_w, uint32_t client_h);
uint32_t touch_test_fingers_down(void);

/* Deliveries of -[UIApplication sendEvent:] observed through the hook. */
uint64_t touch_test_sendevent_count(void);

/* Drop counters: ring-full, invalid coords, stale wire timestamp, and
 * CANCELs emitted by cancel-all on focus loss / disconnect. NULL
 * outputs are ignored. */
void touch_test_drop_counts(uint64_t *ring_full, uint64_t *invalid,
                            uint64_t *stale, uint64_t *cancel_all);

/* Keyboard routing: calls to the host-test
 * keyboard stubs, last event kind delivered, hide requests issued on
 * focus loss, and the last TEXT bytes received. */
int touch_test_kb_init_calls(void);
int touch_test_kb_calls(void);
int touch_test_kb_last_kind(void);
int touch_test_kb_hide_calls(void);
const char *touch_test_kb_last_text(void);
void touch_test_kb_set_visible(int v);
#endif
