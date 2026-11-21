/* render_kq_concurrent_test.c - frames + kq events driven concurrently.
 *
 * A previous crash occurred in the workloop worker thread while
 * frames were flowing and IPC trace was on. The render path must
 * provably not depend on workloop timing: its waits are a Linux
 * ppoll on the presenter socket (carender engine thread), a 1 s
 * nanosleep (snapshot pump thread) and a bounded 100 ms wait for a
 * coalesced main-queue tick (carender snapshot fallback). This
 * test drives the REAL carender.c + snapshot.c + kevent.c + ipc.c
 * at the same time from four host pthreads and pins the
 * invariants:
 *   - engine thread: 200 dirty vsyncs through carender_step with
 *     the coalesced snapshot ops wired to the REAL snapshot.c;
 *   - hammer threads: 2000 snapshot_request_frame() calls;
 *   - drain thread: runs the mock main-queue ticks;
 *   - kq thread: 50 port-set knote round-trips (register, send to
 *     two member ports, expect exactly 2 delivered events,
 *     deregister, drain).
 * At the end: every dispatched tick rendered exactly once (no lost
 * or duplicated ticks), no tick pending, carender frames +
 * coalesced == dirty steps, every kq event delivered, and all
 * threads joined inside the deadline (no deadlock). Frame log
 * lines carry the src tag: every line has exactly one of
 * src=pump / src=ca, and pump + ca == frames rendered.
 *
 * Plain cc + ASan/UBSan. Build with -DCARENDER_HOST_TEST=1
 * -DSNAPSHOT_HOST_TEST=1 -DKEVENT_HOST_TEST=1, linking carender.c,
 * cadamage.c, snapshot.c, kevent.c, ipc.c. No presenter server:
 * the weak ioswine_present_frame in snapshot.c returns -1, so
 * frames log rc=-1; the counts, not the present path, are under
 * test here (present_api is covered by snapshot_pipeline_test).
 *
 * No em dashes are used in this file.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "carender.h"
#include "cadamage.h"
#include "snapshot.h"
#include "workq.h"
#include "ipc.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    uint64_t _a = (uint64_t)(a); uint64_t _b = (uint64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", __FILE__, \
               __LINE__, #a, (unsigned long long)_a, \
               (unsigned long long)_b); \
    } \
} while (0)

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) { }
}

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    size_t ln = strlen(needle);
    const char *p = hay;
    if (!hay || !needle || ln == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) { n++; p += ln; }
    return n;
}

/* ------------------------------------------------------------------ */
/* Mach / IPC / workqueue stubs (same surface as kevent_pset_test)   */
/* ------------------------------------------------------------------ */

static uint64_t g_now_ticks = 1000000;
uint64_t _mach_absolute_time(void) {
    g_now_ticks += 1000;
    return g_now_ticks;
}
int _mach_timebase_info(void *info) {
    uint32_t *p = (uint32_t *)info;
    p[0] = 1;
    p[1] = 1;
    return 0;
}
uint32_t _mach_thread_self(void) {
    return 0x303;
}
int _mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target;
    (void)flags;
    void *p = malloc(size ? size : 1);
    if (!p) return 6;
    *address = (uint64_t)p;
    return 0;
}
int _mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    (void)target;
    (void)size;
    free((void *)address);
    return 0;
}

void set_errno(int err) {
    (void)err;
}

/* The real carender.c is built with
 * CARENDER_HOST_TEST=1 here, which compiles carender_init out, but
 * the real ipc.c calls it from the 50-send lazy arm. Count calls;
 * no assertion here (this test is concurrent, counts are racy). */
static int g_carender_inits;
void carender_init(void) {
    g_carender_inits++;
}

/* Kobject service handlers live in other LSK files; this test never
 * sends a message to a service port, so these stubs are never
 * called. Each returns 0 ("not handled"). */
int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size,
                      uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int hidsys_is_enabled(void) {
    return 0;
}
int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap,
                        uint32_t *out_reply_size) {
    (void)msg; (void)send_size; (void)reply_buf; (void)reply_cap;
    (void)out_reply_size;
    return 0;
}
void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint) {
    (void)service_port;
    (void)endpoint;
}
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                           uint32_t *out_msgh_id) {
    (void)buf; (void)cap; (void)out_len; (void)out_msgh_id;
    return 0;
}

