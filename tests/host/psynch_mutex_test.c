#define _DEFAULT_SOURCE
/*
 * psynch_mutex_test.c - Host unit test for the psynch mutex protocol in
 * libsystem_kernel/src/thread.c.
 *
 * Exercises lsk_psynch_mutexwait / lsk_psynch_mutexdrop, the in-process
 * implementation of the XNU psynch mutex protocol from
 * apple-oss-distributions/libpthread kern/kern_synch.c:
 *   1. A fairshare waiter blocks until the drop grants it (the
 *      regression: the old no-op returned at once, so a contender
 *      was treated as granted while the mutex was still held, and
 *      two threads could own the same mutex).
 *   2. A firstfit waiter blocks until the drop.
 *   3. Prepost consumption: a drop with no waiters records a prepost,
 *      and a later wait returns at once. Fairshare consumes it only
 *      for the matching seq; a non-matching waiter keeps blocking.
 *   4. Fairshare ordering: the drop wakes the waiter whose mgen is
 *      nextgen, not a later one; a second drop wakes the second.
 *   5. Two mutexes are independent (a drop on one does not wake a
 *      waiter on the other).
 *
 * The tests use real host pthreads and the real futex syscalls (via
 * the host-shim linux.h). Waiter threads are detached; the main
 * thread polls their return flags with a deadline, so a stuck waiter
 * fails the test instead of hanging the suite.
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

#define POLICY_FAIRSHARE 0x040u
#define POLICY_FIRSTFIT 0x080u
#define MTX_KBIT 0x1u
#define MTX_EBIT 0x2u

struct waiter_arg {
    void *mutex;
    uint32_t mgen;
    uint32_t ugen;
    uint64_t tid;
    uint32_t flags;
    atomic_int returned;
    int retval;
};

static void *waiter_thread(void *arg) {
    struct waiter_arg *a = (struct waiter_arg *)arg;
    a->retval = lsk_psynch_mutexwait(a->mutex, a->mgen, a->ugen, a->tid, a->flags);
    atomic_store(&a->returned, 1);
    return NULL;
}

static void start_waiter(struct waiter_arg *a, void *mutex, uint32_t mgen, uint32_t flags) {
    memset(a, 0, sizeof(*a));
    a->mutex = mutex;
    a->mgen = mgen;
    a->ugen = 0;
    a->tid = 1234;
    a->flags = flags;
    atomic_init(&a->returned, 0);
    a->retval = -999;
    pthread_t th;
    int rc = pthread_create(&th, NULL, waiter_thread, a);
    assert(rc == 0);
    rc = pthread_detach(th);
    assert(rc == 0);
}

/* Poll until the waiter has returned, or the deadline (ms) passes. */
static int wait_for_return(struct waiter_arg *a, int deadline_ms) {
    int waited = 0;
    while (!atomic_load(&a->returned) && waited < deadline_ms) {
        usleep(5000);
        waited += 5;
    }
    return atomic_load(&a->returned);
}

/* Distinct fake mutex addresses, one per test, so no table entry leaks
 * between tests (entries are keyed by address). */
static uint64_t g_fake_mtx_a[8];
static uint64_t g_fake_mtx_b[8];
static uint64_t g_fake_mtx_c[8];
static uint64_t g_fake_mtx_d[8];
static uint64_t g_fake_mtx_e[8];

/* 1. Fairshare waiter blocks until the drop. The regression: with the
 * old no-op mutexwait the waiter returned at once, so libpthread
 * treated it as granted while the mutex was still held. */
static void test_fairshare_blocks_until_drop(void) {
    struct waiter_arg w;
    /* Waiter wants seq 0x200; the drop's ugen 0x100 gives nextgen 0x200. */
    start_waiter(&w, g_fake_mtx_a, 0x200, POLICY_FAIRSHARE);
    usleep(150000);
    CHECK(!atomic_load(&w.returned));
    int rc = lsk_psynch_mutexdrop(g_fake_mtx_a, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w, 5000));
    CHECK(w.retval == (int)(0x200 | MTX_KBIT | MTX_EBIT));
}

