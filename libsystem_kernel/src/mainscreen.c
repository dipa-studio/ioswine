/*
 * mainscreen.c - hook for +[UIScreen mainScreen].
 *
 * The race:
 *
 * +[UIScreen mainScreen] (UIKitCore, UIScreen.m) returns the first
 * object of +[UIScreen screens] and throws
 * NSInternalInconsistencyException "returning nil screen from
 * mainScreen is not allowed!" when that array is empty. The array
 * gets its first entry either from the CoreAnimation display path
 * (-[UIScreen _mainDisplayInit] -> +[CADisplay mainDisplay] ->
 * CA::Display::ensure_displays -> query_displays -> MIG 40231
 * _CASGetDisplays to the render server) or from the FrontBoard
 * scene (the scene's displayConfiguration creates the UIScreen).
 * On this system the render server never answers 40231, so
 * query_displays retries for 5 s and then gives up, ensure_displays
 * latches its done flag, +[CADisplay mainDisplay] returns NULL from
 * the primary-display slot [0x1ecd99000 + 1488] forever, and the
 * only thing that can ever populate screens is the scene, which
 * arrives asynchronously (fakeboard queues the scene-create
 * callout at initializeClient completion, delivered later via
 * ipc_svc_pump). A mainScreen call that lands after the 5 s latch
 * and before the scene registers throws. Frame stalls are a
 * different mechanism: the main thread blocking in the 5 s
 * query_displays retry is the stall's shape, but the stalls are
 * the carender vsync engine never ticking (already
 * fixed host-side), not the mainScreen throw.
 *
 * The fix: swizzle +[UIScreen mainScreen] (the same
 * method_setImplementation pattern cadisplay.c uses for
 * CADisplayLink and keyboard.c uses for UIResponder) so that, when
 * no scene has been created yet, the call waits (bounded, 5 s,
 * matching the CA deadline) for the first scene instead of
 * throwing immediately, then waits briefly (500 ms) for screens
 * to become non-empty, and only then calls through to the original
 * implementation. When a scene is already present the hook is a
 * straight pass-through. When fakeboard is not inited no scene
 * will ever come, so the hook calls through immediately and never
 * delays the call. When the wait expires with no scene, the hook
 * calls the original, preserving base behavior (it may throw, but
 * only in the genuinely displayless case).
 *
 * The hook never fabricates a UIScreen and never touches the
 * CA::Display* backing: it only serializes the query behind the
 * scene registration that already populates screens. The complete
 * fix for the underlying CA path (answering MIG 40231 so
 * query_displays succeeds) is the in-process render server work.
 *
 * Install timing matters: the first mainScreen call can precede
 * __libkernel_init_late (device logs show the 40231 send
 * before the snapshot pump is armed), so the hook is installed
 * from __libkernel_init (before app main) and retried from
 * __libkernel_init_late and at the 200th svc send, mirroring
 * cadisplay_link_install_hooks. The install is all-or-nothing and
 * safe to call early: it no-ops when the runtime symbols or the
 * UIScreen class are missing.
 *
 * Freestanding C11, no libc, no malloc, no __thread. No
 * function-pointer tables in static data: the runtime functions
 * are resolved once at install time into individual statics, the
 * same pattern snapshot.c, carender.c, cadisplay.c and keyboard.c
 * use. No em dashes are used in this file.
 */
#include "mainscreen.h"

#if !defined(MAINSCREEN_HOST_TEST) && defined(__aarch64__)
#include "linux.h"
extern const char *lsk_getenv(const char *name);
extern int fakeboard_scene_state(void);
extern int fakeboard_is_inited(void);
#endif

/* Scene state value meaning "no scene queued yet"
 * (fakeboard.h FB_SCENE_NONE, 0). */
#define MS_SCENE_NONE 0

/* Wait budgets (milliseconds). The scene wait matches the
 * query_displays 5 s deadline; the screens settle is a short
 * grace for the app to process the delivered scene-create. */
#define MS_WAIT_SCENE_MS 5000u
#define MS_WAIT_SCREENS_MS 500u
#define MS_POLL_MS 10u

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache.
 * Same addresses cadisplay.c and snapshot.c use. */
