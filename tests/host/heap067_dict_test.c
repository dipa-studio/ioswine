/* heap067_dict_test.c - host test for deserializationCache dictionary.
 *
 * Drives two threads decoding the same keyboard layout graph
 * through a model of TUIKBGraphSerialization's _deserializationCache,
 * the way the app does. The model reproduces
 * Apple's objectForOffset: discipline exactly: cache lookup by
 * NSNumber(offset), decode on miss (slow, so two threads overlap),
 * store the decoded object with setObject:forKey:. The model has
 * NO lock of its own: the only serialization is the tuikb_lock
 * wrapper around objectForOffset: (the fix). That is the point:
 * on base the wrapper is absent (or disabled), so the two threads
 * race on the dictionary; with the fix the wrapper serializes
 * whole decodes.
 *
 * The test runs two phases:
 *   Phase A (unguarded, base behaviour): the lock is disabled, so
 *   the wrapper is a pass-through. Two threads decoding the same
 *   offset must both miss the cache and both decode (the race).
 *   The test expects the race; if it does not happen the test is
 *   not discriminating and fails as inconclusive.
 *   Phase B (guarded, fixed behaviour): the lock is enabled. The
 *   same two threads must decode each offset exactly once (the
 *   second thread finds it cached) and the dictionary count must
 *   be exact. Any double decode or count mismatch fails.
 *
 * Usage:
 *   heap067_dict_test           full check: A expects race, B expects none
 *   heap067_dict_test base      base only: asserts no race (FAILS: race present)
 *   heap067_dict_test fixed     fixed only: asserts no race (PASSES)
 *
 * Build (plain and sanitizers):
 *   cc -DTUIKB_LOCK_HOST_TEST -I../../libsystem_kernel/src \
 *      heap067_dict_test.c ../../libsystem_kernel/src/tuikb_lock.c -o heap067_dict_test -lpthread
 *   cc -DTUIKB_LOCK_HOST_TEST -fsanitize=address,undefined -g \
 *      -I../../libsystem_kernel/src \
 *      heap067_dict_test.c ../../libsystem_kernel/src/tuikb_lock.c -o heap067_dict_test_asan -lpthread
 *
 * No em dashes are used in this file.
 */
#define TUIKB_LOCK_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdatomic.h>

#include "tuikb_lock.h"

/* ------------------------------------------------------------------ */
/* Check accounting                                                     */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL: " __VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime (ops dlsym_fn)                                     */
/*                                                                      */
/* One fake class (TUIKBGraphSerialization) and one fake instance      */
/* method (objectForOffset:). method_setImplementation swaps the      */
/* current IMP; method_getImplementation returns it. The test's      */
/* model objectForOffset: is the original IMP; the tuikb wrapper     */
/* is installed over it by tuikb_lock_install_hooks.                  */
/* ------------------------------------------------------------------ */

static void *g_mock_class = (void *)(uintptr_t)0x1001;
static void *g_mock_sel = (void *)(uintptr_t)0x2001;
static void *g_mock_method = (void *)(uintptr_t)0x3001;
static void *g_mock_imp; /* current IMP for objectForOffset: */

static void *mock_objc_getClass(const char *name) {
    if (name && strcmp(name, "TUIKBGraphSerialization") == 0) return g_mock_class;
    return NULL;
}
static void *mock_sel_registerName(const char *name) {
    if (name && strcmp(name, "objectForOffset:") == 0) return g_mock_sel;
    return NULL;
}
static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    if (cls == g_mock_class && sel == g_mock_sel) return g_mock_method;
    return NULL;
}
static void *mock_method_setImplementation(void *m, void *imp) {
    void *old;
    if (m != g_mock_method) return NULL;
    old = g_mock_imp;
    g_mock_imp = imp;
    return old;
}
static void *mock_method_getImplementation(void *m) {
    if (m != g_mock_method) return NULL;
    return g_mock_imp;
}

static void *mock_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    if (strcmp(name, "method_getImplementation") == 0) return (void *)mock_method_getImplementation;
    return NULL;
}

static const char *mock_getenv(const char *name) {
    (void)name;
    return NULL;
}
static void mock_log(const char *msg, size_t len) {
    fwrite(msg, 1, len, stdout);
}
static long mock_tid(void) {
    return (long)syscall(SYS_gettid);
}
static long mock_futex_wait(volatile int *addr, int val) {
    return (long)syscall(SYS_futex, addr, 0 /* FUTEX_WAIT_PRIVATE */, val, NULL, NULL, 0);
}
static long mock_futex_wake(volatile int *addr, int count) {
    return (long)syscall(SYS_futex, addr, 1 /* FUTEX_WAKE_PRIVATE */, count, NULL, NULL, 0);
}

static const tuikb_ops_t g_ops = {
    mock_dlsym, mock_getenv, mock_log, mock_tid, mock_futex_wait, mock_futex_wake
};

