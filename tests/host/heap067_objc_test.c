/*
 * heap067_objc_test.c - Host test for ObjC runtime helpers.
 *
 * Drives the real libsystem_kernel/src/touch.c (TOUCH_HOST_TEST=1)
 * through touch_test_process_touch with a minimal fake ObjC runtime
 * that models REAL retain counts and a REAL autorelease pool, so the
 * object lifetime holes in the route C touch path are observable:
 *
 * Test 1 (the crash fix): the fallback NSSet dangle. In the fallback
 * path, +[NSSet setWithObject:] returns an autoreleased set which is
 * stored into the event's _touches ivar through object_setIvar, which
 * does not retain. touch_inject pops its pool after delivery, which
 * frees the set and leaves event._touches dangling. On base the set
 * is freed after the phase; with the fix (retain the set after the
 * ivar store) it stays alive. FAILS on base, PASSES with the fix.
 *
 * Test 2 (leak fix): the UITouch alloc/init +1 is never released.
 * After a DOWN/MOVE/UP cycle on the KIF path the touch must be held
 * only by the shared event's set (refcount 1), and a second DOWN
 * (whose _clearTouches drops the old touch from the set) must free
 * the first touch. On base the first touch stays alive (refcount 1
 * after the second DOWN, immortal). FAILS on base, PASSES with fix.
 *
 * Test 3 (leak fix): the KIF block CFRetains the parent HID event
 * twice per phase with no matching release. After a DOWN the parent
 * must be freed by touch_inject's single CFRelease, and no CFRetain
 * must have run on a HID event at all. On base the parent is leaked
 * (refcount 2) and CFRetain ran 6 times over a DOWN/MOVE/UP cycle.
 * FAILS on base, PASSES with the fix.
 *
 * The TUIKBGraphSerialization cache lifetime bug (the device crash
 * chain) lives inside Apple's TextInputUI, so
 * it has no host test here; the failing-then-passing test covers the
 * patchable hole in this tree, which has the same failure shape
 * (a borrowed pointer freed while a caller still holds it).
 *
 * NOTE on the mock objc_msgSend: it is declared with mixed GP/FP
 * params (void*, void*, void*, void*, double, double) so it can
 * observe the CGPoint struct and double args the direct path passes
 * through objc_msgSend. This works ONLY because touch.c lives in a
 * different translation unit from this file (no LTO in host_tests.sh):
 * touch.c classifies args per its cast types at the call site, and
 * this mock reads them back per its own prototype. Do not merge this
 * mock into touch.c's translation unit, and do not enable LTO for
 * this test.
 *
 * Build (mirrors the touch_test line in tools/host_tests.sh):
 *   cc -std=gnu11 -O1 -g -w -DPRESENT_TEST -DTOUCH_HOST_TEST=1 \
 *     -iquote libsystem_kernel/src \
 *     tests/host/heap067_objc_test.c libsystem_kernel/src/touch.c \
 *     libsystem_kernel/src/present.c \
 *     libsystem_kernel/src/protocol.c -lpthread
 * and the same with -fsanitize=address,undefined
 * -fno-sanitize-recover=all.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "protocol.h"
#include "touch.h"

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

/* ------------------------------------------------------------------ */
/* Fake ObjC runtime with real retain counts and a real pool          */
/* ------------------------------------------------------------------ */

enum {
    K_CLASS = 1,
    K_APP,
    K_WIN,
    K_FETCHER,
    K_TOUCH,
    K_EVENT,
    K_SET,
    K_HID
};

typedef struct fake_obj {
    void *isa;          /* class pointer */
    int refcount;
    int kind;
    int id;             /* unique per object, never reused; freed checks
                         * use the id, not the pointer, because malloc
                         * reuses a freed address for the next object */
    void *slot0;        /* event: _touches set; set: contained object;
                         * app: keyWindow; touch: window */
    void *slot1;        /* app: fetcher; touch: view */
    void *slot2;        /* app: shared kev (KIF) or NULL; touch: hid event */
    void *touches[8];   /* kev: touches currently in its set */
    int ntouches;
    double d0, d1;
    long l0;
    int i0;
} fake_obj_t;

/* Classes (static singletons; never freed). */
static fake_obj_t g_cls_touch;
static fake_obj_t g_cls_event;
static fake_obj_t g_cls_nsset;
static fake_obj_t g_cls_app;
static fake_obj_t g_cls_control;

/* UIKit singletons (static; never freed). The app's slot2 is the
 * shared UITouchesEvent the KIF path reuses, or NULL when KIF is
 * unavailable and the fallback path must run. */
static fake_obj_t g_app;
static fake_obj_t g_win;
static fake_obj_t g_fetcher;
static fake_obj_t g_kev;
static int g_kif_available = 0;

