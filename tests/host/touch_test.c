/*
 * touch_test.c - Host unit tests for touch.c + ISWS_MSG_TOUCH round-trip.
 *
 * Plain cc + ASan/UBSan on Linux host. Tests:
 *   1. ISWS_MSG_TOUCH protocol encode/decode round-trip and validation
 *   2. touch_map_coords normalization and window-point mapping
 *   3. IOSWINE_TOUCH=1 env gating (and that DIRECT/INTERNAL alone do not enable)
 *   4. IOHIDEvent construction, multi-touch, cancel, logging, CFRelease balance
 *   5. Socket loopback through present.c -> touch_test_process_touch
 *   6. IOSWINE_TOUCH_TRACE=1 stage logging (fetcher thread, queued via
 *      _receiveHIDEvent:, dispatcher, drain, sendEvent: incl. count 0,
 *      window sendEvent:) & context ID & sender ID matrix
 *   7. IOSWINE_TOUCH_SELFTEST=1 tap injection after frame 20 & tab switch logging
 *   8. Coordinate regression: digitizer events carry NORMALIZED [0,1]
 *      coordinates (finding 1); the old code passed window points and the
 *      old test enshrined that. The mock models real UIKit: it multiplies
 *      normalized coordinates by the window size and hit-tests the result.
 *   9. Delivery routes: A (_receiveHIDEvent: wrapper, behind
 *      IOSWINE_TOUCH_QUEUE=1), D (combined path, default, and
 *      IOSWINE_TOUCH_INTERNAL=1 as an alias), C (IOSWINE_TOUCH_DIRECT=1,
 *      UITouchesEvent + sendEvent: fallback, also the fallback of D
 *      when the HID chain drops an event)
 *  10. Trace hooks and the _incomingHIDEventsFiltered queue probe
 *  11. Coordinate fuzz (10000 random touches)
 *  12. Route recombination: the combined path calls Internal
 *      once per phase, signals, drains, runs processEventQueue(env, 4)
 *      and counts sendEvent:; the fallback fires exactly when the HID
 *      chain delivers nothing; route A is kept behind its env flag
 *  13. UIControl tracking: the mock window re-hit-tests
 *      Began touches from locationInWindow and runs a UIControl
 *      tracking machine (begin/continue/end/cancel + action) on the
 *      tab button, so a tap, drag-out, hold and cancel are asserted
 *      end to end; the KIF path ([UIApplication _touchesEvent]) is
 *      now exercised on host, and the device model (a UITouch setter
 *      that does not resolve) is covered by the ivar fallbacks.
 *      The interim TouchUpInside hack is counted separately from
 *      tracking actions and is off by default.
 *
 * NOTE on the mock objc_msgSend: it is declared with mixed GP/FP params
 * (void*, void*, void*, void*, double, double) so it can observe the
 * CGPoint struct and double args the direct path passes through
 * objc_msgSend. This works ONLY because touch.c lives in a different
 * translation unit from this file (no LTO in host_tests.sh): touch.c
 * classifies args per its cast types at the call site, and this mock
 * reads them back per its own prototype. Do not merge this mock into
 * touch.c's translation unit, and do not enable LTO for this test: a
 * same-TU build lets the compiler devirtualize the calls and the FP
 * args read as garbage (verified: a same-TU probe reads point (0,0)
 * at -O2, a cross-TU probe reads (107,791) at -O1 and -O2).
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "present.h"
#include "protocol.h"
#include "touch.h"

extern void present_test_set_socket_name(const char *name);

/* ------------------------------------------------------------------ */
/* Test harness                                                       */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

/* Window size the mock UIKit uses (points), matching the device state. */
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
    uint64_t sender_id;
    /* Context id stamped by BKSHIDEventSetDigitizerInfo
     * (the real setter); bks_calls counts the calls on this event. */
    uint32_t bks_context_id;
    int bks_calls;
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
    /* Previous location (written by the ivar fallback
     * for _previousLocationInWindow). */
    double prev_loc_x;
    double prev_loc_y;
    int prev_loc_set;
    void *hidEvent;
    /* Fields for the restored setters. */
    int is_first_touch_for_view;
    double initial_timestamp;
    /* Retain count. The mock used to ignore retain /
     * release entirely, so every allocated touch leaked its pool slot
     * and the 16-slot pool exhausted after 16 taps; the 240-tap storm
     * then failed every later DOWN (alloc NULL) even though the tree
     * releases correctly. Real ObjC frees at refcount 0, so the mock
     * does too now. Set to 1 by alloc, bumped by retain, freed by
     * release at 0. */
    int refcount;
} fake_touch_t;

/* Fake UITouchesEvent object. */
typedef struct {
    void *isa;
    void *touches;
    double timestamp;
    /* HID event attached by the KIF path's _setHIDEvent:. */
    void *hidEvent;
} fake_touches_event_t;

/* Fake UIControl (the Browse tab button). The mock window's sendEvent:
 * runs a minimal UIControl tracking machine on it: beginTracking: on
 * touchesBegan:, continueTracking: on touchesMoved:,
 * endTracking: on touchesEnded: (fires the action when the touch is
 * still inside), cancelTracking on touchesCancelled:. action_count
 * counts actions fired by tracking; hack_count counts direct
 * sendActionsForControlEvents: calls (the interim hack,
 * which bypasses tracking). */
typedef struct {
    void *isa;
    void *superview;
    int tracking;
    int highlighted;
    int begin_count;
    int continue_count;
    int end_count;
    int cancel_count;
    int action_count;
    int hack_count;
    unsigned long last_hack_events;
} fake_control_t;

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
static int g_fake_layer_cls;
static int g_fake_cactx_cls;
/* The UIControl class the tab button belongs to. */
static int g_fake_uicontrol_cls;

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
/* The window's layer and its CAContext, plus the tab-bar
 * button view hit-testing returns for a tap on the tab.
 * The tab view is a fake UIControl (tracking state machine). */
static int g_fake_layer_obj;
static int g_fake_cactx_obj;
static fake_control_t g_fake_tab_view_obj;
/* The app's shared UITouchesEvent ([UIApplication
 * _touchesEvent]), reused by the KIF path. */
static fake_touches_event_t g_fake_app_event;
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
/* Digitizer and window selectors. */
static int g_sel_layer;
static int g_sel_context;
static int g_sel_layer_context_id;
static int g_sel_window;
static int g_sel_view;
static int g_sel_set_is_first_touch;
static int g_sel_set_initial_ts;
/* Control and event selectors. */
static int g_sel_touches_event;
static int g_sel_clear_touches;
static int g_sel_add_touch;
static int g_sel_retain;
static int g_sel_release;
static int g_sel_is_kind;
static int g_sel_superview;
static int g_sel_send_actions;

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
static const char *g_env_queue = NULL;
static const char *g_env_sender = NULL;
/* Context env values. */
static const char *g_env_ctx_id = NULL;
static const char *g_env_ctx_field = NULL;
/* Tap hack env value. */
static const char *g_env_tap_hack = NULL;
static char g_log_buf[8192];
static size_t g_log_len = 0;

static int g_wrapper_receive_count = 0;
static int g_direct_internal_count = 0;
static int g_internal_receive_count = 0;
static int g_signal_count = 0;
static int g_wakeup_count = 0;
static int g_drain_events_count = 0;
static int g_drain_orig_count = 0;
/* The drain only moves events into the environment; processEventQueue
 * is the step that dispatches them to sendEvent: (mirroring real
 * UIKit). g_drain_call_send_event gates whether processEventQueue
 * delivers a sendEvent: for a staged event. */
static int g_process_event_queue_count = 0;
static uint32_t g_last_process_limit = 0;
static int g_env_pending = 0; /* events staged in the environment by the drain */
static int g_dispatcher_received_count = 0;
static int g_dispatcher_orig_count = 0;
static int g_orig_send_event_called = 0;
static int g_window_send_event_count = 0;
static uint32_t g_fake_context_id = 0;
/* The dispatcher's source of truth, [[win layer] context].contextId.
 * g_fake_context_id is what -[UIWindow _contextId] returns; the two
 * differ on device (garbage vs real id). g_fake_context_id_2nd, when
 * non-zero, is what the second _contextId read returns (models the
 * device's non-deterministic value). */
static uint32_t g_fake_layer_context_id = 0;
static uint32_t g_fake_context_id_2nd = 0;
static int g_context_id_reads = 0;
/* Last BKSHIDEventSetDigitizerInfo call observed. */
static uint32_t g_bks_ctx_id = 0;
static int g_bks_ctx_calls = 0;
/* Hide flags for the route C fallback tests. */
static int g_hide_window_setter = 0;
static int g_hide_view_setter = 0;
static int g_hide_touch_ivars = 0;
/* Whether the last HID event's context id matched the window (set by
 * mock_uikit_model, read by mock_orig_send_event). */
static int g_last_hid_ctx_match = 1;
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
/* UIControl tracking counters (mock window model) and
 * the interim-hack counter. g_tab_action_count counts every tab
 * action fire, tracking or hack: 2 after one tap means double-fire. */
static int g_track_begin = 0;
static int g_track_continue = 0;
static int g_track_end = 0;
static int g_track_cancel = 0;
static int g_track_action = 0;
static int g_hack_fire = 0;
static unsigned long g_hack_events = 0;
static int g_tab_action_count = 0;
/* Hide exactly one UITouch setter (device-model knobs). */
static int g_hide_location_setter = 0;
static int g_hide_phase_setter = 0;
/* Hide _addTouch:forDelayedDelivery: (KIF attach). */
static int g_hide_add_touch = 0;

static void (*g_installed_drain_hook)(void *, void *, void *) = NULL;
static void (*g_installed_dispatcher_hook)(void *, void *, void *) = NULL;
static void (*g_installed_app_send_event_hook)(void *, void *, void *) = NULL;
static void (*g_installed_window_send_event_hook)(void *, void *, void *) = NULL;

static void mock_orig_send_event(void *self, void *cmd, void *event);
static void mock_drain_orig(void *self, void *cmd, void *env);
static void mock_dispatcher_orig(void *self, void *cmd, void *arg);
static void mock_window_send_event_orig(void *self, void *cmd, void *event);
static void mock_drain_events(void *fetcher, void *environment);
static void mock_process_event_queue(void *environment, uint32_t limit);
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
    g_env_queue = NULL;
    g_env_sender = NULL;
    g_env_ctx_id = NULL;
    g_env_ctx_field = NULL;
    g_env_tap_hack = NULL;
    memset(g_log_buf, 0, sizeof(g_log_buf));
    g_log_len = 0;

    g_wrapper_receive_count = 0;
    g_direct_internal_count = 0;
    g_internal_receive_count = 0;
    g_signal_count = 0;
    g_wakeup_count = 0;
    g_drain_events_count = 0;
    g_drain_orig_count = 0;
    g_process_event_queue_count = 0;
    g_last_process_limit = 0;
    g_env_pending = 0;
    g_dispatcher_received_count = 0;
    g_dispatcher_orig_count = 0;
    g_orig_send_event_called = 0;
    g_window_send_event_count = 0;
    g_fake_context_id = 0;
    g_fake_layer_context_id = 0;
    g_fake_context_id_2nd = 0;
    g_context_id_reads = 0;
    g_bks_ctx_id = 0;
    g_bks_ctx_calls = 0;
    g_hide_window_setter = 0;
    g_hide_view_setter = 0;
    g_hide_touch_ivars = 0;
    g_last_hid_ctx_match = 1;
    g_tab_selected_index = 0;
    g_fake_touch_count = 0;
    g_drain_call_send_event = 0;
    g_hide_direct_setters = 0;
    g_hide_touches_ivar = 0;
    g_filtered_queue_count = 0;
    g_raw_queue_count = 0;
    memset(g_down_in_tab, 0, sizeof(g_down_in_tab));

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
    g_phase_seq_n = 0;
    g_loc_seq_n = 0;
    g_track_begin = 0;
    g_track_continue = 0;
    g_track_end = 0;
    g_track_cancel = 0;
    g_track_action = 0;
    g_hack_fire = 0;
    g_hack_events = 0;
    g_tab_action_count = 0;
    g_hide_location_setter = 0;
    g_hide_phase_setter = 0;
    g_hide_add_touch = 0;
    memset(&g_fake_tab_view_obj, 0, sizeof(g_fake_tab_view_obj));
    g_fake_tab_view_obj.isa = &g_fake_uicontrol_cls;
    g_fake_tab_view_obj.superview = &g_fake_win_obj;
    memset(&g_fake_app_event, 0, sizeof(g_fake_app_event));
    g_fake_app_event.isa = &g_fake_touches_event_cls;

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