/* Delivered kevents, recorded by the workq_dispatch_kevent stub.
 * The kq thread is the only producer and the main thread the only
 * reader after join, but the stub takes the mutex anyway: the
 * notify path can run on any thread. */
#define MAX_DELIVERED 512
typedef struct {
    uint64_t ident;
    int16_t filter;
} delivered_ev_t;
static delivered_ev_t g_delivered[MAX_DELIVERED];
static int g_delivered_count;
static pthread_mutex_t g_delivered_mu = PTHREAD_MUTEX_INITIALIZER;

int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev,
                          int nevents, int qos) {
    (void)wlh;
    (void)qos;
    pthread_mutex_lock(&g_delivered_mu);
    for (int i = 0; i < nevents && g_delivered_count < MAX_DELIVERED; i++) {
        g_delivered[g_delivered_count].ident = ev[i].ident;
        g_delivered[g_delivered_count].filter = ev[i].filter;
        g_delivered_count++;
    }
    pthread_mutex_unlock(&g_delivered_mu);
    return 1;
}
void workq_lock(void) {}
void workq_unlock(void) {}
int workq_is_wlh_busy(uint64_t wlh) {
    (void)wlh;
    return 0;
}
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    (void)wlh;
    (void)busy;
    return 1;
}

static int delivered_count(void) {
    int n;
    pthread_mutex_lock(&g_delivered_mu);
    n = g_delivered_count;
    pthread_mutex_unlock(&g_delivered_mu);
    return n;
}

/* ------------------------------------------------------------------ */
/* Mock main queue: one pending dispatch slot (exact for snapshot.c, */
/* which only dispatches when no tick is outstanding). A dispatch   */
/* while the slot is full is counted as an overwrite failure.       */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_mq_mu = PTHREAD_MUTEX_INITIALIZER;
static void (*g_mq_cb)(void *);
static void *g_mq_ctx;
static int g_dispatch_after_calls;
static int g_dispatch_overwrites;
static int g_main_q_dummy;

static uint64_t mock_dispatch_time(uint64_t when, int64_t delta) {
    return when + (uint64_t)delta;
}

static void mock_dispatch_after_f(uint64_t when, void *q, void *ctx,
                                  void (*cb)(void *)) {
    (void)when;
    (void)q;
    pthread_mutex_lock(&g_mq_mu);
    g_dispatch_after_calls++;
    if (g_mq_cb != NULL) g_dispatch_overwrites++;
    else {
        g_mq_cb = cb;
        g_mq_ctx = ctx;
    }
    pthread_mutex_unlock(&g_mq_mu);
}

/* Take the pending callback, if any. The caller runs it without
 * holding the mutex (the tick may dispatch its drain tick). */
static int mq_take(void (**cb_out)(void *), void **ctx_out) {
    int have = 0;
    pthread_mutex_lock(&g_mq_mu);
    if (g_mq_cb != NULL) {
        *cb_out = g_mq_cb;
        *ctx_out = g_mq_ctx;
        g_mq_cb = NULL;
        g_mq_ctx = NULL;
        have = 1;
    }
    pthread_mutex_unlock(&g_mq_mu);
    return have;
}

static int mq_pending(void) {
    int have;
    pthread_mutex_lock(&g_mq_mu);
    have = g_mq_cb != NULL;
    pthread_mutex_unlock(&g_mq_mu);
    return have;
}

/* ------------------------------------------------------------------ */
/* Mock Objective-C / CoreGraphics (one window, one layer)           */
/* ------------------------------------------------------------------ */

static const char *g_env_snapshot;
static const char *g_env_pump;
static uint64_t g_now_ns;
static char g_log_buf[65536];
static size_t g_log_len;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void test_log(const char *msg, size_t len) {
    pthread_mutex_lock(&g_log_mu);
    if (g_log_len + len < sizeof(g_log_buf)) {
        memcpy(g_log_buf + g_log_len, msg, len);
        g_log_len += len;
        g_log_buf[g_log_len] = '\0';
    }
    pthread_mutex_unlock(&g_log_mu);
}

