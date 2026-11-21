/* mock_uikit_async.h - Device-faithful async UIKit mock for touch.c host tests.
 *
 * Derived from the touch_test.c mock, with one deliberate
 * difference from device findings: real UIKit _receiveHIDEvent: only
 * retains the event and performs the block on the fetcher thread. It does
 * NOT drain synchronously. The original mock ran the whole fetch-thread +
 * dispatcher + drain chain synchronously inside the wrapper, which is why
 * route A passed host checks while the device logged
 * "queued via _receiveHIDEvent:" + "queue filtered=0" and sendevent=0.
 *
 * This mock models the device faithfully:
 *   - _receiveHIDEvent: enqueues only (raw queue grows, filtered stays 0).
 *   - mock_run_fetch_thread() runs the queued _receiveHIDEventInternal:
 *     blocks (raw -> filtered) and signals the dispatcher.
 *   - mock_fire_main_runloop_source() fires the dispatcher main-runloop
 *     source: drainEventsIntoEnvironment: delivers the queued events to
 *     -[UIApplication sendEvent:] and, when the event context id matches
 *     the key window _contextId, on to -[UIWindow sendEvent:] (hit-test
 *     by context id). A context-id mismatch reproduces the issue:
 *     sendEvent: reached, window sendEvent: never ran, tab did not switch.
 *
 * A test that only injects and never pumps must therefore see sendevent=0
 * on the unpatched tree; making touch_inject trigger the
 * drain synchronously after queueing (route A + signal + drain_events,
 * the combined route) turns it green. The test-side pump functions
 * exist only as a diagnostic: they prove the queued events are
 * well-formed and would deliver if the drain fired.
 *
 * Same separate-TU caveat as touch_test.c: mock_objc_msgSend uses mixed
 * GP/FP params, sound only because touch.c lives in another TU (no LTO).
 * No Apple code, no device needed.
 *
 * Adds the KIF branch of route C (touch.c
 * touch_deliver_direct): -[UIApplication _touchesEvent] +
 * _clearTouches + _setHIDEvent: + _addTouch:forDelayedDelivery:NO +
 * sendEvent:, plus the interim TouchUpInside hack (sendActions-
 * ForControlEvents: on the nearest UIControl ancestor). Before this
 * extension the mock had none of those selectors, so route C always
 * fell through to the alloc/init direct path and the hack was
 * unobservable on host. The KIF model is additive and inert unless a
 * test selects route C (IOSWINE_TOUCH_DIRECT=1): no existing test
 * does, so no existing test changes behaviour. The shared event is
 * one fake UITouchesEvent + one fake NSSet (KIF reuses the app's
 * shared event); g_kif_hit_is_control is test-controlled: 1 means
 * the hit view has a UIControl ancestor (tab/button taps), 0 means
 * it does not (SwiftUI cell taps). The mock cannot know the real
 * view class names (object_getClassName is not modelled), so the
 * "[touch] direct kif sendEvent view=" log line prints an empty
 * view name on host; assert the prefix and phase only.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "protocol.h"
#include "touch.h"

#define MOCK_WIN_W 360.0
#define MOCK_WIN_H 800.0
/* Browse tab button rect in window points (selftest taps (107, 791)). */
#define MOCK_TAB_X0 72.0
#define MOCK_TAB_X1 144.0
#define MOCK_TAB_Y0 752.0
#define MOCK_TAB_Y1 800.0

/* ------------------------------------------------------------------ */
/* Mock IOKit / CoreFoundation / ObjC / libdispatch                   */
/* ------------------------------------------------------------------ */

#define MAX_MOCK_EVENTS 64
#define MAX_MOCK_CHILDREN 16
#define MAX_MOCK_TOUCHES 16

typedef struct mock_hid_event {
    int in_use;
    int refcount;
    int is_finger;
    uint64_t timestamp;
    uint32_t transducer_type;
    uint32_t index;
    uint32_t identity;
    uint32_t event_mask;
    uint32_t button_mask;
    double x;
    double y;
    double z;
    double pressure;
    double twist;
    uint32_t range;
    uint32_t touch;
    uint32_t options;
    int64_t disp_integrated;
    int64_t context_id;
    uint32_t bks_context_id; /* BKSHIDEventSetDigitizerInfo stamp */
    int bks_calls;
    uint64_t sender_id;
    int n_children;
    struct mock_hid_event *children[MAX_MOCK_CHILDREN];
} mock_hid_event_t;

static mock_hid_event_t g_mock_events[MAX_MOCK_EVENTS];
static int g_created_count = 0;
static int g_released_count = 0;

typedef struct {
    void *isa;
    void *eventFetcher;
    void *eventDispatcher;
    void *keyWindow;
} fake_app_layout_t;

typedef struct {
    void *isa;
    void *environment;
    void *eventFetcher;
    void *reserved;
    void *mainRunLoopCFSource;
    void *mainRunLoop;
} fake_disp_layout_t;

typedef struct {
    void *isa;
    void *cfRunLoop;
    void *eventDispatcher;
    void *incomingFiltered;
    void *incomingRaw;
} fake_fetcher_layout_t;

/* Fake UITouch object (KIF shape, fields the mock setters write). */
typedef struct {
    void *isa;
    void *window;
    void *view;
    long phase;
    double timestamp;
    unsigned long tapCount;
    double loc_x;
    double loc_y;
    int loc_set;
    int loc_reset;
    void *hidEvent;
    /* Retain count. The mock used to ignore release
     * entirely (no "release" selector mapping, no handler) and
     * retain was a bare return with no count, so every allocated
     * touch leaked its pool slot and the 16-slot pool exhausted
     * after 16 taps; the 240-tap storm then failed every later DOWN
     * (alloc NULL) even though the tree releases correctly. Real
     * ObjC frees at refcount 0, so the mock does too now. Set to 1
     * by alloc, bumped by retain, freed by release at 0. */
    int refcount;
} fake_touch_t;

/* Fake UITouchesEvent object. */
typedef struct {
    void *isa;
    void *touches;
    double timestamp;
} fake_touches_event_t;

/* Fake NSSet object (holds one object). */
typedef struct {
    void *isa;
    void *object;
} fake_nsset_t;

static int g_fake_app_cls;
static int g_fake_disp_cls;
static int g_fake_fetcher_cls;
static int g_fake_win_cls;
static int g_fake_tab_cls;
static int g_fake_touch_cls;
static int g_fake_touches_event_cls;
static int g_fake_nsset_cls;
static int g_fake_view_cls;
static int g_fake_control_cls; /* UIControl, for the KIF TouchUpInside hack. */

static fake_app_layout_t g_fake_app_obj;
static fake_disp_layout_t g_fake_disp_obj;
static fake_fetcher_layout_t g_fake_fetcher_obj;
static int g_fake_win_obj;
static int g_fake_tab_obj;
static int g_fake_source_obj;
static int g_fake_env_obj;
static int g_fake_rl_obj;
static int g_fake_touches_set;
static int g_fake_event_obj;
static int g_fake_main_q;
static int g_fake_view_obj;
static int g_fake_filtered_queue;
static int g_fake_raw_queue;
static fake_touch_t g_fake_touch_pool[MAX_MOCK_TOUCHES];
static int g_fake_touch_pool_used[MAX_MOCK_TOUCHES];
static fake_touches_event_t g_fake_event_pool[MAX_MOCK_TOUCHES];
static int g_fake_event_pool_used[MAX_MOCK_TOUCHES];
static fake_nsset_t g_fake_set_pool[MAX_MOCK_TOUCHES];
static int g_fake_set_pool_used[MAX_MOCK_TOUCHES];