/* BKSHIDEventSetDigitizerInfo mock. Records the context id
 * on the event, the way the real BackBoardServices call stamps the
 * digitizer attributes. */
static void mock_BKSHIDEventSetDigitizerInfo(void *ev_v, uint32_t contextID,
                                             uint8_t sysGestures, uint8_t sysChange,
                                             void *displayUUID, double initTs,
                                             float maxForce) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    (void)sysGestures; (void)sysChange; (void)displayUUID; (void)initTs; (void)maxForce;
    if (!ev) return;
    ev->bks_context_id = contextID;
    ev->bks_calls++;
    g_bks_ctx_id = contextID;
    g_bks_ctx_calls++;
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
/*                                                                    */
/* The mock ALSO models the dispatcher window lookup.                */
/* Real UIKit resolves the touch's window from the event's context    */
/* id against the key window's layer CAContext id [guess on the exact */
/* lookup; the drop stage is device-observed]. The event's            */
/* effective context id is the BKSHIDEventSetDigitizerInfo value when */
/* present, else the 0xb0005 field, else 0 (no info). A non-zero      */
/* effective id that does not match the window's layer context id     */
/* finds no window: the event still reaches the fetcher and sendEvent:*/
/* (touch count 1) but no -[UIWindow sendEvent:] runs and the         */
/* tab never flips. A zero effective id falls back to the key window; */
/* that fallback is unmodeled real behavior, kept so tests            */
/* which carry no context id still pass.                              */
/* ------------------------------------------------------------------ */

static int point_in_tab(double wx, double wy) {
    return wx >= MOCK_TAB_X0 && wx <= MOCK_TAB_X1 &&
           wy >= MOCK_TAB_Y0 && wy <= MOCK_TAB_Y1;
}

static void mock_uikit_model(void *fetcher, void *event) {
    (void)fetcher;
    mock_hid_event_t *ev = (mock_hid_event_t *)event;
    uint32_t eff_ctx = 0;
    g_internal_receive_count++;
    g_filtered_queue_count++;
    g_raw_queue_count++;
    if (!ev) return;
    g_last_received = *ev;
    if (ev->bks_calls > 0) {
        eff_ctx = ev->bks_context_id;
    } else if (ev->context_id != 0) {
        eff_ctx = (uint32_t)ev->context_id;
    }
    /* Window lookup: a match is required when the event carries an id. */
    g_last_hid_ctx_match = (eff_ctx == 0 || eff_ctx == g_fake_layer_context_id) ? 1 : 0;
    if (!g_last_hid_ctx_match) {
        /* The touch has no window: sendEvent: still fires with the
         * touch, but nothing is delivered to a view. */
        return;
    }
    for (int i = 0; i < ev->n_children && i < MAX_MOCK_CHILDREN; i++) {
        g_last_children[i] = *ev->children[i];
        mock_hid_event_t *child = ev->children[i];
        double wx = child->x * MOCK_WIN_W;
        double wy = child->y * MOCK_WIN_H;
        uint32_t idx = child->index;
        if (idx >= MAX_MOCK_TOUCHES) continue;
        if (child->touch == 1) {
            if (point_in_tab(wx, wy)) g_down_in_tab[idx] = 1;
        } else {
            if (g_down_in_tab[idx]) {
                g_tab_selected_index = 1;
                g_down_in_tab[idx] = 0;
            }
        }
    }
}

/* Route B direct Internal entry (wired via touch_test_ops_t). */
static void mock_receive_internal(void *fetcher, void *event) {
    g_direct_internal_count++;
    mock_uikit_model(fetcher, event);
}

/* Route A wrapper entry: -[UIEventFetcher _receiveHIDEvent:]. In real
 * UIKit this retains and performs the block on the fetch thread; the
 * mock runs the whole fetch-thread + dispatcher + drain + process-queue
 * chain synchronously so the test can observe it end to end. The drain
 * stages the event in the environment; processEventQueue is what
 * dispatches it to sendEvent:. */
static void mock_wrapper_receive(void *fetcher, void *event) {
    g_wrapper_receive_count++;
    mock_uikit_model(fetcher, event);
    mock_dispatcher_received();
    mock_drain_events(fetcher, &g_fake_env_obj);
    mock_process_event_queue(&g_fake_env_obj, 4);
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
    /* Drain consumes the fetcher queue into the environment: the events
     * are staged there, not delivered. processEventQueue dispatches
     * them to sendEvent: (see mock_process_event_queue). */
    g_filtered_queue_count = 0;
    g_raw_queue_count = 0;
    g_env_pending = 1;
}

/* -[UIEventEnvironment processEventQueue:]: pulls staged events out of
 * the environment and dispatches them to -[UIApplication sendEvent:].
 * Delivers only when the drain staged an event and the fixture allows
 * delivery (g_drain_call_send_event); otherwise the event is dropped
 * upstream and the combined route's sendEvent fallback must fire. */
