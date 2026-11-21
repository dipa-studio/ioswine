/*
 * heap067_tuikb_validate_test.c -- host test for the TUIKB
 * deserializationCache validation swizzle (* second round). Drives tuikb_validate.c with a mock dictionary
 * (a fake bucket array) and a mock ObjC runtime, proving the
 * validator catches a corrupted bucket and logs the first corrupt
 * step, without needing the real TextInputUI.
 *
 * Build: cc -DTUIKB_VALIDATE_HOST_TEST=1 this file + tuikb_validate.c
 */
#define TUIKB_VALIDATE_HOST_TEST 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tuikb_validate.h"

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
/* Mock dictionary (the deserializationCache stand-in)                  */
/* ------------------------------------------------------------------ */

#define MOCK_CACHE_N 64
typedef struct {
    uint64_t offset;
    void *obj;
    int used;
} mock_bucket_t;
static mock_bucket_t g_cache[MOCK_CACHE_N];

static void mock_cache_clear(void) {
    memset(g_cache, 0, sizeof g_cache);
}

static void *mock_cache_lookup(void *cache, uint64_t offset) {
    (void)cache;
    for (int i = 0; i < MOCK_CACHE_N; i++) {
        if (g_cache[i].used && g_cache[i].offset == offset) return g_cache[i].obj;
    }
    return NULL;
}

static void mock_cache_store(uint64_t offset, void *obj) {
    for (int i = 0; i < MOCK_CACHE_N; i++) {
        if (g_cache[i].used && g_cache[i].offset == offset) {
            g_cache[i].obj = obj;
            return;
        }
    }
    for (int i = 0; i < MOCK_CACHE_N; i++) {
        if (!g_cache[i].used) {
            g_cache[i].used = 1;
            g_cache[i].offset = offset;
            g_cache[i].obj = obj;
            return;
        }
    }
}

/* Simulate the writer: overwrite the bucket for an offset with a
 * bad pointer (the -1 the repro crash shows). */
static void mock_cache_corrupt(uint64_t offset, void *bad) {
    for (int i = 0; i < MOCK_CACHE_N; i++) {
        if (g_cache[i].used && g_cache[i].offset == offset) {
            g_cache[i].obj = bad;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime                                                    */
/* ------------------------------------------------------------------ */

/* Fake selectors: distinct non-NULL pointers per name. */
static void *mock_sel_objectForOffset;
static void *mock_sel_deserializationCache;
static void *mock_sel_objectForKey;
static void *mock_sel_numberWithUnsignedLongLong;

/* Fake classes and method. */
static char mock_class_tuikb;
static char mock_class_nsnumber;
static char mock_method;

/* The mock original IMP: a decode that looks the offset up in the
 * mock cache, and on a miss decodes (fabricates) an object, stores
 * it and returns it. Mirrors the real objectForOffset: shape. */
static void *mock_orig_objectForOffset(void *self, void *cmd, void *cursor) {
    (void)self;
    (void)cmd;
    uint64_t offset = (uint64_t)cursor;
    void *obj = mock_cache_lookup(NULL, offset);
    if (obj) return obj;
    obj = (void *)(uintptr_t)(0x10000ULL + offset * 0x100ULL);
    mock_cache_store(offset, obj);
    return obj;
}

/* Installed IMP recorded by the mock method_setImplementation. */
static void *g_installed_imp;

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "TUIKBGraphSerialization") == 0) return &mock_class_tuikb;
    if (strcmp(name, "NSNumber") == 0) return &mock_class_nsnumber;
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    if (strcmp(name, "objectForOffset:") == 0) return mock_sel_objectForOffset;
    if (strcmp(name, "deserializationCache") == 0) return mock_sel_deserializationCache;
    if (strcmp(name, "objectForKey:") == 0) return mock_sel_objectForKey;
    if (strcmp(name, "numberWithUnsignedLongLong:") == 0) return mock_sel_numberWithUnsignedLongLong;
    return NULL;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    if (cls == &mock_class_tuikb && sel == mock_sel_objectForOffset) return &mock_method;
    return NULL;
}

static void *mock_method_getImplementation(void *m) {
    if (m == &mock_method) return (void *)mock_orig_objectForOffset;
    return NULL;
}

static void *mock_method_setImplementation(void *m, void *imp) {
    if (m == &mock_method) {
        void *old = g_installed_imp ? g_installed_imp : (void *)mock_orig_objectForOffset;
        g_installed_imp = imp;
        return old;
    }
    return NULL;
}

static void *mock_objc_msgSend(void *self, void *sel, ...) {
    (void)self;
    if (sel == mock_sel_deserializationCache) return (void *)g_cache;
    return NULL;
}

static void *mock_dlsym(const char *name) {
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    if (strcmp(name, "method_getImplementation") == 0) return (void *)mock_method_getImplementation;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend;
    return NULL;
}

/* Env mock: validate enabled, soft mode off. */
static const char *mock_getenv(const char *name) {
    if (strcmp(name, "IOSWINE_TUIKB_VALIDATE") == 0) return "1";
    if (strcmp(name, "IOSWINE_NO_TUIKB_VALIDATE") == 0) return NULL;
    if (strcmp(name, "IOSWINE_TUIKB_VALIDATE_SOFT") == 0) return NULL;
    return NULL;
}