static int g_sel_responds;
static int g_sel_shared_app;
static int g_sel_event_fetcher;
static int g_sel_receive_hid;
static int g_sel_key_window;
static int g_sel_windows;
static int g_sel_count;
static int g_sel_object_at_index;
static int g_sel_connected_scenes;
static int g_sel_all_objects;
static int g_sel_root_vc;
static int g_sel_tab_bar_vc;
static int g_sel_selected_index;
static int g_sel_view_controllers;
static int g_sel_child_vcs;
static int g_sel_all_touches;
static int g_sel_send_event;
static int g_sel_context_id;
static int g_sel_hit_test;
static int g_sel_set_window;
static int g_sel_set_view;
static int g_sel_set_phase;
static int g_sel_set_timestamp;
static int g_sel_set_tap_count;
static int g_sel_set_location;
static int g_sel_set_hid_event;
static int g_sel_set_touches_priv;
static int g_sel_set_touches;
static int g_sel_set_with_object;
static int g_sel_alloc;
static int g_sel_init;
static int g_sel_drain_events;
static int g_sel_dispatcher_received;
static int g_sel_set_event_timestamp;
/* KIF route C selectors. */
static int g_sel_touches_event;
static int g_sel_clear_touches;
static int g_sel_add_touch;
static int g_sel_set_hid_event_kif;
static int g_sel_is_kind_of_class;
static int g_sel_superview;
static int g_sel_send_actions;
static int g_sel_retain;
static int g_sel_release;

/* Call counters / recorded state. */
static int g_received_count = 0;
static mock_hid_event_t g_last_received;
static mock_hid_event_t g_last_children[MAX_MOCK_CHILDREN];

static int g_dispatch_calls = 0;
static int g_fetcher_absent = 0;
static const char *g_env_touch = NULL;
static const char *g_env_trace = NULL;
static const char *g_env_selftest = NULL;
static const char *g_env_direct = NULL;
static const char *g_env_internal = NULL;
static const char *g_env_sender = NULL;
static char g_log_buf[8192];
static size_t g_log_len = 0;

static int g_wrapper_receive_count = 0;
static int g_direct_internal_count = 0;
static int g_internal_receive_count = 0;
static int g_signal_count = 0;
static int g_wakeup_count = 0;
static int g_drain_events_count = 0;
static int g_drain_orig_count = 0;
static int g_dispatcher_received_count = 0;
static int g_dispatcher_orig_count = 0;
static int g_orig_send_event_called = 0;
static int g_window_send_event_count = 0;
static uint32_t g_fake_context_id = 0;
static uint64_t g_tab_selected_index = 0;
static uint64_t g_fake_touch_count = 0;
static int g_drain_call_send_event = 0;
static int g_hide_direct_setters = 0;
static int g_hide_touches_ivar = 0;
static int g_hide_set_touches = 0;
static int g_hide_all_direct_setters = 0;
static long g_phase_seq[16];
static int g_phase_seq_n = 0;
static int g_loc_reset_seq[16];
static int g_loc_seq_n = 0;
static double g_loc_seq_x[16];
static double g_loc_seq_y[16];
static int g_filtered_queue_count = 0;
static int g_raw_queue_count = 0;
static int g_down_in_tab[MAX_MOCK_TOUCHES];

/* Async fetcher queue (device-faithful). touch.c CFReleases the HID
 * event after inject returns, so the wrapper snapshots the event here
 * instead of holding the pointer: parent context/sender ids plus one
 * snapshot per finger child. internal_done tracks whether the fetch
 * thread ran _receiveHIDEventInternal: for it (raw -> filtered). */
#define MAX_ASYNC_QUEUE 64
typedef struct {
    void *src;
    int64_t context_id;
    uint64_t sender_id;
    int n_children;
    int internal_done;
    int delivered;
    struct {
        uint32_t index;
        double x;
        double y;
        uint32_t touch;
        uint32_t range;
    } child[MAX_MOCK_CHILDREN];
} mock_queued_event_t;
static mock_queued_event_t g_async_queue[MAX_ASYNC_QUEUE];
static int g_async_count = 0;
/* Context id of the event currently being delivered by the drain, so
 * mock_orig_send_event can gate the window forward on it. Only
 * meaningful while g_drain_current_is_hid is set (HID path); the
 * direct path carries its own window/view and is never gated. */
static int64_t g_drain_current_ctx = 0;
static int g_drain_current_is_hid = 0;

static int g_touch_alloc_count = 0;
static int g_hittest_count = 0;
static double g_last_hittest_x = 0.0;
static double g_last_hittest_y = 0.0;
static int g_set_location_count = 0;
static int g_set_timestamp_count = 0;
static double g_last_touch_timestamp = 0.0;
static int g_direct_send_event_count = 0;
static fake_touch_t *g_last_direct_touch = NULL;
static long g_last_direct_phase = -1;

/* KIF route C shared event: the app's shared
 * UITouchesEvent, reused across phases, plus its one-object set.
 * g_kif_hit_is_control is test-controlled per tap: 1 = the hit view
 * has a UIControl ancestor (tab/button taps fire the interim
 * TouchUpInside hack), 0 = it does not (SwiftUI cell taps).
 * g_touchupinside_count counts hack firings (sendActionsForControl-
 * Events: with UIControlEventTouchUpInside). */
static fake_touches_event_t g_kif_shared_event;
static fake_nsset_t g_kif_shared_set;
static int g_kif_hit_is_control = 0;
static int g_touchupinside_count = 0;

static void (*g_installed_drain_hook)(void *, void *, void *) = NULL;
static void (*g_installed_dispatcher_hook)(void *, void *, void *) = NULL;
static void (*g_installed_app_send_event_hook)(void *, void *, void *) = NULL;
static void (*g_installed_window_send_event_hook)(void *, void *, void *) = NULL;

static void mock_orig_send_event(void *self, void *cmd, void *event);
static void mock_drain_orig(void *self, void *cmd, void *env);
static void mock_dispatcher_orig(void *self, void *cmd, void *arg);
static void mock_window_send_event_orig(void *self, void *cmd, void *event);
static void mock_drain_events(void *fetcher, void *environment);
static void mock_dispatcher_received(void);
static void mock_uikit_model(void *fetcher, void *event);
static void *mock_objc_msgSend(void *obj, void *sel, void *a1, void *a2,
                               double f1, double f2);
static int is_fake_touches_event(void *obj);

