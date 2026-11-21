/*
 * touch_plumbing_test.c - Host unit tests for presenter-side
 * touch protocol + hidevent plumbing.
 *
 * Plain cc + ASan/UBSan on Linux host. Covers:
 *   1. touch_input mapper: single tap, letterbox rule, CANCEL-all,
 *      pointer-id slot mapping (no aliasing), MOVE history, MOVE for
 *      an unknown pointer starting a contact, mapping-change cancel,
 *      ignored actions.
 *   2. present.c: an invalid TOUCH payload is dropped and counted and
 *      does NOT wedge the pump (a valid TOUCH after it is delivered);
 *      a bad header is still fatal. Queue-full prefers dropping the
 *      oldest MOVE over a DOWN/UP.
 *   3. touch.c reader dispatch: FOCUS lost cancels every active finger;
 *      a stale wire timestamp is dropped and counted; ts=0 is accepted.
 *   4. Selftest: with a real INFO mapping the tap lands at window
 *      point (107, 791), and the sendevent count is reported.
 *   5. MOVE log sampling and the hid-built detail line.
 *   6. Ring-full and invalid-coord drop counters.
 *   7. hidevent / touch.c finger-table agreement on a scripted
 *      sequence, with the known identical re-DOWN divergence pinned.
 *   8. Fuzz: random wire bytes through the presenter feed hook, random
 *      motion events through the mapper, random touches through
 *      hidevent_touch.
 *
 * The mock ObjC / IOKit / libdispatch harness is adapted from
 * tests/host/touch_test.c (same fake layouts and selector model).
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
#include "touch_input.h"
#include "hidevent.h"

extern int present_test_feed(const uint8_t *data, size_t len);
extern void present_test_reset_rx(void);
extern void present_test_set_socket_name(const char *name);

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

static uint32_t rng_state = 0x12345678u;
static uint32_t rng_u32(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* ------------------------------------------------------------------ */
/* Mock IOKit / CoreFoundation / ObjC / libdispatch                   */
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

static int g_received_count = 0;
static mock_hid_event_t g_last_received;
static mock_hid_event_t g_last_children[MAX_MOCK_CHILDREN];

static int g_dispatch_calls = 0;
static int g_defer_dispatch = 0;
#define MAX_DEFERRED 512
static void (*g_deferred_work[MAX_DEFERRED])(void *);
static void *g_deferred_ctx[MAX_DEFERRED];
static int g_deferred_n = 0;

static const char *g_env_touch = NULL;
static const char *g_env_trace = NULL;
static const char *g_env_selftest = NULL;
static char g_log_buf[16384];
static size_t g_log_len = 0;

static int g_internal_receive_count = 0;
static int g_signal_count = 0;
static int g_wakeup_count = 0;
static int g_drain_events_count = 0;
static int g_orig_send_event_called = 0;
static uint32_t g_fake_context_id = 0;
static uint64_t g_tab_selected_index = 0;
static uint64_t g_fake_touch_count = 0;
static int g_drain_call_send_event = 0;
static void (*g_mock_send_event_imp)(void *self, void *cmd, void *event) = NULL;

static void mock_orig_send_event(void *self, void *cmd, void *event) {
    (void)self; (void)cmd; (void)event;
    g_orig_send_event_called++;
}

static void reset_mocks(void) {
    memset(g_mock_events, 0, sizeof(g_mock_events));
    g_created_count = 0;
    g_released_count = 0;
    g_received_count = 0;
    memset(&g_last_received, 0, sizeof(g_last_received));
    memset(g_last_children, 0, sizeof(g_last_children));
    g_dispatch_calls = 0;
    g_defer_dispatch = 0;
    g_deferred_n = 0;
    g_env_touch = NULL;
    g_env_trace = NULL;
    g_env_selftest = NULL;
    memset(g_log_buf, 0, sizeof(g_log_buf));
    g_log_len = 0;
    g_internal_receive_count = 0;
    g_signal_count = 0;
    g_wakeup_count = 0;
    g_drain_events_count = 0;
    g_orig_send_event_called = 0;
    g_fake_context_id = 0;
    g_tab_selected_index = 0;
    g_fake_touch_count = 0;
    g_drain_call_send_event = 0;
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
    if (field == 0xb0019u) ev->disp_integrated = value;
    else if (field == 0xb0005u) ev->context_id = value;
}

static void mock_IOHIDEventSetSenderID(void *ev_v, uint64_t senderID) {
    mock_hid_event_t *ev = (mock_hid_event_t *)ev_v;
    if (!ev) return;
    ev->sender_id = senderID;
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

static void mock_CFRunLoopSourceSignal(void *source) {
    if (source == &g_fake_source_obj) g_signal_count++;
}

static void mock_CFRunLoopWakeUp(void *rl) {
    if (rl == &g_fake_rl_obj) g_wakeup_count++;
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
    return NULL;
}

/* Forward declarations: the _receiveHIDEvent: branch below runs the
 * fetch-thread chain synchronously (internal receive + drain), so it
 * needs these before their definitions. */
static void mock_receive_internal(void *fetcher, void *event);
static void mock_drain_events(void *fetcher, void *environment);

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
        if (g_mock_send_event_imp) g_mock_send_event_imp(obj, sel, arg1);
        else mock_orig_send_event(obj, sel, arg1);
        return NULL;
    }
    if (obj == &g_fake_fetcher_obj && sel == &g_sel_receive_hid) {
        mock_hid_event_t *ev = (mock_hid_event_t *)arg1;
        g_received_count++;
        if (ev) {
            g_last_received = *ev;
            for (int i = 0; i < ev->n_children && i < MAX_MOCK_CHILDREN; i++) {
                g_last_children[i] = *ev->children[i];
            }
        }
        /* Route A wrapper: real UIKit forwards the retained
         * event to _receiveHIDEventInternal: on the fetch thread and
         * the dispatcher then drains into sendEvent:. The mock runs
         * that chain synchronously so the test observes it end to
         * end. Children carry NORMALIZED [0,1] coordinates;
         * mock_receive_internal multiplies by the window size
         * (360x800 pt) before hit-testing. */
        mock_receive_internal(obj, ev);
        mock_drain_events(obj, &g_fake_env_obj);
        return NULL;
    }
    return NULL;
}

static void *mock_pool_push(void) { return (void *)(uintptr_t)0x1234; }
static void mock_pool_pop(void *p) { (void)p; }

static void mock_dispatch_async_f(void *q, void *ctx, void (*work)(void *)) {
    (void)q;
    g_dispatch_calls++;
    if (g_defer_dispatch) {
        if (g_deferred_n < MAX_DEFERRED) {
            g_deferred_work[g_deferred_n] = work;
            g_deferred_ctx[g_deferred_n] = ctx;
            g_deferred_n++;
        }
        return;
    }
    if (work) work(ctx);
}