/* The _touches ivar: offset of slot0 in fake_obj_t. */
typedef struct fake_ivar {
    const char *name;
    long offset;
} fake_ivar_t;
static fake_ivar_t g_ivar_touches = { "_touches", (long)offsetof(fake_obj_t, slot0) };

/* Allocation registry: every malloc'd fake object, so teardown can
 * free what the test (or the code under test) left alive and the
 * ASan build reports no leak at exit. */
#define MAX_ALLOCS 1024
static void *g_allocs[MAX_ALLOCS];
static int g_allocs_n = 0;

/* Freed-pointer list: every pointer fake_release freed. Kept for
 * teardown bookkeeping only; the tests use freed IDs (below). */
#define MAX_FREED 1024
static void *g_freed[MAX_FREED];
static int g_freed_n = 0;

/* Freed IDs: the id of every object fake_release freed. IDs are
 * never reused, so this is immune to malloc address reuse, which
 * defeated the pointer-based check (a new touch landed on the freed
 * HID child's address and looked freed). */
#define MAX_FREED_IDS 1024
static int g_freed_ids[MAX_FREED_IDS];
static int g_freed_ids_n = 0;
static int g_next_id = 1;

static int is_freed_id(int id) {
    for (int i = 0; i < g_freed_ids_n; i++) {
        if (g_freed_ids[i] == id) return 1;
    }
    return 0;
}

static int g_overrelease = 0;   /* release on a refcount <= 0 object */

static void *fake_alloc(size_t n) {
    void *p = malloc(n);
    if (!p) { printf("FAIL: fake_alloc out of memory\n"); exit(2); }
    memset(p, 0, n);
    if (g_allocs_n < MAX_ALLOCS) g_allocs[g_allocs_n++] = p;
    return p;
}

static int is_static_obj(void *p) {
    return p == &g_app || p == &g_win || p == &g_fetcher || p == &g_kev ||
           p == &g_cls_touch || p == &g_cls_event || p == &g_cls_nsset ||
           p == &g_cls_app || p == &g_cls_control;
}

static void fake_retain_obj(fake_obj_t *o) {
    if (!o || is_static_obj(o)) return;
    o->refcount++;
}

static void fake_release_obj(fake_obj_t *o) {
    if (!o || is_static_obj(o)) return;
    if (o->refcount <= 0) { g_overrelease++; return; }
    o->refcount--;
    if (o->refcount == 0) {
        if (g_freed_n < MAX_FREED) g_freed[g_freed_n++] = o;
        if (g_freed_ids_n < MAX_FREED_IDS) g_freed_ids[g_freed_ids_n++] = o->id;
        free(o);
    }
}

/* Free every still-alive malloc'd fake object (teardown only, after
 * all assertions). Marks them freed so a later is_freed is honest. */
static void teardown_free_all(void) {
    for (int i = 0; i < g_allocs_n; i++) {
        void *p = g_allocs[i];
        int already = 0;
        for (int j = 0; j < g_freed_n; j++) {
            if (g_freed[j] == p) { already = 1; break; }
        }
        if (!already) {
            if (g_freed_n < MAX_FREED) g_freed[g_freed_n++] = p;
            free(p);
        }
    }
    g_allocs_n = 0;
}

/* ------------------------------------------------------------------ */
/* Autorelease pool (real: pop releases every object in it)           */
/* ------------------------------------------------------------------ */

typedef struct fake_pool {
    void *objs[128];
    int n;
} fake_pool_t;

static fake_pool_t *g_pool = NULL;

static void *fake_pool_push(void) {
    fake_pool_t *pool = (fake_pool_t *)fake_alloc(sizeof(fake_pool_t));
    pool->n = 0;
    g_pool = pool;
    return pool;
}

static void fake_pool_pop(void *pool) {
    fake_pool_t *p = (fake_pool_t *)pool;
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        fake_release_obj((fake_obj_t *)p->objs[i]);
    }
    p->n = 0;
    if (g_pool == p) g_pool = NULL;
    /* The pool itself is freed, not released (it has no refcount). */
    if (g_freed_n < MAX_FREED) g_freed[g_freed_n++] = p;
    free(p);
}

static void fake_autorelease(void *obj) {
    if (!obj || !g_pool) return;
    if (g_pool->n < 128) g_pool->objs[g_pool->n++] = obj;
}

/* ------------------------------------------------------------------ */
/* Fake object constructors                                           */
/* ------------------------------------------------------------------ */

/* Last object of each kind created, so the test can find it. The id
 * is captured at creation time, before the object can be freed. */
