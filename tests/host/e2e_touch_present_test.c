/*
 * e2e_touch_present_test.c - End-to-end host simulation for ioswine.
 *
 * Drives the REAL present.c (presenter socket loopback + protocol parser)
 * and the REAL touch.c (TOUCH_HOST_TEST) through a mock UIKit pipeline
 * model, and asserts the whole loop: presenter TOUCH -> present queue ->
 * touch.c IOHIDEvent injection -> UIEventFetcher -> drain -> sendEvent:
 * -> window hit-test -> tab switch -> a changed frame reaches the
 * presenter.
 *
 * Pipeline model (stages, in order):
 *   RECEIVE_INTERNAL -> DRAIN -> DISPATCHER -> DEFERRING -> SENDEVENT
 *   -> HITTEST -> TAB_SWITCH
 * The mock drain stops at a configurable drop point. DROP_DISPATCHER
 * reproduces the device symptom exactly: the trace shows "fetcher thread
 * present? 1 / internal receive / source signalled" and no sendEvent:,
 * and the selftest logs "tab before=0 after=0".
 *
 * Deliberate difference from tests/host/touch_test.c: that test's
 * mock_receive_internal flips the tab itself, so its host selftest
 * passes even though the device pipeline drops the event. Here the tab
 * flips ONLY inside the mock sendEvent: hit-test, i.e. only when the
 * event really reaches sendEvent:. A drop anywhere upstream leaves the
 * tab at 0, which is the bug this test is built to catch.
 *
 * The direct-sendEvent fallback case models the fallback behavior in
 * touch.c (synthesize UITouchesEvent, call sendEvent: directly when the
 * pipeline drops the event). The fallback is invoked here from the test,
 * not from touch.c: this test defines the expected behavior and proves
 * the mock window switches on direct sendEvent:. The real fallback exists
 * in touch.c as route D's last step and is covered by
 * test_combined_route_fallback in touch_test.c; this test does NOT prove
 * the real fallback. That proof is the device check.
 *
 * Geometry: phone 1440x3200, client frame 720x1600 (scale 2.0), window
 * 360x800 pt. The selftest tap at window point (107, 791) is client
 * pixel (214, 1582). Letterbox and aspect-fit cases use
 * isws_compute_mapping / isws_screen_to_client from protocol.c.
 *
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "present.h"
#include "protocol.h"
#include "touch.h"

extern void present_test_set_socket_name(const char *name);
extern int present_test_feed(const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/* Check accounting                                                    */
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

/* ------------------------------------------------------------------ */
/* Pipeline model                                                      */
/* ------------------------------------------------------------------ */

/* Drop point: the stage AFTER which the pipeline stops. */
enum {
    DROP_NONE = 0,        /* healthy pipeline, event reaches the tab bar */
    DROP_DISPATCHER = 1,  /* dropped in UIEventDispatcher (device symptom) */
    DROP_DEFERRING = 2,   /* dropped in _UIEventDeferringManager */
    DROP_HITTEST = 3,     /* reaches sendEvent: but hit-test finds no view */
};

enum {
    STAGE_RECEIVE_INTERNAL = 1,
    STAGE_DRAIN,
    STAGE_DISPATCHER,
    STAGE_DEFERRING,
    STAGE_SENDEVENT,
    STAGE_HITTEST,
    STAGE_TAB_SWITCH,
};

static int g_drop = DROP_NONE;
static int g_stages[32];
static int g_nstages = 0;
static int g_send_event_calls = 0;
static int g_orig_send_event_called = 0;
static uint64_t g_tab_selected_index = 0;
static double g_last_touch_x = -1.0;
static double g_last_touch_y = -1.0;
static int g_last_ctx_id = -1;
static uint64_t g_last_sender_id = 0;
static int g_last_disp_integrated = -1;
static int g_last_child_count = -1;
static double g_last_child_x = -1.0;
static double g_last_child_y = -1.0;
static uint32_t g_last_child_touch = 0;
static uint32_t g_fake_context_id = 42;

static void record_stage(int s) {
    if (g_nstages < 32) g_stages[g_nstages++] = s;
}

