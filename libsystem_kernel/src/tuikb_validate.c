/* tuikb_validate.c - bisect the TUIKB deserializationCache corruption.
 *
 * See tuikb_validate.h. The deterministic repro
 * (tap the Library "Find in Library" search field at 320,560)
 * crashes every time in -[TUIKBGraphSerialization objectForOffset:]
 * +72, in the [[self deserializationCache] objectForKey:@(offset)]
 * lookup, with the dictionary bucket holding -1 / garbage. This
 * module swizzles objectForOffset: to a wrapper that validates the
 * deserializationCache after every call (and before every call)
 * and logs the first corrupt step, so the writer is bracketed
 * between the last good validation and the first bad one.
 *
 * How it works: a side table holds the last 16 decoded (offset,
 * object) pairs. Before each call the wrapper re-looks-up every
 * recorded offset in the cache and compares the returned pointer
 * with the recorded object; after each call it records the new pair
 * and validates again. A mismatch means the cache storage changed
 * underneath live code: the offset, the expected object and the
 * object the cache now returns are logged, and the process traps
 * there (unless IOSWINE_TUIKB_VALIDATE_SOFT=1, which logs and
 * continues). The lookup goes through objectForKey: with an
 * NSNumber key built from the offset, exactly as the real method
 * does, so the validator exercises the same path that crashes.
 *
 * This is a DEBUG tool, OFF by default (IOSWINE_TUIKB_VALIDATE=1
 * enables it). It does not lock and does not change the decode.
 * It composes with tuikb_lock: install both and the
 * validation runs inside the lock.
 *
 * Freestanding C11, no libc, no malloc, no __thread. Follows the
 * CADisplayLink swizzle pattern in cadisplay.c (dlsym via the dyld
 * gAPIs, PAC-sign the replacement IMP, all-or-nothing install).
 * No function-pointer tables in static data.
 *
 * No em dashes are used in this file.
 */
#include "tuikb_validate.h"

#if !defined(TUIKB_VALIDATE_HOST_TEST) && defined(__aarch64__)
#include "linux.h"
extern const char *lsk_getenv(const char *name);
#endif

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache.
 * Same addresses snapshot.c, carender.c, cadisplay.c and
 * tuikb_lock.c use. */
#define TUIKBV_GAPIS_ADDR         0x1ec954040ULL
#define TUIKBV_GAPIS_OBJ_DISC     0x63faULL
#define TUIKBV_SLOT_DLSYM         16ULL
#define TUIKBV_DIV_DLSYM          0x839dULL

/* Runtime functions, resolved once at install time. */
static void *(*tuikbv_p_objc_getClass)(const char *name);
static void *(*tuikbv_p_sel_registerName)(const char *name);
static void *(*tuikbv_p_class_getInstanceMethod)(void *cls, void *sel);
static void *(*tuikbv_p_method_setImplementation)(void *m, void *imp);
static void *(*tuikbv_p_method_getImplementation)(void *m);
static void *(*tuikbv_p_objc_msgSend)(void *self, void *sel, ...);

/* The original objectForOffset: IMP, saved at install time. */
static void *(*tuikbv_orig_objectForOffset)(void *self, void *cmd, void *cursor);

/* Cached selectors and the NSNumber class, resolved at install. */
static void *tuikbv_sel_deserializationCache;
static void *tuikbv_sel_objectForKey;
static void *tuikbv_sel_numberWithUnsignedLongLong;
static void *tuikbv_nsnumber_class;

/* Install state. */
static int tuikbv_installed;

/* Statistics. */
static tuikb_validate_stats_t tuikbv_stats;

/* Side table: the last 16 decoded (offset, object) pairs. A ring:
 * entries[next] is the oldest when the table is full. */
#define TUIKBV_SIDE_N 16
typedef struct {
    uint64_t offset;
    void *obj;
} tuikbv_entry_t;
static tuikbv_entry_t tuikbv_side[TUIKBV_SIDE_N];
static int tuikbv_side_next;
static int tuikbv_side_count;

/* Soft mode: log corruption and continue instead of trapping. */
static int tuikbv_soft;