static void mock_process_event_queue(void *environment, uint32_t limit) {
    (void)environment;
    g_process_event_queue_count++;
    g_last_process_limit = limit;
    if (g_env_pending && g_drain_call_send_event) {
        g_env_pending = 0;
        /* The touch count the sendEvent hook reports comes from the
         * fixture (g_fake_touch_count), not from the drain: the zero
         * test sets it to 0 on purpose. */
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

/* The tab button's action: what a TouchUpInside on it does. */
static void mock_tab_button_action(void) {
    g_tab_selected_index = 1;
    g_tab_action_count++;
}

/* Minimal -[UIWindow sendEvent:] model for the direct path,
 * mirroring real UIKit: Began touches are hit-tested from
 * locationInWindow and assigned a view (overwriting whatever the
 * injector set), then touches are delivered to that view by phase.
 * The tab button runs a UIControl tracking machine: begin on Began,
 * continue on Moved (highlight follows touchInside), end on Ended
 * (fires the action when still inside), cancel on Cancelled. A touch
 * whose location was never set reads (0,0), hit-tests the window,
 * and never reaches the control: the device failure, in host form. */
static void mock_tracking_deliver(void *event) {
    fake_touches_event_t *ev = (fake_touches_event_t *)event;
    fake_touch_t *t = NULL;
    fake_control_t *c;
    if (ev->touches) {
        fake_nsset_t *set = (fake_nsset_t *)ev->touches;
        if (set->isa == &g_fake_nsset_cls && set->object &&
            ((fake_touch_t *)set->object)->isa == &g_fake_touch_cls) {
            t = (fake_touch_t *)set->object;
        }
    }
    if (!t) return;
    /* Record the touch for assertions that read the last delivered
     * touch. The setter handlers do this too, but a touch whose
     * setters are all hidden (ivar fallbacks only) never passes
     * through one. */
    if (!g_last_direct_touch) g_last_direct_touch = t;
    if (t->phase == 0) {
        /* Began: the window hit-tests from locationInWindow. */
        if (point_in_tab(t->loc_x, t->loc_y)) {
            t->view = &g_fake_tab_view_obj;
        } else {
            t->view = &g_fake_win_obj;
        }
    }
    if (t->view != &g_fake_tab_view_obj) return;
    c = &g_fake_tab_view_obj;
    if (t->phase == 0) {
        c->tracking = 1;
        c->highlighted = 1;
        c->begin_count++;
        g_track_begin++;
    } else if (t->phase == 1) {
        if (!c->tracking) return;
        c->continue_count++;
        g_track_continue++;
        c->highlighted = point_in_tab(t->loc_x, t->loc_y) ? 1 : 0;
    } else if (t->phase == 3) {
        if (!c->tracking) return;
        c->end_count++;
        g_track_end++;
        if (point_in_tab(t->loc_x, t->loc_y)) {
            c->action_count++;
            g_track_action++;
            mock_tab_button_action();
        }
        c->tracking = 0;
        c->highlighted = 0;
    } else if (t->phase == 4) {
        if (!c->tracking) return;
        c->cancel_count++;
        g_track_cancel++;
        c->tracking = 0;
        c->highlighted = 0;
    }
}

static void mock_window_send_event_orig(void *self, void *cmd, void *event) {
    (void)self; (void)cmd;
    g_window_send_event_count++;
    if (event && is_fake_touches_event(event)) {
        mock_tracking_deliver(event);
    }
}

static void mock_orig_send_event(void *self, void *cmd, void *event) {
    (void)self; (void)cmd;
    g_orig_send_event_called++;
    /* UIApplication sendEvent: forwards to the touch's window.
     * The HID path forwards only when the dispatcher's
     * window lookup matched (g_last_hid_ctx_match); the direct path
     * forwards only when the UITouch carries a window. A touch with
     * no window is dropped silently here, matching device observations:
     * -[UIApplication sendEvent:] fires, -[UIWindow sendEvent:] does
     * not, and the tab never flips. */
    if (event && is_fake_touches_event(event)) {
        fake_touches_event_t *ev = (fake_touches_event_t *)event;
        fake_touch_t *t = NULL;
        if (ev->touches) {
            fake_nsset_t *set = (fake_nsset_t *)ev->touches;
            if (set->isa == &g_fake_nsset_cls && set->object &&
                ((fake_touch_t *)set->object)->isa == &g_fake_touch_cls) {
                t = (fake_touch_t *)set->object;
            }
        }
        if (t && t->window) {
            if (g_installed_window_send_event_hook) {
                g_installed_window_send_event_hook(&g_fake_win_obj, &g_sel_send_event, event);
            } else {
                mock_window_send_event_orig(&g_fake_win_obj, &g_sel_send_event, event);
            }
            /* The tab flip moved into the mock window's
             * UIControl tracking machine (mock_tracking_deliver):
             * an Ended touch flips the tab only when tracking ran
             * and ended inside. */
        }
        return;
    }
    /* HID path: the event is an opaque object (&g_fake_event_obj, a
     * plain int); the window lookup result comes from the dispatcher
     * model. */
    if (g_last_hid_ctx_match) {
        if (g_installed_window_send_event_hook) {
            g_installed_window_send_event_hook(&g_fake_win_obj, &g_sel_send_event, event);
        } else {
            mock_window_send_event_orig(&g_fake_win_obj, &g_sel_send_event, event);
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
    if (obj == &g_fake_app_event) return 1;
    for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
        if (obj == &g_fake_event_pool[i]) return 1;
    }
    return 0;
}

static int is_fake_nsset(void *obj) {
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
    if (strcmp(name, "UIControl") == 0) return &g_fake_uicontrol_cls;
    if (strcmp(name, "NSSet") == 0) return &g_fake_nsset_cls;
    if (strcmp(name, "UIView") == 0) return &g_fake_view_cls;
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
    if (cls == &g_fake_touch_cls && !g_hide_touch_ivars) {
        if (strcmp(name, "_window") == 0) return (void *)(uintptr_t)30;
        if (strcmp(name, "_view") == 0) return (void *)(uintptr_t)31;
        /* Scalar touch-state ivars for the fallback
         * (touch_set_touch_scalar_ivar). */
        if (strcmp(name, "_phase") == 0) return (void *)(uintptr_t)40;
        if (strcmp(name, "_locationInWindow") == 0) return (void *)(uintptr_t)41;
        if (strcmp(name, "_previousLocationInWindow") == 0) return (void *)(uintptr_t)42;
        if (strcmp(name, "_timestamp") == 0) return (void *)(uintptr_t)43;
        if (strcmp(name, "_tapCount") == 0) return (void *)(uintptr_t)44;
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
    if (id == 30) return (long)offsetof(fake_touch_t, window);
    if (id == 31) return (long)offsetof(fake_touch_t, view);
    /* _locationInWindow is the (loc_x, loc_y) pair, laid
     * out as two adjacent doubles, so a CGPoint-sized write at the
     * loc_x offset lands exactly. */
    if (id == 40) return (long)offsetof(fake_touch_t, phase);
    if (id == 41) return (long)offsetof(fake_touch_t, loc_x);
    if (id == 42) return (long)offsetof(fake_touch_t, prev_loc_x);
    if (id == 43) return (long)offsetof(fake_touch_t, timestamp);
    if (id == 44) return (long)offsetof(fake_touch_t, tapCount);
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
    if (is_fake_touch(obj)) {
        fake_touch_t *t = (fake_touch_t *)obj;
        if ((uintptr_t)iv == 30) {
            void *old = t->window;
            t->window = value;
            return old;
        }
        if ((uintptr_t)iv == 31) {
            void *old = t->view;
            t->view = value;
            return old;
        }
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
    if (strcmp(name, "layer") == 0) return &g_sel_layer;
    if (strcmp(name, "context") == 0) return &g_sel_context;
    if (strcmp(name, "contextId") == 0) return &g_sel_layer_context_id;
    if (strcmp(name, "window") == 0) return &g_sel_window;
    if (strcmp(name, "view") == 0) return &g_sel_view;
    if (strcmp(name, "_setIsFirstTouchForView:") == 0) return &g_sel_set_is_first_touch;
    if (strcmp(name, "_setInitialTouchTimestamp:") == 0) return &g_sel_set_initial_ts;
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
    /* KIF path + hack selectors. */
    if (strcmp(name, "_touchesEvent") == 0) return &g_sel_touches_event;
    if (strcmp(name, "_clearTouches") == 0) return &g_sel_clear_touches;
    if (strcmp(name, "_addTouch:forDelayedDelivery:") == 0) return &g_sel_add_touch;
    if (strcmp(name, "retain") == 0) return &g_sel_retain;
    if (strcmp(name, "release") == 0) return &g_sel_release;
    if (strcmp(name, "isKindOfClass:") == 0) return &g_sel_is_kind;
    if (strcmp(name, "superview") == 0) return &g_sel_superview;
    if (strcmp(name, "sendActionsForControlEvents:") == 0) return &g_sel_send_actions;
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
        if (obj == &g_fake_fetcher_obj && target == &g_sel_receive_hid) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && target == &g_sel_context_id) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && target == &g_sel_layer) return (void *)(uintptr_t)1;
        if (obj == &g_fake_layer_obj && target == &g_sel_context) return (void *)(uintptr_t)1;
        if (obj == &g_fake_cactx_obj && target == &g_sel_layer_context_id) return (void *)(uintptr_t)1;
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
            if (target == &g_sel_set_window && !g_hide_window_setter) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_view && !g_hide_view_setter) return (void *)(uintptr_t)1;
            if (target == &g_sel_window) return (void *)(uintptr_t)1;
            if (target == &g_sel_view) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_phase && !g_hide_phase_setter) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_timestamp) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_tap_count) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_is_first_touch) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_initial_ts) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_location && !g_hide_direct_setters && !g_hide_location_setter) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_hid_event && !g_hide_direct_setters) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_location && (g_hide_direct_setters || g_hide_location_setter)) return NULL;
            if (target == &g_sel_set_hid_event && g_hide_direct_setters) return NULL;
        }
        if (obj == &g_fake_app_obj && target == &g_sel_touches_event) return (void *)(uintptr_t)1;
        if (is_fake_touches_event(obj)) {
            if (target == &g_sel_clear_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_add_touch && !g_hide_add_touch) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_hid_event) return (void *)(uintptr_t)1;
        }
        if (is_fake_touches_event(obj)) {
            if (target == &g_sel_all_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_touches_priv && !g_hide_set_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_touches && !g_hide_set_touches) return (void *)(uintptr_t)1;
            if (target == &g_sel_set_event_timestamp) return (void *)(uintptr_t)1;
        }
        return NULL;
    }
    if (obj == &g_fake_app_cls && sel == &g_sel_shared_app) {
        return &g_fake_app_obj;
    }
    if (obj == &g_fake_app_obj && sel == &g_sel_event_fetcher) {
        return g_fetcher_absent ? NULL : &g_fake_fetcher_obj;
    }
    /* The app's shared UITouchesEvent for the KIF path. */
    if (obj == &g_fake_app_obj && sel == &g_sel_touches_event) {
        return &g_fake_app_event;
    }
    if (is_fake_touches_event(obj)) {
        fake_touches_event_t *ev = (fake_touches_event_t *)obj;
        if (sel == &g_sel_clear_touches) {
            ev->touches = NULL;
            return NULL;
        }
        if (sel == &g_sel_add_touch) {
            /* _addTouch:forDelayedDelivery:: the touch joins the
             * event's touches set (a fresh one-object NSSet). */
            for (int i = 0; i < MAX_MOCK_TOUCHES; i++) {
                if (!g_fake_set_pool_used[i]) {
                    g_fake_set_pool_used[i] = 1;
                    memset(&g_fake_set_pool[i], 0, sizeof(fake_nsset_t));
                    g_fake_set_pool[i].isa = &g_fake_nsset_cls;
                    g_fake_set_pool[i].object = a1;
                    ev->touches = &g_fake_set_pool[i];
                    return NULL;
                }
            }
            return NULL;
        }
        if (sel == &g_sel_set_hid_event) {
            ev->hidEvent = a1;
            return NULL;
        }
    }
    if (obj == &g_fake_tab_view_obj && sel == &g_sel_is_kind) {
        /* The tab button is a UIControl. */
        return (a1 == &g_fake_uicontrol_cls) ? (void *)(uintptr_t)1 : NULL;
    }
    if (obj == &g_fake_tab_view_obj && sel == &g_sel_superview) {
        return &g_fake_win_obj;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_superview) {
        return NULL;
    }
    if (obj == &g_fake_tab_view_obj && sel == &g_sel_send_actions) {
        /* The interim hack: fires the control's action directly,
         * bypassing tracking. Count it separately from tracking
         * actions so the tests can tell them apart. */
        fake_control_t *c = &g_fake_tab_view_obj;
        c->hack_count++;
        g_hack_fire++;
        g_hack_events = (unsigned long)a1;
        mock_tab_button_action();
        return NULL;
    }
    if (obj == &g_fake_app_obj && sel == &g_sel_key_window) {
        return &g_fake_win_obj;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_context_id) {
        /* Models the device's non-deterministic _contextId:
         * the first read returns g_fake_context_id; later reads return
         * g_fake_context_id_2nd when set. */
        g_context_id_reads++;
        if (g_context_id_reads > 1 && g_fake_context_id_2nd != 0) {
            return (void *)(uintptr_t)g_fake_context_id_2nd;
        }
        return (void *)(uintptr_t)g_fake_context_id;
    }
    if (obj == &g_fake_win_obj && sel == &g_sel_layer) {
        return &g_fake_layer_obj;
    }
    if (obj == &g_fake_layer_obj && sel == &g_sel_context) {
        return &g_fake_cactx_obj;
    }
    if (obj == &g_fake_cactx_obj && sel == &g_sel_layer_context_id) {
        return (void *)(uintptr_t)g_fake_layer_context_id;
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
        /* A point on the tab bar hits the tab button view; anywhere
         * else hits the window itself (no subview). */
        if (point_in_tab(f1, f2)) return &g_fake_tab_view_obj;
        return &g_fake_win_obj;
    }
    if (obj == &g_fake_fetcher_obj && sel == &g_sel_receive_hid) {
        mock_wrapper_receive(obj, a1);
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
        /* Retain / release for fake touches. These
         * used to fall through to the final return NULL, so the
         * tree's DOWN retain and UP releases were no-ops and the
         * touch pool leaked a slot per tap. Real ObjC refcounts:
         * retain bumps, release drops, and the slot is freed at 0
         * so the pool cycles across a storm. The refcount is
         * guarded against underflow (a release of an already-freed
         * slot is a no-op, never a double free); is_fake_touch
         * matches any pool pointer, live or freed, so a stale
         * retain can only bump a count the next alloc resets. */
        if (sel == &g_sel_retain) {
            t->refcount++;
            return obj;
        }
        if (sel == &g_sel_release) {
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
        if (sel == &g_sel_set_window) { t->window = a1; return NULL; }
        if (sel == &g_sel_set_view) { t->view = a1; return NULL; }
        if (sel == &g_sel_window) { return t->window; }
        if (sel == &g_sel_view) { return t->view; }
        if (sel == &g_sel_set_is_first_touch) {
            t->is_first_touch_for_view = (int)(long)a1;
            return NULL;
        }
        if (sel == &g_sel_set_initial_ts) {
            t->initial_timestamp = f1;
            return NULL;
        }
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
    if (strcmp(name, "IOSWINE_TOUCH_QUEUE") == 0) return g_env_queue;
    if (strcmp(name, "IOSWINE_TOUCH_SENDER_ID") == 0) return g_env_sender;
    if (strcmp(name, "IOSWINE_TOUCH_CTX_ID") == 0) return g_env_ctx_id;
    if (strcmp(name, "IOSWINE_TOUCH_CTX_FIELD") == 0) return g_env_ctx_field;
    if (strcmp(name, "IOSWINE_TOUCH_TAP_HACK") == 0) return g_env_tap_hack;
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

static const touch_test_ops_t g_ops = {
    .dlsym_default = mock_dlsym,
    .dlopen_fn = mock_dlopen,
    .getenv_fn = mock_getenv,
    .mach_abs_time = mock_mach_abs_time,
    .log_fn = mock_log,
    .receive_internal_fn = NULL,
    .drain_events_fn = NULL,
    .process_event_queue_fn = NULL,
    .timebase_fn = mock_timebase,
};

/* Shared fixture: app + dispatcher + fetcher + window wired together. */
static void setup_full_fixture(void) {
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

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_protocol_touch_roundtrip(void) {
    uint8_t msg[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
    const uint32_t actions[4] = {
        ISWS_TOUCH_DOWN, ISWS_TOUCH_MOVE, ISWS_TOUCH_UP, ISWS_TOUCH_CANCEL
    };
    for (int i = 0; i < 4; i++) {
        isws_touch_t in = {
            .action = actions[i],
            .pointer_id = (uint32_t)(i * 2),
            .pointer_count = (uint32_t)(i + 1),
            .x = 123.75f + (float)i,
            .y = 456.25f - (float)i,
            .timestamp_ns = 9876543210LL + i
        };
        isws_touch_t out;
        uint16_t type = 0, flags = 0;
        uint32_t plen = 0;
        size_t hlen = isws_encode_header(msg, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE);
        size_t n = isws_encode_touch(msg + ISWS_HEADER_SIZE, ISWS_TOUCH_PAYLOAD_SIZE, &in);
        CHECK(hlen == ISWS_HEADER_SIZE);
        CHECK(n == ISWS_TOUCH_PAYLOAD_SIZE);
        CHECK(isws_decode_header(msg, sizeof(msg), &type, &flags, &plen) == 0);
        CHECK(type == ISWS_MSG_TOUCH && flags == 0 && plen == ISWS_TOUCH_PAYLOAD_SIZE);
        CHECK(isws_decode_touch(msg + ISWS_HEADER_SIZE, plen, &out) == 0);
        CHECK(out.action == in.action);
        CHECK(out.pointer_id == in.pointer_id);
        CHECK(out.pointer_count == in.pointer_count);
        CHECK_NEAR(out.x, in.x, 1e-5);
        CHECK_NEAR(out.y, in.y, 1e-5);
        CHECK(out.timestamp_ns == in.timestamp_ns);
    }

    /* Validation rejections */
    {
        uint8_t buf[ISWS_TOUCH_PAYLOAD_SIZE];
        isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                           .pointer_count = 1, .x = 10.0f, .y = 20.0f,
                           .timestamp_ns = 1 };
        isws_touch_t out;
        CHECK(isws_encode_touch(buf, ISWS_TOUCH_PAYLOAD_SIZE - 1, &t) == 0);
        CHECK(isws_encode_touch(buf, sizeof(buf), &t) == ISWS_TOUCH_PAYLOAD_SIZE);
        CHECK(isws_decode_touch(buf, ISWS_TOUCH_PAYLOAD_SIZE - 1, &out) == -1);
        isws_wr_u32le(buf + 0, 9); /* invalid action */
        CHECK(isws_decode_touch(buf, sizeof(buf), &out) == -1);
        isws_wr_u32le(buf + 0, ISWS_TOUCH_MOVE);
        isws_wr_u32le(buf + 4, ISWS_MAX_POINTERS); /* invalid pointer_id */
        CHECK(isws_decode_touch(buf, sizeof(buf), &out) == -1);
        isws_wr_u32le(buf + 4, 0);
        isws_wr_u32le(buf + 8, 0); /* invalid pointer_count */
        CHECK(isws_decode_touch(buf, sizeof(buf), &out) == -1);
    }
}

static void test_coord_mapping(void) {
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 0 };
    double nx = 0, ny = 0, wx = 0, wy = 0;
    CHECK(touch_map_coords(&t, 1080, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK_NEAR(nx, 0.5, 1e-6);
    CHECK_NEAR(ny, 0.5, 1e-6);
    CHECK_NEAR(wx, 180.0, 1e-6);
    CHECK_NEAR(wy, 400.0, 1e-6);

    /* Clamping out-of-range finite coords */
    t.x = -20.0f;
    t.y = 5000.0f;
    CHECK(touch_map_coords(&t, 1080, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK_NEAR(nx, 0.0, 1e-6);
    CHECK_NEAR(ny, 1.0, 1e-6);
    CHECK_NEAR(wx, 0.0, 1e-6);
    CHECK_NEAR(wy, 800.0, 1e-6);

    /* Reject invalid inputs */
    CHECK(touch_map_coords(NULL, 1080, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    t.x = 10.0f; t.y = 10.0f;
    CHECK(touch_map_coords(&t, 0, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 1080, 0, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    CHECK(touch_map_coords(&t, 1080, 2400, 0.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    t.x = NAN;
    CHECK(touch_map_coords(&t, 1080, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
    t.x = INFINITY;
    CHECK(touch_map_coords(&t, 1080, 2400, 360.0, 800.0, &nx, &ny, &wx, &wy) == -1);
}

static void test_env_gating(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);

    g_env_touch = NULL;
    touch_init();
    CHECK(g_log_len == 0);

    g_env_touch = "0";
    touch_init();
    CHECK(g_log_len == 0);

    /* DIRECT/INTERNAL/QUEUE alone do not enable touch. */
    g_env_direct = "1";
    touch_init();
    CHECK(g_log_len == 0);
    g_env_direct = NULL;
    g_env_internal = "1";
    touch_init();
    CHECK(g_log_len == 0);
    g_env_internal = NULL;
    g_env_queue = "1";
    touch_init();
    CHECK(g_log_len == 0);
    g_env_queue = NULL;

    g_env_touch = "1";
    touch_init();
    CHECK(strstr(g_log_buf, "[touch] reader started") != NULL);
}

static void test_single_tap_and_cancel(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_env_touch = "1";
    g_env_queue = "1"; /* route A, pinned: the default is route D now */
    g_drain_call_send_event = 1;
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* DOWN at (540, 1200) in 1080x2400 -> normalized (0.5, 0.5). Route A. */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 10 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_dispatch_calls == 1);
    CHECK(g_wrapper_receive_count == 1);
    CHECK(g_direct_internal_count == 0);
    CHECK(g_drain_events_count == 1);
    CHECK(g_orig_send_event_called == 1);
    CHECK(touch_test_fingers_down() == 1);
    CHECK(g_last_received.transducer_type == 3);
    CHECK(g_last_received.event_mask == 0x7u);
    CHECK(g_last_received.range == 1 && g_last_received.touch == 1);
    CHECK(g_last_received.disp_integrated == 1);
    /* No context id in this fixture, no sender override: sender is 0. */
    CHECK(g_last_received.sender_id == 0);
    CHECK(g_last_received.n_children == 1);
    CHECK(g_last_children[0].index == 0);
    CHECK(g_last_children[0].identity == 2);
    CHECK(g_last_children[0].event_mask == 0x7u);
    CHECK(g_last_children[0].range == 1 && g_last_children[0].touch == 1);
    CHECK_NEAR(g_last_children[0].pressure, 1.0, 1e-6);
    /* Finding 1: normalized coordinates, not window points. */
    CHECK_NEAR(g_last_received.x, 0.5, 1e-6);
    CHECK_NEAR(g_last_received.y, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].x, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.5, 1e-6);
    CHECK(strstr(g_log_buf, "[touch] down x=180 y=400 id=0\n") != NULL);

    /* Duplicate MOVE at identical coords is suppressed */
    t.action = ISWS_TOUCH_MOVE;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 1);

    /* MOVE to (270, 600) in 1080x2400 -> normalized (0.25, 0.25) */
    t.x = 270.0f; t.y = 600.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 2);
    CHECK(g_last_received.event_mask == 0x4u);
    CHECK(g_last_children[0].event_mask == 0x4u);
    CHECK(g_last_children[0].range == 1 && g_last_children[0].touch == 1);
    CHECK_NEAR(g_last_children[0].x, 0.25, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.25, 1e-6);
    CHECK(strstr(g_log_buf, "[touch] move x=90 y=200 id=0\n") != NULL);

    /* UP at (90, 200) points */
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 3);
    CHECK(touch_test_fingers_down() == 0);
    CHECK(g_last_received.event_mask == 0x3u);
    CHECK(g_last_received.touch == 0);
    CHECK(g_last_children[0].event_mask == 0x3u);
    CHECK(g_last_children[0].range == 0 && g_last_children[0].touch == 0);
    CHECK_NEAR(g_last_children[0].pressure, 0.0, 1e-6);
    CHECK(strstr(g_log_buf, "[touch] up x=90 y=200 id=0\n") != NULL);

    /* Stray MOVE/UP when finger not down is ignored */
    t.action = ISWS_TOUCH_MOVE;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 3);

    /* DOWN then CANCEL */
    t.action = ISWS_TOUCH_DOWN; t.pointer_id = 1; t.x = 540.0f; t.y = 1200.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(touch_test_fingers_down() == 1);
    t.action = ISWS_TOUCH_CANCEL;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(touch_test_fingers_down() == 0);
    CHECK(g_wrapper_receive_count == 5);
    CHECK(g_last_received.event_mask == 0x83u);
    CHECK(g_last_children[0].event_mask == 0x83u);
    CHECK(g_last_children[0].range == 0 && g_last_children[0].touch == 0);
    CHECK(strstr(g_log_buf, "[touch] cancel x=180 y=400 id=1\n") != NULL);

    /* All IOHIDEvents released cleanly */
    CHECK(g_created_count == g_released_count);
}

static void test_multitouch_and_window_resize(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_env_touch = "1";
    touch_init();
    touch_set_window_size(400.0, 1000.0);

    isws_touch_t t0 = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                        .pointer_count = 1, .x = 270.0f, .y = 600.0f, .timestamp_ns = 1 };
    isws_touch_t t1 = { .action = ISWS_TOUCH_DOWN, .pointer_id = 2,
                        .pointer_count = 2, .x = 810.0f, .y = 1800.0f, .timestamp_ns = 2 };
    CHECK(touch_test_process_touch(&t0, 1080, 2400) == 0);
    CHECK(touch_test_process_touch(&t1, 1080, 2400) == 0);
    CHECK(touch_test_fingers_down() == 2);
    CHECK(g_last_received.n_children == 2);
    CHECK(g_last_received.event_mask == 0x7u);
    /* Child 0 = held finger 0, Child 1 = down finger 2; normalized. */
    CHECK(g_last_children[0].index == 0);
    CHECK(g_last_children[0].event_mask == 0x3u);
    CHECK_NEAR(g_last_children[0].x, 0.25, 1e-5);
    CHECK_NEAR(g_last_children[0].y, 0.25, 1e-5);
    CHECK(g_last_children[1].index == 2);
    CHECK(g_last_children[1].event_mask == 0x7u);
    CHECK_NEAR(g_last_children[1].x, 0.75, 1e-5);
    CHECK_NEAR(g_last_children[1].y, 0.75, 1e-5);

    /* Release finger 2: parent touch stays 1 because finger 0 is still down */
    t1.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t1, 1080, 2400) == 0);
    CHECK(touch_test_fingers_down() == 1);
    CHECK(g_last_received.n_children == 2);
    CHECK(g_last_received.touch == 1);
    CHECK(g_last_children[0].touch == 1);
    CHECK(g_last_children[1].touch == 0);

    /* Release finger 0 */
    t0.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t0, 1080, 2400) == 0);
    CHECK(touch_test_fingers_down() == 0);
    CHECK(g_last_received.n_children == 1);
    CHECK(g_last_received.touch == 0);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Loopback socket test through present.c                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int listen_fd;
    pthread_t th;
} loopback_srv_t;

static int srv_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int srv_send_msg(int fd, uint16_t type, const uint8_t *pay, uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    if (srv_send_all(fd, hdr, sizeof(hdr)) != 0) return -1;
    if (len > 0 && srv_send_all(fd, pay, len) != 0) return -1;
    return 0;
}

static void *loopback_srv_fn(void *arg) {
    loopback_srv_t *s = (loopback_srv_t *)arg;
    int fd = accept(s->listen_fd, NULL, NULL);
    uint8_t buf[256];
    struct msghdr mh;
    struct iovec iov = { .iov_base = buf, .iov_len = ISWS_HEADER_SIZE + ISWS_HELLO_PAYLOAD_SIZE };
    uint8_t cbuf[CMSG_SPACE(sizeof(int))];
    if (fd < 0) return NULL;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    if (recvmsg(fd, &mh, 0) > 0) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int mfd;
                memcpy(&mfd, CMSG_DATA(c), sizeof(mfd));
                if (mfd >= 0) close(mfd);
            }
        }
        isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0, .max_in_flight = 2 };
        uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
        isws_encode_ready(rb, sizeof(rb), &rd);
        srv_send_msg(fd, ISWS_MSG_READY, rb, sizeof(rb));

        isws_info_t info;
        isws_compute_mapping(1080, 2400, 1080, 2400, 0, &info);
        uint8_t ib[ISWS_INFO_PAYLOAD_SIZE];
        isws_encode_info(ib, sizeof(ib), &info);
        srv_send_msg(fd, ISWS_MSG_INFO, ib, sizeof(ib));

        isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                           .pointer_count = 1, .x = 270.0f, .y = 600.0f, .timestamp_ns = 42 };
        uint8_t tb[ISWS_TOUCH_PAYLOAD_SIZE];
        isws_encode_touch(tb, sizeof(tb), &t);
        srv_send_msg(fd, ISWS_MSG_TOUCH, tb, sizeof(tb));

        /* Drain until client closes */
        while (recv(fd, buf, sizeof(buf), 0) > 0) {}
    }
    close(fd);
    return NULL;
}

