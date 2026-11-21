/* tuikb_lock.c - serialize TUIKBGraphSerialization's decoded-object
 * cache.
 *
 * Root cause summary: the
 * _deserializationCache NSMutableDictionary (ivar +80 of
 * TUIKBGraphSerialization) is read and written by
 * -[TUIKBGraphSerialization objectForOffset:] with no lock anywhere
 * in the method and no lock ivar in the class. Two threads decoding
 * the same keyboard layout graph at once corrupt the dictionary,
 * which then hands out garbage objects (the 0x220
 * -[__NSArrayM insertObject:atIndex:] fault and the objectForKey:
 * bucket-garbage fault in the device crashes).
 *
 * The fix swizzles objectForOffset: to a wrapper that takes a
 * recursive lock around the original IMP. Recursive, because the
 * decode is a deep recursion: objectForOffset: calls itself (and
 * the ref/array helpers that call it) for nested offsets, so the
 * same thread re-enters the wrapper on every nested object. The
 * lock is taken only at the outermost entry (depth 0) and released
 * when the outermost call returns; nested entries just bump the
 * depth. That keeps the whole decode of one graph atomic with
 * respect to a second thread decoding the same graph, while a
 * single thread's recursion never blocks on itself.
 *
 * Threading: the decode runs on the main thread (keyboard
 * presentation) and on the keyboard-layout background queue
 * (TextInput preload). The lock serializes those two. The lock
 * owner is the Linux thread id, so two different threads can
 * never both hold it, and the same thread can re-enter freely.
 *
 * Freestanding C11, no libc, no malloc, no __thread. Follows the
 * CADisplayLink swizzle pattern in cadisplay.c (dlsym via the
 * dyld gAPIs, PAC-sign the replacement IMP, all-or-nothing
 * install, env kill-switch). No function-pointer tables in static
 * data: the runtime functions are resolved once at install time
 * into individual statics.
 *
 * No em dashes are used in this file.
 */
#include "tuikb_lock.h"

#if !defined(TUIKB_LOCK_HOST_TEST) && defined(__aarch64__)
#include "linux.h"
extern const char *lsk_getenv(const char *name);
#endif

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache.
 * Same addresses snapshot.c, carender.c and cadisplay.c use. */
#define TUIKB_GAPIS_ADDR         0x1ec954040ULL
#define TUIKB_GAPIS_OBJ_DISC     0x63faULL
#define TUIKB_SLOT_DLSYM         16ULL
#define TUIKB_DIV_DLSYM          0x839dULL

/* Runtime functions, resolved once at install time. */
static void *(*tuikb_p_objc_getClass)(const char *);
static void *(*tuikb_p_sel_registerName)(const char *);
static void *(*tuikb_p_class_getInstanceMethod)(void *, void *);
static void *(*tuikb_p_method_setImplementation)(void *, void *);
static void *(*tuikb_p_method_getImplementation)(void *);

/* The original objectForOffset: IMP, saved at install time. The
 * wrapper calls through it. Signature: (self, _cmd, cursor) -> id,
 * where cursor is a pointer to the graph reader state (the offset
 * int lives at [cursor]). */
static void *(*tuikb_orig_objectForOffset)(void *, void *, void *);

static int g_tuikb_hooks_installed;
static void *g_tuikb_class;

/* Statistics. */
static uint64_t g_tuikb_installs;
static uint64_t g_tuikb_calls;
static uint64_t g_tuikb_recursive;
static uint64_t g_tuikb_contended;
static uint64_t g_tuikb_max_depth;

#ifdef TUIKB_LOCK_HOST_TEST
static const tuikb_ops_t *g_tuikb_ops;
static int g_tuikb_enabled = 1;
#endif

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static size_t tuikb_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void tuikb_log(const char *s) {
    size_t n;
    if (!s) return;
    n = tuikb_strlen(s);
    if (n == 0) return;
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->log_fn) g_tuikb_ops->log_fn(s, n);
#elif defined(__aarch64__)
    (void)sys_write(2, s, n);
#else
    /* Plain host build: the module is never exercised on host
     * outside the host test. */
#endif
}