static const char *test_getenv(const char *name) {
    if (strcmp(name, "IOSWINE_SNAPSHOT") == 0) return g_env_snapshot;
    if (strcmp(name, "IOSWINE_SNAPSHOT_PUMP") == 0) return g_env_pump;
    if (strcmp(name, "IOSWINE_SNAPSHOT_PNG") == 0) return NULL;
    return NULL;
}

static uint64_t test_now_ns(void) {
    return g_now_ns;
}

static void *test_alloc_buf(size_t size) {
    return calloc(1, size);
}

static void test_free_buf(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}

typedef struct {
    double x, y;
} test_point_t;

typedef struct {
    double width, height;
} test_size_t;

typedef struct {
    test_point_t origin;
    test_size_t size;
} test_rect_t;

enum {
    KIND_NONE = 0,
    KIND_CLASS_UIAPP,
    KIND_CLASS_UISCREEN,
    KIND_APP,
    KIND_SET,
    KIND_ARRAY,
    KIND_SCENE,
    KIND_WINDOW,
    KIND_SCREEN,
    KIND_LAYER,
};

typedef struct mock_obj {
    int kind;
    void *items[16];
    uint64_t count;
    uint8_t hidden;
    double alpha;
    test_rect_t bounds;
    test_rect_t frame;
    double level;
    struct mock_obj *screen;
    struct mock_obj *layer;
    double content_scale;
    double scale;
    struct mock_obj *scenes_set;
    struct mock_obj *legacy_windows;
} mock_obj_t;

typedef struct {
    void *data;
    uint32_t w, h, stride;
} mock_cg_ctx_t;

static mock_obj_t g_cls_uiapp = { .kind = KIND_CLASS_UIAPP };
static mock_obj_t g_cls_uiscreen = { .kind = KIND_CLASS_UISCREEN };
static mock_obj_t *g_shared_app = NULL;
static mock_obj_t *g_main_screen = NULL;

static int g_render_count;
static int g_pool_push_calls;
static int g_pool_pop_calls;

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "UIApplication") == 0) return &g_cls_uiapp;
    if (strcmp(name, "UIScreen") == 0) return &g_cls_uiscreen;
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    return (void *)name;
}

static int sel_eq(void *sel, const char *name) {
    return sel && strcmp((const char *)sel, name) == 0;
}

static uint8_t mock_responds(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return 0;
    switch (o->kind) {
    case KIND_CLASS_UIAPP:
        return sel_eq(sel, "sharedApplication");
    case KIND_CLASS_UISCREEN:
        return sel_eq(sel, "mainScreen");
    case KIND_APP:
        return sel_eq(sel, "connectedScenes") || sel_eq(sel, "windows");
    case KIND_SET:
        return sel_eq(sel, "allObjects");
    case KIND_ARRAY:
        return sel_eq(sel, "count") || sel_eq(sel, "objectAtIndex:");
    case KIND_SCENE:
        return sel_eq(sel, "windows");
    case KIND_WINDOW:
        return sel_eq(sel, "isHidden") || sel_eq(sel, "alpha") ||
               sel_eq(sel, "bounds") || sel_eq(sel, "frame") ||
               sel_eq(sel, "windowLevel") || sel_eq(sel, "screen") ||
               sel_eq(sel, "contentScaleFactor") || sel_eq(sel, "layer");
    case KIND_SCREEN:
        return sel_eq(sel, "scale");
    case KIND_LAYER:
        return sel_eq(sel, "renderInContext:");
    default:
        return 0;
    }
}