#ifdef TUIKB_VALIDATE_HOST_TEST
static const tuikb_validate_ops_t *g_tuikbv_ops;
#endif

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static void tuikbv_log_str(const char *s) {
#ifdef TUIKB_VALIDATE_HOST_TEST
    if (g_tuikbv_ops && g_tuikbv_ops->log_fn) {
        size_t n = 0;
        while (s[n]) n++;
        g_tuikbv_ops->log_fn(s, n);
        return;
    }
#elif defined(__aarch64__)
    size_t n = 0;
    while (s[n]) n++;
    sys_write(2, s, n);
#else
    (void)s;
#endif
}

static void tuikbv_log_hex(const char *tag, uint64_t v) {
    char buf[96];
    int pos = 0;
    while (*tag && pos < 48) buf[pos++] = *tag++;
    buf[pos++] = '=';
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xf);
        buf[pos++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[pos++] = '\n';
    buf[pos] = '\0';
    tuikbv_log_str(buf);
}

/* ------------------------------------------------------------------ */
/* dlsym / PAC signing (cadisplay.c pattern)                            */
/* ------------------------------------------------------------------ */

static void *tuikbv_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(TUIKB_VALIDATE_HOST_TEST)
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

#if defined(__aarch64__) && !defined(TUIKB_VALIDATE_HOST_TEST)
static __attribute__((noinline)) void *tuikbv_gapis_call2(uint64_t slot, uint64_t div,
                                                          uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)TUIKBV_GAPIS_ADDR;
    uint64_t vt;
    uint64_t obj_div;
    uint64_t slot_addr;
    uint64_t fn;
    uint64_t fn_div;
    if (!obj) return NULL;
    vt = *(volatile uint64_t *)obj;
    obj_div = (obj & 0x0000ffffffffffffULL) | (TUIKBV_GAPIS_OBJ_DISC << 48);
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

