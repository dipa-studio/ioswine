/*
 * attributegraph.c - SwiftUI AttributeGraph update-cycle precondition
 * tracker. See attributegraph.h for the contract.
 *
 * Freestanding C11, no libc, no malloc, no __thread. No
 * function-pointer tables in static data (there is no dispatch
 * here at all: plain functions over plain state).
 *
 * No em dashes are used in this file.
 */
#include "attributegraph.h"

static uint32_t g_ag_tick_count;
static int64_t g_ag_last_tick_ns = -1;

void ag_note_display_tick(int64_t tick_ns) {
    if (tick_ns < 0) return;
    if (g_ag_last_tick_ns >= 0 && tick_ns <= g_ag_last_tick_ns) return;
    g_ag_last_tick_ns = tick_ns;
    g_ag_tick_count++;
}

int ag_update_cycle_ready(void) {
    return g_ag_tick_count > 0 ? 1 : 0;
}

int ag_update_begin(void) {
    return ag_update_cycle_ready() ? 0 : -1;
}

uint32_t ag_tick_count(void) {
    return g_ag_tick_count;
}

int64_t ag_last_tick_ns(void) {
    return g_ag_last_tick_ns;
}

#ifdef AG_HOST_TEST
void ag_test_reset(void) {
    g_ag_tick_count = 0;
    g_ag_last_tick_ns = -1;
}
#endif