static void *mock_objc_msgSend_id(void *obj, void *sel, ...) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return NULL;
    if (sel_eq(sel, "respondsToSelector:")) {
        __builtin_va_list ap;
        __builtin_va_start(ap, sel);
        void *qsel = __builtin_va_arg(ap, void *);
        __builtin_va_end(ap);
        return (void *)(uintptr_t)mock_responds(obj, qsel);
    }
    if (o->kind == KIND_CLASS_UIAPP && sel_eq(sel, "sharedApplication")) {
        return g_shared_app;
    }
    if (o->kind == KIND_CLASS_UISCREEN && sel_eq(sel, "mainScreen")) {
        return g_main_screen;
    }
    if (o->kind == KIND_APP) {
        if (sel_eq(sel, "connectedScenes")) return o->scenes_set;
        if (sel_eq(sel, "windows")) return o->legacy_windows;
    }
    if (o->kind == KIND_SET && sel_eq(sel, "allObjects")) {
        return o->items[0];
    }
    if (o->kind == KIND_ARRAY) {
        if (sel_eq(sel, "count")) return (void *)(uintptr_t)o->count;
        if (sel_eq(sel, "objectAtIndex:")) {
            __builtin_va_list ap;
            __builtin_va_start(ap, sel);
            uint64_t idx = __builtin_va_arg(ap, uint64_t);
            __builtin_va_end(ap);
            return idx < o->count ? o->items[idx] : NULL;
        }
    }
    if (o->kind == KIND_SCENE && sel_eq(sel, "windows")) {
        return o->items[0];
    }
    if (o->kind == KIND_WINDOW) {
        if (sel_eq(sel, "isHidden")) return (void *)(uintptr_t)o->hidden;
        if (sel_eq(sel, "screen")) return o->screen;
        if (sel_eq(sel, "layer")) return o->layer;
    }
    if (o->kind == KIND_LAYER && sel_eq(sel, "renderInContext:")) {
        __atomic_fetch_add(&g_render_count, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    return NULL;
}

static double mock_objc_msgSend_f64(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return 0.0;
    if (o->kind == KIND_WINDOW) {
        if (sel_eq(sel, "alpha")) return o->alpha;
        if (sel_eq(sel, "windowLevel")) return o->level;
        if (sel_eq(sel, "contentScaleFactor")) return o->content_scale;
    }
    if (o->kind == KIND_SCREEN && sel_eq(sel, "scale")) {
        return o->scale;
    }
    return 0.0;
}

static test_rect_t mock_objc_msgSend_rect(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    test_rect_t z = { {0, 0}, {0, 0} };
    if (!o || o->kind != KIND_WINDOW) return z;
    if (sel_eq(sel, "bounds")) return o->bounds;
    if (sel_eq(sel, "frame")) return o->frame;
    return z;
}

static void *mock_pool_push(void) {
    g_pool_push_calls++;
    return (void *)(uintptr_t)0x1234;
}

static void mock_pool_pop(void *p) {
    (void)p;
    g_pool_pop_calls++;
}

static void *mock_cg_cs_create(void) {
    static int cs_dummy;
    return &cs_dummy;
}

static void mock_cg_cs_release(void *cs) { (void)cs; }

static void *mock_cg_ctx_create(void *data, size_t w, size_t h, size_t bpc,
                                size_t bpr, void *cs, uint32_t bitmapInfo) {
    (void)bpc;
    (void)cs;
    (void)bitmapInfo;
    mock_cg_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->data = data;
    ctx->w = (uint32_t)w;
    ctx->h = (uint32_t)h;
    ctx->stride = (uint32_t)bpr;
    return ctx;
}

static void mock_cg_ctx_release(void *c) { free(c); }
static void mock_cg_translate(void *c, double tx, double ty) {
    (void)c; (void)tx; (void)ty;
}
static void mock_cg_scale(void *c, double sx, double sy) {
    (void)c; (void)sx; (void)sy;
}
static void mock_cg_save(void *c) { (void)c; }
static void mock_cg_restore(void *c) { (void)c; }

static void *mock_cg_create_image(void *c) {
    (void)c;
    static int img_dummy;
    return &img_dummy;
}

static void mock_cg_image_release(void *i) { (void)i; }

static void *mock_dlsym(const char *name) {
    if (strcmp(name, "_dispatch_main_q") == 0) return &g_main_q_dummy;
    if (strcmp(name, "dispatch_after_f") == 0) return (void *)mock_dispatch_after_f;
    if (strcmp(name, "dispatch_time") == 0) return (void *)mock_dispatch_time;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend_id;
    if (strcmp(name, "objc_msgSend_fpret") == 0) return (void *)mock_objc_msgSend_f64;
    if (strcmp(name, "objc_msgSend_stret") == 0) return (void *)mock_objc_msgSend_rect;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "CGColorSpaceCreateDeviceRGB") == 0) return (void *)mock_cg_cs_create;
    if (strcmp(name, "CGColorSpaceRelease") == 0) return (void *)mock_cg_cs_release;
    if (strcmp(name, "CGBitmapContextCreate") == 0) return (void *)mock_cg_ctx_create;
    if (strcmp(name, "CGContextRelease") == 0) return (void *)mock_cg_ctx_release;
    if (strcmp(name, "CGContextTranslateCTM") == 0) return (void *)mock_cg_translate;
    if (strcmp(name, "CGContextScaleCTM") == 0) return (void *)mock_cg_scale;
    if (strcmp(name, "CGContextSaveGState") == 0) return (void *)mock_cg_save;
    if (strcmp(name, "CGContextRestoreGState") == 0) return (void *)mock_cg_restore;
    if (strcmp(name, "CGBitmapContextCreateImage") == 0) return (void *)mock_cg_create_image;
    if (strcmp(name, "CGImageRelease") == 0) return (void *)mock_cg_image_release;
    return NULL;
}