static void *g_last_touch = NULL;
static int g_last_touch_id = 0;
static void *g_last_event = NULL;
static int g_last_event_id = 0;
static void *g_last_set = NULL;
static int g_last_set_id = 0;
static void *g_last_parent = NULL;   /* last DigitizerEvent (parent) */
static int g_last_parent_id = 0;
static void *g_last_child = NULL;    /* last DigitizerFingerEvent */

static long g_cfretain_count = 0;
static long g_cfrelease_count = 0;
static long g_sendevent_calls = 0;
static void *g_last_send_event = NULL;

static fake_obj_t *fake_new_obj(int kind, void *isa) {
    fake_obj_t *o = (fake_obj_t *)fake_alloc(sizeof(fake_obj_t));
    o->isa = isa;
    o->refcount = 1;
    o->kind = kind;
    o->id = g_next_id++;
    return o;
}

static void *fake_new_touch(void) {
    fake_obj_t *o = fake_new_obj(K_TOUCH, &g_cls_touch);
    g_last_touch = o;
    g_last_touch_id = o->id;
    return o;
}

static void *fake_new_event(void) {
    fake_obj_t *o = fake_new_obj(K_EVENT, &g_cls_event);
    g_last_event = o;
    g_last_event_id = o->id;
    return o;
}

static void *fake_new_set(void *obj) {
    fake_obj_t *o = fake_new_obj(K_SET, &g_cls_nsset);
    o->slot0 = obj;
    fake_autorelease(o);
    g_last_set = o;
    g_last_set_id = o->id;
    return o;
}

static void *fake_new_hid(int is_parent) {
    fake_obj_t *o = fake_new_obj(K_HID, NULL);
    if (is_parent) {
        g_last_parent = o;
        g_last_parent_id = o->id;
    } else {
        g_last_child = o;
    }
    return o;
}

/* kev set bookkeeping: _clearTouches releases every touch in the
 * set, _addTouch:forDelayedDelivery: retains and stores it. */
static void fake_kev_clear(fake_obj_t *kev) {
    for (int i = 0; i < kev->ntouches; i++) {
        fake_release_obj((fake_obj_t *)kev->touches[i]);
        kev->touches[i] = NULL;
    }
    kev->ntouches = 0;
}

static void fake_kev_add(fake_obj_t *kev, void *touch) {
    if (!touch) return;
    fake_retain_obj((fake_obj_t *)touch);
    if (kev->ntouches < 8) kev->touches[kev->ntouches++] = touch;
}

/* ------------------------------------------------------------------ */
/* Selector interning (sel_registerName returns a stable pointer     */
/* per name, so pointer equality works; the pointer IS the name).     */
/* ------------------------------------------------------------------ */

#define MAX_SELS 128
static char g_sel_names[MAX_SELS][64];
static int g_sel_n = 0;

static void *fake_sel_registerName(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_sel_n; i++) {
        if (strcmp(g_sel_names[i], name) == 0) return g_sel_names[i];
    }
    if (g_sel_n >= MAX_SELS) return NULL;
    size_t len = strlen(name);
    if (len >= sizeof(g_sel_names[0])) len = sizeof(g_sel_names[0]) - 1;
    memcpy(g_sel_names[g_sel_n], name, len);
    g_sel_names[g_sel_n][len] = '\0';
    return g_sel_names[g_sel_n++];
}

static void *fake_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "UITouch") == 0) return &g_cls_touch;
    if (strcmp(name, "UITouchesEvent") == 0) return &g_cls_event;
    if (strcmp(name, "NSSet") == 0) return &g_cls_nsset;
    if (strcmp(name, "UIApplication") == 0) return &g_cls_app;
    if (strcmp(name, "UIControl") == 0) return &g_cls_control;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* respondsToSelector: which selectors each fake kind answers.        */
/* ------------------------------------------------------------------ */