static void reset_mocks(void) {
    memset(g_mock_events, 0, sizeof(g_mock_events));
    g_created_count = 0;
    g_released_count = 0;
    g_received_count = 0;
    memset(&g_last_received, 0, sizeof(g_last_received));
    memset(g_last_children, 0, sizeof(g_last_children));
    g_dispatch_calls = 0;
    g_fetcher_absent = 0;
    g_env_touch = NULL;
    g_env_trace = NULL;
    g_env_selftest = NULL;
    g_env_direct = NULL;
    g_env_internal = NULL;
    g_env_sender = NULL;
    memset(g_log_buf, 0, sizeof(g_log_buf));
    g_log_len = 0;

    g_wrapper_receive_count = 0;
    g_direct_internal_count = 0;
    g_internal_receive_count = 0;
    g_signal_count = 0;
    g_wakeup_count = 0;
    g_drain_events_count = 0;
    g_drain_orig_count = 0;
    g_dispatcher_received_count = 0;
    g_dispatcher_orig_count = 0;
    g_orig_send_event_called = 0;
    g_window_send_event_count = 0;
    g_fake_context_id = 0;
    g_tab_selected_index = 0;
    g_fake_touch_count = 0;
    g_drain_call_send_event = 0;
    g_hide_direct_setters = 0;
    g_hide_touches_ivar = 0;
    g_filtered_queue_count = 0;
    g_raw_queue_count = 0;
    memset(g_down_in_tab, 0, sizeof(g_down_in_tab));
    memset(g_async_queue, 0, sizeof(g_async_queue));
    g_async_count = 0;
    g_drain_current_ctx = 0;
    g_drain_current_is_hid = 0;

    g_touch_alloc_count = 0;
    g_hittest_count = 0;
    g_last_hittest_x = 0.0;
    g_last_hittest_y = 0.0;
    g_set_location_count = 0;
    g_set_timestamp_count = 0;
    g_last_touch_timestamp = 0.0;
    g_direct_send_event_count = 0;
    g_last_direct_touch = NULL;
    g_last_direct_phase = -1;
    g_hide_all_direct_setters = 0;
    memset(&g_kif_shared_event, 0, sizeof(g_kif_shared_event));
    g_kif_shared_event.isa = &g_fake_touches_event_cls;
    memset(&g_kif_shared_set, 0, sizeof(g_kif_shared_set));
    g_kif_shared_set.isa = &g_fake_nsset_cls;
    g_kif_hit_is_control = 0;
    g_touchupinside_count = 0;
    g_phase_seq_n = 0;
    g_loc_seq_n = 0;

    g_installed_drain_hook = NULL;
    g_installed_dispatcher_hook = NULL;
    g_installed_app_send_event_hook = NULL;
    g_installed_window_send_event_hook = NULL;

    memset(g_fake_touch_pool, 0, sizeof(g_fake_touch_pool));
    memset(g_fake_touch_pool_used, 0, sizeof(g_fake_touch_pool_used));
    memset(g_fake_event_pool, 0, sizeof(g_fake_event_pool));
    memset(g_fake_event_pool_used, 0, sizeof(g_fake_event_pool_used));
    memset(g_fake_set_pool, 0, sizeof(g_fake_set_pool));
    memset(g_fake_set_pool_used, 0, sizeof(g_fake_set_pool_used));

    memset(&g_fake_app_obj, 0, sizeof(g_fake_app_obj));
    memset(&g_fake_disp_obj, 0, sizeof(g_fake_disp_obj));
    memset(&g_fake_fetcher_obj, 0, sizeof(g_fake_fetcher_obj));
    g_fake_app_obj.eventFetcher = &g_fake_fetcher_obj;
    g_fake_fetcher_obj.incomingFiltered = &g_fake_filtered_queue;
    g_fake_fetcher_obj.incomingRaw = &g_fake_raw_queue;
}

static mock_hid_event_t *alloc_mock_event(void) {
    for (int i = 0; i < MAX_MOCK_EVENTS; i++) {
        if (!g_mock_events[i].in_use) {
            memset(&g_mock_events[i], 0, sizeof(g_mock_events[i]));
            g_mock_events[i].in_use = 1;
            g_mock_events[i].refcount = 1;
            g_created_count++;
            return &g_mock_events[i];
        }
    }
    return NULL;
}

static void *mock_IOHIDEventCreateDigitizerEvent(
    void *allocator, uint64_t timestamp, uint32_t transducerType,
    uint32_t index, uint32_t identity, uint32_t eventMask, uint32_t buttonMask,
    double x, double y, double z, double tipPressure, double twist,
    uint32_t range, uint32_t touch, uint32_t options) {
    (void)allocator;
    mock_hid_event_t *ev = alloc_mock_event();
    if (!ev) return NULL;
    ev->is_finger = 0;
    ev->timestamp = timestamp;
    ev->transducer_type = transducerType;
    ev->index = index;
    ev->identity = identity;
    ev->event_mask = eventMask;
    ev->button_mask = buttonMask;
    ev->x = x;
    ev->y = y;
    ev->z = z;
    ev->pressure = tipPressure;
    ev->twist = twist;
    ev->range = range;
    ev->touch = touch;
    ev->options = options;
    return ev;
}

static void *mock_IOHIDEventCreateDigitizerFingerEvent(
    void *allocator, uint64_t timestamp, uint32_t index, uint32_t identity,
    uint32_t eventMask, double x, double y, double z,
    double tipPressure, double twist,
    uint32_t range, uint32_t touch, uint32_t options) {
    (void)allocator;
    mock_hid_event_t *ev = alloc_mock_event();
    if (!ev) return NULL;
    ev->is_finger = 1;
    ev->timestamp = timestamp;
    ev->transducer_type = 2;
    ev->index = index;
    ev->identity = identity;
    ev->event_mask = eventMask;
    ev->x = x;
    ev->y = y;
    ev->z = z;
    ev->pressure = tipPressure;
    ev->twist = twist;
    ev->range = range;
    ev->touch = touch;
    ev->options = options;
    return ev;
}

static void mock_IOHIDEventAppendEvent(void *parent_v, void *child_v, uint32_t options) {
    (void)options;
    mock_hid_event_t *parent = (mock_hid_event_t *)parent_v;
    mock_hid_event_t *child = (mock_hid_event_t *)child_v;
    if (!parent || !child || parent->n_children >= MAX_MOCK_CHILDREN) return;
    child->refcount++;
    parent->children[parent->n_children++] = child;
}

static void mock_IOHIDEventSetIntegerValue(void *ev_v, uint32_t field, int64_t value) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    if (field == 0xb0019u) {
        ev->disp_integrated = value;
    } else if (field == 0xb0005u) {
        ev->context_id = value;
    }
}

static void mock_IOHIDEventSetSenderID(void *ev_v, uint64_t senderID) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    ev->sender_id = senderID;
}

/* BKSHIDEventSetDigitizerInfo stamp, the real context-id
 * setter now that the guessed 0xb0005 field write is opt-in. */
static void mock_BKSHIDEventSetDigitizerInfo(void *ev_v, uint32_t contextID,
                                             uint8_t a, uint8_t b, void *c,
                                             double d, float e) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (!ev) return;
    ev->bks_context_id = contextID;
    ev->bks_calls++;
}