/* ------------------------------------------------------------------ */
/* Model of the _deserializationCache + objectForOffset:                */
/*                                                                      */
/* A fixed open-addressing table mapping offset (int) to a decoded    */
/* object (a heap int holding the offset). No lock of its own.        */
/* objectForOffset: mirrors Apple's method: look the offset up,     */
/* decode on miss (busy loop, so two threads overlap), store the    */
/* result. The decode also performs one nested objectForOffset:    */
/* call (offset+1000) through the same IMP, which exercises the     */
/* recursive lock: the wrapper re-enters on the same thread and    */
/* must not deadlock.                                               */
/*                                                                      */
/* Counters: g_decode_count[offset] counts decodes per offset;     */
/* g_table_count counts entries stored. On a correct run each       */
/* offset decodes exactly once and g_table_count equals the number  */
/* of distinct offsets.                                             */
/* ------------------------------------------------------------------ */

#define MODEL_TABLE_SIZE 64
#define MODEL_MAX_OFFSET 16

static int g_table_keys[MODEL_TABLE_SIZE];
static int g_table_vals[MODEL_TABLE_SIZE];
static int g_table_used[MODEL_TABLE_SIZE];
static int g_table_count;
static int g_decode_count[MODEL_MAX_OFFSET + 1001];
static _Atomic int g_model_calls;

static void model_reset(void) {
    memset(g_table_keys, 0, sizeof(g_table_keys));
    memset(g_table_vals, 0, sizeof(g_table_vals));
    memset(g_table_used, 0, sizeof(g_table_used));
    g_table_count = 0;
    memset(g_decode_count, 0, sizeof(g_decode_count));
    atomic_store(&g_model_calls, 0);
}

static int model_lookup(int offset) {
    unsigned int h = ((unsigned int)offset * 2654435761u) % MODEL_TABLE_SIZE;
    int i;
    for (i = 0; i < MODEL_TABLE_SIZE; i++) {
        int slot = (int)((h + (unsigned int)i) % MODEL_TABLE_SIZE);
        if (!g_table_used[slot]) return -1;
        if (g_table_keys[slot] == offset) return g_table_vals[slot];
    }
    return -1;
}

static void model_store(int offset, int value) {
    unsigned int h = ((unsigned int)offset * 2654435761u) % MODEL_TABLE_SIZE;
    int i;
    for (i = 0; i < MODEL_TABLE_SIZE; i++) {
        int slot = (int)((h + (unsigned int)i) % MODEL_TABLE_SIZE);
        if (!g_table_used[slot]) {
            g_table_used[slot] = 1;
            g_table_keys[slot] = offset;
            g_table_vals[slot] = value;
            g_table_count++;
            return;
        }
        if (g_table_keys[slot] == offset) {
            g_table_vals[slot] = value;
            return;
        }
    }
}

/* The model original IMP. Signature matches the real method:
 * (self, _cmd, cursor) -> id, where cursor points at the offset
 * int (the graph reader state). The nested call for offset+1000
 * goes through the current IMP (the wrapper once installed), so
 * it re-enters the lock on the same thread. */
static void *model_objectForOffset(void *self, void *sel, void *cursor) {
    int offset;
    int found;
    int value;
    void *(*imp)(void *, void *, void *);
    (void)self;
    (void)sel;
    offset = *(int *)cursor;
    atomic_fetch_add(&g_model_calls, 1);
    found = model_lookup(offset);
    if (found >= 0) return (void *)(uintptr_t)(found + 1);
    /* Miss. Yield so the other thread also reaches its lookup
     * before this thread stores: on a single core (or under a
     * sanitizer) the two threads would otherwise run strictly
     * sequentially and never overlap, which makes the race
     * invisible. The yield is what makes the race deterministic
     * in the test; the real decode overlaps because it is slow. */
    sched_yield();
    /* Miss: decode. The busy loop is the slow graph decode; it is
     * what lets two threads overlap and both miss the cache. */
    {
        volatile unsigned int sink = 0;
        int k;
        for (k = 0; k < 200000; k++) sink += (unsigned int)k;
        (void)sink;
    }
    sched_yield();
    g_decode_count[offset]++;
    /* One nested decode (a child offset), through the current IMP.
     * This is the deep recursion in the real decode
     * (objectForOffset -> relativeRefArrayForOffset ->
     * objectForOffset ...). It must not deadlock the wrapper. */
    if (offset < MODEL_MAX_OFFSET) {
        int nested = offset + 1000;
        imp = (void *(*)(void *, void *, void *))g_mock_imp;
        if (imp) imp(self, sel, &nested);
    }
    value = offset * 10 + 1;
    model_store(offset, value);
    return (void *)(uintptr_t)(value + 1);
}

/* ------------------------------------------------------------------ */
/* Two-thread driver                                                    */
/* ------------------------------------------------------------------ */

#define NTHREADS 2
#define NROUNDS 20

typedef struct {
    pthread_barrier_t *barrier;
    int offset;
    int rounds;
} worker_arg_t;

static void *worker_fn(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    void *(*imp)(void *, void *, void *);
    int r;
    imp = (void *(*)(void *, void *, void *))tuikb_lock_test_imp();
    for (r = 0; r < w->rounds; r++) {
        int cursor = w->offset;
        /* Start both threads together so they overlap in the
         * decode. */
        pthread_barrier_wait(w->barrier);
        if (imp) imp(NULL, NULL, &cursor);
    }
    return NULL;
}