static void drain_deferred(void) {
    int n = g_deferred_n;
    g_deferred_n = 0;
    for (int i = 0; i < n; i++) {
        if (g_deferred_work[i]) g_deferred_work[i](g_deferred_ctx[i]);
    }
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

static void mock_receive_internal(void *fetcher, void *event) {
    (void)fetcher;
    mock_hid_event_t *ev = (mock_hid_event_t *)event;
    g_internal_receive_count++;
    if (ev) {
        g_last_received = *ev;
        for (int i = 0; i < ev->n_children && i < MAX_MOCK_CHILDREN; i++) {
            g_last_children[i] = *ev->children[i];
            /* Children carry NORMALIZED [0,1] coordinates:
             * the tap point (107, 791) pt is (107/360, 791/800)
             * in normalized units. The old window-point comparison
             * (x near 107, y near 791) could never match again. */
            if (fabs(ev->children[i]->x - 107.0 / 360.0) < 0.003 &&
                fabs(ev->children[i]->y - 791.0 / 800.0) < 0.003 &&
                ev->children[i]->touch == 1) {
                g_tab_selected_index = 1;
            }
        }
    }
}

static void mock_drain_events(void *fetcher, void *environment) {
    (void)fetcher;
    (void)environment;
    g_drain_events_count++;
    if (g_drain_call_send_event) {
        mock_objc_msgSend(&g_fake_app_obj, &g_sel_send_event, &g_fake_event_obj, NULL);
    }
}

static touch_test_ops_t g_ops = {
    .dlsym_default = mock_dlsym,
    .dlopen_fn = mock_dlopen,
    .getenv_fn = mock_getenv,
    .mach_abs_time = mock_mach_abs_time,
    .log_fn = mock_log,
    .receive_internal_fn = NULL,
    .drain_events_fn = NULL,
};

/* Env vars must be set before touch_init() reads them. */
static void setup_touch_with(int trace, int selftest) {
    /* g_ops is static: touch_test_configure keeps the pointer. */
    g_ops.receive_internal_fn = mock_receive_internal;
    g_ops.drain_events_fn = mock_drain_events;
    touch_test_reset();
    reset_mocks();
    touch_test_configure(&g_ops);
    g_env_touch = "1";
    g_env_trace = trace ? "1" : NULL;
    g_env_selftest = selftest ? "1" : NULL;
    touch_init();
    touch_set_window_size(360.0, 800.0);
}

static void setup_touch(void) {
    setup_touch_with(0, 0);
}

static int count_log_lines(const char *needle) {
    int n = 0;
    const char *p = g_log_buf;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Mapper tests                                                       */
/* ------------------------------------------------------------------ */

#define MAX_EMITTED 256
static isws_touch_t g_emitted[MAX_EMITTED];
static int g_emitted_n = 0;

static void collect_emit(void *ctx, const isws_touch_t *t) {
    (void)ctx;
    if (g_emitted_n < MAX_EMITTED && t) {
        g_emitted[g_emitted_n++] = *t;
    }
}

static void reset_emit(void) {
    g_emitted_n = 0;
    memset(g_emitted, 0, sizeof(g_emitted));
}

/* 1440x3200 screen, 1080x2400 client: scale 4/3, no bars. */
static void make_map_full(isws_info_t *map) {
    isws_compute_mapping(1440, 3200, 1080, 2400, 0, map);
}

/* 1440x3200 screen, 1080x1920 client: scale 4/3, 320 px bars top/bottom. */
static void make_map_bars(isws_info_t *map) {
    isws_compute_mapping(1440, 3200, 1080, 1920, 0, map);
}

static ti_motion_t motion_down(uint32_t android_id, float sx, float sy, int64_t ts) {
    ti_motion_t m;
    memset(&m, 0, sizeof(m));
    m.masked_action = TI_ACTION_DOWN;
    m.pointer_index = 0;
    m.pointer_count = 1;
    m.timestamp_ns = ts;
    m.pointers[0].android_id = android_id;
    m.pointers[0].x = sx;
    m.pointers[0].y = sy;
    m.pointers[0].pressure = 1.0f;
    return m;
}

static void test_mapper_single_tap(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    reset_emit();

    ti_motion_t down = motion_down(0, 540.0f, 1200.0f, 1000);
    CHECK(ti_mapper_feed(&mapper, &down, collect_emit, NULL) == 1);
    CHECK(g_emitted_n == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_DOWN);
    CHECK(g_emitted[0].pointer_id == 0);
    CHECK(g_emitted[0].pointer_count == 1);
    CHECK_NEAR(g_emitted[0].x, 405.0, 1e-3);
    CHECK_NEAR(g_emitted[0].y, 900.0, 1e-3);
    CHECK(g_emitted[0].timestamp_ns == 1000);
    CHECK(ti_mapper_active_count(&mapper) == 1);

    ti_motion_t up = down;
    up.masked_action = TI_ACTION_UP;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &up, collect_emit, NULL) == 1);
    CHECK(g_emitted_n == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_UP);
    CHECK(g_emitted[0].pointer_count == 1);
    CHECK(ti_mapper_active_count(&mapper) == 0);
    CHECK(mapper.stats.emitted == 2);
}

static void test_mapper_letterbox(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_bars(&map);
    CHECK_NEAR(map.offset_y, 320.0, 1e-3);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    reset_emit();

    /* DOWN in the top black bar: dropped, no contact begins (F3). */
    ti_motion_t down_bar = motion_down(0, 720.0f, 100.0f, 2000);
    CHECK(ti_mapper_feed(&mapper, &down_bar, collect_emit, NULL) == 0);
    CHECK(g_emitted_n == 0);
    CHECK(mapper.stats.dropped_down_outside == 1);
    CHECK(ti_mapper_active_count(&mapper) == 0);

    /* DOWN inside the frame: client (540, 960). */
    ti_motion_t down = motion_down(0, 720.0f, 1600.0f, 3000);
    CHECK(ti_mapper_feed(&mapper, &down, collect_emit, NULL) == 1);
    CHECK_NEAR(g_emitted[0].x, 540.0, 1e-3);
    CHECK_NEAR(g_emitted[0].y, 960.0, 1e-3);

    /* MOVE back into the bar: clamped to the top edge, still emitted. */
    ti_motion_t move = down;
    move.masked_action = TI_ACTION_MOVE;
    move.pointers[0].x = 720.0f;
    move.pointers[0].y = 100.0f;
    move.timestamp_ns = 4000;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &move, collect_emit, NULL) == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_MOVE);
    CHECK_NEAR(g_emitted[0].x, 540.0, 1e-3);
    CHECK_NEAR(g_emitted[0].y, 0.0, 1e-3);

    /* UP in the bar: clamped, finger released (F3). */
    ti_motion_t up = move;
    up.masked_action = TI_ACTION_UP;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &up, collect_emit, NULL) == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_UP);
    CHECK_NEAR(g_emitted[0].y, 0.0, 1e-3);
    CHECK(ti_mapper_active_count(&mapper) == 0);
}

static void test_mapper_cancel_all_and_aliasing(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    reset_emit();

    /* Two fingers: Android ids 0 and 5. */
    ti_motion_t d0 = motion_down(0, 360.0f, 800.0f, 1000);
    ti_motion_t d1 = motion_down(5, 1080.0f, 2400.0f, 1100);
    d1.pointer_index = 0;
    d1.pointer_count = 2;
    CHECK(ti_mapper_feed(&mapper, &d0, collect_emit, NULL) == 1);
    CHECK(ti_mapper_feed(&mapper, &d1, collect_emit, NULL) == 1);
    CHECK(ti_mapper_active_count(&mapper) == 2);
    /* Distinct slots: id 5 did not alias onto slot 0. */
    CHECK(g_emitted[0].pointer_id == 0);
    CHECK(g_emitted[1].pointer_id == 1);
    CHECK(g_emitted[1].pointer_count == 2);

    /* Android ids 10 and 20 must NOT alias onto slots 0 and 1 (F4):
     * they get their own slots. */
    reset_emit();
    ti_motion_t d10 = motion_down(10, 100.0f, 100.0f, 1200);
    ti_motion_t d20 = motion_down(20, 200.0f, 200.0f, 1300);
    CHECK(ti_mapper_feed(&mapper, &d10, collect_emit, NULL) == 1);
    CHECK(g_emitted[0].pointer_id == 2);
    CHECK(ti_mapper_feed(&mapper, &d20, collect_emit, NULL) == 1);
    CHECK(g_emitted[1].pointer_id == 3);
    CHECK(ti_mapper_active_count(&mapper) == 4);

    /* CANCEL cancels every active pointer, not just the changed one (F2). */
    ti_motion_t cancel;
    memset(&cancel, 0, sizeof(cancel));
    cancel.masked_action = TI_ACTION_CANCEL;
    cancel.pointer_index = 0;
    cancel.pointer_count = 4;
    cancel.timestamp_ns = 2000;
    cancel.pointers[0].android_id = 0;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &cancel, collect_emit, NULL) == 4);
    CHECK(g_emitted_n == 4);
    CHECK(g_emitted[0].action == ISWS_TOUCH_CANCEL);
    CHECK(g_emitted[1].action == ISWS_TOUCH_CANCEL);
    CHECK(g_emitted[2].action == ISWS_TOUCH_CANCEL);
    CHECK(g_emitted[3].action == ISWS_TOUCH_CANCEL);
    CHECK(g_emitted[0].pointer_id == 0);
    CHECK(g_emitted[1].pointer_id == 1);
    CHECK(g_emitted[2].pointer_id == 2);
    CHECK(g_emitted[3].pointer_id == 3);
    CHECK(g_emitted[0].pointer_count == 4);
    CHECK(g_emitted[3].pointer_count == 1);
    CHECK(mapper.stats.cancel_all_sent == 4);
    CHECK(ti_mapper_active_count(&mapper) == 0);

    /* CANCEL with nothing down is ignored, not an error. */
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &cancel, collect_emit, NULL) == 0);
    CHECK(mapper.stats.ignored_actions == 1);
}

static void test_mapper_no_slot(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    for (uint32_t i = 0; i < ISWS_MAX_POINTERS; i++) {
        ti_motion_t d = motion_down(i, 100.0f + (float)i, 100.0f, 1000 + i);
        CHECK(ti_mapper_feed(&mapper, &d, collect_emit, NULL) == 1);
    }
    CHECK(ti_mapper_active_count(&mapper) == ISWS_MAX_POINTERS);
    ti_motion_t extra = motion_down(99, 500.0f, 500.0f, 9999);
    CHECK(ti_mapper_feed(&mapper, &extra, collect_emit, NULL) == 0);
    CHECK(mapper.stats.dropped_no_slot == 1);
    CHECK(ti_mapper_active_count(&mapper) == ISWS_MAX_POINTERS);
}

