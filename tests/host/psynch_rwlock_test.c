#pragma GCC diagnostic warning "-Wincompatible-pointer-types"
/*
 * psynch_rwlock_test.c - Host stress test for psynch rwlock.
 *
 * Tests the in-process psynch_rw_* implementation in
 * libsystem_kernel/src/thread.c against a faithful replica of the
 * 23G90 libpthread consumer (pthread_rwlock.c, 23G90
 * libsystem_pthread.dylib 0x1e2e97250 _pthread_rwlock_lock_wait and
 * _pthread_rwlock_updateval). Host glibc pthread_rwlock_* does NOT
 * call our psynch functions (glibc has its own rwlock), so this test
 * cannot use the glibc entry points: it replicates the Apple caller
 * instead, and the qrun gate runs the real libpthread.
 *
 * What the replica does:
 *   - rwlock word layout: sig at +0x00, lock at
 *     +0x08, rw_flags at +0x10, rw_tid at +0x18, seq area (L, S, U, pad)
 *     at +0x20, 16-byte aligned. Initial L = 0xC0, S = 0x01, U = 0.
 *   - fast path: read acquires iff (L & (WBIT|KBIT)) == 0, clearing
 *     UBIT and doing L += PTHRW_INC and S += PTHRW_INC; write acquires
 *     iff (L & UBIT) != 0, keeping the old L count, setting
 *     IBIT|KBIT|EBIT in the low byte, then L += INC and S += INC.
 *     Single 64-bit CAS on the L|S pair; retry on mismatch.
 *   - slow path: read clears UBIT, write sets KBIT|WBIT, L += INC; if S
 *     has SBIT, clear it and set S count to the OLD L count; CAS, then
 *     lock_wait with the new L, current U, current S, rw_flags.
 *   - lock_wait: call lsk_psynch_rw_rdlock / wrlock with the same
 *     arguments libpthread passes; on -1 retry only on EINTR (Darwin 4),
 *     otherwise record the error (real libpthread crashes the process
 *     with PTHREAD_INTERNAL_CRASH); on success merge the returned
 *     updatebits with updateval.
 *   - updateval: read L|S as one 64-bit value; if neither MBIT is set in
 *     updateval nor IBIT in old S, no-op; else new L = modbits(old L,
 *     updateval), new S = old S advanced by the granted count (added in
 *     both the fresh and the overlap case; a non-overlap grant then
 *     masks S to the count field, an overlap grant clears only
 *     SAVEMASK), single 64-bit CAS on L|S, retry on mismatch.
 *   - unlock: U += INC; last unlock (new U count == old L count) is pure
 *     userland (L = oldcount | RWLOCK_INIT, S = oldcount | RWS_INIT);
 *     else if (L & (EBIT|WBIT|KBIT)) == 0 or new U != S count, no kernel
 *     call; else slow path (L = oldcount | KBIT, S |= IBIT, plus WSVBIT
 *     if WBIT was set) and unlock_drop with OLD L and NEW U/S, retrying
 *     only on EINTR, return value otherwise ignored.
 *
 * Replica judgement calls (documented, qrun is the authority):
 *   - modbits masks updateval to its low byte before OR-ing, so the
 *     result keeps the OLD L count field;
 *     OR-ing the full updateval would leak the granted count into L.
 *   - updateval advances the S count by the granted count in both the
 *     fresh and the overlap case, then masks it: a non-overlap grant
 *     masks S to the count field (clearing the whole low byte,
 *     including IBIT), an overlap grant clears only SAVEMASK. Once a
 *     grant is merged the sequence is set, so the "sequence not set"
 *     flags must not survive into the next merge.
 *   - Unlock updates L|S and U together with a single 128-bit CAS over
 *     the whole seq area (pad word preserved), exactly like the real
 *     libpthread; a plain U store would lose an increment when two
 *     readers unlock at once, and two separate CAS operations leave a
 *     window where a waiter can increment L between them, so the kernel
 *     is called with a stale oldL. On host x86-64 the 128-bit atomic
 *     needs libatomic, so the harness links -latomic for this test.
 *   - The test declares the psynch functions with the kernel signature
 *     (uintptr_t first argument, uint32_t return). Against the base tree
 *     the callee reads only the low 32 bits of the address, which is
 *     exactly the pointer-truncation bug, and the return register is
 *     identical, so the same test binary links against both trees.
 *
 * What the replica is NOT: it is not the real 23G90 libpthread. It does
 * not cover EDEADLK, trylock, reader overflow (EAGAIN), timed lock, or
 * the writer-tid tracking (rw_tid is never set or checked here). Those
 * paths never call psynch_rw_* (EDEADLK/trylock) or are unexercised by
 * the field crashes, and they are the qrun gate's job with the real
 * libpthread under qemu.
 *
 * Coverage: reader-after-writer,
 * writer-after-reader, writer-after-writer (lowest seq wins), many
 * readers with a writer queued behind them (readers above the writer
 * must NOT be granted), prepost (unlock arriving before the waiter),
 * interrupted wakeup (signal during the wait, then retry), overlap
 * (second reader arriving while a read grant is outstanding), stale
 * unlock (same generation unlocked twice), a lock address above 4 GiB
 * (pointer-truncation regression, including two locks sharing the same
 * low 32 bits), and the field crash shapes as data-structure races
 * (concurrent dictionary-like get and array-like insert under contended
 * readers/writers). Invariants asserted throughout: never reader+writer
 * inside at once, never two writers inside at once, no lost wakeups (no
 * test thread stuck after all holders released), and every slow-path
 * grant returns nonzero updatebits (returning 0
 * from rdlock/wrlock when the caller is not granted is a bug).
 *
 * RED check: run against the base tree first.
 * The no-op functions return 0, so every contended waiter is "granted"
 * instantly and the overlap/ordering/count assertions fail.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Declarations from libsystem_kernel/src/thread.c, kernel signature
 * (see header comment). __asm__ names are what the loader binds by. */
