/*
 * heap067_lock_test.c - Host test for locks.
 *
 * Covers the ulock / psynch primitives in libsystem_kernel/src/thread.c:
 *
 *   H2 (PRIMARY, fails on base, passes with the fix): ulock_wait ignores
 *   the operation and always does a 32-bit FUTEX_WAIT_PRIVATE, even for
 *   UL_COMPARE_AND_WAIT64 (op 2). libdispatch's dispatch_once waits
 *   exactly that way, on a 64-bit thread-pointer predicate. If the waker
 *   stores a new 64-bit value whose LOW 32 bits equal the waited value's
 *   low 32 bits, the 32-bit entry check still passes, so a waiter that
 *   calls ulock_wait AFTER the value changed and the wake already fired
 *   sleeps forever (lost wakeup). The test forces that ordering
 *   deterministically with two flags: the waiter calls ulock_wait only
 *   after the main thread has already stored the new value and woken.
 *   On base the waiter sleeps until its 500 ms timeout and returns
 *   -60 (ETIMEDOUT); with the fix it sees the 64-bit value already
 *   changed and returns at once.
 *
 *   Controls (pass on base and with the fix):
 *   - low32 differs: the 32-bit entry check fails, so even the base
 *     implementation returns at once.
 *   - normal wake: the waiter is already asleep when the value changes
 *     and the wake fires, so it wakes normally on both.
 *   - 8 waiters on one predicate, same low32: all must return.
 *
 *   Hammer (mutual exclusion, passes on base and with the fix;
 *   regression guard for the 32-bit path):
 *   - os_unfair_lock-style lock built on ulock_wait/wake (32-bit):
 *     8 threads x 100k iterations, counter must be exactly 800k and
 *     at most one thread inside the critical section at a time.
 *
 *   Sanity (thread.c was touched, so the neighbour must still work):
 *   - psynch mutex fairshare blocks until drop, then grants.
 *   The rwlock neighbour is covered by the existing
 *   psynch_rwlock_test.c (11 scenarios, 312 checks); this file does
 *   not re-drive it.
 *
 * Host note: the host shim (tests/host/host-shim/linux.h) returns
 * libc-style -1 from the futex syscall instead of the raw -errno the
 * target svc returns. The patched ulock code normalizes ret == -1 via
 * errno under THREAD_HOST_TEST (same pattern as the rwlock slow path),
 * so the host test sees the same return values as the target. The
 * base tree has no such normalization in ulock_wait, so on the host
 * the base returns -1 for EAGAIN/ETIMEDOUT; the H2 assertions below
 * still fail on base (the waiter sleeps instead of returning 1) and
 * pass with the fix.
 *
 * Build (mirrors tests/host/psynch_mutex_test.c):
 *   cc -DTHREAD_HOST_TEST=1 -O1 -g -fsanitize=address,undefined \
 *      tests/heap067_lock_test.c libsystem_kernel/src/thread.c -lpthread
 * The test uses real host pthreads and real futex syscalls via the
 * host-shim linux.h. Waiter threads are detached; the main thread polls
 * their return flags with a deadline, so a stuck waiter fails the test
 * instead of hanging the suite.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Declarations from libsystem_kernel/src/thread.c (compiled with -DTHREAD_HOST_TEST). */
int lsk_ulock_wait(uint32_t op, void *addr, uint64_t val, uint32_t timeout_us)
    __asm__("___ulock_wait");
int lsk_ulock_wake(uint32_t op, void *addr, uint64_t wake_val)
    __asm__("___ulock_wake");
int lsk_psynch_mutexwait(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags)
    __asm__("___psynch_mutexwait");
int lsk_psynch_mutexdrop(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags)
    __asm__("___psynch_mutexdrop");

/* ---- Stubs required by thread.c (same set as thread_reg_test.c) ---- */
int g_ipc_trace = 0;
static int g_last_errno = 0;
void set_errno(int err) { g_last_errno = err; }
int linux_to_darwin_errno(long linux_err) { return (int)(linux_err < 0 ? -linux_err : linux_err); }

static uint32_t g_next_kport = 0x1003;
uint32_t ipc_port_allocate_thread(void) {
    return __atomic_fetch_add(&g_next_kport, 4, __ATOMIC_RELAXED);
}