static void *tuikbv_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef TUIKB_VALIDATE_HOST_TEST
    if (g_tuikbv_ops && g_tuikbv_ops->dlsym_fn) return g_tuikbv_ops->dlsym_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return tuikbv_gapis_call2(TUIKBV_SLOT_DLSYM, TUIKBV_DIV_DLSYM,
                              (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static const char *tuikbv_getenv(const char *name) {
    if (!name) return NULL;
#ifdef TUIKB_VALIDATE_HOST_TEST
    if (g_tuikbv_ops && g_tuikbv_ops->getenv_fn) return g_tuikbv_ops->getenv_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return lsk_getenv(name);
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Side table + validation                                            */
/* ------------------------------------------------------------------ */

static void tuikbv_record(uint64_t offset, void *obj) {
    if (!obj) return;
    /* Refresh an existing entry for the same offset instead of
     * adding a duplicate. */
    for (int i = 0; i < tuikbv_side_count; i++) {
        if (tuikbv_side[i].offset == offset) {
            tuikbv_side[i].obj = obj;
            return;
        }
    }
    tuikbv_side[tuikbv_side_next].offset = offset;
    tuikbv_side[tuikbv_side_next].obj = obj;
    tuikbv_side_next = (tuikbv_side_next + 1) % TUIKBV_SIDE_N;
    if (tuikbv_side_count < TUIKBV_SIDE_N) tuikbv_side_count++;
}

/* The dictionary lookup. On device this is the real objectForKey:
 * with an NSNumber key built from the offset. In the host test it
 * is the configured mock. Returns the object the cache holds for
 * the offset, or NULL. */
static void *tuikbv_lookup(void *cache, uint64_t offset) {
#ifdef TUIKB_VALIDATE_HOST_TEST
    if (g_tuikbv_ops && g_tuikbv_ops->cache_lookup_fn) {
        return g_tuikbv_ops->cache_lookup_fn(cache, offset);
    }
    return NULL;
#else
    void *key;
    if (!cache || !tuikbv_p_objc_msgSend || !tuikbv_nsnumber_class ||
        !tuikbv_sel_numberWithUnsignedLongLong || !tuikbv_sel_objectForKey) {
        return NULL;
    }
    key = tuikbv_p_objc_msgSend(tuikbv_nsnumber_class,
                                tuikbv_sel_numberWithUnsignedLongLong,
                                offset);
    if (!key) return NULL;
    return tuikbv_p_objc_msgSend(cache, tuikbv_sel_objectForKey, key);
#endif
}

/* Re-look-up every recorded offset and compare. Returns 0 when
 * every recorded offset still returns its recorded object, 1 when
 * a mismatch was found (logged, stats updated). A NULL cache means
 * there is nothing to validate. */
static int tuikbv_check(void *cache) {
    if (!cache) return 0;
    tuikbv_stats.validations++;
    for (int i = 0; i < tuikbv_side_count; i++) {
        void *got = tuikbv_lookup(cache, tuikbv_side[i].offset);
        if (got != tuikbv_side[i].obj) {
            tuikbv_stats.corruptions++;
            if (tuikbv_stats.corruptions == 1) {
                tuikbv_stats.first_corrupt_offset = tuikbv_side[i].offset;
                tuikbv_stats.first_expected = (uint64_t)tuikbv_side[i].obj;
                tuikbv_stats.first_got = (uint64_t)got;
            }
            tuikbv_log_str("[tuikb-validate] CORRUPT deserializationCache\n");
            tuikbv_log_hex("[tuikb-validate] offset", tuikbv_side[i].offset);
            tuikbv_log_hex("[tuikb-validate] expected", (uint64_t)tuikbv_side[i].obj);
            tuikbv_log_hex("[tuikb-validate] got", (uint64_t)got);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The wrapper                                                          */
/* ------------------------------------------------------------------ */

static void *tuikbv_hook_objectForOffset(void *self, void *cmd, void *cursor) {
    void *cache = NULL;
    void *result;
    uint64_t offset = (uint64_t)cursor;
    tuikbv_stats.calls++;

    /* The cache is the deserializationCache ivar; the real method
     * reads it through the getter, and so do we. */
    if (tuikbv_p_objc_msgSend && tuikbv_sel_deserializationCache) {
        cache = tuikbv_p_objc_msgSend(self, tuikbv_sel_deserializationCache);
    }
#ifdef TUIKB_VALIDATE_HOST_TEST
    if (g_tuikbv_ops) cache = g_tuikbv_ops->cache;
#endif

    /* Pre-call: catch corruption the writer left behind before
     * this decode step runs. */
    if (tuikbv_check(cache)) {
        tuikbv_log_hex("[tuikb-validate] corrupt BEFORE offset", offset);
        if (!tuikbv_soft) {
#ifndef TUIKB_VALIDATE_HOST_TEST
            __builtin_trap();
#endif
            return NULL;
        }
    }

    result = tuikbv_orig_objectForOffset(self, cmd, cursor);

    /* Record the newly decoded pair, then validate again: this
     * catches corruption introduced by the decode itself. */
    if (result) tuikbv_record(offset, result);
    if (tuikbv_check(cache)) {
        tuikbv_log_hex("[tuikb-validate] corrupt AFTER offset", offset);
        if (!tuikbv_soft) {
#ifndef TUIKB_VALIDATE_HOST_TEST
            __builtin_trap();
#endif
            return NULL;
        }
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */

int tuikb_validate_install_hooks(void) {
    const char *v;
    void *cls;
    void *sel;
    void *m;
    void *orig;

    if (tuikbv_installed) return 0;
    v = tuikbv_getenv("IOSWINE_NO_TUIKB_VALIDATE");
    if (v && v[0] == '1' && v[1] == '\0') {
        tuikbv_log_str("[tuikb-validate] disabled by IOSWINE_NO_TUIKB_VALIDATE=1\n");
        return -1;
    }
    v = tuikbv_getenv("IOSWINE_TUIKB_VALIDATE");
    if (!v || !(v[0] == '1' && v[1] == '\0')) {
        /* Off by default: this is a debug bisect tool. */
        return -1;
    }
    v = tuikbv_getenv("IOSWINE_TUIKB_VALIDATE_SOFT");
    tuikbv_soft = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;

    tuikbv_p_objc_getClass = (void *(*)(const char *))tuikbv_sign_fn(tuikbv_dlsym("objc_getClass"));
    tuikbv_p_sel_registerName = (void *(*)(const char *))tuikbv_sign_fn(tuikbv_dlsym("sel_registerName"));
    tuikbv_p_class_getInstanceMethod = (void *(*)(void *, void *))tuikbv_sign_fn(tuikbv_dlsym("class_getInstanceMethod"));
    tuikbv_p_method_setImplementation = (void *(*)(void *, void *))tuikbv_sign_fn(tuikbv_dlsym("method_setImplementation"));
    tuikbv_p_method_getImplementation = (void *(*)(void *))tuikbv_sign_fn(tuikbv_dlsym("method_getImplementation"));
    tuikbv_p_objc_msgSend = (void *(*)(void *, void *, ...))tuikbv_sign_fn(tuikbv_dlsym("objc_msgSend"));
    if (!tuikbv_p_objc_getClass || !tuikbv_p_sel_registerName ||
        !tuikbv_p_class_getInstanceMethod || !tuikbv_p_method_setImplementation ||
        !tuikbv_p_method_getImplementation || !tuikbv_p_objc_msgSend) {
        tuikbv_log_str("[tuikb-validate] missing ObjC runtime symbol\n");
        return -1;
    }

    cls = tuikbv_p_objc_getClass("TUIKBGraphSerialization");
    if (!cls) {
        tuikbv_log_str("[tuikb-validate] TUIKBGraphSerialization not found\n");
        return -1;
    }
    sel = tuikbv_p_sel_registerName("objectForOffset:");
    if (!sel) return -1;
    m = tuikbv_p_class_getInstanceMethod(cls, sel);
    if (!m) {
        tuikbv_log_str("[tuikb-validate] objectForOffset: not found\n");
        return -1;
    }
    orig = tuikbv_p_method_getImplementation(m);
    if (!orig) return -1;
    tuikbv_orig_objectForOffset = (void *(*)(void *, void *, void *))tuikbv_sign_fn(orig);

    /* Cache the selectors and NSNumber class the lookup needs. */
    tuikbv_sel_deserializationCache = tuikbv_p_sel_registerName("deserializationCache");
    tuikbv_sel_objectForKey = tuikbv_p_sel_registerName("objectForKey:");
    tuikbv_sel_numberWithUnsignedLongLong = tuikbv_p_sel_registerName("numberWithUnsignedLongLong:");
    tuikbv_nsnumber_class = tuikbv_p_objc_getClass("NSNumber");
    if (!tuikbv_sel_deserializationCache || !tuikbv_sel_objectForKey ||
        !tuikbv_sel_numberWithUnsignedLongLong || !tuikbv_nsnumber_class) {
        tuikbv_log_str("[tuikb-validate] missing selector or NSNumber class\n");
        return -1;
    }

    tuikbv_p_method_setImplementation(m, tuikbv_sign_fn((void *)tuikbv_hook_objectForOffset));
    tuikbv_installed = 1;
    tuikbv_stats.installs++;
    tuikbv_log_str("[tuikb-validate] hooks installed\n");
    return 0;
}

void tuikb_validate_stats(tuikb_validate_stats_t *out) {
    if (!out) return;
    *out = tuikbv_stats;
}

#ifdef TUIKB_VALIDATE_HOST_TEST
void tuikb_validate_test_configure(const tuikb_validate_ops_t *ops) {
    g_tuikbv_ops = ops;
}

void tuikb_validate_test_reset(void) {
    tuikbv_installed = 0;
    tuikbv_stats.installs = 0;
    tuikbv_stats.calls = 0;
    tuikbv_stats.validations = 0;
    tuikbv_stats.corruptions = 0;
    tuikbv_stats.first_corrupt_offset = 0;
    tuikbv_stats.first_expected = 0;
    tuikbv_stats.first_got = 0;
    tuikbv_side_next = 0;
    tuikbv_side_count = 0;
    tuikbv_soft = 0;
    for (int i = 0; i < TUIKBV_SIDE_N; i++) {
        tuikbv_side[i].offset = 0;
        tuikbv_side[i].obj = NULL;
    }
}

void *tuikb_validate_test_imp(void) {
    if (!tuikbv_installed) return NULL;
    return (void *)tuikbv_hook_objectForOffset;
}

int tuikb_validate_test_record(uint64_t offset, void *obj) {
    tuikbv_record(offset, obj);
    return 0;
}

int tuikb_validate_test_check(void) {
    void *cache = g_tuikbv_ops ? g_tuikbv_ops->cache : NULL;
    return tuikbv_check(cache);
}
#endif
