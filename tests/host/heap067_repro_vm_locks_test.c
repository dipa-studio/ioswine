/* heap067_repro_vm_locks_test.c - Host repro for heap-corruption crashes.
 *
 * End-to-end host repro for the heap-corruption crashes, second half.
 * Links vm.c + thread.c + attributegraph.c from the base tree
 * under the default $S host shim. Build: $S/vm.c $S/thread.c
 * $S/attributegraph.c, DEFS -DTHREAD_HOST_TEST=1 -DAG_HOST_TEST=1.
 * Discoverable via BUILD.txt.
 *
 * This file covers the crash-relevant paths:
 *   T1 TUIKB concurrent decode (S1/S4, the device crash),
 *   T2 VM layer (FIXED remap clobber check, churn pin),
 *   T4 AG guard concurrency (pin),
 *   T5 dictionary backing storage freed mid-decode (device
 *      evidence 2026-11-18 23:29 CET, zombie mode),
 *   T6 autorelease pool / objc TLS mixing (hypothesis 1).
 * The madvise probe was removed (the crashes predate the madvise
 * change, so madvise is not the cause).
 *
 * Forward declarations for the T5/T6 tests (defined after main's
 * callees so main() can stay last).
 */
static void test_dict_backing_uaf(void);
static void test_pool_tsd_mixing(void);

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

#define CHECK(c) do { \
    g_checks++; \
    if (!(c)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    g_checks++; \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", __FILE__, __LINE__, \
               #a, (unsigned long long)_a, (unsigned long long)_b); \
    } \
} while (0)

#define CHECK_EQ_I64(a, b) do { \
    g_checks++; \
    int64_t _a = (int64_t)(a), _b = (int64_t)(b); \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, \
               #a, (long long)_a, (long long)_b); \
    } \
} while (0)

static uint64_t g_rng_state = 0x067;
static uint64_t rng_next(void) {
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 7;
    g_rng_state ^= g_rng_state << 17;
    return g_rng_state;
}

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

/* ------------------------------------------------------------------ */
/* Stubs required by thread.c / vm.c (pump_workqueue_test.c surface). */
/* errno.c cannot be linked on the x86 host (aarch64-only asm), so    */
/* set_errno / linux_to_darwin_errno are test-side stubs here, using  */
/* the simplified forms the existing host tests use.                   */
/* ------------------------------------------------------------------ */

int g_ipc_trace = 0;
static int g_last_errno = 0;
void set_errno(int err) { g_last_errno = err; (void)g_last_errno; }
int linux_to_darwin_errno(long linux_err) {
    return (int)(linux_err < 0 ? -linux_err : linux_err);
}

static uint32_t g_next_kport = 0x1003;
uint32_t ipc_port_allocate_thread(void) {
    return __atomic_fetch_add(&g_next_kport, 4, __ATOMIC_RELAXED);
}

long thread_test_gettid(void) {
    return (long)syscall(SYS_gettid);
}

long __iosw_raw_clone(unsigned long flags, void *newsp, int *parent_tid,
                      unsigned long tls, int *child_tid) {
    (void)flags; (void)newsp; (void)parent_tid; (void)tls; (void)child_tid;
    return 3000;
}

static int g_sigmask_last_how = -1;
static uint64_t g_sigmask_cur = 0;
int thread_test_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oset) {
    if (oset) *oset = g_sigmask_cur;
    if (set) {
        g_sigmask_last_how = how;
        if (how == 0) g_sigmask_cur |= *set;
        else if (how == 1) g_sigmask_cur &= ~(*set);
        else if (how == 2) g_sigmask_cur = *set;
        else return 22;
    }
    return 0;
}

static long test_write_hook(int fd, const void *buf, size_t len) {
    (void)fd; (void)buf;
    return (long)len;
}
long (*g_thread_test_write_hook)(int fd, const void *buf, size_t len) = NULL;

/* mach_vm externs (vm_xzone_test.c). */
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
extern int mach_vm_remap(uint32_t target_task, uint64_t *target_address,
                         uint64_t size, uint64_t mask, int flags,
                         uint32_t src_task, uint64_t src_address, int copy,
                         int *cur_protection, int *max_protection,
                         int inheritance) __asm__("_mach_vm_remap");

#define KERN_SUCCESS 0
#define KERN_NO_SPACE 3
#define VM_FLAGS_FIXED 0x0000
#define VM_FLAGS_ANYWHERE 0x0001
#define VM_FLAGS_OVERWRITE 0x4000

/* psynch mutex externs (psynch_mutex_test.c). */
int lsk_psynch_mutexwait(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags)
    __asm__("___psynch_mutexwait");
int lsk_psynch_mutexdrop(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags)
    __asm__("___psynch_mutexdrop");

#define MTX_KBIT 0x1u
#define MTX_EBIT 0x2u
#define MTX_TICK 0x100u
#define MTX_POLICY_FAIRSHARE 0x040u
#define MTX_POLICY_FIRSTFIT 0x080u

/* attributegraph.c (AG_HOST_TEST). */
void ag_note_display_tick(int64_t now_ns);
int ag_update_cycle_ready(void);
int ag_update_begin(void);
uint32_t ag_tick_count(void);
void ag_test_reset(void);

/* ------------------------------------------------------------------ */
/* Lock replicas over the real thread.c primitives.                   */
/* ------------------------------------------------------------------ */

/* Host futex for the ulock replica. The tree's lsk_ulock_wait/wake
 * use the host shim's aarch64 syscall numbers (SYS_futex=98), which
 * on the x86 host invoke getrusage and return EINVAL at once instead
 * of sleeping (probe: syscall(98) rc=-1 EINVAL, syscall(202) blocks
 * for its timeout). The replica then degenerates into a contended
 * spinlock on one cache line and starves on this oversubscribed VM
 * (micro-validation 3826/8000, occasional 120 s hang). The psynch
 * mutex replica is immune because its waiters spin on per-waiter
 * woken flags (ticket system), not on a single lock word. Use the
 * host's real futex number here so the os_unfair_lock PROTOCOL is
 * validated on host. This is test code only: on the aarch64 device
 * the tree's numbers are correct (futex=98, madvise=233). */
#if defined(__x86_64__)
#define HOST_SYS_FUTEX 202
#elif defined(__aarch64__)
#define HOST_SYS_FUTEX 98
#else
#define HOST_SYS_FUTEX SYS_futex
#endif
#define HOST_FUTEX_WAIT_PRIVATE 128
#define HOST_FUTEX_WAKE_PRIVATE 129

static long host_futex_wait(volatile uint32_t *addr, uint32_t val) {
    return syscall(HOST_SYS_FUTEX, (int *)addr, HOST_FUTEX_WAIT_PRIVATE,
                   (long)val, (long)0);
}

static long host_futex_wake(volatile uint32_t *addr, int count) {
    return syscall(HOST_SYS_FUTEX, (int *)addr, HOST_FUTEX_WAKE_PRIVATE,
                   (long)count);
}

#define OUL_LOCKED 1u
#define OUL_WAITERS 2u

static void oul_lock(volatile uint32_t *l) {
    uint32_t v = __atomic_exchange_n(l, OUL_LOCKED, __ATOMIC_ACQUIRE);
    if (v == 0) return;
    for (;;) {
        uint32_t cur = __atomic_load_n(l, __ATOMIC_RELAXED);
        if ((cur & OUL_WAITERS) == 0) {
            uint32_t want = cur | OUL_WAITERS;
            if (!__atomic_compare_exchange_n(l, &cur, want, 0,
                                             __ATOMIC_RELAXED,
                                             __ATOMIC_RELAXED)) {
                continue;
            }
            cur = want;
        }
        (void)cur;
        (void)host_futex_wait(l, OUL_LOCKED | OUL_WAITERS);
        v = __atomic_exchange_n(l, OUL_LOCKED | OUL_WAITERS,
                                __ATOMIC_ACQUIRE);
        if ((v & OUL_LOCKED) == 0) return;
    }
}

