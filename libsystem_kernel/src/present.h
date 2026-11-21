/*
 * present.h - lsk-side presenter client for the ioswine-surface protocol.
 *
 * Fixed API contract between the presenter client (present.c) and its
 * consumers: the renderer (framebuffer + PRESENT), the display/vsync
 * layer (CADisplayLink feed), and the HID event builder (hidevent.c,
 * which consumes isws_touch_t values from the event queue).
 *
 * This header is the contract. Do not change field names, types or
 * function signatures without updating every consumer.
 *
 * Freestanding: C11, no libc. Depends on protocol.h (isws_* types).
 * present.c uses linux.h (raw syscalls) for socket/memfd/mmap; this
 * header does not.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

/* ------------------------------------------------------------------ */
/* Event queue                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    PRESENT_EV_TOUCH = 1,
    PRESENT_EV_KEY,
    PRESENT_EV_FOCUS,
    PRESENT_EV_INFO,
    PRESENT_EV_TEXT
} present_ev_kind_t;

typedef struct {
    present_ev_kind_t kind;
    union {
        isws_touch_t touch;
        isws_key_t key;
        isws_focus_t focus;
        isws_info_t info;
        isws_text_t text;
    } u;
} present_event_t;

/* Queue depth. When the queue is full, present_pump() drops the oldest
 * pending event to make room and counts it (see present_dropped_events).
 * Touch moves are the common case; down/up/cancel/key/focus/info are
 * rare, so a drop normally only loses a move. */
#define PRESENT_EVENT_QUEUE_LEN 64

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/* Connect to @ioswine-surface, create the memfd double buffer, send
 * HELLO + memfd (SCM_RIGHTS), wait for READY (up to 5 s). The client
 * frame is width x height pixels, BGRA8888 premultiplied, stride =
 * width * 4. Returns 0 on success, -1 on error (no connection, no
 * READY, server ERROR, bad geometry). On failure nothing is left
 * half-open: the socket is closed and the memfd unmapped. */
int present_init(uint32_t width, uint32_t height);

/* Same as present_init(), but bounds the READY wait to ready_timeout_ms
 * (< 0 uses the default 5000 ms). */
int present_init_timeout(uint32_t width, uint32_t height, int ready_timeout_ms);

/* Bytes per row of a presenter frame of `width` pixels: the single
 * source of truth for the presenter stride (tight, width * 4).
 * Returns 0 for width == 0, width > ISWS_MAX_WIDTH, or overflow.
 * The IOSurface side keeps its own aligned bytes_per_row (64-byte
 * aligned); the copy layers translate between the two row by row,
 * they never assume the strides are equal. */
uint32_t present_stride_for_width(uint32_t width);

/* Frame id the next PRESENT will carry (== PRESENTs sent this
 * session). 0 before any init. Lets a second consumer (the
 * snapshot / present_api layer, or cadisplay) compute the exact
 * in-flight count as next - (last_frame_id + 1) without keeping
 * its own counter, which would drift whenever another consumer
 * presents directly. */
uint64_t present_next_frame_id(void);

/* CLOCK_MONOTONIC ns, same clock as present_last_vsync_ns.
 * Returns 0 if the clock cannot be read. */
int64_t present_now_ns(void);

/* Exported convenience API: lazily initializes at w x h, copies/clips
 * BGRA8888 rows from bgra (with stride bytes/row), and presents. Returns
 * 0 on success, -1 if presenter socket is unavailable or on error.
 * Never crashes and never blocks > 100 ms. */
int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h,
                          uint32_t stride);

/* Same as ioswine_present_frame(), but submits `damage` rects with
 * the PRESENT (damage_count 0 means the whole frame; at most
 * ISWS_MAX_DAMAGE rects). Rects are clipped to the copied region
 * and empty rects are dropped; a caller passing a rect wholly
 * outside the frame gets damage_count 0 (whole frame) back on the
 * wire, never a bad rect. Geometry changes re-initialise the
 * session at the new size. */
int ioswine_present_frame_damage(const void *bgra, uint32_t w, uint32_t h,
                                 uint32_t stride,
                                 const isws_rect_t *damage,
                                 uint32_t damage_count);

/* Wait (up to timeout_ms, < 0 waits forever) until every PRESENT
 * sent so far has been acked by a FRAME_DONE. Returns 0 when
 * nothing is in flight, 1 on timeout, -1 on disconnect. */
int ioswine_present_flush(int timeout_ms);

/* Flush with a 100 ms budget, then send GOODBYE, close the socket,
 * unmap and close the memfd. Safe to call when not initialised.
 * After this, ioswine_present_frame() may be called again to
 * reconnect (fresh HELLO, fresh memfd). */
void ioswine_present_shutdown(void);

/* Send GOODBYE, close the socket, unmap and close the memfd. Safe to
 * call when not initialised. After this, present_init() may be called
 * again to reconnect (fresh HELLO, fresh memfd). */