static void mock_CFRelease(void *cf) {
    mock_hid_event_t *ev = (mock_hid_event_t *)cf;
    if (!ev || !ev->in_use) return;
    ev->refcount--;
    if (ev->refcount == 0) {
        for (int i = 0; i < ev->n_children; i++) {
            mock_CFRelease(ev->children[i]);
        }
        ev->in_use = 0;
        g_released_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Mock UIKit model: real UIKit's coordinate contract.                */
/*                                                                    */
/* The mock multiplies the event's NORMALIZED coordinates by the     */
/* window size (360x800 pt) and hit-tests the result. A tap is a     */
/* DOWN inside a control rect followed by an UP for the same finger: */
/* the tab flips only then. An event carrying window points (the old */
/* bug: x=107, y=791) computes a point of (38520, 632800), which    */
/* hits nothing, so the tab does not flip. This is what makes the    */
/* coordinate regression test fail on the old code.                   */
/* ------------------------------------------------------------------ */

static int point_in_tab(double wx, double wy) {
    return wx >= MOCK_TAB_X0 && wx <= MOCK_TAB_X1 &&
           wy >= MOCK_TAB_Y0 && wy <= MOCK_TAB_Y1;
}

/* Snapshot a live HID event into the async queue. Returns the queue
 * slot, or -1 when the queue is full (the event is dropped, like a
 * ring-full drop in touch.c). */
static int mock_async_snapshot(void *event) {
    mock_hid_event_t *ev = (mock_hid_event_t *)event;
    mock_queued_event_t *q;
    if (!ev || !ev->in_use) return -1;
    if (g_async_count >= MAX_ASYNC_QUEUE) return -1;
    q = &g_async_queue[g_async_count];
    memset(q, 0, sizeof(*q));
    q->src = event;
    q->context_id = ev->context_id;
    q->sender_id = ev->sender_id;
    g_last_received = *ev;
    for (int i = 0; i < ev->n_children && i < MAX_MOCK_CHILDREN; i++) {
        g_last_children[i] = *ev->children[i];
        mock_hid_event_t *child = ev->children[i];
        q->child[q->n_children].index = child->index;
        q->child[q->n_children].x = child->x;
        q->child[q->n_children].y = child->y;
        q->child[q->n_children].touch = child->touch;
        q->child[q->n_children].range = child->range;
        q->n_children++;
    }
    g_async_count++;
    return g_async_count - 1;
}

/* Drain-time delivery of one queued snapshot: the UIKit coordinate
 * contract (normalized coords times window size, hit-test the tab
 * rect) plus the context-id gate. A tap is a DOWN inside the tab
 * rect followed by an UP for the same finger. An event whose
 * context id does not match the key window _contextId never reaches
 * the window: sendEvent: fired, window sendEvent: never ran, tab did not switch. A zero context id
 * means touch.c found no window context to set, so it passes. */
static void mock_deliver_queued(mock_queued_event_t *q) {
    if (!q || q->delivered) return;
    q->delivered = 1;
    if (q->context_id != 0 && q->context_id != (int64_t)g_fake_context_id) {
        return;
    }
    for (int i = 0; i < q->n_children; i++) {
        double wx = q->child[i].x * MOCK_WIN_W;
        double wy = q->child[i].y * MOCK_WIN_H;
        uint32_t idx = q->child[i].index;
        if (idx >= MAX_MOCK_TOUCHES) continue;
        if (q->child[i].touch == 1) {
            if (point_in_tab(wx, wy)) g_down_in_tab[idx] = 1;
        } else {
            if (g_down_in_tab[idx]) {
                g_tab_selected_index = 1;
                g_down_in_tab[idx] = 0;
            }
        }
    }
}

/* _receiveHIDEventInternal: entry (wired via touch_test_ops_t, and
 * used by mock_run_fetch_thread to simulate the fetcher thread
 * executing the block _receiveHIDEvent: posted). Moves one event
 * from the raw queue to the filtered queue and signals the
 * dispatcher. Called with the live event during route B; the
 * snapshot lookup by pointer finds the wrapper snapshot on route A
 * (still queued) and snapshots directly on route B (no wrapper). */
static void mock_receive_internal(void *fetcher, void *event) {
    (void)fetcher;
    int slot = -1;
    for (int i = 0; i < g_async_count; i++) {
        if (g_async_queue[i].src == event && !g_async_queue[i].internal_done) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = mock_async_snapshot(event);
        if (slot < 0) return;
        g_raw_queue_count++;
    }
    g_direct_internal_count++;
    g_internal_receive_count++;
    g_async_queue[slot].internal_done = 1;
    if (g_raw_queue_count > 0) g_raw_queue_count--;
    g_filtered_queue_count++;
    mock_dispatcher_received();
}

/* Route A wrapper entry: -[UIEventFetcher _receiveHIDEvent:]. Device
 * faithful: it only retains the event and performs the block on the
 * fetch thread. The mock enqueues the snapshot (raw queue grows,
 * filtered stays 0) and returns WITHOUT draining. If nothing ever
 * runs the fetch thread or fires the main runloop source, the event
 * never reaches sendEvent: matching the device log. */
static void mock_wrapper_receive(void *fetcher, void *event) {
    (void)fetcher;
    g_wrapper_receive_count++;
    if (mock_async_snapshot(event) >= 0) {
        g_raw_queue_count++;
    }
}

/* Test-side pump, diagnostic only: run the fetcher thread over every
 * queued event that has not been internal'd yet (the block
 * _receiveHIDEvent: posted). This models iOS pumping the fetcher
 * thread; it is what the unpatched tree relies on and what never
 * happened on the device. Returns the number of events internal'd. */
static int mock_run_fetch_thread(void) {
    int n = 0;
    for (int i = 0; i < g_async_count; i++) {
        if (!g_async_queue[i].internal_done && !g_async_queue[i].delivered) {
            g_async_queue[i].internal_done = 1;
            g_internal_receive_count++;
            if (g_raw_queue_count > 0) g_raw_queue_count--;
            g_filtered_queue_count++;
            mock_dispatcher_received();
            n++;
        }
    }
    return n;
}

/* Test-side pump, diagnostic only: fire the dispatcher main-runloop
 * source, i.e. drainEventsIntoEnvironment:. Delivers every
 * internal'd snapshot to -[UIApplication sendEvent:] (through the
 * installed hook, so touch.c counts it) and applies the tab model.
 * Raw (not yet internal'd) events stay queued. */
static void mock_fire_main_runloop_source(void) {
    mock_drain_events(&g_fake_fetcher_obj, &g_fake_env_obj);
}

static void mock_dispatcher_received(void) {
    if (g_installed_dispatcher_hook) {
        g_installed_dispatcher_hook(&g_fake_disp_obj, &g_sel_dispatcher_received,
                                    &g_fake_fetcher_obj);
        return;
    }
    g_dispatcher_received_count++;
}

static void mock_dispatcher_orig(void *self, void *cmd, void *arg) {
    (void)self; (void)cmd; (void)arg;
    g_dispatcher_orig_count++;
}

static void mock_drain_orig(void *self, void *cmd, void *env) {
    (void)self; (void)cmd; (void)env;
    g_drain_orig_count++;
    /* Drain consumes the filtered queue into the environment. Raw
     * (not yet internal'd) events stay queued: on device the drain
     * only sees what _receiveHIDEventInternal: moved to filtered. */
    for (int i = 0; i < g_async_count; i++) {
        mock_queued_event_t *q = &g_async_queue[i];
        int finger_count;
        if (!q->internal_done || q->delivered) continue;
        finger_count = q->n_children > 0 ? q->n_children : 1;
        /* The touch count the sendEvent hook reports comes from the
         * delivered snapshot, not from a fixture. */
        g_fake_touch_count = (uint64_t)finger_count;
        g_drain_current_ctx = q->context_id;
        g_drain_current_is_hid = 1;
        mock_objc_msgSend(&g_fake_app_obj, &g_sel_send_event, &g_fake_event_obj,
                          NULL, 0.0, 0.0);
        g_drain_current_is_hid = 0;
        g_drain_current_ctx = 0;
        mock_deliver_queued(q);
    }
    /* Compact the queue: drop delivered snapshots. */
    {
        int w = 0;
        for (int i = 0; i < g_async_count; i++) {
            if (!g_async_queue[i].delivered) {
                if (w != i) g_async_queue[w] = g_async_queue[i];
                w++;
            }
        }
        g_async_count = w;
    }
    g_filtered_queue_count = 0;
    if (g_drain_call_send_event) {
        /* Legacy fixture path (kept for the copied touch_test cases
         * that set it): one extra sendEvent: with the fixture count. */
        mock_objc_msgSend(&g_fake_app_obj, &g_sel_send_event, &g_fake_event_obj,
                          NULL, 0.0, 0.0);
    }
}

static void mock_drain_events(void *fetcher, void *environment) {
    g_drain_events_count++;
    if (g_installed_drain_hook) {
        g_installed_drain_hook(fetcher, &g_sel_drain_events, environment);
        return;
    }
    mock_drain_orig(fetcher, NULL, environment);
}

static void mock_window_send_event_orig(void *self, void *cmd, void *event) {
    (void)self; (void)cmd; (void)event;
    g_window_send_event_count++;
}

static void mock_orig_send_event(void *self, void *cmd, void *event) {
    (void)self; (void)cmd;
    g_orig_send_event_called++;
    /* UIApplication sendEvent: forwards to the key window, unless this
     * is an HID delivery whose context id does not match the key
     * window _contextId: _UIEventDispatcher hit-tests by context id,
     * and a mismatch means the touch has no window (sendEvent:
     * reached with touch count 1, window sendEvent: never
     * ran). The direct path (UITouchesEvent, window set on the touch)
     * is never gated. */
    if (!g_drain_current_is_hid || g_drain_current_ctx == 0 ||
        g_drain_current_ctx == (int64_t)g_fake_context_id) {
        if (g_installed_window_send_event_hook) {
            g_installed_window_send_event_hook(&g_fake_win_obj, &g_sel_send_event, event);
        } else {
            mock_window_send_event_orig(&g_fake_win_obj, &g_sel_send_event, event);
        }
    }
    /* Direct-path tap model: an Ended UITouch on the Browse tab flips it.
     * Only fake UITouchesEvent pool objects are dereferenced; the HID
     * path's opaque event object (&g_fake_event_obj, a plain int) is not. */
    if (event && is_fake_touches_event(event)) {
        fake_touches_event_t *ev = (fake_touches_event_t *)event;
        if (ev->touches) {
            fake_nsset_t *set = (fake_nsset_t *)ev->touches;
            if (set->isa == &g_fake_nsset_cls && set->object) {
                fake_touch_t *t = (fake_touch_t *)set->object;
                if (t->isa == &g_fake_touch_cls && t->phase == 3 &&
                    point_in_tab(t->loc_x, t->loc_y)) {
                    g_tab_selected_index = 1;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime                                                  */
/* ------------------------------------------------------------------ */

static int is_fake_touch(void *obj) {
    for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
        if (obj == &g_fake_touch_pool[i]) return 1;
    }
    return 0;
}

static int is_fake_touches_event(void *obj) {
    if (obj == &g_kif_shared_event) return 1; /* KIF shared event. */
    for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
        if (obj == &g_fake_event_pool[i]) return 1;
    }
    return 0;
}

static int is_fake_nsset(void *obj) {
    if (obj == &g_kif_shared_set) return 1; /* KIF shared set. */
    for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
        if (obj == &g_fake_set_pool[i]) return 1;
    }
    return 0;
}

static void *mock_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "UIApplication") == 0) return &g_fake_app_cls;
    if (strcmp(name, "UIEventDispatcher") == 0) return &g_fake_disp_cls;
    if (strcmp(name, "UIEventFetcher") == 0) return &g_fake_fetcher_cls;
    if (strcmp(name, "UIWindow") == 0) return &g_fake_win_cls;
    if (strcmp(name, "UITabBarController") == 0) return &g_fake_tab_cls;
    if (strcmp(name, "UITouch") == 0) return &g_fake_touch_cls;
    if (strcmp(name, "UITouchesEvent") == 0) return &g_fake_touches_event_cls;
    if (strcmp(name, "NSSet") == 0) return &g_fake_nsset_cls;
    if (strcmp(name, "UIView") == 0) return &g_fake_view_cls;
    if (strcmp(name, "UIControl") == 0) return &g_fake_control_cls;
    return NULL;
}

static void *mock_object_getClass(void *obj) {
    if (!obj) return NULL;
    if (obj == &g_fake_app_obj) return &g_fake_app_cls;
    if (obj == &g_fake_disp_obj) return &g_fake_disp_cls;
    if (obj == &g_fake_fetcher_obj) return &g_fake_fetcher_cls;
    if (obj == &g_fake_win_obj) return &g_fake_win_cls;
    if (obj == &g_fake_tab_obj) return &g_fake_tab_cls;
    if (obj == &g_fake_view_obj) return &g_fake_view_cls;
    if (is_fake_touch(obj)) return &g_fake_touch_cls;
    if (is_fake_touches_event(obj)) return &g_fake_touches_event_cls;
    if (is_fake_nsset(obj)) return &g_fake_nsset_cls;
    return NULL;
}

static void *mock_class_getInstanceVariable(void *cls, const char *name) {
    if (!cls || !name) return NULL;
    if (cls == &g_fake_app_cls) {
        if (strcmp(name, "_eventFetcher") == 0) return (void *)(uintptr_t)1;
        if (strcmp(name, "_eventDispatcher") == 0) return (void *)(uintptr_t)2;
        if (strcmp(name, "_keyWindow") == 0) return (void *)(uintptr_t)3;
    }
    if (cls == &g_fake_disp_cls) {
        if (strcmp(name, "_environment") == 0) return (void *)(uintptr_t)4;
        if (strcmp(name, "_eventFetcher") == 0) return (void *)(uintptr_t)5;
        if (strcmp(name, "_mainRunLoopCFSource") == 0) return (void *)(uintptr_t)6;
        if (strcmp(name, "_mainRunLoop") == 0) return (void *)(uintptr_t)7;
    }
    if (cls == &g_fake_fetcher_cls) {
        if (strcmp(name, "_cfRunLoop") == 0) return (void *)(uintptr_t)8;
        if (strcmp(name, "_eventDispatcher") == 0) return (void *)(uintptr_t)9;
        if (strcmp(name, "_incomingHIDEventsFiltered") == 0) return (void *)(uintptr_t)21;
        if (strcmp(name, "_incomingHIDEvents") == 0) return (void *)(uintptr_t)22;
    }
    if (cls == &g_fake_touches_event_cls) {
        if (strcmp(name, "_touches") == 0 && !g_hide_touches_ivar) {
            return (void *)(uintptr_t)20;
        }
    }
    return NULL;
}

static long mock_ivar_getOffset(void *iv) {
    uintptr_t id = (uintptr_t)iv;
    if (id == 1) return (long)offsetof(fake_app_layout_t, eventFetcher);
    if (id == 2) return (long)offsetof(fake_app_layout_t, eventDispatcher);
    if (id == 3) return (long)offsetof(fake_app_layout_t, keyWindow);
    if (id == 4) return (long)offsetof(fake_disp_layout_t, environment);
    if (id == 5) return (long)offsetof(fake_disp_layout_t, eventFetcher);
    if (id == 6) return (long)offsetof(fake_disp_layout_t, mainRunLoopCFSource);
    if (id == 7) return (long)offsetof(fake_disp_layout_t, mainRunLoop);
    if (id == 8) return (long)offsetof(fake_fetcher_layout_t, cfRunLoop);
    if (id == 9) return (long)offsetof(fake_fetcher_layout_t, eventDispatcher);
    if (id == 20) return (long)offsetof(fake_touches_event_t, touches);
    if (id == 21) return (long)offsetof(fake_fetcher_layout_t, incomingFiltered);
    if (id == 22) return (long)offsetof(fake_fetcher_layout_t, incomingRaw);
    return 0;
}

static void *mock_object_setIvar(void *obj, void *iv, void *value) {
    if (!obj || !iv) return NULL;
    if (is_fake_touches_event(obj) && (uintptr_t)iv == 20) {
        fake_touches_event_t *ev = (fake_touches_event_t *)obj;
        void *old = ev->touches;
        ev->touches = value;
        return old;
    }
    return NULL;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    if (cls == &g_fake_app_cls && sel == &g_sel_send_event) {
        return (void *)(uintptr_t)0x8001;
    }
    if (cls == &g_fake_fetcher_cls && sel == &g_sel_drain_events) {
        return (void *)(uintptr_t)0x8002;
    }
    if (cls == &g_fake_disp_cls && sel == &g_sel_dispatcher_received) {
        return (void *)(uintptr_t)0x8003;
    }
    if (cls == &g_fake_win_cls && sel == &g_sel_send_event) {
        return (void *)(uintptr_t)0x8004;
    }
    return NULL;
}

static void *mock_method_setImplementation(void *m, void *imp) {
    if (m == (void *)(uintptr_t)0x8001) {
        g_installed_app_send_event_hook = (void (*)(void *, void *, void *))imp;
        return (void *)mock_orig_send_event;
    }
    if (m == (void *)(uintptr_t)0x8002) {
        g_installed_drain_hook = (void (*)(void *, void *, void *))imp;
        return (void *)mock_drain_orig;
    }
    if (m == (void *)(uintptr_t)0x8003) {
        g_installed_dispatcher_hook = (void (*)(void *, void *, void *))imp;
        return (void *)mock_dispatcher_orig;
    }
    if (m == (void *)(uintptr_t)0x8004) {
        g_installed_window_send_event_hook = (void (*)(void *, void *, void *))imp;
        return (void *)mock_window_send_event_orig;
    }
    return NULL;
}

static void mock_CFRunLoopSourceSignal(void *source) {
    if (source == &g_fake_source_obj) {
        g_signal_count++;
    }
}

static void mock_CFRunLoopWakeUp(void *rl) {
    if (rl == &g_fake_rl_obj) {
        g_wakeup_count++;
    }
}

static void *mock_CFRunLoopGetMain(void) {
    return &g_fake_rl_obj;
}

static void *mock_sel_registerName(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "respondsToSelector:") == 0) return &g_sel_responds;
    if (strcmp(name, "sharedApplication") == 0) return &g_sel_shared_app;
    if (strcmp(name, "_eventFetcher") == 0) return &g_sel_event_fetcher;
    if (strcmp(name, "_receiveHIDEvent:") == 0) return &g_sel_receive_hid;
    if (strcmp(name, "keyWindow") == 0) return &g_sel_key_window;
    if (strcmp(name, "windows") == 0) return &g_sel_windows;
    if (strcmp(name, "count") == 0) return &g_sel_count;
    if (strcmp(name, "objectAtIndex:") == 0) return &g_sel_object_at_index;
    if (strcmp(name, "connectedScenes") == 0) return &g_sel_connected_scenes;
    if (strcmp(name, "allObjects") == 0) return &g_sel_all_objects;
    if (strcmp(name, "rootViewController") == 0) return &g_sel_root_vc;
    if (strcmp(name, "tabBarController") == 0) return &g_sel_tab_bar_vc;
    if (strcmp(name, "selectedIndex") == 0) return &g_sel_selected_index;
    if (strcmp(name, "viewControllers") == 0) return &g_sel_view_controllers;
    if (strcmp(name, "childViewControllers") == 0) return &g_sel_child_vcs;
    if (strcmp(name, "allTouches") == 0) return &g_sel_all_touches;
    if (strcmp(name, "sendEvent:") == 0) return &g_sel_send_event;
    if (strcmp(name, "_contextId") == 0) return &g_sel_context_id;
    if (strcmp(name, "hitTest:withEvent:") == 0) return &g_sel_hit_test;
    if (strcmp(name, "setWindow:") == 0) return &g_sel_set_window;
    if (strcmp(name, "setView:") == 0) return &g_sel_set_view;
    if (strcmp(name, "setPhase:") == 0) return &g_sel_set_phase;
    if (strcmp(name, "setTimestamp:") == 0) return &g_sel_set_timestamp;
    if (strcmp(name, "setTapCount:") == 0) return &g_sel_set_tap_count;
    if (strcmp(name, "_setLocationInWindow:resetPrevious:") == 0) return &g_sel_set_location;
    if (strcmp(name, "_setHidEvent:") == 0) return &g_sel_set_hid_event;
    if (strcmp(name, "_setTouches:") == 0) return &g_sel_set_touches_priv;
    if (strcmp(name, "setTouches:") == 0) return &g_sel_set_touches;
    if (strcmp(name, "setWithObject:") == 0) return &g_sel_set_with_object;
    if (strcmp(name, "alloc") == 0) return &g_sel_alloc;
    if (strcmp(name, "init") == 0) return &g_sel_init;
    if (strcmp(name, "drainEventsIntoEnvironment:") == 0) return &g_sel_drain_events;
    if (strcmp(name, "eventFetcherDidReceiveEvents:") == 0) return &g_sel_dispatcher_received;
    if (strcmp(name, "_setTimestamp:") == 0) return &g_sel_set_event_timestamp;
    /* KIF route C selectors. */
    if (strcmp(name, "_touchesEvent") == 0) return &g_sel_touches_event;
    if (strcmp(name, "_clearTouches") == 0) return &g_sel_clear_touches;
    if (strcmp(name, "_addTouch:forDelayedDelivery:") == 0) return &g_sel_add_touch;
    if (strcmp(name, "_setHIDEvent:") == 0) return &g_sel_set_hid_event_kif;
    if (strcmp(name, "isKindOfClass:") == 0) return &g_sel_is_kind_of_class;
    if (strcmp(name, "superview") == 0) return &g_sel_superview;
    if (strcmp(name, "sendActionsForControlEvents:") == 0) return &g_sel_send_actions;
    if (strcmp(name, "retain") == 0) return &g_sel_retain;
    /* "release" was unmapped (NULL), so every
     * [touch release] in the tree was a no-op in this mock. */
    if (strcmp(name, "release") == 0) return &g_sel_release;
    return NULL;
}