static const snapshot_test_ops_t g_snap_ops = {
    .dlsym_default = mock_dlsym,
    .getenv_fn = test_getenv,
    .now_ns = test_now_ns,
    .log_fn = test_log,
    .alloc_buf = test_alloc_buf,
    .free_buf = test_free_buf,
};

static mock_obj_t g_scr;
static mock_obj_t g_layer;
static mock_obj_t g_win;
static mock_obj_t g_scene_wins;
static mock_obj_t g_scene;
static mock_obj_t g_scenes_arr;
static mock_obj_t g_scenes_set;
static mock_obj_t g_app;

static void setup_mock_window(double wpt, double hpt, double scale) {
    memset(&g_scr, 0, sizeof(g_scr));
    memset(&g_layer, 0, sizeof(g_layer));
    memset(&g_win, 0, sizeof(g_win));
    memset(&g_scene_wins, 0, sizeof(g_scene_wins));
    memset(&g_scene, 0, sizeof(g_scene));
    memset(&g_scenes_arr, 0, sizeof(g_scenes_arr));
    memset(&g_scenes_set, 0, sizeof(g_scenes_set));
    memset(&g_app, 0, sizeof(g_app));
    g_scr.kind = KIND_SCREEN;
    g_scr.scale = scale;
    g_layer.kind = KIND_LAYER;
    g_win.kind = KIND_WINDOW;
    g_win.hidden = 0;
    g_win.alpha = 1.0;
    g_win.bounds.origin.x = 0;
    g_win.bounds.origin.y = 0;
    g_win.bounds.size.width = wpt;
    g_win.bounds.size.height = hpt;
    g_win.frame = g_win.bounds;
    g_win.level = 0.0;
    g_win.screen = &g_scr;
    g_win.layer = &g_layer;
    g_win.content_scale = scale;
    g_scene_wins.kind = KIND_ARRAY;
    g_scene_wins.count = 1;
    g_scene_wins.items[0] = &g_win;
    g_scene.kind = KIND_SCENE;
    g_scene.items[0] = &g_scene_wins;
    g_scenes_arr.kind = KIND_ARRAY;
    g_scenes_arr.count = 1;
    g_scenes_arr.items[0] = &g_scene;
    g_scenes_set.kind = KIND_SET;
    g_scenes_set.items[0] = &g_scenes_arr;
    g_app.kind = KIND_APP;
    g_app.scenes_set = &g_scenes_set;
    g_app.legacy_windows = NULL;
    g_shared_app = &g_app;
    g_main_screen = &g_scr;
}

/* ------------------------------------------------------------------ */
/* Carender mock ops: fake 60 Hz vsync clock, no CA surface, the    */
/* coalesced snapshot trio wired to the REAL snapshot.c. sleep_ms   */
/* advances the fake clock AND sleeps for real, so the drain       */
/* thread gets scheduled while the engine waits.                    */
/* ------------------------------------------------------------------ */

#define FAKE_PERIOD_NS 16666667LL

static int64_t fake_time_ns;
static int fake_fps_lines;
static int fake_legacy_renders;

static int64_t fake_now_ns(void) {
    return fake_time_ns;
}