static void test_socket_loopback(void) {
    char sname[64];
    struct sockaddr_un addr;
    loopback_srv_t srv;
    int got_touch = 0;

    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_env_touch = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    snprintf(sname, sizeof(sname), "ioswine-touch-test-%d", (int)getpid());
    present_test_set_socket_name(sname);

    srv.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(srv.listen_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, sname, strlen(sname));
    CHECK(bind(srv.listen_fd, (struct sockaddr *)&addr,
               (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sname))) == 0);
    CHECK(listen(srv.listen_fd, 1) == 0);
    CHECK(pthread_create(&srv.th, NULL, loopback_srv_fn, &srv) == 0);

    CHECK(present_init_timeout(1080, 2400, 1000) == 0);
    for (int tries = 0; tries < 20 && !got_touch; tries++) {
        present_event_t ev;
        if (present_wait_readable(50) > 0) {
            CHECK(present_pump() == 0);
        } else {
            CHECK(present_pump() == 0);
        }
        while (present_poll_event(&ev)) {
            if (ev.kind == PRESENT_EV_TOUCH) {
                isws_info_t info = present_last_info();
                uint32_t cw = info.client_w ? info.client_w : 1080;
                uint32_t ch = info.client_h ? info.client_h : 2400;
                CHECK(touch_test_process_touch(&ev.u.touch, cw, ch) == 0);
                got_touch = 1;
            }
        }
    }
    CHECK(got_touch == 1);
    CHECK(g_wrapper_receive_count == 1);
    /* Normalized (0.25, 0.25), not window points (90, 200). */
    CHECK_NEAR(g_last_children[0].x, 0.25, 1e-5);
    CHECK_NEAR(g_last_children[0].y, 0.25, 1e-5);
    CHECK(strstr(g_log_buf, "[touch] down x=90 y=200 id=0\n") != NULL);

    present_shutdown();
    pthread_join(srv.th, NULL);
    close(srv.listen_fd);
}

