/*
 * heap067_mainscreen_test.c - host test for the
 * +[UIScreen mainScreen] hook in mainscreen.{c,h}.
 *
 * What it proves:
 * (a) the boot race reproduced: calling the mock "real" mainScreen
 *     while no scene has been created and screens is empty throws
 *     (the mock records the throw), which is the 4/11 boot crash
 *     on the unpatched tree;
 * (b) with the hook installed, the same call made while scene
 *     registration is still in flight waits for the scene and
 *     returns a screen instead of throwing;
 * (c) when the scene is already present the hook is a straight
 *     pass-through (no waiting);
 * (d) the wait is bounded: when no scene ever arrives the hook
 *     calls through to the original after the budget (the throw
 *     is preserved, base behavior);
 * (e) when no scene can ever arrive (fakeboard off) the hook does
 *     not wait at all;
 * (f) the settle phase: when the scene state flips before the
 *     screen object is in the array, the hook waits for the screen
 *     too;
 * (g) the install is idempotent and IOSWINE_NO_MAINSCREEN=1
 *     disables it.
 *
 * The mock ObjC runtime below is deliberately small: one class
 * (UIScreen) with a class-method table, selector interning by name
 * (the real runtime interns selectors, so pointer identity holds),
 * and a mock screens array whose contents the scene thread fills
 * in. The mock "real" mainScreen models the UIKitCore throw: it
 * records a throw and returns NULL when screens is empty, and
 * returns the first screen otherwise.
 *
 * Build (plain):
 *   cc -std=gnu11 -O1 -g -w -DMAINSCREEN_HOST_TEST=1 -iquote srcc \
 *      tests/host/heap067_mainscreen_test.c srcc/mainscreen.c \
 *      -o /tmp/heap067_mainscreen_test -lpthread
 * Build (ASan+UBSan): add -fsanitize=address,undefined
 *      -fno-sanitize-recover=all.
 * Wired into tools/host_tests.sh as heap067_mainscreen_test.
 *
 * RED check: against the unpatched tree this file does not link
 * (mainscreen_install_hooks and the mainscreen_test_* helpers do
 * not exist), and scenario (a) shows the throw the device sees.
 * Verified by building against a pristine base copy.
 *
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include "mainscreen.h"

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

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ */
/* Mock scene + screens state (shared with the scene thread)            */
/* ------------------------------------------------------------------ */

#define MOCK_SCENE_NONE 0
#define MOCK_SCENE_CREATING 1

static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_scene_state = MOCK_SCENE_NONE;
static int g_scene_expected = 1;
static int g_screen_count = 0;      /* screens array length */
static int g_throws = 0;            /* mock mainScreen throws */
static uint32_t g_slept_ms = 0;     /* total ms the hook slept */
static const char *g_env_wait_ms = NULL;
static const char *g_env_no_ms = NULL;

static void *mock_screen_obj = (void *)(uintptr_t)0x5c4e9a1;

static void state_lock(void) { pthread_mutex_lock(&g_state_mu); }
static void state_unlock(void) { pthread_mutex_unlock(&g_state_mu); }

static int mock_scene_state_fn(void) {
    int s;
    state_lock();
    s = g_scene_state;
    state_unlock();
    return s;
}

static int mock_scene_expected_fn(void) {
    int s;
    state_lock();
    s = g_scene_expected;
    state_unlock();
    return s;
}

static void mock_sleep_ms_fn(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
    state_lock();
    g_slept_ms += ms;
    state_unlock();
}

static const char *mock_getenv_fn(const char *name) {
    if (streq(name, "IOSWINE_MAINSCREEN_WAIT_MS")) return g_env_wait_ms;
    if (streq(name, "IOSWINE_NO_MAINSCREEN")) return g_env_no_ms;
    return NULL;
}

static void mock_log_fn(const char *msg, size_t len) {
    (void)msg;
    (void)len;
}

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime                                                    */
/* ------------------------------------------------------------------ */

#define MOCK_MAX_SELS 16
#define MOCK_MAX_METHODS 8

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

typedef struct {
    const char *sel;
    void *imp;
} mock_method_t;

typedef struct {
    const char *name;
    mock_method_t class_methods[MOCK_MAX_METHODS];
    unsigned int class_method_n;
} mock_class_t;

static mock_class_t mock_uiscreen_class;
static void *mock_screens_array = (void *)(uintptr_t)0xa44a9;

static mock_method_t *mock_method_find(mock_method_t *table, unsigned int n, void *sel) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        if ((void *)table[i].sel == sel) return &table[i];
    }
    return NULL;
}

static void *mock_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (streq(name, "UIScreen")) return (void *)&mock_uiscreen_class;
    return NULL;
}