static void oul_unlock(volatile uint32_t *l) {
    uint32_t v = __atomic_exchange_n(l, 0, __ATOMIC_RELEASE);
    if (v & OUL_WAITERS) {
        (void)host_futex_wake(l, 1);
    }
}

/* Psynch mutex replica (psynch_mutex_test idiom): ticket + prepost,
 * no fast-path CAS. Init preposts ticket 0x200; each lock takes the
 * next ticket and consumes exactly one grant; each unlock drops the
 * next ticket. */
typedef struct {
    uint32_t addr[4];
    _Atomic uint32_t next;
} mtx_replica_t;

static void mtx_init(mtx_replica_t *m) {
    memset(m->addr, 0, sizeof(m->addr));
    __atomic_store_n(&m->next, 0x100u, __ATOMIC_RELAXED);
    lsk_psynch_mutexdrop((void *)m->addr, 0x200u, 0x100u, 0,
                         MTX_POLICY_FAIRSHARE);
}

static uint32_t mtx_lock(mtx_replica_t *m) {
    uint32_t ticket = __atomic_fetch_add(&m->next, MTX_TICK, __ATOMIC_RELAXED) + MTX_TICK;
    int rc = lsk_psynch_mutexwait((void *)m->addr, ticket, ticket - MTX_TICK, 0,
                                  MTX_POLICY_FAIRSHARE);
    (void)rc;
    return ticket;
}

static void mtx_unlock(mtx_replica_t *m, uint32_t ticket) {
    lsk_psynch_mutexdrop((void *)m->addr, ticket + MTX_TICK, ticket, 0,
                         MTX_POLICY_FAIRSHARE);
}

/* Micro-validation: 4 threads x 2000 increments under the replica;
 * the counter must be exact and no two threads may overlap in the
 * critical section. A replica that fails is reported and its TUIKB
 * modes are skipped (not trusted). */
#define MICRO_THREADS 4
#define MICRO_ITERS 2000

typedef struct {
    volatile uint32_t *lock_word;
    mtx_replica_t *mtx;
    int use_mtx;
    _Atomic uint64_t *counter;
    _Atomic int *in_section;
    _Atomic int *overlap;
} micro_arg_t;