static int stages_reached(int s) {
    for (int i = 0; i < g_nstages; i++) {
        if (g_stages[i] == s) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mock IOKit / ObjC / libdispatch (same seam as touch_test.c)         */
/* ------------------------------------------------------------------ */

#define MAX_MOCK_EVENTS 64
#define MAX_MOCK_CHILDREN 16

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
    /* Context id stamped by BKSHIDEventSetDigitizerInfo,
     * the real setter (WebKit HIDEventGenerator.mm). The guessed
     * 0xb0005 field is no longer written by default. */
    int64_t bks_context_id;
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
} fake_fetcher_layout_t;

static int g_fake_app_cls;
static int g_fake_disp_cls;
static int g_fake_fetcher_cls;
static int g_fake_win_cls;
static int g_fake_tab_cls;

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
static uint64_t g_fake_touch_count = 0;

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

static int g_dispatch_calls = 0;
static int g_signal_count = 0;
static int g_wakeup_count = 0;
static int g_internal_receive_count = 0;
static int g_drain_events_count = 0;
static const char *g_env_touch = NULL;
static const char *g_env_trace = NULL;
static const char *g_env_selftest = NULL;
static char g_log_buf[8192];
static size_t g_log_len = 0;
static void (*g_mock_send_event_imp)(void *self, void *cmd, void *event) = NULL;

static void mock_orig_send_event(void *self, void *cmd, void *event) {
    (void)self; (void)cmd; (void)event;
    g_orig_send_event_called++;
}

static void reset_mocks(void) {
    memset(g_mock_events, 0, sizeof(g_mock_events));
    g_created_count = 0;
    g_released_count = 0;
    g_dispatch_calls = 0;
    g_signal_count = 0;
    g_wakeup_count = 0;
    g_internal_receive_count = 0;
    g_drain_events_count = 0;
    g_env_touch = NULL;
    g_env_trace = NULL;
    g_env_selftest = NULL;
    memset(g_log_buf, 0, sizeof(g_log_buf));
    g_log_len = 0;
    g_drop = DROP_NONE;
    g_nstages = 0;
    g_send_event_calls = 0;
    g_orig_send_event_called = 0;
    g_tab_selected_index = 0;
    g_last_touch_x = -1.0;
    g_last_touch_y = -1.0;
    g_last_ctx_id = -1;
    g_last_sender_id = 0;
    g_last_disp_integrated = -1;
    g_last_child_count = -1;
    g_last_child_x = -1.0;
    g_last_child_y = -1.0;
    g_last_child_touch = 0;
    g_fake_context_id = 42;
    g_fake_touch_count = 0;
    g_mock_send_event_imp = NULL;
    memset(&g_fake_app_obj, 0, sizeof(g_fake_app_obj));
    memset(&g_fake_disp_obj, 0, sizeof(g_fake_disp_obj));
    memset(&g_fake_fetcher_obj, 0, sizeof(g_fake_fetcher_obj));
    g_fake_app_obj.eventFetcher = &g_fake_fetcher_obj;
    g_fake_app_obj.eventDispatcher = &g_fake_disp_obj;
    g_fake_app_obj.keyWindow = &g_fake_win_obj;
    g_fake_disp_obj.environment = &g_fake_env_obj;
    g_fake_disp_obj.eventFetcher = &g_fake_fetcher_obj;
    g_fake_disp_obj.mainRunLoopCFSource = &g_fake_source_obj;
    g_fake_disp_obj.mainRunLoop = &g_fake_rl_obj;
    g_fake_fetcher_obj.cfRunLoop = &g_fake_rl_obj;
    g_fake_fetcher_obj.eventDispatcher = &g_fake_disp_obj;
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

static void *mock_create_digitizer(void *allocator, uint64_t timestamp,
    uint32_t transducerType, uint32_t index, uint32_t identity,
    uint32_t eventMask, uint32_t buttonMask, double x, double y, double z,
    double tipPressure, double twist, uint32_t range, uint32_t touch,
    uint32_t options) {
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
    ev->x = x; ev->y = y; ev->z = z;
    ev->pressure = tipPressure;
    ev->twist = twist;
    ev->range = range;
    ev->touch = touch;
    ev->options = options;
    return ev;
}

static void *mock_create_finger(void *allocator, uint64_t timestamp,
    uint32_t index, uint32_t identity, uint32_t eventMask,
    double x, double y, double z, double tipPressure, double twist,
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
    ev->x = x; ev->y = y; ev->z = z;
    ev->pressure = tipPressure;
    ev->twist = twist;
    ev->range = range;
    ev->touch = touch;
    ev->options = options;
    return ev;
}

static void mock_append_event(void *parent_v, void *child_v, uint32_t options) {
    (void)options;
    mock_hid_event_t *parent = (mock_hid_event_t *)parent_v;
    mock_hid_event_t *child = (mock_hid_event_t *)child_v;
    if (!parent || !child || parent->n_children >= MAX_MOCK_CHILDREN) return;
    child->refcount++;
    parent->children[parent->n_children++] = child;
}

static void mock_set_integer(void *ev_v, uint32_t field, int64_t value) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    if (field == 0xb0019u) ev->disp_integrated = value;
    else if (field == 0xb0005u) ev->context_id = value;
}

static void mock_set_sender(void *ev_v, uint64_t senderID) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    ev->sender_id = senderID;
}

/* BKSHIDEventSetDigitizerInfo(event, contextID, ...).
 * Records the stamped context id and the call count on the event. */