static int fake_responds(fake_obj_t *o, const char *t) {
    if (!o || !t) return 0;
    if (o->kind == K_CLASS) {
        if (o == &g_cls_touch && strcmp(t, "alloc") == 0) return 1;
        if (o == &g_cls_event && strcmp(t, "alloc") == 0) return 1;
        if (o == &g_cls_nsset && strcmp(t, "setWithObject:") == 0) return 1;
        if (o == &g_cls_app && strcmp(t, "sharedApplication") == 0) return 1;
        return 0;
    }
    if (strcmp(t, "retain") == 0) return 1;
    if (strcmp(t, "release") == 0) return 1;
    if (strcmp(t, "init") == 0) return 1;
    switch (o->kind) {
    case K_APP:
        if (strcmp(t, "keyWindow") == 0) return 1;
        if (strcmp(t, "_eventFetcher") == 0) return 1;
        if (strcmp(t, "_touchesEvent") == 0) return g_kif_available;
        if (strcmp(t, "sendEvent:") == 0) return 1;
        return 0;
    case K_WIN:
    case K_FETCHER:
    case K_HID:
        return 0;
    case K_TOUCH:
        if (strcmp(t, "setWindow:") == 0) return 1;
        if (strcmp(t, "setView:") == 0) return 1;
        if (strcmp(t, "setPhase:") == 0) return 1;
        if (strcmp(t, "setTimestamp:") == 0) return 1;
        if (strcmp(t, "setTapCount:") == 0) return 1;
        if (strcmp(t, "_setLocationInWindow:resetPrevious:") == 0) return 1;
        if (strcmp(t, "_setHidEvent:") == 0) return 1;
        if (strcmp(t, "_setIsFirstTouchForView:") == 0) return 1;
        if (strcmp(t, "_setInitialTouchTimestamp:") == 0) return 1;
        return 0;
    case K_EVENT:
        if (strcmp(t, "_clearTouches") == 0) return 1;
        if (strcmp(t, "_addTouch:forDelayedDelivery:") == 0) return 1;
        if (strcmp(t, "_setTouches:") == 0) return 1;
        if (strcmp(t, "setTouches:") == 0) return 1;
        if (strcmp(t, "setEventTimestamp:") == 0) return 1;
        /* The kev deliberately does NOT respond to _setHidEvent:,
         * so the KIF block never retains the HID event through the
         * event and test 3 measures only the CFRetain leak. */
        return 0;
    case K_SET:
        if (strcmp(t, "count") == 0) return 1;
        return 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* objc_msgSend. Mixed GP/FP prototype, see the header note: this     */
/* works only because touch.c is a separate translation unit.         */
/* ------------------------------------------------------------------ */

static void *fake_objc_msgSend(void *obj, void *sel, void *a1, void *a2,
                               double f1, double f2) {
    const char *sname = (const char *)sel;
    fake_obj_t *o = (fake_obj_t *)obj;
    (void)a2;
    if (!o || !sname) return NULL;

    if (strcmp(sname, "respondsToSelector:") == 0) {
        return (void *)(uintptr_t)fake_responds(o, (const char *)a1);
    }

    if (o->kind == K_CLASS) {
        if (o == &g_cls_touch && strcmp(sname, "alloc") == 0) {
            return fake_new_touch();
        }
        if (o == &g_cls_event && strcmp(sname, "alloc") == 0) {
            return fake_new_event();
        }
        if (o == &g_cls_nsset && strcmp(sname, "setWithObject:") == 0) {
            return fake_new_set(a1);
        }
        if (o == &g_cls_app && strcmp(sname, "sharedApplication") == 0) {
            return &g_app;
        }
        return NULL;
    }

    if (strcmp(sname, "retain") == 0) {
        fake_retain_obj(o);
        return o;
    }
    if (strcmp(sname, "release") == 0) {
        fake_release_obj(o);
        return NULL;
    }
    if (strcmp(sname, "init") == 0) return o;

    switch (o->kind) {
    case K_APP:
        if (strcmp(sname, "keyWindow") == 0) return o->slot0;
        if (strcmp(sname, "_eventFetcher") == 0) return o->slot1;
        if (strcmp(sname, "_touchesEvent") == 0) return o->slot2;
        if (strcmp(sname, "sendEvent:") == 0) {
            g_sendevent_calls++;
            g_last_send_event = a1;
            return NULL;
        }
        return NULL;
    case K_WIN:
    case K_FETCHER:
    case K_HID:
        return NULL;
    case K_TOUCH:
        /* Setters store without retaining. _setHidEvent: in real
         * UIKit retains, but this mock stores without retaining so
         * test 3 measures only the CFRetain leak, not the property. */
        if (strcmp(sname, "setWindow:") == 0) { o->slot0 = a1; return NULL; }
        if (strcmp(sname, "setView:") == 0) { o->slot1 = a1; return NULL; }
        if (strcmp(sname, "setPhase:") == 0) {
            o->l0 = (long)(uintptr_t)a1;
            return NULL;
        }
        if (strcmp(sname, "setTimestamp:") == 0) { o->d0 = f1; return NULL; }
        if (strcmp(sname, "setTapCount:") == 0) {
            o->l0 = (long)(uintptr_t)a1;
            return NULL;
        }
        if (strcmp(sname, "_setLocationInWindow:resetPrevious:") == 0) {
            o->d0 = f1;
            o->d1 = f2;
            return NULL;
        }
        if (strcmp(sname, "_setHidEvent:") == 0) { o->slot2 = a1; return NULL; }
        if (strcmp(sname, "_setIsFirstTouchForView:") == 0) {
            o->i0 = (int)(uintptr_t)a1;
            return NULL;
        }
        if (strcmp(sname, "_setInitialTouchTimestamp:") == 0) {
            o->d0 = f1;
            return NULL;
        }
        return NULL;
    case K_EVENT:
        if (strcmp(sname, "_clearTouches") == 0) {
            fake_kev_clear(o);
            return NULL;
        }
        if (strcmp(sname, "_addTouch:forDelayedDelivery:") == 0) {
            fake_kev_add(o, a1);
            return NULL;
        }
        if (strcmp(sname, "_setTouches:") == 0 ||
            strcmp(sname, "setTouches:") == 0) {
            o->slot0 = a1;
            return NULL;
        }
        if (strcmp(sname, "setEventTimestamp:") == 0) {
            o->d0 = f1;
            return NULL;
        }
        return NULL;
    case K_SET:
        if (strcmp(sname, "count") == 0) return (void *)(uintptr_t)1;
        return NULL;
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* ObjC runtime helpers (ivar, class)                                 */
/* ------------------------------------------------------------------ */

static void *fake_object_getClass(void *obj) {
    if (!obj) return NULL;
    return ((fake_obj_t *)obj)->isa;
}

static void *fake_class_getInstanceMethod(void *cls, void *sel) {
    (void)cls;
    (void)sel;
    return NULL;    /* no trace hooks: every hook install is a no-op */
}

static void *fake_method_setImplementation(void *m, void *imp) {
    (void)m;
    (void)imp;
    return NULL;
}

static void *fake_class_getInstanceVariable(void *cls, const char *name) {
    if (cls == &g_cls_event && name && strcmp(name, "_touches") == 0) {
        return &g_ivar_touches;
    }
    return NULL;    /* touch ivars do not resolve; setters carry it */
}

static long fake_ivar_getOffset(void *iv) {
    if (!iv) return 0;
    return ((fake_ivar_t *)iv)->offset;
}

static void *fake_object_setIvar(void *obj, void *iv, void *val) {
    if (!obj || !iv) return NULL;
    *(void **)((char *)obj + fake_ivar_getOffset(iv)) = val;
    return val;
}

static void *fake_object_getIvar(void *obj, void *iv) {
    if (!obj || !iv) return NULL;
    return *(void **)((char *)obj + fake_ivar_getOffset(iv));
}

static const char *fake_object_getClassName(void *obj) {
    (void)obj;
    return "Fake";
}

/* ------------------------------------------------------------------ */
/* CoreFoundation / IOKit fakes (signatures match touch.c's casts)    */
/* ------------------------------------------------------------------ */

static void *fake_cf_retain(void *obj) {
    fake_obj_t *o = (fake_obj_t *)obj;
    if (!o) return NULL;
    g_cfretain_count++;
    if (o->kind == K_HID) fake_retain_obj(o);
    return obj;
}

static void fake_cf_release(void *obj) {
    fake_obj_t *o = (fake_obj_t *)obj;
    if (!o) return;
    g_cfrelease_count++;
    if (o->kind == K_HID) fake_release_obj(o);
}

static void *fake_hid_digitizer_event(void *allocator, uint64_t time,
        uint32_t transducer_type, uint32_t transducer_index,
        uint32_t button_mask, uint32_t event_mask, uint32_t event_type,
        double x, double y, double z, double tip_pressure,
        double twist, uint32_t range, uint32_t touch,
        uint32_t options) {
    (void)allocator; (void)time; (void)transducer_type;
    (void)transducer_index; (void)button_mask; (void)event_mask;
    (void)event_type; (void)x; (void)y; (void)z; (void)tip_pressure;
    (void)twist; (void)range; (void)touch; (void)options;
    return fake_new_hid(1);
}

static void *fake_hid_finger_event(void *allocator, uint64_t time,
        uint32_t index, uint32_t type, uint32_t event_mask,
        double x, double y, double z, double pressure, double twist,
        uint32_t range, uint32_t touch, uint32_t options) {
    (void)allocator; (void)time; (void)index; (void)type;
    (void)event_mask; (void)x; (void)y; (void)z; (void)pressure;
    (void)twist; (void)range; (void)touch; (void)options;
    return fake_new_hid(0);
}

static void fake_hid_append(void *event, void *child, uint32_t options) {
    (void)event;
    (void)child;
    (void)options;
    /* Real AppendEvent retains the child; this mock does not, so the
     * child is balanced by touch_inject's CFRelease alone. The parent
     * is what tests 2 and 3 measure. */
}

static void fake_hid_set_int(void *event, uint32_t field, int64_t value) {
    (void)event;
    (void)field;
    (void)value;
}

static void fake_hid_set_sender(void *event, uint64_t sender_id) {
    (void)event;
    (void)sender_id;
}

static void fake_bks_info(void *event, uint32_t ctx, uint8_t a, uint8_t b,
                          void *c, double d, float e) {
    (void)event; (void)ctx; (void)a; (void)b; (void)c; (void)d; (void)e;
}

static void fake_noop1(void *a) { (void)a; }
static void *fake_null_ret(void) { return NULL; }

/* ------------------------------------------------------------------ */
/* libdispatch / pthread fakes                                        */
/* ------------------------------------------------------------------ */

static char g_main_q_dummy;

static void fake_dispatch_async_f(void *queue, void *ctx,
                                  void (*work)(void *)) {
    (void)queue;
    if (work) work(ctx);    /* run synchronously on the test thread */
}

static int fake_pthread_create(void *t, const void *attr,
                               void *(*fn)(void *), void *arg) {
    (void)t; (void)attr; (void)fn; (void)arg;
    return 0;   /* reader thread never starts on host */
}

/* ------------------------------------------------------------------ */
/* Environment, time, log, dlsym                                      */
/* ------------------------------------------------------------------ */

static const char *mock_getenv(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "IOSWINE_TOUCH") == 0) return "1";
    if (strcmp(name, "IOSWINE_TOUCH_DIRECT") == 0) return "1";
    return NULL;    /* trace, selftest, queue, sender, ctx, tap hack,
                     * keyboard: all off */
}

static uint64_t mock_mach_abs_time(void) {
    static uint64_t t = 1000000;
    return (t += 1000);
}

static int mock_timebase(uint32_t *numer, uint32_t *denom) {
    if (numer) *numer = 1;
    if (denom) *denom = 1;
    return 0;
}

static void mock_log(const char *msg, size_t len) {
    (void)msg;
    (void)len;    /* trace is off; discard */
}

static void *mock_dlopen(const char *path) {
    (void)path;
    return NULL;
}

static void *mock_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "_dispatch_main_q") == 0) return &g_main_q_dummy;
    if (strcmp(name, "dispatch_async_f") == 0) return (void *)fake_dispatch_async_f;
    if (strcmp(name, "pthread_create") == 0) return (void *)fake_pthread_create;
    if (strcmp(name, "objc_getClass") == 0) return (void *)fake_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)fake_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)fake_objc_msgSend;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)fake_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)fake_pool_pop;
    if (strcmp(name, "object_getClass") == 0) return (void *)fake_object_getClass;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)fake_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)fake_method_setImplementation;
    if (strcmp(name, "class_getInstanceVariable") == 0) return (void *)fake_class_getInstanceVariable;
    if (strcmp(name, "ivar_getOffset") == 0) return (void *)fake_ivar_getOffset;
    if (strcmp(name, "object_setIvar") == 0) return (void *)fake_object_setIvar;
    if (strcmp(name, "object_getIvar") == 0) return (void *)fake_object_getIvar;
    if (strcmp(name, "object_getClassName") == 0) return (void *)fake_object_getClassName;
    if (strcmp(name, "CFRunLoopSourceSignal") == 0) return (void *)fake_noop1;
    if (strcmp(name, "CFRunLoopWakeUp") == 0) return (void *)fake_noop1;
    if (strcmp(name, "CFRunLoopGetMain") == 0) return (void *)fake_null_ret;
    if (strcmp(name, "CFRelease") == 0) return (void *)fake_cf_release;
    if (strcmp(name, "CFRetain") == 0) return (void *)fake_cf_retain;
    if (strcmp(name, "IOHIDEventCreateDigitizerEvent") == 0) return (void *)fake_hid_digitizer_event;
    if (strcmp(name, "IOHIDEventCreateDigitizerFingerEvent") == 0) return (void *)fake_hid_finger_event;
    if (strcmp(name, "IOHIDEventAppendEvent") == 0) return (void *)fake_hid_append;
    if (strcmp(name, "IOHIDEventSetIntegerValue") == 0) return (void *)fake_hid_set_int;
    if (strcmp(name, "IOHIDEventSetSenderID") == 0) return (void *)fake_hid_set_sender;
    if (strcmp(name, "BKSHIDEventSetDigitizerInfo") == 0) return (void *)fake_bks_info;
    return NULL;
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

