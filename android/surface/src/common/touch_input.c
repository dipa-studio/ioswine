/*
 * touch_input.c - Presenter-side motion-event to ISWS TOUCH mapper.
 *
 * Implements touch_input.h. Pure C11, no platform calls, no malloc.
 * See the header for the rules.
 */
#include "touch_input.h"

static int ti_map_equal(const isws_info_t *a, const isws_info_t *b) {
    if (a->screen_w != b->screen_w) return 0;
    if (a->screen_h != b->screen_h) return 0;
    if (a->client_w != b->client_w) return 0;
    if (a->client_h != b->client_h) return 0;
    if (a->scale != b->scale) return 0;
    if (a->offset_x != b->offset_x) return 0;
    if (a->offset_y != b->offset_y) return 0;
    if (a->rotation != b->rotation) return 0;
    return 1;
}

static uint32_t ti_active_count(const ti_mapper_t *m) {
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (m->slot_active[i]) n++;
    }
    return n;
}

static int ti_find_slot(const ti_mapper_t *m, uint32_t android_id) {
    uint32_t i;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (m->slot_active[i] && m->slot_android_id[i] == android_id) {
            return (int)i;
        }
    }
    return -1;
}

static int ti_alloc_slot(ti_mapper_t *m, uint32_t android_id) {
    uint32_t i;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (!m->slot_active[i]) {
            m->slot_active[i] = 1;
            m->slot_android_id[i] = android_id;
            m->slot_last_x[i] = 0.0f;
            m->slot_last_y[i] = 0.0f;
            return (int)i;
        }
    }
    return -1;
}

static void ti_emit_one(ti_mapper_t *m, ti_emit_fn emit, void *emit_ctx,
                        uint32_t action, uint32_t slot, uint32_t count,
                        float cx, float cy, int64_t ts) {
    isws_touch_t t;
    t.action = action;
    t.pointer_id = slot;
    t.pointer_count = count;
    t.x = cx;
    t.y = cy;
    t.timestamp_ns = ts;
    emit(emit_ctx, &t);
    m->stats.emitted++;
    m->slot_last_x[slot] = cx;
    m->slot_last_y[slot] = cy;
}

/* Emit CANCEL for every active contact, in slot order, then clear the
 * table. The count in each message is the number of contacts still
 * down including the one being cancelled, matching the wire rule. */
static void ti_cancel_all(ti_mapper_t *m, ti_emit_fn emit, void *emit_ctx,
                          int64_t ts) {
    uint32_t i;
    uint32_t remaining = ti_active_count(m);
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (!m->slot_active[i]) continue;
        ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_CANCEL, i, remaining,
                    m->slot_last_x[i], m->slot_last_y[i], ts);
        m->stats.cancel_all_sent++;
        m->slot_active[i] = 0;
        m->slot_android_id[i] = 0;
        if (remaining > 0) remaining--;
    }
}

void ti_mapper_init(ti_mapper_t *m) {
    uint32_t i;
    if (!m) return;
    m->map.screen_w = 0;
    m->map.screen_h = 0;
    m->map.client_w = 0;
    m->map.client_h = 0;
    m->map.scale = 0.0f;
    m->map.offset_x = 0.0f;
    m->map.offset_y = 0.0f;
    m->map.rotation = 0;
    m->map_set = 0;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        m->slot_android_id[i] = 0;
        m->slot_active[i] = 0;
        m->slot_last_x[i] = 0.0f;
        m->slot_last_y[i] = 0.0f;
    }
    m->stats.emitted = 0;
    m->stats.dropped_down_outside = 0;
    m->stats.dropped_no_slot = 0;
    m->stats.dropped_unknown_up = 0;
    m->stats.dropped_dup_move = 0;
    m->stats.ignored_actions = 0;
    m->stats.cancel_all_sent = 0;
}

void ti_mapper_set_map(ti_mapper_t *m, const isws_info_t *map,
                       ti_emit_fn emit, void *emit_ctx) {
    if (!m || !map) return;
    if (m->map_set && ti_map_equal(&m->map, map)) return;
    if (m->map_set && ti_active_count(m) > 0) {
        /* Geometry changed under live contacts: cancel them first so
         * the client does not keep a finger down at stale coords. The
         * timestamp here is 0; the client treats 0 as unknown and does
         * not apply its stale-event guard to it. With no emit callback
         * the contacts are still forgotten, so no stale slot survives
         * the resize. */
        if (emit) {
            ti_cancel_all(m, emit, emit_ctx, 0);
        } else {
            ti_mapper_reset(m);
        }
    }
    m->map = *map;
    m->map_set = 1;
}

void ti_mapper_reset(ti_mapper_t *m) {
    uint32_t i;
    if (!m) return;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        m->slot_active[i] = 0;
        m->slot_android_id[i] = 0;
        m->slot_last_x[i] = 0.0f;
        m->slot_last_y[i] = 0.0f;
    }
}

uint32_t ti_mapper_active_count(const ti_mapper_t *m) {
    if (!m) return 0;
    return ti_active_count(m);
}

