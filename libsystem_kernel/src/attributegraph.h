/*
 * attributegraph.h - SwiftUI AttributeGraph update-cycle precondition
 * tracker.
 *
 * What this is for: on iOS, SwiftUI's graph update is driven by the
 * display link. The display-link callback advances the host's time
 * and the renderer host updates graph outputs (ViewRendererHost.render
 * -> viewGraph.flushTransactions -> AG::Subgraph::update ->
 * AG::Graph::update_attribute -> AG::Graph::UpdateStack::update).
 * The update therefore needs a display-driven host time. Our runtime
 * has no CADisplay and no firing display link (see crash B: the
 * display-link creation itself faults on a nil display, x8=0), so the
 * first animated SwiftUI transaction (a cell tap) flushes with no
 * display/time source and faults in AG::Graph::UpdateStack::update
 * with the same nil-deref signature (device measurement
 * 2026-11-17: pc=0x1b6cecb70, x8=0, x0=1, x9=0x20).
 *
 * This module tracks that precondition:
 *   - the display-link driver (cadisplay/carender) calls
 *     ag_note_display_tick() once per display tick that advances
 *     host time. The call site is the display-link fire path, after
 *     the tick is accepted, NOT the snapshot pump tick (the pump is
 *     not a display tick and must never call this).
 *   - any driver in our code that forces a SwiftUI graph update
 *     (layoutIfNeeded, CATransaction flush) consults
 *     ag_update_cycle_ready() / ag_update_begin() first.
 *   - ag_update_begin() is the guard: 0 means a display tick has
 *     advanced host time at least once, so a graph update has its
 *     time source; -1 means no tick yet, the update would run in
 *     the degraded context that faulted on device.
 *
 * Production wiring: lsk_cadisplay_vsync_tick()
 * in cadisplay.c calls ag_note_display_tick() once per acted
 * engine tick, so ag_update_cycle_ready() goes 1 after the first
 * display-link tick and the guard opens. The host test
 * (tests/host/attributegraph_test.c) pins this behavior.
 *
 * Freestanding C11, no libc, no malloc, no __thread. The state is
 * plain aligned words; aligned 32/64-bit loads and stores are
 * single-copy atomic on AArch64, and the reader only needs
 * monotonic visibility of the tick count and last tick time.
 *
 * No em dashes are used in this file.
 */
#ifndef IOSWINE_ATTRIBUTEGRAPH_H
#define IOSWINE_ATTRIBUTEGRAPH_H

#include <stdint.h>

/* Record one display tick that advances host time. tick_ns is a
 * monotonic nanosecond timestamp (mach_absolute_time scale). Ticks
 * that go backwards are ignored: the host time must never run
 * backwards, and a backwards tick means the caller is not a real
 * display tick. Safe to call from the display-link driver thread. */
void ag_note_display_tick(int64_t tick_ns);

/* 1 if at least one display tick has been recorded, else 0. This is
 * the precondition SwiftUI's graph update needs: a display-driven
 * host time exists. */
int ag_update_cycle_ready(void);

/* Guard for forcing a SwiftUI graph update. Returns 0 when the
 * update may run (a display tick has advanced host time at least
 * once); returns -1 when no tick has ever been recorded, the
 * degraded context that faulted on device. Callers that get -1 must
 * NOT force the update; they defer it until the cycle exists. */
int ag_update_begin(void);

/* Diagnostics: how many display ticks have been recorded, and the
 * timestamp of the most recent one (-1 if none). */
uint32_t ag_tick_count(void);
int64_t ag_last_tick_ns(void);

#ifdef AG_HOST_TEST
/* Reset all state (host test only). Not for production use. */
void ag_test_reset(void);
#endif

#endif