/* 2. Firstfit waiter blocks until the drop. */
static void test_firstfit_blocks_until_drop(void) {
    struct waiter_arg w;
    start_waiter(&w, g_fake_mtx_b, 0x200, POLICY_FIRSTFIT);
    usleep(150000);
    CHECK(!atomic_load(&w.returned));
    int rc = lsk_psynch_mutexdrop(g_fake_mtx_b, 0x200, 0x100, 0, POLICY_FIRSTFIT);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w, 5000));
    CHECK(w.retval == (int)(0x200 | MTX_KBIT | MTX_EBIT));
}

/* 3. Prepost: a drop with no waiters records one, and a later wait
 * returns at once. Fairshare consumes it only for the matching seq. */
static void test_fairshare_prepost(void) {
    /* Drop first (no waiters): prepost for nextgen 0x200. */
    int rc = lsk_psynch_mutexdrop(g_fake_mtx_c, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    /* A waiter with a non-matching seq must NOT consume it. */
    struct waiter_arg w_bad;
    start_waiter(&w_bad, g_fake_mtx_c, 0x300, POLICY_FAIRSHARE);
    usleep(150000);
    CHECK(!atomic_load(&w_bad.returned));
    /* Drop for the non-matching waiter: nextgen 0x300 wakes it. */
    rc = lsk_psynch_mutexdrop(g_fake_mtx_c, 0x300, 0x200, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w_bad, 5000));
    CHECK(w_bad.retval == (int)(0x300 | MTX_KBIT | MTX_EBIT));
    /* The original prepost (lseq 0x200) is still pending: a matching
     * waiter consumes it at once. */
    int r = lsk_psynch_mutexwait(g_fake_mtx_c, 0x200, 0x100, 1234, POLICY_FAIRSHARE);
    CHECK(r == (int)(0x200 | MTX_KBIT | MTX_EBIT));
}

/* 3b. Firstfit prepost: any seq consumes it. */
static void test_firstfit_prepost(void) {
    int rc = lsk_psynch_mutexdrop(g_fake_mtx_d, 0x200, 0x100, 0, POLICY_FIRSTFIT);
    CHECK(rc == 0);
    int r = lsk_psynch_mutexwait(g_fake_mtx_d, 0x777700, 0, 1234, POLICY_FIRSTFIT);
    CHECK(r == (int)((0x777700 & 0xffffff00u) | MTX_KBIT | MTX_EBIT));
}

/* 4. Fairshare ordering: the drop wakes the waiter whose mgen is
 * nextgen, not a later one; a second drop wakes the second. */
static void test_fairshare_ordering(void) {
    struct waiter_arg w1, w2;
    start_waiter(&w1, g_fake_mtx_e, 0x200, POLICY_FAIRSHARE);
    start_waiter(&w2, g_fake_mtx_e, 0x300, POLICY_FAIRSHARE);
    usleep(200000); /* let both enqueue */
    int rc = lsk_psynch_mutexdrop(g_fake_mtx_e, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w1, 5000));
    CHECK(w1.retval == (int)(0x200 | MTX_KBIT | MTX_EBIT));
    usleep(100000);
    CHECK(!atomic_load(&w2.returned));
    rc = lsk_psynch_mutexdrop(g_fake_mtx_e, 0x300, 0x200, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w2, 5000));
    CHECK(w2.retval == (int)(0x300 | MTX_KBIT | MTX_EBIT));
}

/* 5. Two mutexes are independent. */
static void test_two_mutexes_independent(void) {
    static uint64_t mtx_f[8];
    static uint64_t mtx_g[8];
    struct waiter_arg w;
    start_waiter(&w, mtx_f, 0x200, POLICY_FAIRSHARE);
    usleep(150000);
    CHECK(!atomic_load(&w.returned));
    /* A drop on a different mutex must not wake it. */
    int rc = lsk_psynch_mutexdrop(mtx_g, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    usleep(100000);
    CHECK(!atomic_load(&w.returned));
    rc = lsk_psynch_mutexdrop(mtx_f, 0x200, 0x100, 0, POLICY_FAIRSHARE);
    CHECK(rc == 0);
    CHECK(wait_for_return(&w, 5000));
    CHECK(w.retval == (int)(0x200 | MTX_KBIT | MTX_EBIT));
}

int main(void) {
    test_fairshare_blocks_until_drop();
    test_firstfit_blocks_until_drop();
    test_fairshare_prepost();
    test_firstfit_prepost();
    test_fairshare_ordering();
    test_two_mutexes_independent();
    printf("psynch_mutex_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