/* ------------------------------------------------------------------ */
/* IOSWINE_TOUCH_TRACE=1 & context ID verification (route A)          */
/* ------------------------------------------------------------------ */

static void test_touch_trace_and_context_id(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();

    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device: layer id agrees */
    g_fake_touch_count = 1;
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_queue = "1"; /* route A, pinned: the default is route D now */
    g_env_ctx_field = "1"; /* the 0xb0005 field is opt-in now */
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = {
        .action = ISWS_TOUCH_DOWN,
        .pointer_id = 0,
        .pointer_count = 1,
        .x = 540.0f,
        .y = 1200.0f,
        .timestamp_ns = 100
    };

    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* Route A: wrapper, not the direct Internal call, no manual signal. */
    CHECK(g_wrapper_receive_count == 1);
    CHECK(g_direct_internal_count == 0);
    CHECK(g_signal_count == 0);
    CHECK(g_wakeup_count == 0);
    CHECK(g_drain_events_count == 1);
    CHECK(g_drain_orig_count == 1);
    CHECK(g_dispatcher_orig_count == 1);
    CHECK(g_orig_send_event_called == 1);
    CHECK(g_window_send_event_count == 1);

    /* Display integrated and context ID are set on parent & child. */
    CHECK(g_last_received.disp_integrated == 1);
    CHECK(g_last_received.context_id == 0x12345);
    CHECK(g_last_received.n_children == 1);
    CHECK(g_last_children[0].disp_integrated == 1);
    CHECK(g_last_children[0].context_id == 0x12345);
    /* The context id is stamped by
     * BKSHIDEventSetDigitizerInfo (the real setter), and the resolution
     * is trace-logged with both candidates. */
    CHECK(g_last_received.bks_context_id == 0x12345);
    CHECK(g_bks_ctx_calls == 1);
    CHECK(strstr(g_log_buf, "[touch] ctx win=74565 win2=74565 layer=74565 use=74565\n") != NULL);
    /* Sender ID defaults to the window context id (finding 3). */
    CHECK(g_last_received.sender_id == 0x12345);

    /* Normalized coordinates. */
    CHECK_NEAR(g_last_children[0].x, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.5, 1e-6);

    /* Trace stages for route A. */
    CHECK(strstr(g_log_buf, "[touch] fetcher thread present? 1\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] down x=180 y=400 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] queued via _receiveHIDEvent:\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] sender id 74565 ctx=74565\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] dispatcher received events\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] drain events\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] queue filtered=1") != NULL);
    CHECK(strstr(g_log_buf, "[touch] sendEvent: reached with touch count 1\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] window sendEvent:\n") != NULL);
    /* Route B lines must NOT appear on route A. */
    CHECK(strstr(g_log_buf, "[touch] source signalled") == NULL);
    CHECK(strstr(g_log_buf, "[touch] internal receive") == NULL);

    /* No [touch] dbg logs */
    CHECK(strstr(g_log_buf, "[touch] dbg") == NULL);

    /* Release balance */
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* sendEvent: hook logs even with zero touches (finding 5)            */
/* ------------------------------------------------------------------ */

static void test_hooks_log_zero_touch(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device */
    g_fake_touch_count = 0; /* empty event */
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_queue = "1"; /* route A, pinned: the default is route D now */
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 100 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_orig_send_event_called == 1);
    /* The old hook logged nothing for count 0; the new one must. */
    CHECK(strstr(g_log_buf, "[touch] sendEvent: reached with touch count 0\n") != NULL);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* IOSWINE_TOUCH_SELFTEST=1 tab switch test (route A)                 */
/* ------------------------------------------------------------------ */

static void test_touch_selftest_tab_switch(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();

    g_fake_context_id = 0x54321u;
    g_fake_layer_context_id = 0x54321u; /* healthy device */
    g_tab_selected_index = 0; /* initially tab 0 (Library/History) */
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_selftest = "1";
    g_env_trace = "1";
    g_env_queue = "1"; /* route A, pinned: the default is route D now */
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* Frames other than 20 should be ignored */
    touch_on_frame(19);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);
    touch_on_frame(21);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);

    /* Frame 20 triggers the self-test: synthesizes tap at (107, 791) */
    touch_on_frame(20);

    /* Tab switch executed and logged */
    CHECK(strstr(g_log_buf, "[touch] down x=107 y=791 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] up x=107 y=791 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1\n") != NULL);
    CHECK(g_wrapper_receive_count == 2);

    /* Repeated frame 20 does not re-run */
    size_t len_before = g_log_len;
    touch_on_frame(20);
    CHECK(g_log_len == len_before);
}

/* ------------------------------------------------------------------ */
/* Coordinate regression (finding 1)                                  */
/* The old code passed window points to the digitizer constructors.   */
/* The mock models real UIKit: normalized in, points out. A tap at    */
/* client (321, 2373) must carry normalized (0.2972, 0.9888) and the  */
/* mock must compute window point (107, 791) and flip the tab. The    */
/* old code fails this test: it carries (107, 791) as "normalized",   */
/* the mock computes (38520, 632800), and the tab never flips.        */
/* ------------------------------------------------------------------ */

static void test_coord_fix_regression(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_env_touch = "1";
    g_drain_call_send_event = 1;
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* Selftest tap point, client pixels in 1080x2400. */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK_NEAR(g_last_children[0].x, 321.0 / 1080.0, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 2373.0 / 2400.0, 1e-6);
    /* The old buggy values must not appear anywhere in the event. */
    CHECK(g_last_children[0].x < 1.0);
    CHECK(g_last_children[0].y < 1.0);
    CHECK(g_last_received.x < 1.0);
    CHECK(g_last_received.y < 1.0);
    CHECK(g_last_children[0].x != 107.0);
    CHECK(g_last_children[0].y != 791.0);

    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* The mock UIKit hit-tests (107, 791) and flips the tab. */
    CHECK(g_tab_selected_index == 1);

    /* Sweep: every corner and the center stay in [0,1]. */
    static const float pts[6][2] = {
        { 0.0f, 0.0f }, { 1080.0f, 0.0f }, { 0.0f, 2400.0f },
        { 1080.0f, 2400.0f }, { 540.0f, 1200.0f }, { 107.0f, 791.0f }
    };
    for (int i = 0; i < 6; i++) {
        t.action = ISWS_TOUCH_DOWN;
        t.pointer_id = (uint32_t)(i % 4);
        t.x = pts[i][0];
        t.y = pts[i][1];
        int before = g_created_count;
        CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
        if (g_created_count > before) {
            CHECK(g_last_children[0].x >= 0.0 && g_last_children[0].x <= 1.0);
            CHECK(g_last_children[0].y >= 0.0 && g_last_children[0].y <= 1.0);
        }
        t.action = ISWS_TOUCH_UP;
        CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    }
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Route D (combined): IOSWINE_TOUCH_INTERNAL=1 is now an alias for   */
/* the default combined path (old route B plus processEventQueue and   */
/* the sendEvent fallback).                                            */
/* ------------------------------------------------------------------ */

static void test_internal_route(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device */
    g_fake_touch_count = 1;
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_internal = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 100 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_internal_count == 1);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_signal_count == 1);
    CHECK(g_wakeup_count == 1);
    CHECK(g_drain_events_count == 1);
    /* The step old route B was missing: processEventQueue(env, 4). */
    CHECK(g_process_event_queue_count == 1);
    CHECK(g_last_process_limit == 4);
    CHECK(g_orig_send_event_called == 1);
    CHECK(touch_test_sendevent_count() == 1);
    CHECK(g_direct_send_event_count == 0);
    CHECK(strstr(g_log_buf, "[touch] internal receive\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] source signalled\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] processEventQueue\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] queued via _receiveHIDEvent:") == NULL);
    CHECK(strstr(g_log_buf, "[touch] combined: no sendEvent") == NULL);
    CHECK_NEAR(g_last_children[0].x, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.5, 1e-6);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Queue probe (finding 5): filtered queue count after Internal       */
/* ------------------------------------------------------------------ */