long thread_test_gettid(void) {
    return 2000;
}

long __iosw_raw_clone(unsigned long flags, void *newsp, int *parent_tid, unsigned long tls, int *child_tid) {
    (void)flags; (void)newsp; (void)parent_tid; (void)tls; (void)child_tid;
    return 3000;
}

int thread_test_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oset) {
    (void)how; (void)set; (void)oset;
    return 0;
}

long (*g_thread_test_write_hook)(int fd, const void *buf, size_t len) = NULL;

/* ---- Test harness ---- */
static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define UL_COMPARE_AND_WAIT 1u
#define UL_COMPARE_AND_WAIT64 2u
#define ULF_WAKE_ALL 0x100u

/* Poll until *flag is set, or the deadline (ms) passes. Returns 1 if set. */
static int wait_for_flag(volatile atomic_int *flag, int deadline_ms) {
    int waited = 0;
    while (!atomic_load(flag) && waited < deadline_ms) {
        usleep(1000);
        waited += 1;
    }
    return atomic_load(flag) != 0;
}

/* ================= H2: 64-bit ulock lost wakeup ================= */

/* One 64-bit predicate, one waiter. The main thread stores the new value
 * (same low32, different high32) and wakes BEFORE the waiter calls
 * ulock_wait. Correct behaviour: the waiter sees the value already
 * changed and returns at once (ret 1). Base behaviour: the 32-bit futex
 * entry check passes (low32 matches), so the waiter sleeps until its
 * timeout and returns -60. */
struct h2_arg {
    volatile uint64_t *pred;
    uint64_t wait_val;
    uint32_t timeout_us;
    atomic_int ready;
    atomic_int go;
    atomic_int done;
    int retval;
};

static void *h2_waiter(void *arg) {
    struct h2_arg *a = (struct h2_arg *)arg;
    atomic_store(&a->ready, 1);
    while (!atomic_load(&a->go)) usleep(200);
    a->retval = lsk_ulock_wait(UL_COMPARE_AND_WAIT64, (void *)a->pred,
                               a->wait_val, a->timeout_us);
    atomic_store(&a->done, 1);
    return NULL;
}

static void test_h2_lost_wakeup(void) {
    volatile uint64_t pred;
    /* V1 and V2 share the low 32 bits (0x00000001) and differ in the
     * high 32 bits, exactly like a dispatch_once predicate going from
     * one thread-pointer RUNNING value to the DONE value. */
    uint64_t v1 = 0x0000000100000001ULL;
    uint64_t v2 = 0x0000000200000001ULL;
    struct h2_arg a;
    memset(&a, 0, sizeof(a));
    pred = v1;
    a.pred = &pred;
    a.wait_val = v1;
    a.timeout_us = 500000; /* 500 ms: long enough to prove a sleep, short enough to keep the suite fast */
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);
    atomic_init(&a.done, 0);
    a.retval = -999;
    pthread_t th;
    assert(pthread_create(&th, NULL, h2_waiter, &a) == 0);
    assert(pthread_detach(th) == 0);
    CHECK(wait_for_flag(&a.ready, 5000));
    /* Main thread: change the value, then wake, then let the waiter in. */
    __atomic_store_n(&pred, v2, __ATOMIC_RELEASE);
    lsk_ulock_wake(UL_COMPARE_AND_WAIT64 | ULF_WAKE_ALL, (void *)&pred, 0);
    atomic_store(&a.go, 1);
    CHECK(wait_for_flag(&a.done, 5000));
    /* The waiter must return 1 (value already changed), not -60. */
    CHECK(a.retval == 1);
}

/* Control: low32 differs, so even the 32-bit entry check fails and the
 * base implementation returns at once. Must pass on base and fixed. */