/* Mixed GP/FP params: GP args arrive in rdx/rcx (a1/a2), FP args
 * (CGPoint struct, double) in xmm0/xmm1 (f1/f2). See the file header
 * note: this is only sound because touch.c is a separate TU. */
static void *mock_objc_msgSend(void *obj, void *sel, void *a1, void *a2,
                               double f1, double f2) {
    if (sel == &g_sel_responds) {
        void *target = a1;
        if (obj == &g_fake_app_cls && target == &g_sel_shared_app) return (void *)(uintptr_t)1;
        if (obj == &g_fake_app_obj && target == &g_sel_event_fetcher) return (void *)(uintptr_t)1;
        if (obj == &g_fake_app_obj && target == &g_sel_key_window) return (void *)(uintptr_t)1;
        if (obj == &g_fake_app_obj && target == &g_sel_touches_event) return (void *)(uintptr_t)1;
        if (obj == &g_fake_fetcher_obj && target == &g_sel_receive_hid) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && target == &g_sel_context_id) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && target == &g_sel_root_vc) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && target == &g_sel_hit_test) return (void *)(uintptr_t)1;
        if (obj == &g_fake_tab_obj && target == &g_sel_selected_index) return (void *)(uintptr_t)1;
        if (obj == &g_fake_event_obj && target == &g_sel_all_touches) return (void *)(uintptr_t)1;
        if (obj == &g_fake_touches_set && target == &g_sel_count) return (void *)(uintptr_t)1;
        if (obj == &g_fake_filtered_queue && target == &g_sel_count) return (void *)(uintptr_t)1;
        if (obj == &g_fake_raw_queue && target == &g_sel_count) return (void *)(uintptr_t)1;
        if (is_fake_nsset(obj) && target == &g_sel_count) return (void *)(uintptr_t)1;
        if (is_fake_touch(obj)) {
            if (g_hide_all_direct_setters) return NULL;
            if (target == &g_sel_set_window) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_view) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_phase) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_timestamp) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_tap_count) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_location && !g_hide_direct_setters) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_hid_event && !g_hide_direct_setters) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_location && g_hide_direct_setters) return NULL;
            if (target == &g_sel_set_hid_event && g_hide_direct_setters) return NULL;
        }
        if (is_fake_touches_event(obj)) {
            if (target == &g_sel_all_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_touches_priv && !g_hide_set_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_touches && !g_hide_set_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_event_timestamp) return (void *)(uintptr_t)1;
            /* KIF shared event: clear/add/setHID always respond. */
            if (target == &g_sel_clear_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_add_touch) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_hid_event_kif) return (void *)(uintptr_t)1;
        }
        return NULL;
    }
    if (obj == &g_fake_app_cls && sel == &g_sel_shared_app) {
        return &g_fake_app_obj;
    }
    if (obj == &g_fake_app_obj && sel == &g_sel_event_fetcher) {
        return g_fetcher_absent ? NULL : &g_fake_fetcher_obj;
    }
    if (obj == &g_fake_app_obj && sel == &g_sel_key_window) {
        return &g_fake_win_obj;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_context_id) {
        return (void *)(uintptr_t)g_fake_context_id;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_root_vc) {
        return &g_fake_tab_obj;
    }
    if (obj == &g_fake_tab_obj && sel == &g_sel_selected_index) {
        return (void *)(uintptr_t)g_tab_selected_index;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_hit_test) {
        g_hittest_count++;
        g_last_hittest_x = f1;
        g_last_hittest_y = f2;
        return &g_fake_view_obj;
    }
    if (obj == &g_fake_fetcher_obj && sel == &g_sel_receive_hid) {
        mock_wrapper_receive(obj, a1);
        return NULL;
    }
    /* KIF route C: the app's shared UITouchesEvent,
     * cleared/attached per phase, plus the interim TouchUpInside
     * hack walk (isKindOfClass: UIControl, superview, sendActions-
     * ForControlEvents:). The hit view is always the mock hit view;
     * whether it has a UIControl ancestor is g_kif_hit_is_control,
     * set by the test per tap. superview returns NULL, so the walk
     * ends after one step when there is no control ancestor. */
    if (obj == &g_fake_app_obj && sel == &g_sel_touches_event) {
        return &g_kif_shared_event;
    }
    if (obj == &g_kif_shared_event && sel == &g_sel_clear_touches) {
        g_kif_shared_event.touches = NULL;
        return NULL;
    }
    if (obj == &g_kif_shared_event && sel == &g_sel_set_hid_event_kif) {
        (void)a1;
        return NULL;
    }
    if (obj == &g_kif_shared_event && sel == &g_sel_add_touch) {
        g_kif_shared_set.object = a1;
        g_kif_shared_event.touches = &g_kif_shared_set;
        return NULL;
    }
    if (sel == &g_sel_is_kind_of_class) {
        return (void *)(uintptr_t)(g_kif_hit_is_control ? 1 : 0);
    }
    if (sel == &g_sel_superview) {
        return NULL;
    }
    if (sel == &g_sel_send_actions) {
        g_touchupinside_count++;
        return NULL;
    }
    if (is_fake_touch(obj) && sel == &g_sel_retain) {
        /* Bump the count; alloc sets it to 1. */
        ((fake_touch_t *)obj)->refcount++;
        return obj;
    }
    if (is_fake_touch(obj) && sel == &g_sel_release) {
        /* Release frees at refcount 0; guarded against underflow
         * (a release of an already-freed slot is a no-op, never a double free). */
        fake_touch_t *t = (fake_touch_t *)obj;
        if (t->refcount > 0) t->refcount--;
        if (t->refcount == 0) {
            for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
                if (obj == &g_fake_touch_pool[i]) {
                    g_fake_touch_pool_used[i] = 0;
                    break;
                }
            }
        }
        return NULL;
    }
    if (obj == &g_fake_app_obj && sel == &g_sel_send_event) {
        if (is_fake_touches_event(a1)) g_direct_send_event_count++;
        if (g_installed_app_send_event_hook) {
            g_installed_app_send_event_hook(obj, sel, a1);
        } else {
            mock_orig_send_event(obj, sel, a1);
        }
        return NULL;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_send_event) {
        if (g_installed_window_send_event_hook) {
            g_installed_window_send_event_hook(obj, sel, a1);
        } else {
            mock_window_send_event_orig(obj, sel, a1);
        }
        return NULL;
    }
    if ((obj == &g_fake_event_obj || is_fake_touches_event(obj)) && sel == &g_sel_all_touches) {
        if (obj == &g_fake_event_obj) return &g_fake_touches_set;
        return ((fake_touches_event_t *)obj)->touches;
    }
    if (sel == &g_sel_count) {
        if (obj == &g_fake_touches_set) return (void *)(uintptr_t)g_fake_touch_count;
        if (obj == &g_fake_filtered_queue) return (void *)(uintptr_t)g_filtered_queue_count;
        if (obj == &g_fake_raw_queue) return (void *)(uintptr_t)g_raw_queue_count;
        if (is_fake_nsset(obj)) {
            return (void *)(uintptr_t)(((fake_nsset_t *)obj)->object ? 1 : 0);
        }
    }
    if (sel == &g_sel_alloc) {
        if (obj == &g_fake_touch_cls) {
            for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
                if (!g_fake_touch_pool_used[i]) {
                    g_fake_touch_pool_used[i] = 1;
                    memset(&g_fake_touch_pool[i], 0, sizeof(fake_touch_t));
                    g_fake_touch_pool[i].isa = &g_fake_touch_cls;
                    /* Alloc owns the +1, as in real ObjC. */
                    g_fake_touch_pool[i].refcount = 1;
                    g_touch_alloc_count++;
                    return &g_fake_touch_pool[i];
                }
            }
            return NULL;
        }
        if (obj == &g_fake_touches_event_cls) {
            for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
                if (!g_fake_event_pool_used[i]) {
                    g_fake_event_pool_used[i] = 1;
                    memset(&g_fake_event_pool[i], 0, sizeof(fake_touches_event_t));
                    g_fake_event_pool[i].isa = &g_fake_touches_event_cls;
                    return &g_fake_event_pool[i];
                }
            }
            return NULL;
        }
    }
    if (sel == &g_sel_init) {
        return obj;
    }
    if (obj == &g_fake_nsset_cls && sel == &g_sel_set_with_object) {
        for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
            if (!g_fake_set_pool_used[i]) {
                g_fake_set_pool_used[i] = 1;
                memset(&g_fake_set_pool[i], 0, sizeof(fake_nsset_t));
                g_fake_set_pool[i].isa = &g_fake_nsset_cls;
                g_fake_set_pool[i].object = a1;
                return &g_fake_set_pool[i];
            }
        }
        return NULL;
    }
    if (is_fake_touch(obj)) {
        fake_touch_t *t = (fake_touch_t *)obj;
        if (sel == &g_sel_set_window) { t->window = a1; return NULL; }
        if (sel == &g_sel_set_view) { t->view = a1; return NULL; }
        if (sel == &g_sel_set_phase) {
            t->phase = (long)a1;
            g_last_direct_touch = t;
            g_last_direct_phase = t->phase;
            if (g_phase_seq_n < 16) g_phase_seq[g_phase_seq_n++] = t->phase;
            return NULL;
        }
        if (sel == &g_sel_set_timestamp) {
            t->timestamp = f1;
            g_set_timestamp_count++;
            g_last_touch_timestamp = f1;
            return NULL;
        }
        if (sel == &g_sel_set_tap_count) { t->tapCount = (unsigned long)a1; return NULL; }
        if (sel == &g_sel_set_location) {
            t->loc_x = f1;
            t->loc_y = f2;
            t->loc_set = 1;
            t->loc_reset = (int)(long)a1;
            g_set_location_count++;
            if (g_loc_seq_n < 16) {
                g_loc_seq_x[g_loc_seq_n] = f1;
                g_loc_seq_y[g_loc_seq_n] = f2;
                g_loc_reset_seq[g_loc_seq_n] = (int)(long)a1;
                g_loc_seq_n++;
            }
            return NULL;
        }
        if (sel == &g_sel_set_hid_event) { t->hidEvent = a1; return NULL; }
    }
    if (is_fake_touches_event(obj)) {
        fake_touches_event_t *ev = (fake_touches_event_t *)obj;
        if (sel == &g_sel_set_touches_priv || sel == &g_sel_set_touches) {
            ev->touches = a1;
            return NULL;
        }
        if (sel == &g_sel_set_event_timestamp) {
            ev->timestamp = f1;
            return NULL;
        }
    }
    return NULL;
}