static void test_queue_probe(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_internal = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 100 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* Internal enqueued exactly one event; the drain hook logs it. */
    CHECK(strstr(g_log_buf, "[touch] queue filtered=1 raw=1\n") != NULL);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Keyboard routing: TEXT/KEY events                                   */
/* dispatched to keyboard.c on the main queue, focus loss hides the   */
/* keyboard. The keyboard symbols are host-test stubs in touch.c     */
/* (the real keyboard.c is not linked into this binary); the mock     */
/* dispatch_async_f runs the main-queue work synchronously.           */
/* ------------------------------------------------------------------ */

static void test_keyboard_routing(void) {
    static const uint8_t hello[] = { 'h', 'i' };
    static uint8_t big[5000];
    size_t i;
    present_event_t ev;
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();

    g_env_touch = "1";
    touch_init();
    CHECK(touch_test_kb_init_calls() == 1);

    /* TEXT "hi" reaches the keyboard stub with its bytes intact. */
    memset(&ev, 0, sizeof(ev));
    ev.kind = PRESENT_EV_TEXT;
    ev.u.text.kind = ISWS_TEXT_COMMIT;
    ev.u.text.utf8 = hello;
    ev.u.text.utf8_len = sizeof(hello);
    CHECK(touch_handle_present_event(&ev) == 0);
    CHECK(touch_test_kb_calls() == 1);
    CHECK(touch_test_kb_last_kind() == (int)PRESENT_EV_TEXT);
    CHECK(strcmp(touch_test_kb_last_text(), "hi") == 0);

    /* The bytes are copied, not borrowed: mutating the source after
     * dispatch must not change what the stub received. (The mock
     * dispatch runs synchronously, so this also pins the copy.) */
    CHECK(touch_test_kb_last_text()[0] == 'h');

    /* KEY DEL down reaches the keyboard stub. */
    memset(&ev, 0, sizeof(ev));
    ev.kind = PRESENT_EV_KEY;
    ev.u.key.keycode = 67; /* AKEYCODE_DEL */
    ev.u.key.action = ISWS_KEY_ACTION_DOWN;
    CHECK(touch_handle_present_event(&ev) == 0);
    CHECK(touch_test_kb_calls() == 2);
    CHECK(touch_test_kb_last_kind() == (int)PRESENT_EV_KEY);

    /* Focus loss hides a visible keyboard, and does not hide when
     * none is showing. */
    touch_test_kb_set_visible(1);
    memset(&ev, 0, sizeof(ev));
    ev.kind = PRESENT_EV_FOCUS;
    ev.u.focus.focused = 0;
    CHECK(touch_handle_present_event(&ev) == 0);
    CHECK(touch_test_kb_hide_calls() == 1);
    touch_test_kb_set_visible(0);
    CHECK(touch_handle_present_event(&ev) == 0);
    CHECK(touch_test_kb_hide_calls() == 1);

    /* Oversize text (over the 4096-byte copy cap) is dropped, never
     * truncated mid-sequence. */
    for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)'a';
    memset(&ev, 0, sizeof(ev));
    ev.kind = PRESENT_EV_TEXT;
    ev.u.text.kind = ISWS_TEXT_COMMIT;
    ev.u.text.utf8 = big;
    ev.u.text.utf8_len = sizeof(big);
    CHECK(touch_handle_present_event(&ev) == -1);
    CHECK(touch_test_kb_calls() == 2);
}

/* ------------------------------------------------------------------ */
/* Route C: IOSWINE_TOUCH_DIRECT=1 (UITouchesEvent fallback)          */
/* ------------------------------------------------------------------ */

static void test_direct_route(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* Tap on the Browse tab: DOWN (107, 791), MOVE (110, 793.33), UP. */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_touch_alloc_count == 1);
    CHECK(g_hittest_count == 1);
    CHECK_NEAR(g_last_hittest_x, 107.0, 1e-6);
    CHECK_NEAR(g_last_hittest_y, 791.0, 1e-6);
    CHECK(g_phase_seq_n == 1 && g_phase_seq[0] == 0);
    CHECK(g_loc_seq_n == 1);
    CHECK_NEAR(g_loc_seq_x[0], 107.0, 1e-6);
    CHECK_NEAR(g_loc_seq_y[0], 791.0, 1e-6);
    CHECK(g_loc_reset_seq[0] == 1);
    CHECK(g_set_timestamp_count == 1);
    CHECK_NEAR(g_last_touch_timestamp, 4.886718345, 1e-6);
    CHECK(g_direct_send_event_count == 1);
    CHECK(g_orig_send_event_called == 1);
    CHECK(g_window_send_event_count == 1);
    /* DOWN applies 9 setters (7 old + _setIsFirstTouchForView:
     * and _setInitialTouchTimestamp:), and the readback log shows the
     * touch carries a window and hit a real subview. */
    CHECK(strstr(g_log_buf, "[touch] direct phase=0 setters=9/9\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct win=1 view=subview\n") != NULL);

    t.action = ISWS_TOUCH_MOVE;
    t.x = 330.0f; t.y = 2380.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* Same UITouch object reused, not a new one. */
    CHECK(g_touch_alloc_count == 1);
    CHECK(g_hittest_count == 2);
    CHECK_NEAR(g_last_hittest_x, 110.0, 1e-6);
    CHECK_NEAR(g_last_hittest_y, 793.333333, 1e-4);
    CHECK(g_phase_seq_n == 2 && g_phase_seq[1] == 1);
    CHECK(g_loc_seq_n == 2);
    CHECK(g_loc_reset_seq[1] == 0);
    CHECK(g_direct_send_event_count == 2);
    CHECK(strstr(g_log_buf, "[touch] direct phase=1 setters=7/7\n") != NULL);

    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_touch_alloc_count == 1);
    CHECK(g_phase_seq_n == 3 && g_phase_seq[2] == 3);
    CHECK(g_direct_send_event_count == 3);
    CHECK(g_orig_send_event_called == 3);
    CHECK(g_window_send_event_count == 3);
    CHECK(strstr(g_log_buf, "[touch] direct phase=3 setters=7/7\n") != NULL);
    /* The Ended touch on the Browse tab flips it. */
    CHECK(g_tab_selected_index == 1);
    /* Touch object fields at the end of the tap. */
    CHECK(g_last_direct_touch != NULL);
    CHECK(g_last_direct_touch->window == &g_fake_win_obj);
    CHECK(g_last_direct_touch->view == &g_fake_tab_view_obj);
    CHECK(g_last_direct_touch->tapCount == 1);
    CHECK(g_last_direct_touch->phase == 3);
    CHECK(g_last_direct_touch->hidEvent != NULL);
    /* Restored setters. */
    CHECK(g_last_direct_touch->is_first_touch_for_view == 1);
    CHECK_NEAR(g_last_direct_touch->initial_timestamp, 4.886718345, 1e-6);
    CHECK(strstr(g_log_buf, "[touch] sendEvent: reached with touch count 1\n") != NULL);

    /* A second tap allocates a new UITouch: the slot was cleared on UP. */
    g_tab_selected_index = 0;
    t.action = ISWS_TOUCH_DOWN;
    t.x = 321.0f; t.y = 2373.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_touch_alloc_count == 2);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Direct path degrades gracefully when selectors are missing         */
/* ------------------------------------------------------------------ */

static void test_direct_missing_selectors(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_tab_selected_index = 0;

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };

    /* Case A: no UITouch setters at all, and the touch ivars hidden
     * too, so the ivar fallback has nothing to write. */
    g_hide_all_direct_setters = 1;
    g_hide_touch_ivars = 1;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_send_event_count == 0);
    CHECK(g_tab_selected_index == 0);
    CHECK(strstr(g_log_buf, "[touch] direct unavailable: no UITouch setters or ivars resolved\n") != NULL);
    g_hide_all_direct_setters = 0;
    g_hide_touch_ivars = 0;

    /* Case B: setters resolve but no way to attach touches to the
     * event. The KIF attach (_addTouch:) is hidden too, so the KIF
     * path is skipped and the fallback attach fails as before. */
    g_hide_touches_ivar = 1;
    g_hide_set_touches = 1;
    g_hide_add_touch = 1;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_send_event_count == 0);
    CHECK(g_tab_selected_index == 0);
    CHECK(strstr(g_log_buf, "[touch] direct unavailable: no way to attach touches to event\n") != NULL);
    g_hide_touches_ivar = 0;
    g_hide_set_touches = 0;
    g_hide_add_touch = 0;

    /* Case C: same tap works once the selectors are back. */
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_send_event_count == 2);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Sender ID matrix (finding 3)                                       */
/* ------------------------------------------------------------------ */

static void sender_case(uint32_t ctx, const char *env, uint64_t expected) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = ctx;
    g_env_touch = "1";
    g_env_sender = env;
    touch_init();
    touch_set_window_size(360.0, 800.0);
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_last_received.sender_id == expected);
    CHECK(g_created_count == g_released_count);
}

static void test_sender_id_matrix(void) {
    sender_case(0x12345u, NULL, 0x12345u);              /* default: window context id */
    sender_case(0, NULL, 0);                            /* no context id: undefined */
    sender_case(0x12345u, "magic", 0x8000000817319a7eULL);
    sender_case(0x12345u, "0", 0);
    sender_case(0, "0x1234", 0x1234);
    sender_case(0x12345u, "12x", 0x12345u);             /* bad value ignored */
    sender_case(0, "0x10000000000000000", 0);           /* overflow ignored */
    sender_case(0, "4294967295", 4294967295ULL);
}

/* ------------------------------------------------------------------ */
/* Self-test on the direct path (route C)                             */
/* ------------------------------------------------------------------ */

static void test_selftest_direct(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;

    g_env_touch = "1";
    g_env_selftest = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1\n") != NULL);
    CHECK(g_direct_send_event_count == 2);
    CHECK(strstr(g_log_buf, "[touch] direct phase=0 setters=9/9\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct phase=3 setters=7/7\n") != NULL);
}

/* ------------------------------------------------------------------ */
/* Route D (combined, default): direct delivery. No env flag.         */
/* The HID chain must run in full, synchronously, on the main queue:   */
/* _receiveHIDEventInternal: once per phase, source signal + wakeup,   */
/* drain, processEventQueue(env, 4); sendEvent: fires once per phase   */
/* and route C must NOT run (the HID chain delivered). Normalized      */
/* coords, context id and the runtime-derived sender id still hold.   */
/* ------------------------------------------------------------------ */

static void test_combined_route_default(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device */
    g_fake_touch_count = 1;
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 100 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_internal_count == 1);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_signal_count == 1);
    CHECK(g_wakeup_count == 1);
    CHECK(g_drain_events_count == 1);
    CHECK(g_process_event_queue_count == 1);
    CHECK(g_last_process_limit == 4);
    CHECK(g_orig_send_event_called == 1);
    CHECK(touch_test_sendevent_count() == 1);
    CHECK(g_direct_send_event_count == 0);
    CHECK(strstr(g_log_buf, "[touch] combined: no sendEvent") == NULL);
    CHECK_NEAR(g_last_children[0].x, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.5, 1e-6);
    /* The id is stamped via BKSHIDEventSetDigitizerInfo
     * now; the guessed 0xb0005 field is NOT written by default. */
    CHECK(g_last_received.bks_context_id == 0x12345);
    CHECK(g_last_received.context_id == 0);
    CHECK(g_last_received.sender_id == 0x12345);
    CHECK(strstr(g_log_buf, "[touch] internal receive\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] source signalled\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] processEventQueue\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] queued via _receiveHIDEvent:") == NULL);

    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_internal_count == 2);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_signal_count == 2);
    CHECK(g_wakeup_count == 2);
    CHECK(g_drain_events_count == 2);
    CHECK(g_process_event_queue_count == 2);
    CHECK(g_orig_send_event_called == 2);
    CHECK(touch_test_sendevent_count() == 2);
    CHECK(g_direct_send_event_count == 0);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Route D fallback: the HID chain runs in full but delivers nothing   */
/* (processEventQueue never calls sendEvent:), which models the        */
/* device case where the drain drops the event. The combined path must */
/* fall back to route C for the same phase, and the fallback's         */
/* sendEvent: is what the hook counts. NOTE on the mock:               */
/* mock_uikit_model flips the tab at internal-receive time (its        */
/* simplification of the whole downstream pipeline), so the tab       */
/* assertion here does not distinguish the paths; the counters do:     */
/* the HID chain's sendEvent: count stays 0 while the direct path's   */
/* count goes to 1 per phase.                                          */
/* ------------------------------------------------------------------ */