uint32_t lsk_psynch_rw_rdlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_rdlock");
uint32_t lsk_psynch_rw_wrlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_wrlock");
uint32_t lsk_psynch_rw_unlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_unlock");
uint32_t lsk_psynch_rw_unlock2(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_unlock2");

/* ---- Stubs required by thread.c (same pattern as thread_reg_test.c) ---- */
int g_ipc_trace = 0;
static _Thread_local int g_last_errno = 0;
void set_errno(int err) { g_last_errno = err; }
int linux_to_darwin_errno(long linux_err) { return (int)(linux_err < 0 ? -linux_err : linux_err); }

static uint32_t g_next_kport = 0x1003;
uint32_t ipc_port_allocate_thread(void) {
    return __atomic_fetch_add(&g_next_kport, 4, __ATOMIC_RELAXED);
}

long thread_test_gettid(void) {
    return (long)syscall(SYS_gettid);
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

/* ---- Constants (synch_internal.h) ---- */
#define PTHRW_INC          0x100u
#define PTHRW_BIT_MASK     0xFFu
#define PTHRW_COUNT_MASK   (~0xFFu)
#define PTH_RWL_KBIT       0x01u
#define PTH_RWL_EBIT       0x02u
#define PTH_RWL_WBIT       0x04u
#define PTH_RWL_UBIT       0x40u
#define PTH_RWL_MBIT       0x40u
#define PTH_RWL_IBIT       0x80u
#define PTHRW_RWL_INIT     PTH_RWL_IBIT
#define PTHRW_RWLOCK_INIT  (PTH_RWL_IBIT | PTH_RWL_UBIT)
#define PTH_RWS_SBIT       0x01u
#define PTH_RWS_IBIT       0x02u
#define PTH_RWS_WSVBIT     0x04u
#define PTHRW_RWS_SAVEMASK PTH_RWS_WSVBIT
#define PTHRW_RWS_INIT     PTH_RWS_SBIT
#define DARWIN_EINTR       4
#define DARWIN_ESRCH       3

/* ---- rwlock word layout ---- */
typedef struct {
    uint64_t sig;          /* +0x00 'RWLK' */
    uint32_t lock;         /* +0x08 os_unfair_lock */
    uint32_t bitfield;     /* +0x0c */
    uint32_t rw_flags;     /* +0x10 0x10 shared / 0x20 private */
    uint32_t pad;          /* +0x14 */
    uint32_t rw_tid[2];    /* +0x18 writer thread id */
    union {                /* +0x20 16-byte seq area, 128-bit aligned */
        struct { uint32_t l, s, u, pad; } w; /* L, S, U */
        uint64_t ls;       /* L|S pair, one 64-bit CAS unit */
    } seq;
    uint32_t rw_mis[4];    /* +0x30 */
    uint32_t reserved[34]; /* +0x40 */
} rwlock_t;

_Static_assert(__builtin_offsetof(rwlock_t, sig) == 0x00, "sig offset");
_Static_assert(__builtin_offsetof(rwlock_t, rw_flags) == 0x10, "rw_flags offset");
_Static_assert(__builtin_offsetof(rwlock_t, seq) == 0x20, "seq offset");
_Static_assert(sizeof(rwlock_t) == 0xC8, "rwlock size");

static void rwlock_init(rwlock_t *rw) {
    memset(rw, 0, sizeof(*rw));
    rw->sig = 0x52574C4Bull; /* 'RWLK' */
    rw->rw_flags = 0x20u;    /* process-private */
    rw->seq.w.l = PTHRW_RWLOCK_INIT; /* 0xC0 */
    rw->seq.w.s = PTHRW_RWS_INIT;    /* 0x01 */
    rw->seq.w.u = 0;
}

static uint64_t ls_load(rwlock_t *rw) {
    return __atomic_load_n(&rw->seq.ls, __ATOMIC_SEQ_CST);
}
static int ls_cas(rwlock_t *rw, uint64_t *expected, uint64_t want) {
    return __atomic_compare_exchange_n(&rw->seq.ls, expected, want, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static uint32_t u_load(rwlock_t *rw) {
    return __atomic_load_n(&rw->seq.w.u, __ATOMIC_SEQ_CST);
}
static uint32_t l_load(rwlock_t *rw) {
    return __atomic_load_n(&rw->seq.w.l, __ATOMIC_SEQ_CST);
}

/* ---- Check accounting ---- */
static int g_checks = 0;
static int g_failures = 0;
static int g_skipped = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---- Per-thread lock-call record (also the worker arg base) ---- */
typedef struct tracker tracker_t;
struct tracker {
    pthread_mutex_t mu;
    int readers;
    int writers;
    int violation; /* reader+writer or two writers inside at once */
    int max_readers;
};

typedef struct {
    rwlock_t *rw;
    tracker_t *trk;
    int is_read;
    volatile int attempting; /* set just before calling replica_lock */
    volatile int entered;    /* set once inside the critical section */
    volatile int holding;    /* 1 while holding the lock */
    volatile int release;    /* holder thread exits when set */
    volatile int done;
    volatile int failed;
    long entry_seq;          /* global entry order, assigned on entry */
    uint32_t last_updatebits;
    int took_slow_path;
    int psynch_calls;
    int zero_grant_slow;     /* slow-path grants that returned 0 */
    int psynch_error;        /* non-EINTR errno from a psynch call */
    uint32_t last_unlock_lgen, last_unlock_ugen, last_unlock_wc;
    uint32_t last_unlock_ret;
    long ops_done;
} worker_arg_t;

static long g_entry_seq = 0;

static void track_enter(worker_arg_t *a) {
    tracker_t *t = a->trk;
    pthread_mutex_lock(&t->mu);
    if (a->is_read) {
        t->readers++;
        if (t->writers > 0) t->violation = 1;
        if (t->readers > t->max_readers) t->max_readers = t->readers;
    } else {
        t->writers++;
        if (t->writers > 1 || t->readers > 0) t->violation = 1;
    }
    pthread_mutex_unlock(&t->mu);
    a->entry_seq = __atomic_fetch_add(&g_entry_seq, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&a->entered, 1, __ATOMIC_SEQ_CST);
}

static void track_exit(worker_arg_t *a) {
    tracker_t *t = a->trk;
    pthread_mutex_lock(&t->mu);
    if (a->is_read) t->readers--;
    else t->writers--;
    pthread_mutex_unlock(&t->mu);
    __atomic_store_n(&a->entered, 0, __ATOMIC_SEQ_CST);
}

static void tracker_reset(tracker_t *t) {
    pthread_mutex_lock(&t->mu);
    t->readers = 0;
    t->writers = 0;
    t->violation = 0;
    t->max_readers = 0;
    pthread_mutex_unlock(&t->mu);
}

/* ---- Replica of _pthread_rwlock_modbits (pthread_rwlock.c L896-937) ---- */
static uint32_t replica_modbits(uint32_t lval, uint32_t uval, uint32_t savebits) {
    /* Mask updateval to its low byte: the result keeps the OLD count
     * field, the count advance lives in S. */
    uint32_t nlval = (lval | (uval & PTHRW_BIT_MASK)) & ~PTH_RWL_MBIT;
    if ((uval & PTH_RWL_KBIT) == 0 && (lval & PTH_RWL_WBIT) == 0)
        nlval &= ~PTH_RWL_KBIT;
    if ((savebits & PTH_RWS_WSVBIT) != 0 &&
        (nlval & (PTH_RWL_WBIT | PTH_RWL_EBIT)) == 0)
        nlval |= (PTH_RWL_WBIT | PTH_RWL_KBIT);
    return nlval;
}

/* ---- Replica of _pthread_rwlock_updateval (pthread_rwlock.c L944-994) ---- */
static void replica_updateval(rwlock_t *rw, uint32_t updateval) {
    for (;;) {
        uint64_t old = ls_load(rw);
        uint32_t oldL = (uint32_t)old;
        uint32_t oldS = (uint32_t)(old >> 32);
        uint32_t isoverlap = updateval & PTH_RWL_MBIT;
        if (isoverlap == 0 && (oldS & PTH_RWS_IBIT) == 0) return; /* no-op */
        uint32_t newL = replica_modbits(oldL, updateval, oldS & PTHRW_RWS_SAVEMASK);
        uint32_t newS;
        /* PATCHED: real _pthread_rwlock_updateval (pthread_rwlock.c
         * L976-984) ADVANCES the S count by the granted count in both
         * cases; a non-overlap grant then masks S to the count field
         * only. The replica used to REPLACE the S count here, which
         * left S one generation behind: the holder's unlock then saw
         * newU != S count, took no kernel transition, and the next
         * waiter was never granted. */
        newS = oldS + (updateval & PTHRW_COUNT_MASK);
        if (!isoverlap) {
            newS &= PTHRW_COUNT_MASK;
        }
        newS &= ~PTHRW_RWS_SAVEMASK;
        uint64_t want = ((uint64_t)newS << 32) | newL;
        uint64_t cur = old;
        if (ls_cas(rw, &cur, want)) return;
    }
}

/* ---- Replica of _pthread_rwlock_lock_wait (pthread_rwlock.c L1209-1255) ---- */
static int replica_lock_wait(rwlock_t *rw, int is_read, uint32_t lgen,
                             uint32_t ugen, uint32_t wc, worker_arg_t *info) {
    info->took_slow_path = 1;
    for (;;) {
        uint32_t ret;
        info->psynch_calls++;
        if (is_read)
            ret = lsk_psynch_rw_rdlock((uintptr_t)rw, lgen, ugen, wc, (int)rw->rw_flags);
        else
            ret = lsk_psynch_rw_wrlock((uintptr_t)rw, lgen, ugen, wc, (int)rw->rw_flags);
        if (ret == (uint32_t)-1) {
            if (g_last_errno == DARWIN_EINTR) continue; /* retry, same seq */
            info->psynch_error = g_last_errno;
            return -1; /* real libpthread takes PTHREAD_INTERNAL_CRASH here */
        }
        info->last_updatebits = ret;
        if (ret == 0) info->zero_grant_slow++;
        replica_updateval(rw, ret);
        return 0;
    }
}

/* ---- Replica of _pthread_rwlock_lock fast path + _pthread_rwlock_lock_slow ---- */
static int replica_lock(rwlock_t *rw, int is_read, worker_arg_t *info) {
    for (;;) {
        uint64_t old = ls_load(rw);
        uint32_t oldL = (uint32_t)old;
        uint32_t oldS = (uint32_t)(old >> 32);
        uint32_t newL, newS;
        int fast_ok = 0;
        if (is_read) {
            if ((oldL & (PTH_RWL_WBIT | PTH_RWL_KBIT)) == 0) {
                fast_ok = 1;
                newL = (oldL & ~PTH_RWL_UBIT) + PTHRW_INC;
                /* PATCHED: real fast path increments S too
                 * (pthread_rwlock.c _pthread_rwlock_lock: newseq.lcntval
                 * += INC; newseq.rw_seq += INC for both read and write).
                 * The replica left S unchanged on a fast read, so S fell
                 * behind L and the unlock transition (newU == S count)
                 * fired at the wrong generation. */
                newS = oldS + PTHRW_INC;
            }
        } else {
            if ((oldL & PTH_RWL_UBIT) != 0) {
                fast_ok = 1;
                /* PATCHED: real fast write keeps the old L count and
                 * adds INC (newseq.lcntval &= COUNT; |= IBIT|KBIT|EBIT;
                 * += INC). The replica reset the count to 0x100 every
                 * time (INIT|KBIT|EBIT|INC), so L fell behind U and S
                 * and unlocks computed a phantom diff. */
                newL = (oldL & PTHRW_COUNT_MASK) | PTH_RWL_IBIT | PTH_RWL_KBIT | PTH_RWL_EBIT;
                newL += PTHRW_INC;
                newS = oldS + PTHRW_INC;
            }
        }
        if (fast_ok) {
            uint64_t want = ((uint64_t)newS << 32) | newL;
            uint64_t cur = old;
            if (ls_cas(rw, &cur, want)) {
                info->took_slow_path = 0;
                return 0;
            }
            continue; /* CAS lost, retry */
        }
        /* Slow path: read clears UBIT, write sets KBIT|WBIT, L += INC;
         * if S has SBIT, clear it and set S count to the OLD L count. */
        newL = oldL;
        if (is_read) newL &= ~PTH_RWL_UBIT;
        else newL |= (PTH_RWL_KBIT | PTH_RWL_WBIT);
        newL += PTHRW_INC;
        newS = oldS;
        if ((oldS & PTH_RWS_SBIT) != 0) {
            newS &= ~PTH_RWS_SBIT;
            newS = (newS & PTHRW_BIT_MASK) | (oldL & PTHRW_COUNT_MASK); /* PATCHED: real libpthread uses OLD L count here (pthread_rwlock.c _pthread_rwlock_lock_slow: newseq.rw_seq |= (oldseq.lcntval & PTHRW_COUNT_MASK)) */
        }
        uint64_t want = ((uint64_t)newS << 32) | newL;
        uint64_t cur = old;
        if (!ls_cas(rw, &cur, want)) continue; /* CAS lost, retry */
        /* CAS won: lock_wait with the new L, current U, current S. */
        return replica_lock_wait(rw, is_read, newL, u_load(rw), newS, info);
    }
}

/* ---- Replica of _pthread_rwlock_unlock / unlock_slow / unlock_drop ----
 * One logical unlock updates L|S and U together. The real libpthread
 * does it with a single 128-bit CAS; this replica used to do an
 * LS-CAS first and a U-CAS second, which left a window where a
 * waiter could increment L between the two CAS operations: the
 * kernel was then called with a stale oldL, computed a phantom
 * diff, preposted for a waiter that never arrives, and the real
 * waiters (higher seqs) were stranded with no holder left to unlock
 * them. PATCHED: single 128-bit CAS over the whole seq area, exactly
 * like the real libpthread, so oldL and newU are one atomic
 * snapshot. */
static unsigned __int128 seq128_load(rwlock_t *rw) {
    return __atomic_load_n((unsigned __int128 *)&rw->seq, __ATOMIC_SEQ_CST);
}
static int seq128_cas(rwlock_t *rw, unsigned __int128 *expected, unsigned __int128 want) {
    return __atomic_compare_exchange_n((unsigned __int128 *)&rw->seq, expected, want, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static int replica_unlock(rwlock_t *rw, worker_arg_t *info) {
    for (;;) {
        unsigned __int128 old128 = seq128_load(rw);
        uint32_t oldL = (uint32_t)old128;
        uint32_t oldS = (uint32_t)(old128 >> 32);
        uint32_t oldU = (uint32_t)(old128 >> 64);
        if ((oldL & PTH_RWL_UBIT) != 0) return 0; /* spurious unlock */
        uint32_t newU = oldU + PTHRW_INC;
        uint32_t newL, newS;
        int slow = 0;
        if ((newU & PTHRW_COUNT_MASK) == (oldL & PTHRW_COUNT_MASK)) {
            /* Last unlock: pure userland, no kernel call. */
            newL = (oldL & PTHRW_COUNT_MASK) | PTHRW_RWLOCK_INIT;
            newS = (oldL & PTHRW_COUNT_MASK) | PTHRW_RWS_INIT;
        } else if ((oldL & (PTH_RWL_EBIT | PTH_RWL_WBIT | PTH_RWL_KBIT)) == 0 ||
                   (newU & PTHRW_COUNT_MASK) != (oldS & PTHRW_COUNT_MASK)) {
            /* No kernel transition needed: L|S unchanged. */
            newL = oldL;
            newS = oldS;
        } else {
            /* Slow path: L = oldcount | KBIT, S |= IBIT, plus WSVBIT if
             * WBIT was set; then unlock_drop with OLD L and NEW U/S. */
            slow = 1;
            newL = (oldL & PTHRW_COUNT_MASK) | PTH_RWL_KBIT;
            newS = oldS | PTH_RWS_IBIT;
            if ((oldL & PTH_RWL_WBIT) != 0) newS |= PTH_RWS_WSVBIT;
        }
        unsigned __int128 want128 = ((unsigned __int128)newU << 64) |
                                     ((unsigned __int128)newS << 32) |
                                     (unsigned __int128)newL;
        want128 |= (old128 & (unsigned __int128)0xFFFFFFFFULL << 96);
        unsigned __int128 cur128 = old128;
        if (!seq128_cas(rw, &cur128, want128)) continue; /* changed, retry */
        if (!slow) return 0;
        info->last_unlock_lgen = oldL;
        info->last_unlock_ugen = newU;
        info->last_unlock_wc = newS;
        for (;;) {
            uint32_t ret = lsk_psynch_rw_unlock((uintptr_t)rw, oldL, newU, newS,
                                                (int)rw->rw_flags);
            info->last_unlock_ret = ret;
            if (ret == (uint32_t)-1) {
                if (g_last_errno == DARWIN_EINTR) continue; /* retry */
                info->psynch_error = g_last_errno;
                return -1;
            }
            return 0;
        }
    }
}

/* ---- Timing and wait helpers ---- */
static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Wait up to timeout_ms for *flag to become 1. Returns 1 if set. */
static int wait_flag(volatile int *flag, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (__atomic_load_n(flag, __ATOMIC_SEQ_CST) != 0) return 1;
        msleep(1);
    }
    return __atomic_load_n(flag, __ATOMIC_SEQ_CST) != 0;
}

/* Wait up to timeout_ms for (L & mask) == want. Returns 1 if reached. */
static int wait_l_bits(rwlock_t *rw, uint32_t mask, uint32_t want, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if ((l_load(rw) & mask) == want) return 1;
        msleep(1);
    }
    return (l_load(rw) & mask) == want;
}

/* Join a worker, bounded: if its done flag is not set within timeout_ms,
 * the thread is stuck (lost wakeup) and the process must fail fast
 * instead of hanging the suite. Returns 1 on clean join. */
static int join_bounded(pthread_t th, worker_arg_t *a, int timeout_ms, const char *what) {
    if (!wait_flag(&a->done, timeout_ms)) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL stuck thread: %s did not finish in %d ms (lost wakeup?)\n",
                what, timeout_ms);
        return 0;
    }
    pthread_join(th, NULL);
    return 1;
}

static void spawn_or_fail(pthread_t *th, void *(*fn)(void *), worker_arg_t *a, const char *what) {
    int rc = pthread_create(th, NULL, fn, a);
    if (rc != 0) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL pthread_create %s: %d\n", what, rc);
        __atomic_store_n(&a->done, 1, __ATOMIC_SEQ_CST);
    }
}

static tracker_t g_trk_main = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0 };