static void *mock_pool_push(void) { return (void *)(uintptr_t)0x1234; }
static void mock_pool_pop(void *p) { (void)p; }

static void mock_dispatch_async_f(void *q, void *ctx, void (*work)(void *)) {
    (void)q;
    g_dispatch_calls++;
    if (work) work(ctx);
}

static int mock_pthread_create(void *t, const void *a, void *(*fn)(void *), void *arg) {
    (void)t; (void)a;
    if (fn) {
        fn(arg);
    }
    return 0;
}

static void *mock_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "_dispatch_main_q") == 0) return &g_fake_main_q;
    if (strcmp(name, "dispatch_async_f") == 0) return (void *)mock_dispatch_async_f;
    if (strcmp(name, "pthread_create") == 0) return (void *)mock_pthread_create;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "object_getClass") == 0) return (void *)mock_object_getClass;
    if (strcmp(name, "class_getInstanceVariable") == 0) return (void *)mock_class_getInstanceVariable;
    if (strcmp(name, "ivar_getOffset") == 0) return (void *)mock_ivar_getOffset;
    if (strcmp(name, "object_setIvar") == 0) return (void *)mock_object_setIvar;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    if (strcmp(name, "CFRunLoopSourceSignal") == 0) return (void *)mock_CFRunLoopSourceSignal;
    if (strcmp(name, "CFRunLoopWakeUp") == 0) return (void *)mock_CFRunLoopWakeUp;
    if (strcmp(name, "CFRunLoopGetMain") == 0) return (void *)mock_CFRunLoopGetMain;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "CFRelease") == 0) return (void *)mock_CFRelease;
    if (strcmp(name, "IOHIDEventCreateDigitizerEvent") == 0)
        return (void *)mock_IOHIDEventCreateDigitizerEvent;
    if (strcmp(name, "IOHIDEventCreateDigitizerFingerEvent") == 0)
        return (void *)mock_IOHIDEventCreateDigitizerFingerEvent;
    if (strcmp(name, "IOHIDEventAppendEvent") == 0) return (void *)mock_IOHIDEventAppendEvent;
    if (strcmp(name, "IOHIDEventSetIntegerValue") == 0)
        return (void *)mock_IOHIDEventSetIntegerValue;
    if (strcmp(name, "IOHIDEventSetSenderID") == 0) return (void *)mock_IOHIDEventSetSenderID;
    if (strcmp(name, "BKSHIDEventSetDigitizerInfo") == 0)
        return (void *)mock_BKSHIDEventSetDigitizerInfo;
    return NULL;
}