static void test_combined_route_fallback(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x54321u;
    g_fake_layer_context_id = 0x54321u; /* healthy device */
    g_tab_selected_index = 0;
    /* The HID chain delivers nothing. */
    g_drain_call_send_event = 0;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* Tap on the Browse tab: DOWN (107, 791), UP. */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* HID chain ran in full... */
    CHECK(g_direct_internal_count == 1);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_signal_count == 1);
    CHECK(g_drain_events_count == 1);
    CHECK(g_process_event_queue_count == 1);
    /* ...but delivered nothing, so route C fired for the DOWN phase. */
    CHECK(g_direct_send_event_count == 1);
    CHECK(g_phase_seq_n == 1 && g_phase_seq[0] == 0);
    CHECK(strstr(g_log_buf, "[touch] combined: no sendEvent, falling back to direct\n") != NULL);
    /* The fallback's sendEvent: is counted by the hook. */
    CHECK(touch_test_sendevent_count() == 1);
    CHECK(g_orig_send_event_called == 1);

    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_internal_count == 2);
    CHECK(g_process_event_queue_count == 2);
    CHECK(g_direct_send_event_count == 2);
    CHECK(g_phase_seq_n == 2 && g_phase_seq[1] == 3);
    CHECK(touch_test_sendevent_count() == 2);
    CHECK(g_orig_send_event_called == 2);
    /* The Ended touch on the Browse tab flips it (direct-path model). */
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Route A behind IOSWINE_TOUCH_QUEUE=1, and route precedence:         */
/* INTERNAL now selects the combined path, DIRECT wins over both.      */
/* ------------------------------------------------------------------ */

static void test_queue_route_ab(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device */
    g_fake_touch_count = 1;
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_queue = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 100 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 1);
    CHECK(g_direct_internal_count == 0);
    CHECK(g_signal_count == 0);
    CHECK(g_wakeup_count == 0);
    CHECK(g_drain_events_count == 1);
    CHECK(g_process_event_queue_count == 1);
    CHECK(g_orig_send_event_called == 1);
    CHECK(touch_test_sendevent_count() == 1);
    CHECK(g_direct_send_event_count == 0);
    CHECK(strstr(g_log_buf, "[touch] queued via _receiveHIDEvent:\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] internal receive") == NULL);
    CHECK(strstr(g_log_buf, "[touch] source signalled") == NULL);
    CHECK(strstr(g_log_buf, "[touch] combined: no sendEvent") == NULL);

    /* INTERNAL is now the combined path, not the old route B. */
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x12345u;
    g_fake_layer_context_id = 0x12345u; /* healthy device */
    g_fake_touch_count = 1;
    g_drain_call_send_event = 1;
    touch_test_configure(&ops);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_internal = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_direct_internal_count == 1);
    CHECK(g_signal_count == 1);
    CHECK(g_drain_events_count == 1);
    CHECK(g_process_event_queue_count == 1);
    CHECK(g_last_process_limit == 4);
    CHECK(g_orig_send_event_called == 1);
    CHECK(touch_test_sendevent_count() == 1);
    CHECK(g_direct_send_event_count == 0);

    /* DIRECT wins over QUEUE and INTERNAL. */
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    touch_test_configure(&g_ops);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    g_env_queue = "1";
    g_env_internal = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_direct_send_event_count == 1);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_direct_internal_count == 0);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Self-test on the combined (default) route: the device acceptance    */
/* in host form. Frame 20 synthesizes the Browse-tab tap; the tab      */
/* must flip and sendevent must count both phases.                     */
/* ------------------------------------------------------------------ */

static void test_combined_selftest(void) {
    touch_test_reset();
    reset_mocks();
    setup_full_fixture();
    g_fake_context_id = 0x54321u;
    g_fake_layer_context_id = 0x54321u; /* healthy device */
    g_tab_selected_index = 0;
    g_drain_call_send_event = 1;

    touch_test_ops_t ops = g_ops;
    ops.receive_internal_fn = mock_receive_internal;
    ops.drain_events_fn = mock_drain_events;
    ops.process_event_queue_fn = mock_process_event_queue;
    touch_test_configure(&ops);

    g_env_touch = "1";
    g_env_selftest = "1";
    g_env_trace = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] selftest sendevent=2\n") != NULL);
    CHECK(g_wrapper_receive_count == 0);
    CHECK(g_direct_internal_count == 2);
    CHECK(g_process_event_queue_count == 2);
    CHECK(g_direct_send_event_count == 0);
}

/* ------------------------------------------------------------------ */
/* Window / hit-test. The mock dispatcher                              */
/* resolves the touch's window from the event's context id against    */
/* the key window's layer CAContext id; a mismatch finds no window.   */
/* ------------------------------------------------------------------ */

/* One route A tap on the Browse tab (client px in 1080x2400). */
static void hid_tab_tap(void) {
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
}

/* Device bug reproduced on host: -[UIWindow _contextId]
 * returns per-run garbage (0x9e8fe2a3 in the selftest run) while the
 * layer CAContext carries the real id (7). The OLD code stamped the
 * garbage (via the guessed 0xb0005 field only, no BKS call): the
 * dispatcher finds no window, -[UIWindow sendEvent:] never runs, the
 * tab never flips, although -[UIApplication sendEvent:] fires. The
 * NEW code stamps the layer id via BKSHIDEventSetDigitizerInfo and
 * the tap lands. RED check: this test fails on the unpatched tree
 * (tab stays 0, window sendEvent count 0, no BKS call). */
static void test_window_context_mismatch_reproduces_device(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;      /* device garbage, selftest run */
    g_fake_layer_context_id = 7;          /* real layer CAContext id */
    g_tab_selected_index = 0;
    g_drain_call_send_event = 1;

    g_env_touch = "1";
    g_env_trace = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    hid_tab_tap();

    /* The event carries the layer id through the real BKS setter. */
    CHECK(g_bks_ctx_calls == 2);
    CHECK(g_bks_ctx_id == 7);
    CHECK(g_last_received.bks_context_id == 7);
    /* The guessed 0xb0005 field is NOT written by default. */
    CHECK(g_last_received.context_id == 0);
    /* The dispatcher found the window: app AND window sendEvent ran. */
    CHECK(g_orig_send_event_called == 2);
    CHECK(g_window_send_event_count == 2);
    CHECK(g_tab_selected_index == 1);
    CHECK(strstr(g_log_buf, "[touch] ctx win=2660229795 win2=2660229795 layer=7 use=7\n") != NULL);
    CHECK(g_created_count == g_released_count);
}

/* Layer id wins over _contextId, and a second _contextId read that
 * differs from the first is logged (device non-determinism). */
static void test_window_context_layer_vs_win(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;      /* first read: selftest value */
    g_fake_context_id_2nd = 0xdc438f25u;  /* second read: interactive value */
    g_fake_layer_context_id = 7;
    g_tab_selected_index = 0;
    g_drain_call_send_event = 1;

    g_env_touch = "1";
    g_env_trace = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    hid_tab_tap();

    CHECK(strstr(g_log_buf, "[touch] ctx win=2660229795 win2=3695415077 layer=7 use=7\n") != NULL);
    CHECK(g_bks_ctx_id == 7);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* IOSWINE_TOUCH_CTX_ID matrix. */
static void ctx_override_case(const char *env, int expect_flip) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;
    g_fake_layer_context_id = 7;
    g_tab_selected_index = 0;
    g_drain_call_send_event = 1;

    g_env_touch = "1";
    g_env_ctx_id = env;
    touch_init();
    touch_set_window_size(360.0, 800.0);

    hid_tab_tap();

    if (expect_flip) {
        CHECK(g_tab_selected_index == 1);
        CHECK(g_window_send_event_count == 2);
    } else {
        /* App sendEvent fires, window sendEvent does not. */
        CHECK(g_tab_selected_index == 0);
        CHECK(g_window_send_event_count == 0);
        CHECK(g_orig_send_event_called == 2);
    }
    CHECK(g_created_count == g_released_count);
}

static void test_ctx_env_override(void) {
    ctx_override_case("7", 1);            /* decimal, matches the layer id */
    ctx_override_case("0x7", 1);          /* hex, same */
    ctx_override_case("0x9e8fe2a3", 0);   /* the garbage id: no window */
    ctx_override_case("bogus", 1);        /* bad value ignored, layer wins */
    ctx_override_case("0x100000000", 1);  /* >32 bits ignored, layer wins */

    /* Forced 0 disables the stamp entirely (no BKS call); the event
     * carries no context id, so the dispatcher falls back to the
     * no-context path and the tap still lands. */
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;
    g_fake_layer_context_id = 7;
    g_tab_selected_index = 0;
    g_drain_call_send_event = 1;
    g_env_touch = "1";
    g_env_ctx_id = "0";
    touch_init();
    touch_set_window_size(360.0, 800.0);
    hid_tab_tap();
    CHECK(g_bks_ctx_calls == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* The 0xb0005 field: off by default, written when the flag is set. */
static void test_ctx_field_flag(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;
    g_fake_layer_context_id = 7;
    g_drain_call_send_event = 1;

    g_env_touch = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_last_received.context_id == 0);
    CHECK(g_last_children[0].context_id == 0);
    /* The BKS stamp still happens without the field. */
    CHECK(g_last_received.bks_context_id == 7);

    /* With the flag, the field is written on parent and child. */
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x9e8fe2a3u;
    g_fake_layer_context_id = 7;
    g_env_touch = "1";
    g_env_ctx_field = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_last_received.context_id == 7);
    CHECK(g_last_children[0].context_id == 7);
    CHECK(g_created_count == g_released_count);
}

/* Route C: setWindow:/setView: missing, _window/_view ivars rescue
 * the touch. RED check: fails on the unpatched tree (no ivar
 * fallback, window stays NULL, tab stays 0). */
static void test_direct_ivar_fallback(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_tab_selected_index = 0;
    g_hide_window_setter = 1;
    g_hide_view_setter = 1;

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    /* The setters are missing, so DOWN applies 7 of 9; the ivar
     * fallback still gives the touch a window and a view. */
    CHECK(strstr(g_log_buf, "[touch] direct phase=0 setters=7/9\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct win=1 view=subview\n") != NULL);
    CHECK(g_last_direct_touch != NULL);
    CHECK(g_last_direct_touch->window == &g_fake_win_obj);
    CHECK(g_last_direct_touch->view == &g_fake_tab_view_obj);

    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_window_send_event_count == 2);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* Route C with no window at all on the direct path.
 * -[UIApplication sendEvent:] fires, -[UIWindow sendEvent:] does not,
 * the tab never flips. */
static void test_direct_no_window_no_delivery(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_tab_selected_index = 0;
    g_hide_window_setter = 1;
    g_hide_view_setter = 1;
    g_hide_touch_ivars = 1;

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(strstr(g_log_buf, "[touch] direct win=0 view=nil\n") != NULL);
    CHECK(g_direct_send_event_count == 2);
    CHECK(g_orig_send_event_called == 2);
    CHECK(g_window_send_event_count == 0);
    CHECK(g_tab_selected_index == 0);
    CHECK(g_created_count == g_released_count);
}

/* Route C hit result: a tap outside any control hits the window
 * itself (view=win, no tab flip); a tap on the tab hits a subview. */
static void test_direct_hit_window_vs_subview(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_tab_selected_index = 0;

    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    /* Center tap (180, 400): no subview under the point. */
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 540.0f, .y = 1200.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(strstr(g_log_buf, "[touch] direct win=1 view=win\n") != NULL);
    CHECK(g_last_direct_touch != NULL);
    CHECK(g_last_direct_touch->view == &g_fake_win_obj);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_window_send_event_count == 2);
    CHECK(g_tab_selected_index == 0);

    /* Tab tap: a real subview, tab flips. */
    t.action = ISWS_TOUCH_DOWN;
    t.x = 321.0f; t.y = 2373.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_last_direct_touch->view == &g_fake_tab_view_obj);
    t.action = ISWS_TOUCH_UP;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* UIControl tracking                                   */