static void worker_init(worker_arg_t *a, rwlock_t *rw, tracker_t *trk, int is_read) {
    memset(a, 0, sizeof(*a));
    a->rw = rw;
    a->trk = trk;
    a->is_read = is_read;
    a->entry_seq = -1;
}

/* Holder thread: lock once, sit inside until release is set, unlock. */
static void *worker_hold(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;
    __atomic_store_n(&a->attempting, 1, __ATOMIC_SEQ_CST);
    if (replica_lock(a->rw, a->is_read, a) != 0) a->failed = 1;
    else {
        track_enter(a);
        __atomic_store_n(&a->holding, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&a->release, __ATOMIC_SEQ_CST) == 0) msleep(1);
        track_exit(a);
        __atomic_store_n(&a->holding, 0, __ATOMIC_SEQ_CST);
        if (replica_unlock(a->rw, a) != 0) a->failed = 1;
    }
    __atomic_store_n(&a->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* Once thread: lock, enter, unlock immediately. */
static void *worker_once(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;
    __atomic_store_n(&a->attempting, 1, __ATOMIC_SEQ_CST);
    if (replica_lock(a->rw, a->is_read, a) != 0) a->failed = 1;
    else {
        track_enter(a);
        track_exit(a);
        if (replica_unlock(a->rw, a) != 0) a->failed = 1;
    }
    __atomic_store_n(&a->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* ---- Signal handling for the interrupted-wakeup test ---- */
static volatile sig_atomic_t g_sigusr1_seen = 0;
static void sigusr1_handler(int sig) {
    (void)sig;
    g_sigusr1_seen = 1;
}

/* ---- Field crash shapes: dictionary-like get, array-like insert ---- */
#define DICT_SLOTS 1024
#define DICT_KEYS 1000
static uint32_t g_dict_keys[DICT_SLOTS];
static uint32_t g_dict_vals[DICT_SLOTS];

static uint32_t dict_hash(uint32_t key) {
    return (key * 2654435761u) >> (32 - 10);
}

/* Caller must hold the replica lock (read for get, write for put). */
static int dict_get(uint32_t key, uint32_t *val_out) {
    uint32_t slot = dict_hash(key);
    for (int i = 0; i < DICT_SLOTS; i++) {
        uint32_t at = (slot + (uint32_t)i) & (DICT_SLOTS - 1);
        uint32_t k = g_dict_keys[at];
        if (k == key) {
            *val_out = g_dict_vals[at];
            return 1;
        }
        if (k == 0) return 0;
    }
    return 0; /* full table without the key: corrupt or exhausted */
}

static void dict_put(uint32_t key, uint32_t val) {
    uint32_t slot = dict_hash(key);
    for (int i = 0; i < DICT_SLOTS; i++) {
        uint32_t at = (slot + (uint32_t)i) & (DICT_SLOTS - 1);
        uint32_t k = g_dict_keys[at];
        if (k == key || k == 0) {
            g_dict_keys[at] = key;
            g_dict_vals[at] = val;
            return;
        }
    }
    /* Table full: a real insert would grow or fail; here it means the
     * workload exceeded the table, which the test sizes to avoid. */
    fprintf(stderr, "FAIL dict_put: table full for key %u\n", key);
    __atomic_fetch_add(&g_checks, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
}

static uint32_t *g_arr_buf = NULL;
static size_t g_arr_cap = 0;
static size_t g_arr_len = 0;

/* Caller must hold the replica write lock. */
static void arr_append(uint32_t v) {
    if (g_arr_len == g_arr_cap) {
        size_t ncap = g_arr_cap == 0 ? 64 : g_arr_cap * 2;
        uint32_t *nb = (uint32_t *)realloc(g_arr_buf, ncap * sizeof(uint32_t));
        if (!nb) {
            fprintf(stderr, "FAIL arr_append: realloc failed\n");
            __atomic_fetch_add(&g_checks, 1, __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
            return;
        }
        g_arr_buf = nb;
        g_arr_cap = ncap;
    }
    g_arr_buf[g_arr_len++] = v;
}

typedef struct {
    rwlock_t *rw;
    tracker_t *trk;
    int id;              /* writer id 0..3, reader id 0..3 */
    int is_writer;
    volatile int done;
    volatile int failed;
    long ops_done;
    int zero_grant_slow;
    int psynch_error;
    worker_arg_t info;   /* replica call record for this thread */
} stress_arg_t;

static void *stress_worker(void *arg) {
    stress_arg_t *s = (stress_arg_t *)arg;
    worker_init(&s->info, s->rw, s->trk, s->is_writer ? 0 : 1);
    if (s->is_writer) {
        /* Writer id w inserts keys w*250+1 .. w*250+250. */
        for (int i = 0; i < 250; i++) {
            uint32_t key = (uint32_t)(s->id * 250 + i + 1);
            if (replica_lock(s->rw, 0, &s->info) != 0) { s->failed = 1; break; }
            track_enter(&s->info);
            dict_put(key, key * 7u);
            arr_append(key);
            track_exit(&s->info);
            if (replica_unlock(s->rw, &s->info) != 0) { s->failed = 1; break; }
            s->ops_done++;
        }
    } else {
        for (int i = 0; i < 2000; i++) {
            uint32_t key = (uint32_t)(((i * 37 + s->id * 11) % DICT_KEYS) + 1);
            if (replica_lock(s->rw, 1, &s->info) != 0) { s->failed = 1; break; }
            track_enter(&s->info);
            uint32_t v = 0;
            if (dict_get(key, &v) && v != key * 7u) {
                fprintf(stderr, "FAIL dict_get: key %u value %u, want %u\n",
                        key, v, key * 7u);
                __atomic_fetch_add(&g_checks, 1, __ATOMIC_SEQ_CST);
                __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
                s->failed = 1;
            }
            if (g_arr_len > 0) {
                uint32_t el = g_arr_buf[(size_t)((i * 13 + s->id) % (int)g_arr_len)];
                if (el < 1 || el > DICT_KEYS) {
                    fprintf(stderr, "FAIL arr read: element %u out of range\n", el);
                    __atomic_fetch_add(&g_checks, 1, __ATOMIC_SEQ_CST);
                    __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
                    s->failed = 1;
                }
            }
            track_exit(&s->info);
            if (replica_unlock(s->rw, &s->info) != 0) { s->failed = 1; break; }
            s->ops_done++;
        }
    }
    s->zero_grant_slow = s->info.zero_grant_slow;
    s->psynch_error = s->info.psynch_error;
    __atomic_store_n(&s->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* Counter stress: many host pthreads, one shared counter under the lock. */
static long g_counter = 0;

typedef struct {
    rwlock_t *rw;
    tracker_t *trk;
    int is_writer;
    volatile int done;
    volatile int failed;
    long ops_done;
    worker_arg_t info;
} counter_arg_t;

static void *counter_worker(void *arg) {
    counter_arg_t *c = (counter_arg_t *)arg;
    worker_init(&c->info, c->rw, c->trk, c->is_writer ? 0 : 1);
    for (int i = 0; i < 500; i++) {
        if (replica_lock(c->rw, c->is_writer ? 0 : 1, &c->info) != 0) {
            c->failed = 1;
            break;
        }
        track_enter(&c->info);
        if (c->is_writer) g_counter++;
        else {
            long v = g_counter;
            if (v < 0 || v > 2000) {
                fprintf(stderr, "FAIL counter read: %ld out of range\n", v);
                __atomic_fetch_add(&g_checks, 1, __ATOMIC_SEQ_CST);
                __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
                c->failed = 1;
            }
        }
        track_exit(&c->info);
        if (replica_unlock(c->rw, &c->info) != 0) {
            c->failed = 1;
            break;
        }
        c->ops_done++;
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* ---- Scenario 1: reader after writer ---- */
static void test_reader_after_writer(void) {
    printf("scenario reader-after-writer\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t w, r;
    worker_init(&w, &rw, &g_trk_main, 0);
    worker_init(&r, &rw, &g_trk_main, 1);

    /* Main thread holds the write lock. */
    CHECK(replica_lock(&rw, 0, &w) == 0);
    track_enter(&w);

    pthread_t rth;
    spawn_or_fail(&rth, worker_once, &r, "reader-after-writer reader");
    /* Reader must NOT enter while the writer holds. */
    msleep(150);
    CHECK(__atomic_load_n(&r.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(g_trk_main.violation == 0);

    track_exit(&w);
    CHECK(replica_unlock(&rw, &w) == 0);
    CHECK(join_bounded(rth, &r, 5000, "reader-after-writer reader"));
    CHECK(r.failed == 0);
    CHECK(r.psynch_error == 0);
    CHECK(r.took_slow_path == 1);
    /* Read grant for one reader, no writers pending: exactly 0x100. */
    CHECK(r.last_updatebits == 0x100u);
    CHECK(r.zero_grant_slow == 0);
    CHECK(g_trk_main.violation == 0);
    printf("scenario reader-after-writer done\n");
}

/* ---- Scenario 2: writer after reader ---- */
static void test_writer_after_reader(void) {
    printf("scenario writer-after-reader\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t r, w;
    worker_init(&r, &rw, &g_trk_main, 1);
    worker_init(&w, &rw, &g_trk_main, 0);

    CHECK(replica_lock(&rw, 1, &r) == 0);
    track_enter(&r);

    pthread_t wth;
    spawn_or_fail(&wth, worker_once, &w, "writer-after-reader writer");
    msleep(150);
    CHECK(__atomic_load_n(&w.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(g_trk_main.violation == 0);

    track_exit(&r);
    CHECK(replica_unlock(&rw, &r) == 0);
    CHECK(join_bounded(wth, &w, 5000, "writer-after-reader writer"));
    CHECK(w.failed == 0);
    CHECK(w.psynch_error == 0);
    CHECK(w.took_slow_path == 1);
    /* Write grant, no more writers queued: exactly 0x103. */
    CHECK(w.last_updatebits == 0x103u);
    CHECK(w.zero_grant_slow == 0);
    CHECK(g_trk_main.violation == 0);
    printf("scenario writer-after-reader done\n");
}

/* ---- Scenario 3: writer after writer, lowest seq wins ---- */
static void test_writer_after_writer(void) {
    printf("scenario writer-after-writer\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t w1, w2, w3;
    worker_init(&w1, &rw, &g_trk_main, 0);
    worker_init(&w2, &rw, &g_trk_main, 0);
    worker_init(&w3, &rw, &g_trk_main, 0);

    CHECK(replica_lock(&rw, 0, &w1) == 0);
    track_enter(&w1);

    pthread_t t2, t3;
    spawn_or_fail(&t2, worker_hold, &w2, "writer-after-writer w2");
    /* Stagger so w2 queues before w3 (lower seq). */
    msleep(50);
    spawn_or_fail(&t3, worker_hold, &w3, "writer-after-writer w3");
    msleep(150);
    CHECK(__atomic_load_n(&w2.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_load_n(&w3.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(g_trk_main.violation == 0);

    track_exit(&w1);
    CHECK(replica_unlock(&rw, &w1) == 0);
    /* w2 (lowest seq) is granted first; w3 stays blocked. */
    CHECK(wait_flag(&w2.entered, 5000));
    msleep(150);
    CHECK(__atomic_load_n(&w3.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(w2.failed == 0);
    /* Write grant with another writer queued behind: exactly 0x107. */
    CHECK(w2.last_updatebits == 0x107u);
    CHECK(w2.zero_grant_slow == 0);

    __atomic_store_n(&w2.release, 1, __ATOMIC_SEQ_CST);
    CHECK(wait_flag(&w3.entered, 5000));
    CHECK(w3.failed == 0);
    /* Last writer: exactly 0x103. */
    CHECK(w3.last_updatebits == 0x103u);
    CHECK(w3.zero_grant_slow == 0);
    CHECK(w2.entry_seq < w3.entry_seq);
    __atomic_store_n(&w3.release, 1, __ATOMIC_SEQ_CST);
    CHECK(join_bounded(t2, &w2, 5000, "writer-after-writer w2"));
    CHECK(join_bounded(t3, &w3, 5000, "writer-after-writer w3"));
    CHECK(g_trk_main.violation == 0);
    printf("scenario writer-after-writer done\n");
}

/* ---- Scenario 4: many readers, writer queued, late readers must wait ---- */
static void test_writer_preference(void) {
    printf("scenario writer-preference\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t early[4], w, late[2];
    pthread_t eth[4], wth, lth[2];
    for (int i = 0; i < 4; i++) worker_init(&early[i], &rw, &g_trk_main, 1);
    worker_init(&w, &rw, &g_trk_main, 0);
    for (int i = 0; i < 2; i++) worker_init(&late[i], &rw, &g_trk_main, 1);

    for (int i = 0; i < 4; i++) {
        spawn_or_fail(&eth[i], worker_hold, &early[i], "early reader");
    }
    for (int i = 0; i < 4; i++) CHECK(wait_flag(&early[i].entered, 5000));

    /* Writer arrives and queues: its slow path sets WBIT in L. */
    spawn_or_fail(&wth, worker_hold, &w, "queued writer");
    CHECK(wait_l_bits(&rw, PTH_RWL_WBIT, PTH_RWL_WBIT, 5000));
    msleep(50);
    CHECK(__atomic_load_n(&w.entered, __ATOMIC_SEQ_CST) == 0);

    /* Late readers arrive after the writer queued: they must NOT be
     * granted while the writer is pending (writer-preference rule). */
    for (int i = 0; i < 2; i++) {
        spawn_or_fail(&lth[i], worker_hold, &late[i], "late reader");
    }
    msleep(150);
    CHECK(__atomic_load_n(&late[0].entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_load_n(&late[1].entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_load_n(&w.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(g_trk_main.violation == 0);

    /* Release the early readers: the writer must enter before either
     * late reader. */
    for (int i = 0; i < 4; i++) __atomic_store_n(&early[i].release, 1, __ATOMIC_SEQ_CST);
    CHECK(wait_flag(&w.entered, 5000));
    msleep(100);
    CHECK(__atomic_load_n(&late[0].entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_load_n(&late[1].entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(w.failed == 0);
    CHECK(w.last_updatebits == 0x103u);
    CHECK(w.zero_grant_slow == 0);

    __atomic_store_n(&w.release, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2; i++) CHECK(wait_flag(&late[i].entered, 5000));
    CHECK(late[0].failed == 0);
    CHECK(late[1].failed == 0);
    /* Read grant for the two late readers together: count 0x200. */
    CHECK((late[0].last_updatebits & PTHRW_COUNT_MASK) == 0x200u);
    CHECK((late[1].last_updatebits & PTHRW_COUNT_MASK) == 0x200u);
    CHECK(late[0].zero_grant_slow == 0);
    CHECK(late[1].zero_grant_slow == 0);
    CHECK(w.entry_seq < late[0].entry_seq);
    CHECK(w.entry_seq < late[1].entry_seq);
    for (int i = 0; i < 2; i++) __atomic_store_n(&late[i].release, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 4; i++) CHECK(join_bounded(eth[i], &early[i], 5000, "early reader"));
    CHECK(join_bounded(wth, &w, 5000, "queued writer"));
    for (int i = 0; i < 2; i++) CHECK(join_bounded(lth[i], &late[i], 5000, "late reader"));
    CHECK(g_trk_main.violation == 0);
    printf("scenario writer-preference done\n");
}

/* ---- Scenario 5: prepost (unlock arriving before the waiter) ---- */
static void test_prepost(void) {
    printf("scenario prepost\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);

    /* Coherent word story, traced from the replica (see header):
     * fresh lock L=0xC0 S=0x01 U=0; writer fast-acquires (L=0x183,
     * S=0x101); reader A slow-attempts (L=0x283, S=0x200) but has not
     * called rdlock yet; writer unlocks with OLD L and NEW U/S. The
     * waiter is expected (diff 1) but not queued, so unlock must
     * prepost and return updatebits = lgenval. */
    uint32_t ret = lsk_psynch_rw_unlock((uintptr_t)&rw, 0x283u, 0x100u, 0x202u, 0x20);
    fprintf(stderr, "prepost unlock returned 0x%x (want lgenval 0x283)\n", ret);
    CHECK(ret == 0x283u);

    /* Reader A now arrives with the same seq and consumes the prepost
     * instead of blocking: read grant for one reader, count 0x100. */
    ret = lsk_psynch_rw_rdlock((uintptr_t)&rw, 0x283u, 0x100u, 0x200u, 0x20);
    fprintf(stderr, "prepost rdlock returned 0x%x (want count 0x100)\n", ret);
    CHECK(ret != 0u);
    CHECK((ret & PTHRW_COUNT_MASK) == 0x100u);

    /* Threaded prepost race: 20 rounds of holder-unlock vs waiter
     * arrival. No lost wakeup: the waiter must always enter. */
    for (int round = 0; round < 20; round++) {
        static rwlock_t r2 __attribute__((aligned(16)));
        rwlock_init(&r2);
        worker_arg_t h, w;
        worker_init(&h, &r2, &g_trk_main, 0);
        worker_init(&w, &r2, &g_trk_main, 1);
        CHECK(replica_lock(&r2, 0, &h) == 0);
        track_enter(&h);
        pthread_t wth;
        spawn_or_fail(&wth, worker_once, &w, "prepost race waiter");
        /* Unlock immediately: the waiter may or may not have enqueued
         * yet, which is exactly the prepost race. */
        track_exit(&h);
        CHECK(replica_unlock(&r2, &h) == 0);
        CHECK(join_bounded(wth, &w, 5000, "prepost race waiter"));
        CHECK(w.failed == 0);
        CHECK(w.zero_grant_slow == 0);
    }
    CHECK(g_trk_main.violation == 0);
    printf("scenario prepost done\n");
}

/* ---- Scenario 6: interrupted wakeup (signal during the wait, then retry) ---- */
static void test_interrupted_wakeup(void) {
    printf("scenario interrupted-wakeup\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t w, r;
    worker_init(&w, &rw, &g_trk_main, 0);
    worker_init(&r, &rw, &g_trk_main, 1);

    CHECK(replica_lock(&rw, 0, &w) == 0);
    track_enter(&w);

    pthread_t rth;
    spawn_or_fail(&rth, worker_once, &r, "interrupted waiter");
    /* Gate on the waiter being blocked in the slow path: L must still
     * carry KBIT (writer hold) and the waiter must not have entered. */
    msleep(150);
    CHECK(__atomic_load_n(&r.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK((l_load(&rw) & PTH_RWL_KBIT) != 0);

    /* Interrupt the futex wait with SIGUSR1 (handler installed without
     * SA_RESTART, so the wait returns EINTR). The replica retries with
     * the same seq; a grant that lands while the waiter is out must be
     * recorded as an interrupted wakeup, not dropped. Retry the signal
     * until the waiter has demonstrably re-entered the psynch call. */
    g_sigusr1_seen = 0;
    int interrupted = 0;
    for (int attempt = 0; attempt < 3 && !interrupted; attempt++) {
        int calls_before = r.psynch_calls;
        pthread_kill(rth, SIGUSR1);
        msleep(100);
        if (g_sigusr1_seen && r.psynch_calls > calls_before) interrupted = 1;
        /* If the waiter already left the wait (grant race), stop. */
        if (__atomic_load_n(&r.entered, __ATOMIC_SEQ_CST) != 0) break;
    }
    fprintf(stderr, "interrupted-wakeup: signal seen=%d psynch_calls=%d\n",
            (int)g_sigusr1_seen, r.psynch_calls);
    CHECK(g_sigusr1_seen == 1);
    CHECK(interrupted == 1);
    /* Still blocked: the signal must not have granted the lock. */
    CHECK(__atomic_load_n(&r.entered, __ATOMIC_SEQ_CST) == 0);

    track_exit(&w);
    CHECK(replica_unlock(&rw, &w) == 0);
    CHECK(join_bounded(rth, &r, 5000, "interrupted waiter"));
    CHECK(r.failed == 0);
    CHECK(r.psynch_error == 0);
    CHECK(r.took_slow_path == 1);
    CHECK(r.last_updatebits != 0u);
    CHECK((r.last_updatebits & PTHRW_COUNT_MASK) == 0x100u);
    CHECK(r.zero_grant_slow == 0);
    CHECK(g_trk_main.violation == 0);
    printf("scenario interrupted-wakeup done\n");
}

/* ---- Scenario 7: overlap (second reader while a read grant is outstanding) ---- */
static void test_overlap(void) {
    printf("scenario overlap\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t w, r1;
    worker_init(&w, &rw, &g_trk_main, 0);
    worker_init(&r1, &rw, &g_trk_main, 1);

    CHECK(replica_lock(&rw, 0, &w) == 0);
    track_enter(&w);

    pthread_t t1;
    spawn_or_fail(&t1, worker_hold, &r1, "overlap first reader");
    msleep(150);
    CHECK(__atomic_load_n(&r1.entered, __ATOMIC_SEQ_CST) == 0);

    track_exit(&w);
    CHECK(replica_unlock(&rw, &w) == 0);
    CHECK(wait_flag(&r1.entered, 5000));
    CHECK(r1.failed == 0);
    CHECK(r1.last_updatebits == 0x100u);
    /* r1 holds a read share granted with no writers pending, so the
     * overlap guard is set. A second reader arriving now with a
     * slow-path seq inside the granted range must be admitted by the
     * overlap path without blocking: updatebits carries MBIT (0x40)
     * and count 0x100. Traced words after r1's grant: L=0x200,
     * S=0x100, U=0x100, so the probe slow-path L is 0x300. */
    CHECK(l_load(&rw) == 0x200u);
    uint32_t ret = lsk_psynch_rw_rdlock((uintptr_t)&rw, 0x300u, 0x100u, 0x100u, 0x20);
    fprintf(stderr, "overlap probe rdlock returned 0x%x (want MBIT 0x40, count 0x100)\n", ret);
    CHECK(ret != 0u);
    CHECK((ret & PTH_RWL_MBIT) != 0u);
    CHECK((ret & PTHRW_COUNT_MASK) == 0x100u);

    __atomic_store_n(&r1.release, 1, __ATOMIC_SEQ_CST);
    CHECK(join_bounded(t1, &r1, 5000, "overlap first reader"));
    CHECK(g_trk_main.violation == 0);
    printf("scenario overlap done\n");
}

/* ---- Scenario 8: stale unlock (same generation unlocked twice) ---- */
static void test_stale_unlock(void) {
    printf("scenario stale-unlock\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    worker_arg_t w, r;
    worker_init(&w, &rw, &g_trk_main, 0);
    worker_init(&r, &rw, &g_trk_main, 1);

    CHECK(replica_lock(&rw, 0, &w) == 0);
    track_enter(&w);

    pthread_t rth;
    spawn_or_fail(&rth, worker_once, &r, "stale-unlock reader");
    msleep(150);
    CHECK(__atomic_load_n(&r.entered, __ATOMIC_SEQ_CST) == 0);

    /* The real unlock must grant the reader (nonzero updatebits). */
    track_exit(&w);
    CHECK(replica_unlock(&rw, &w) == 0);
    CHECK(w.last_unlock_ret != 0u);
    CHECK(join_bounded(rth, &r, 5000, "stale-unlock reader"));
    CHECK(r.failed == 0);
    CHECK(r.last_updatebits == 0x100u);

    /* The same generation unlocked twice: an exact replay of the most
     * recent unlock is NOT caught by the kernel's stale check, so it
     * must return lgenval again, not 0. Adjudicated 2026-11-18 against
     * the kern_synch.c snapshot:
     * the stale check is strict is_seqlower(ugenval,
     * kw_lastunlockseq), guarded by kw_lastunlockseq !=
     * PTHRW_RWL_INIT. The real unlock above ran handle_unlock, which
     * set kw_lastunlockseq = rw_wc & COUNT = 0x100, and the replay
     * passes ugenval = 0x100, so the counts are equal and the check
     * does not fire; and if the original unlock cleared INITCLEARED,
     * _ksyn_check_init resets kw_lastunlockseq to PTHRW_RWL_INIT
     * (0x80) before the stale check, so the guard skips it entirely.
     * Either way diff = 1 with empty queues goes to prepost
     * (prepost step 4) and returns lgenval. The stale check only
     * catches a replay of an OLDER generation, whose ugenval is
     * strictly below kw_lastunlockseq. The earlier expectation of 0
     * assumed the check would catch an exact replay; it does not.
     * unlock2 is specified as an exact alias of unlock, so it must
     * agree. */
    uint32_t stale = lsk_psynch_rw_unlock((uintptr_t)&rw, w.last_unlock_lgen,
                                          w.last_unlock_ugen, w.last_unlock_wc, 0x20);
    fprintf(stderr, "stale unlock returned 0x%x (want lgenval 0x%x)\n", stale, w.last_unlock_lgen);
    CHECK(stale == w.last_unlock_lgen);
    uint32_t stale2 = lsk_psynch_rw_unlock2((uintptr_t)&rw, w.last_unlock_lgen,
                                            w.last_unlock_ugen, w.last_unlock_wc, 0x20);
    fprintf(stderr, "stale unlock2 returned 0x%x (want lgenval 0x%x)\n", stale2, w.last_unlock_lgen);
    CHECK(stale2 == w.last_unlock_lgen);
    CHECK(g_trk_main.violation == 0);
    printf("scenario stale-unlock done\n");
}

/* ---- Scenario 9: lock address above 4 GiB (pointer-truncation regression) ---- */
static void test_high_address(void) {
    printf("scenario high-address\n");
    /* Two locks with the same low 32 bits but different high bits: a
     * uint32_t first argument would key both to the same queue. Each
     * lock gets a writer plus a blocked reader; unlocking A must grant
     * only A's reader, never B's. mmap with a fixed hint above 4 GiB:
     * the kernel honors the hint when the range is free. If no candidate
     * pair maps, the subtest is skipped, not failed (documented). */
    static const uintptr_t bases[][2] = {
        { 0x100000000ULL, 0x200000000ULL },
        { 0x300000000ULL, 0x400000000ULL },
        { 0x1000000000ULL, 0x2000000000ULL },
    };
    rwlock_t *la = NULL, *lb = NULL;
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        void *a = mmap((void *)bases[i][0], 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (a == MAP_FAILED) continue;
        void *b = mmap((void *)bases[i][1], 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (b == MAP_FAILED) {
            munmap(a, 4096);
            continue;
        }
        la = (rwlock_t *)a;
        lb = (rwlock_t *)b;
        break;
    }
    if (!la || !lb) {
        g_skipped++;
        printf("scenario high-address SKIPPED: no fixed mapping above 4 GiB\n");
        return;
    }
    fprintf(stderr, "high-address locks at %p and %p (low32 both 0x%08x)\n",
            (void *)la, (void *)lb, (uint32_t)(uintptr_t)la);
    CHECK((uintptr_t)la > 0xFFFFFFFFULL);
    CHECK((uintptr_t)lb > 0xFFFFFFFFULL);
    CHECK(((uintptr_t)la & 0xFFFFFFFFULL) == ((uintptr_t)lb & 0xFFFFFFFFULL));
    rwlock_init(la);
    rwlock_init(lb);

    tracker_t trk_a = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0 };
    tracker_t trk_b = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0 };
    worker_arg_t wa, ra, wb, rb;
    worker_init(&wa, la, &trk_a, 0);
    worker_init(&ra, la, &trk_a, 1);
    worker_init(&wb, lb, &trk_b, 0);
    worker_init(&rb, lb, &trk_b, 1);

    CHECK(replica_lock(la, 0, &wa) == 0);
    track_enter(&wa);
    pthread_t tbh, tra, trb;
    spawn_or_fail(&tra, worker_once, &ra, "high-address reader A");
    spawn_or_fail(&tbh, worker_hold, &wb, "high-address writer B");
    CHECK(wait_flag(&wb.entered, 5000));
    spawn_or_fail(&trb, worker_once, &rb, "high-address reader B");
    msleep(150);
    CHECK(__atomic_load_n(&ra.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(__atomic_load_n(&rb.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(trk_a.violation == 0);
    CHECK(trk_b.violation == 0);

    /* Unlock A: only A's reader may enter. B's reader must stay blocked
     * behind B's writer even though both locks share low32 bits. */
    track_exit(&wa);
    CHECK(replica_unlock(la, &wa) == 0);
    CHECK(join_bounded(tra, &ra, 5000, "high-address reader A"));
    CHECK(ra.failed == 0);
    CHECK(ra.last_updatebits == 0x100u);
    msleep(150);
    CHECK(__atomic_load_n(&rb.entered, __ATOMIC_SEQ_CST) == 0);
    CHECK(trk_b.violation == 0);

    __atomic_store_n(&wb.release, 1, __ATOMIC_SEQ_CST);
    CHECK(join_bounded(tbh, &wb, 5000, "high-address writer B"));
    CHECK(join_bounded(trb, &rb, 5000, "high-address reader B"));
    CHECK(rb.failed == 0);
    CHECK(rb.last_updatebits == 0x100u);
    CHECK(trk_a.violation == 0);
    CHECK(trk_b.violation == 0);
    munmap(la, 4096);
    munmap(lb, 4096);
    printf("scenario high-address done\n");
}

/* ---- Scenario 10: field crash shapes as data-structure races ---- */
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void test_field_shapes(void) {
    printf("scenario field-shapes\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    memset(g_dict_keys, 0, sizeof(g_dict_keys));
    memset(g_dict_vals, 0, sizeof(g_dict_vals));
    free(g_arr_buf);
    g_arr_buf = NULL;
    g_arr_cap = 0;
    g_arr_len = 0;

    /* 4 writers x 250 distinct inserts (NSDictionaryM objectForKey /
     * NSArrayM insertObject shape: concurrent get vs insert under one
     * contended lock), 4 readers x 2000 lookups. A broken lock loses
     * inserts, returns wrong values, or corrupts the realloc. */
    stress_arg_t args[8];
    pthread_t th[8];
    for (int i = 0; i < 4; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].rw = &rw;
        args[i].trk = &g_trk_main;
        args[i].id = i;
        args[i].is_writer = 1;
        spawn_or_fail(&th[i], stress_worker, (worker_arg_t *)(void *)&args[i], "field-shape writer");
    }
    for (int i = 0; i < 4; i++) {
        memset(&args[4 + i], 0, sizeof(args[4 + i]));
        args[4 + i].rw = &rw;
        args[4 + i].trk = &g_trk_main;
        args[4 + i].id = i;
        args[4 + i].is_writer = 0;
        spawn_or_fail(&th[4 + i], stress_worker, (worker_arg_t *)(void *)&args[4 + i], "field-shape reader");
    }
    for (int i = 0; i < 8; i++) {
        if (!wait_flag(&args[i].done, 30000)) {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL stuck thread: field-shape worker %d (lost wakeup?)\n", i);
            break;
        }
        pthread_join(th[i], NULL);
        char what[32];
        snprintf(what, sizeof(what), "field-shape worker %d", i);
        CHECK(args[i].failed == 0);
        CHECK(args[i].psynch_error == 0);
        CHECK(args[i].zero_grant_slow == 0);
        (void)what;
    }
    CHECK(g_trk_main.violation == 0);

    /* Final state, single-threaded: every key present with the right
     * value, and the array holds 1..1000 exactly once. */
    for (uint32_t key = 1; key <= DICT_KEYS; key++) {
        uint32_t v = 0;
        if (!dict_get(key, &v)) {
            fprintf(stderr, "FAIL dict final: key %u missing\n", key);
            g_checks++;
            g_failures++;
            break;
        }
        if (v != key * 7u) {
            fprintf(stderr, "FAIL dict final: key %u value %u\n", key, v);
            g_checks++;
            g_failures++;
            break;
        }
    }
    g_checks++;
    if (g_arr_len != DICT_KEYS) {
        fprintf(stderr, "FAIL array final: len %zu, want %u\n", g_arr_len, DICT_KEYS);
        g_failures++;
    } else {
        qsort(g_arr_buf, g_arr_len, sizeof(uint32_t), cmp_u32);
        int ok = 1;
        for (size_t i = 0; i < g_arr_len; i++) {
            if (g_arr_buf[i] != (uint32_t)(i + 1)) { ok = 0; break; }
        }
        if (!ok) {
            fprintf(stderr, "FAIL array final: contents are not 1..1000 exactly once\n");
            g_failures++;
        }
    }
    printf("scenario field-shapes done\n");
}

/* ---- Scenario 11: many host pthreads, shared counter ---- */
static void test_counter_stress(void) {
    printf("scenario counter-stress\n");
    tracker_reset(&g_trk_main);
    static rwlock_t rw __attribute__((aligned(16)));
    rwlock_init(&rw);
    g_counter = 0;

    counter_arg_t args[12];
    pthread_t th[12];
    for (int i = 0; i < 4; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].rw = &rw;
        args[i].trk = &g_trk_main;
        args[i].is_writer = 1;
        spawn_or_fail(&th[i], counter_worker, &args[i], "counter writer");
    }
    for (int i = 0; i < 8; i++) {
        memset(&args[4 + i], 0, sizeof(args[4 + i]));
        args[4 + i].rw = &rw;
        args[4 + i].trk = &g_trk_main;
        args[4 + i].is_writer = 0;
        spawn_or_fail(&th[4 + i], counter_worker, &args[4 + i], "counter reader");
    }
    for (int i = 0; i < 12; i++) {
        if (!wait_flag(&args[i].done, 30000)) {
            g_checks++;
            g_failures++;
            fprintf(stderr, "FAIL stuck thread: counter worker %d (lost wakeup?)\n", i);
            break;
        }
        pthread_join(th[i], NULL);
        CHECK(args[i].failed == 0);
        CHECK(args[i].ops_done == 500);
        CHECK(args[i].info.zero_grant_slow == 0);
        CHECK(args[i].info.psynch_error == 0);
    }
    /* 4 writers x 500 increments, all under the lock: exact total. */
    CHECK(g_counter == 2000);
    CHECK(g_trk_main.violation == 0);
    printf("scenario counter-stress done: counter=%ld\n", g_counter);
}

int main(void) {
    /* SIGUSR1 handler without SA_RESTART: the futex wait must return
     * EINTR when the signal lands, so the retry path is exercised. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        fprintf(stderr, "FAIL sigaction SIGUSR1\n");
        return 1;
    }

    test_reader_after_writer();
    test_writer_after_reader();
    test_writer_after_writer();
    test_writer_preference();
    test_prepost();
    test_interrupted_wakeup();
    test_overlap();
    test_stale_unlock();
    test_high_address();
    test_counter_stress();
    test_field_shapes();

    free(g_arr_buf);
    printf("TOTAL: %d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skipped);
    return g_failures == 0 ? 0 : 1;
}