/* ------------------------------------------------------------------ */
/* Setup / teardown / drive                                           */
/* ------------------------------------------------------------------ */

static void reset_mock(void) {
    teardown_free_all();
    g_freed_n = 0;
    g_freed_ids_n = 0;
    g_overrelease = 0;
    g_cfretain_count = 0;
    g_cfrelease_count = 0;
    g_sendevent_calls = 0;
    g_last_send_event = NULL;
    g_last_touch = NULL;
    g_last_touch_id = 0;
    g_last_event = NULL;
    g_last_event_id = 0;
    g_last_set = NULL;
    g_last_set_id = 0;
    g_last_parent = NULL;
    g_last_parent_id = 0;
    g_last_child = NULL;
    g_pool = NULL;
    g_sel_n = 0;

    memset(&g_cls_touch, 0, sizeof(g_cls_touch));
    memset(&g_cls_event, 0, sizeof(g_cls_event));
    memset(&g_cls_nsset, 0, sizeof(g_cls_nsset));
    memset(&g_cls_app, 0, sizeof(g_cls_app));
    memset(&g_cls_control, 0, sizeof(g_cls_control));
    g_cls_touch.isa = &g_cls_touch;
    g_cls_touch.refcount = 1;
    g_cls_touch.kind = K_CLASS;
    g_cls_event.isa = &g_cls_event;
    g_cls_event.refcount = 1;
    g_cls_event.kind = K_CLASS;
    g_cls_nsset.isa = &g_cls_nsset;
    g_cls_nsset.refcount = 1;
    g_cls_nsset.kind = K_CLASS;
    g_cls_app.isa = &g_cls_app;
    g_cls_app.refcount = 1;
    g_cls_app.kind = K_CLASS;
    g_cls_control.isa = &g_cls_control;
    g_cls_control.refcount = 1;
    g_cls_control.kind = K_CLASS;

    memset(&g_app, 0, sizeof(g_app));
    memset(&g_win, 0, sizeof(g_win));
    memset(&g_fetcher, 0, sizeof(g_fetcher));
    memset(&g_kev, 0, sizeof(g_kev));
    g_app.isa = &g_cls_app;
    g_app.refcount = 1;
    g_app.kind = K_APP;
    g_app.slot0 = &g_win;       /* keyWindow */
    g_app.slot1 = &g_fetcher;   /* _eventFetcher */
    g_app.slot2 = g_kif_available ? &g_kev : NULL;  /* _touchesEvent */
    g_win.isa = NULL;
    g_win.refcount = 1;
    g_win.kind = K_WIN;
    g_fetcher.isa = NULL;
    g_fetcher.refcount = 1;
    g_fetcher.kind = K_FETCHER;
    g_kev.isa = &g_cls_event;
    g_kev.refcount = 1;
    g_kev.kind = K_EVENT;
    g_kev.ntouches = 0;
}