static void *mock_class_getClassMethod(void *cls, void *sel) {
    mock_class_t *c = (mock_class_t *)cls;
    if (!c || !sel) return NULL;
    return (void *)mock_method_find(c->class_methods, c->class_method_n, sel);
}

static void *mock_method_setImplementation(void *method, void *imp) {
    mock_method_t *m = (mock_method_t *)method;
    void *old;
    if (!m) return NULL;
    old = m->imp;
    m->imp = imp;
    return old;
}

/* The mock "real" +[UIScreen mainScreen]: throws (records) when
 * screens is empty, returns the first screen otherwise. This is
 * the UIKitCore behavior the hook exists to serialize. */
static void *mock_orig_mainScreen(void *cls, void *sel) {
    int n;
    (void)cls;
    (void)sel;
    state_lock();
    n = g_screen_count;
    if (n == 0) g_throws++;
    state_unlock();
    if (n == 0) return NULL;
    return mock_screen_obj;
}

/* +[UIScreen screens]: returns the mock array object. */
static void *mock_screens(void *cls, void *sel) {
    (void)cls;
    (void)sel;
    return mock_screens_array;
}

/* -[NSArray count] on the mock array object. */
static uint64_t mock_array_count(void *self, void *sel) {
    uint64_t n;
    (void)sel;
    if (self != mock_screens_array) return 0;
    state_lock();
    n = (uint64_t)g_screen_count;
    state_unlock();
    return n;
}

/* objc_msgSend dispatch through the mock method tables. */
static void *mock_objc_msgSend(void *self, void *sel) {
    mock_class_t *c;
    mock_method_t *m;
    if (!self || !sel) return NULL;
    if (self == mock_screens_array) {
        if (streq((const char *)sel, "count")) return (void *)(uintptr_t)mock_array_count(self, sel);
        return NULL;
    }
    c = (mock_class_t *)self;
    if (c != &mock_uiscreen_class) return NULL;
    m = mock_method_find(c->class_methods, c->class_method_n, sel);
    if (!m || !m->imp) return NULL;
    return ((void *(*)(void *, void *))m->imp)(self, sel);
}

static void *mock_dlsym_fn(const char *name) {
    if (streq(name, "objc_getClass")) return (void *)mock_objc_getClass;
    if (streq(name, "sel_registerName")) return (void *)mock_sel_register;
    if (streq(name, "class_getClassMethod")) return (void *)mock_class_getClassMethod;
    if (streq(name, "method_setImplementation")) return (void *)mock_method_setImplementation;
    if (streq(name, "objc_msgSend")) return (void *)mock_objc_msgSend;
    return NULL;
}

static void mock_class_add(const char *sel_name, void *imp) {
    void *sel = mock_sel_register(sel_name);
    mock_class_t *c = &mock_uiscreen_class;
    if (c->class_method_n >= MOCK_MAX_METHODS) return;
    c->class_methods[c->class_method_n].sel = (const char *)sel;
    c->class_methods[c->class_method_n].imp = imp;
    c->class_method_n++;
}

static void mock_runtime_reset(void) {
    memset(&mock_uiscreen_class, 0, sizeof(mock_uiscreen_class));
    mock_uiscreen_class.name = "UIScreen";
    mock_sel_n = 0;
    mock_class_add("mainScreen", (void *)mock_orig_mainScreen);
    mock_class_add("screens", (void *)mock_screens);
    /* "count" is an instance method on the array; the dispatch
     * above matches it by name on the array object. Register the
     * selector so pointer identity holds. */
    (void)mock_sel_register("count");
}

static const ms_ops_t g_ops = {
    mock_dlsym_fn,
    mock_scene_state_fn,
    mock_scene_expected_fn,
    mock_sleep_ms_fn,
    mock_getenv_fn,
    mock_log_fn,
};

static void test_reset(void) {
    state_lock();
    g_scene_state = MOCK_SCENE_NONE;
    g_scene_expected = 1;
    g_screen_count = 0;
    g_throws = 0;
    g_slept_ms = 0;
    g_env_wait_ms = NULL;
    g_env_no_ms = NULL;
    state_unlock();
    mock_runtime_reset();
    mainscreen_test_configure(&g_ops);
}

/* Call +[UIScreen mainScreen] the way the runtime would: through
 * objc_msgSend, so an installed hook runs. */
static void *call_mainScreen(void) {
    void *cls = mock_objc_getClass("UIScreen");
    void *sel = mock_sel_register("mainScreen");
    return mock_objc_msgSend(cls, sel);
}