static void test_h2_low32_differs(void) {
    volatile uint64_t pred;
    uint64_t v1 = 0x0000000100000001ULL;
    uint64_t v3 = 0x0000000100000002ULL; /* low32 differs */
    struct h2_arg a;
    memset(&a, 0, sizeof(a));
    pred = v1;
    a.pred = &pred;
    a.wait_val = v1;
    a.timeout_us = 500000;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);
    atomic_init(&a.done, 0);
    a.retval = -999;
    pthread_t th;
    assert(pthread_create(&th, NULL, h2_waiter, &a) == 0);
    assert(pthread_detach(th) == 0);
    CHECK(wait_for_flag(&a.ready, 5000));
    __atomic_store_n(&pred, v3, __ATOMIC_RELEASE);
    lsk_ulock_wake(UL_COMPARE_AND_WAIT64 | ULF_WAKE_ALL, (void *)&pred, 0);
    atomic_store(&a.go, 1);
    CHECK(wait_for_flag(&a.done, 5000));
    CHECK(a.retval == 1);
}

/* Control: the waiter is already asleep when the value changes and the
 * wake fires. Must pass on base and fixed (the wake lands while the
 * waiter is in the futex). */
static void test_h2_normal_wake(void) {
    volatile uint64_t pred;
    uint64_t v1 = 0x0000000100000001ULL;
    uint64_t v2 = 0x0000000200000001ULL;
    struct h2_arg a;
    memset(&a, 0, sizeof(a));
    pred = v1;
    a.pred = &pred;
    a.wait_val = v1;
    a.timeout_us = 0; /* wait forever; the wake must arrive */
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 1); /* go at once: the waiter enters ulock_wait immediately */
    atomic_init(&a.done, 0);
    a.retval = -999;
    pthread_t th;
    assert(pthread_create(&th, NULL, h2_waiter, &a) == 0);
    assert(pthread_detach(th) == 0);
    usleep(200000); /* let the waiter enter the futex */
    __atomic_store_n(&pred, v2, __ATOMIC_RELEASE);
    lsk_ulock_wake(UL_COMPARE_AND_WAIT64 | ULF_WAKE_ALL, (void *)&pred, 0);
    CHECK(wait_for_flag(&a.done, 5000));
    CHECK(a.retval == 1);
}

/* 8 waiters on one predicate, same low32. All must return once the
 * value changes and the wake-all fires, even when they call ulock_wait
 * after the change. On base they all sleep until timeout. */
#define H2_NWAITERS 8
struct h2_multi {
    volatile uint64_t *pred;
    uint64_t wait_val;
    atomic_int ready_count;
    atomic_int go;
    atomic_int done_count;
    int retval[H2_NWAITERS];
};

static void *h2_multi_waiter(void *arg) {
    struct h2_multi *m = (struct h2_multi *)arg;
    int idx = (int)__atomic_fetch_add(&m->ready_count, 1, __ATOMIC_RELAXED);
    while (!atomic_load(&m->go)) usleep(200);
    m->retval[idx] = lsk_ulock_wait(UL_COMPARE_AND_WAIT64, (void *)m->pred,
                                    m->wait_val, 500000);
    __atomic_fetch_add(&m->done_count, 1, __ATOMIC_RELAXED);
    return NULL;
}

static void test_h2_eight_waiters(void) {
    volatile uint64_t pred;
    uint64_t v1 = 0x0000000100000001ULL;
    uint64_t v2 = 0x0000000200000001ULL;
    struct h2_multi m;
    memset(&m, 0, sizeof(m));
    pred = v1;
    m.pred = &pred;
    m.wait_val = v1;
    atomic_init(&m.ready_count, 0);
    atomic_init(&m.go, 0);
    atomic_init(&m.done_count, 0);
    pthread_t th[H2_NWAITERS];
    for (int i = 0; i < H2_NWAITERS; i++) {
        assert(pthread_create(&th[i], NULL, h2_multi_waiter, &m) == 0);
        assert(pthread_detach(th[i]) == 0);
    }
    int waited = 0;
    while (atomic_load(&m.ready_count) < H2_NWAITERS && waited < 5000) {
        usleep(1000);
        waited++;
    }
    CHECK(atomic_load(&m.ready_count) == H2_NWAITERS);
    __atomic_store_n(&pred, v2, __ATOMIC_RELEASE);
    lsk_ulock_wake(UL_COMPARE_AND_WAIT64 | ULF_WAKE_ALL, (void *)&pred, 0);
    atomic_store(&m.go, 1);
    waited = 0;
    while (atomic_load(&m.done_count) < H2_NWAITERS && waited < 5000) {
        usleep(1000);
        waited++;
    }
    CHECK(atomic_load(&m.done_count) == H2_NWAITERS);
    for (int i = 0; i < H2_NWAITERS; i++) {
        CHECK(m.retval[i] == 1);
    }
}