static void mock_BKSHIDEventSetDigitizerInfo(void *ev_v, uint32_t contextID,
                                             uint8_t a, uint8_t b, void *c,
                                             double d, float e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    ev->bks_context_id = contextID;
    ev->bks_calls++;
}

static void mock_cf_release(void *cf) {
    mock_hid_event_t *ev = (mock_hid_event_t *)cf;
    if (!ev || !ev->in_use) return;
    ev->refcount--;
    if (ev->refcount == 0) {
        for (int i = 0; i < ev->n_children; i++) {
            mock_cf_release(ev->children[i]);
        }
        ev->in_use = 0;
        g_released_count++;
    }
}

static void *mock_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "UIApplication") == 0) return &g_fake_app_cls;
    if (strcmp(name, "UIEventDispatcher") == 0) return &g_fake_disp_cls;
    if (strcmp(name, "UIEventFetcher") == 0) return &g_fake_fetcher_cls;
    if (strcmp(name, "UIWindow") == 0) return &g_fake_win_cls;
    if (strcmp(name, "UITabBarController") == 0) return &g_fake_tab_cls;
    return NULL;
}

static void *mock_object_getClass(void *obj) {
    if (!obj) return NULL;
    if (obj == &g_fake_app_obj) return &g_fake_app_cls;
    if (obj == &g_fake_disp_obj) return &g_fake_disp_cls;
    if (obj == &g_fake_fetcher_obj) return &g_fake_fetcher_cls;
    if (obj == &g_fake_win_obj) return &g_fake_win_cls;
    if (obj == &g_fake_tab_obj) return &g_fake_tab_cls;
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
    return 0;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    if (cls == &g_fake_app_cls && sel == &g_sel_send_event) {
        return (void *)(uintptr_t)0x8001;
    }
    return NULL;
}

static void *mock_method_setImplementation(void *m, void *imp) {
    if (m == (void *)(uintptr_t)0x8001) {
        g_mock_send_event_imp = (void (*)(void *, void *, void *))imp;
        return (void *)mock_orig_send_event;
    }
    return NULL;
}

/* Forward declarations: the _receiveHIDEvent: branch of
 * mock_objc_msgSend runs the fetch-thread chain synchronously (see
 * below); the definitions live further down with the other mocks. */
static void mock_receive_internal(void *fetcher, void *event);
static void mock_drain_events(void *fetcher, void *environment);

static void mock_source_signal(void *source) {
    if (source == &g_fake_source_obj) g_signal_count++;
}

static void mock_rl_wakeup(void *rl) {
    if (rl == &g_fake_rl_obj) g_wakeup_count++;
}

static void *mock_rl_get_main(void) {
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
    return NULL;
}

/* The tab bar occupies the bottom of the 360x800 pt window: y >= 750,
 * full width. A tap there with tab 0 selected switches to tab 1. */
static void mock_hit_test_and_switch(void) {
    record_stage(STAGE_HITTEST);
    if (g_drop == DROP_HITTEST) return;
    /* Hit test in the digitizer event's coordinate space:
     * normalized [0,1] (the HID constructors receive
     * normalized coords, not window points). The Browse tab strip
     * is y in [750, 800] window points = [0.9375, 1.0] normalized,
     * x anywhere across the 360 pt window. */
    if (g_last_touch_y >= 0.9375 && g_last_touch_y <= 1.0 &&
        g_last_touch_x >= 0.0 && g_last_touch_x <= 1.0 &&
        g_tab_selected_index == 0) {
        g_tab_selected_index = 1;
        record_stage(STAGE_TAB_SWITCH);
    }
}