static void fake_ca_log(const char *msg, size_t len) {
    if (msg && len >= 5 && msg[0] == '[' && msg[1] == 'f' &&
        msg[2] == 'p' && msg[3] == 's' && msg[4] == ']') {
        __atomic_fetch_add(&fake_fps_lines, 1, __ATOMIC_RELAXED);
    }
}

static const char *fake_ca_getenv(const char *name) {
    (void)name;
    return NULL;
}

static int fake_wait_vsync(int64_t *vsync_ns_out, int timeout_ms) {
    (void)timeout_ms;
    fake_time_ns += FAKE_PERIOD_NS;
    if (vsync_ns_out) *vsync_ns_out = fake_time_ns;
    return 0;
}

static void fake_on_vsync(int64_t vsync_ns) {
    (void)vsync_ns;
}

static uint32_t fake_find_surface(void) {
    return 0;
}

static int fake_surface_seed(uint32_t surface_id, uint32_t *seed_out) {
    (void)surface_id;
    (void)seed_out;
    return -1;
}

static int fake_forward_surface(uint32_t surface_id, const void *damage,
                                uint32_t damage_count) {
    (void)surface_id;
    (void)damage;
    (void)damage_count;
    return -1;
}

static int fake_legacy_render_snapshot(void) {
    __atomic_fetch_add(&fake_legacy_renders, 1, __ATOMIC_RELAXED);
    return -1;
}

static void fake_snap_sleep_ms(int ms) {
    /* Advance the fake clock the wait loop reads, and sleep for
     * real so the drain thread runs the tick. */
    fake_time_ns += (int64_t)ms * 1000000LL;
    sleep_ms(ms);
}

static const carender_ops_t g_ca_ops = {
    fake_now_ns,
    fake_ca_log,
    fake_ca_getenv,
    fake_wait_vsync,
    fake_on_vsync,
    fake_find_surface,
    fake_surface_seed,
    fake_forward_surface,
    fake_legacy_render_snapshot,
    snapshot_request_frame_ca,
    snapshot_frame_no,
    fake_snap_sleep_ms
};

/* ------------------------------------------------------------------ */
/* KQ workload: one port set, two member ports. Each round registers */
/* a knote on the set, sends one message to each member, expects    */
/* exactly two delivered events, deregisters and drains.            */
/* ------------------------------------------------------------------ */

static uint64_t g_wlh = 0x500000000ULL;
static mach_port_name_t g_member1, g_member2, g_setA;

static int send_plain(mach_port_name_t dest, uint32_t id, uint32_t size) {
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    mach_msg_header_t *hdr = (mach_msg_header_t *)buf;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    hdr->msgh_size = size;
    hdr->msgh_remote_port = dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = id;
    return ipc_msg(buf, MACH_SEND_MSG, size, 0, 0, 0, 0, NULL, 0);
}

static int drain_one(mach_port_name_t port) {
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    ((mach_msg_header_t *)buf)->msgh_size = sizeof buf;
    return ipc_msg(buf, MACH_RCV_MSG, 0, sizeof buf, port, 0, 0, NULL, 0);
}

static int knote_add(uint64_t ident) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    memset(&ch, 0, sizeof ch);
    ch.ident = ident;
    ch.filter = EVFILT_MACHPORT;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = 4;
    int n = kevent_apply_changes(g_wlh, &ch, 1, ev, 1, KEVENT_FLAG_ERROR_EVENTS);
    if (n != 0) {
        printf("knote_add ident=0x%llx failed err=%lld\n",
               (unsigned long long)ident, (long long)ev[0].data);
        return -1;
    }
    return 0;
}

static void knote_del(uint64_t ident) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof ch);
    ch.ident = ident;
    ch.filter = EVFILT_MACHPORT;
    ch.flags = EV_DELETE;
    kevent_apply_changes(g_wlh, &ch, 1, NULL, 0, 0);
}