/* ================= Hammer: os_unfair_lock-style 32-bit lock ================= */

typedef struct { volatile uint32_t v; } my_ulock_t;

static void my_ulock_lock(my_ulock_t *l) {
    for (;;) {
        uint32_t prev = __atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE);
        if (prev == 0) return;
        /* Contended: sleep until the value changes, then retry the CAS.
         * The lock word is 32-bit, so the 32-bit wait is correct here. */
        lsk_ulock_wait(UL_COMPARE_AND_WAIT, (void *)&l->v, 1, 0);
    }
}

static void my_ulock_unlock(my_ulock_t *l) {
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
    lsk_ulock_wake(UL_COMPARE_AND_WAIT, (void *)&l->v, 0);
}

#define HAMMER_THREADS 8
#define HAMMER_ITERS 100000
static my_ulock_t g_hammer_lock = { 0 };
static volatile long g_hammer_counter = 0;
static volatile int g_hammer_inside = 0;
static volatile int g_hammer_max_inside = 0;

static void *hammer_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < HAMMER_ITERS; i++) {
        my_ulock_lock(&g_hammer_lock);
        int cur = __atomic_add_fetch(&g_hammer_inside, 1, __ATOMIC_RELAXED);
        int prev_max;
        do {
            prev_max = __atomic_load_n(&g_hammer_max_inside, __ATOMIC_RELAXED);
            if (cur <= prev_max) break;
        } while (!__atomic_compare_exchange_n(&g_hammer_max_inside, &prev_max, cur,
                                              0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
        __atomic_add_fetch(&g_hammer_counter, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&g_hammer_inside, 1, __ATOMIC_RELAXED);
        my_ulock_unlock(&g_hammer_lock);
    }
    return NULL;
}

static void test_ulock_hammer(void) {
    pthread_t th[HAMMER_THREADS];
    for (int i = 0; i < HAMMER_THREADS; i++) {
        assert(pthread_create(&th[i], NULL, hammer_thread, NULL) == 0);
    }
    for (int i = 0; i < HAMMER_THREADS; i++) {
        assert(pthread_join(th[i], NULL) == 0);
    }
    CHECK(g_hammer_counter == (long)HAMMER_THREADS * HAMMER_ITERS);
    CHECK(g_hammer_max_inside == 1);
}

/* ================= Sanity: psynch mutex still works ================= */

#define POLICY_FAIRSHARE 0x040u
#define MTX_KBIT 0x1u
#define MTX_EBIT 0x2u

struct mtx_waiter_arg {
    void *mutex;
    uint32_t mgen;
    atomic_int returned;
    int retval;
};

static void *mtx_waiter_thread(void *arg) {
    struct mtx_waiter_arg *a = (struct mtx_waiter_arg *)arg;
    a->retval = lsk_psynch_mutexwait(a->mutex, a->mgen, 0, 1234, POLICY_FAIRSHARE);
    atomic_store(&a->returned, 1);
    return NULL;
}

static void test_mutex_sanity(void) {
    static uint64_t mtx[8];
    struct mtx_waiter_arg w;
    memset(&w, 0, sizeof(w));
    w.mutex = mtx;
    w.mgen = 0x200;
    atomic_init(&w.returned, 0);
    w.retval = -999;
    pthread_t th;
    assert(pthread_create(&th, NULL, mtx_waiter_thread, &w) == 0);
    assert(pthread_detach(th) == 0);
    usleep(150000);
    CHECK(!atomic_load(&w.returned));
    int rc = lsk_psynch_mutexdrop(mtx, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_flag(&w.returned, 5000));
    CHECK(w.retval == (int)(0x200 | MTX_KBIT | MTX_EBIT));
}

int main(void) {
    test_h2_lost_wakeup();
    test_h2_low32_differs();
    test_h2_normal_wake();
    test_h2_eight_waiters();
    test_ulock_hammer();
    test_mutex_sanity();
    printf("heap067_lock_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