#define MS_GAPIS_ADDR 0x1ec954040ULL
#define MS_GAPIS_OBJ_DISC 0x63faULL
#define MS_SLOT_DLSYM 16ULL
#define MS_DIV_DLSYM 0x839dULL

/* ------------------------------------------------------------------ */
/* Resolved runtime functions (individual statics, no tables)          */
/* ------------------------------------------------------------------ */

static void *(*ms_objc_getClass)(const char *name);
static void *(*ms_sel_registerName)(const char *name);
static void *(*ms_class_getClassMethod)(void *cls, void *sel);
static void *(*ms_method_setImplementation)(void *method, void *imp);
static void *ms_objc_msgSend;
static void *ms_orig_mainScreen;
static void *ms_uiscreen_class;
static void *ms_sel_mainScreen;
static void *ms_sel_screens;
static void *ms_sel_count;
static int ms_hooks_installed;
static int ms_syms_resolved;

#ifdef MAINSCREEN_HOST_TEST
static const ms_ops_t *ms_ops;
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* PAC-sign a function pointer for an authenticated call (device
 * aarch64 only; identity on host). Same as cdl_sign_fn /
 * kb_sign_fn. */
static void *ms_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(MAINSCREEN_HOST_TEST)
    uint64_t v = (uint64_t)p;
    __asm__ volatile(
        "xpaci %0\n"
        "paciza %0\n"
        : "+r"(v)
    );
    return (void *)v;
#else
    return p;
#endif
}

/* dlsym through dyld4::gAPIs on device (same call2 pattern
 * cadisplay.c uses); the test ops table on host. */
#if defined(__aarch64__) && !defined(MAINSCREEN_HOST_TEST)
static __attribute__((noinline)) void *ms_gapis_call2(uint64_t slot, uint64_t div,
                                                       uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)MS_GAPIS_ADDR;
    uint64_t vt;
    uint64_t obj_div;
    uint64_t slot_addr;
    uint64_t fn;
    uint64_t fn_div;
    if (!obj) return NULL;
    vt = *(volatile uint64_t *)obj;
    obj_div = (obj & 0x0000ffffffffffffULL) | (MS_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    slot_addr = vt + slot * 8ULL;
    fn = *(volatile uint64_t *)slot_addr;
    fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

    register uint64_t x0 __asm__("x0") = obj;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x16 __asm__("x16") = fn;
    register uint64_t x17 __asm__("x17") = fn_div;
    __asm__ volatile(
        "blraa x16, x17"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "+r"(x17)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "cc", "memory"
    );
    return (void *)x0;
}
#endif