static void setup_route(int kif_available) {
    g_kif_available = kif_available;
    touch_test_reset();
    reset_mock();
    touch_test_configure(&g_ops);
    touch_init();
    touch_set_window_size(360.0, 800.0);
}

static int drive(uint32_t action, uint32_t pointer_id, float x, float y) {
    isws_touch_t t;
    memset(&t, 0, sizeof(t));
    t.action = action;
    t.pointer_id = pointer_id;
    t.pointer_count = 1;
    t.x = x;
    t.y = y;
    t.timestamp_ns = 100;
    return touch_test_process_touch(&t, 1080, 2400);
}

/* ------------------------------------------------------------------ */
/* Test 1: fallback NSSet dangle (the crash fix)                      */
/*                                                                    */
/* KIF unavailable, so the fallback builds a fresh UITouchesEvent    */
/* per phase and attaches an autoreleased NSSet through the _touches */
/* ivar. The set must survive the pool pop at the end of the inject. */
/* ------------------------------------------------------------------ */

static void test_fallback_nsset_dangle(void) {
    void *down_event;
    int down_set_id;
    void *up_event;
    int up_set_id;
    fake_obj_t *set_obj;

    setup_route(0);   /* KIF unavailable: fallback path */

    CHECK(drive(ISWS_TOUCH_DOWN, 0, 540.0f, 1200.0f) == 0);
    CHECK(g_sendevent_calls == 1);
    down_event = g_last_event;
    down_set_id = g_last_set_id;
    CHECK(down_event != NULL);
    CHECK(down_set_id != 0);
    /* The event's _touches ivar must hold the set. */
    CHECK(down_event != NULL &&
          ((fake_obj_t *)down_event)->slot0 == g_last_set);
    /* The set must still be alive after the pool popped. On base it
     * was freed by the pop and this fails. */
    CHECK(!is_freed_id(down_set_id));
    if (!is_freed_id(down_set_id)) {
        set_obj = (fake_obj_t *)g_last_set;
        CHECK(set_obj->slot0 == g_last_touch);
        CHECK(set_obj->refcount >= 1);
    }

    CHECK(drive(ISWS_TOUCH_UP, 0, 540.0f, 1200.0f) == 0);
    CHECK(g_sendevent_calls == 2);
    up_event = g_last_event;
    up_set_id = g_last_set_id;
    CHECK(up_event != NULL && up_event != down_event);
    CHECK(up_set_id != 0 && up_set_id != down_set_id);
    CHECK(up_event != NULL &&
          ((fake_obj_t *)up_event)->slot0 == g_last_set);
    CHECK(!is_freed_id(up_set_id));
    if (!is_freed_id(up_set_id)) {
        set_obj = (fake_obj_t *)g_last_set;
        CHECK(set_obj->slot0 == g_last_touch);
        CHECK(set_obj->refcount >= 1);
    }

    CHECK(g_overrelease == 0);
    teardown_free_all();
}

