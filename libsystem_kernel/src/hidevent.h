/*
 * hidevent.h - build IOHIDEvent serializations from presenter touch
 * messages.
 *
 * hidevent.c maintains the finger table: the
 * presenter protocol sends one TOUCH message per pointer per motion
 * event, while one IOHIDEvent digitizer event carries all currently
 * down fingers as child events. So each call updates the finger table
 * from one touch message and emits one complete digitizer event.
 *
 * Freestanding: C11, no libc. Depends on protocol.h (isws_touch_t).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

/* Feed one decoded TOUCH message (from present_poll_event()).
 * Emits one complete digitizer IOHIDEvent (all currently-down fingers
 * as child events) into `out`.
 * Returns bytes written, 0 if the message produced no event to emit
 * (e.g. a duplicate move with no state change), -1 on error (NULL
 * arguments, bad pointer id, out too small, unknown action).
 * The emitted bytes are a complete, self-contained IOHIDEvent
 * serialization ready to deliver to the UIEventFetcher callback
 * UIKit registers. */
int hidevent_touch(const isws_touch_t *t, uint8_t *out, size_t cap);

/* Forget all finger state (e.g. on disconnect, focus loss, or a
 * CANCEL that clears every finger). After this, the next DOWN starts
 * a fresh contact set. */
void hidevent_reset(void);

/* Number of fingers currently down, according to the finger table. */
uint32_t hidevent_fingers_down(void);