static void *mock_objc_msgSend(void *obj, void *sel, void *arg1, void *arg2) {
    (void)arg2;
    if (sel == &g_sel_responds) {
        if (obj == &g_fake_app_cls && arg1 == &g_sel_shared_app) return (void *)(uintptr_t)1;
        if (obj == &g_fake_app_obj && arg1 == &g_sel_event_fetcher) return (void *)(uintptr_t)1;
        if (obj == &g_fake_app_obj && arg1 == &g_sel_key_window) return (void *)(uintptr_t)1;
        if (obj == &g_fake_fetcher_obj && arg1 == &g_sel_receive_hid) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && arg1 == &g_sel_context_id) return (void *)(uintptr_t)1;
        if (obj == &g_fake_win_obj && arg1 == &g_sel_root_vc) return (void *)(uintptr_t)1;
        if (obj == &g_fake_tab_obj && arg1 == &g_sel_selected_index) return (void *)(uintptr_t)1;
        if (obj == &g_fake_event_obj && arg1 == &g_sel_all_touches) return (void *)(uintptr_t)1;
        if (obj == &g_fake_touches_set && arg1 == &g_sel_count) return (void *)(uintptr_t)1;
        return NULL;
    }
    if (obj == &g_fake_app_cls && sel == &g_sel_shared_app) return &g_fake_app_obj;
    if (obj == &g_fake_app_obj && sel == &g_sel_event_fetcher) return &g_fake_fetcher_obj;
    if (obj == &g_fake_app_obj && sel == &g_sel_key_window) return &g_fake_win_obj;
    if (obj == &g_fake_win_obj && sel == &g_sel_context_id) return (void *)(uintptr_t)g_fake_context_id;
    if (obj == &g_fake_win_obj && sel == &g_sel_root_vc) return &g_fake_tab_obj;
    if (obj == &g_fake_tab_obj && sel == &g_sel_selected_index) return (void *)(uintptr_t)g_tab_selected_index;
    if (obj == &g_fake_event_obj && sel == &g_sel_all_touches) return &g_fake_touches_set;
    if (obj == &g_fake_touches_set && sel == &g_sel_count) return (void *)(uintptr_t)g_fake_touch_count;
    if (obj == &g_fake_app_obj && sel == &g_sel_send_event) {
        g_send_event_calls++;
        record_stage(STAGE_SENDEVENT);
        if (g_mock_send_event_imp) {
            g_mock_send_event_imp(obj, sel, arg1);
        } else {
            mock_orig_send_event(obj, sel, arg1);
        }
        mock_hit_test_and_switch();
        return NULL;
    }
    if (obj == &g_fake_fetcher_obj && sel == &g_sel_receive_hid) {
        /* Route A: _receiveHIDEvent: is the
         * thread-safe wrapper. It retains the event and performs
         * the block onto the fetch thread, which runs
         * _receiveHIDEventInternal:, signals the dispatcher, and
         * the main runloop source drains into the environment.
         * The host mock runs that whole chain synchronously (the
         * same model touch_test.c uses): receive_internal records
         * the HID fields, drain runs the dispatcher and sendEvent:.
         * Base touch.c called receive_internal directly from
         * touch_inject, so this branch used to be a bare counter. */
        mock_receive_internal(obj, arg1);
        mock_drain_events(obj, NULL);
        return NULL;
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
    if (fn) fn(arg);
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
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    if (strcmp(name, "CFRunLoopSourceSignal") == 0) return (void *)mock_source_signal;
    if (strcmp(name, "CFRunLoopWakeUp") == 0) return (void *)mock_rl_wakeup;
    if (strcmp(name, "CFRunLoopGetMain") == 0) return (void *)mock_rl_get_main;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "CFRelease") == 0) return (void *)mock_cf_release;
    if (strcmp(name, "IOHIDEventCreateDigitizerEvent") == 0) return (void *)mock_create_digitizer;
    if (strcmp(name, "IOHIDEventCreateDigitizerFingerEvent") == 0) return (void *)mock_create_finger;
    if (strcmp(name, "IOHIDEventAppendEvent") == 0) return (void *)mock_append_event;
    if (strcmp(name, "IOHIDEventSetIntegerValue") == 0) return (void *)mock_set_integer;
    if (strcmp(name, "IOHIDEventSetSenderID") == 0) return (void *)mock_set_sender;
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
    return NULL;
}