static void test_mapper_history_and_dedupe(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);

    ti_motion_t down = motion_down(0, 360.0f, 800.0f, 1000);
    CHECK(ti_mapper_feed(&mapper, &down, collect_emit, NULL) == 1);

    /* MOVE with 2 historical samples then the current sample (F5). */
    ti_motion_t move;
    memset(&move, 0, sizeof(move));
    move.masked_action = TI_ACTION_MOVE;
    move.pointer_index = 0;
    move.pointer_count = 1;
    move.timestamp_ns = 2000;
    move.pointers[0].android_id = 0;
    move.pointers[0].x = 720.0f;
    move.pointers[0].y = 1600.0f;
    move.history_count = 2;
    move.hist_x[0][0] = 400.0f; move.hist_y[0][0] = 900.0f;
    move.hist_x[1][0] = 540.0f; move.hist_y[1][0] = 1200.0f;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &move, collect_emit, NULL) == 3);
    CHECK(g_emitted_n == 3);
    /* Oldest first: history 0, history 1, current. Client coords are
     * screen coords * 3/4. */
    CHECK_NEAR(g_emitted[0].x, 300.0, 1e-3);
    CHECK_NEAR(g_emitted[0].y, 675.0, 1e-3);
    CHECK_NEAR(g_emitted[1].x, 405.0, 1e-3);
    CHECK_NEAR(g_emitted[1].y, 900.0, 1e-3);
    CHECK_NEAR(g_emitted[2].x, 540.0, 1e-3);
    CHECK_NEAR(g_emitted[2].y, 1200.0, 1e-3);
    for (int i = 0; i < 3; i++) {
        CHECK(g_emitted[i].action == ISWS_TOUCH_MOVE);
        CHECK(g_emitted[i].pointer_id == 0);
    }

    /* An identical MOVE with no history is suppressed at the source.
     * (Re-feeding the same event with its history would replay the old
     * samples, which are not identical to the last emitted position,
     * so that is not a duplicate.) */
    ti_motion_t move2;
    memset(&move2, 0, sizeof(move2));
    move2.masked_action = TI_ACTION_MOVE;
    move2.pointer_index = 0;
    move2.pointer_count = 1;
    move2.timestamp_ns = 3000;
    move2.pointers[0].android_id = 0;
    move2.pointers[0].x = 720.0f;
    move2.pointers[0].y = 1600.0f;
    move2.history_count = 0;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &move2, collect_emit, NULL) == 0);
    CHECK(mapper.stats.dropped_dup_move == 1);
}

static void test_mapper_move_unknown_starts_down(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);

    /* A MOVE for a pointer whose DOWN was never seen (for example it
     * began in a bar and slid in) starts a contact with a DOWN. */
    ti_motion_t move;
    memset(&move, 0, sizeof(move));
    move.masked_action = TI_ACTION_MOVE;
    move.pointer_index = 0;
    move.pointer_count = 1;
    move.timestamp_ns = 1000;
    move.pointers[0].android_id = 7;
    move.pointers[0].x = 720.0f;
    move.pointers[0].y = 1600.0f;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &move, collect_emit, NULL) == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_DOWN);
    CHECK(ti_mapper_active_count(&mapper) == 1);

    /* UP for an unknown pointer is dropped, not an error. */
    ti_motion_t up = move;
    up.masked_action = TI_ACTION_UP;
    up.pointers[0].android_id = 8;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &up, collect_emit, NULL) == 0);
    CHECK(mapper.stats.dropped_unknown_up == 1);
}

static void test_mapper_set_map_cancels(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    isws_info_t map2;
    make_map_full(&map);
    make_map_bars(&map2);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);

    ti_motion_t down = motion_down(0, 360.0f, 800.0f, 1000);
    CHECK(ti_mapper_feed(&mapper, &down, collect_emit, NULL) == 1);

    /* Same mapping again: no-op, no CANCEL. */
    reset_emit();
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    CHECK(g_emitted_n == 0);
    CHECK(ti_mapper_active_count(&mapper) == 1);

    /* Different geometry under a live contact: CANCEL first. */
    reset_emit();
    ti_mapper_set_map(&mapper, &map2, collect_emit, NULL);
    CHECK(g_emitted_n == 1);
    CHECK(g_emitted[0].action == ISWS_TOUCH_CANCEL);
    CHECK(ti_mapper_active_count(&mapper) == 0);

    /* No mapping at all: events are ignored. */
    ti_mapper_t fresh;
    ti_mapper_init(&fresh);
    CHECK(ti_mapper_feed(&fresh, &down, collect_emit, NULL) == 0);
    CHECK(fresh.stats.ignored_actions == 1);

    /* Other actions (hover etc.) are ignored. */
    ti_motion_t other = down;
    other.masked_action = TI_ACTION_OTHER;
    reset_emit();
    CHECK(ti_mapper_feed(&mapper, &other, collect_emit, NULL) == 0);
    CHECK(mapper.stats.ignored_actions == 1);
}

/* ------------------------------------------------------------------ */
/* present.c: invalid TOUCH drop, queue MOVE preference               */
/* ------------------------------------------------------------------ */

static void feed_msg(uint16_t type, const uint8_t *pay, uint32_t len) {
    uint8_t buf[ISWS_HEADER_SIZE + 65535];
    isws_encode_header(buf, type, 0, len);
    if (len > 0) memcpy(buf + ISWS_HEADER_SIZE, pay, len);
    int rc = present_test_feed(buf, ISWS_HEADER_SIZE + len);
    CHECK(rc == 0);
}

static void test_present_invalid_touch_does_not_wedge(void) {
    uint8_t pay[ISWS_TOUCH_PAYLOAD_SIZE];
    isws_touch_t good = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 100.0f, .y = 200.0f,
                          .timestamp_ns = 42 };
    isws_encode_touch(pay, sizeof(pay), &good);

    present_test_reset_rx();
    CHECK(present_touch_invalid_dropped() == 0);

    /* Invalid action (9). Must be dropped, counted, and the pump must
     * keep working (F1). */
    uint8_t bad_action[ISWS_TOUCH_PAYLOAD_SIZE];
    memcpy(bad_action, pay, sizeof(bad_action));
    isws_wr_u32le(bad_action + 0, 9);
    {
        uint8_t buf[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
        isws_encode_header(buf, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE);
        memcpy(buf + ISWS_HEADER_SIZE, bad_action, sizeof(bad_action));
        CHECK(present_test_feed(buf, sizeof(buf)) == 0);
    }
    CHECK(present_touch_invalid_dropped() == 1);

    /* Invalid pointer id (10). */
    uint8_t bad_id[ISWS_TOUCH_PAYLOAD_SIZE];
    memcpy(bad_id, pay, sizeof(bad_id));
    isws_wr_u32le(bad_id + 4, ISWS_MAX_POINTERS);
    {
        uint8_t buf[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
        isws_encode_header(buf, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE);
        memcpy(buf + ISWS_HEADER_SIZE, bad_id, sizeof(bad_id));
        CHECK(present_test_feed(buf, sizeof(buf)) == 0);
    }
    CHECK(present_touch_invalid_dropped() == 2);

    /* Invalid pointer count (0). */
    uint8_t bad_count[ISWS_TOUCH_PAYLOAD_SIZE];
    memcpy(bad_count, pay, sizeof(bad_count));
    isws_wr_u32le(bad_count + 8, 0);
    {
        uint8_t buf[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
        isws_encode_header(buf, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE);
        memcpy(buf + ISWS_HEADER_SIZE, bad_count, sizeof(bad_count));
        CHECK(present_test_feed(buf, sizeof(buf)) == 0);
    }
    CHECK(present_touch_invalid_dropped() == 3);

    /* A valid TOUCH after the invalid ones is delivered: the pump is
     * not wedged. Before the fix this feed returned -1 and every
     * later message was re-dispatched and failed. */
    feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
    present_event_t ev;
    CHECK(present_poll_event(&ev) == 1);
    CHECK(ev.kind == PRESENT_EV_TOUCH);
    CHECK(ev.u.touch.action == ISWS_TOUCH_DOWN);
    CHECK(ev.u.touch.pointer_id == 0);
    CHECK_NEAR(ev.u.touch.x, 100.0, 1e-5);
    CHECK(present_poll_event(&ev) == 0);
    CHECK(present_touch_invalid_dropped() == 3);

    /* A bad header is still fatal, as before. */
    present_test_reset_rx();
    {
        uint8_t garbage[16];
        memset(garbage, 0x5a, sizeof(garbage));
        CHECK(present_test_feed(garbage, sizeof(garbage)) != 0);
    }
    present_test_reset_rx();
}

static void test_present_queue_prefers_move_drop(void) {
    uint8_t pay[ISWS_TOUCH_PAYLOAD_SIZE];
    present_test_reset_rx();
    CHECK(present_dropped_events() == 0);

    /* Fill the 64-slot queue: 1 DOWN + 63 MOVEs. */
    isws_touch_t down = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 100.0f, .y = 100.0f,
                          .timestamp_ns = 1 };
    isws_encode_touch(pay, sizeof(pay), &down);
    feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
    for (int i = 0; i < 63; i++) {
        isws_touch_t mv = { .action = ISWS_TOUCH_MOVE, .pointer_id = 0,
                            .pointer_count = 1, .x = 100.0f + (float)i,
                            .y = 100.0f, .timestamp_ns = 2 + i };
        isws_encode_touch(pay, sizeof(pay), &mv);
        feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
    }

    /* One more event: the UP. The queue must drop the oldest MOVE,
     * not the DOWN and not the UP. */
    isws_touch_t up = { .action = ISWS_TOUCH_UP, .pointer_id = 0,
                        .pointer_count = 1, .x = 162.0f, .y = 100.0f,
                        .timestamp_ns = 100 };
    isws_encode_touch(pay, sizeof(pay), &up);
    feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
    CHECK(present_dropped_events() == 1);

    int n_down = 0, n_move = 0, n_up = 0, total = 0;
    uint32_t first_action = 0;
    present_event_t ev;
    while (present_poll_event(&ev) == 1) {
        total++;
        if (total == 1) first_action = ev.u.touch.action;
        if (ev.u.touch.action == ISWS_TOUCH_DOWN) n_down++;
        if (ev.u.touch.action == ISWS_TOUCH_MOVE) n_move++;
        if (ev.u.touch.action == ISWS_TOUCH_UP) n_up++;
    }
    CHECK(total == 64);
    CHECK(first_action == ISWS_TOUCH_DOWN);
    CHECK(n_down == 1);
    CHECK(n_move == 62);
    CHECK(n_up == 1);
    present_test_reset_rx();
}

/* ------------------------------------------------------------------ */
/* touch.c reader dispatch                                            */
/* ------------------------------------------------------------------ */

static void feed_info_and_drain(void) {
    /* Deliver an INFO (1080x2400 client) through the real feed path so
     * present_last_info() is populated, then drain the queued INFO. */
    isws_info_t info;
    uint8_t pay[ISWS_INFO_PAYLOAD_SIZE];
    isws_compute_mapping(1080, 2400, 1080, 2400, 0, &info);
    isws_encode_info(pay, sizeof(pay), &info);
    feed_msg(ISWS_MSG_INFO, pay, sizeof(pay));
    present_event_t ev;
    while (present_poll_event(&ev) == 1) {
        touch_handle_present_event(&ev);
    }
}

static void feed_touch_and_handle(const isws_touch_t *t) {
    uint8_t pay[ISWS_TOUCH_PAYLOAD_SIZE];
    isws_encode_touch(pay, sizeof(pay), t);
    feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
    present_event_t ev;
    while (present_poll_event(&ev) == 1) {
        touch_handle_present_event(&ev);
    }
}

static void test_reader_focus_cancels(void) {
    present_test_reset_rx();
    setup_touch_with(1, 0);
    g_fake_touch_count = 1;
    g_fake_context_id = 0x12345u;
    feed_info_and_drain();

    isws_touch_t down = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 540.0f, .y = 1200.0f,
                          .timestamp_ns = 1000000 };
    feed_touch_and_handle(&down);
    CHECK(touch_test_fingers_down() == 1);
    CHECK(g_internal_receive_count == 1);

    /* Focus lost: every active finger is cancelled (F7). */
    present_event_t focus_ev;
    memset(&focus_ev, 0, sizeof(focus_ev));
    focus_ev.kind = PRESENT_EV_FOCUS;
    focus_ev.u.focus.focused = 0;
    CHECK(touch_handle_present_event(&focus_ev) == 0);
    CHECK(touch_test_fingers_down() == 0);
    uint64_t rf = 0, inv = 0, stale = 0, cancel = 0;
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(cancel == 1);
    CHECK(strstr(g_log_buf, "[touch] cancel x=180 y=400 id=0\n") != NULL);

    /* Focus gained: no-op, nothing cancelled, no crash. */
    focus_ev.u.focus.focused = 1;
    CHECK(touch_handle_present_event(&focus_ev) == 0);
    CHECK(touch_test_fingers_down() == 0);

    /* A fresh DOWN after the cancel works: the finger is not stuck. */
    feed_touch_and_handle(&down);
    CHECK(touch_test_fingers_down() == 1);
    CHECK(g_created_count == g_released_count);
    present_test_reset_rx();
}