static void *mock_dlopen(const char *path) {
    (void)path;
    return (void *)(uintptr_t)0x1;
}

static const char *mock_getenv(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "IOSWINE_TOUCH") == 0) return g_env_touch;
    if (strcmp(name, "IOSWINE_TOUCH_TRACE") == 0) return g_env_trace;
    if (strcmp(name, "IOSWINE_TOUCH_SELFTEST") == 0) return g_env_selftest;
    if (strcmp(name, "IOSWINE_TOUCH_DIRECT") == 0) return g_env_direct;
    if (strcmp(name, "IOSWINE_TOUCH_INTERNAL") == 0) return g_env_internal;
    if (strcmp(name, "IOSWINE_TOUCH_SENDER_ID") == 0) return g_env_sender;
    return NULL;
}

static uint64_t mock_mach_abs_time(void) {
    return 0x123456789ULL;
}

static int mock_timebase(uint32_t *numer, uint32_t *denom) {
    if (!numer || !denom) return -1;
    *numer = 1;
    *denom = 1;
    return 0;
}

static void mock_log(const char *msg, size_t len) {
    if (!msg || len == 0) return;
    size_t room = sizeof(g_log_buf) - 1 - g_log_len;
    size_t take = len < room ? len : room;
    if (take > 0) {
        memcpy(g_log_buf + g_log_len, msg, take);
        g_log_len += take;
        g_log_buf[g_log_len] = '\0';
    }
}