/* ------------------------------------------------------------------ */
/* Test 2: UITouch alloc/init +1 is never released (leak fix)         */
/*                                                                    */
/* KIF path. After DOWN/MOVE/UP the touch must be held only by the   */
/* shared event's set (refcount 1). A second DOWN clears the set,    */
/* which must free the first touch. On base the first touch stays    */
/* alive (refcount 1 after the second DOWN, immortal).                */
/* ------------------------------------------------------------------ */

static void test_touch_alloc_balance(void) {
    fake_obj_t *touch1;
    int touch1_id;
    fake_obj_t *touch2;

    setup_route(1);   /* KIF available */

    CHECK(drive(ISWS_TOUCH_DOWN, 0, 540.0f, 1200.0f) == 0);
    CHECK(g_sendevent_calls == 1);
    touch1 = (fake_obj_t *)g_last_touch;
    touch1_id = g_last_touch_id;
    CHECK(touch1 != NULL);
    /* alloc +1, DOWN retain +1, _addTouch: set +1 = 3. */
    CHECK(touch1 != NULL && touch1->refcount == 3);
    CHECK(!is_freed_id(touch1_id));

    CHECK(drive(ISWS_TOUCH_MOVE, 0, 545.0f, 1205.0f) == 0);
    CHECK(g_sendevent_calls == 2);
    CHECK(g_last_touch == touch1);   /* same touch reused across phases */
    CHECK(touch1->refcount == 3);    /* clear + add balance on MOVE */

    CHECK(drive(ISWS_TOUCH_UP, 0, 545.0f, 1205.0f) == 0);
    CHECK(g_sendevent_calls == 3);
    /* After UP the touch must be held only by the set: refcount 1.
     * On base it is 2 (the alloc +1 was never released). */
    CHECK(touch1->refcount == 1);
    CHECK(!is_freed_id(touch1_id));

    /* A second DOWN clears the set, which must free the first touch. */
    CHECK(drive(ISWS_TOUCH_DOWN, 0, 540.0f, 1200.0f) == 0);
    CHECK(g_sendevent_calls == 4);
    touch2 = (fake_obj_t *)g_last_touch;
    CHECK(touch2 != NULL && touch2 != touch1);
    CHECK(is_freed_id(touch1_id));
    CHECK(!is_freed_id(g_last_touch_id));
    CHECK(touch2->refcount == 3);

    CHECK(g_overrelease == 0);
    teardown_free_all();
}