static void test_reader_stale_timestamp(void) {
    present_test_reset_rx();
    setup_touch();
    feed_info_and_drain();

    isws_touch_t down = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 540.0f, .y = 1200.0f,
                          .timestamp_ns = 2000000000LL };
    feed_touch_and_handle(&down);
    CHECK(touch_test_fingers_down() == 1);

    /* A MOVE 1.5 s older than the last accepted timestamp is stale. */
    isws_touch_t stale_move = down;
    stale_move.action = ISWS_TOUCH_MOVE;
    stale_move.x = 600.0f;
    stale_move.timestamp_ns = 500000000LL;
    {
        uint8_t pay[ISWS_TOUCH_PAYLOAD_SIZE];
        isws_encode_touch(pay, sizeof(pay), &stale_move);
        feed_msg(ISWS_MSG_TOUCH, pay, sizeof(pay));
        present_event_t ev;
        int rc = 0;
        while (present_poll_event(&ev) == 1) {
            rc = touch_handle_present_event(&ev);
        }
        CHECK(rc == -1);
    }
    uint64_t rf = 0, inv = 0, stale = 0, cancel = 0;
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(stale == 1);
    /* The finger did not move to the stale position. Children carry
     * NORMALIZED [0,1] coordinates: the DOWN at client
     * (540, 1200) in a 1080x2400 client is (0.5, 0.5). */
    CHECK_NEAR(g_last_children[0].x, 0.5, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.5, 1e-6);

    /* A MOVE 0.5 s older is within tolerance and accepted. */
    isws_touch_t ok_move = down;
    ok_move.action = ISWS_TOUCH_MOVE;
    ok_move.x = 600.0f;
    ok_move.timestamp_ns = 1500000000LL;
    feed_touch_and_handle(&ok_move);
    CHECK_NEAR(g_last_children[0].x, 600.0 / 1080.0, 1e-6);
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(stale == 1);

    /* A newer MOVE is accepted, and a timestamp of 0 (unknown,
     * synthetic) is always accepted. */
    isws_touch_t new_move = down;
    new_move.action = ISWS_TOUCH_MOVE;
    new_move.x = 700.0f;
    new_move.timestamp_ns = 2100000000LL;
    feed_touch_and_handle(&new_move);
    isws_touch_t zero_move = down;
    zero_move.action = ISWS_TOUCH_MOVE;
    zero_move.x = 800.0f;
    zero_move.timestamp_ns = 0;
    feed_touch_and_handle(&zero_move);
    CHECK_NEAR(g_last_children[0].x, 800.0f / 1080.0f, 1e-4);
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(stale == 1);
    present_test_reset_rx();
}

static void test_selftest_sendevent_count(void) {
    present_test_reset_rx();
    setup_touch_with(1, 1);
    g_fake_touch_count = 1;
    g_fake_context_id = 0x54321u;
    g_drain_call_send_event = 1;
    g_tab_selected_index = 0;
    feed_info_and_drain();

    touch_on_frame(20);

    /* With a real INFO mapping (1080x2400 client) the tap still lands
     * at window point (107, 791) (F6). */
    CHECK(strstr(g_log_buf, "[touch] down x=107 y=791 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] up x=107 y=791 id=0\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] selftest tab before=0 after=1\n") != NULL);
    /* End-to-end delivery: the drain drove sendEvent: through the hook. */
    CHECK(touch_test_sendevent_count() >= 1);
    CHECK(strstr(g_log_buf, "[touch] selftest sendevent=") != NULL);
    CHECK(g_orig_send_event_called >= 1);
    present_test_reset_rx();
}

static void test_move_log_sampling_and_detail(void) {
    setup_touch_with(1, 0);
    g_fake_touch_count = 1;
    g_fake_context_id = 0x12345u;

    isws_touch_t down = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 540.0f, .y = 1200.0f,
                          .timestamp_ns = 1000000 };
    CHECK(touch_test_process_touch(&down, 1080, 2400) == 0);
    /* 40 distinct MOVEs: only the 1st, 17th and 33rd log (F10). */
    for (int i = 1; i <= 40; i++) {
        isws_touch_t mv = down;
        mv.action = ISWS_TOUCH_MOVE;
        mv.x = 540.0f + (float)i;
        mv.y = 1200.0f;
        mv.timestamp_ns = 1000000 + i;
        CHECK(touch_test_process_touch(&mv, 1080, 2400) == 0);
    }
    CHECK(count_log_lines("[touch] move ") == 3);
    CHECK(count_log_lines("[touch] down ") == 1);
    /* The trace detail line carries normalized coords and the wire ts. */
    CHECK(strstr(g_log_buf, "[touch] hid-built down nx=500 ny=500 id=0 wire_ts=1000000\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] hid-built move nx=") != NULL);
}

static void test_ring_full_and_invalid_counts(void) {
    setup_touch();
    g_fake_touch_count = 1;

    /* Defer the main-queue work so the ring fills up. */
    g_defer_dispatch = 1;
    isws_touch_t down = { .action = ISWS_TOUCH_DOWN, .pointer_id = 0,
                          .pointer_count = 1, .x = 540.0f, .y = 1200.0f,
                          .timestamp_ns = 1000 };
    int rc = 0;
    for (int i = 0; i < 257; i++) {
        isws_touch_t t = down;
        t.pointer_id = (uint32_t)(i % 10);
        t.timestamp_ns = 1000 + i;
        rc = touch_test_process_touch(&t, 1080, 2400);
    }
    CHECK(rc == -1);
    uint64_t rf = 0, inv = 0, stale = 0, cancel = 0;
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(rf >= 1);
    g_defer_dispatch = 0;
    drain_deferred();
    touch_test_reset();
    reset_mocks();

    /* Invalid coords are counted, not silently dropped (F9). */
    setup_touch();
    isws_touch_t bad = down;
    bad.x = NAN;
    CHECK(touch_test_process_touch(&bad, 1080, 2400) == -1);
    touch_test_drop_counts(&rf, &inv, &stale, &cancel);
    CHECK(inv == 1);
    CHECK(rf == 0);
}