/* ------------------------------------------------------------------ */
/* Scene thread: registers the scene (and its screen) after a delay    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state_at_ms;
    uint32_t screen_at_ms;
} scene_plan_t;

static void *scene_thread(void *arg) {
    scene_plan_t *p = (scene_plan_t *)arg;
    struct timespec ts;
    if (p->state_at_ms > 0) {
        ts.tv_sec = (time_t)(p->state_at_ms / 1000u);
        ts.tv_nsec = (long)(p->state_at_ms % 1000u) * 1000000L;
        nanosleep(&ts, NULL);
    }
    state_lock();
    g_scene_state = MOCK_SCENE_CREATING;
    state_unlock();
    if (p->screen_at_ms > p->state_at_ms) {
        uint32_t d = p->screen_at_ms - p->state_at_ms;
        ts.tv_sec = (time_t)(d / 1000u);
        ts.tv_nsec = (long)(d % 1000u) * 1000000L;
        nanosleep(&ts, NULL);
    }
    state_lock();
    g_screen_count = 1;
    state_unlock();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                            */
/* ------------------------------------------------------------------ */

int main(void) {
    pthread_t th;
    scene_plan_t plan;
    void *r;
    void *orig_before;
    void *imp;

    /* (a) The race reproduced, no hook: mainScreen while no scene
     * has been created and screens is empty throws. */
    test_reset();
    r = call_mainScreen();
    CHECK(r == NULL);
    CHECK_EQ_LL(g_throws, 1);

    /* (b) The fix: hook installed, scene registers while the call
     * is in flight. State flips at 100 ms, screen lands at 150 ms.
     * The call must wait and return the screen with no throw. */
    test_reset();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    CHECK(mainscreen_hooks_installed() == 1);
    CHECK(mainscreen_test_imp() != NULL);
    orig_before = mainscreen_test_orig();
    CHECK(orig_before != NULL);
    plan.state_at_ms = 100;
    plan.screen_at_ms = 150;
    CHECK_EQ_LL(pthread_create(&th, NULL, scene_thread, &plan), 0);
    r = call_mainScreen();
    CHECK_EQ_LL(pthread_join(th, NULL), 0);
    CHECK(r == mock_screen_obj);
    CHECK_EQ_LL(g_throws, 0);
    /* It waited for the scene (state at 100 ms, screen at 150 ms)
     * and returned promptly after, well inside the 5 s budget. */
    state_lock();
    CHECK(g_slept_ms >= 50);
    CHECK(g_slept_ms < 5000);
    state_unlock();

    /* (c) Scene already present: straight pass-through, no wait. */
    test_reset();
    state_lock();
    g_scene_state = MOCK_SCENE_CREATING;
    g_screen_count = 1;
    state_unlock();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    r = call_mainScreen();
    CHECK(r == mock_screen_obj);
    CHECK_EQ_LL(g_throws, 0);
    state_lock();
    CHECK_EQ_LL(g_slept_ms, 0);
    state_unlock();

    /* (d) No scene ever arrives: the wait is bounded and the
     * original behavior (throw) is preserved. Short budget via env
     * so the test stays fast. */
    test_reset();
    g_env_wait_ms = "80";
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    r = call_mainScreen();
    CHECK(r == NULL);
    CHECK_EQ_LL(g_throws, 1);
    state_lock();
    CHECK(g_slept_ms >= 80);
    CHECK(g_slept_ms < 2000);
    state_unlock();

    /* (e) No scene can ever arrive (fakeboard off): no waiting. */
    test_reset();
    state_lock();
    g_scene_expected = 0;
    state_unlock();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    r = call_mainScreen();
    CHECK(r == NULL);
    CHECK_EQ_LL(g_throws, 1);
    state_lock();
    CHECK_EQ_LL(g_slept_ms, 0);
    state_unlock();

    /* (f) Settle phase: the scene state flips at 100 ms but the
     * screen object lands only at 250 ms. The hook must wait for
     * the screen too, not just for the state. */
    test_reset();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    plan.state_at_ms = 100;
    plan.screen_at_ms = 250;
    CHECK_EQ_LL(pthread_create(&th, NULL, scene_thread, &plan), 0);
    r = call_mainScreen();
    CHECK_EQ_LL(pthread_join(th, NULL), 0);
    CHECK(r == mock_screen_obj);
    CHECK_EQ_LL(g_throws, 0);
    state_lock();
    CHECK(g_slept_ms >= 200);
    CHECK(g_slept_ms < 5000);
    state_unlock();

    /* (g) Install is idempotent and the kill switch disables it. */
    test_reset();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    orig_before = mainscreen_test_orig();
    imp = mainscreen_test_imp();
    CHECK_EQ_LL(mainscreen_install_hooks(), 0);
    CHECK(mainscreen_test_orig() == orig_before);
    CHECK(mainscreen_test_imp() == imp);
    CHECK(mainscreen_hooks_installed() == 1);
    test_reset();
    g_env_no_ms = "1";
    CHECK_EQ_LL(mainscreen_install_hooks(), -1);
    CHECK(mainscreen_hooks_installed() == 0);
    CHECK(mainscreen_test_imp() == NULL);

    printf("heap067_mainscreen_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
