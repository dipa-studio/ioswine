/*
 * touch_input.h - Presenter-side motion-event to ISWS TOUCH mapper.
 *
 * Pure C11, no Android headers, no libc beyond <stdint.h>/<stddef.h>.
 * The NativeActivity server (server.c) fills a ti_motion_t from one
 * AMotionEvent and calls ti_mapper_feed(); the mapper owns the
 * active-pointer table and emits one isws_touch_t per contact change
 * through the caller-supplied emit callback.
 *
 * Rules implemented here:
 *   - Android pointer ids are mapped to stable slots 0..ISWS_MAX_POINTERS-1
 *     first come, first served. A DOWN with no free slot is dropped and
 *     counted, never aliased onto another contact (F4).
 *   - ACTION_CANCEL applies to every active pointer, not just the changed
 *     one (F2). CANCEL is emitted per active pointer, in slot order.
 *   - A DOWN whose point falls in a letterbox bar is dropped. A MOVE or
 *     UP for a finger that is already down is sent clamped to the client
 *     frame, because the finger must still be able to lift (F3).
 *   - MOVE history is emitted first, oldest sample first, capped at
 *     TI_MAX_HISTORY samples per event (F5).
 *   - Identical consecutive MOVE positions for a slot are suppressed at
 *     the source, so the ring on the client side is not flooded.
 *   - pointer_count counts the pointers currently down including the
 *     finger in the message. For UP it is the count before the finger
 *     is removed; for DOWN the count after it is added.
 *   - Pressure is read from the motion event but is NOT forwarded: the
 *     v1 TOUCH payload has no pressure field, and the client synthesizes
 *     1.0 down / 0.0 up (F13). The field is kept in ti_pointer_t so a
 *     future wire version can use it without changing the mapper API.
 *
 * Freestanding rules: no malloc, no __thread, no function-pointer tables
 * in static data. The emit callback is a caller-supplied function
 * pointer in a parameter, not a table.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

/* Motion actions, mirroring the AMotionEvent masked actions the
 * presenter cares about. Unknown actions map to TI_ACTION_OTHER. */
#define TI_ACTION_DOWN    0u
#define TI_ACTION_MOVE    1u
#define TI_ACTION_UP      2u
#define TI_ACTION_CANCEL  3u
#define TI_ACTION_OTHER   4u

/* Historical MOVE samples carried per event (oldest first). */
#define TI_MAX_HISTORY    8u

typedef struct {
    uint32_t android_id; /* AMotionEvent pointer id */
    float x;             /* screen px (window coordinates) */
    float y;
    float pressure;      /* AMotionEvent pressure, 0.0 if unknown */
} ti_pointer_t;

typedef struct {
    uint32_t masked_action; /* TI_ACTION_* */
    uint32_t pointer_index; /* index of the changed pointer */
    uint32_t pointer_count; /* AMotionEvent pointer count */
    int64_t timestamp_ns;   /* CLOCK_MONOTONIC */
    ti_pointer_t pointers[ISWS_MAX_POINTERS];
    uint32_t history_count; /* 0..TI_MAX_HISTORY */
    /* history[h][i] is the h-th historical sample of pointers[i],
     * oldest first. Only the first pointer_count entries are used. */
    float hist_x[TI_MAX_HISTORY][ISWS_MAX_POINTERS];
    float hist_y[TI_MAX_HISTORY][ISWS_MAX_POINTERS];
} ti_motion_t;

/* Emit one decoded TOUCH message. ctx is caller-owned. */
typedef void (*ti_emit_fn)(void *ctx, const isws_touch_t *t);

typedef struct {
    uint32_t emitted;              /* TOUCH messages emitted */
    uint32_t dropped_down_outside; /* DOWN in a letterbox bar */
    uint32_t dropped_no_slot;      /* DOWN with no free slot */
    uint32_t dropped_unknown_up;   /* UP for a pointer with no slot */
    uint32_t dropped_dup_move;     /* identical MOVE suppressed */
    uint32_t ignored_actions;      /* hover, button, scroll, unknown */
    uint32_t cancel_all_sent;      /* CANCEL messages from cancel-all */
} ti_stats_t;

typedef struct {
    isws_info_t map;
    int map_set;
    uint32_t slot_android_id[ISWS_MAX_POINTERS];
    uint8_t slot_active[ISWS_MAX_POINTERS];
    float slot_last_x[ISWS_MAX_POINTERS]; /* last emitted client px */
    float slot_last_y[ISWS_MAX_POINTERS];
    ti_stats_t stats;
} ti_mapper_t;

/* Initialise a mapper with no mapping. Feed returns 0 until a mapping
 * is installed; events fed before that are counted as ignored. */
void ti_mapper_init(ti_mapper_t *m);

/* Install a new mapping. If contacts are active and the mapping geometry
 * changed, CANCEL is emitted for every active contact first (the client
 * coordinates of a contact do not survive a resize/rotation), then the
 * table is cleared and the new mapping installed. A mapping that is
 * identical to the current one is a no-op. */
void ti_mapper_set_map(ti_mapper_t *m, const isws_info_t *map,
                       ti_emit_fn emit, void *emit_ctx);

/* Forget all contacts without emitting anything. Use when the client
 * session is gone (disconnect): there is nobody left to send to. */
void ti_mapper_reset(ti_mapper_t *m);

/* Feed one motion event. Emits zero or more TOUCH messages through emit.
 * Returns the number of messages emitted. A NULL mapper, motion or emit
 * returns 0. */
uint32_t ti_mapper_feed(ti_mapper_t *m, const ti_motion_t *ev,
                        ti_emit_fn emit, void *emit_ctx);

/* Number of contacts currently down, according to the mapper table. */
uint32_t ti_mapper_active_count(const ti_mapper_t *m);