/* Shared ops + fixture for tests built on this mock. Route D is the
 * default (no DIRECT/QUEUE env): touch_inject
 * delivers synchronously through _receiveHIDEventInternal:. Route A
 * (IOSWINE_TOUCH_QUEUE=1) still enqueues only through the wrapper, so
 * a test that injects on route A without pumping sees the device
 * outcome. */
static const touch_test_ops_t mock_async_ops_base = {
    .dlsym_default = mock_dlsym,
    .dlopen_fn = mock_dlopen,
    .getenv_fn = mock_getenv,
    .mach_abs_time = mock_mach_abs_time,
    .log_fn = mock_log,
    .receive_internal_fn = mock_receive_internal,
    .drain_events_fn = mock_drain_events,
    .timebase_fn = mock_timebase,
};

/* App + dispatcher + fetcher + window wired together, as in
 * touch_test.c setup_full_fixture. */
static void mock_async_setup_fixture(void) {
    g_fake_app_obj.eventDispatcher = &g_fake_disp_obj;
    g_fake_app_obj.eventFetcher = &g_fake_fetcher_obj;
    g_fake_app_obj.keyWindow = &g_fake_win_obj;
    g_fake_disp_obj.environment = &g_fake_env_obj;
    g_fake_disp_obj.eventFetcher = &g_fake_fetcher_obj;
    g_fake_disp_obj.mainRunLoopCFSource = &g_fake_source_obj;
    g_fake_disp_obj.mainRunLoop = &g_fake_rl_obj;
    g_fake_fetcher_obj.cfRunLoop = &g_fake_rl_obj;
    g_fake_fetcher_obj.eventDispatcher = &g_fake_disp_obj;
}
