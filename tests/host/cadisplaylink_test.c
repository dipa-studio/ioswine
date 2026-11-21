/*
 * cadisplaylink_test.c - host test for CADisplayLink
 * machinery in cadisplay.{c,h}.
 *
 * What it proves:
 *  (a) the main display exists with bounds 360x800 pt at scale 4
 *      (1440x3200 @ 4.0) once cadisplay_init ran, and
 *      lsk_cadisplay_has_display tracks it;
 *  (b) a display link created through the hooked factory fires N
 *      times for N vsync ticks, with monotonic timestamps and
 *      targetTimestamp - timestamp equal to the frame period;
 *  (c) displayLinkWithDisplay: with a nil display (or a display
 *      whose backing is NULL) returns a working link instead of
 *      faulting: the mock "real" factory models the +84
 *      dereference in QuartzCore 26.6.2 (it records a fault where
 *      the device segfaults), and the hooked factory returns a
 *      link in both cases.
 *
 * The mock ObjC runtime below is deliberately small: one class
 * (CADisplayLink) with a class-method table and an instance-method
 * table, selector interning by name (the real runtime interns
 * selectors, so pointer identity holds across translation units),
 * and a pending list standing in for the main queue (the test
 * pumps it by hand, which is what proves the delivery is async).
 *
 * Build (plain):
 *   cc -std=gnu11 -O1 -g -w -DCADISPLAYLINK_HOST_TEST=1 -iquote srcc \
 *      -iquote tests/host tests/host/cadisplaylink_test.c srcc/cadisplay.c \
 *      -o /tmp/cadisplaylink_test
 * Build (ASan+UBSan): add -fsanitize=address,undefined
 *      -fno-sanitize-recover=all.
 * Wired into tools/host_tests.sh as cadisplaylink_test.
 *
 * RED check: against the unpatched tree this file does not link
 * (lsk_cadisplay_vsync_tick, lsk_cadisplay_has_display,
 * cadisplay_link_install_hooks, cadisplay_link_stats and the
 * cadisplay_link_test_* helpers do not exist), and the mock "real"
 * factory faults on a nil display, which is the device crash (B)
 * reproduced in miniature.
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cadisplay.h"

/* ------------------------------------------------------------------ */
/* Check harness                                                        */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_LL(a, b) do { \
    g_checks++; \
    { long long _a = (long long)(a); long long _b = (long long)(b); \
      if (_a != _b) { \
          g_failures++; \
          fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n", \
                  __FILE__, __LINE__, #a, _a, _b); \
      } } \
} while (0)

#define CHECK_NEAR_DBL(a, b, eps) do { \
    g_checks++; \
    { double _a = (double)(a); double _b = (double)(b); double _d = _a - _b; \
      if (_d < 0) _d = -_d; \
      if (_d > (double)(eps)) { \
          g_failures++; \
          fprintf(stderr, "FAIL %s:%d: %s == %f, expected %f (+/-%f)\n", \
                  __FILE__, __LINE__, #a, _a, _b, (double)(eps)); \
      } } \
} while (0)

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ */
/* Mock present.c / iosurface.c (only what cadisplay.c references)      */
/* ------------------------------------------------------------------ */

static uint8_t mock_fb0[4320u * 2400u];
static uint8_t mock_fb1[4320u * 2400u];
static int mock_connected = 1;
static uint32_t mock_stride = 5760;
static int64_t mock_last_vsync_ns = -1;
static uint64_t mock_last_frame_id = (uint64_t)-1;
static isws_info_t mock_info;

int present_connected(void) { return mock_connected; }

uint8_t *present_framebuffer(int index) {
    if (!mock_connected) return NULL;
    if (index == 0) return mock_fb0;
    if (index == 1) return mock_fb1;
    return NULL;
}

uint32_t present_stride(void) { return mock_stride; }

int present_present(int index, const isws_rect_t *damage, uint32_t damage_count) {
    (void)damage;
    (void)damage_count;
    if (index < 0 || index > 1) return -1;
    return 0;
}

int64_t present_last_vsync_ns(void) { return mock_last_vsync_ns; }
uint64_t present_last_frame_id(void) { return mock_last_frame_id; }
isws_info_t present_last_info(void) { return mock_info; }
int present_buffer_fd(int index) { (void)index; return -1; }

int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    (void)surface_id;
    (void)out;
    return KERN_INVALID_NAME;
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    (void)surface_id;
    if (fd_out) *fd_out = -1;
    return KERN_INVALID_NAME;
}

uint32_t iosurface_surface_count(void) { return 0; }

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime                                                    */
/* ------------------------------------------------------------------ */

#define MOCK_MAX_SELS 64
#define MOCK_MAX_METHODS 32
#define MOCK_MAX_OBJECTS 64
#define MOCK_MAX_PENDING 256
#define MOCK_MAX_MSGSEND_LOG 512

static const char *mock_sels[MOCK_MAX_SELS];
static unsigned int mock_sel_n;

static void *mock_sel_register(const char *name) {
    unsigned int i;
    if (!name) return NULL;
    for (i = 0; i < mock_sel_n; i++) {
        if (streq(mock_sels[i], name)) return (void *)mock_sels[i];
    }
    if (mock_sel_n >= MOCK_MAX_SELS) return NULL;
    mock_sels[mock_sel_n] = name;
    return (void *)mock_sels[mock_sel_n++];
}

static const char *mock_sel_name(void *sel) {
    return (const char *)sel;
}

typedef struct {
    const char *sel;
    void *imp;
} mock_method_t;

typedef struct {
    const char *name;
    mock_method_t class_methods[MOCK_MAX_METHODS];
    unsigned int class_method_n;
    mock_method_t instance_methods[MOCK_MAX_METHODS];
    unsigned int instance_method_n;
} mock_class_t;

static mock_class_t mock_dl_class;

static mock_method_t *mock_method_find(mock_method_t *table, unsigned int n, void *sel) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        if ((void *)table[i].sel == sel) return &table[i];
    }
    return NULL;
}