/*                                                                    */
/* The mock window runs a UIControl tracking machine (see
 * mock_tracking_deliver): Began hit-tests from the touch's
 * locationInWindow, Moved/Ended/Cancelled go to the tracking view.
 * The tests run in the DEVICE MODEL first: the
 * _setLocationInWindow: setter does not resolve (the leading
 * hypothesis for the device failure), so on the unpatched tree the
 * window hit-tests (0,0), tracking never starts, and only the
 * interim hack fires. With the ivar fallback the touch
 * carries the right location and tracking fires the action itself.
 * Points are client pixels in 1080x2400 (tab: x=321 y=2373,
 * center: x=540 y=1200, outside-tab: x=600 y=1200).              */
/* ------------------------------------------------------------------ */

static void tracking_setup(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1";
}

/* Start the touch subsystem after the test has set its env values
 * (e.g. g_env_tap_hack) and mock knobs. */
static void tracking_start(void) {
    touch_init();
    touch_set_window_size(360.0, 800.0);
}

/* DOWN at (x0,y0), optional MOVE to (x1,y1), UP at (x1,y1). */
static void tracking_tap(double x0, double y0, double x1, double y1, int move) {
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = (float)x0, .y = (float)y0, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    if (move) {
        t.action = ISWS_TOUCH_MOVE;
        t.x = (float)x1; t.y = (float)y1;
        CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    }
    t.action = ISWS_TOUCH_UP;
    t.x = (float)x1; t.y = (float)y1;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
}

/* Tap on the tab, device model (location setter hidden), hack off:
 * tracking must fire the action once and the tab must flip, with no
 * hack call. RED check: fails on the unpatched tree (begin 0,
 * action 0, tab 0). */
static void test_uicontrol_tracking_tap(void) {
    tracking_setup();
    g_hide_location_setter = 1;
    tracking_start();
    tracking_tap(321.0, 2373.0, 321.0, 2373.0, 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_continue == 0);
    CHECK(g_track_end == 1);
    CHECK(g_track_cancel == 0);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_tab_action_count == 1);
    CHECK(g_fake_tab_view_obj.highlighted == 0);
    CHECK(g_fake_tab_view_obj.tracking == 0);
    /* The ivar fallback fired for the location, and the readback
     * proves the touch carried the right state. DOWN applies 8 of 9
     * setters (all but the hidden location setter); UP 6 of 7. */
    CHECK(strstr(g_log_buf, "[touch] direct phase=0 setters=8/9\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct phase=3 setters=6/7\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct ivar phase=0 loc=1 ts=0 tap=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct readback phase=0 loc=107000,791000\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct readback phase=3 loc=107000,791000\n") != NULL);
    CHECK(g_last_direct_touch != NULL);
    CHECK(g_last_direct_touch->phase == 3);
    CHECK_NEAR(g_last_direct_touch->loc_x, 107.0, 1e-6);
    CHECK_NEAR(g_last_direct_touch->loc_y, 791.0, 1e-6);
    CHECK(g_created_count == g_released_count);
}

/* Same tap with every setter resolving (control): tracking works on
 * both trees; the ivar fallback never fires. */
static void test_uicontrol_tracking_tap_full_setters(void) {
    tracking_setup();
    tracking_start();
    tracking_tap(321.0, 2373.0, 321.0, 2373.0, 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_tab_action_count == 1);
    CHECK(strstr(g_log_buf, "[touch] direct phase=0 setters=9/9\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct ivar ") == NULL);
    CHECK(g_created_count == g_released_count);
}

/* Device model + hack flag on: tracking fires the action AND the
 * hack fires it again, so the tab action runs twice. This is why
 * the hack defaults off. RED check: fails on the unpatched tree
 * (tracking 0, hack 1, tab action 1: the hack alone). */
static void test_uicontrol_tracking_hack_double_fires(void) {
    tracking_setup();
    g_hide_location_setter = 1;
    g_env_tap_hack = "1";
    tracking_start();
    tracking_tap(321.0, 2373.0, 321.0, 2373.0, 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 1);
    CHECK(g_hack_events == (1ul << 6));
    CHECK(g_tab_selected_index == 1);
    CHECK(g_tab_action_count == 2);
    CHECK(strstr(g_log_buf, "[touch] direct fire TouchUpInside\n") != NULL);
    CHECK(g_created_count == g_released_count);
}

/* Drag-out, device model: Began in the tab, Moved outside, Ended
 * outside. Tracking runs but the action must NOT fire, and the tab
 * must not flip. RED check: fails on the unpatched tree (begin 0). */
static void test_uicontrol_tracking_drag_out(void) {
    tracking_setup();
    g_hide_location_setter = 1;
    tracking_start();
    tracking_tap(321.0, 2373.0, 600.0, 1200.0, 1);
    CHECK(g_track_begin == 1);
    CHECK(g_track_continue == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_cancel == 0);
    CHECK(g_track_action == 0);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 0);
    CHECK(g_tab_action_count == 0);
    CHECK(g_fake_tab_view_obj.highlighted == 0);
    CHECK(g_fake_tab_view_obj.tracking == 0);
    CHECK(g_created_count == g_released_count);
}

/* Tap-and-hold, device model: Began in the tab, small MOVE inside,
 * Ended inside. The action fires on release. */
static void test_uicontrol_tracking_hold(void) {
    tracking_setup();
    g_hide_location_setter = 1;
    tracking_start();
    tracking_tap(321.0, 2373.0, 330.0, 2380.0, 1);
    CHECK(g_track_begin == 1);
    CHECK(g_track_continue == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(g_tab_action_count == 1);
    CHECK(g_created_count == g_released_count);
}

/* Cancel, device model: Began in the tab, then CANCEL. Tracking
 * cancels, no action, no tab flip. RED check: fails on the
 * unpatched tree (cancel 0). */
static void test_uicontrol_tracking_cancel(void) {
    tracking_setup();
    g_hide_location_setter = 1;
    tracking_start();
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 321.0f, .y = 2373.0f, .timestamp_ns = 1 };
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    t.action = ISWS_TOUCH_CANCEL;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_continue == 0);
    CHECK(g_track_end == 0);
    CHECK(g_track_cancel == 1);
    CHECK(g_track_action == 0);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 0);
    CHECK(g_tab_action_count == 0);
    CHECK(g_fake_tab_view_obj.tracking == 0);
    CHECK(g_created_count == g_released_count);
}

/* Phase setter hidden (the second prime suspect): the touch's phase
 * must still sequence Began -> Ended through the _phase ivar.
 * RED check: fails on the unpatched tree (phase stuck at Began:
 * begin fires twice, end never runs, no action). */
static void test_uicontrol_tracking_no_phase_setter(void) {
    tracking_setup();
    g_hide_phase_setter = 1;
    tracking_start();
    tracking_tap(321.0, 2373.0, 321.0, 2373.0, 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(strstr(g_log_buf, "[touch] direct ivar phase=1 loc=0 ts=0 tap=0\n") != NULL);
    CHECK(g_last_direct_touch != NULL);
    CHECK(g_last_direct_touch->phase == 3);
    CHECK(g_created_count == g_released_count);
}

/* No UITouch setters at all, ivars visible: the whole touch state
 * comes from the ivar fallbacks and tracking still completes. RED
 * check: fails on the unpatched tree (no sendEvent, tab 0). */
static void test_uicontrol_tracking_no_setters_ivars_rescue(void) {
    tracking_setup();
    g_hide_all_direct_setters = 1;
    tracking_start();
    tracking_tap(321.0, 2373.0, 321.0, 2373.0, 0);
    CHECK(g_track_begin == 1);
    CHECK(g_track_end == 1);
    CHECK(g_track_action == 1);
    CHECK(g_hack_fire == 0);
    CHECK(g_tab_selected_index == 1);
    CHECK(strstr(g_log_buf, "[touch] direct ivar phase=1 loc=1 ts=1 tap=1\n") != NULL);
    CHECK(g_created_count == g_released_count);
}

/* ------------------------------------------------------------------ */
/* Coordinate fuzz: 10000 random touches, no crash, coords in [0,1]   */
/* ------------------------------------------------------------------ */

static uint32_t fuzz_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void test_fuzz_coords(void) {
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    setup_full_fixture();
    g_env_touch = "1";
    touch_init();
    touch_set_window_size(360.0, 800.0);

    uint32_t st = 0x12345678u;
    int created_before = 0;
    for (int i = 0; i < 10000; i++) {
        isws_touch_t t;
        uint32_t r = fuzz_next(&st);
        t.action = r % 4;
        t.pointer_id = (r >> 4) % 12; /* sometimes out of range (10, 11) */
        t.pointer_count = 1 + ((r >> 8) % 3);
        t.timestamp_ns = (int64_t)i;
        /* Coordinate pool: in-range, edge, out-of-range, NaN, Inf. */
        switch ((r >> 12) % 8) {
        case 0: t.x = (float)(fuzz_next(&st) % 1081); t.y = (float)(fuzz_next(&st) % 2401); break;
        case 1: t.x = 0.0f; t.y = 0.0f; break;
        case 2: t.x = 1080.0f; t.y = 2400.0f; break;
        case 3: t.x = -100.0f; t.y = 5000.0f; break;
        case 4: t.x = NAN; t.y = 100.0f; break;
        case 5: t.x = 100.0f; t.y = INFINITY; break;
        case 6: t.x = 1e30f; t.y = -1e30f; break;
        default: t.x = 321.0f; t.y = 2373.0f; break;
        }
        created_before = g_created_count;
        int rc = touch_test_process_touch(&t, 1080, 2400);
        CHECK(rc == 0 || rc == -1);
        if (g_created_count > created_before) {
            CHECK(g_last_received.x >= 0.0 && g_last_received.x <= 1.0);
            CHECK(g_last_received.y >= 0.0 && g_last_received.y <= 1.0);
            for (int c = 0; c < g_last_received.n_children; c++) {
                CHECK(g_last_children[c].x >= 0.0 && g_last_children[c].x <= 1.0);
                CHECK(g_last_children[c].y >= 0.0 && g_last_children[c].y <= 1.0);
            }
        }
    }
    CHECK(g_created_count == g_released_count);
    CHECK(touch_test_fingers_down() <= ISWS_MAX_POINTERS);
}

int main(void) {
    test_protocol_touch_roundtrip();
    test_coord_mapping();
    test_env_gating();
    test_single_tap_and_cancel();
    test_multitouch_and_window_resize();
    test_socket_loopback();
    test_touch_trace_and_context_id();
    test_hooks_log_zero_touch();
    test_touch_selftest_tab_switch();
    test_coord_fix_regression();
    test_internal_route();
    test_queue_probe();
    test_direct_route();
    test_direct_missing_selectors();
    test_direct_ivar_fallback();
    test_direct_no_window_no_delivery();
    test_direct_hit_window_vs_subview();
    test_uicontrol_tracking_tap();
    test_uicontrol_tracking_tap_full_setters();
    test_uicontrol_tracking_hack_double_fires();
    test_uicontrol_tracking_drag_out();
    test_uicontrol_tracking_hold();
    test_uicontrol_tracking_cancel();
    test_uicontrol_tracking_no_phase_setter();
    test_uicontrol_tracking_no_setters_ivars_rescue();
    test_sender_id_matrix();
    test_selftest_direct();
    test_combined_route_default();
    test_combined_route_fallback();
    test_queue_route_ab();
    test_combined_selftest();
    test_window_context_mismatch_reproduces_device();
    test_window_context_layer_vs_win();
    test_ctx_env_override();
    test_ctx_field_flag();
    test_keyboard_routing();
    test_fuzz_coords();
    printf("touch_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