/* ------------------------------------------------------------------ */
/* hidevent / touch.c finger-table agreement                          */
/* ------------------------------------------------------------------ */

static isws_touch_t norm_touch(uint32_t action, uint32_t id, float nx, float ny) {
    isws_touch_t t;
    t.action = action;
    t.pointer_id = id;
    t.pointer_count = 1;
    t.x = nx;
    t.y = ny;
    t.timestamp_ns = 1000;
    return t;
}

static void check_tables_agree(const char *step) {
    (void)step;
    CHECK(touch_test_fingers_down() == hidevent_fingers_down());
}

static void test_hidevent_touch_agreement(void) {
    uint8_t out[1144];
    setup_touch();
    g_fake_touch_count = 1;
    hidevent_reset();

    /* Scripted sequence, client pixels 1080x2400, window 360x800. */
    isws_touch_t t;
    int r;

    t = norm_touch(ISWS_TOUCH_DOWN, 0, 0.5f, 0.5f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    t.x = 540.0f; t.y = 1200.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("down0");

    t = norm_touch(ISWS_TOUCH_MOVE, 0, 0.25f, 0.25f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    t.x = 270.0f; t.y = 600.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("move0");
    /* Child positions agree: hidevent child coords are IOFixed 16.16
     * (0.25 -> 16384), touch.c child coords are NORMALIZED [0,1]
     * (the old code carried window points). */
    CHECK(isws_rd_u32le(out + 104 + 16) == 16384u);
    CHECK(isws_rd_u32le(out + 104 + 20) == 16384u);
    CHECK_NEAR(g_last_children[0].x, 0.25, 1e-6);
    CHECK_NEAR(g_last_children[0].y, 0.25, 1e-6);

    t = norm_touch(ISWS_TOUCH_DOWN, 1, 0.75f, 0.75f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    t.x = 810.0f; t.y = 1800.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("down1");

    t = norm_touch(ISWS_TOUCH_MOVE, 0, 0.3f, 0.3f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    t.x = 324.0f; t.y = 720.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("move0-again");

    t = norm_touch(ISWS_TOUCH_UP, 1, 0.75f, 0.75f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 312);
    t.x = 810.0f; t.y = 1800.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("up1");

    t = norm_touch(ISWS_TOUCH_UP, 0, 0.3f, 0.3f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    t.x = 324.0f; t.y = 720.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("up0");

    t = norm_touch(ISWS_TOUCH_DOWN, 2, 0.5f, 0.5f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    t.x = 540.0f; t.y = 1200.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("down2");

    t = norm_touch(ISWS_TOUCH_CANCEL, 2, 0.5f, 0.5f);
    r = hidevent_touch(&t, out, sizeof(out));
    CHECK(r == 208);
    t.x = 540.0f; t.y = 1200.0f;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    check_tables_agree("cancel2");
    CHECK(g_created_count == g_released_count);

    /* Known divergence, pinned (F12): an identical re-DOWN is
     * suppressed by hidevent (returns 0) and emitted by touch.c.
     * Neither behaviour is changed; the difference is documented. */
    hidevent_reset();
    touch_test_reset();
    reset_mocks();
    setup_touch();
    t = norm_touch(ISWS_TOUCH_DOWN, 0, 0.5f, 0.5f);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 208);
    CHECK(hidevent_touch(&t, out, sizeof(out)) == 0);
    t.x = 540.0f; t.y = 1200.0f;
    int before = g_internal_receive_count;
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(touch_test_process_touch(&t, 1080, 2400) == 0);
    CHECK(g_internal_receive_count == before + 2);
}

/* ------------------------------------------------------------------ */
/* Fuzz                                                               */
/* ------------------------------------------------------------------ */

static void test_fuzz_wire(void) {
    present_test_reset_rx();
    uint64_t invalid_before = present_touch_invalid_dropped();
    for (int i = 0; i < 10000; i++) {
        uint8_t buf[ISWS_HEADER_SIZE + ISWS_TOUCH_PAYLOAD_SIZE];
        /* Random bytes, then a well-formed header around them. */
        for (size_t j = 0; j < sizeof(buf); j++) {
            buf[j] = (uint8_t)rng_u32();
        }
        if ((i % 2) == 0) {
            isws_encode_header(buf, ISWS_MSG_TOUCH, 0, ISWS_TOUCH_PAYLOAD_SIZE);
        }
        int rc = present_test_feed(buf, sizeof(buf));
        if (rc != 0) present_test_reset_rx();
        /* Drain anything that queued, so the queue never fills and
         * the fuzz stays about parsing, not queueing. */
        present_event_t ev;
        while (present_poll_event(&ev) == 1) {}
    }
    CHECK(present_touch_invalid_dropped() >= invalid_before);
    present_test_reset_rx();
}

static void test_fuzz_mapper(void) {
    ti_mapper_t mapper;
    isws_info_t map;
    make_map_full(&map);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    reset_emit();
    for (int i = 0; i < 10000; i++) {
        ti_motion_t m;
        memset(&m, 0, sizeof(m));
        m.masked_action = rng_u32() % 5;
        m.pointer_index = rng_u32() % 3;
        m.pointer_count = 1 + (rng_u32() % 3);
        m.timestamp_ns = (int64_t)(rng_u32() % 100000000);
        for (uint32_t p = 0; p < m.pointer_count; p++) {
            m.pointers[p].android_id = rng_u32() % 12;
            m.pointers[p].x = (float)(rng_u32() % 2000) - 200.0f;
            m.pointers[p].y = (float)(rng_u32() % 4000) - 200.0f;
            m.pointers[p].pressure = 1.0f;
        }
        m.history_count = rng_u32() % 4;
        for (uint32_t h = 0; h < m.history_count; h++) {
            for (uint32_t p = 0; p < m.pointer_count; p++) {
                m.hist_x[h][p] = (float)(rng_u32() % 2000) - 200.0f;
                m.hist_y[h][p] = (float)(rng_u32() % 4000) - 200.0f;
            }
        }
        uint32_t n = ti_mapper_feed(&mapper, &m, collect_emit, NULL);
        CHECK(n <= 40);
        CHECK(ti_mapper_active_count(&mapper) <= ISWS_MAX_POINTERS);
        reset_emit();
    }
}

static void test_fuzz_hidevent(void) {
    uint8_t out[1144];
    hidevent_reset();
    for (int i = 0; i < 10000; i++) {
        isws_touch_t t;
        t.action = rng_u32() % 6;
        t.pointer_id = rng_u32() % 12;
        t.pointer_count = rng_u32() % 12;
        uint32_t xb = rng_u32();
        uint32_t yb = rng_u32();
        memcpy(&t.x, &xb, sizeof(t.x));
        memcpy(&t.y, &yb, sizeof(t.y));
        t.timestamp_ns = 0;
        int r = hidevent_touch(&t, out, sizeof(out));
        CHECK(r == -1 || r == 0 || (r % 104) == 0);
        CHECK(hidevent_fingers_down() <= ISWS_MAX_POINTERS);
        if ((i % 997) == 0) hidevent_reset();
    }
    hidevent_reset();
}

/* ------------------------------------------------------------------ */
/* Dest-rect agreement, device-log replay, pump-through */
/* ------------------------------------------------------------------ */

/* The frame blit and the touch mapper must agree on geometry by
 * construction, not by parallel float math: both derive from the
 * integer destination rect of the same isws_info_t. The old
 * isws_screen_to_client divided by the float scale and subtracted
 * the float offset, so for a mapping whose offset is fractional the
 * two disagreed by up to one screen pixel at the frame edge: a tap
 * on a black-bar pixel just outside the dest rect mapped inside
 * (phantom app-edge tap), and a tap on the last frame pixel column
 * mapped outside (dropped DOWN). The device mapping is exact-fit
 * (offsets 0), so this did not fire in round 1, but it is the same
 * class of bug the screenshot's letterbox bars would have exposed
 * on any aspect mismatch. These cases pin the boundary exactly. */
static void test_dest_rect_touch_agreement(void) {
    isws_info_t m;
    int32_t x0, y0, x1, y1;
    float cx, cy, sx, sy;

    /* Fractional-offset mapping: 1000x3000 client on a 1440x3200
     * screen. scale = 3200/3000 = 1.0667, offset_x = 186.67, so the
     * dest rect starts at x0 = 187 (rounded) while the float offset
     * is 186.67. */
    CHECK(isws_compute_mapping(1440, 3200, 1000, 3000, 0, &m) == 0);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 187);
    CHECK(y0 == 0);
    CHECK(x1 == 1254);
    CHECK(y1 == 3200);

    /* A tap on the bar pixel at 186.7 (left of the dest rect) is
     * outside, clamped to the left edge. The old float math mapped
     * it inside to client x 0.03. */
    CHECK(isws_screen_to_client(&m, 186.7f, 100.0f, &cx, &cy) == -1);
    CHECK_NEAR(cx, 0.0, 1e-3);
    /* A tap on the last dest pixel column (1253.9) is inside, near
     * the right edge. The old float math mapped it outside
     * (client x 1000.5) and dropped the DOWN. */
    CHECK(isws_screen_to_client(&m, 1253.9f, 100.0f, &cx, &cy) == 0);
    /* Inside the last frame column; clamped to the last client
     * pixel (client_w - 1), as before. */
    CHECK_NEAR(cx, 999.0, 1e-3);
    /* First dest pixel maps to client 0 exactly. */
    CHECK(isws_screen_to_client(&m, 187.0f, 100.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 0.0, 1e-3);
    /* Round trip through the same rect. */
    isws_client_to_screen(&m, 999.0f, 100.0f, &sx, &sy);
    CHECK(isws_screen_to_client(&m, sx, sy, &cx, &cy) == 0);
    CHECK_NEAR(cx, 999.0, 1e-3);
    CHECK_NEAR(cy, 100.0, 1e-3);

    /* The mapper enforces the same boundary: DOWN on the bar pixel
     * is dropped, DOWN on the last frame column is delivered. */
    {
        ti_mapper_t mapper;
        ti_mapper_init(&mapper);
        ti_mapper_set_map(&mapper, &m, collect_emit, NULL);
        reset_emit();
        ti_motion_t down_bar = motion_down(0, 186.7f, 100.0f, 1000);
        CHECK(ti_mapper_feed(&mapper, &down_bar, collect_emit, NULL) == 0);
        CHECK(g_emitted_n == 0);
        CHECK(mapper.stats.dropped_down_outside == 1);
        ti_motion_t down_edge = motion_down(0, 1253.9f, 100.0f, 2000);
        CHECK(ti_mapper_feed(&mapper, &down_edge, collect_emit, NULL) == 1);
        CHECK(g_emitted_n == 1);
        CHECK(g_emitted[0].action == ISWS_TOUCH_DOWN);
        CHECK_NEAR(g_emitted[0].x, 999.0, 1e-3);
    }

    /* Exact-fit device geometry: 1440x3200 on 1440x3200, no bars,
     * dest rect is the whole screen and the mapping is the identity. */
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &m) == 0);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == 1440 && y1 == 3200);
    CHECK(isws_screen_to_client(&m, 0.0f, 0.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 0.0, 1e-3);
    CHECK(isws_screen_to_client(&m, 1439.0f, 3199.0f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 1439.0, 1e-3);
    CHECK_NEAR(cy, 3199.0, 1e-3);
    /* The last sub-pixel column clamps to client_w - 1, as before. */
    CHECK(isws_screen_to_client(&m, 1439.9f, 3199.9f, &cx, &cy) == 0);
    CHECK_NEAR(cx, 1439.0, 1e-3);
    CHECK_NEAR(cy, 3199.0, 1e-3);

    /* Integral bars (letterbox case): 1080x1920 on 1440x3200,
     * offset_y = 320 exactly. The bar/frame boundary is exact in
     * both the old and the new math; pin it so a regression shows. */
    CHECK(isws_compute_mapping(1440, 3200, 1080, 1920, 0, &m) == 0);
    isws_dest_rect(&m, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 320 && x1 == 1440 && y1 == 2880);
    CHECK(isws_screen_to_client(&m, 720.0f, 319.5f, &cx, &cy) == -1);
    CHECK(isws_screen_to_client(&m, 720.0f, 320.0f, &cx, &cy) == 0);
    CHECK_NEAR(cy, 0.0, 1e-3);
}


/* Device-log coordinate replay: every pairable (x, y, nx, ny)
 * from touch-019-selftest.txt (61 pairs) and
 * touch-019-interactive.txt (24 pairs). The wire client pixels are
 * reconstructed from the hid-built milli-units for the 1440x3200
 * client (wire_x = nx * 1440 / 1000, wire_y = ny * 3200 / 1000);
 * touch_map_coords must reproduce the logged window point (x, y)
 * and the logged milli-units exactly: nx = x/360*1000 and ny = y/800*1000
 * within the rounding of the two log lines. All 85 pairs pass on the
 * original code; any future presenter change that shifts the mapping by
 * more than half a milli-unit fails here. */
static const struct { int x, y, nx, ny; } g_device_pairs[] = {
    { 77, 787, 213, 984 }, /* down id=0 */
    { 77, 787, 213, 984 }, /* up id=0 */
    { 101, 774, 280, 968 }, /* down id=0 */
    { 101, 774, 280, 968 }, /* up id=0 */
    { 100, 776, 279, 970 }, /* down id=0 */
    { 100, 776, 279, 970 }, /* up id=0 */
    { 304, 773, 843, 967 }, /* down id=0 */
    { 304, 773, 843, 967 }, /* up id=0 */
    { 87, 757, 241, 946 }, /* down id=0 */
    { 87, 757, 241, 946 }, /* up id=0 */
    { 48, 763, 134, 954 }, /* down id=0 */
    { 48, 763, 134, 954 }, /* up id=0 */
    { 273, 761, 758, 951 }, /* down id=0 */
    { 273, 761, 758, 951 }, /* up id=0 */
    { 60, 753, 165, 942 }, /* down id=0 */
    { 60, 753, 165, 942 }, /* up id=0 */
    { 292, 771, 812, 964 }, /* down id=0 */
    { 185, 738, 514, 923 }, /* down id=1 */
    { 292, 771, 812, 964 }, /* up id=0 */
    { 185, 742, 514, 927 }, /* move id=1 */
    { 185, 747, 515, 934 }, /* up id=1 */
    { 65, 756, 181, 945 }, /* down id=0 */
    { 65, 756, 181, 945 }, /* up id=0 */
    { 193, 754, 536, 943 }, /* down id=0 */
    { 193, 754, 536, 943 }, /* up id=0 */
    { 85, 747, 237, 934 }, /* down id=0 */
    { 85, 747, 237, 934 }, /* up id=0 */
    { 290, 783, 804, 979 }, /* down id=0 */
    { 290, 783, 804, 979 }, /* up id=0 */
    { 77, 756, 215, 945 }, /* down id=0 */
    { 77, 756, 215, 945 }, /* up id=0 */
    { 59, 748, 163, 935 }, /* down id=0 */
    { 59, 748, 163, 935 }, /* up id=0 */
    { 309, 788, 858, 985 }, /* down id=0 */
    { 176, 749, 488, 936 }, /* down id=1 */
    { 176, 749, 488, 936 }, /* up id=1 */
    { 309, 789, 858, 986 }, /* move id=0 */
    { 309, 791, 858, 989 }, /* up id=0 */
    { 100, 750, 278, 938 }, /* down id=0 */
    { 100, 750, 278, 938 }, /* up id=0 */
    { 318, 782, 884, 978 }, /* down id=0 */
    { 207, 754, 574, 942 }, /* down id=1 */
    { 207, 754, 574, 942 }, /* up id=1 */
    { 127, 749, 352, 937 }, /* down id=1 */
    { 318, 782, 884, 978 }, /* up id=0 */
    { 127, 749, 352, 937 }, /* up id=1 */
    { 341, 781, 947, 977 }, /* down id=0 */
    { 341, 781, 947, 977 }, /* up id=0 */
    { 100, 762, 279, 952 }, /* down id=0 */
    { 100, 762, 279, 952 }, /* up id=0 */
    { 336, 795, 933, 994 }, /* down id=0 */
    { 336, 795, 933, 994 }, /* up id=0 */
    { 110, 756, 306, 946 }, /* down id=0 */
    { 110, 756, 306, 946 }, /* up id=0 */
    { 221, 794, 615, 992 }, /* down id=0 */
    { 221, 794, 615, 992 }, /* up id=0 */
    { 119, 790, 331, 988 }, /* down id=0 */
    { 119, 790, 331, 988 }, /* up id=0 */
    { 334, 798, 929, 997 }, /* down id=0 */
    { 334, 798, 929, 997 }, /* up id=0 */
    { 145, 776, 402, 970 }, /* down id=0 */
    { 107, 791, 297, 989 }, /* down id=0 */
    { 107, 791, 297, 989 }, /* up id=0 */
    { 94, 770, 261, 963 }, /* down id=0 */
    { 94, 770, 261, 963 }, /* up id=0 */
    { 79, 757, 221, 946 }, /* down id=0 */
    { 80, 757, 221, 946 }, /* move id=0 */
    { 84, 758, 234, 947 }, /* up id=0 */
    { 342, 27, 950, 34 }, /* down id=0 */
    { 342, 27, 950, 34 }, /* up id=0 */
    { 335, 22, 930, 28 }, /* down id=0 */
    { 335, 22, 930, 28 }, /* up id=0 */
    { 247, 118, 687, 148 }, /* down id=0 */
    { 247, 120, 687, 150 }, /* move id=0 */
    { 247, 131, 686, 164 }, /* up id=0 */
    { 248, 121, 688, 152 }, /* down id=0 */
    { 248, 121, 688, 152 }, /* up id=0 */
    { 252, 115, 699, 143 }, /* down id=0 */
    { 252, 115, 699, 143 }, /* up id=0 */
    { 256, 114, 712, 142 }, /* down id=0 */
    { 256, 114, 712, 142 }, /* up id=0 */
    { 254, 119, 705, 149 }, /* down id=0 */
    { 254, 120, 705, 150 }, /* move id=0 */
    { 242, 171, 673, 214 }, /* move id=0 */
    { 223, 230, 621, 288 }, /* up id=0 */
};

static void test_device_log_coordinates(void) {
    size_t i;
    CHECK(sizeof(g_device_pairs) / sizeof(g_device_pairs[0]) == 85);
    for (i = 0; i < sizeof(g_device_pairs) / sizeof(g_device_pairs[0]); i++) {
        /* The two log lines are roundings of one normalized
         * coordinate: the base line rounds norm * 360 (or 800) and
         * the hid-built line rounds norm * 1000. The pair is
         * consistent when the two implied norm intervals overlap.
         * A wire value at the overlap midpoint must then reproduce
         * both printed lines through touch_map_coords: that routes
         * the exact device pair through the real function instead
         * of rebuilding a wire value from one rounded line alone
         * (nx * 1440 / 1000 alone can land outside the window-point
         * interval by half a milli-unit). */
        double lo_x, hi_x, lo_y, hi_y, mid_x, mid_y;
        isws_touch_t t;
        double norm_x, norm_y, win_x, win_y;
        int rx, ry, rnx, rny;
        lo_x = (g_device_pairs[i].x - 0.5) / 360.0;
        if ((g_device_pairs[i].nx - 0.5) / 1000.0 > lo_x)
            lo_x = (g_device_pairs[i].nx - 0.5) / 1000.0;
        hi_x = (g_device_pairs[i].x + 0.5) / 360.0;
        if ((g_device_pairs[i].nx + 0.5) / 1000.0 < hi_x)
            hi_x = (g_device_pairs[i].nx + 0.5) / 1000.0;
        lo_y = (g_device_pairs[i].y - 0.5) / 800.0;
        if ((g_device_pairs[i].ny - 0.5) / 1000.0 > lo_y)
            lo_y = (g_device_pairs[i].ny - 0.5) / 1000.0;
        hi_y = (g_device_pairs[i].y + 0.5) / 800.0;
        if ((g_device_pairs[i].ny + 0.5) / 1000.0 < hi_y)
            hi_y = (g_device_pairs[i].ny + 0.5) / 1000.0;
        if (!(lo_x < hi_x && lo_y < hi_y)) {
            g_checks++;
            g_failures++;
            printf("FAIL device pair %zu: log (%d,%d,%d,%d) has no "
                   "common normalized coordinate\n",
                   i, g_device_pairs[i].x, g_device_pairs[i].y,
                   g_device_pairs[i].nx, g_device_pairs[i].ny);
            continue;
        }
        g_checks++;
        mid_x = (lo_x + hi_x) * 0.5;
        mid_y = (lo_y + hi_y) * 0.5;
        t.action = ISWS_TOUCH_DOWN;
        t.pointer_id = 0;
        t.pointer_count = 1;
        t.x = (float)(mid_x * 1440.0);
        t.y = (float)(mid_y * 3200.0);
        t.timestamp_ns = 1000000 + (int64_t)i;
        CHECK(touch_map_coords(&t, 1440, 3200, 360.0, 800.0,
                               &norm_x, &norm_y, &win_x, &win_y) == 0);
        rx = (int)(win_x + 0.5);
        ry = (int)(win_y + 0.5);
        rnx = (int)(norm_x * 1000.0 + 0.5);
        rny = (int)(norm_y * 1000.0 + 0.5);
        if (rx != g_device_pairs[i].x || ry != g_device_pairs[i].y ||
            rnx != g_device_pairs[i].nx || rny != g_device_pairs[i].ny) {
            g_checks++;
            g_failures++;
            printf("FAIL device pair %zu: log (%d,%d,%d,%d) replayed "
                   "(%d,%d,%d,%d)\n",
                   i, g_device_pairs[i].x, g_device_pairs[i].y,
                   g_device_pairs[i].nx, g_device_pairs[i].ny,
                   rx, ry, rnx, rny);
        } else {
            g_checks++;
        }
    }
}

/* Device DOWN/UP replay tables, in log order, one per device run.
 * The two runs have separate CLOCK_MONOTONIC epochs (the
 * interactive run's timestamps are smaller than the selftest run's),
 * so each table is replayed as its own run with fresh reader state:
 * the F8 stale guard would otherwise drop the second run's taps. */
typedef struct {
    uint32_t action;
    uint32_t pid; /* Android pointer id */
    int nx, ny;   /* hid-built milli-units */
    int64_t ts;   /* wire timestamp, CLOCK_MONOTONIC ns */
} device_tap_t;

static const device_tap_t g_device_taps_selftest[] = {
    { ISWS_TOUCH_DOWN, 0, 213, 984, 35255469114101LL },
    { ISWS_TOUCH_UP, 0, 213, 984, 35255505986289LL },
    { ISWS_TOUCH_DOWN, 0, 280, 968, 35255628932382LL },
    { ISWS_TOUCH_UP, 0, 280, 968, 35255687761445LL },
    { ISWS_TOUCH_DOWN, 0, 279, 970, 35255779263007LL },
    { ISWS_TOUCH_UP, 0, 279, 970, 35255840822070LL },
    { ISWS_TOUCH_DOWN, 0, 843, 967, 35256025003684LL },
    { ISWS_TOUCH_UP, 0, 843, 967, 35256107684726LL },
    { ISWS_TOUCH_DOWN, 0, 241, 946, 35256121554205LL },
    { ISWS_TOUCH_UP, 0, 241, 946, 35256168255559LL },
    { ISWS_TOUCH_DOWN, 0, 134, 954, 35256301589257LL },
    { ISWS_TOUCH_UP, 0, 134, 954, 35256335472642LL },
    { ISWS_TOUCH_DOWN, 0, 758, 951, 35256350859726LL },
    { ISWS_TOUCH_UP, 0, 758, 951, 35256429932747LL },
    { ISWS_TOUCH_DOWN, 0, 165, 942, 35256470563944LL },
    { ISWS_TOUCH_UP, 0, 165, 942, 35256496974986LL },
    { ISWS_TOUCH_DOWN, 0, 812, 964, 35256541299674LL },
    { ISWS_TOUCH_DOWN, 1, 514, 923, 35256556599674LL },
    { ISWS_TOUCH_UP, 0, 812, 964, 35256584150455LL },
    { ISWS_TOUCH_UP, 1, 515, 934, 35256622553319LL },
    { ISWS_TOUCH_DOWN, 0, 181, 945, 35256628621861LL },
    { ISWS_TOUCH_UP, 0, 181, 945, 35256662651028LL },
    { ISWS_TOUCH_DOWN, 0, 536, 943, 35256713169205LL },
    { ISWS_TOUCH_UP, 0, 536, 943, 35256733891601LL },
    { ISWS_TOUCH_DOWN, 0, 237, 934, 35256763307278LL },
    { ISWS_TOUCH_UP, 0, 237, 934, 35256816200924LL },
    { ISWS_TOUCH_DOWN, 0, 804, 979, 35256864509517LL },
    { ISWS_TOUCH_UP, 0, 804, 979, 35256913347434LL },
    { ISWS_TOUCH_DOWN, 0, 215, 945, 35256939873528LL },
    { ISWS_TOUCH_UP, 0, 215, 945, 35256974174101LL },
    { ISWS_TOUCH_DOWN, 0, 163, 935, 35257089384621LL },
    { ISWS_TOUCH_UP, 0, 163, 935, 35257109874361LL },
    { ISWS_TOUCH_DOWN, 0, 858, 985, 35257286854100LL },
    { ISWS_TOUCH_DOWN, 1, 488, 936, 35257300514673LL },
    { ISWS_TOUCH_UP, 1, 488, 936, 35257357446444LL },
    { ISWS_TOUCH_UP, 0, 858, 989, 35257388883319LL },
    { ISWS_TOUCH_DOWN, 0, 278, 938, 35257396614882LL },
    { ISWS_TOUCH_UP, 0, 278, 938, 35257439202434LL },
    { ISWS_TOUCH_DOWN, 0, 884, 978, 35257439406288LL },
    { ISWS_TOUCH_DOWN, 1, 574, 942, 35257460305142LL },
    { ISWS_TOUCH_UP, 1, 574, 942, 35257494578163LL },
    { ISWS_TOUCH_DOWN, 1, 352, 937, 35257526174361LL },
    { ISWS_TOUCH_UP, 0, 884, 978, 35257537612434LL },
    { ISWS_TOUCH_UP, 1, 352, 937, 35257576649413LL },
    { ISWS_TOUCH_DOWN, 0, 947, 977, 35257638776652LL },
    { ISWS_TOUCH_UP, 0, 947, 977, 35257645551913LL },
    { ISWS_TOUCH_DOWN, 0, 279, 952, 35257683705767LL },
    { ISWS_TOUCH_UP, 0, 279, 952, 35257716367069LL },
    { ISWS_TOUCH_DOWN, 0, 933, 994, 35257789496809LL },
    { ISWS_TOUCH_UP, 0, 933, 994, 35257796593631LL },
    { ISWS_TOUCH_DOWN, 0, 306, 946, 35257836333684LL },
    { ISWS_TOUCH_UP, 0, 306, 946, 35257877571444LL },
    { ISWS_TOUCH_DOWN, 0, 615, 992, 35257923106184LL },
    { ISWS_TOUCH_UP, 0, 615, 992, 35257941610038LL },
    { ISWS_TOUCH_DOWN, 0, 331, 988, 35258000351913LL },
    { ISWS_TOUCH_UP, 0, 331, 988, 35258034793579LL },
    { ISWS_TOUCH_DOWN, 0, 929, 997, 35258100039777LL },
    { ISWS_TOUCH_UP, 0, 929, 997, 35258106269569LL },
    /* The log ends mid-tap here: this DOWN has no UP line
     * recorded, so it is not replayed (it stays in the
     * coordinate-pairs table above). */
};
static const device_tap_t g_device_taps_interactive[] = {
    { ISWS_TOUCH_DOWN, 0, 297, 989, 0LL },
    { ISWS_TOUCH_UP, 0, 297, 989, 0LL },
    { ISWS_TOUCH_DOWN, 0, 261, 963, 35209369453337LL },
    { ISWS_TOUCH_UP, 0, 261, 963, 35209464036514LL },
    { ISWS_TOUCH_DOWN, 0, 221, 946, 35209887541358LL },
    { ISWS_TOUCH_UP, 0, 234, 947, 35209974639379LL },
    { ISWS_TOUCH_DOWN, 0, 950, 34, 35210708261774LL },
    { ISWS_TOUCH_UP, 0, 950, 34, 35210812988806LL },
    { ISWS_TOUCH_DOWN, 0, 930, 28, 35211213301722LL },
    { ISWS_TOUCH_UP, 0, 930, 28, 35211324209639LL },
    { ISWS_TOUCH_DOWN, 0, 687, 148, 35211503944222LL },
    { ISWS_TOUCH_UP, 0, 686, 164, 35211576780941LL },
    { ISWS_TOUCH_DOWN, 0, 688, 152, 35211711447087LL },
    { ISWS_TOUCH_UP, 0, 688, 152, 35211797267972LL },
    { ISWS_TOUCH_DOWN, 0, 699, 143, 35211873960420LL },
    { ISWS_TOUCH_UP, 0, 699, 143, 35211976999170LL },
    { ISWS_TOUCH_DOWN, 0, 712, 142, 35212064473284LL },
    { ISWS_TOUCH_UP, 0, 712, 142, 35212196640368LL },
    { ISWS_TOUCH_DOWN, 0, 705, 149, 35212254527034LL },
    { ISWS_TOUCH_UP, 0, 621, 288, 35212389825836LL },
};

/* Pump-through: presenter DOWN -> wire payload -> LSK
 * touch.c, replaying the exact device DOWN/UP taps. The Android
 * screen pixels are reconstructed from the hid-built milli-units
 * for the exact-fit device mapping (screen 1440x3200, client
 * 1440x3200, scale 1.0): android_px = nx * 1440 / 1000,
 * ny * 3200 / 1000. The mapper emits one wire TOUCH per tap, the
 * wire bytes go through the real present.c feed path, and the mock
 * HID child for the emitted slot must carry the logged normalized
 * coordinates. End state per run: no finger left down, zero drops
 * of every kind, and every tap reached sendEvent: (the mock drain
 * calls it once per injected event). On the device this path
 * delivered every tap to touch_inject (85/85 pairs logged) while
 * sendevent stayed 0, which pins the round-1 failure to the LSK
 * side of _receiveHIDEvent:, not to the presenter: the presenter
 * drops nothing here. */
static void pump_device_taps(const device_tap_t *tab, size_t n) {
    ti_mapper_t mapper;
    isws_info_t map;
    size_t i;
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &map) == 0);
    ti_mapper_init(&mapper);
    ti_mapper_set_map(&mapper, &map, collect_emit, NULL);
    for (i = 0; i < n; i++) {
        ti_motion_t m;
        uint32_t slot;
        int before;
        memset(&m, 0, sizeof(m));
        m.masked_action = tab[i].action == ISWS_TOUCH_DOWN ? TI_ACTION_DOWN : TI_ACTION_UP;
        m.pointer_index = 0;
        m.pointer_count = 1;
        m.timestamp_ns = tab[i].ts;
        m.pointers[0].android_id = tab[i].pid;
        m.pointers[0].x = (float)(tab[i].nx * 1440.0 / 1000.0);
        m.pointers[0].y = (float)(tab[i].ny * 3200.0 / 1000.0);
        m.pointers[0].pressure = 1.0f;
        reset_emit();
        before = (int)touch_test_sendevent_count();
        CHECK(ti_mapper_feed(&mapper, &m, collect_emit, NULL) == 1);
        CHECK(g_emitted_n == 1);
        CHECK(g_emitted[0].action == tab[i].action);
        CHECK(g_emitted[0].timestamp_ns == tab[i].ts);
        slot = g_emitted[0].pointer_id;
        CHECK(slot < ISWS_MAX_POINTERS);
        feed_touch_and_handle(&g_emitted[0]);
        if (tab[i].action == ISWS_TOUCH_DOWN) {
            /* The mock HID child for the slot carries the logged
             * normalized coordinates. */
            CHECK_NEAR(g_last_children[slot].x, tab[i].nx / 1000.0, 1e-6);
            CHECK_NEAR(g_last_children[slot].y, tab[i].ny / 1000.0, 1e-6);
        }
        CHECK((int)touch_test_sendevent_count() == before + 1);
    }
    CHECK(touch_test_fingers_down() == 0);
    {
        uint64_t rf = 0, inv = 0, stale = 0, cancel = 0;
        touch_test_drop_counts(&rf, &inv, &stale, &cancel);
        CHECK(rf == 0);
        CHECK(inv == 0);
        CHECK(stale == 0);
        CHECK(cancel == 0);
    }
    CHECK(mapper.stats.dropped_down_outside == 0);
    CHECK(mapper.stats.dropped_no_slot == 0);
    CHECK(mapper.stats.dropped_unknown_up == 0);
    CHECK(mapper.stats.dropped_dup_move == 0);
    CHECK(mapper.stats.ignored_actions == 0);
}

static void test_device_pump_through(void) {
    isws_info_t info;
    uint8_t pay[ISWS_INFO_PAYLOAD_SIZE];
    present_event_t ev;

    /* Run 1: the selftest log's 58 taps (29 complete pairs). */
    present_test_reset_rx();
    setup_touch_with(1, 0);
    g_fake_touch_count = 1;
    g_fake_context_id = 0x12345u;
    g_drain_call_send_event = 1;
    CHECK(isws_compute_mapping(1440, 3200, 1440, 3200, 0, &info) == 0);
    isws_encode_info(pay, sizeof(pay), &info);
    feed_msg(ISWS_MSG_INFO, pay, sizeof(pay));
    while (present_poll_event(&ev) == 1) {
        touch_handle_present_event(&ev);
    }
    pump_device_taps(g_device_taps_selftest,
                     sizeof(g_device_taps_selftest) / sizeof(g_device_taps_selftest[0]));
    CHECK(touch_test_sendevent_count() == 58);
    present_test_reset_rx();

    /* Run 2: the interactive log's 20 taps (own epoch). */
    setup_touch_with(1, 0);
    g_fake_touch_count = 1;
    g_fake_context_id = 0x12345u;
    g_drain_call_send_event = 1;
    isws_encode_info(pay, sizeof(pay), &info);
    feed_msg(ISWS_MSG_INFO, pay, sizeof(pay));
    while (present_poll_event(&ev) == 1) {
        touch_handle_present_event(&ev);
    }
    pump_device_taps(g_device_taps_interactive,
                     sizeof(g_device_taps_interactive) / sizeof(g_device_taps_interactive[0]));
    CHECK(touch_test_sendevent_count() == 20);
    present_test_reset_rx();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_mapper_single_tap();
    test_mapper_letterbox();
    test_mapper_cancel_all_and_aliasing();
    test_mapper_no_slot();
    test_mapper_history_and_dedupe();
    test_mapper_move_unknown_starts_down();
    test_mapper_set_map_cancels();
    test_present_invalid_touch_does_not_wedge();
    test_present_queue_prefers_move_drop();
    test_reader_focus_cancels();
    test_reader_stale_timestamp();
    test_selftest_sendevent_count();
    test_move_log_sampling_and_detail();
    test_ring_full_and_invalid_counts();
    test_hidevent_touch_agreement();
    test_dest_rect_touch_agreement();
    test_device_log_coordinates();
    test_device_pump_through();
    test_fuzz_wire();
    test_fuzz_mapper();
    test_fuzz_hidevent();
    printf("touch_plumbing_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