static void *ms_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef MAINSCREEN_HOST_TEST
    if (ms_ops && ms_ops->dlsym_fn) return ms_ops->dlsym_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return ms_gapis_call2(MS_SLOT_DLSYM, MS_DIV_DLSYM,
                          (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static const char *ms_getenv(const char *name) {
    if (!name) return NULL;
#ifdef MAINSCREEN_HOST_TEST
    if (ms_ops && ms_ops->getenv_fn) return ms_ops->getenv_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return lsk_getenv(name);
#else
    return NULL;
#endif
}

static void ms_log(const char *s) {
    size_t n = 0;
    if (!s) return;
#ifdef MAINSCREEN_HOST_TEST
    while (s[n]) n++;
    if (ms_ops && ms_ops->log_fn) ms_ops->log_fn(s, n);
#else
#if defined(__aarch64__)
    while (s[n]) n++;
    sys_write(2, s, n);
#else
    (void)n;
#endif
#endif
}

static void ms_sleep_ms(uint32_t ms) {
#ifdef MAINSCREEN_HOST_TEST
    if (ms_ops && ms_ops->sleep_ms_fn) ms_ops->sleep_ms_fn(ms);
#else
#if defined(__aarch64__)
    struct {
        long s;
        long ns;
    } ts;
    ts.s = (long)(ms / 1000u);
    ts.ns = (long)(ms % 1000u) * 1000000L;
    sys2(SYS_nanosleep, (long)&ts, 0);
#else
    (void)ms;
#endif
#endif
}

/* Parse IOSWINE_MAINSCREEN_WAIT_MS (decimal ms, clamped to
 * 0..30000); default MS_WAIT_SCENE_MS when unset or invalid. */
static uint32_t ms_wait_budget_ms(void) {
    const char *s = ms_getenv("IOSWINE_MAINSCREEN_WAIT_MS");
    uint32_t v = 0;
    uint32_t digits = 0;
    if (!s) return MS_WAIT_SCENE_MS;
    while (*s >= '0' && *s <= '9' && digits < 5) {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
        digits++;
    }
    if (digits == 0 || *s != '\0') return MS_WAIT_SCENE_MS;
    if (v > 30000u) v = 30000u;
    return v;
}

/* ------------------------------------------------------------------ */
/* Scene state (fakeboard on device, ops table on host)                */
/* ------------------------------------------------------------------ */

static int ms_scene_state(void) {
#ifdef MAINSCREEN_HOST_TEST
    if (ms_ops && ms_ops->scene_state_fn) return ms_ops->scene_state_fn();
    return MS_SCENE_NONE;
#elif defined(__aarch64__)
    return fakeboard_scene_state();
#else
    return MS_SCENE_NONE;
#endif
}

/* 1 when a scene can still arrive (fakeboard inited), else 0. */
static int ms_scene_expected(void) {
#ifdef MAINSCREEN_HOST_TEST
    if (ms_ops && ms_ops->scene_expected_fn) return ms_ops->scene_expected_fn();
    return 0;
#elif defined(__aarch64__)
    return fakeboard_is_inited();
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* The hook                                                            */
/* ------------------------------------------------------------------ */

/* Call the saved original +[UIScreen mainScreen] IMP. Same direct
 * cast-and-call pattern keyboard.c uses for its saved IMPs. */
typedef void *(*ms_id_msg_t)(void *self, void *cmd);

static void *ms_call_orig(void *cls, void *sel) {
    ms_id_msg_t orig = (ms_id_msg_t)ms_orig_mainScreen;
    if (!orig) return NULL;
    return orig(cls, sel);
}

/* Number of objects in +[UIScreen screens], or 0 when the class,
 * the selectors, or the call itself are unavailable. */
static uint64_t ms_screens_count(void) {
    void *arr;
    if (!ms_objc_msgSend || !ms_uiscreen_class || !ms_sel_screens || !ms_sel_count) return 0;
    arr = ((ms_id_msg_t)ms_objc_msgSend)(ms_uiscreen_class, ms_sel_screens);
    if (!arr) return 0;
    return ((uint64_t (*)(void *, void *))ms_objc_msgSend)(arr, ms_sel_count);
}

/* Replacement for +[UIScreen mainScreen]. No re-entrancy guard is
 * needed: the hook calls only fakeboard state, nanosleep, and
 * +[UIScreen screens] / -[NSArray count], none of which calls
 * mainScreen (mainScreen is the caller of screens, not the other
 * way round), and the original mainScreen reads screens directly
 * without calling mainScreen again. */
static void *ms_hook_mainScreen(void *cls, void *sel) {
    uint32_t waited = 0;
    uint32_t budget;
    /* No scene can ever arrive (fakeboard off): straight
     * pass-through, never delay the call. */
    if (!ms_scene_expected()) return ms_call_orig(cls, sel);
    /* Phase 1: wait for the first scene to be queued (state
     * leaves NONE). This is the serialization: the query waits
     * behind the registration instead of throwing while the
     * registration is still in flight. */
    budget = ms_wait_budget_ms();
    while (ms_scene_state() == MS_SCENE_NONE && waited < budget) {
        ms_sleep_ms(MS_POLL_MS);
        waited += MS_POLL_MS;
    }
    if (ms_scene_state() == MS_SCENE_NONE) {
        /* Timed out with no scene: original behavior (it may
         * throw, but only in the genuinely displayless case). */
        ms_log("[mainscreen] mainScreen waited for scene, none arrived\n");
        return ms_call_orig(cls, sel);
    }
    if (waited > 0) ms_log("[mainscreen] mainScreen waited for scene\n");
    /* Phase 2: wait briefly for screens to become non-empty. The
     * scene-create callout may still be queued when the state
     * flips; this covers the gap between "scene queued" and
     * "UIScreen registered in screens". */
    waited = 0;
    while (ms_screens_count() == 0 && waited < MS_WAIT_SCREENS_MS) {
        ms_sleep_ms(MS_POLL_MS);
        waited += MS_POLL_MS;
    }
    return ms_call_orig(cls, sel);
}

/* ------------------------------------------------------------------ */
/* Install                                                             */
/* ------------------------------------------------------------------ */

static int ms_ensure_syms(void) {
    if (ms_syms_resolved) return 0;
    ms_objc_getClass = (void *(*)(const char *))ms_sign_fn(ms_dlsym("objc_getClass"));
    ms_sel_registerName = (void *(*)(const char *))ms_sign_fn(ms_dlsym("sel_registerName"));
    ms_class_getClassMethod = (void *(*)(void *, void *))ms_sign_fn(ms_dlsym("class_getClassMethod"));
    ms_method_setImplementation = (void *(*)(void *, void *))ms_sign_fn(ms_dlsym("method_setImplementation"));
    ms_objc_msgSend = ms_sign_fn(ms_dlsym("objc_msgSend"));
    if (!ms_objc_getClass || !ms_sel_registerName || !ms_class_getClassMethod ||
        !ms_method_setImplementation || !ms_objc_msgSend) {
        return -1;
    }
    ms_syms_resolved = 1;
    return 0;
}

int mainscreen_install_hooks(void) {
    const char *off;
    void *cls;
    void *m;
    if (ms_hooks_installed) return 0;
    off = ms_getenv("IOSWINE_NO_MAINSCREEN");
    if (off && off[0] == '1' && off[1] == '\0') {
        ms_log("[mainscreen] hooks disabled by IOSWINE_NO_MAINSCREEN=1\n");
        return -1;
    }
    if (ms_ensure_syms() != 0) {
        ms_log("[mainscreen] hooks unavailable (missing runtime symbols)\n");
        return -1;
    }
    cls = ms_objc_getClass("UIScreen");
    if (!cls) {
        ms_log("[mainscreen] UIScreen class not found, hooks off\n");
        return -1;
    }
    ms_uiscreen_class = cls;
    ms_sel_mainScreen = ms_sel_registerName("mainScreen");
    ms_sel_screens = ms_sel_registerName("screens");
    ms_sel_count = ms_sel_registerName("count");
    if (!ms_sel_mainScreen || !ms_sel_screens || !ms_sel_count) {
        ms_log("[mainscreen] selectors missing, hooks off\n");
        return -1;
    }
    m = ms_class_getClassMethod(cls, ms_sel_mainScreen);
    if (!m) {
        ms_log("[mainscreen] +[UIScreen mainScreen] method missing, hooks off\n");
        return -1;
    }
    ms_orig_mainScreen = ms_method_setImplementation(m, ms_sign_fn((void *)ms_hook_mainScreen));
    if (!ms_orig_mainScreen) {
        ms_log("[mainscreen] method_setImplementation failed, hooks off\n");
        return -1;
    }
    ms_hooks_installed = 1;
    ms_log("[mainscreen] +[UIScreen mainScreen] hook installed\n");
    return 0;
}

int mainscreen_hooks_installed(void) {
    return ms_hooks_installed;
}

#ifdef MAINSCREEN_HOST_TEST
void mainscreen_test_configure(const ms_ops_t *ops) {
    ms_ops = ops;
    ms_objc_getClass = NULL;
    ms_sel_registerName = NULL;
    ms_class_getClassMethod = NULL;
    ms_method_setImplementation = NULL;
    ms_objc_msgSend = NULL;
    ms_orig_mainScreen = NULL;
    ms_uiscreen_class = NULL;
    ms_sel_mainScreen = NULL;
    ms_sel_screens = NULL;
    ms_sel_count = NULL;
    ms_hooks_installed = 0;
    ms_syms_resolved = 0;
}

void mainscreen_test_reset(void) {
    mainscreen_test_configure(ms_ops);
}

void *mainscreen_test_imp(void) {
    if (!ms_hooks_installed) return NULL;
    return ms_sign_fn((void *)ms_hook_mainScreen);
}

void *mainscreen_test_orig(void) {
    if (!ms_hooks_installed) return NULL;
    return ms_orig_mainScreen;
}
#endif /* MAINSCREEN_HOST_TEST */