void present_shutdown(void);

/* 1 if the client is connected and READY was received, 0 otherwise. */
int present_connected(void);

/* ------------------------------------------------------------------ */
/* Framebuffer and presenting                                           */
/* ------------------------------------------------------------------ */

/* Pointer to frame buffer `index` (0 or 1), or NULL if not
 * initialised / bad index. The renderer draws into this memory
 * directly (BGRA8888 premultiplied). */
uint8_t *present_framebuffer(int index);

/* Bytes per row of one buffer (== width * 4). 0 if not initialised. */
uint32_t present_stride(void);

/* Height in pixels of one buffer. 0 if not initialised. */
uint32_t present_height(void);

/* The memfd backing buffer `index` (0 or 1), for the cadisplay
 * zero-copy memfd-match check. present.c uses ONE memfd
 * for both buffers (buffer 1 at offset stride*height), so both
 * indices return the same fd; the match picks the lowest matching
 * index that is not in flight. Returns -1 if not initialised or
 * the index is bad. Additive only; no existing signature changes. */
int present_buffer_fd(int index);

/* Submit buffer `index` with damage rects (damage_count 0 means the
 * whole frame; at most ISWS_MAX_DAMAGE rects). The buffer must not
 * already be in flight (a previous PRESENT for it must have been
 * acked, or dropped by the server). Returns 0 on success, -1 on
 * error (not connected, bad index, send failure). The caller owns
 * pacing: wait for the FRAME_DONE of a buffer before reusing it
 * (present_wait_vsync), with a one-frame-period timeout after which
 * it may present on the other buffer anyway. */
int present_present(int index, const isws_rect_t *damage,
                    uint32_t damage_count);

/* ------------------------------------------------------------------ */
/* Vsync (FRAME_DONE)                                                   */
/* ------------------------------------------------------------------ */

/* Wait for the next FRAME_DONE (timeout_ms < 0 waits forever, 0 polls
 * once). Returns 0 and fills *vsync_ns_out with the vsync timestamp
 * (CLOCK_MONOTONIC ns, from the FRAME_DONE) on a new ack, 1 on
 * timeout, -1 on error or disconnect. This is the vsync source that
 * feeds CADisplayLink / the CA local display timing: one call per
 * display refresh, driven by the server's AChoreographer callback.
 * Note the FRAME_DONE vsync is the most recent vsync at ack time, not
 * a guarantee the frame hit that exact vsync. */
int present_wait_vsync(int64_t *vsync_ns_out, int timeout_ms);

/* CLOCK_MONOTONIC ns of the latest FRAME_DONE vsync, or -1 if no
 * FRAME_DONE has arrived yet. */
int64_t present_last_vsync_ns(void);

/* Frame id of the latest FRAME_DONE, or (uint64_t)-1 if none yet. */
uint64_t present_last_frame_id(void);

/* ------------------------------------------------------------------ */
/* Event pump and queue                                                 */
/* ------------------------------------------------------------------ */

/* Read available messages from the socket once (non-blocking) and
 * dispatch them: FRAME_DONE updates the vsync state, TOUCH / KEY /
 * FOCUS / INFO go into the event queue. Returns 0 on success, -1 on
 * disconnect, protocol error or ERROR from the server. Callers that
 * block in present_wait_vsync() do not need to call this separately;
 * callers that poll the queue should call present_pump() first. */
int present_pump(void);

/* Wait up to timeout_ms (< 0 waits forever, 0 polls once) for the
 * presenter socket to become readable, without consuming bytes.
 * Returns 1 if readable, 0 on timeout, -1 if not connected or error. */
int present_wait_readable(int timeout_ms);

/* Dequeue one pending event. Returns 1 if an event was written to
 * *out, 0 if the queue is empty. Does not touch the socket; call
 * present_pump() first to refill the queue. */
int present_poll_event(present_event_t *out);

/* Send a keyboard visibility request (ISWS_MSG_KEYBOARD) to the
 * presenter: show != 0 asks the presenter to show the soft
 * keyboard, show == 0 asks it to hide it. traits is the
 * KB_TRAIT_* bitfield, frame_h_pts the keyboard height in window
 * points (0 = unknown, v1). Returns 0 on success, -1 if not
 * connected or the send failed. */
int present_send_keyboard_request(uint32_t show, uint32_t traits,
                                  float frame_h_pts);

/* Number of events dropped because the queue was full (see above). */
uint64_t present_dropped_events(void);

/* TOUCH payloads dropped as invalid instead of killing the session.
 * A nonzero count means the presenter sent a malformed TOUCH (bad
 * action, pointer id, or pointer count); the session stays alive. */
uint64_t present_touch_invalid_dropped(void);

/* Latest INFO mapping (screen/client sizes, scale, offsets, rotation),
 * or 0-filled if no INFO has arrived yet. The server already maps
 * touches into client pixels, so this is for the client's own layout. */
isws_info_t present_last_info(void);