static void mock_class_add(mock_class_t *cls, const char *sel_name, void *imp, int is_class) {
    mock_method_t *table;
    unsigned int *n;
    void *sel = mock_sel_register(sel_name);
    if (is_class) {
        table = cls->class_methods;
        n = &cls->class_method_n;
    } else {
        table = cls->instance_methods;
        n = &cls->instance_method_n;
    }
    if (*n >= MOCK_MAX_METHODS) return;
    table[*n].sel = (const char *)sel;
    table[*n].imp = imp;
    (*n)++;
}

static void *mock_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (streq(name, "CADisplayLink")) return (void *)&mock_dl_class;
    return NULL;
}

static void *mock_class_getClassMethod(void *cls, void *sel) {
    mock_class_t *c = (mock_class_t *)cls;
    mock_method_t *m;
    if (!c || !sel) return NULL;
    m = mock_method_find(c->class_methods, c->class_method_n, sel);
    return (void *)m;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    mock_class_t *c = (mock_class_t *)cls;
    mock_method_t *m;
    if (!c || !sel) return NULL;
    m = mock_method_find(c->instance_methods, c->instance_method_n, sel);
    return (void *)m;
}

static void *mock_method_setImplementation(void *method, void *imp) {
    mock_method_t *m = (mock_method_t *)method;
    void *old;
    if (!m) return NULL;
    old = m->imp;
    m->imp = imp;
    return old;
}

static void *mock_method_getImplementation(void *method) {
    mock_method_t *m = (mock_method_t *)method;
    if (!m) return NULL;
    return m->imp;
}

/* Fake objects: distinct addresses from a static pool. Nothing
 * ever dereferences them; they are identity tokens only. */
static uint64_t mock_objects[MOCK_MAX_OBJECTS];
static unsigned int mock_object_n;

static void *mock_alloc_object(void) {
    if (mock_object_n >= MOCK_MAX_OBJECTS) return NULL;
    return (void *)&mock_objects[mock_object_n++];
}

static void *mock_class_createInstance(void *cls, size_t extra) {
    (void)cls;
    (void)extra;
    return mock_alloc_object();
}

/* ------------------------------------------------------------------ */
/* Mock dispatch (main queue as a pending list)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    void *ctx;
    void (*fn)(void *);
} mock_pending_t;

static mock_pending_t mock_pending[MOCK_MAX_PENDING];
static unsigned int mock_pending_n;
static uint64_t mock_main_queue_token = 0x5151;

static void *mock_dispatch_get_main_queue(void) {
    return (void *)&mock_main_queue_token;
}

static void mock_dispatch_async_f(void *queue, void *ctx, void (*fn)(void *)) {
    (void)queue;
    if (mock_pending_n >= MOCK_MAX_PENDING) return;
    mock_pending[mock_pending_n].ctx = ctx;
    mock_pending[mock_pending_n].fn = fn;
    mock_pending_n++;
}

/* Run every pending main-queue block, in order. Returns the number
 * of blocks run. */