static uint64_t mock_mach_abs_time(void) {
    return 0x123456789ULL;
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

/* receive_internal records the HID event fields and the touch point.
 * It does NOT flip the tab: delivery happens only in sendEvent:. */
static void mock_receive_internal(void *fetcher, void *event) {
    (void)fetcher;
    mock_hid_event_t *ev = (mock_hid_event_t *)event;
    g_internal_receive_count++;
    record_stage(STAGE_RECEIVE_INTERNAL);
    if (ev) {
        /* The context id now arrives via BKSHIDEventSetDigitizerInfo,
         * not the 0xb0005 field. */
        g_last_ctx_id = (int)ev->bks_context_id;
        g_last_sender_id = ev->sender_id;
        g_last_disp_integrated = (int)ev->disp_integrated;
        g_last_child_count = ev->n_children;
        if (ev->n_children > 0 && ev->children[0]) {
            g_last_child_x = ev->children[0]->x;
            g_last_child_y = ev->children[0]->y;
            g_last_child_touch = ev->children[0]->touch;
            g_last_touch_x = ev->children[0]->x;
            g_last_touch_y = ev->children[0]->y;
        }
    }
}

/* drain models the rest of the pipeline. It stops at the drop point. */
static void mock_drain_events(void *fetcher, void *environment) {
    (void)fetcher;
    (void)environment;
    g_drain_events_count++;
    record_stage(STAGE_DRAIN);
    if (g_drop == DROP_DISPATCHER) return;
    record_stage(STAGE_DISPATCHER);
    if (g_drop == DROP_DEFERRING) return;
    record_stage(STAGE_DEFERRING);
    g_fake_touch_count = 1;
    mock_objc_msgSend(&g_fake_app_obj, &g_sel_send_event, &g_fake_event_obj, NULL);
}

static const touch_test_ops_t g_ops = {
    .dlsym_default = mock_dlsym,
    .dlopen_fn = mock_dlopen,
    .getenv_fn = mock_getenv,
    .mach_abs_time = mock_mach_abs_time,
    .log_fn = mock_log,
    .receive_internal_fn = mock_receive_internal,
    .drain_events_fn = mock_drain_events,
};

static void setup_touch(void) {
    reset_mocks();
    touch_test_reset();
    touch_test_configure(&g_ops);
    touch_set_window_size(360.0, 800.0);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_selftest = NULL;
    touch_init();
}

/* Feed one presenter TOUCH message through the real present.c parser,
 * dequeue it, and run it through the real touch.c. */
static int feed_touch(uint32_t action, float x, float y) {
    uint8_t buf[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
    isws_touch_t t;
    present_event_t ev;
    t.action = action;
    t.pointer_id = 0;
    t.pointer_count = 1;
    t.x = x;
    t.y = y;
    t.timestamp_ns = 1000000;
    if (isws_encode_header(buf, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE) != ISWS_HEADER_SIZE) return -1;
    if (isws_encode_touch(buf + ISWS_HEADER_SIZE, ISWS_TOUCH_PAYLOAD_SIZE, &t) != ISWS_TOUCH_PAYLOAD_SIZE) return -1;
    if (present_test_feed(buf, sizeof(buf)) != 0) return -1;
    if (!present_poll_event(&ev)) return -1;
    if (ev.kind != PRESENT_EV_TOUCH) return -1;
    return touch_test_process_touch(&ev.u.touch, 720, 1600);
}

/* ------------------------------------------------------------------ */
/* Fake presenter server (frames only): HELLO/READY/PRESENT/FRAME_DONE */
/* ------------------------------------------------------------------ */

static char g_sock_name[64];
static volatile int g_server_stop = 0;
static volatile int g_server_ready = 0;
static uint32_t g_server_pixels[16];
static int g_server_npresents = 0;
static uint64_t g_server_frame_ids[16];

static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void *fake_server_thread(void *arg) {
    (void)arg;
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    size_t nlen = strlen(g_sock_name);
    memcpy(addr.sun_path + 1, g_sock_name, nlen);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
    if (bind(listen_fd, (struct sockaddr *)&addr, alen) != 0) {
        close(listen_fd);
        return NULL;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return NULL;
    }
    __atomic_store_n(&g_server_ready, 1, __ATOMIC_RELEASE);
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    int conn = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (conn < 0) return NULL;
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* HELLO: the client sends header + 24-byte payload + the memfd in
     * ONE sendmsg with SCM_RIGHTS. The fd must be read with the same
     * recvmsg as the bytes: a plain recv() of the header first would
     * silently discard the ancillary data and lose the memfd. */
    uint8_t hello_msg[ISWS_HEADER_SIZE + ISWS_HELLO_PAYLOAD_SIZE];
    uint8_t cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    iov.iov_base = hello_msg;
    iov.iov_len = sizeof(hello_msg);
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    ssize_t nr = recvmsg(conn, &mh, MSG_WAITALL);
    if (nr != (ssize_t)sizeof(hello_msg)) { close(conn); return NULL; }
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (!cm || cm->cmsg_type != SCM_RIGHTS) { close(conn); return NULL; }
    int memfd = *(int *)CMSG_DATA(cm);
    uint8_t hdr[ISWS_HEADER_SIZE];
    memcpy(hdr, hello_msg, sizeof(hdr));
    uint8_t payload[ISWS_HELLO_PAYLOAD_SIZE];
    memcpy(payload, hello_msg + ISWS_HEADER_SIZE, sizeof(payload));
    uint16_t type = 0, flags = 0;
    uint32_t plen = 0;
    if (isws_decode_header(hdr, sizeof(hdr), &type, &flags, &plen) != 0 ||
        type != ISWS_MSG_HELLO || plen != ISWS_HELLO_PAYLOAD_SIZE) {
        close(memfd);
        close(conn);
        return NULL;
    }
    isws_hello_t hello;
    if (isws_decode_hello(payload, sizeof(payload), &hello) != 0) {
        close(memfd);
        close(conn);
        return NULL;
    }
    uint8_t *map = mmap(NULL, (size_t)hello.stride * hello.height * ISWS_NUM_BUFFERS,
                        PROT_READ, MAP_SHARED, memfd, 0);
    if (map == MAP_FAILED) {
        close(memfd);
        close(conn);
        return NULL;
    }
    /* READY */
    uint8_t rdy[ISWS_HEADER_SIZE + ISWS_READY_PAYLOAD_SIZE];
    isws_ready_t r = { .version = ISWS_VERSION, .flags = 0, .max_in_flight = ISWS_NUM_BUFFERS };
    isws_encode_header(rdy, ISWS_MSG_READY, 0, ISWS_READY_PAYLOAD_SIZE);
    isws_encode_ready(rdy + ISWS_HEADER_SIZE, ISWS_READY_PAYLOAD_SIZE, &r);
    if (write_all(conn, rdy, sizeof(rdy)) != 0) {
        munmap(map, (size_t)hello.stride * hello.height * ISWS_NUM_BUFFERS);
        close(memfd);
        close(conn);
        return NULL;
    }
    while (!g_server_stop) {
        if (read_exact(conn, hdr, sizeof(hdr)) != 0) break;
        if (isws_decode_header(hdr, sizeof(hdr), &type, &flags, &plen) != 0) break;
        if (plen > 65535) break;
        uint8_t pbuf[65535];
        if (plen > 0 && read_exact(conn, pbuf, plen) != 0) break;
        if (type == ISWS_MSG_PRESENT) {
            isws_present_t p;
            if (isws_decode_present(pbuf, plen, &p) != 0) break;
            if (g_server_npresents < 16) {
                size_t off = (size_t)p.buffer_index * hello.stride * hello.height;
                uint32_t px = 0;
                memcpy(&px, map + off, sizeof(px));
                g_server_pixels[g_server_npresents] = px;
                g_server_frame_ids[g_server_npresents] = p.frame_id;
                g_server_npresents++;
            }
            uint8_t fdmsg[ISWS_HEADER_SIZE + ISWS_FRAME_DONE_PAYLOAD_SIZE];
            isws_frame_done_t fd = { .frame_id = p.frame_id, .vsync_ns = 1000000000LL,
                                     .status = 0, .reserved = 0 };
            isws_encode_header(fdmsg, ISWS_MSG_FRAME_DONE, 0, ISWS_FRAME_DONE_PAYLOAD_SIZE);
            isws_encode_frame_done(fdmsg + ISWS_HEADER_SIZE, ISWS_FRAME_DONE_PAYLOAD_SIZE, &fd);
            if (write_all(conn, fdmsg, sizeof(fdmsg)) != 0) break;
        } else if (type == ISWS_MSG_GOODBYE) {
            break;
        }
    }
    munmap(map, (size_t)hello.stride * hello.height * ISWS_NUM_BUFFERS);
    close(memfd);
    close(conn);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* 1. Healthy pipeline end to end: tap -> tab switch -> changed frame. */
static void test_e2e_healthy(void) {
    pthread_t server_thr;
    snprintf(g_sock_name, sizeof(g_sock_name), "ioswine-e2e-019-%d", (int)getpid());
    present_test_set_socket_name(g_sock_name);
    g_server_stop = 0;
    g_server_ready = 0;
    g_server_npresents = 0;
    CHECK(pthread_create(&server_thr, NULL, fake_server_thread, NULL) == 0);
    /* Wait for the server to be listening before connecting; without
     * this the connect races the bind and present_init fails. */
    int waited_ms = 0;
    while (__atomic_load_n(&g_server_ready, __ATOMIC_ACQUIRE) == 0 && waited_ms < 5000) {
        usleep(1000);
        waited_ms++;
    }
    CHECK(g_server_ready == 1);
    CHECK(present_init_timeout(720, 1600, 5000) == 0);
    CHECK(present_connected() == 1);

    setup_touch();
#ifdef E2E_RED_DROP
    /* RED-check build only: force the dispatcher drop that the device
     * shows, so the healthy-path assertions below must fail. */
    g_drop = DROP_DISPATCHER;
#endif

    /* Frame A: tab 0 (Library) colour. */
    uint8_t *fb0 = present_framebuffer(0);
    CHECK(fb0 != NULL);
    for (int i = 0; i < 720 * 1600; i++) ((uint32_t *)fb0)[i] = 0xFF112233u;
    CHECK(present_present(0, NULL, 0) == 0);
    int64_t vsync = 0;
    CHECK(present_wait_vsync(&vsync, 5000) == 0);
    CHECK(g_server_npresents == 1);
    CHECK(g_server_pixels[0] == 0xFF112233u);

    /* Tap the Browse tab: window point (107, 791) = client px (214, 1582). */
    CHECK(feed_touch(ISWS_TOUCH_DOWN, 214.0f, 1582.0f) == 0);
    CHECK(feed_touch(ISWS_TOUCH_UP, 214.0f, 1582.0f) == 0);
    CHECK(g_internal_receive_count == 2);
    CHECK(g_drain_events_count == 2);
    CHECK(g_send_event_calls == 2);
    CHECK(stages_reached(STAGE_SENDEVENT));
    CHECK(stages_reached(STAGE_TAB_SWITCH));
    CHECK(g_tab_selected_index == 1);
    /* The HID event carried the window context id, the pinned sender id,
     * display integration, and the tap point as NORMALIZED digitizer
     * coordinates (digitizer coords are [0,1], not window
     * points; the old code passed window points and hit-testing found
     * nothing). Window point (107, 791) = (107/360, 791/800). */
    CHECK(g_last_ctx_id == 42);
    /* Default sender id is the key window's _contextId (* the legacy magic constant is a per-device IORegistry entry id
     * from another device, so it cannot be this device's digitizer
     * id; it is only used with IOSWINE_TOUCH_SENDER_ID=magic). */
    CHECK(g_last_sender_id == 42);
    CHECK(g_last_disp_integrated == 1);
    CHECK(g_last_child_count == 1);
    CHECK_NEAR(g_last_child_x, 107.0 / 360.0, 0.005);
    CHECK_NEAR(g_last_child_y, 791.0 / 800.0, 0.005);
    CHECK(g_last_child_touch == 0); /* UP ends the touch */
    CHECK(strstr(g_log_buf, "[touch] sendEvent: reached with touch count 1") != NULL);
    CHECK(touch_test_fingers_down() == 0);
    CHECK(g_created_count == g_released_count); /* no HID event leak */

    /* Frame B: tab 1 (Browse) colour. The tap changed the frame. */
    uint8_t *fb1 = present_framebuffer(1);
    CHECK(fb1 != NULL);
    for (int i = 0; i < 720 * 1600; i++) ((uint32_t *)fb1)[i] = 0xFF445566u;
    CHECK(present_present(1, NULL, 0) == 0);
    CHECK(present_wait_vsync(&vsync, 5000) == 0);
    CHECK(g_server_npresents == 2);
    CHECK(g_server_pixels[1] == 0xFF445566u);
    CHECK(g_server_pixels[1] != g_server_pixels[0]);
    CHECK(g_server_frame_ids[1] > g_server_frame_ids[0]);

    present_shutdown();
    g_server_stop = 1;
    pthread_join(server_thr, NULL);
    present_test_set_socket_name(NULL);
}

/* 2. Drop at the dispatcher reproduces the device symptom on host:
 * trace shows fetcher thread / internal receive / source signalled,
 * no sendEvent:, tab stays 0, selftest logs after=0. */
static void test_drop_dispatcher_reproduces_device(void) {
    setup_touch();
    g_drop = DROP_DISPATCHER;
    CHECK(feed_touch(ISWS_TOUCH_DOWN, 214.0f, 1582.0f) == 0);
    CHECK(feed_touch(ISWS_TOUCH_UP, 214.0f, 1582.0f) == 0);
    CHECK(strstr(g_log_buf, "[touch] fetcher thread present? 1") != NULL);
    /* Route D (default) drives the chain synchronously on the
     * main queue: direct Internal receive, then the source signal.
     * The old route A log line ("queued via _receiveHIDEvent:") no
     * longer appears: the wrapper is behind IOSWINE_TOUCH_QUEUE=1
     * now, and it is dead on device. The drop is below the drain, so
     * the combined path's sendEvent fallback also fires here (and
     * fails gracefully in this mock, which has no UITouch setters),
     * which is why the tab still stays 0. */
    CHECK(strstr(g_log_buf, "[touch] internal receive\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] source signalled\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] queued via _receiveHIDEvent:") == NULL);
    CHECK(strstr(g_log_buf, "[touch] combined: no sendEvent, falling back to direct\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] sendEvent:") == NULL);
    CHECK(g_send_event_calls == 0);
    CHECK(stages_reached(STAGE_DRAIN));
    CHECK(!stages_reached(STAGE_SENDEVENT));
    CHECK(g_tab_selected_index == 0);

    /* The real selftest path detects the drop: after stays 0. */
    g_env_selftest = "1";
    touch_init();
    g_log_len = 0;
    g_log_buf[0] = '\0';
    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=0") != NULL);
}

/* 3. Drop at the deferring manager is also detected by the selftest. */
static void test_drop_deferring(void) {
    setup_touch();
    g_drop = DROP_DEFERRING;
    CHECK(feed_touch(ISWS_TOUCH_DOWN, 214.0f, 1582.0f) == 0);
    CHECK(feed_touch(ISWS_TOUCH_UP, 214.0f, 1582.0f) == 0);
    CHECK(stages_reached(STAGE_DISPATCHER));
    CHECK(!stages_reached(STAGE_SENDEVENT));
    CHECK(g_tab_selected_index == 0);
    g_env_selftest = "1";
    touch_init();
    g_log_len = 0;
    g_log_buf[0] = '\0';
    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=0") != NULL);
}

/* 4. Healthy selftest: the real touch_on_frame(20) path logs after=1. */
static void test_selftest_healthy(void) {
    setup_touch();
    g_env_selftest = "1";
    touch_init();
    touch_on_frame(19);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);
    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1") != NULL);
    CHECK(g_tab_selected_index == 1);
    /* Second frame 20 call must not re-run the selftest. */
    g_log_len = 0;
    g_log_buf[0] = '\0';
    touch_on_frame(20);
    CHECK(strstr(g_log_buf, "[touch] selftest") == NULL);
}

/* 5. Direct-sendEvent fallback: with the pipeline dropped, a direct
 * sendEvent: call (what the fallback must do in touch.c) still switches
 * the tab. Proves the fallback route works; the real fallback code
 * itself is verified on device. */
static void test_fallback_contract(void) {
    setup_touch();
    g_drop = DROP_DISPATCHER;
    CHECK(feed_touch(ISWS_TOUCH_DOWN, 214.0f, 1582.0f) == 0);
    CHECK(feed_touch(ISWS_TOUCH_UP, 214.0f, 1582.0f) == 0);
    CHECK(g_tab_selected_index == 0);
    /* Fallback: synthesize the touches event and call sendEvent:
     * directly, bypassing the dropped pipeline. */
    g_drop = DROP_NONE;
    g_fake_touch_count = 1;
    /* Normalized digitizer coords, as the HID event
     * carries them: window point (107, 791). */
    g_last_touch_x = 107.0 / 360.0;
    g_last_touch_y = 791.0 / 800.0;
    mock_objc_msgSend(&g_fake_app_obj, &g_sel_send_event, &g_fake_event_obj, NULL);
    CHECK(g_send_event_calls == 1);
    CHECK(g_tab_selected_index == 1);
}

/* 6. Presenter geometry: aspect-fit, scale, letterbox, and the
 * phone-to-window mapping for the selftest tap. */
static void test_presenter_geometry(void) {
    isws_info_t m;
    float cx = 0.0f, cy = 0.0f;
    /* Exact match: 1440x3200 client on a 1440x3200 phone, no black bar. */
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &m) == 0);
    CHECK_NEAR(m.scale, 1.0, 1e-6);
    CHECK_NEAR(m.offset_x, 0.0, 1e-6);
    CHECK_NEAR(m.offset_y, 0.0, 1e-6);
    /* Scale 2.0 client (720x1600) on the same phone: exact aspect, no bar. */
    CHECK(isws_compute_mapping(1440, 3200, 720, 1600, 0, &m) == 0);
    CHECK_NEAR(m.scale, 2.0, 1e-6);
    CHECK_NEAR(m.offset_x, 0.0, 1e-6);
    CHECK_NEAR(m.offset_y, 0.0, 1e-6);
    /* 720x1280 client on 1440x3200: scale 2.0, centred, 320 px bars top/bottom. */
    CHECK(isws_compute_mapping(1440, 3200, 720, 1280, 0, &m) == 0);
    CHECK_NEAR(m.scale, 2.0, 1e-6);
    CHECK_NEAR(m.offset_x, 0.0, 1e-6);
    CHECK_NEAR(m.offset_y, 320.0, 1e-6);
    /* A point in the top bar is outside: -1, clamped into the frame. */
    CHECK(isws_screen_to_client(&m, 720.0f, 100.0f, &cx, &cy) == -1);
    CHECK_NEAR(cx, 360.0, 1e-6);
    CHECK_NEAR(cy, 0.0, 1e-6);
    /* A point inside the frame maps through the offset and scale. */
    CHECK(isws_screen_to_client(&m, 720.0f, 1000.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 360.0, 1e-6);
    CHECK_NEAR(cy, 340.0, 1e-6);
    /* Round trip client -> screen -> client. */
    float sx = 0.0f, sy = 0.0f;
    isws_client_to_screen(&m, 360.0f, 340.0f, &sx, &sy);
    CHECK_NEAR(sx, 720.0, 1e-6);
    CHECK_NEAR(sy, 1000.0, 1e-6);
    /* Invalid mappings are rejected. */
    CHECK(isws_compute_mapping(0, 3200, 720, 1600, 0, &m) == -1);
    CHECK(isws_compute_mapping(1440, 3200, 720, 1600, 45, &m) == -1);
    /* Window-point mapping for the selftest tap: client (428, 3164) at
     * scale 4.0 on the 1440x3200 phone is window point (107, 791). */
    double nx = 0.0, ny = 0.0, wx = 0.0, wy = 0.0;
    isws_touch_t t = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                       .pointer_count = 1, .x = 428.0f, .y = 3164.0f,
                       .timestamp_ns = 0 };
    CHECK(touch_map_coords(&t, 1440, 3200, 360.0, 800.0, &nx, &ny, &wx, &wy) == 0);
    CHECK_NEAR(wx, 107.0, 0.5);
    CHECK_NEAR(wy, 791.0, 0.5);
}

int main(void) {
    /* Unbuffered stdout: CHECK failures must survive a sanitizer abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_e2e_healthy();
    test_drop_dispatcher_reproduces_device();
    test_drop_deferring();
    test_selftest_healthy();
    test_fallback_contract();
    test_presenter_geometry();
    printf("e2e_touch_present_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