/* Log capture. */
static char g_log[4096];
static size_t g_log_len;
static void mock_log(const char *msg, size_t len) {
    if (g_log_len + len < sizeof g_log) {
        memcpy(g_log + g_log_len, msg, len);
        g_log_len += len;
        g_log[g_log_len] = '\0';
    }
}

static void setup_ops(void) {
    static tuikb_validate_ops_t ops;
    mock_sel_objectForOffset = (void *)(uintptr_t)0x1001;
    mock_sel_deserializationCache = (void *)(uintptr_t)0x1002;
    mock_sel_objectForKey = (void *)(uintptr_t)0x1003;
    mock_sel_numberWithUnsignedLongLong = (void *)(uintptr_t)0x1004;
    g_installed_imp = NULL;
    g_log_len = 0;
    g_log[0] = '\0';
    mock_cache_clear();
    ops.dlsym_fn = mock_dlsym;
    ops.getenv_fn = mock_getenv;
    ops.log_fn = mock_log;
    ops.cache_lookup_fn = mock_cache_lookup;
    ops.cache = (void *)g_cache;
    tuikb_validate_test_configure(&ops);
    tuikb_validate_test_reset();
}

/* The wrapper as the runtime would call it. */
typedef void *(*objectForOffset_fn)(void *self, void *cmd, void *cursor);
static void *call_wrapper(uint64_t offset) {
    objectForOffset_fn imp = (objectForOffset_fn)tuikb_validate_test_imp();
    if (!imp) return NULL;
    return imp((void *)(uintptr_t)0x5000, mock_sel_objectForOffset, (void *)(uintptr_t)offset);
}

int main(void) {
    tuikb_validate_stats_t st;
    void *r;

    /* 1. Clean decodes pass validation. */
    setup_ops();
    CHECK(tuikb_validate_install_hooks() == 0);
    CHECK(tuikb_validate_test_imp() != NULL);
    r = call_wrapper(100);
    CHECK(r == (void *)(uintptr_t)(0x10000ULL + 100 * 0x100ULL));
    r = call_wrapper(200);
    CHECK(r == (void *)(uintptr_t)(0x10000ULL + 200 * 0x100ULL));
    r = call_wrapper(300);
    CHECK(r == (void *)(uintptr_t)(0x10000ULL + 300 * 0x100ULL));
    tuikb_validate_stats(&st);
    CHECK(st.calls == 3);
    CHECK(st.corruptions == 0);
    CHECK(st.validations > 0);

    /* 2. A bucket corrupted between calls is caught by the next
     * pre-call validation, with the right first-corrupt step. */
    mock_cache_corrupt(200, (void *)(uintptr_t)-1);
    r = call_wrapper(400);
    CHECK(r == NULL);
    tuikb_validate_stats(&st);
    CHECK(st.corruptions == 1);
    CHECK(st.first_corrupt_offset == 200);
    CHECK(st.first_expected == (uint64_t)(0x10000ULL + 200 * 0x100ULL));
    CHECK(st.first_got == (uint64_t)(uintptr_t)-1);
    CHECK(strstr(g_log, "CORRUPT") != NULL);
    CHECK(strstr(g_log, "BEFORE") != NULL);

    /* 3. Direct record + check: clean, then corrupt. */
    setup_ops();
    CHECK(tuikb_validate_install_hooks() == 0);
    CHECK(tuikb_validate_test_record(10, (void *)(uintptr_t)0xaaaa) == 0);
    mock_cache_store(10, (void *)(uintptr_t)0xaaaa);
    CHECK(tuikb_validate_test_check() == 0);
    mock_cache_corrupt(10, (void *)(uintptr_t)0xbbbb);
    CHECK(tuikb_validate_test_check() == 1);
    tuikb_validate_stats(&st);
    CHECK(st.first_corrupt_offset == 10);
    CHECK(st.first_expected == 0xaaaa);
    CHECK(st.first_got == 0xbbbb);

    /* 4. Ring wrap: 17 records evict the first; corrupting the
     * evicted offset is not caught, corrupting a kept one is. */
    setup_ops();
    CHECK(tuikb_validate_install_hooks() == 0);
    for (uint64_t o = 1; o <= 17; o++) {
        void *obj = (void *)(uintptr_t)(0x20000ULL + o);
        mock_cache_store(o, obj);
        CHECK(tuikb_validate_test_record(o, obj) == 0);
    }
    mock_cache_corrupt(1, (void *)(uintptr_t)-1);
    CHECK(tuikb_validate_test_check() == 0);
    mock_cache_corrupt(2, (void *)(uintptr_t)-1);
    CHECK(tuikb_validate_test_check() == 1);
    tuikb_validate_stats(&st);
    CHECK(st.first_corrupt_offset == 2);

    /* 5. Install gate: without IOSWINE_TUIKB_VALIDATE=1 it stays off. */
    setup_ops();
    /* Reconfigure with an env that does not enable validation. */
    {
        static tuikb_validate_ops_t ops2;
        ops2.dlsym_fn = mock_dlsym;
        ops2.getenv_fn = NULL;
        ops2.log_fn = mock_log;
        ops2.cache_lookup_fn = mock_cache_lookup;
        ops2.cache = (void *)g_cache;
        tuikb_validate_test_configure(&ops2);
        tuikb_validate_test_reset();
    }
    CHECK(tuikb_validate_install_hooks() == -1);
    CHECK(tuikb_validate_test_imp() == NULL);

    printf("heap067_tuikb_validate_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