/* ------------------------------------------------------------------ */
/* Test 3: KIF block CFRetains the parent HID event twice per phase   */
/* (leak fix). After each phase the parent must be freed by           */
/* touch_inject's single CFRelease, and CFRetain must never run on    */
/* a HID event. On base each parent is leaked (refcount 2) and       */
/* CFRetain runs twice per phase.                                     */
/* ------------------------------------------------------------------ */

static void test_hid_cfretain_balance(void) {
    int down_parent_id;
    int up_parent_id;

    setup_route(1);   /* KIF available */

    CHECK(drive(ISWS_TOUCH_DOWN, 0, 540.0f, 1200.0f) == 0);
    CHECK(g_sendevent_calls == 1);
    down_parent_id = g_last_parent_id;
    CHECK(down_parent_id != 0);
    /* Created +1, CFReleased once by touch_inject: must be freed.
     * On base the two CFRetains leave it at refcount 2. */
    CHECK(is_freed_id(down_parent_id));

    CHECK(drive(ISWS_TOUCH_MOVE, 0, 545.0f, 1205.0f) == 0);
    CHECK(g_sendevent_calls == 2);
    CHECK(is_freed_id(g_last_parent_id));

    CHECK(drive(ISWS_TOUCH_UP, 0, 545.0f, 1205.0f) == 0);
    CHECK(g_sendevent_calls == 3);
    up_parent_id = g_last_parent_id;
    CHECK(up_parent_id != 0 && up_parent_id != down_parent_id);
    CHECK(is_freed_id(up_parent_id));

    /* No CFRetain on a HID event at all, over the whole cycle. */
    CHECK(g_cfretain_count == 0);
    /* Every CFRelease was matched by a create, never an over-release. */
    CHECK(g_overrelease == 0);
    teardown_free_all();
}

int main(void) {
    test_fallback_nsset_dangle();
    test_touch_alloc_balance();
    test_hid_cfretain_balance();
    printf("heap067_objc_test: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