uint32_t ti_mapper_feed(ti_mapper_t *m, const ti_motion_t *ev,
                        ti_emit_fn emit, void *emit_ctx) {
    uint32_t before;
    uint32_t i;
    uint32_t h;
    if (!m || !ev || !emit) return 0;
    before = m->stats.emitted;
    if (!m->map_set) {
        m->stats.ignored_actions++;
        return 0;
    }
    if (ev->masked_action == TI_ACTION_OTHER) {
        m->stats.ignored_actions++;
        return 0;
    }
    if (ev->pointer_count > ISWS_MAX_POINTERS) {
        /* The caller promised pointer_count entries; more than the
         * table holds cannot be represented. Count it as ignored
         * rather than reading past the array. */
        m->stats.ignored_actions++;
        return 0;
    }

    if (ev->masked_action == TI_ACTION_DOWN) {
        uint32_t idx = ev->pointer_index;
        float cx = 0.0f;
        float cy = 0.0f;
        int inside = 0;
        int slot = -1;
        if (idx >= ev->pointer_count) idx = 0;
        if (ev->pointer_count == 0) {
            m->stats.ignored_actions++;
            return 0;
        }
        inside = isws_screen_to_client(&m->map,
                                       ev->pointers[idx].x,
                                       ev->pointers[idx].y, &cx, &cy);
        if (inside != 0) {
            /* DOWN in a letterbox bar: no contact begins. A tap on a
             * black bar must not become a tap on the app edge (F3). */
            m->stats.dropped_down_outside++;
            return 0;
        }
        slot = ti_find_slot(m, ev->pointers[idx].android_id);
        if (slot < 0) {
            slot = ti_alloc_slot(m, ev->pointers[idx].android_id);
            if (slot < 0) {
                m->stats.dropped_no_slot++;
                return 0;
            }
        }
        /* Re-DOWN for a slot that is already down: re-emit DOWN at the
         * new position. The client treats it as a fresh contact. */
        ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_DOWN, (uint32_t)slot,
                    ti_active_count(m), cx, cy, ev->timestamp_ns);
        return m->stats.emitted - before;
    }

    if (ev->masked_action == TI_ACTION_MOVE) {
        /* Historical samples first, oldest first, per pointer. A
         * historical sample for a pointer with no slot is a contact
         * whose DOWN was dropped (for example it began in a bar and
         * slid in): start it with a DOWN at that sample (F5). */
        for (h = 0; h < ev->history_count && h < TI_MAX_HISTORY; h++) {
            for (i = 0; i < ev->pointer_count; i++) {
                float cx = 0.0f;
                float cy = 0.0f;
                int slot = ti_find_slot(m, ev->pointers[i].android_id);
                (void)isws_screen_to_client(&m->map, ev->hist_x[h][i],
                                            ev->hist_y[h][i], &cx, &cy);
                if (slot < 0) {
                    slot = ti_alloc_slot(m, ev->pointers[i].android_id);
                    if (slot < 0) {
                        m->stats.dropped_no_slot++;
                        continue;
                    }
                    ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_DOWN,
                                (uint32_t)slot, ti_active_count(m),
                                cx, cy, ev->timestamp_ns);
                    continue;
                }
                if (cx == m->slot_last_x[slot] &&
                    cy == m->slot_last_y[slot]) {
                    m->stats.dropped_dup_move++;
                    continue;
                }
                ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_MOVE,
                            (uint32_t)slot, ti_active_count(m),
                            cx, cy, ev->timestamp_ns);
            }
        }
        for (i = 0; i < ev->pointer_count; i++) {
            float cx = 0.0f;
            float cy = 0.0f;
            int slot = ti_find_slot(m, ev->pointers[i].android_id);
            (void)isws_screen_to_client(&m->map, ev->pointers[i].x,
                                        ev->pointers[i].y, &cx, &cy);
            if (slot < 0) {
                slot = ti_alloc_slot(m, ev->pointers[i].android_id);
                if (slot < 0) {
                    m->stats.dropped_no_slot++;
                    continue;
                }
                ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_DOWN,
                            (uint32_t)slot, ti_active_count(m),
                            cx, cy, ev->timestamp_ns);
                continue;
            }
            if (cx == m->slot_last_x[slot] &&
                cy == m->slot_last_y[slot]) {
                m->stats.dropped_dup_move++;
                continue;
            }
            ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_MOVE,
                        (uint32_t)slot, ti_active_count(m),
                        cx, cy, ev->timestamp_ns);
        }
        return m->stats.emitted - before;
    }

    if (ev->masked_action == TI_ACTION_UP) {
        uint32_t idx = ev->pointer_index;
        float cx = 0.0f;
        float cy = 0.0f;
        int slot = -1;
        uint32_t count = 0;
        if (idx >= ev->pointer_count) idx = 0;
        if (ev->pointer_count == 0) {
            m->stats.ignored_actions++;
            return 0;
        }
        slot = ti_find_slot(m, ev->pointers[idx].android_id);
        if (slot < 0) {
            m->stats.dropped_unknown_up++;
            return 0;
        }
        /* Clamped: a finger that lifts outside the frame must still
         * lift, at the edge (F3). */
        (void)isws_screen_to_client(&m->map, ev->pointers[idx].x,
                                    ev->pointers[idx].y, &cx, &cy);
        count = ti_active_count(m);
        ti_emit_one(m, emit, emit_ctx, ISWS_TOUCH_UP, (uint32_t)slot,
                    count, cx, cy, ev->timestamp_ns);
        m->slot_active[slot] = 0;
        m->slot_android_id[slot] = 0;
        return m->stats.emitted - before;
    }

    if (ev->masked_action == TI_ACTION_CANCEL) {
        /* Android CANCEL applies to every pointer (F2). The changed
         * pointer index is irrelevant. */
        if (ti_active_count(m) == 0) {
            m->stats.ignored_actions++;
            return 0;
        }
        ti_cancel_all(m, emit, emit_ctx, ev->timestamp_ns);
        return m->stats.emitted - before;
    }

    m->stats.ignored_actions++;
    return 0;
}