/* Run one phase: two threads decode the same offset NROUNDS times.
 * Returns the number of decodes of that offset and the table
 * count. A correct (serialized) run decodes the offset exactly
 * once (first round) and the nested offset exactly once. */
static void run_phase(int offset, int *out_decodes, int *out_table_count,
                      int *out_model_calls) {
    pthread_t th[NTHREADS];
    pthread_barrier_t barrier;
    worker_arg_t w;
    int i;
    pthread_barrier_init(&barrier, NULL, NTHREADS);
    w.barrier = &barrier;
    w.offset = offset;
    w.rounds = NROUNDS;
    for (i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], NULL, worker_fn, &w) != 0) {
            printf("FAIL: pthread_create failed\n");
            g_failures++;
            return;
        }
    }
    for (i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    pthread_barrier_destroy(&barrier);
    *out_decodes = g_decode_count[offset];
    *out_table_count = g_table_count;
    *out_model_calls = atomic_load(&g_model_calls);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int mode_base_only = 0;
    int mode_fixed_only = 0;
    int decodes_a = 0, count_a = 0, calls_a = 0;
    int decodes_b = 0, count_b = 0, calls_b = 0;
    tuikb_lock_stats_t st;
    if (argc > 1 && strcmp(argv[1], "base") == 0) mode_base_only = 1;
    if (argc > 1 && strcmp(argv[1], "fixed") == 0) mode_fixed_only = 1;

    tuikb_lock_test_configure(&g_ops);
    /* Install the model as the original IMP, then install the
     * wrapper over it (the fix). */
    g_mock_imp = (void *)model_objectForOffset;
    CHECK(tuikb_lock_install_hooks() == 0, "install hooks");
    CHECK(tuikb_lock_test_imp() != NULL, "wrapper IMP installed");
    CHECK(tuikb_lock_test_imp() != (void *)model_objectForOffset,
          "wrapper IMP differs from the original");

    if (!mode_fixed_only) {
        /* Phase A: unguarded (base behaviour). The lock is
         * disabled, so the wrapper passes through to the model
         * with no serialization. Two threads decoding the same
         * offset must both miss and both decode. */
        tuikb_lock_test_set_enabled(0);
        model_reset();
        run_phase(7, &decodes_a, &count_a, &calls_a);
        printf("phase A (unguarded/base): offset 7 decoded %d times, "
               "table count %d, model calls %d\n",
               decodes_a, count_a, calls_a);
        if (mode_base_only) {
            /* Base-only run: assert the correct (serialized)
             * outcome. It must fail here, because the race is
             * present on base. */
            CHECK(decodes_a == 1, "base: offset 7 decoded exactly once (got %d)", decodes_a);
            CHECK(count_a == 2, "base: table count is 2 (offset + nested, got %d)", count_a);
            printf("checks=%d failures=%d\n", g_checks, g_failures);
            return g_failures ? 1 : 0;
        }
        /* Full run: Phase A must show the race, or the test
         * cannot discriminate. */
        CHECK(decodes_a > 1, "phase A shows the race (offset 7 decoded %d times, want >1)", decodes_a);
    }

    if (!mode_base_only) {
        /* Phase B: guarded (fixed behaviour). The lock is enabled.
         * The same two threads must now decode offset 7 exactly
         * once (the second thread finds it cached) and the table
         * must hold exactly the offset and its nested child. */
        tuikb_lock_test_set_enabled(1);
        model_reset();
        run_phase(7, &decodes_b, &count_b, &calls_b);
        printf("phase B (guarded/fixed): offset 7 decoded %d times, "
               "table count %d, model calls %d\n",
               decodes_b, count_b, calls_b);
        CHECK(decodes_b == 1, "phase B: offset 7 decoded exactly once (got %d)", decodes_b);
        CHECK(count_b == 2, "phase B: table count is 2 (offset + nested, got %d)", count_b);
        /* The nested decode must have run (recursion reached the
         * wrapper) and must not have deadlocked. Model calls in
         * phase B are exactly 41: round 1, the decoding thread
         * makes 2 calls (offset + nested) and the other thread
         * makes 1 (a hit); rounds 2..20, both threads hit, 2 calls
         * per round * 19 = 38. Total 3 + 38 = 41. The nested call
         * happens only on a miss, so it is not 80. */
        CHECK(calls_b == 41,
              "phase B: model calls %d (want 41)", calls_b);
        tuikb_lock_stats(&st);
        printf("stats: installs=%llu calls=%llu recursive=%llu contended=%llu max_depth=%llu\n",
               (unsigned long long)st.installs, (unsigned long long)st.calls,
               (unsigned long long)st.recursive, (unsigned long long)st.contended,
               (unsigned long long)st.max_depth);
        CHECK(st.calls > 0, "phase B: wrapper was entered");
        CHECK(st.recursive > 0, "phase B: nested (recursive) entries observed");
        CHECK(st.max_depth >= 2, "phase B: max depth >= 2 (got %llu)", (unsigned long long)st.max_depth);
    }

    printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