static unsigned int mock_pump_main_queue(void) {
    unsigned int n = mock_pending_n;
    unsigned int i;
    mock_pending_n = 0;
    for (i = 0; i < n; i++) {
        if (mock_pending[i].fn) mock_pending[i].fn(mock_pending[i].ctx);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Mock objc_msgSend: records target/selector/arg, and invokes the     */
/* registered fire handler for the test target.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    void *target;
    void *sel;
    void *arg;
} mock_msgsend_t;

static mock_msgsend_t mock_msgsend_log[MOCK_MAX_MSGSEND_LOG];
static unsigned int mock_msgsend_n;
static void *mock_target_obj;
static void *mock_fire_sel;
static unsigned int mock_target_fires;
static void *mock_target_last_link;

static void mock_msgsend(void *target, void *sel, void *arg) {
    if (mock_msgsend_n < MOCK_MAX_MSGSEND_LOG) {
        mock_msgsend_log[mock_msgsend_n].target = target;
        mock_msgsend_log[mock_msgsend_n].sel = sel;
        mock_msgsend_log[mock_msgsend_n].arg = arg;
        mock_msgsend_n++;
    }
    if (target == mock_target_obj && sel == mock_fire_sel) {
        mock_target_fires++;
        mock_target_last_link = arg;
    }
}

/* ------------------------------------------------------------------ */
/* Mock clocks                                                          */
/* ------------------------------------------------------------------ */

static int64_t mock_mono_ns = 1000000000LL;
static uint64_t mock_mach_ticks;
static uint32_t mock_tb_numer = 1;
static uint32_t mock_tb_denom = 1;
/* When set, mach ticks are derived from mono ns at 24 MHz instead
 * of being equal to mono ns (exercises the timebase conversion). */
static int mock_tb_24mhz;

static int64_t mock_clock_mono_ns(void) { return mock_mono_ns; }

static uint64_t mock_mach_absolute_time(void) {
    if (mock_tb_24mhz) return (uint64_t)(mock_mono_ns * 24 / 1000);
    return mock_mach_ticks;
}

static void mock_mach_timebase(uint32_t *numer, uint32_t *denom) {
    if (numer) *numer = mock_tb_numer;
    if (denom) *denom = mock_tb_denom;
}

static const char *mock_getenv_result;
static const char *mock_getenv_fn(const char *name) {
    (void)name;
    return mock_getenv_result;
}

static char mock_log_buf[8192];
static size_t mock_log_len;
static void mock_log_fn(const char *msg, size_t len) {
    size_t room;
    if (!msg || len == 0) return;
    room = sizeof(mock_log_buf) - mock_log_len - 1;
    if (len > room) len = room;
    memcpy(mock_log_buf + mock_log_len, msg, len);
    mock_log_len += len;
    mock_log_buf[mock_log_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Mock dlsym                                                           */
/* ------------------------------------------------------------------ */

static const char *mock_missing_symbol;

static void *mock_dlsym(const char *name) {
    if (!name) return NULL;
    if (mock_missing_symbol && streq(name, mock_missing_symbol)) return NULL;
    if (streq(name, "objc_getClass")) return (void *)mock_objc_getClass;
    if (streq(name, "sel_registerName")) return (void *)mock_sel_register;
    if (streq(name, "sel_getName")) return (void *)mock_sel_name;
    if (streq(name, "class_getClassMethod")) return (void *)mock_class_getClassMethod;
    if (streq(name, "class_getInstanceMethod")) return (void *)mock_class_getInstanceMethod;
    if (streq(name, "method_setImplementation")) return (void *)mock_method_setImplementation;
    if (streq(name, "method_getImplementation")) return (void *)mock_method_getImplementation;
    if (streq(name, "class_createInstance")) return (void *)mock_class_createInstance;
    if (streq(name, "objc_msgSend")) return (void *)mock_msgsend;
    if (streq(name, "dispatch_get_main_queue")) return (void *)mock_dispatch_get_main_queue;
    if (streq(name, "dispatch_async_f")) return (void *)mock_dispatch_async_f;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The mock "real" CADisplayLink factory: the +84 dereference model    */
/* ------------------------------------------------------------------ */

typedef struct {
    void *internal; /* CA::Display*: NULL on this runtime */
} mock_display_t;

static int mock_real_faulted;

static void *mock_real_factory_display(void *cls, void *sel, void *display,
                                       void *target, void *selector) {
    mock_display_t *d = (mock_display_t *)display;
    void *internal = d ? d->internal : NULL;
    (void)cls;
    (void)sel;
    (void)target;
    (void)selector;
    /* QuartzCore 26.6.2 displayLinkWithDisplay:target:selector:
     * +84 loads display->_display and dereferences it. On device
     * that is the SIGSEGV (x8=0); here it is recorded. */
    if (!internal) {
        mock_real_faulted = 1;
        return NULL;
    }
    return mock_alloc_object();
}

static void *mock_real_factory(void *cls, void *sel, void *target, void *selector) {
    (void)cls;
    (void)sel;
    (void)target;
    (void)selector;
    /* The public entry funnels into the private one with
     * [CADisplay mainDisplay]; on this runtime that display has a
     * NULL backing, so it faults the same way. */
    return mock_real_factory_display(cls, sel, NULL, target, selector);
}

static double mock_real_timestamp(void *self, void *sel) { (void)self; (void)sel; return 0.0; }
static double mock_real_duration(void *self, void *sel) { (void)self; (void)sel; return 0.0; }
static double mock_real_target_timestamp(void *self, void *sel) { (void)self; (void)sel; return 0.0; }
static int mock_real_is_paused(void *self, void *sel) { (void)self; (void)sel; return 1; }
static void mock_real_set_paused(void *self, void *sel, int p) { (void)self; (void)sel; (void)p; }
static long mock_real_fps(void *self, void *sel) { (void)self; (void)sel; return 0; }
static void mock_real_set_fps(void *self, void *sel, long f) { (void)self; (void)sel; (void)f; }
static long mock_real_max_fps(void *self, void *sel) { (void)self; (void)sel; return 60; }
static void *mock_real_init(void *self, void *sel) { (void)sel; return self; }
static void mock_real_add(void *self, void *sel, void *rl, void *m) { (void)self; (void)sel; (void)rl; (void)m; }
static void mock_real_remove(void *self, void *sel, void *rl, void *m) { (void)self; (void)sel; (void)rl; (void)m; }
static void mock_real_invalidate(void *self, void *sel) { (void)self; (void)sel; }
static ca_frame_rate_range_t mock_real_range(void *self, void *sel) {
    ca_frame_rate_range_t r;
    (void)self;
    (void)sel;
    r.minimum = 30.0f;
    r.maximum = 60.0f;
    r.preferred = 60.0f;
    return r;
}
static void mock_real_set_range(void *self, void *sel, ca_frame_rate_range_t r) {
    (void)self;
    (void)sel;
    (void)r;
}

/* Pre-register the mock "real" methods on the fake class, so the
 * install has something to replace and the test can prove the
 * replacement happened. */
static void mock_register_real_methods(void) {
    memset(&mock_dl_class, 0, sizeof(mock_dl_class));
    mock_dl_class.name = "CADisplayLink";
    mock_class_add(&mock_dl_class, "displayLinkWithDisplay:target:selector:",
                   (void *)mock_real_factory_display, 1);
    mock_class_add(&mock_dl_class, "displayLinkWithTarget:selector:",
                   (void *)mock_real_factory, 1);
    mock_class_add(&mock_dl_class, "init", (void *)mock_real_init, 0);
    mock_class_add(&mock_dl_class, "addToRunLoop:forMode:", (void *)mock_real_add, 0);
    mock_class_add(&mock_dl_class, "removeFromRunLoop:forMode:", (void *)mock_real_remove, 0);
    mock_class_add(&mock_dl_class, "invalidate", (void *)mock_real_invalidate, 0);
    mock_class_add(&mock_dl_class, "timestamp", (void *)mock_real_timestamp, 0);
    mock_class_add(&mock_dl_class, "duration", (void *)mock_real_duration, 0);
    mock_class_add(&mock_dl_class, "targetTimestamp", (void *)mock_real_target_timestamp, 0);
    mock_class_add(&mock_dl_class, "isPaused", (void *)mock_real_is_paused, 0);
    mock_class_add(&mock_dl_class, "setPaused:", (void *)mock_real_set_paused, 0);
    mock_class_add(&mock_dl_class, "preferredFramesPerSecond", (void *)mock_real_fps, 0);
    mock_class_add(&mock_dl_class, "setPreferredFramesPerSecond:", (void *)mock_real_set_fps, 0);
    mock_class_add(&mock_dl_class, "maximumFramesPerSecond", (void *)mock_real_max_fps, 0);
    mock_class_add(&mock_dl_class, "preferredFrameRateRange", (void *)mock_real_range, 0);
    mock_class_add(&mock_dl_class, "setPreferredFrameRateRange:", (void *)mock_real_set_range, 0);
}

/* ------------------------------------------------------------------ */
/* Test setup                                                           */
/* ------------------------------------------------------------------ */

typedef void *(*factory_display_fn)(void *, void *, void *, void *, void *);
typedef void *(*factory_fn)(void *, void *, void *, void *);
typedef void *(*init_fn)(void *, void *);
typedef void (*add_fn)(void *, void *, void *, void *);
typedef void (*invalidate_fn)(void *, void *);
typedef double (*double_getter_fn)(void *, void *);
typedef int (*int_getter_fn)(void *, void *);
typedef void (*set_paused_fn)(void *, void *, int);
typedef long (*long_getter_fn)(void *, void *);
typedef void (*set_fps_fn)(void *, void *, long);
typedef ca_frame_rate_range_t (*range_getter_fn)(void *, void *);
typedef void (*set_range_fn)(void *, void *, ca_frame_rate_range_t);

static cdl_ops_t g_ops;

static void test_setup(void) {
    mock_sel_n = 0;
    mock_object_n = 0;
    mock_pending_n = 0;
    mock_msgsend_n = 0;
    mock_target_fires = 0;
    mock_target_last_link = NULL;
    mock_real_faulted = 0;
    mock_mono_ns = 1000000000LL;
    mock_mach_ticks = 1000000000ULL;
    mock_tb_numer = 1;
    mock_tb_denom = 1;
    mock_tb_24mhz = 0;
    mock_missing_symbol = NULL;
    mock_getenv_result = NULL;
    mock_log_len = 0;
    mock_log_buf[0] = '\0';
    mock_target_obj = mock_alloc_object();
    mock_fire_sel = mock_sel_register("displayLinkDidFire:");
    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.dlsym_fn = mock_dlsym;
    g_ops.clock_mono_ns_fn = mock_clock_mono_ns;
    g_ops.mach_ticks_fn = mock_mach_absolute_time;
    g_ops.mach_timebase_fn = mock_mach_timebase;
    g_ops.getenv_fn = mock_getenv_fn;
    g_ops.log_fn = mock_log_fn;
    mock_register_real_methods();
    cadisplay_shutdown();
    cadisplay_link_test_configure(&g_ops);
}

/* Advance the mock clocks by one 60 Hz frame and run one vsync tick. */
static void tick_once(void) {
    mock_mono_ns += 16666667LL;
    mock_mach_ticks = (uint64_t)mock_mono_ns;
    lsk_cadisplay_vsync_tick((uint64_t)mock_mono_ns);
}

static void *make_link(void) {
    factory_display_fn factory;
    factory = (factory_display_fn)cadisplay_link_test_imp(
        "displayLinkWithDisplay:target:selector:", 1);
    if (!factory) return NULL;
    return factory(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
}

/* ------------------------------------------------------------------ */
/* (a) Main display geometry                                            */
/* ------------------------------------------------------------------ */

static void test_has_display(void) {
    cadisplay_info_t info;
    test_setup();
    CHECK_EQ_LL(lsk_cadisplay_has_display(), 0);
    CHECK(cadisplay_init(1440, 3200, 4) == 0);
    CHECK_EQ_LL(lsk_cadisplay_has_display(), 1);
    CHECK(cadisplay_info(&info) == 0);
    CHECK_EQ_LL(info.width, 1440);
    CHECK_EQ_LL(info.height, 3200);
    CHECK_EQ_LL(info.scale, 4);
    CHECK_EQ_LL(info.width / info.scale, 360);
    CHECK_EQ_LL(info.height / info.scale, 800);
    CHECK_EQ_LL(info.refresh_hz, 60);
    cadisplay_shutdown();
    CHECK_EQ_LL(lsk_cadisplay_has_display(), 0);
}

/* ------------------------------------------------------------------ */
/* (c) The fault model: the real factory faults on a nil display        */
/* ------------------------------------------------------------------ */

static void test_real_factory_faults(void) {
    mock_display_t nil_internal;
    mock_display_t healthy;
    void *link;
    test_setup();
    nil_internal.internal = NULL;
    healthy.internal = (void *)0x1234;
    mock_real_faulted = 0;
    link = mock_real_factory_display(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
    CHECK(link == NULL);
    CHECK_EQ_LL(mock_real_faulted, 1);
    mock_real_faulted = 0;
    link = mock_real_factory_display(NULL, NULL, &nil_internal, mock_target_obj, mock_fire_sel);
    CHECK(link == NULL);
    CHECK_EQ_LL(mock_real_faulted, 1);
    mock_real_faulted = 0;
    link = mock_real_factory_display(NULL, NULL, &healthy, mock_target_obj, mock_fire_sel);
    CHECK(link != NULL);
    CHECK_EQ_LL(mock_real_faulted, 0);
}

/* ------------------------------------------------------------------ */
/* Install + factory through the hooks                                  */
/* ------------------------------------------------------------------ */

static void test_install_and_factory(void) {
    cadisplay_link_stats_t st;
    void *imp;
    factory_display_fn factory;
    factory_fn pub_factory;
    init_fn init_imp;
    void *link_nil, *link_ok, *link_pub, *raw, *inited;
    mock_display_t healthy;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    imp = cadisplay_link_test_imp("displayLinkWithDisplay:target:selector:", 1);
    CHECK(imp != NULL);
    CHECK(imp != (void *)mock_real_factory_display);
    imp = cadisplay_link_test_imp("displayLinkWithTarget:selector:", 1);
    CHECK(imp != NULL);
    CHECK(imp != (void *)mock_real_factory);
    imp = cadisplay_link_test_imp("timestamp", 0);
    CHECK(imp != NULL);
    CHECK(imp != (void *)mock_real_timestamp);
    imp = cadisplay_link_test_imp("noSuchMethod:", 0);
    CHECK(imp == NULL);
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    healthy.internal = (void *)0x1234;
    factory = (factory_display_fn)cadisplay_link_test_imp(
        "displayLinkWithDisplay:target:selector:", 1);
    CHECK(factory != NULL);
    link_nil = factory(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
    CHECK(link_nil != NULL);
    link_ok = factory(NULL, NULL, &healthy, mock_target_obj, mock_fire_sel);
    CHECK(link_ok != NULL);
    CHECK(link_nil != link_ok);
    pub_factory = (factory_fn)cadisplay_link_test_imp("displayLinkWithTarget:selector:", 1);
    CHECK(pub_factory != NULL);
    link_pub = pub_factory(NULL, NULL, mock_target_obj, mock_fire_sel);
    CHECK(link_pub != NULL);
    CHECK(link_pub != link_nil);
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.links_created, 3);
    CHECK_EQ_LL(st.links_live, 3);
    CHECK_EQ_LL(st.faults_avoided, 2);
    /* init returns self without registering a link. */
    init_imp = (init_fn)cadisplay_link_test_imp("init", 0);
    CHECK(init_imp != NULL);
    raw = mock_alloc_object();
    CHECK(raw != NULL);
    inited = init_imp(raw, NULL);
    CHECK(inited == raw);
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.links_created, 3);
}

/* ------------------------------------------------------------------ */
/* (b) Firing: N ticks, N callbacks, monotonic timestamps               */
/* ------------------------------------------------------------------ */

static void test_firing(void) {
    factory_display_fn factory;
    add_fn add;
    double_getter_fn ts_get, dur_get, target_ts_get;
    cadisplay_link_stats_t st;
    void *link;
    double ts[5];
    double prev_ts;
    int i;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    factory = (factory_display_fn)cadisplay_link_test_imp(
        "displayLinkWithDisplay:target:selector:", 1);
    link = factory(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
    CHECK(link != NULL);
    /* Not added to a runloop: no fire. */
    tick_once();
    CHECK_EQ_LL(mock_pump_main_queue(), 0);
    CHECK_EQ_LL(mock_target_fires, 0);
    add = (add_fn)cadisplay_link_test_imp("addToRunLoop:forMode:", 0);
    CHECK(add != NULL);
    add(link, NULL, NULL, NULL);
    ts_get = (double_getter_fn)cadisplay_link_test_imp("timestamp", 0);
    target_ts_get = (double_getter_fn)cadisplay_link_test_imp("targetTimestamp", 0);
    dur_get = (double_getter_fn)cadisplay_link_test_imp("duration", 0);
    CHECK(ts_get != NULL);
    CHECK(target_ts_get != NULL);
    CHECK(dur_get != NULL);
    prev_ts = 0.0;
    for (i = 0; i < 5; i++) {
        tick_once();
        CHECK_EQ_LL(mock_pump_main_queue(), 1);
        ts[i] = ts_get(link, NULL);
        CHECK(ts[i] > prev_ts);
        prev_ts = ts[i];
        if (i > 0) CHECK_NEAR_DBL(ts[i] - ts[i - 1], 0.016666667, 1e-6);
        CHECK_NEAR_DBL(target_ts_get(link, NULL) - ts[i], 0.016666667, 1e-6);
    }
    CHECK_EQ_LL(mock_target_fires, 5);
    CHECK(mock_target_last_link == link);
    CHECK_NEAR_DBL(dur_get(link, NULL), 1.0 / 60.0, 1e-9);
    CHECK(ts[0] > 1.0);
    CHECK(ts[4] < 1.2);
    CHECK_EQ_LL(mock_msgsend_n, 5);
    for (i = 0; i < 5; i++) {
        CHECK(mock_msgsend_log[i].target == mock_target_obj);
        CHECK(mock_msgsend_log[i].sel == mock_fire_sel);
        CHECK(mock_msgsend_log[i].arg == link);
    }
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.fired, 5);
    CHECK_EQ_LL(st.ticks, 6);
    CHECK_EQ_LL(st.dropped, 0);
}

/* ------------------------------------------------------------------ */
/* Rate, pause, invalidate, unknown objects                             */
/* ------------------------------------------------------------------ */

static void test_rate_pause_invalidate(void) {
    add_fn add;
    invalidate_fn inv;
    set_fps_fn set_fps;
    long_getter_fn get_fps;
    set_paused_fn set_paused;
    int_getter_fn is_paused;
    double_getter_fn ts_get;
    cadisplay_link_stats_t st;
    void *link;
    void *unknown;
    int i;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    link = make_link();
    CHECK(link != NULL);
    add = (add_fn)cadisplay_link_test_imp("addToRunLoop:forMode:", 0);
    add(link, NULL, NULL, NULL);
    set_fps = (set_fps_fn)cadisplay_link_test_imp("setPreferredFramesPerSecond:", 0);
    get_fps = (long_getter_fn)cadisplay_link_test_imp("preferredFramesPerSecond", 0);
    CHECK(set_fps != NULL);
    CHECK(get_fps != NULL);
    set_fps(link, NULL, 30);
    CHECK_EQ_LL(get_fps(link, NULL), 30);
    for (i = 0; i < 4; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 2);
    set_paused = (set_paused_fn)cadisplay_link_test_imp("setPaused:", 0);
    is_paused = (int_getter_fn)cadisplay_link_test_imp("isPaused", 0);
    CHECK(set_paused != NULL);
    CHECK(is_paused != NULL);
    set_paused(link, NULL, 1);
    CHECK_EQ_LL(is_paused(link, NULL), 1);
    for (i = 0; i < 2; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 2);
    set_paused(link, NULL, 0);
    CHECK_EQ_LL(is_paused(link, NULL), 0);
    for (i = 0; i < 2; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 3);
    inv = (invalidate_fn)cadisplay_link_test_imp("invalidate", 0);
    CHECK(inv != NULL);
    inv(link, NULL);
    for (i = 0; i < 2; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 3);
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.links_live, 0);
    /* Unknown objects get safe defaults, never a crash. */
    unknown = mock_alloc_object();
    CHECK(unknown != NULL);
    ts_get = (double_getter_fn)cadisplay_link_test_imp("timestamp", 0);
    CHECK_NEAR_DBL(ts_get(unknown, NULL), 0.0, 0.0);
    CHECK_EQ_LL(is_paused(unknown, NULL), 1);
    add(unknown, NULL, NULL, NULL);
    tick_once();
    (void)mock_pump_main_queue();
    CHECK_EQ_LL(mock_target_fires, 3);
}

/* ------------------------------------------------------------------ */
/* Stale fire dropped on invalidate (generation guard)                  */
/* ------------------------------------------------------------------ */

static void test_stale_fire_dropped(void) {
    add_fn add;
    invalidate_fn inv;
    cadisplay_link_stats_t st;
    void *link, *link2;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    link = make_link();
    CHECK(link != NULL);
    add = (add_fn)cadisplay_link_test_imp("addToRunLoop:forMode:", 0);
    add(link, NULL, NULL, NULL);
    tick_once();
    CHECK_EQ_LL(mock_pending_n, 1);
    inv = (invalidate_fn)cadisplay_link_test_imp("invalidate", 0);
    inv(link, NULL);
    CHECK_EQ_LL(mock_pump_main_queue(), 1);
    CHECK_EQ_LL(mock_target_fires, 0);
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.dropped, 1);
    /* Slot reuse fires the new link, not the old one. */
    link2 = make_link();
    CHECK(link2 != NULL);
    CHECK(link2 != link);
    add(link2, NULL, NULL, NULL);
    tick_once();
    CHECK_EQ_LL(mock_pump_main_queue(), 1);
    CHECK_EQ_LL(mock_target_fires, 1);
    CHECK(mock_target_last_link == link2);
}

/* ------------------------------------------------------------------ */
/* Table full, frame rate range, 24 MHz clock, install failures        */
/* ------------------------------------------------------------------ */

static void test_table_full(void) {
    cadisplay_link_stats_t st;
    factory_display_fn factory;
    void *link;
    int i;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    factory = (factory_display_fn)cadisplay_link_test_imp(
        "displayLinkWithDisplay:target:selector:", 1);
    for (i = 0; i < 16; i++) {
        link = factory(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
        CHECK(link != NULL);
    }
    link = factory(NULL, NULL, NULL, mock_target_obj, mock_fire_sel);
    CHECK(link == NULL);
    CHECK(cadisplay_link_stats(&st) == 0);
    CHECK_EQ_LL(st.links_created, 16);
    CHECK_EQ_LL(st.links_live, 16);
}

static void test_frame_rate_range(void) {
    add_fn add;
    set_range_fn set_range;
    range_getter_fn get_range;
    long_getter_fn get_fps;
    ca_frame_rate_range_t set, got;
    void *link;
    int i;
    test_setup();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    link = make_link();
    CHECK(link != NULL);
    set_range = (set_range_fn)cadisplay_link_test_imp("setPreferredFrameRateRange:", 0);
    get_range = (range_getter_fn)cadisplay_link_test_imp("preferredFrameRateRange", 0);
    get_fps = (long_getter_fn)cadisplay_link_test_imp("preferredFramesPerSecond", 0);
    CHECK(set_range != NULL);
    CHECK(get_range != NULL);
    CHECK(get_fps != NULL);
    got = get_range(link, NULL);
    CHECK_NEAR_DBL(got.minimum, 30.0, 1e-6);
    CHECK_NEAR_DBL(got.maximum, 60.0, 1e-6);
    CHECK_NEAR_DBL(got.preferred, 60.0, 1e-6);
    set.minimum = 15.0f;
    set.maximum = 30.0f;
    set.preferred = 30.0f;
    set_range(link, NULL, set);
    got = get_range(link, NULL);
    CHECK_NEAR_DBL(got.minimum, 15.0, 1e-6);
    CHECK_NEAR_DBL(got.maximum, 30.0, 1e-6);
    CHECK_NEAR_DBL(got.preferred, 30.0, 1e-6);
    CHECK_EQ_LL(get_fps(link, NULL), 30);
    add = (add_fn)cadisplay_link_test_imp("addToRunLoop:forMode:", 0);
    add(link, NULL, NULL, NULL);
    for (i = 0; i < 4; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 2);
}

static void test_clock_24mhz(void) {
    add_fn add;
    double_getter_fn ts_get;
    void *link;
    double ts;
    int i;
    test_setup();
    mock_tb_24mhz = 1;
    mock_tb_numer = 1000;
    mock_tb_denom = 24;
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    link = make_link();
    CHECK(link != NULL);
    add = (add_fn)cadisplay_link_test_imp("addToRunLoop:forMode:", 0);
    add(link, NULL, NULL, NULL);
    for (i = 0; i < 3; i++) {
        tick_once();
        (void)mock_pump_main_queue();
    }
    CHECK_EQ_LL(mock_target_fires, 3);
    ts_get = (double_getter_fn)cadisplay_link_test_imp("timestamp", 0);
    ts = ts_get(link, NULL);
    /* Timestamp tracks the mono clock (seconds) within a
     * microsecond, despite the 24 MHz timebase conversion. */
    CHECK_NEAR_DBL(ts, (double)mock_mono_ns * 1e-9, 1e-6);
}

static void test_missing_symbol(void) {
    test_setup();
    mock_missing_symbol = "objc_msgSend";
    CHECK_EQ_LL(cadisplay_link_install_hooks(), -1);
    CHECK(cadisplay_link_test_imp("timestamp", 0) == NULL);
    mock_missing_symbol = NULL;
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
}

static void test_kill_switch(void) {
    test_setup();
    mock_getenv_result = "1";
    CHECK_EQ_LL(cadisplay_link_install_hooks(), -1);
    mock_getenv_result = NULL;
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
    mock_getenv_result = "0";
    cadisplay_link_test_reset();
    CHECK_EQ_LL(cadisplay_link_install_hooks(), 0);
}

int main(void) {
    test_has_display();
    test_real_factory_faults();
    test_install_and_factory();
    test_firing();
    test_rate_pause_invalidate();
    test_stale_fire_dropped();
    test_table_full();
    test_frame_rate_range();
    test_clock_24mhz();
    test_missing_symbol();
    test_kill_switch();
    if (g_failures == 0) {
        printf("cadisplaylink_test: all %d checks passed\n", g_checks);
        return 0;
    }
    printf("cadisplaylink_test: %d failures out of %d checks\n", g_failures, g_checks);
    return 1;
}