static int setup_kq_world(void) {
    int r;
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &g_member1);
    if (r != 0) return -1;
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &g_member2);
    if (r != 0) return -1;
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_PORT_SET, &g_setA);
    if (r != 0) return -1;
    r = ipc_port_insert_right(0x103, g_member1, g_member1, MACH_MSG_TYPE_MAKE_SEND);
    if (r != 0) return -1;
    r = ipc_port_insert_right(0x103, g_member2, g_member2, MACH_MSG_TYPE_MAKE_SEND);
    if (r != 0) return -1;
    r = ipc_port_insert_member(0x103, g_member1, g_setA);
    if (r != 0) return -1;
    r = ipc_port_insert_member(0x103, g_member2, g_setA);
    if (r != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Worker threads                                                     */
/* ------------------------------------------------------------------ */

#define ENGINE_STEPS 200
#define HAMMER_THREADS 2
#define HAMMER_PER_THREAD 1000
#define KQ_ROUNDS 50

static volatile int g_stop_drain;

static void *engine_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ENGINE_STEPS; i++) {
        carender_commit_notify();
        if (carender_step() != 0) {
            printf("engine: carender_step failed at step %d\n", i);
            return (void *)(uintptr_t)1;
        }
    }
    return NULL;
}

typedef struct {
    int n;
} hammer_arg_t;

static void *hammer_worker(void *arg) {
    hammer_arg_t *a = (hammer_arg_t *)arg;
    for (int i = 0; i < a->n; i++) snapshot_request_frame();
    return NULL;
}