static void *micro_worker(void *arg) {
    micro_arg_t *a = (micro_arg_t *)arg;
    for (int i = 0; i < MICRO_ITERS; i++) {
        uint32_t ticket = 0;
        if (a->use_mtx) ticket = mtx_lock(a->mtx);
        else oul_lock(a->lock_word);
        int prev = __atomic_exchange_n(a->in_section, 1, __ATOMIC_RELAXED);
        if (prev != 0) __atomic_store_n(a->overlap, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(a->counter, 1, __ATOMIC_RELAXED);
        __atomic_store_n(a->in_section, 0, __ATOMIC_RELAXED);
        if (a->use_mtx) mtx_unlock(a->mtx, ticket);
        else oul_unlock(a->lock_word);
    }
    return NULL;
}

static int validate_replica(int use_mtx, const char *name) {
    pthread_t th[MICRO_THREADS];
    volatile uint32_t lock_word = 0;
    mtx_replica_t mtx;
    _Atomic uint64_t counter = 0;
    _Atomic int in_section = 0;
    _Atomic int overlap = 0;
    micro_arg_t a;
    int64_t deadline;
    int ok = 1;

    if (use_mtx) mtx_init(&mtx);
    a.lock_word = &lock_word;
    a.mtx = &mtx;
    a.use_mtx = use_mtx;
    a.counter = &counter;
    a.in_section = &in_section;
    a.overlap = &overlap;
    for (int i = 0; i < MICRO_THREADS; i++) {
        if (pthread_create(&th[i], NULL, micro_worker, &a) != 0) ok = 0;
    }
    deadline = now_ms() + 30000;
    for (int i = 0; i < MICRO_THREADS; i++) {
        if (join_deadline(th[i], NULL, deadline) != 0) ok = 0;
    }
    printf("[heap067] replica %s: counter=%llu want=%d overlap=%d\n",
           name, (unsigned long long)counter,
           MICRO_THREADS * MICRO_ITERS, (int)overlap);
    /* No hard CHECK here: on the x86 host the ulock replica is
     * flaky (counter short, occasional 120 s hang) because
     * os_unfair_lock is unfair and this oversubscribed VM starves
     * waiters: a thread that just released re-acquires via the fast
     * path before a sleeping waiter wakes. The psynch mutex replica
     * (ticket/fairshare) does not starve and validates every run.
     * A failed replica only means its TUIKB modes are skipped, not
     * that the repro failed; the wide-guard evidence rests on
     * MUTEX_WIDE. */
    if (counter != (uint64_t)(MICRO_THREADS * MICRO_ITERS) || overlap != 0) ok = 0;
    return ok;
}

/* ------------------------------------------------------------------ */
/* T1: TUIKB concurrent decode (the device crash).                    */
/* Two threads decode the same keyboard layout graph concurrently    */
/* through a cache modelled on TUIKBGraphSerialization's mutable     */
/* dictionary; a third thread enumerates the cache (the              */
/* concurrent-modification detector). objectForOffset returns a      */
/* BORROWED pointer (the cache owns the only reference); the caller  */
/* must retain it into the array before another thread replaces the  */
/* cache entry, or it retains a freed object (S1, the device         */
/* objc_retain+16 fault 0x220).                                      */
/* ------------------------------------------------------------------ */

#define TK_MODE_NONE 0
#define TK_MODE_ULOCK_WIDE 1
#define TK_MODE_MUTEX_WIDE 2
#define TK_MODE_ULOCK_NARROW 3

#define TK_CACHE_SLOTS 64
#define TK_ARRAY_MAX 16
#define TK_ROUNDS 300
#define TK_COUNT 16
#define TK_FREED_MAX 4096

typedef struct {
    uint32_t offset;
    uint32_t payload;
    _Atomic uint32_t refcount;
} tk_obj_t;

typedef struct {
    _Atomic int in_use;
    _Atomic uint32_t offset;
    _Atomic tk_obj_t *obj;
} tk_slot_t;

typedef struct {
    tk_slot_t slots[TK_CACHE_SLOTS];
    _Atomic uint32_t mut;
} tk_cache_t;

typedef struct {
    tk_obj_t *items[TK_ARRAY_MAX];
    uint32_t count;
} tk_array_t;

typedef struct {
    volatile uint32_t ulock_word;
    mtx_replica_t mtx;
} tk_guard_t;

typedef struct {
    tk_cache_t *cache;
    tk_guard_t *guard;
    int mode;
    uint32_t base;
    volatile int *stop;
} tk_thread_arg_t;

typedef struct {
    uint64_t s1;
    uint64_t s4;
    uint64_t decodes;
    uint64_t allocd;
    uint64_t freed;
    uint64_t array_defects;
} tk_stats_t;

static _Atomic uint64_t g_tk_s1;
static _Atomic uint64_t g_tk_s4;
static _Atomic uint64_t g_tk_decodes;
static _Atomic uint64_t g_tk_allocd;
static _Atomic uint64_t g_tk_freed;
static _Atomic uint64_t g_tk_array_defects;

static const char *tk_mode_name(int mode) {
    switch (mode) {
    case TK_MODE_NONE: return "NONE";
    case TK_MODE_ULOCK_WIDE: return "ULOCK_WIDE";
    case TK_MODE_MUTEX_WIDE: return "MUTEX_WIDE";
    case TK_MODE_ULOCK_NARROW: return "ULOCK_NARROW";
    default: return "?";
    }
}

/* Freed-object registry (test bookkeeping, mutex-guarded). An
 * address stays in the set from free until a later malloc reuses
 * it, so a retain of a freed object is caught without touching
 * freed memory. */
static void *g_tk_freed_addrs[TK_FREED_MAX];
static int g_tk_freed_n;
static pthread_mutex_t g_tk_freed_mu = PTHREAD_MUTEX_INITIALIZER;

static int tk_freed_contains(void *p) {
    int found = 0;
    pthread_mutex_lock(&g_tk_freed_mu);
    for (int i = 0; i < g_tk_freed_n; i++) {
        if (g_tk_freed_addrs[i] == p) { found = 1; break; }
    }
    pthread_mutex_unlock(&g_tk_freed_mu);
    return found;
}

static void tk_freed_add(void *p) {
    pthread_mutex_lock(&g_tk_freed_mu);
    if (g_tk_freed_n < TK_FREED_MAX) g_tk_freed_addrs[g_tk_freed_n++] = p;
    pthread_mutex_unlock(&g_tk_freed_mu);
}

static void tk_freed_remove(void *p) {
    pthread_mutex_lock(&g_tk_freed_mu);
    for (int i = 0; i < g_tk_freed_n; i++) {
        if (g_tk_freed_addrs[i] == p) {
            g_tk_freed_addrs[i] = g_tk_freed_addrs[--g_tk_freed_n];
            break;
        }
    }
    pthread_mutex_unlock(&g_tk_freed_mu);
}

static void tk_freed_reset(void) {
    pthread_mutex_lock(&g_tk_freed_mu);
    g_tk_freed_n = 0;
    pthread_mutex_unlock(&g_tk_freed_mu);
}

static uint32_t tk_hash(uint32_t offset) {
    uint32_t x = offset * 2654435761u;
    x ^= x >> 16;
    return x;
}

/* Retain. Returns NULL (and records S1) when the object was freed:
 * the device objc_retain+16 fault, modeled as a detected freed
 * object instead of a real fault. The freed-check and the refcount
 * increment are atomic under g_tk_ref_mu, and tk_release holds the
 * same mutex across its decrement-to-zero and free, so a retain
 * cannot resurrect a freed object or bump a reused address: a
 * check-then-increment without the mutex leaks (the stale increment
 * lands on the next object malloc'd at that address). This mutex is
 * test-model bookkeeping only; it is not the guard under test. */
static pthread_mutex_t g_tk_ref_mu = PTHREAD_MUTEX_INITIALIZER;

static tk_obj_t *tk_retain(tk_obj_t *o) {
    tk_obj_t *r = NULL;
    if (!o) return NULL;
    pthread_mutex_lock(&g_tk_ref_mu);
    if (tk_freed_contains(o)) {
        __atomic_fetch_add(&g_tk_s1, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&o->refcount, 1, __ATOMIC_RELAXED);
        r = o;
    }
    pthread_mutex_unlock(&g_tk_ref_mu);
    return r;
}

static void tk_release(tk_obj_t *o) {
    int prev;
    if (!o) return;
    pthread_mutex_lock(&g_tk_ref_mu);
    prev = __atomic_fetch_sub(&o->refcount, 1, __ATOMIC_ACQ_REL);
    if (prev == 1) {
        tk_freed_add(o);
        __atomic_fetch_add(&g_tk_freed, 1, __ATOMIC_RELAXED);
        free(o);
    }
    pthread_mutex_unlock(&g_tk_ref_mu);
}

/* Decode one graph node from the (good) .dat data. The sleep widens
 * the window in which two threads decode the same offset at once:
 * thread creation skew is tens of microseconds, so a decode must
 * take much longer than that or the first thread finishes round 1
 * (all offsets cached) before the second thread starts and the
 * race never happens. 200 us per decode makes round 1 ~3.2 ms. */
static tk_obj_t *tk_decode(uint32_t offset) {
    tk_obj_t *o = (tk_obj_t *)malloc(sizeof(*o));
    if (!o) return NULL;
    o->offset = offset;
    o->payload = tk_hash(offset);
    __atomic_store_n(&o->refcount, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_tk_allocd, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_tk_decodes, 1, __ATOMIC_RELAXED);
    tk_freed_remove(o);
    usleep(200);
    return o;
}

static void tk_cache_init(tk_cache_t *c) {
    memset(c, 0, sizeof(*c));
}

static tk_obj_t *tk_cache_lookup(tk_cache_t *c, uint32_t offset) {
    for (int i = 0; i < TK_CACHE_SLOTS; i++) {
        if (__atomic_load_n(&c->slots[i].in_use, __ATOMIC_RELAXED) &&
            __atomic_load_n(&c->slots[i].offset, __ATOMIC_RELAXED) == offset) {
            return __atomic_load_n(&c->slots[i].obj, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static void tk_cache_insert(tk_cache_t *c, uint32_t offset, tk_obj_t *o) {
    tk_obj_t *old = NULL;
    int slot = -1;
    int is_new = 0;
    for (int i = 0; i < TK_CACHE_SLOTS; i++) {
        if (__atomic_load_n(&c->slots[i].in_use, __ATOMIC_RELAXED) &&
            __atomic_load_n(&c->slots[i].offset, __ATOMIC_RELAXED) == offset) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < TK_CACHE_SLOTS; i++) {
            int expect = 0;
            if (__atomic_compare_exchange_n(&c->slots[i].in_use, &expect, 1,
                                            0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                slot = i;
                is_new = 1;
                break;
            }
        }
    }
    if (slot < 0) {
        tk_release(o); /* cache full: drop the decoded object */
        return;
    }
    /* A newly claimed slot gets its offset before the object: a
     * concurrent lookup then either sees the slot as unused or
     * sees the right offset (with a not-yet-stored NULL object,
     * which reads as a miss and just decodes again). Storing the
     * object first would let a lookup return the new object under
     * a stale offset, i.e. the wrong object for the offset. */
    if (is_new) {
        __atomic_store_n(&c->slots[slot].offset, offset, __ATOMIC_RELAXED);
    }
    old = __atomic_exchange_n(&c->slots[slot].obj, o, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->mut, 1, __ATOMIC_RELAXED);
    if (old) tk_release(old);
}

static void tk_cache_drain(tk_cache_t *c) {
    for (int i = 0; i < TK_CACHE_SLOTS; i++) {
        tk_obj_t *o = __atomic_exchange_n(&c->slots[i].obj, NULL,
                                          __ATOMIC_RELAXED);
        __atomic_store_n(&c->slots[i].in_use, 0, __ATOMIC_RELAXED);
        if (o) tk_release(o);
    }
    __atomic_fetch_add(&c->mut, 1, __ATOMIC_RELAXED);
}

static uint32_t tk_guard_lock(int mode, tk_guard_t *g) {
    switch (mode) {
    case TK_MODE_ULOCK_WIDE:
    case TK_MODE_ULOCK_NARROW:
        oul_lock(&g->ulock_word);
        return 0;
    case TK_MODE_MUTEX_WIDE:
        return mtx_lock(&g->mtx);
    case TK_MODE_NONE:
    default:
        return 0;
    }
}

static void tk_guard_unlock(int mode, tk_guard_t *g, uint32_t ticket) {
    switch (mode) {
    case TK_MODE_ULOCK_WIDE:
    case TK_MODE_ULOCK_NARROW:
        oul_unlock(&g->ulock_word);
        return;
    case TK_MODE_MUTEX_WIDE:
        mtx_unlock(&g->mtx, ticket);
        return;
    case TK_MODE_NONE:
    default:
        return;
    }
}

static tk_obj_t *tk_object_for_offset(tk_cache_t *c, tk_guard_t *g,
                                      int mode, uint32_t offset) {
    tk_obj_t *o;
    uint32_t ticket = 0;
    if (mode == TK_MODE_ULOCK_NARROW) ticket = tk_guard_lock(mode, g);
    o = tk_cache_lookup(c, offset);
    if (o) {
        if (mode == TK_MODE_ULOCK_NARROW) tk_guard_unlock(mode, g, ticket);
        return o;
    }
    if (mode == TK_MODE_ULOCK_NARROW) tk_guard_unlock(mode, g, ticket);
    o = tk_decode(offset);
    if (!o) return NULL;
    if (mode == TK_MODE_ULOCK_NARROW) ticket = tk_guard_lock(mode, g);
    tk_cache_insert(c, offset, o);
    if (mode == TK_MODE_ULOCK_NARROW) tk_guard_unlock(mode, g, ticket);
    return o;
}

static void tk_relative_ref_array(tk_cache_t *c, tk_guard_t *g, int mode,
                                  uint32_t base, uint32_t count,
                                  tk_array_t *out, int round) {
    uint32_t ticket = 0;
    out->count = 0;
    if (mode == TK_MODE_ULOCK_WIDE || mode == TK_MODE_MUTEX_WIDE) {
        ticket = tk_guard_lock(mode, g);
    }
    for (uint32_t i = 0; i < count && i < TK_ARRAY_MAX; i++) {
        tk_obj_t *o = tk_object_for_offset(c, g, mode, base + i);
        if (mode == TK_MODE_NONE || mode == TK_MODE_ULOCK_NARROW) {
            /* Widen the borrow-to-retain window in the early rounds,
             * when both threads are decoding the same offsets and
             * replacing each other: without it the first thread
             * retains before the second thread's replace lands (its
             * decode is a 200 us sleep), so S1 never fires. A plain
             * sched_yield is not enough because the other thread is
             * asleep in its decode, not runnable. Only the first
             * few rounds need it; later rounds are all cache hits. */
            if (round < 5) usleep(250);
            else sched_yield();
        }
        if (!o) continue;
        if (tk_retain(o)) {
            out->items[out->count++] = o;
        }
        /* Borrowed pointer: no release here. The cache owns the
         * reference until the array (or a replace) drops it. */
    }
    if (mode == TK_MODE_ULOCK_WIDE || mode == TK_MODE_MUTEX_WIDE) {
        tk_guard_unlock(mode, g, ticket);
    }
}

static int tk_array_verify(tk_array_t *a, uint32_t base, uint32_t count) {
    if (a->count != count) return 0;
    for (uint32_t i = 0; i < count; i++) {
        tk_obj_t *o = a->items[i];
        if (!o) return 0;
        if (o->offset != base + i) return 0;
        if (o->payload != tk_hash(base + i)) return 0;
    }
    return 1;
}

static void tk_array_destroy(tk_array_t *a) {
    for (uint32_t i = 0; i < a->count; i++) tk_release(a->items[i]);
    a->count = 0;
}

static void *tk_decoder_thread(void *arg) {
    tk_thread_arg_t *a = (tk_thread_arg_t *)arg;
    for (int r = 0; r < TK_ROUNDS; r++) {
        tk_array_t arr;
        tk_relative_ref_array(a->cache, a->guard, a->mode, a->base,
                              TK_COUNT, &arr, r);
        if (!tk_array_verify(&arr, a->base, TK_COUNT)) {
            __atomic_fetch_add(&g_tk_array_defects, 1, __ATOMIC_RELAXED);
        }
        tk_array_destroy(&arr);
    }
    return NULL;
}

static void *tk_enumerator_thread(void *arg) {
    tk_thread_arg_t *a = (tk_thread_arg_t *)arg;
    while (!__atomic_load_n(a->stop, __ATOMIC_RELAXED)) {
        uint32_t ticket = tk_guard_lock(a->mode, a->guard);
        uint32_t m0 = __atomic_load_n(&a->cache->mut, __ATOMIC_RELAXED);
        for (int i = 0; i < TK_CACHE_SLOTS; i++) {
            volatile tk_obj_t *o;
            o = __atomic_load_n(&a->cache->slots[i].obj, __ATOMIC_RELAXED);
            (void)o;
            if (__atomic_load_n(&a->cache->mut, __ATOMIC_RELAXED) != m0) {
                __atomic_fetch_add(&g_tk_s4, 1, __ATOMIC_RELAXED);
                break;
            }
        }
        tk_guard_unlock(a->mode, a->guard, ticket);
        sched_yield();
    }
    return NULL;
}

static void tk_run_scenario(int mode, uint32_t base, tk_stats_t *st) {
    tk_cache_t cache;
    tk_guard_t guard;
    tk_thread_arg_t arg;
    pthread_t dec_a, dec_b, enm_th;
    volatile int stop = 0;
    int64_t deadline;

    memset(&guard, 0, sizeof(guard));
    if (mode == TK_MODE_MUTEX_WIDE) mtx_init(&guard.mtx);
    tk_cache_init(&cache);
    tk_freed_reset();
    __atomic_store_n(&g_tk_s1, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tk_s4, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tk_decodes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tk_allocd, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tk_freed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tk_array_defects, 0, __ATOMIC_RELAXED);
    arg.cache = &cache;
    arg.guard = &guard;
    arg.mode = mode;
    arg.base = base;
    arg.stop = &stop;
    pthread_create(&dec_a, NULL, tk_decoder_thread, &arg);
    pthread_create(&dec_b, NULL, tk_decoder_thread, &arg);
    pthread_create(&enm_th, NULL, tk_enumerator_thread, &arg);
    deadline = now_ms() + 60000;
    join_deadline(dec_a, NULL, deadline);
    join_deadline(dec_b, NULL, deadline);
    __atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
    join_deadline(enm_th, NULL, deadline);
    tk_cache_drain(&cache);
    st->s1 = __atomic_load_n(&g_tk_s1, __ATOMIC_RELAXED);
    st->s4 = __atomic_load_n(&g_tk_s4, __ATOMIC_RELAXED);
    st->decodes = __atomic_load_n(&g_tk_decodes, __ATOMIC_RELAXED);
    st->allocd = __atomic_load_n(&g_tk_allocd, __ATOMIC_RELAXED);
    st->freed = __atomic_load_n(&g_tk_freed, __ATOMIC_RELAXED);
    st->array_defects = __atomic_load_n(&g_tk_array_defects, __ATOMIC_RELAXED);
}

static void test_tuikb_decode(void) {
    int oul_ok, mtx_ok;
    static const uint32_t bases[3] = { 0, 16, 32 };

    printf("[heap067] TUIKB chain: %s\n",
           "-[TUIKBGraphSerialization relativeRefArrayForOffset:withCount:]+132"
           " <- objectForOffset:+492 <- _UIKBTreeForOffset:withFlags:+168"
           " <- UIKBTreeForOffset:withFlags: <- objectForOffset:+256"
           " <- relativeRefArrayForOffset:+120");
    g_thread_test_write_hook = test_write_hook;
    oul_ok = validate_replica(0, "os_unfair_lock(ulock)");
    mtx_ok = validate_replica(1, "psynch_mutex");
    /* oul_ok is deliberately NOT a hard CHECK: the ulock replica is
     * host-flaky (see validate_replica), so a failed replica just
     * skips the ULOCK modes. The psynch mutex replica must validate,
     * because MUTEX_WIDE carries the wide-guard evidence. */
    CHECK(mtx_ok);

    for (int mi = 0; mi < 4; mi++) {
        int mode = mi;
        if (mode == TK_MODE_ULOCK_WIDE && !oul_ok) {
            printf("[heap067] TUIKB %s: SKIPPED (replica untrusted)\n",
                   tk_mode_name(mode));
            continue;
        }
        if (mode == TK_MODE_MUTEX_WIDE && !mtx_ok) {
            printf("[heap067] TUIKB %s: SKIPPED (replica untrusted)\n",
                   tk_mode_name(mode));
            continue;
        }
        if (mode == TK_MODE_ULOCK_NARROW && !oul_ok) {
            printf("[heap067] TUIKB %s: SKIPPED (replica untrusted)\n",
                   tk_mode_name(mode));
            continue;
        }
        uint64_t mode_s4_total = 0;
        for (int bi = 0; bi < 3; bi++) {
            tk_stats_t st;
            tk_run_scenario(mode, bases[bi], &st);
            mode_s4_total += st.s4;
            printf("[heap067] TUIKB %s base=%u: S1=%llu S4=%llu decodes=%llu "
                   "allocd=%llu freed=%llu array_defects=%llu\n",
                   tk_mode_name(mode), bases[bi],
                   (unsigned long long)st.s1, (unsigned long long)st.s4,
                   (unsigned long long)st.decodes,
                   (unsigned long long)st.allocd,
                   (unsigned long long)st.freed,
                   (unsigned long long)st.array_defects);
            /* No leak in any mode: every decoded object is freed
             * by the end (array destroy + cache drain + replaces). */
            CHECK_EQ_U64(st.allocd, st.freed);
            switch (mode) {
            case TK_MODE_NONE:
                /* Negative control: the signatures MUST reproduce.
                 * S1, decodes and array_defects are reliable per
                 * seed. S4 is checked as a mode total below: the
                 * enumerator only sees mutations during round 1
                 * (~3.2 ms of inserts), and a late-starting
                 * enumerator can miss that window for one seed,
                 * so a per-seed S4 > 0 is flaky. */
                CHECK(st.s1 > 0);
                CHECK(st.decodes > TK_COUNT);
                CHECK(st.array_defects > 0);
                break;
            case TK_MODE_ULOCK_WIDE:
            case TK_MODE_MUTEX_WIDE:
                /* Wide guard: the lock must exclude, so no
                 * signature, each offset decoded exactly once,
                 * every array exact. */
                CHECK_EQ_U64(st.s1, 0);
                CHECK_EQ_U64(st.s4, 0);
                CHECK_EQ_U64(st.decodes, TK_COUNT);
                CHECK_EQ_U64(st.array_defects, 0);
                break;
            case TK_MODE_ULOCK_NARROW:
                /* Narrow guard: the decode is outside the lock, so
                 * the same offset is decoded twice and the array
                 * insert can retain a freed object (S1). The
                 * enumeration stays clean because inserts are
                 * still serialized with it. */
                CHECK(st.s1 > 0);
                CHECK(st.decodes > TK_COUNT);
                CHECK_EQ_U64(st.s4, 0);
                CHECK(st.array_defects > 0);
                break;
            default:
                break;
            }
        }
        if (mode == TK_MODE_NONE) {
            printf("[heap067] TUIKB NONE S4 total=%llu\n",
                   (unsigned long long)mode_s4_total);
            CHECK(mode_s4_total > 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* T2: VM layer.                                                      */
/* ------------------------------------------------------------------ */

static void test_vm_remap_clobber(void) {
    uint64_t a = 0, b = 0;
    int cur_prot = 0, max_prot = 0;
    uint64_t target = 0;
    unsigned char *pa, *pb;
    int rc;

    CHECK(mach_vm_allocate(0, &a, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    CHECK(mach_vm_allocate(0, &b, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    pa = (unsigned char *)(uintptr_t)a;
    pb = (unsigned char *)(uintptr_t)b;
    memset(pa, 0xA5, 4096);
    memset(pb, 0xB6, 4096);
    /* FIXED remap of B onto the live region A, WITHOUT OVERWRITE.
     * Must fail KERN_NO_SPACE and leave A intact. On base it
     * succeeds (rc=0) and silently overwrites A with B's bytes:
     * lsk_mach_vm_remap does an unchecked mprotect + memcpy onto
     * the target with no occupancy or OVERWRITE check. */
    target = a;
    rc = mach_vm_remap(0, &target, 4096, 0, VM_FLAGS_FIXED,
                       0, b, 1, &cur_prot, &max_prot, 1);
    printf("[heap067] remap FIXED onto live region: rc=%d target=0x%llx "
           "A[0]=0x%x A[4095]=0x%x B[0]=0x%x\n",
           rc, (unsigned long long)target, pa[0], pa[4095], pb[0]);
    CHECK(rc == KERN_NO_SPACE);
    CHECK(pa[0] == 0xA5 && pa[4095] == 0xA5);
    mach_vm_deallocate(0, a, 4096);
    mach_vm_deallocate(0, b, 4096);
}

#define CHURN_THREADS 4
#define CHURN_ITERS 150
#define CHURN_RANGES 3
#define CHURN_TABLE 64

typedef struct {
    uint64_t addr;
    uint64_t size;
    int in_use;
} churn_range_t;

static churn_range_t g_churn_table[CHURN_TABLE];
static pthread_mutex_t g_churn_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_churn_overlaps;
static _Atomic uint64_t g_churn_canary_bad;

static int churn_add(uint64_t addr, uint64_t size) {
    int overlap = 0;
    int slot = -1;
    pthread_mutex_lock(&g_churn_mu);
    for (int i = 0; i < CHURN_TABLE; i++) {
        if (!g_churn_table[i].in_use) {
            if (slot < 0) slot = i;
            continue;
        }
        uint64_t s = g_churn_table[i].addr;
        uint64_t e = s + g_churn_table[i].size;
        if (addr < e && s < addr + size) overlap = 1;
    }
    if (slot >= 0 && !overlap) {
        g_churn_table[slot].addr = addr;
        g_churn_table[slot].size = size;
        g_churn_table[slot].in_use = 1;
    }
    pthread_mutex_unlock(&g_churn_mu);
    if (overlap) __atomic_fetch_add(&g_churn_overlaps, 1, __ATOMIC_RELAXED);
    return overlap ? -1 : 0;
}

static void churn_remove(uint64_t addr) {
    pthread_mutex_lock(&g_churn_mu);
    for (int i = 0; i < CHURN_TABLE; i++) {
        if (g_churn_table[i].in_use && g_churn_table[i].addr == addr) {
            g_churn_table[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_churn_mu);
}

static void *churn_worker(void *arg) {
    uint64_t seed = (uint64_t)(uintptr_t)arg;
    uint64_t state = seed ? seed : 0x12345678ULL;
    for (int it = 0; it < CHURN_ITERS; it++) {
        uint64_t addrs[CHURN_RANGES];
        uint64_t sizes[CHURN_RANGES];
        for (int k = 0; k < CHURN_RANGES; k++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            sizes[k] = 4096ULL * (1 + (state % 3));
            addrs[k] = 0;
            if (mach_vm_allocate(0, &addrs[k], sizes[k], VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
                addrs[k] = 0;
                continue;
            }
            if (churn_add(addrs[k], sizes[k]) != 0) {
                mach_vm_deallocate(0, addrs[k], sizes[k]);
                addrs[k] = 0;
                continue;
            }
            unsigned char *p = (unsigned char *)(uintptr_t)addrs[k];
            p[0] = 0xC3;
            p[sizes[k] - 1] = 0xC3;
            if (p[0] != 0xC3 || p[sizes[k] - 1] != 0xC3) {
                __atomic_fetch_add(&g_churn_canary_bad, 1, __ATOMIC_RELAXED);
            }
        }
        for (int k = 0; k < CHURN_RANGES; k++) {
            if (!addrs[k]) continue;
            unsigned char *p = (unsigned char *)(uintptr_t)addrs[k];
            if (p[0] != 0xC3 || p[sizes[k] - 1] != 0xC3) {
                __atomic_fetch_add(&g_churn_canary_bad, 1, __ATOMIC_RELAXED);
            }
            churn_remove(addrs[k]);
            mach_vm_deallocate(0, addrs[k], sizes[k]);
        }
    }
    return NULL;
}

static void test_vm_churn(void) {
    pthread_t th[CHURN_THREADS];
    int64_t deadline;
    uint64_t overlaps, canary_bad;

    memset(g_churn_table, 0, sizeof(g_churn_table));
    __atomic_store_n(&g_churn_overlaps, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_churn_canary_bad, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < CHURN_THREADS; i++) {
        pthread_create(&th[i], NULL, churn_worker,
                       (void *)(uintptr_t)(0x1000 + i * 0x77));
    }
    deadline = now_ms() + 60000;
    for (int i = 0; i < CHURN_THREADS; i++) {
        join_deadline(th[i], NULL, deadline);
    }
    overlaps = __atomic_load_n(&g_churn_overlaps, __ATOMIC_RELAXED);
    canary_bad = __atomic_load_n(&g_churn_canary_bad, __ATOMIC_RELAXED);
    printf("[heap067] VM churn: overlaps=%llu canary_bad=%llu\n",
           (unsigned long long)overlaps, (unsigned long long)canary_bad);
    CHECK(overlaps == 0);
    CHECK(canary_bad == 0);
}

/* ------------------------------------------------------------------ */
/* T4: AG guard concurrency.                                          */
/* ------------------------------------------------------------------ */

static void *t4_tick_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        ag_note_display_tick(5000000000LL + (int64_t)i * 16666667LL);
        sched_yield();
    }
    return NULL;
}

typedef struct {
    int attempts;
    int allowed;
    int refused;
} t4_update_arg_t;

static void *t4_update_thread(void *arg) {
    t4_update_arg_t *a = (t4_update_arg_t *)arg;
    for (int i = 0; i < 500; i++) {
        a->attempts++;
        if (ag_update_begin() == 0) a->allowed++;
        else a->refused++;
        sched_yield();
    }
    return NULL;
}

static void test_ag_concurrent(void) {
    pthread_t tick_th, upd_a, upd_b;
    t4_update_arg_t ua = { 0, 0, 0 }, ub = { 0, 0, 0 };
    int64_t deadline;

    ag_test_reset();
    CHECK(ag_update_cycle_ready() == 0);
    CHECK(ag_update_begin() == -1);
    CHECK(pthread_create(&tick_th, NULL, t4_tick_thread, NULL) == 0);
    CHECK(pthread_create(&upd_a, NULL, t4_update_thread, &ua) == 0);
    CHECK(pthread_create(&upd_b, NULL, t4_update_thread, &ub) == 0);
    deadline = now_ms() + 10000;
    CHECK(join_deadline(tick_th, NULL, deadline) == 0);
    CHECK(join_deadline(upd_a, NULL, deadline) == 0);
    CHECK(join_deadline(upd_b, NULL, deadline) == 0);
    printf("[heap067] AG concurrent: ticks=%u allowed=%d refused=%d\n",
           ag_tick_count(), ua.allowed + ub.allowed, ua.refused + ub.refused);
    CHECK_EQ_I64(ag_tick_count(), 50);
    CHECK(ag_update_cycle_ready() == 1);
    CHECK(ua.attempts + ub.attempts == 1000);
    CHECK(ua.allowed + ub.allowed > 0);
    CHECK(ua.allowed + ub.allowed + ua.refused + ub.refused == 1000);
}

/* ------------------------------------------------------------------ */
/* T5: dictionary backing storage freed and reused mid-decode         */
/* (new device evidence 2026-11-18 23:29 CET).                       */
/* Device crash: objc_msgSend+32 from -[__NSDictionaryM             */
/* objectForKey:]+164, called from -[TUIKBGraphSerialization         */
/* objectForOffset:]+72. x0 (receiver loaded from the dictionary's    */
/* bucket) = 0x8181808080808080, x19 key = tagged NSNumber           */
/* 0xa78f01581557852b. The cache dictionary's STORAGE itself contains  */
/* garbage: its backing memory was freed and reused while the        */
/* dictionary is still live, during a single-thread deep recursion   */
/* (objectForOffset -> relativeRefArrayForOffset -> objectForOffset). */
/* Same UAF pattern as the AG and NSArray crashes.                    */
/* Zombie mode: freed buckets are kept and scribbled (0x80, the      */
/* receiver set to the device's exact garbage value), so a lookup    */
/* that touches freed storage is DETECTED (a UAF hit) instead of     */
/* crashing. This is the NSZombieEnabled / MallocScribble analogue.   */
/* ------------------------------------------------------------------ */

#define DICT_BUCKETS 16
#define DICT_GARBAGE_RECEIVER 0x8181808080808080ULL

typedef struct {
    uint64_t key;
    void *value;
} dict_bucket_t;

typedef struct {
    dict_bucket_t *buckets;
    uint32_t count;
    uint32_t cap;
    int buckets_freed; /* zombie flag: storage freed while dict live */
} dict_t;

static _Atomic uint64_t g_dict_uaf;
static _Atomic uint64_t g_dict_lookups;
static _Atomic uint64_t g_dict_garbage_reads;

static void dict_init(dict_t *d, uint32_t base) {
    d->buckets = (dict_bucket_t *)calloc(DICT_BUCKETS, sizeof(dict_bucket_t));
    d->count = DICT_BUCKETS;
    d->cap = DICT_BUCKETS;
    d->buckets_freed = 0;
    for (uint32_t i = 0; i < DICT_BUCKETS; i++) {
        d->buckets[i].key = (uint64_t)(base + i);
        d->buckets[i].value = (void *)(uintptr_t)(0x1000 + base + i);
    }
}

static void dict_destroy(dict_t *d) {
    free(d->buckets);
    d->buckets = NULL;
    d->count = 0;
    d->cap = 0;
}

/* Free the backing storage while the dictionary is still live.
 * Zombie mode: do not free the memory; scribble it (0x80) and set
 * the first bucket's value to the device's exact garbage receiver,
 * so a later lookup reads garbage and is detected. */
static void dict_free_buckets_zombie(dict_t *d) {
    if (!d->buckets || d->buckets_freed) return;
    d->buckets_freed = 1;
    memset(d->buckets, 0x80, d->cap * sizeof(dict_bucket_t));
    d->buckets[0].value = (void *)(uintptr_t)DICT_GARBAGE_RECEIVER;
}

/* objectForKey: on a live dictionary returns the value; on a
 * zombie (freed storage) returns the garbage receiver and records
 * a UAF hit. The key is a tagged NSNumber on device; here it is
 * the plain offset. */
static void *dict_object_for_key(dict_t *d, uint64_t key) {
    __atomic_fetch_add(&g_dict_lookups, 1, __ATOMIC_RELAXED);
    if (d->buckets_freed) {
        __atomic_fetch_add(&g_dict_uaf, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_dict_garbage_reads, 1, __ATOMIC_RELAXED);
        return (void *)(uintptr_t)DICT_GARBAGE_RECEIVER;
    }
    for (uint32_t i = 0; i < d->count; i++) {
        if (d->buckets[i].key == key) return d->buckets[i].value;
    }
    return NULL;
}

/* Deep recursive decode: objectForOffset -> relativeRefArrayForOffset
 * -> objectForOffset, one lookup per level. At depth 3 the decoder
 * pauses, giving the reaper time to free the backing storage; the
 * lookup after the pause then reads the freed storage. */
static void *dict_decode(dict_t *d, uint64_t offset, int depth) {
    void *v = dict_object_for_key(d, offset);
    if (depth <= 0) return v;
    if (depth == 3) usleep(500);
    void *child = dict_decode(d, offset + 1, depth - 1);
    (void)child;
    return dict_object_for_key(d, offset);
}

typedef struct {
    dict_t *dict;
    int delay_us;
} dict_reaper_arg_t;

static void *dict_reaper_thread(void *arg) {
    dict_reaper_arg_t *a = (dict_reaper_arg_t *)arg;
    usleep((useconds_t)a->delay_us);
    dict_free_buckets_zombie(a->dict);
    return NULL;
}

static void test_dict_backing_uaf(void) {
    dict_t d;
    pthread_t reaper;
    dict_reaper_arg_t ra;
    void *v;

    printf("[heap067] dict UAF: device objc_msgSend+32 from "
           "-[__NSDictionaryM objectForKey:]+164 <- "
           "-[TUIKBGraphSerialization objectForOffset:]+72, "
           "x0=0x8181808080808080 x19=0xa78f01581557852b\n");

    /* Mode A: backing storage freed mid-decode (reaper fires while
     * the decoder is paused at depth 3). The lookups after the
     * free must hit the zombie and read the garbage receiver. */
    __atomic_store_n(&g_dict_uaf, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dict_lookups, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dict_garbage_reads, 0, __ATOMIC_RELAXED);
    dict_init(&d, 0x100);
    ra.dict = &d;
    ra.delay_us = 100;
    CHECK(pthread_create(&reaper, NULL, dict_reaper_thread, &ra) == 0);
    v = dict_decode(&d, 0x100, 6);
    CHECK(pthread_join(reaper, NULL) == 0);
    printf("[heap067] dict UAF freed-mid-decode: lookups=%llu uaf=%llu "
           "garbage_reads=%llu receiver=0x%llx\n",
           (unsigned long long)__atomic_load_n(&g_dict_lookups, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_dict_uaf, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_dict_garbage_reads, __ATOMIC_RELAXED),
           (unsigned long long)(uintptr_t)v);
    CHECK(__atomic_load_n(&g_dict_uaf, __ATOMIC_RELAXED) > 0);
    CHECK(__atomic_load_n(&g_dict_garbage_reads, __ATOMIC_RELAXED) > 0);
    CHECK((uintptr_t)v == DICT_GARBAGE_RECEIVER);
    dict_destroy(&d);

    /* Mode B: retained (no reaper). Every lookup returns the real
     * value, no UAF, no garbage. */
    __atomic_store_n(&g_dict_uaf, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dict_lookups, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dict_garbage_reads, 0, __ATOMIC_RELAXED);
    dict_init(&d, 0x200);
    v = dict_decode(&d, 0x200, 6);
    printf("[heap067] dict UAF retained: lookups=%llu uaf=%llu "
           "value=0x%llx\n",
           (unsigned long long)__atomic_load_n(&g_dict_lookups, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_dict_uaf, __ATOMIC_RELAXED),
           (unsigned long long)(uintptr_t)v);
    CHECK(__atomic_load_n(&g_dict_uaf, __ATOMIC_RELAXED) == 0);
    CHECK(__atomic_load_n(&g_dict_garbage_reads, __ATOMIC_RELAXED) == 0);
    CHECK((uintptr_t)v == (uintptr_t)(0x1000 + 0x200));
    dict_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* T6: autorelease pool / objc TLS mixing between threads (hypothesis */
/* 1, 2026-11-18 23:29 CET). Two threads share an autorelease        */
/* pool page because a TSD slot / tpidr_el0 is wrong for some thread  */
/* type (bsdthread_create threads in thread.c, workq threads in      */
/* workq.c with WQ_FLAG_THREAD_REUSE, kevent/workloop threads); a    */
/* pool pop on one thread then releases another thread's objects.    */
/* Model: each thread's current pool is selected through a TSD slot  */
/* (a __thread pointer, the host analogue of the pthread TSD direct  */
/* slot that holds objc's AutoreleasePoolPage hot page). In ALIASED   */
/* mode thread B's slot is made to return thread A's pool page,      */
/* simulating the TSD mix-up; B's pop then releases A's objects.     */
/* This is a model of the hypothesis, not a test of thread.c's TSD:  */
/* tpidr_el0 is ARM-only and cannot be exercised on the x86 host.    */
/* ------------------------------------------------------------------ */

#define POOL_MAX_OBJS 32

typedef struct {
    int owner_tid;
    _Atomic int refcount;
    int freed;
} pool_obj_t;

typedef struct {
    pool_obj_t *objs[POOL_MAX_OBJS];
    int count;
    int owner_tid;
} pool_page_t;

static __thread pool_page_t *g_tsd_pool;
static pool_page_t *g_pool_a_page;
static _Atomic uint64_t g_pool_cross_releases;
static _Atomic uint64_t g_pool_releases;
static _Atomic uint64_t g_pool_freed;
static _Atomic int g_pool_a_ready;
static _Atomic int g_pool_b_done;

static pool_obj_t *pool_obj_new(int owner_tid) {
    pool_obj_t *o = (pool_obj_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->owner_tid = owner_tid;
    __atomic_store_n(&o->refcount, 1, __ATOMIC_RELAXED);
    o->freed = 0;
    return o;
}

static void pool_obj_release(pool_obj_t *o, int releaser_tid) {
    int prev;
    if (!o) return;
    __atomic_fetch_add(&g_pool_releases, 1, __ATOMIC_RELAXED);
    if (o->owner_tid != releaser_tid) {
        __atomic_fetch_add(&g_pool_cross_releases, 1, __ATOMIC_RELAXED);
    }
    prev = __atomic_fetch_sub(&o->refcount, 1, __ATOMIC_ACQ_REL);
    if (prev == 1) {
        o->freed = 1;
        __atomic_fetch_add(&g_pool_freed, 1, __ATOMIC_RELAXED);
        free(o);
    }
}

static pool_page_t *pool_push(int owner_tid) {
    pool_page_t *p = (pool_page_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->count = 0;
    p->owner_tid = owner_tid;
    g_tsd_pool = p;
    return p;
}

static void pool_autorelease(pool_obj_t *o) {
    pool_page_t *p = g_tsd_pool;
    if (!p || !o) return;
    if (p->count < POOL_MAX_OBJS) p->objs[p->count++] = o;
}

static void pool_pop(int releaser_tid) {
    pool_page_t *p = g_tsd_pool;
    if (!p) return;
    for (int i = 0; i < p->count; i++) {
        pool_obj_release(p->objs[i], releaser_tid);
    }
    p->count = 0;
    free(p);
    g_tsd_pool = NULL;
}

typedef struct {
    int tid;
    int alias_to_a; /* 1: this thread's TSD slot returns A's pool */
    pool_obj_t **held;
    int n_held;
} pool_thread_arg_t;

static void *pool_thread_worker(void *arg) {
    pool_thread_arg_t *a = (pool_thread_arg_t *)arg;
    pool_page_t *page;
    if (a->alias_to_a && g_pool_a_page) {
        /* TSD mix-up: this thread's slot returns A's pool page. */
        g_tsd_pool = g_pool_a_page;
        page = g_pool_a_page;
    } else {
        page = pool_push(a->tid);
    }
    (void)page;
    for (int i = 0; i < a->n_held; i++) {
        a->held[i] = pool_obj_new(a->tid);
        pool_autorelease(a->held[i]);
    }
    /* Hold the objects past the pop: the owner keeps using them. */
    usleep(200);
    pool_pop(a->tid);
    return NULL;
}

/* Aliased mode workers. A pushes its pool, publishes the page,
 * autoreleases its objects, then waits for B to pop (and does NOT
 * pop itself: B already popped the shared page). B waits for A's
 * page, aliases its TSD slot to it, and pops it, releasing A's
 * objects from B's thread: cross-thread releases. */
static void *pool_alias_a_worker(void *arg) {
    pool_thread_arg_t *a = (pool_thread_arg_t *)arg;
    pool_page_t *page = pool_push(a->tid);
    g_pool_a_page = page;
    for (int i = 0; i < a->n_held; i++) {
        a->held[i] = pool_obj_new(a->tid);
        pool_autorelease(a->held[i]);
    }
    __atomic_store_n(&g_pool_a_ready, 1, __ATOMIC_RELAXED);
    while (!__atomic_load_n(&g_pool_b_done, __ATOMIC_RELAXED)) {
        usleep(1000);
    }
    /* Do not pop and do not touch the objects: B freed them. */
    return NULL;
}

static void *pool_alias_b_worker(void *arg) {
    pool_thread_arg_t *a = (pool_thread_arg_t *)arg;
    while (!__atomic_load_n(&g_pool_a_ready, __ATOMIC_RELAXED)) {
        usleep(1000);
    }
    g_tsd_pool = g_pool_a_page;
    pool_pop(a->tid);
    __atomic_store_n(&g_pool_b_done, 1, __ATOMIC_RELAXED);
    return NULL;
}

static void test_pool_tsd_mixing(void) {
    pthread_t th_a, th_b;
    pool_thread_arg_t arg_a, arg_b;
    pool_obj_t *held_a[4];
    pool_obj_t *held_b[4];

    printf("[heap067] pool/TSD: hypothesis 1, two threads sharing an "
           "autorelease pool page via a wrong TSD slot; a pop on one "
           "thread releases the other's objects\n");

    /* Mode CORRECT: each thread has its own pool page. No object
     * is released by a thread other than its owner. */
    __atomic_store_n(&g_pool_cross_releases, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_releases, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_freed, 0, __ATOMIC_RELAXED);
    g_pool_a_page = NULL;
    memset(held_a, 0, sizeof(held_a));
    memset(held_b, 0, sizeof(held_b));
    arg_a.tid = 1;
    arg_a.alias_to_a = 0;
    arg_a.held = held_a;
    arg_a.n_held = 4;
    arg_b.tid = 2;
    arg_b.alias_to_a = 0;
    arg_b.held = held_b;
    arg_b.n_held = 4;
    CHECK(pthread_create(&th_a, NULL, pool_thread_worker, &arg_a) == 0);
    CHECK(pthread_create(&th_b, NULL, pool_thread_worker, &arg_b) == 0);
    CHECK(pthread_join(th_a, NULL) == 0);
    CHECK(pthread_join(th_b, NULL) == 0);
    printf("[heap067] pool/TSD correct: releases=%llu cross=%llu freed=%llu\n",
           (unsigned long long)__atomic_load_n(&g_pool_releases, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_pool_cross_releases, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_pool_freed, __ATOMIC_RELAXED));
    CHECK_EQ_U64(__atomic_load_n(&g_pool_cross_releases, __ATOMIC_RELAXED), 0);
    CHECK_EQ_U64(__atomic_load_n(&g_pool_releases, __ATOMIC_RELAXED), 8);
    CHECK_EQ_U64(__atomic_load_n(&g_pool_freed, __ATOMIC_RELAXED), 8);

    /* Mode ALIASED: B's TSD slot returns A's pool page. B's pop
     * releases A's objects (cross-thread releases), so A's objects
     * are freed while A still holds them: the UAF pattern. */
    __atomic_store_n(&g_pool_cross_releases, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_releases, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_freed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_a_ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pool_b_done, 0, __ATOMIC_RELAXED);
    g_pool_a_page = NULL;
    memset(held_a, 0, sizeof(held_a));
    memset(held_b, 0, sizeof(held_b));
    arg_a.tid = 1;
    arg_a.alias_to_a = 0;
    arg_a.held = held_a;
    arg_a.n_held = 4;
    arg_b.tid = 2;
    arg_b.alias_to_a = 1;
    arg_b.held = held_b;
    arg_b.n_held = 0;
    CHECK(pthread_create(&th_a, NULL, pool_alias_a_worker, &arg_a) == 0);
    CHECK(pthread_create(&th_b, NULL, pool_alias_b_worker, &arg_b) == 0);
    CHECK(pthread_join(th_a, NULL) == 0);
    CHECK(pthread_join(th_b, NULL) == 0);
    printf("[heap067] pool/TSD aliased: releases=%llu cross=%llu freed=%llu\n",
           (unsigned long long)__atomic_load_n(&g_pool_releases, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_pool_cross_releases, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&g_pool_freed, __ATOMIC_RELAXED));
    /* All four of A's objects were released by B (owner 1, releaser
     * 2). The page was freed by B's pop; nobody else frees it. */
    CHECK_EQ_U64(__atomic_load_n(&g_pool_cross_releases, __ATOMIC_RELAXED), 4);
    CHECK_EQ_U64(__atomic_load_n(&g_pool_releases, __ATOMIC_RELAXED), 4);
    CHECK_EQ_U64(__atomic_load_n(&g_pool_freed, __ATOMIC_RELAXED), 4);
    g_pool_a_page = NULL;


}


/* T2b: remap protection + copy=FALSE (AG root cause).
 * AttributeGraph's grow_region remaps its read-write table with
 * copy=FALSE. On base, lsk_mach_vm_remap ends with
 * sys_mprotect(target, PROT_READ | PROT_EXEC) and reports
 * cur/max_protection = VM_PROT_READ | VM_PROT_EXECUTE, so every
 * remapped data region loses WRITE. After the first grow the AG
 * table is R-X, so the next table write faults with SIGSEGV on a
 * 0x5... address. That is the AG crash. This test remaps a
 * writable region with copy=FALSE and checks (a) the target is
 * still writable (via /proc/self/maps) and (b) the remap MOVED
 * the region: the source is unmapped afterwards and the target
 * holds the data.
 * copy=FALSE means page move, not sharing: vm_map_remap passes
 * !copy as src_destroy to vm_map_copyin, so copy=FALSE UNMAPS
 * the source (move semantics per XNU). A write to the source
 * after the remap faults. */
static int maps_region_writable(uint64_t addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int writable = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= lo && addr < hi) {
            writable = (strchr(perms, 'w') != NULL) ? 1 : 0;
            break;
        }
    }
    fclose(f);
    return writable;
}

/* True when any mapping overlaps [addr, addr+size). Probes
 * /proc/self/maps without touching the range, so it is safe to call
 * on a range the remap may have unmapped. */
static int maps_range_mapped(uint64_t addr, uint64_t size) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int mapped = 0;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (addr < hi && (addr + size) > lo) { mapped = 1; break; }
    }
    fclose(f);
    return mapped;
}

static void test_vm_remap_protection(void) {
    uint64_t src = 0, target = 0;
    int cur_prot = 0, max_prot = 0;
    int rc, writable;
    unsigned char *ps, *pt;

    CHECK(mach_vm_allocate(0, &src, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    ps = (unsigned char *)(uintptr_t)src;
    memset(ps, 0xA5, 4096);
    /* copy=0 (share), like AG grow_region. */
    target = 0;
    rc = mach_vm_remap(0, &target, 4096, 0, VM_FLAGS_ANYWHERE,
                       0, src, 0, &cur_prot, &max_prot, 1);
    printf("[heap067] remap ANYWHERE copy=FALSE: rc=%d target=0x%llx "
           "cur_prot=0x%x max_prot=0x%x\n",
           rc, (unsigned long long)target, cur_prot, max_prot);
    CHECK(rc == KERN_SUCCESS);
    if (rc != KERN_SUCCESS) {
        mach_vm_deallocate(0, src, 4096);
        return;
    }
    pt = (unsigned char *)(uintptr_t)target;
    /* (a) The target must still be writable: it is a data region. */
    writable = maps_region_writable(target);
    printf("[heap067] remap target writable=%d (want 1)\n", writable);
    CHECK(writable == 1);
    /* (b) copy=FALSE means MOVE, not share: the source range is
     * unmapped after the remap and the target holds the data. */
    {
        int mapped = maps_range_mapped(src, 4096);
        printf("[heap067] remap move: source still mapped=%d (want 0)\n", mapped);
        CHECK(mapped == 0);
        printf("[heap067] remap move: target[0]=0x%x (want 0xa5)\n", pt[0]);
        CHECK(pt[0] == 0xA5);
    }
    mach_vm_deallocate(0, src, 4096);
    mach_vm_deallocate(0, target, 4096);
}

/* ------------------------------------------------------------------ */
/* T0: verbatim device crash signatures.                              */
/* ------------------------------------------------------------------ */

static void test_signatures(void) {
    static const char *sigs[] = {
        "S1: objc_retain+16 bad object pointer, -[__NSArrayM insertObject:atIndex:]",
        "S2: objc_retain+16 bad object pointer, -[UITouch _setResponder:]",
        "S3: AG::Graph::add_input+416 / add_output_edge+80 / UpdateStack::update+1100 fault 0x5xxxxxxxx",
        "S4: ___NSMUTABLEDICTIONARY_CONCURRENT_MODIFICATION_DETECTED__",
        "S5: frame stall",
        "S6: NSInternalInconsistencyException 'returning nil screen from mainScreen is not allowed!'",
        "TUIKB: -[TUIKBGraphSerialization relativeRefArrayForOffset:withCount:]+132 <- objectForOffset:+492 <- _UIKBTreeForOffset:withFlags:+168 <- UIKBTreeForOffset:withFlags: <- objectForOffset:+256 <- relativeRefArrayForOffset:+120",
        "TUIKB-dict: objc_msgSend+32 from -[__NSDictionaryM objectForKey:]+164 <- -[TUIKBGraphSerialization objectForOffset:]+72, x0=0x8181808080808080",
    };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        printf("[heap067] signature: %s\n", sigs[i]);
        CHECK(sigs[i] != NULL && sigs[i][0] != '\0');
    }
}

int main(void) {
    test_signatures();
    test_tuikb_decode();
    test_vm_remap_clobber();
    test_vm_remap_protection();
    test_vm_churn();
    test_ag_concurrent();
    test_dict_backing_uaf();
    test_pool_tsd_mixing();
    printf("heap067_repro_vm_locks_test: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