static void tuikb_log_u64(const char *prefix, uint64_t v) {
    char buf[128];
    char num[24];
    size_t pos = 0;
    size_t n = 0;
    uint64_t t = v;
    size_t i;
    if (v == 0) {
        num[n++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (t > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (rn > 0) num[n++] = rev[--rn];
    }
    while (prefix[pos] && pos < sizeof(buf) - 32) {
        buf[pos] = prefix[pos];
        pos++;
    }
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (i = 0; i < n && pos < sizeof(buf) - 2; i++) buf[pos++] = num[i];
    buf[pos++] = '\n';
    buf[pos] = '\0';
    tuikb_log(buf);
}

/* ------------------------------------------------------------------ */
/* dlsym / PAC signing (cadisplay.c pattern)                            */
/* ------------------------------------------------------------------ */

static void *tuikb_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(TUIKB_LOCK_HOST_TEST)
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

#if defined(__aarch64__) && !defined(TUIKB_LOCK_HOST_TEST)
static __attribute__((noinline)) void *tuikb_gapis_call2(uint64_t slot, uint64_t div,
                                                         uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)TUIKB_GAPIS_ADDR;
    uint64_t vt;
    uint64_t obj_div;
    uint64_t slot_addr;
    uint64_t fn;
    uint64_t fn_div;
    if (!obj) return NULL;
    vt = *(volatile uint64_t *)obj;
    obj_div = (obj & 0x0000ffffffffffffULL) | (TUIKB_GAPIS_OBJ_DISC << 48);
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

static void *tuikb_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->dlsym_fn) return g_tuikb_ops->dlsym_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return tuikb_gapis_call2(TUIKB_SLOT_DLSYM, TUIKB_DIV_DLSYM,
                             (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static const char *tuikb_getenv(const char *name) {
    if (!name) return NULL;
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->getenv_fn) return g_tuikb_ops->getenv_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return lsk_getenv(name);
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Recursive lock                                                       */
/*                                                                      */
/* A tiny recursive mutex: one owner thread id, a recursion count,    */
/* and a futex word for sleeping contenders. The lock is taken only   */
/* at depth 0; nested entries on the same thread just bump the       */
/* count. This is not a pthread mutex (our psynch mutex is not        */
/* recursive, and the wrapper must re-enter on the same thread), so  */
/* the recursion is tracked here, in this module, with the thread    */
/* id as the owner tag.                                                */
/*                                                                      */
/* The futex word mirrors the owner: 0 means free, 1 means held.      */
/* A contender that finds it held spins briefly (the decode of one   */
/* graph is short), then sleeps on the futex until the holder        */
/* releases. The holder wakes one waiter on release.                 */
/* ------------------------------------------------------------------ */

static volatile long g_tuikb_lock_owner;   /* 0 = free, else thread id */
static volatile int g_tuikb_lock_depth;    /* recursion depth, owner only */
static volatile int g_tuikb_lock_word;     /* futex word: 0 free, 1 held */

static long tuikb_tid(void) {
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->tid_fn) return g_tuikb_ops->tid_fn();
    return 0;
#elif defined(__aarch64__)
    return sys0(SYS_gettid);
#else
    return 0;
#endif
}

static void tuikb_futex_wait(volatile int *addr, int val) {
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->futex_wait_fn) {
        g_tuikb_ops->futex_wait_fn(addr, val);
        return;
    }
#elif defined(__aarch64__)
    sys4(SYS_futex, (long)addr, FUTEX_WAIT_PRIVATE, (long)val, 0);
#endif
}

static void tuikb_futex_wake(volatile int *addr, int count) {
#ifdef TUIKB_LOCK_HOST_TEST
    if (g_tuikb_ops && g_tuikb_ops->futex_wake_fn) {
        g_tuikb_ops->futex_wake_fn(addr, count);
        return;
    }
#elif defined(__aarch64__)
    sys3(SYS_futex, (long)addr, FUTEX_WAKE_PRIVATE, (long)count);
#endif
}

static int tuikb_lock_acquire(void) {
    long tid = tuikb_tid();
    long owner;
    int spins;
    /* Fast path: we already own it (recursion). Returns 1 so the
     * caller can count the re-entry. */
    owner = __atomic_load_n(&g_tuikb_lock_owner, __ATOMIC_ACQUIRE);
    if (owner == tid && tid != 0) {
        g_tuikb_lock_depth++;
        return 1;
    }
    /* Slow path: take ownership. Spin briefly, then futex-sleep. */
    spins = 0;
    for (;;) {
        long prev = 0;
        if (__atomic_compare_exchange_n(&g_tuikb_lock_owner, &prev, tid,
                                        0 /* weak */, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            g_tuikb_lock_depth = 1;
            __atomic_store_n(&g_tuikb_lock_word, 1, __ATOMIC_RELEASE);
            return 0;
        }
        /* Lost the race, or another thread holds it. */
        spins++;
        if (spins < 100) {
            /* Brief spin: the holder is decoding one graph, which
             * is short. No pause instruction is available in
             * freestanding C; the loop itself is the backoff. */
            continue;
        }
        /* Sleep until the holder releases. The futex word is 1
         * while held, 0 when free; re-check the owner after
         * waking, because another waiter may have taken it. */
        g_tuikb_contended++;
        tuikb_futex_wait(&g_tuikb_lock_word, 1);
        spins = 0;
    }
}

static void tuikb_lock_release(void) {
    long tid = tuikb_tid();
    long owner = __atomic_load_n(&g_tuikb_lock_owner, __ATOMIC_ACQUIRE);
    if (owner != tid) {
        /* Not the owner: a release without an acquire. Never
         * happens in the wrapper; log and ignore rather than
         * corrupting the lock. */
        tuikb_log("[tuikb] lock release by non-owner\n");
        return;
    }
    g_tuikb_lock_depth--;
    if (g_tuikb_lock_depth > 0) return;
    /* Outermost release: hand the lock over. */
    g_tuikb_lock_depth = 0;
    __atomic_store_n(&g_tuikb_lock_owner, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tuikb_lock_word, 0, __ATOMIC_RELEASE);
    tuikb_futex_wake(&g_tuikb_lock_word, 1);
}

/* ------------------------------------------------------------------ */
/* Wrapper                                                              */
/* ------------------------------------------------------------------ */

/* The swizzled IMP for -[TUIKBGraphSerialization objectForOffset:].
 * Takes the recursive lock, calls the original IMP, releases the
 * lock, returns the decoded object. The lock serializes whole
 * decodes (the outermost call) across threads; nested calls on
 * the same thread re-enter without blocking. */
static void *tuikb_hook_objectForOffset(void *self, void *sel, void *cursor) {
    void *result;
    int reentered;
#ifdef TUIKB_LOCK_HOST_TEST
    if (!g_tuikb_enabled) {
        if (!tuikb_orig_objectForOffset) return NULL;
        return tuikb_orig_objectForOffset(self, sel, cursor);
    }
#endif
    if (!tuikb_orig_objectForOffset) return NULL;
    reentered = tuikb_lock_acquire();
    g_tuikb_calls++;
    if (reentered) g_tuikb_recursive++;
    if ((uint64_t)g_tuikb_lock_depth > g_tuikb_max_depth)
        g_tuikb_max_depth = (uint64_t)g_tuikb_lock_depth;
    result = tuikb_orig_objectForOffset(self, sel, cursor);
    tuikb_lock_release();
    return result;
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */

int tuikb_lock_install_hooks(void) {
    const char *off;
    void *cls;
    void *sel;
    void *m;
    void *orig;
    tuikb_log("[tuikb] install hooks\n");
    if (g_tuikb_hooks_installed) return 0;
    off = tuikb_getenv("IOSWINE_NO_TUIKB_LOCK");
    if (off && off[0] == '1' && off[1] == '\0') {
        tuikb_log("[tuikb] hooks disabled by IOSWINE_NO_TUIKB_LOCK=1\n");
        return -1;
    }
    /* Resolve everything first: the install is all-or-nothing. */
    tuikb_p_objc_getClass = (void *(*)(const char *))tuikb_sign_fn(tuikb_dlsym("objc_getClass"));
    tuikb_p_sel_registerName = (void *(*)(const char *))tuikb_sign_fn(tuikb_dlsym("sel_registerName"));
    tuikb_p_class_getInstanceMethod = (void *(*)(void *, void *))tuikb_sign_fn(tuikb_dlsym("class_getInstanceMethod"));
    tuikb_p_method_setImplementation = (void *(*)(void *, void *))tuikb_sign_fn(tuikb_dlsym("method_setImplementation"));
    tuikb_p_method_getImplementation = (void *(*)(void *))tuikb_sign_fn(tuikb_dlsym("method_getImplementation"));
    if (!tuikb_p_objc_getClass || !tuikb_p_sel_registerName ||
        !tuikb_p_class_getInstanceMethod || !tuikb_p_method_setImplementation ||
        !tuikb_p_method_getImplementation) {
        tuikb_log("[tuikb] hooks unavailable (missing runtime symbols)\n");
        return -1;
    }
    cls = tuikb_p_objc_getClass("TUIKBGraphSerialization");
    if (!cls) {
        tuikb_log("[tuikb] TUIKBGraphSerialization class not found, hooks off\n");
        return -1;
    }
    g_tuikb_class = cls;
    sel = tuikb_p_sel_registerName("objectForOffset:");
    if (!sel) {
        tuikb_log("[tuikb] objectForOffset: selector missing\n");
        return -1;
    }
    m = tuikb_p_class_getInstanceMethod(cls, sel);
    if (!m) {
        tuikb_log("[tuikb] objectForOffset: method missing\n");
        return -1;
    }
    orig = tuikb_p_method_getImplementation(m);
    if (!orig) {
        tuikb_log("[tuikb] objectForOffset: no original IMP\n");
        return -1;
    }
    tuikb_orig_objectForOffset = (void *(*)(void *, void *, void *))tuikb_sign_fn(orig);
    tuikb_p_method_setImplementation(m, tuikb_sign_fn((void *)tuikb_hook_objectForOffset));
    g_tuikb_hooks_installed = 1;
    g_tuikb_installs++;
    tuikb_log("[tuikb] objectForOffset: hooks installed\n");
    tuikb_log_u64("[tuikb] class=", (uint64_t)cls);
    return 0;
}

void tuikb_lock_stats(tuikb_lock_stats_t *out) {
    if (!out) return;
    out->installs = g_tuikb_installs;
    out->calls = g_tuikb_calls;
    out->recursive = g_tuikb_recursive;
    out->contended = g_tuikb_contended;
    out->max_depth = g_tuikb_max_depth;
}

#ifdef TUIKB_LOCK_HOST_TEST
void tuikb_lock_test_configure(const tuikb_ops_t *ops) {
    g_tuikb_ops = ops;
    g_tuikb_enabled = 1;
    g_tuikb_hooks_installed = 0;
    g_tuikb_class = NULL;
    tuikb_orig_objectForOffset = NULL;
    g_tuikb_installs = 0;
    g_tuikb_calls = 0;
    g_tuikb_recursive = 0;
    g_tuikb_contended = 0;
    g_tuikb_max_depth = 0;
    g_tuikb_lock_owner = 0;
    g_tuikb_lock_depth = 0;
    g_tuikb_lock_word = 0;
    tuikb_p_objc_getClass = NULL;
    tuikb_p_sel_registerName = NULL;
    tuikb_p_class_getInstanceMethod = NULL;
    tuikb_p_method_setImplementation = NULL;
    tuikb_p_method_getImplementation = NULL;
}

void tuikb_lock_test_reset(void) {
    tuikb_lock_test_configure(g_tuikb_ops);
}

void *tuikb_lock_test_imp(void) {
    void *sel;
    void *m;
    if (!g_tuikb_hooks_installed) return NULL;
    if (!tuikb_p_sel_registerName) return NULL;
    sel = tuikb_p_sel_registerName("objectForOffset:");
    if (!sel || !g_tuikb_class) return NULL;
    if (!tuikb_p_class_getInstanceMethod) return NULL;
    m = tuikb_p_class_getInstanceMethod(g_tuikb_class, sel);
    if (!m) return NULL;
    if (!tuikb_p_method_getImplementation) return NULL;
    return tuikb_p_method_getImplementation(m);
}

void tuikb_lock_test_set_enabled(int enabled) {
    g_tuikb_enabled = enabled ? 1 : 0;
}
#endif