static void *drain_worker(void *arg) {
    (void)arg;
    while (!g_stop_drain) {
        void (*cb)(void *) = NULL;
        void *ctx = NULL;
        if (mq_take(&cb, &ctx) && cb) {
            cb(ctx);
        } else {
            struct timespec ts = { 0, 200000 }; /* 200 us */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void *kq_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < KQ_ROUNDS; i++) {
        int before = delivered_count();
        int64_t deadline;
        if (knote_add(g_setA) != 0) return (void *)(uintptr_t)1;
        if (send_plain(g_member1, 40231, 36) != 0) return (void *)(uintptr_t)2;
        if (send_plain(g_member2, 40231, 36) != 0) return (void *)(uintptr_t)3;
        /* Delivery is synchronous on the send path; poll briefly
         * in case the notify is deferred. */
        deadline = now_ms() + 2000;
        while (delivered_count() < before + 2 && now_ms() < deadline) {
            sleep_ms(1);
        }
        if (delivered_count() != before + 2) {
            printf("kq: round %d delivered %d, want %d\n", i,
                   delivered_count(), before + 2);
            return (void *)(uintptr_t)4;
        }
        knote_del(g_setA);
        if (drain_one(g_member1) != 0) return (void *)(uintptr_t)5;
        if (drain_one(g_member2) != 0) return (void *)(uintptr_t)6;
    }
    return NULL;
}

/* Join with a deadline; 0 = joined (retval stored), -1 = still
 * running (deadlock). */
static int join_deadline(pthread_t th, void **retval, int64_t deadline_ms) {
    for (;;) {
        void *r = NULL;
        if (pthread_tryjoin_np(th, &r) == 0) {
            if (retval) *retval = r;
            return 0;
        }
        if (now_ms() >= deadline_ms) return -1;
        sleep_ms(5);
    }
}

int main(void) {
    pthread_t engine_th, drain_th, kq_th, hammer_th[HAMMER_THREADS];
    hammer_arg_t hammer_args[HAMMER_THREADS];
    void *engine_rc = NULL;
    void *kq_rc = NULL;
    carender_stats_t st;
    int64_t deadline;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* Snapshot pump, on-demand mode: every dispatch is a request
     * tick, so renders == dispatch_after_calls after the drain. */
    snapshot_test_reset();
    snapshot_test_configure(&g_snap_ops);
    setup_mock_window(360.0, 800.0, 4.0);
    g_env_snapshot = "1";
    g_env_pump = "0";
    g_now_ns = 0;
    g_log_len = 0;
    g_log_buf[0] = '\0';
    g_dispatch_after_calls = 0;
    g_dispatch_overwrites = 0;
    snapshot_init();
    CHECK(g_dispatch_after_calls == 0);

    fake_time_ns = 0;
    fake_fps_lines = 0;
    fake_legacy_renders = 0;
    carender_test_configure(&g_ca_ops);
    carender_test_set_frame(1080, 2400, 3.0);

    CHECK(setup_kq_world() == 0);

    g_stop_drain = 0;
    CHECK(pthread_create(&drain_th, NULL, drain_worker, NULL) == 0);
    CHECK(pthread_create(&engine_th, NULL, engine_worker, NULL) == 0);
    for (i = 0; i < HAMMER_THREADS; i++) {
        hammer_args[i].n = HAMMER_PER_THREAD;
        CHECK(pthread_create(&hammer_th[i], NULL, hammer_worker,
                             &hammer_args[i]) == 0);
    }
    CHECK(pthread_create(&kq_th, NULL, kq_worker, NULL) == 0);

    /* Every thread must finish inside 20 s: a hang anywhere is a
     * deadlock, and the test fails instead of hanging the suite. */
    deadline = now_ms() + 20000;
    CHECK(join_deadline(engine_th, &engine_rc, deadline) == 0);
    for (i = 0; i < HAMMER_THREADS; i++) {
        CHECK(join_deadline(hammer_th[i], NULL, deadline) == 0);
    }
    CHECK(join_deadline(kq_th, &kq_rc, deadline) == 0);
    CHECK(engine_rc == NULL);
    CHECK(kq_rc == NULL);

    g_stop_drain = 1;
    CHECK(pthread_join(drain_th, NULL) == 0);

    /* Final drain on the main thread: run anything still queued. */
    {
        int guard = 0;
        while ((snapshot_frame_pending() || mq_pending()) && guard < 100) {
            void (*cb)(void *) = NULL;
            void *ctx = NULL;
            if (mq_take(&cb, &ctx) && cb) cb(ctx);
            guard++;
        }
        CHECK(guard < 100);
    }

    /* Invariants. */
    CHECK(g_dispatch_overwrites == 0);
    CHECK(snapshot_frame_pending() == 0);
    CHECK(!mq_pending());
    CHECK_EQ_U64(g_render_count, (uint64_t)g_dispatch_after_calls);
    CHECK_EQ_U64(snapshot_frame_no(), (uint64_t)g_render_count);
    {
        uint64_t pf = 0, cf = 0;
        snapshot_frame_counts(&pf, &cf);
        CHECK_EQ_U64(pf + cf, (uint64_t)g_render_count);
        CHECK(g_render_count > 0);
    }
    /* Every frame line carries exactly one src tag. */
    {
        int frame_lines = count_substr(g_log_buf, "[snapshot] frame ");
        int pump_lines = count_substr(g_log_buf, "src=pump");
        int ca_lines = count_substr(g_log_buf, "src=ca");
        CHECK_EQ_U64(frame_lines, (uint64_t)g_render_count);
        CHECK_EQ_U64(pump_lines + ca_lines, (uint64_t)g_render_count);
        CHECK(ca_lines > 0);
    }
    /* Engine: every step waited exactly one vsync; every dirty
     * step either rendered or coalesced, none lost (skipped
     * vsyncs from rate adaptation count in neither, so the sum is
     * a <= bound, not an equality); at least one frame rendered;
     * the legacy sync path was never used; the engine logged
     * [fps] lines while alive. */
    CHECK(carender_stats(&st) == 0);
    CHECK_EQ_U64(st.vsyncs, (uint64_t)ENGINE_STEPS);
    CHECK(st.frames + st.coalesced <= (uint64_t)ENGINE_STEPS);
    CHECK(st.frames > 0);
    CHECK_EQ_U64(st.snap_frames, st.frames);
    CHECK_EQ_U64(st.ca_frames, 0u);
    CHECK(fake_legacy_renders == 0);
    CHECK(fake_fps_lines >= 1);
    /* KQ: exactly two delivered events per round, ident is the set. */
    CHECK_EQ_U64(delivered_count(), (uint64_t)(KQ_ROUNDS * 2));
    {
        int ok = 1;
        pthread_mutex_lock(&g_delivered_mu);
        for (i = 0; i < g_delivered_count; i++) {
            if (g_delivered[i].ident != g_setA ||
                g_delivered[i].filter != EVFILT_MACHPORT) {
                ok = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_delivered_mu);
        CHECK(ok);
    }

    snapshot_shutdown();
    if (g_failures == 0) {
        printf("render_kq_concurrent_test: all %d checks passed\n", g_checks);
        return 0;
    }
    printf("render_kq_concurrent_test: %d checks, %d FAILURES\n", g_checks,
           g_failures);
    return 1;
}
