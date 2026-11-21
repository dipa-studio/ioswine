#include "linux.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

#ifndef SYS_nanosleep
#define SYS_nanosleep 101
#endif
#ifndef SYS_sched_yield
#define SYS_sched_yield 124
#endif

#ifdef THREAD_HOST_TEST
extern long (*g_thread_test_write_hook)(int fd, const void *buf, size_t len);
extern long thread_test_gettid(void);
extern int thread_test_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oset);
static inline long thread_write(int fd, const void *buf, size_t len) {
    if (g_thread_test_write_hook) return g_thread_test_write_hook(fd, buf, len);
    return sys_write(fd, buf, len);
}
static inline long thread_gettid(void) {
    return thread_test_gettid();
}
#else
static inline long thread_write(int fd, const void *buf, size_t len) {
    return sys_write(fd, buf, len);
}
static inline long thread_gettid(void) {
    return sys0(SYS_gettid);
}
#endif

static void *g_threadstart = 0;
void *g_wqthread = 0;
static uint64_t g_dispatchqueue_offset = 0;
static uint64_t g_tsd_offset = 0;
/* NOTE: g_kport_counter (was here, value 0x2000) is deleted: it
 * was written nowhere and read nowhere in the tree, and it broke
 * -Werror -Wall builds with -Wunused-variable. No behaviour change. */

/* Trace flag (IOSWINE_LOG), owned by ipc.c. The create/exit prints below
 * are gated on it, so a normal run stays quiet. */
extern int g_ipc_trace;

/* Single-write stack-buffered line formatting for async-signal-safe fd-2
 * trace and dump lines (one write() syscall per line). */
struct thread_buf {
    char data[320];
    size_t len;
};

static void tb_init(struct thread_buf *b) {
    b->len = 0;
}

static void tb_str(struct thread_buf *b, const char *s) {
    while (*s && b->len < sizeof(b->data) - 1) {
        b->data[b->len++] = *s++;
    }
}

static void tb_hex(struct thread_buf *b, uint64_t val) {
    char tmp[16];
    int idx = 16;
    if (val == 0) {
        if (b->len < sizeof(b->data) - 1) b->data[b->len++] = '0';
        return;
    }
    static const char hexd[] = "0123456789abcdef";
    while (val > 0 && idx > 0) {
        tmp[--idx] = hexd[val & 0xf];
        val >>= 4;
    }
    while (idx < 16 && b->len < sizeof(b->data) - 1) {
        b->data[b->len++] = tmp[idx++];
    }
}

static void tb_flush(struct thread_buf *b) {
    if (b->len > 0) {
        thread_write(2, b->data, b->len);
        b->len = 0;
    }
}

/* Live-thread registry (extended for workqueue/workloop in 048):
 * start routine + arg per spawned thread, so the SIGQUIT dump at a hang can
 * list both pthread_create and workqueue/workloop threads. Bounded and static,
 * never freed. Uses a 3-state atomic alive flag (0=free, 2=claiming, 1=live)
 * so concurrent spawns cannot clobber each other and the SIGQUIT dump stays
 * completely lock-free and async-signal-safe. */
#define MAX_THREAD_RECS 128
struct thread_rec {
    long tid;
    uint64_t func;
    uint64_t arg;
    uint64_t pthread;
    uint32_t kport;
    uint32_t wq_flags;
    int alive;
};
static struct thread_rec g_thread_recs[MAX_THREAD_RECS];

void lsk_thread_reg_add(long tid, void *func, void *arg, void *pthread, uint32_t kport, uint32_t wq_flags) {
    for (int i = 0; i < MAX_THREAD_RECS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_thread_recs[i].alive, &expected, 2, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            g_thread_recs[i].tid = tid;
            g_thread_recs[i].func = (uint64_t)func;
            g_thread_recs[i].arg = (uint64_t)arg;
            g_thread_recs[i].pthread = (uint64_t)pthread;
            g_thread_recs[i].kport = kport;
            g_thread_recs[i].wq_flags = wq_flags;
            __atomic_store_n(&g_thread_recs[i].alive, 1, __ATOMIC_RELEASE);
            return;
        }
    }
    /* Table full: fail silent. The trace line still prints, so the creation
     * is not lost, only the dump entry. */
}

void lsk_thread_reg_remove(uint32_t kport) {
    for (int i = 0; i < MAX_THREAD_RECS; i++) {
        if (__atomic_load_n(&g_thread_recs[i].alive, __ATOMIC_ACQUIRE) == 1 &&
            g_thread_recs[i].kport == kport) {
            __atomic_store_n(&g_thread_recs[i].alive, 0, __ATOMIC_RELEASE);
            return;
        }
    }
}

#ifdef THREAD_HOST_TEST
void lsk_thread_reg_reset(void) {
    for (int i = 0; i < MAX_THREAD_RECS; i++) {
        __atomic_store_n(&g_thread_recs[i].alive, 0, __ATOMIC_RELAXED);
        g_thread_recs[i].tid = 0;
        g_thread_recs[i].func = 0;
        g_thread_recs[i].arg = 0;
        g_thread_recs[i].pthread = 0;
        g_thread_recs[i].kport = 0;
        g_thread_recs[i].wq_flags = 0;
    }
}
#endif

static void thread_rec_add(long tid, void *func, void *arg, void *pthread, uint32_t kport) {
    lsk_thread_reg_add(tid, func, arg, pthread, kport, 0);
}

static void thread_rec_remove(uint32_t kport) {
    lsk_thread_reg_remove(kport);
}

/* Raw clone trampoline in assembly so the child starts on its new stack
 * and never returns into C parent context.
 */
#if defined(__aarch64__) && !defined(THREAD_HOST_TEST)
__asm__(
".text\n"
".globl ___iosw_raw_clone\n"
".p2align 2\n"
"___iosw_raw_clone:\n"
"    // Linux SYS_clone(flags=x0, newsp=x1, parent_tid=x2, tls=x3, child_tid=x4)\n"
"    mov  x8, #220\n"
"    svc  #0\n"
"    cbz  x0, 1f\n"
"    ret\n"
"\n"
"1:\n"
"    // Child running on new stack (sp was set to newsp)\n"
"    ldr  x16, [sp, #0]\n"   // threadstart
"    ldr  x0,  [sp, #8]\n"   // pthread (arg 0)
"    ldr  x1,  [sp, #16]\n"  // kport   (arg 1)
"    ldr  x2,  [sp, #24]\n"  // func    (arg 2)
"    ldr  x3,  [sp, #32]\n"  // arg     (arg 3)
"    ldr  x4,  [sp, #40]\n"  // stacksize (arg 4)
"    ldr  x5,  [sp, #48]\n"  // flags   (arg 5)
"    add  sp, sp, #64\n"
"    blr  x16\n"
"\n"
"    // If threadstart returns, exit the thread\n"
"    mov  x8, #93\n"         // SYS_exit
"    mov  x0, #0\n"
"    svc  #0\n"
"    b    .\n"
);
#endif

extern long __iosw_raw_clone(unsigned long flags, void *newsp, int *parent_tid, unsigned long tls, int *child_tid);

int lsk_bsdthread_register(void *threadstart, void *wqthread, int pthsize, void *dummy_val,
                           void *targetconc_ptr, uint64_t dispatchqueue_offset, uint64_t tsd_offset) __asm__("___bsdthread_register");
int lsk_bsdthread_register(void *threadstart, void *wqthread, int pthsize, void *dummy_val,
                           void *targetconc_ptr, uint64_t dispatchqueue_offset, uint64_t tsd_offset) {
    (void)pthsize;
    (void)dummy_val;
    (void)targetconc_ptr;
    g_threadstart = threadstart;
    g_wqthread = wqthread;
    g_dispatchqueue_offset = dispatchqueue_offset;
    g_tsd_offset = tsd_offset;

    /* Return feature bits required by libpthread:
     *   w8 = 0x4000001e (checked in ___pthread_init)
     *   bit 21 = 0x00200000 (checked in _pthread_start)
     *   bit 6 = 0x00000040, bit 7 = 0x00000080 (checked in libdispatch)
     */
    return 0x402000de;
}

int lsk_bsdthread_create(void *func, void *arg, void *stack, void *pthread, unsigned int flags) __asm__("___bsdthread_create");
int lsk_bsdthread_create(void *func, void *arg, void *stack, void *pthread, unsigned int flags) {
    if (!g_threadstart || !stack || !pthread) {
        set_errno(22); /* EINVAL */
        return -1;
    }

    extern uint32_t ipc_port_allocate_thread(void);
    uint32_t kport = ipc_port_allocate_thread();
    uint64_t tsd_offset = g_tsd_offset ? g_tsd_offset : 224;

    /* Darwin TSD layout:
     * Slot 0: __TSD_THREAD_SELF = pthread
     * Slot 1: __TSD_ERRNO = &pthread->err_no (already set by _pthread_create)
     * Slot 2: __TSD_MIG_REPLY = 0
     * Slot 3: __TSD_MACH_THREAD_SELF = kport (at pthread + 248)
     */
    uint64_t tsd_base = (uint64_t)pthread + tsd_offset;
    *(uint64_t *)tsd_base = (uint64_t)pthread;
    *(uint32_t *)(tsd_base + 2 * sizeof(void *)) = 0;
    *(uint32_t *)(tsd_base + 3 * sizeof(void *)) = kport;

    uintptr_t sp = (uintptr_t)stack & ~15ULL;
    sp -= 64; /* reserve space for payload */

    uint64_t stacktop = *(uint64_t *)((uintptr_t)pthread + 176);
    uint64_t stackbottom = *(uint64_t *)((uintptr_t)pthread + 184);
    uint64_t stacksize = (stacktop > stackbottom) ? (stacktop - stackbottom) : 0x80000;

    /* Flags for _pthread_start:
     * bit 28 (0x10000000): PTHREAD_START_TSD_BASE_SET (required by _pthread_start)
     * bit 29 (0x20000000): PTHREAD_START_SUSPENDED (must NOT be set)
     * bit 21 (0x00200000): PTHREAD_FEATURE_REQCOUNT
     */
    uint32_t start_flags = (flags & ~0x20000000) | 0x10000000 | 0x00200000;

    uint64_t *payload = (uint64_t *)sp;
    payload[0] = (uint64_t)g_threadstart;
    payload[1] = (uint64_t)pthread;
    payload[2] = (uint64_t)kport;
    payload[3] = (uint64_t)func;
    payload[4] = (uint64_t)arg;
    payload[5] = stacksize;
    payload[6] = (uint64_t)start_flags;
    payload[7] = 0;

    /* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID */
    unsigned long clone_flags = 0x3d0f00;
    int *tid_loc = (int *)((uintptr_t)pthread + 216);

    long ret = __iosw_raw_clone(clone_flags, (void *)sp, tid_loc, tsd_base, tid_loc);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    /* Diagnostic: record the thread (ret is the child tid in the
     * parent) and log its start routine. A QuartzCore server thread would
     * show func in the 0x183f-0x1842 range; if libpthread passes
     * _pthread_start for every thread instead, func is uniform and the
     * creation order + pthread pointer still identify the thread, with the
     * real start routine to be read out of the pthread struct. */
    thread_rec_add(ret, func, arg, pthread, kport);
    if (g_ipc_trace) {
        struct thread_buf b;
        tb_init(&b);
        tb_str(&b, "[thread] create func=0x");
        tb_hex(&b, (uint64_t)func);
        tb_str(&b, " arg=0x");
        tb_hex(&b, (uint64_t)arg);
        tb_str(&b, " pthread=0x");
        tb_hex(&b, (uint64_t)pthread);
        tb_str(&b, " kport=0x");
        tb_hex(&b, kport);
        tb_str(&b, " tid=");
        tb_hex(&b, (uint64_t)ret);
        tb_str(&b, "\n");
        tb_flush(&b);
    }
    return 0;
}

void lsk_bsdthread_terminate(void *stackaddr, size_t size, uint32_t kport, uint32_t sem) __asm__("___bsdthread_terminate");
void lsk_bsdthread_terminate(void *stackaddr, size_t size, uint32_t kport, uint32_t sem) {
    (void)stackaddr;
    (void)size;
    (void)sem;
    thread_rec_remove(kport);
    if (g_ipc_trace) {
        struct thread_buf b;
        tb_init(&b);
        tb_str(&b, "[thread] exit kport=0x");
        tb_hex(&b, kport);
        tb_str(&b, "\n");
        tb_flush(&b);
    }
#ifdef THREAD_HOST_TEST
    return;
#else
    sys1(SYS_exit, 0);
    while (1) {}
#endif
}

uint64_t lsk_thread_selfid(void) __asm__("___thread_selfid");
uint64_t lsk_thread_selfid(void) {
    return (uint64_t)thread_gettid();
}

int lsk_thread_set_tsd_base(uint64_t base) __asm__("__thread_set_tsd_base");
int lsk_thread_set_tsd_base(uint64_t base) {
#if defined(__aarch64__) && !defined(THREAD_HOST_TEST)
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(base));
#else
    (void)base;
#endif
    return 0;
}

int lsk_disable_threadsignal(int val) __asm__("___disable_threadsignal");
int lsk_disable_threadsignal(int val) {
    (void)val;
    return 0;
}


/* Darwin signal number -> Linux (arm64). 0 = no Linux equivalent. */
static const uint8_t k_d2l_sig[32] = {
    0, 1, 2, 3, 4, 5, 6, 0 /*EMT*/, 8, 9, 7 /*BUS*/, 11, 31 /*SYS*/, 13, 14, 15,
    23 /*URG*/, 19 /*STOP*/, 20 /*TSTP*/, 18 /*CONT*/, 17 /*CHLD*/, 21, 22, 29 /*IO*/,
    24, 25, 26, 27, 28 /*WINCH*/, 0 /*INFO*/, 10 /*USR1*/, 12 /*USR2*/ };
static inline uint64_t lsk_darwin_sigset_to_linux(uint32_t d) {
    uint64_t l = 0;
    for (int i = 1; i < 32; i++)
        if ((d & (1u << (i - 1))) && k_d2l_sig[i]) l |= 1ULL << (k_d2l_sig[i] - 1);
    return l;
}
static inline uint32_t lsk_linux_sigset_to_darwin(uint64_t l) {
    uint32_t d = 0;
    for (int i = 1; i < 32; i++)
        if (k_d2l_sig[i] && (l & (1ULL << (k_d2l_sig[i] - 1)))) d |= 1u << (i - 1);
    return d;
}
/* Linux signals never blocked: synchronous faults (else the kernel force-kills
 * silently), SIGQUIT/SIGUSR2 dump diagnostics, SIGSYS seccomp traps. */
#define LSK_SIG_NEVER_BLOCK ((1ULL<<(3-1))|(1ULL<<(4-1))|(1ULL<<(5-1))|(1ULL<<(7-1))|(1ULL<<(8-1))|(1ULL<<(11-1))|(1ULL<<(12-1))|(1ULL<<(31-1)))
int lsk_pthread_sigmask(int how, const void *set, void *oset) __asm__("___pthread_sigmask");
int lsk_pthread_sigmask(int how, const void *set, void *oset) {
    /* Translate Darwin how (SIG_BLOCK=1, SIG_UNBLOCK=2, SIG_SETMASK=3) to Linux
     * (0, 1, 2) and widen 4-byte Darwin sigset_t to 8-byte Linux sigset_t.
     * Keep SIGQUIT (3), SIGUSR2 (12), and SIGSYS (31) unblocked so crash/dump
     * diagnostics and seccomp traps always fire. */
    int lhow = 0;
    if (set) {
        if (how == 1) lhow = 0;       /* SIG_BLOCK */
        else if (how == 2) lhow = 1;  /* SIG_UNBLOCK */
        else if (how == 3) lhow = 2;  /* SIG_SETMASK */
        else return 22;               /* EINVAL */
    }
    uint64_t lset = 0;
    uint64_t loset = 0;
    if (set) {
        lset = lsk_darwin_sigset_to_linux(*(const uint32_t *)set);
        if (lhow == 0 || lhow == 2) lset &= ~LSK_SIG_NEVER_BLOCK;
    }
#ifdef THREAD_HOST_TEST
    int rc = thread_test_rt_sigprocmask(lhow, set ? &lset : 0, oset ? &loset : 0);
    if (rc == 0 && oset) *(uint32_t *)oset = lsk_linux_sigset_to_darwin(loset);
    return rc;
#else
    long ret = sys4(SYS_rt_sigprocmask, lhow, set ? (long)&lset : 0, oset ? (long)&loset : 0, 8);
    if (ret == 0 && oset) *(uint32_t *)oset = lsk_linux_sigset_to_darwin(loset);
    return ret == 0 ? 0 : linux_to_darwin_errno(ret);
#endif
}

int lsk_pthread_kill(uint32_t tid, int sig) __asm__("___pthread_kill");
int lsk_pthread_kill(uint32_t tid, int sig) {
    (void)tid;
    (void)sig;
    return 0;
}

int lsk_pthread_markcancel(uint32_t tid) __asm__("___pthread_markcancel");
int lsk_pthread_markcancel(uint32_t tid) {
    (void)tid;
    return 0;
}

int lsk_pthread_canceled(int action) __asm__("___pthread_canceled");
int lsk_pthread_canceled(int action) {
    (void)action;
    return 0;
}

/* Futex-based ulock */
/* Debug: last ulock wait per thread (tid & 63), dumped by the SIGQUIT handler in init.c. */
struct ulock_dbg { long tid; uint64_t addr; uint64_t val; uint32_t op; uint32_t sleeping; };
struct ulock_dbg g_ulock_dbg[64];

/* Dump the live-thread registry to fd 2 (updated 048). Called from
 * the SIGQUIT handler in init.c. Lock-free, no allocation, async-signal-safe:
 * each dumped line is formatted into a stack buffer and emitted in one write().
 * Appends the ulock wait state when the thread is asleep in __ulock_wait. */
void lsk_thread_dump(void) {
    struct thread_buf b;
    tb_init(&b);
    tb_str(&b, "[lsk THREADS] live threads:\n");
    tb_flush(&b);
    for (int i = 0; i < MAX_THREAD_RECS; i++) {
        if (__atomic_load_n(&g_thread_recs[i].alive, __ATOMIC_ACQUIRE) != 1) continue;
        long tid = g_thread_recs[i].tid;
        uint64_t func = g_thread_recs[i].func;
        uint64_t arg = g_thread_recs[i].arg;
        uint64_t pthread = g_thread_recs[i].pthread;
        uint32_t kport = g_thread_recs[i].kport;
        uint32_t wq_flags = g_thread_recs[i].wq_flags;
        if (__atomic_load_n(&g_thread_recs[i].alive, __ATOMIC_ACQUIRE) != 1) continue;

        tb_init(&b);
        tb_str(&b, "[lsk THREAD] tid=");
        tb_hex(&b, (uint64_t)tid);
        tb_str(&b, " func=0x");
        tb_hex(&b, func);
        tb_str(&b, " arg=0x");
        tb_hex(&b, arg);
        tb_str(&b, " pthread=0x");
        tb_hex(&b, pthread);
        tb_str(&b, " kport=0x");
        tb_hex(&b, kport);
        if (wq_flags != 0) {
            tb_str(&b, " wq=0x");
            tb_hex(&b, wq_flags);
        }
        for (int j = 0; j < 64; j++) {
            if (g_ulock_dbg[j].sleeping && g_ulock_dbg[j].tid == tid) {
                tb_str(&b, " ulock op=0x");
                tb_hex(&b, g_ulock_dbg[j].op);
                tb_str(&b, " addr=0x");
                tb_hex(&b, g_ulock_dbg[j].addr);
                break;
            }
        }
        tb_str(&b, "\n");
        tb_flush(&b);
    }
}
static inline void ulock_dbg_set(uint32_t op, void *addr, uint64_t val, uint32_t sleeping) {
    long tid = thread_gettid();
    struct ulock_dbg *d = &g_ulock_dbg[tid & 63];
    d->tid = tid; d->addr = (uint64_t)addr; d->val = val; d->op = op; d->sleeping = sleeping;
}
/* 64-bit ulock wait support. Linux futexes compare
 * only 32 bits, so a UL_COMPARE_AND_WAIT64 wait on a 64-bit value cannot
 * be done with a plain FUTEX_WAIT on the address: if the waker stores a
 * new 64-bit value whose LOW 32 bits equal the waited value's low 32
 * bits, the futex entry check still passes and the waiter sleeps after
 * the wake already happened (lost wakeup). libdispatch's dispatch_once
 * waits exactly this way (UL_COMPARE_AND_WAIT64 on a 64-bit
 * thread-pointer predicate), so a lost wakeup hangs the waiter inside
 * dispatch_once forever. 64-bit waits therefore sleep on a
 * per-address generation word instead: the waiter reads the generation
 * first, then the value, then FUTEX_WAITs on the generation; the waker
 * bumps the generation for that address and wakes it. The waiter
 * re-checks the full 64-bit value after every wake, so hash collisions
 * between unrelated addresses only cause spurious wakeups, never a
 * missed one. The table is fixed-size and freestanding. */
#define LSK_ULOCK64_SLOTS 64
static volatile uint32_t lsk_ulock64_gen[LSK_ULOCK64_SLOTS];
static inline uint32_t lsk_ulock64_slot(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    uint32_t h = (uint32_t)(a ^ (a >> 16) ^ (a >> 32));
    return h & (LSK_ULOCK64_SLOTS - 1);
}
/* True when op is a 64-bit compare-and-wait (the low byte is the
 * operation; the high bits are ULF flags). 1 = 32-bit, 2 = 64-bit,
 * 3 = 32-bit shared, 4 = 64-bit shared. Shared (cross-process) ulocks
 * do not occur in-process; the generation table is in-process, so the
 * private futex is correct for it either way. */
static inline int lsk_ulock_is64(uint32_t op) {
    uint32_t o = op & 0xffu;
    return (o == 4u || o == 5u || o == 6u);  /* UNFAIR_LOCK64_SHARED, COMPARE_AND_WAIT64(_SHARED) */
}
/* 64-bit wait: sleep on the generation word for this address.
 * Returns 1 when the caller should re-check the value (woken, value
 * already changed, or spurious wakeup), or -60 on timeout, or a
 * negative Darwin errno on error. The caller loops and re-checks the
 * predicate, which is the standard dispatch_once / os_unfair_lock
 * pattern, so one wait per call is enough. */
static int lsk_ulock_wait64(void *addr, uint64_t val, void *timeout_ptr) {
    volatile uint32_t *gen = &lsk_ulock64_gen[lsk_ulock64_slot(addr)];
    /* Read the generation FIRST, then the value. A waker bumps the
     * generation after changing the value, so: if the generation we
     * read is still current, the value we read next is the value as
     * of that generation; if the value already changed, we return at
     * once; if it changes after we read it, the generation bump makes
     * the futex entry check fail (EAGAIN) instead of sleeping. */
    uint32_t g = __atomic_load_n(gen, __ATOMIC_ACQUIRE);
    uint64_t cur = __atomic_load_n((volatile uint64_t *)addr, __ATOMIC_ACQUIRE);
    if (cur != val) return 1;
    long r = sys4(SYS_futex, (long)gen, FUTEX_WAIT_PRIVATE, (long)g, (long)timeout_ptr);
#ifdef THREAD_HOST_TEST
    /* Host shim only: the futex call goes through libc syscall(),
     * which returns -1 and sets errno instead of the raw -errno the
     * target svc returns. Normalize so the checks below see the raw
     * values (same pattern as the rwlock slow path). */
    if (r == -1) {
        if (errno == 4) r = -4L;
        else if (errno == 11) r = -11L;
        else if (errno == 110) r = -110L;
    }
#endif
    if (r == -110) return -60; /* -ETIMEDOUT */
    if (r < 0 && r != -11 /* -EAGAIN */) {
        return -linux_to_darwin_errno(r);
    }
    return 1;
}
/* 64-bit wake: bump the generation for this address and wake its
 * sleepers. The waiter re-checks the full 64-bit value, so waking a
 * slot shared by another address is only a spurious wakeup. */
static void lsk_ulock_wake64(void *addr, int count) {
    volatile uint32_t *gen = &lsk_ulock64_gen[lsk_ulock64_slot(addr)];
    __atomic_add_fetch(gen, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)gen, FUTEX_WAKE_PRIVATE, count);
}
int lsk_ulock_wait(uint32_t op, void *addr, uint64_t val, uint32_t timeout_us) __asm__("___ulock_wait");
int lsk_ulock_wait(uint32_t op, void *addr, uint64_t val, uint32_t timeout_us) {
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    void *timeout_ptr = 0;
    if (timeout_us > 0) {
        ts.tv_sec = timeout_us / 1000000;
        ts.tv_nsec = (timeout_us % 1000000) * 1000;
        timeout_ptr = &ts;
    }
    ulock_dbg_set(op, addr, val, 1);
    long ret;
    if (lsk_ulock_is64(op)) {
        ret = lsk_ulock_wait64(addr, val, timeout_ptr);
    } else {
        ret = sys4(SYS_futex, (long)addr, FUTEX_WAIT_PRIVATE, (long)val, (long)timeout_ptr);
#ifdef THREAD_HOST_TEST
        /* Host shim only: libc syscall() returns -1 and sets errno
         * instead of the raw -errno (same pattern as the rwlock slow
         * path). Normalize so the checks below see the raw values. */
        if (ret == -1) {
            if (errno == 4) ret = -4L;
            else if (errno == 11) ret = -11L;
            else if (errno == 110) ret = -110L;
        }
#endif
        if (ret == -110) ret = -60; /* -ETIMEDOUT */
        else if (ret < 0 && ret != -11 /* -EAGAIN */) {
            ret = -linux_to_darwin_errno(ret);
        } else {
            ret = 1;
        }
    }
    ulock_dbg_set(op, addr, val, 0);
    if (ret == -60) return -60;
    if (ret < 0) return (int)ret;
    /* XNU returns the number of threads still waiting. libplatform's os_unfair_lock (and pthread mutex)
     * use ret > 0 to keep the "waiters" bit when acquiring, so the next unlock wakes the remaining
     * sleepers. We do not track waiters, so report "maybe more" (1): returning 0 lost wakeups (two
     * threads asleep on a released lock, Aidoku stall 2026-11-12). */
    return 1;
}

int lsk_ulock_wait2(uint32_t op, void *addr, uint64_t val, uint64_t timeout_ns, uint64_t val2) __asm__("___ulock_wait2");
int lsk_ulock_wait2(uint32_t op, void *addr, uint64_t val, uint64_t timeout_ns, uint64_t val2) {
    (void)val2;
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    void *timeout_ptr = 0;
    if (timeout_ns > 0) {
        ts.tv_sec = timeout_ns / 1000000000ULL;
        ts.tv_nsec = timeout_ns % 1000000000ULL;
        timeout_ptr = &ts;
    }
    ulock_dbg_set(op, addr, val, 1);
    long ret;
    if (lsk_ulock_is64(op)) {
        ret = lsk_ulock_wait64(addr, val, timeout_ptr);
    } else {
        ret = sys4(SYS_futex, (long)addr, FUTEX_WAIT_PRIVATE, (long)val, (long)timeout_ptr);
#ifdef THREAD_HOST_TEST
        /* Host shim only: libc syscall() returns -1 and sets errno
         * instead of the raw -errno (same pattern as the rwlock slow
         * path). Normalize so the checks below see the raw values. */
        if (ret == -1) {
            if (errno == 4) ret = -4L;
            else if (errno == 11) ret = -11L;
            else if (errno == 110) ret = -110L;
        }
#endif
        if (ret == -110) ret = -60; /* -ETIMEDOUT */
        else if (ret < 0 && ret != -11 /* -EAGAIN */) {
            ret = -linux_to_darwin_errno(ret);
        } else {
            ret = 1;
        }
    }
    ulock_dbg_set(op, addr, val, 0);
    if (ret == -60) return -60;
    if (ret < 0) return (int)ret;
    /* XNU returns the number of threads still waiting. libplatform's os_unfair_lock (and pthread mutex)
     * use ret > 0 to keep the "waiters" bit when acquiring, so the next unlock wakes the remaining
     * sleepers. We do not track waiters, so report "maybe more" (1): returning 0 lost wakeups (two
     * threads asleep on a released lock, Aidoku stall 2026-11-12). */
    return 1;
}

int lsk_ulock_wake(uint32_t op, void *addr, uint64_t wake_val) __asm__("___ulock_wake");
int lsk_ulock_wake(uint32_t op, void *addr, uint64_t wake_val) {
    (void)wake_val;
    int count = (op & 0x100) ? 0x7fffffff : 1; /* ULOCK_WAKE_ALL */
    sys3(SYS_futex, (long)addr, FUTEX_WAKE_PRIVATE, count);
    /* Also wake 64-bit waiters on this address. The waker does not
     * know whether the sleepers are 32-bit or 64-bit, so bump the
     * generation for this address as well; a 64-bit waiter on an
     * unrelated address that shares the slot only wakes spuriously
     * and re-checks its value. */
    lsk_ulock_wake64(addr, count);
    return 0;
}

/* Psynch mutex protocol. In-process implementation
 * of the XNU psynch mutexwait/mutexdrop protocol from
 * apple-oss-distributions/libpthread kern/kern_synch.c. The user mutex
 * struct has no room for kernel state (prepost, wait queue), so the
 * state lives here in a side table keyed by mutex address. The table
 * is fixed-size and freestanding (no malloc, no __thread); entries are
 * freed when they hold no waiters and no prepost, so the table
 * recycles. The tid argument is stored on the waiter record but no
 * turnstile priority inheritance is implemented (out of scope). */
#define LSK_MTX_COUNT_MASK 0xffffff00u
#define LSK_MTX_INC 0x100u
#define LSK_MTX_KBIT 0x1u
#define LSK_MTX_EBIT 0x2u
#define LSK_MTX_POLICY_MASK 0x1c0u
#define LSK_MTX_POLICY_FIRSTFIT 0x080u
#define LSK_MTX_TABLE_SIZE 64
#define LSK_MTX_MAX_WAITERS 32

struct lsk_mtx_waiter {
    uint32_t mgen;  /* waiter's lgen seq, count-masked */
    uint64_t tid;   /* waiter tid (owner hint) */
    uint64_t order; /* arrival order (FIFO for firstfit) */
    int futex;      /* 0 = asleep, 1 = woken */
    int woken;      /* 1 = this waiter was granted by a drop */
    int in_use;
};
struct lsk_mtx_state {
    void *mutex;
    uint32_t prepost_count; /* unconsumed preposted wakeups */
    uint32_t prepost_lseq;  /* seq the prepost was recorded for */
    uint32_t lastunlockseq; /* last ugen dropped, count-masked */
    int in_use;
    struct lsk_mtx_waiter waiters[LSK_MTX_MAX_WAITERS];
};
static struct lsk_mtx_state lsk_mtx_table[LSK_MTX_TABLE_SIZE];
static int lsk_mtx_table_lock;
static uint64_t lsk_mtx_order_ctr;

static void lsk_mtx_lock(void) {
    while (__atomic_exchange_n(&lsk_mtx_table_lock, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&lsk_mtx_table_lock, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

static void lsk_mtx_unlock(void) {
    __atomic_store_n(&lsk_mtx_table_lock, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&lsk_mtx_table_lock, FUTEX_WAKE_PRIVATE, 1);
}

/* Back off 1 ms when the state table is full. Never fabricate a grant;
 * the caller retries the lookup. Uses a futex wait with a timeout on
 * the table lock word, which works on host and device. */
static void lsk_mtx_backoff_1ms(void) {
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    int cur = __atomic_load_n(&lsk_mtx_table_lock, __ATOMIC_RELAXED);
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    sys4(SYS_futex, (long)&lsk_mtx_table_lock, FUTEX_WAIT_PRIVATE, cur, (long)&ts);
}

static struct lsk_mtx_state *lsk_mtx_find_locked(void *mutex) {
    for (int i = 0; i < LSK_MTX_TABLE_SIZE; i++) {
        if (lsk_mtx_table[i].in_use && lsk_mtx_table[i].mutex == mutex) {
            return &lsk_mtx_table[i];
        }
    }
    return 0;
}

static struct lsk_mtx_state *lsk_mtx_find_or_create_locked(void *mutex) {
    struct lsk_mtx_state *st = lsk_mtx_find_locked(mutex);
    if (st) return st;
    for (int i = 0; i < LSK_MTX_TABLE_SIZE; i++) {
        if (!lsk_mtx_table[i].in_use) {
            st = &lsk_mtx_table[i];
            st->mutex = mutex;
            st->prepost_count = 0;
            st->prepost_lseq = 0;
            st->lastunlockseq = 0;
            st->in_use = 1;
            for (int j = 0; j < LSK_MTX_MAX_WAITERS; j++) {
                st->waiters[j].in_use = 0;
            }
            return st;
        }
    }
    return 0;
}

static int lsk_mtx_nwaiters_locked(struct lsk_mtx_state *st) {
    int n = 0;
    for (int i = 0; i < LSK_MTX_MAX_WAITERS; i++) {
        if (st->waiters[i].in_use) n++;
    }
    return n;
}

/* Free the entry when it holds no waiters and no prepost, so the table
 * recycles. Only called with the table lock held, and only from the
 * waiter paths (a drop either wakes a waiter or records a prepost,
 * so the entry is never empty after a drop). */
static void lsk_mtx_maybe_free_locked(struct lsk_mtx_state *st) {
    if (st->prepost_count == 0 && lsk_mtx_nwaiters_locked(st) == 0) {
        st->in_use = 0;
    }
}

/* Psynch primitives */
int lsk_psynch_mutexwait(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags) __asm__("___psynch_mutexwait");
int lsk_psynch_mutexwait(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags) {
    int firstfit = (flags & LSK_MTX_POLICY_MASK) == LSK_MTX_POLICY_FIRSTFIT;
    uint32_t lseq = mgen & LSK_MTX_COUNT_MASK;
    (void)ugen;
    for (;;) {
        struct lsk_mtx_waiter *w = 0;
        lsk_mtx_lock();
        struct lsk_mtx_state *st = lsk_mtx_find_or_create_locked(mutex);
        if (st && st->prepost_count > 0 && (firstfit || lseq == st->prepost_lseq)) {
            /* Consume a preposted wakeup (kern_synch.c _psynch_mutexwait). */
            st->prepost_count--;
            if (!firstfit) {
                st->prepost_count = 0;
                st->prepost_lseq = 0;
            }
            lsk_mtx_maybe_free_locked(st);
            lsk_mtx_unlock();
            return (int)(lseq | LSK_MTX_KBIT | LSK_MTX_EBIT);
        }
        if (st) {
            for (int i = 0; i < LSK_MTX_MAX_WAITERS; i++) {
                if (!st->waiters[i].in_use) {
                    w = &st->waiters[i];
                    w->mgen = lseq;
                    w->tid = tid;
                    w->order = __atomic_fetch_add(&lsk_mtx_order_ctr, 1, __ATOMIC_RELAXED);
                    w->futex = 0;
                    w->woken = 0;
                    w->in_use = 1;
                    break;
                }
            }
        }
        lsk_mtx_unlock();
        if (!st || !w) {
            /* No state: table full. Never fabricate a grant; back off and retry. */
            lsk_mtx_backoff_1ms();
            continue;
        }
        /* Sleep until the drop grants us. The futex wait re-checks the
         * woken flag, so a wake that lands before we enter the syscall
         * is not lost (the syscall returns at once on value mismatch). */
        for (;;) {
            if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE) != 0) break;
            long r = sys4(SYS_futex, (long)&w->futex, FUTEX_WAIT_PRIVATE, 0, 0);
            if (r == -4) { /* -EINTR */
                /* Re-check woken UNDER the table lock. The drop grants
                 * under the same lock, so a grant that lands between
                 * the unlocked check above and our removal here is
                 * seen here, not lost. (the old code
                 * checked woken outside the lock, so a grant landing
                 * in that window was lost and a fairshare mutex was
                 * left in SWITCHING with no owner, hanging the next
                 * locker.) */
                lsk_mtx_lock();
                if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE) != 0) {
                    lsk_mtx_unlock();
                    break; /* granted: take it via the common path below */
                }
                /* Not woken: leave the queue so the caller retries (it
                 * loops while the return is -1). The grant is not lost
                 * because none was issued to us, and a later drop will
                 * not find us (in_use is 0) so it preposts instead of
                 * granting to a departed waiter. */
                w->in_use = 0;
                st = lsk_mtx_find_locked(mutex);
                if (st) lsk_mtx_maybe_free_locked(st);
                lsk_mtx_unlock();
                return -1;
            }
            /* r == 0 (woken) or r == -11 (EAGAIN, already 1): re-check woken. */
        }
        lsk_mtx_lock();
        w->in_use = 0;
        st = lsk_mtx_find_locked(mutex);
        if (st) lsk_mtx_maybe_free_locked(st);
        lsk_mtx_unlock();
        return (int)(lseq | LSK_MTX_KBIT | LSK_MTX_EBIT);
    }
}

int lsk_psynch_mutexdrop(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags) __asm__("___psynch_mutexdrop");
int lsk_psynch_mutexdrop(void *mutex, uint32_t mgen, uint32_t ugen, uint64_t tid, uint32_t flags) {
    (void)tid;
    int firstfit = (flags & LSK_MTX_POLICY_MASK) == LSK_MTX_POLICY_FIRSTFIT;
    uint32_t nextgen = (ugen + LSK_MTX_INC) & LSK_MTX_COUNT_MASK;
    for (;;) {
        lsk_mtx_lock();
        struct lsk_mtx_state *st = lsk_mtx_find_or_create_locked(mutex);
        if (!st) {
            /* Table full: back off and retry; the prepost must be recorded. */
            lsk_mtx_unlock();
            lsk_mtx_backoff_1ms();
            continue;
        }
        st->lastunlockseq = ugen & LSK_MTX_COUNT_MASK;
        struct lsk_mtx_waiter *target = 0;
        if (firstfit) {
            /* Signal the first waiter (FIFO by arrival order). */
            uint64_t low = (uint64_t)-1;
            for (int i = 0; i < LSK_MTX_MAX_WAITERS; i++) {
                if (st->waiters[i].in_use && !st->waiters[i].woken &&
                    st->waiters[i].order < low) {
                    low = st->waiters[i].order;
                    target = &st->waiters[i];
                }
            }
        } else {
            /* Fairshare: signal the waiter whose seq is nextgen. If it
             * has not arrived yet, prepost below so it returns at once
             * when it does (kern_synch.c _psynch_mutexdrop_internal). */
            for (int i = 0; i < LSK_MTX_MAX_WAITERS; i++) {
                if (st->waiters[i].in_use && !st->waiters[i].woken &&
                    st->waiters[i].mgen == nextgen) {
                    target = &st->waiters[i];
                    break;
                }
            }
        }
        if (target) {
            __atomic_store_n(&target->woken, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&target->futex, 1, __ATOMIC_RELEASE);
            sys3(SYS_futex, (long)&target->futex, FUTEX_WAKE_PRIVATE, 1);
            lsk_mtx_unlock();
            return 0;
        }
        /* No waiter to wake: prepost for a later arrival. */
        if (firstfit) {
            st->prepost_count++;
            st->prepost_lseq = mgen & LSK_MTX_COUNT_MASK;
        } else {
            st->prepost_count = 1;
            st->prepost_lseq = nextgen;
        }
        lsk_mtx_unlock();
        return 0;
    }
}

int lsk_psynch_cvwait(void *cv, uint64_t cvgen, uint32_t cvugen, void *mutex, uint64_t mugen, uint32_t flags, int64_t sec, uint32_t nsec) __asm__("___psynch_cvwait");
/* Condition variables: the kernel side is emulated with a per-cv wake counter (hashed by address)
 * and a bounded futex sleep. Returning 0 (spurious wakeup) is always legal for pthread_cond_wait;
 * the old no-op made every waiter spin at 100% CPU (AsyncDisplayKit workers, NSCondition users).
 * A signal that lands between reading the counter and sleeping costs at most CV_MAX_NS. */
#define CV_SLOTS 256
#define CV_MAX_NS 20000000LL
static volatile uint32_t g_cv_seq[CV_SLOTS];
static inline volatile uint32_t *cv_slot(const void *cv) {
    uint64_t a = (uint64_t)(uintptr_t)cv;
    return &g_cv_seq[((a >> 3) ^ (a >> 11)) & (CV_SLOTS - 1)];
}
int lsk_psynch_cvwait(void *cv, uint64_t cvgen, uint32_t cvugen, void *mutex, uint64_t mugen, uint32_t flags, int64_t sec, uint32_t nsec) {
    (void)cvgen; (void)cvugen; (void)mutex; (void)mugen; (void)flags;
    volatile uint32_t *slot = cv_slot(cv);
    uint32_t seen = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    int64_t ns = CV_MAX_NS;
    if (sec > 0 || nsec > 0) {
        int64_t want = sec * 1000000000LL + (int64_t)nsec;
        if (want < ns) ns = want;
    }
    struct { long tv_sec; long tv_nsec; } ts = { (long)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
    sys4(SYS_futex, (long)slot, FUTEX_WAIT_PRIVATE, (long)seen, (long)&ts);
    return 0; /* same contract as before: callers re-check their predicate */
}

int lsk_psynch_cvsignal(void *cv, uint64_t cvgen, uint32_t cvugen, int thread_port, void *mutex, uint64_t mugen, uint64_t tid, uint32_t flags) __asm__("___psynch_cvsignal");
int lsk_psynch_cvsignal(void *cv, uint64_t cvgen, uint32_t cvugen, int thread_port, void *mutex, uint64_t mugen, uint64_t tid, uint32_t flags) {
    (void)cvgen; (void)cvugen; (void)thread_port; (void)mutex; (void)mugen; (void)tid; (void)flags;
    volatile uint32_t *slot = cv_slot(cv);
    __atomic_add_fetch(slot, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)slot, FUTEX_WAKE_PRIVATE, 0x7fffffff); /* shared slots: wake all */
    return 0;
}

int lsk_psynch_cvbroad(void *cv, uint64_t cvgen, uint32_t cvugen, void *mutex, uint64_t mugen, uint64_t tid, uint32_t flags) __asm__("___psynch_cvbroad");
int lsk_psynch_cvbroad(void *cv, uint64_t cvgen, uint32_t cvugen, void *mutex, uint64_t mugen, uint64_t tid, uint32_t flags) {
    (void)cvgen; (void)cvugen; (void)mutex; (void)mugen; (void)tid; (void)flags;
    volatile uint32_t *slot = cv_slot(cv);
    __atomic_add_fetch(slot, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)slot, FUTEX_WAKE_PRIVATE, 0x7fffffff); /* shared slots: wake all */
    return 0;
}

int lsk_psynch_cvclrprepost(void *cv, uint32_t cvgen, uint32_t cvugen, uint32_t cvsgen, uint32_t prepocnt, uint32_t preposeq, uint32_t flags) __asm__("___psynch_cvclrprepost");
int lsk_psynch_cvclrprepost(void *cv, uint32_t cvgen, uint32_t cvugen, uint32_t cvsgen, uint32_t prepocnt, uint32_t preposeq, uint32_t flags) {
    (void)cv; (void)cvgen; (void)cvugen; (void)cvsgen; (void)prepocnt; (void)preposeq; (void)flags;
    return 0;
}

/* ------------------------------------------------------------------ */
/* psynch rwlock                                   */
/*                                                                     */
/* In-process wait queues with Linux futex sleeps, replacing the       */
/* kernel for the eight psynch_rw_* calls. The protocol follows        */
/* apple-oss-distributions/libpthread kern/kern_synch.c                */
/* (__psynch_rw_lock, _psynch_rw_unlock, kwq_handle_unlock) and the    */
/* 23G90 libpthread consumer in src/pthread_rwlock.c. Functions        */
/* implemented here include rdlock, wrlock, unlock, unlock2, upgrade,  */
/* downgrade, longrdlock, and yieldwrlock, sharing the same queue      */
/* and helpers.                                                        */
/*                                                                     */
/* Rules this code lives by:              */
/*  - Never write the user rwlock words (L/S/U). Only libpthread      */
/*    writes them, via CAS, from the updatebits returned here.         */
/*  - Never return 0 from rdlock/wrlock when the caller is not        */
/*    granted.                                                         */
/*  - Never grant readers past the lowest pending writer, and never    */
/*    grant more than one writer. Lowest seq wins.                     */
/*  - Never sleep holding kwq_lock. Never call a libpthread function  */
/*    while holding kwq_lock.                                         */
/*  - The first argument is the full 64-bit rwlock address. The old   */
/*    uint32_t rwl declaration truncated it.                          */
/*                                                                     */
/* Futex note: every waiter on one queue sleeps on the same            */
/* per-queue futex word, so a grant wakes ALL waiters on that word    */
/* (FUTEX_WAKE_PRIVATE, 0x7fffffff). The wake is only a               */
/* notification: each waiter re-checks its own grant flag under       */
/* kwq_lock, and only the granted waiter proceeds. Waking exactly    */
/* one waiter could wake the wrong one and strand the granted one.    */
/* Deviations from the public kernel snapshot are documented inline.  */
/* ------------------------------------------------------------------ */

/* Constants from synch_internal.h. Do not renumber. */
#ifndef PTHRW_COUNT_SHIFT
#define PTHRW_COUNT_SHIFT 8
#endif
#ifndef PTHRW_INC
#define PTHRW_INC (1 << PTHRW_COUNT_SHIFT)
#endif
#ifndef PTHRW_BIT_MASK
#define PTHRW_BIT_MASK 0xFF
#endif
#ifndef PTHRW_COUNT_MASK
#define PTHRW_COUNT_MASK ((uint32_t)~0xFFu)
#endif
#ifndef PTHRW_MAX_READERS
#define PTHRW_MAX_READERS PTHRW_COUNT_MASK
#endif
#ifndef PTH_RWL_KBIT
#define PTH_RWL_KBIT 0x01
#endif
#ifndef PTH_RWL_EBIT
#define PTH_RWL_EBIT 0x02
#endif
#ifndef PTH_RWL_WBIT
#define PTH_RWL_WBIT 0x04
#endif
#ifndef PTH_RWL_PBIT
#define PTH_RWL_PBIT 0x04
#endif
#ifndef PTH_RWL_MTX_WAIT
#define PTH_RWL_MTX_WAIT 0x20
#endif
#ifndef PTH_RWL_UBIT
#define PTH_RWL_UBIT 0x40
#endif
#ifndef PTH_RWL_MBIT
#define PTH_RWL_MBIT 0x40
#endif
#ifndef PTH_RWL_IBIT
#define PTH_RWL_IBIT 0x80
#endif
#ifndef PTHRW_RWL_INIT
#define PTHRW_RWL_INIT PTH_RWL_IBIT
#endif
#ifndef PTHRW_RWLOCK_INIT
#define PTHRW_RWLOCK_INIT (PTH_RWL_IBIT | PTH_RWL_UBIT)
#endif
#ifndef PTH_RWS_SBIT
#define PTH_RWS_SBIT 0x01
#endif
#ifndef PTH_RWS_IBIT
#define PTH_RWS_IBIT 0x02
#endif
#ifndef PTH_RWS_CV_CBIT
#define PTH_RWS_CV_CBIT 0x01
#endif
#ifndef PTH_RWS_CV_PBIT
#define PTH_RWS_CV_PBIT 0x02
#endif
#ifndef PTH_RWS_WSVBIT
#define PTH_RWS_WSVBIT 0x04
#endif
#ifndef PTHRW_RWS_SAVEMASK
#define PTHRW_RWS_SAVEMASK PTH_RWS_WSVBIT
#endif
#ifndef PTHRW_RWS_INIT
#define PTHRW_RWS_INIT PTH_RWS_SBIT
#endif
#ifndef PTH_RW_TYPE_READ
#define PTH_RW_TYPE_READ 0x01
#endif
#ifndef PTH_RW_TYPE_WRITE
#define PTH_RW_TYPE_WRITE 0x04
#endif
#ifndef PTH_RW_TYPE_MASK
#define PTH_RW_TYPE_MASK 0xff
#endif
#ifndef PTH_RW_TYPE_SHIFT
#define PTH_RW_TYPE_SHIFT 8
#endif
#ifndef PTH_RWSHFT_TYPE_READ
#define PTH_RWSHFT_TYPE_READ 0x0100
#endif
#ifndef PTH_RWSHFT_TYPE_WRITE
#define PTH_RWSHFT_TYPE_WRITE 0x0400
#endif
#ifndef PTH_RWSHFT_TYPE_MASK
#define PTH_RWSHFT_TYPE_MASK 0xff00
#endif
#ifndef KSYN_WQTYPE_RWLOCK
#define KSYN_WQTYPE_RWLOCK 0x04
#endif
#ifndef KW_UNLOCK_PREPOST
#define KW_UNLOCK_PREPOST 0x01
#endif
#ifndef KW_UNLOCK_PREPOST_READLOCK
#define KW_UNLOCK_PREPOST_READLOCK 0x08
#endif
#ifndef KW_UNLOCK_PREPOST_WRLOCK
#define KW_UNLOCK_PREPOST_WRLOCK 0x20
#endif
#ifndef SEQFIT
#define SEQFIT 0
#endif
#ifndef FIRSTFIT
#define FIRSTFIT 1
#endif

/* Queue indices (kern_synch.c KSYN_QUEUE_READ / KSYN_QUEUE_WRITE). */
#define LSK_RW_Q_READ 0
#define LSK_RW_Q_WRITE 1

/* Interrupted-wakeup types (kern_synch.c kwq_intr_type_t). */
#define LSK_RW_INTR_NONE 0
#define LSK_RW_INTR_READ 0x1
#define LSK_RW_INTR_WRITE 0x2

/* Seq comparisons, exactly as in synch_internal.h. All comparisons
 * use only the count field (bits 8..31) and wrap at half of
 * PTHRW_MAX_READERS. */
static inline int lsk_rw_is_seqlower(uint32_t x, uint32_t y) {
    x &= PTHRW_COUNT_MASK;
    y &= PTHRW_COUNT_MASK;
    if (x < y) {
        return (y - x) < (PTHRW_MAX_READERS / 2);
    }
    return (x - y) > (PTHRW_MAX_READERS / 2);
}

static inline int lsk_rw_is_seqlower_eq(uint32_t x, uint32_t y) {
    if ((x & PTHRW_COUNT_MASK) == (y & PTHRW_COUNT_MASK)) {
        return 1;
    }
    return lsk_rw_is_seqlower(x, y);
}

static inline int lsk_rw_is_seqhigher(uint32_t x, uint32_t y) {
    x &= PTHRW_COUNT_MASK;
    y &= PTHRW_COUNT_MASK;
    if (x > y) {
        return (x - y) < (PTHRW_MAX_READERS / 2);
    }
    return (y - x) > (PTHRW_MAX_READERS / 2);
}

static inline int lsk_rw_is_seqhigher_eq(uint32_t x, uint32_t y) {
    if ((x & PTHRW_COUNT_MASK) == (y & PTHRW_COUNT_MASK)) {
        return 1;
    }
    return lsk_rw_is_seqhigher(x, y);
}

static inline int lsk_rw_diff_genseq(uint32_t x, uint32_t y) {
    x &= PTHRW_COUNT_MASK;
    y &= PTHRW_COUNT_MASK;
    if (x == y) {
        return 0;
    }
    if (x > y) {
        return (int)(x - y);
    }
    return (int)((PTHRW_MAX_READERS - y) + x + PTHRW_INC);
}

static inline int lsk_rw_find_diff(uint32_t upto, uint32_t lowest) {
    uint32_t diff;
    if (upto == lowest) {
        return 0;
    }
    if (lsk_rw_is_seqlower(upto, lowest) != 0) {
        diff = (uint32_t)lsk_rw_diff_genseq(lowest, upto);
    } else {
        diff = (uint32_t)lsk_rw_diff_genseq(upto, lowest);
    }
    diff = (diff >> PTHRW_COUNT_SHIFT);
    return (int)diff;
}

static inline int lsk_rw_is_rwl_wbit_set(uint32_t x) {
    return (x & PTH_RWL_WBIT) != 0;
}

static inline int lsk_rw_is_rws_sbit_set(uint32_t x) {
    return (x & PTH_RWS_SBIT) != 0;
}

static inline int lsk_rw_is_rws_savemask_set(uint32_t x) {
    return (x & PTHRW_RWS_SAVEMASK) != 0;
}

/* Short critical sections only: kwq_lock is never held across a
 * futex wait or a libpthread call. Test-and-test-and-set spin with
 * a yield after a long spin. */
static void lsk_rw_spin_lock(uint32_t *lock) {
    unsigned spin = 0;
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0) {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#elif defined(__x86_64__)
            __asm__ volatile("pause");
#endif
            if (++spin >= 1024) {
                spin = 0;
                sys0(SYS_sched_yield);
            }
        }
    }
}

static void lsk_rw_spin_unlock(uint32_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

/* One waiter element. Lives on the waiting thread's own stack; the
 * grantor stores the grant into it under kwq_lock and removes it
 * from the queue in the same critical section, so a queued element
 * always belongs to a thread that is still waiting. The element is
 * never touched after its owner returns. */
struct lsk_rw_waiter {
    struct lsk_rw_waiter *next;
    uint32_t lockseq; /* count field of the waiter's L */
    uint32_t grant;   /* updatebits once granted, 0 until then */
    int granted;      /* set under kwq_lock together with grant */
};

/* One wait queue per rwlock address (kern_synch.c struct
 * ksyn_wait_queue, rwlock use only). kw_overlap_guard and
 * kw_initcleared are kept as separate fields, following the contract
 * section 5 list, instead of packed into a kw_kflags word. */
struct lsk_rw_queue {
    uint32_t kwq_lock; /* spinlock guarding every field below */
    uintptr_t addr;    /* rwlock address, 0 when the slot is free */
    uint32_t iocount;  /* references from lsk_rw_get; eviction needs 0 */
    uint32_t futex_word; /* bumped under kwq_lock on grant/prepost/intr */
    struct lsk_rw_waiter *read_head;
    struct lsk_rw_waiter *read_tail;
    struct lsk_rw_waiter *write_head;
    struct lsk_rw_waiter *write_tail;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t read_first;  /* lowest read seq (count field) */
    uint32_t read_last;   /* highest read seq (count field) */
    uint32_t write_first; /* lowest write seq (count field) */
    uint32_t write_last;  /* highest write seq (count field) */
    uint32_t kw_lword;    /* last L seen (count + bits) */
    uint32_t kw_uword;    /* last U seen */
    uint32_t kw_sword;    /* last S seen */
    uint32_t kw_nextseqword;   /* next sequence to hand out in updatebits */
    uint32_t kw_lastseqword;   /* rw_wc of last unlock (S count) */
    uint32_t kw_lastunlockseq; /* last unlock seq (U count) */
    uint32_t kw_overlap_guard; /* set when a read grant covered all readers */
    uint32_t kw_initcleared;   /* set after first _ksyn_check_init */
    struct {
        uint32_t count; /* prepost count */
        uint32_t lseq;  /* prepost target seq */
        uint32_t sseq;  /* prepost target sword */
    } prepost;
    struct {
        uint32_t type;       /* LSK_RW_INTR_READ / LSK_RW_INTR_WRITE */
        uint32_t count;      /* missed wakeups due to interrupts */
        uint32_t seq;        /* missed wakeup limit seq */
        uint32_t returnbits; /* return bits for the missed wakeup */
    } intr;
};

/* Fixed hash table of queues, keyed by the full 64-bit address.
 * 128 slots: a process holds far fewer live rwlocks, and idle slots
 * are reclaimed on demand. No malloc: the table is static, queues
 * are zeroed when a slot is reclaimed. */
#define LSK_RW_NQUEUES 128
static struct lsk_rw_queue g_lsk_rw_queues[LSK_RW_NQUEUES];
static uint32_t g_lsk_rw_table_lock; /* spinlock guarding the table */

static inline uint32_t lsk_rw_hash(uintptr_t addr) {
    uint64_t a = (uint64_t)addr;
    uint32_t h = (uint32_t)((a >> 4) ^ (a >> 12) ^ (a >> 20));
    return h & (LSK_RW_NQUEUES - 1);
}

static void lsk_rw_queue_zero(struct lsk_rw_queue *q) {
    unsigned char *p = (unsigned char *)q;
    size_t i;
    for (i = 0; i < sizeof(*q); i++) {
        p[i] = 0;
    }
}

/* Find or create the queue for addr. The returned queue is
 * referenced (iocount++); call lsk_rw_put when done. A queue is
 * only reclaimed for another address when it is unreferenced and
 * fully idle (no waiters, no prepost, no interrupted wakeup, no
 * overlap guard), so a queue can never be zeroed under a thread
 * that is using it. Returns NULL only if the table is full of live
 * queues; the caller treats that as EAGAIN. Must not be called
 * with kwq_lock held. */
static struct lsk_rw_queue *lsk_rw_get(uintptr_t addr) {
    uint32_t h = lsk_rw_hash(addr);
    int attempt;
    for (attempt = 0; attempt < 1000; attempt++) {
        struct lsk_rw_queue *free_slot = 0;
        struct lsk_rw_queue *evict = 0;
        uint32_t i;
        lsk_rw_spin_lock(&g_lsk_rw_table_lock);
        for (i = 0; i < LSK_RW_NQUEUES; i++) {
            struct lsk_rw_queue *q = &g_lsk_rw_queues[(h + i) & (LSK_RW_NQUEUES - 1)];
            if (q->addr == addr) {
                q->iocount++;
                lsk_rw_spin_unlock(&g_lsk_rw_table_lock);
                return q;
            }
            if (q->addr == 0 && free_slot == 0) {
                free_slot = q;
            }
            if (evict == 0 && q->addr != 0 && q->iocount == 0 &&
                q->read_count == 0 && q->write_count == 0 &&
                q->prepost.count == 0 && q->intr.count == 0 &&
                q->kw_overlap_guard == 0) {
                evict = q;
            }
        }
        if (free_slot != 0 || evict != 0) {
            struct lsk_rw_queue *q = free_slot != 0 ? free_slot : evict;
            lsk_rw_queue_zero(q);
            q->addr = addr;
            q->iocount = 1;
            lsk_rw_spin_unlock(&g_lsk_rw_table_lock);
            return q;
        }
        lsk_rw_spin_unlock(&g_lsk_rw_table_lock);
        sys0(SYS_sched_yield);
    }
    return 0;
}

static void lsk_rw_put(struct lsk_rw_queue *q) {
    lsk_rw_spin_lock(&g_lsk_rw_table_lock);
    if (q->iocount > 0) {
        q->iocount--;
    }
    lsk_rw_spin_unlock(&g_lsk_rw_table_lock);
}

/* Bump the queue futex word. Caller must hold kwq_lock. */
static void lsk_rw_bump(struct lsk_rw_queue *q) {
    q->futex_word++;
}

/* Wake every futex waiter on the queue. Caller must NOT hold
 * kwq_lock. All waiters share one futex word, so every grant is an
 * all-waiters notification; only the granted waiter proceeds, the
 * rest re-check their own grant flag and sleep again. */
static void lsk_rw_wake_all(struct lsk_rw_queue *q) {
    sys3(SYS_futex, (long)&q->futex_word, FUTEX_WAKE_PRIVATE, 0x7fffffff);
}

/* Insert a waiter SEQFIT (sorted by lockseq, FIFO within equal seq).
 * Duplicates are allowed, unlike the kernel which returns EBUSY:
 * two waiters can legitimately share a seq after a prepost/cancel
 * race, and crashing the process is worse. Caller must hold
 * kwq_lock. */
static void lsk_rw_queue_insert(struct lsk_rw_queue *q, int kqi, struct lsk_rw_waiter *w) {
    struct lsk_rw_waiter **head = kqi == LSK_RW_Q_READ ? &q->read_head : &q->write_head;
    struct lsk_rw_waiter **tail = kqi == LSK_RW_Q_READ ? &q->read_tail : &q->write_tail;
    uint32_t *count = kqi == LSK_RW_Q_READ ? &q->read_count : &q->write_count;
    uint32_t *first = kqi == LSK_RW_Q_READ ? &q->read_first : &q->write_first;
    uint32_t *last = kqi == LSK_RW_Q_READ ? &q->read_last : &q->write_last;
    struct lsk_rw_waiter *prev = 0;
    struct lsk_rw_waiter *cur;
    w->next = 0;
    if (*head == 0) {
        *head = *tail = w;
        *first = *last = w->lockseq;
    } else {
        cur = *head;
        /* Stop before the first entry with a strictly higher seq, so
         * the new waiter lands after any equal seq (FIFO). */
        while (cur != 0 && !lsk_rw_is_seqhigher(cur->lockseq, w->lockseq)) {
            prev = cur;
            cur = cur->next;
        }
        w->next = cur;
        if (prev == 0) {
            *head = w;
        } else {
            prev->next = w;
        }
        if (cur == 0) {
            *tail = w;
        }
        *first = (*head)->lockseq;
        *last = (*tail)->lockseq;
    }
    (*count)++;
}

/* Remove a waiter from its queue. Caller must hold kwq_lock. The
 * waiter must be present; removing an absent waiter is a silent
 * no-op. */
static void lsk_rw_queue_remove(struct lsk_rw_queue *q, int kqi, struct lsk_rw_waiter *w) {
    struct lsk_rw_waiter **head = kqi == LSK_RW_Q_READ ? &q->read_head : &q->write_head;
    struct lsk_rw_waiter **tail = kqi == LSK_RW_Q_READ ? &q->read_tail : &q->write_tail;
    uint32_t *count = kqi == LSK_RW_Q_READ ? &q->read_count : &q->write_count;
    uint32_t *first = kqi == LSK_RW_Q_READ ? &q->read_first : &q->write_first;
    uint32_t *last = kqi == LSK_RW_Q_READ ? &q->read_last : &q->write_last;
    struct lsk_rw_waiter *prev = 0;
    struct lsk_rw_waiter *cur = *head;
    while (cur != 0 && cur != w) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == 0) {
        return;
    }
    if (prev == 0) {
        *head = cur->next;
    } else {
        prev->next = cur->next;
    }
    if (*tail == w) {
        *tail = prev;
    }
    w->next = 0;
    (*count)--;
    if (*count == 0) {
        *first = 0;
        *last = 0;
    } else {
        *first = (*head)->lockseq;
        *last = (*tail)->lockseq;
    }
}

/* Count waiters in one queue with seq <= upto, mirroring
 * ksyn_queue_count_tolowest: the scan stops at the first entry with
 * a strictly higher seq, and also stops after counting one entry
 * equal to upto. */
static uint32_t lsk_rw_count_tolowest(struct lsk_rw_waiter *head, uint32_t count, uint32_t upto) {
    uint32_t i = 0;
    struct lsk_rw_waiter *cur;
    if (count == 0) {
        return 0;
    }
    if (lsk_rw_is_seqhigher(head->lockseq, upto)) {
        return 0;
    }
    if (upto == head->lockseq) {
        return 1;
    }
    cur = head;
    while (cur != 0) {
        uint32_t curval = cur->lockseq & PTHRW_COUNT_MASK;
        if (lsk_rw_is_seqhigher(curval, upto)) {
            break;
        }
        i++;
        if (upto == curval) {
            break;
        }
        cur = cur->next;
    }
    return i;
}

/* Count queued waiters with seq <= upto across both queues, capped
 * at nwaiters at queue granularity (mirrors find_seq_till). */
static uint32_t lsk_rw_find_seq_till(struct lsk_rw_queue *q, uint32_t upto, uint32_t nwaiters) {
    uint32_t count = 0;
    count += lsk_rw_count_tolowest(q->read_head, q->read_count, upto);
    if (count >= nwaiters) {
        return count;
    }
    count += lsk_rw_count_tolowest(q->write_head, q->write_count, upto);
    return count;
}

/* First-use init (kern_synch.c _ksyn_check_init). If the incoming L
 * has IBIT set and this queue has not been initialised since the
 * last reset, clear all per-cycle state and set kw_initcleared.
 * Returns whether IBIT was set. Caller must hold kwq_lock. */
static int lsk_rw_check_init(struct lsk_rw_queue *q, uint32_t lgenval) {
    int res = (lgenval & PTHRW_RWL_INIT) != 0;
    if (res && q->kw_initcleared == 0) {
        /* First to notice the reset of the lock, clear preposts. */
        q->kw_nextseqword = PTHRW_RWS_INIT;
        q->kw_overlap_guard = 0;
        q->prepost.count = 0;
        q->prepost.lseq = 0;
        q->prepost.sseq = PTHRW_RWS_INIT;
        q->kw_lastunlockseq = PTHRW_RWL_INIT;
        q->kw_lastseqword = PTHRW_RWS_INIT;
        q->intr.type = LSK_RW_INTR_NONE;
        q->intr.count = 0;
        q->intr.seq = 0;
        q->intr.returnbits = 0;
        q->kw_lword = 0;
        q->kw_uword = 0;
        q->kw_sword = PTHRW_RWS_INIT;
        q->kw_initcleared = 1;
    }
    return res;
}

static void lsk_rw_clear_interrupted(struct lsk_rw_queue *q) {
    q->intr.type = LSK_RW_INTR_NONE;
    q->intr.count = 0;
    q->intr.seq = 0;
    q->intr.returnbits = 0;
}

static void lsk_rw_mark_interrupted(struct lsk_rw_queue *q, uint32_t type, uint32_t count, uint32_t seq, uint32_t returnbits) {
    q->intr.count = count;
    q->intr.seq = seq;
    q->intr.returnbits = returnbits;
    q->intr.type = type;
}

/* Map a PTH_RW_TYPE_* lock type to the LSK_RW_INTR_* type stored in
 * intr. The public snapshot compares the two encodings directly
 * (PTH_RW_TYPE_WRITE 0x04 against KWQ_INTR_WRITE 0x02), so a failed
 * write wakeup could never be consumed by a retrying wrlock; the
 * mapping fixes that comparison. */
static uint32_t lsk_rw_intr_type_for(uint32_t rwtype) {
    return rwtype == PTH_RW_TYPE_READ ? (uint32_t)LSK_RW_INTR_READ : (uint32_t)LSK_RW_INTR_WRITE;
}

/* Consume one pending interrupted wakeup for this lock type and seq.
 * Returns 1 and fills *retval when consumed. Caller must hold
 * kwq_lock. */
static int lsk_rw_handle_interrupted(struct lsk_rw_queue *q, uint32_t rwtype, uint32_t lseq, uint32_t *retval) {
    if (q->intr.count != 0 && q->intr.type == lsk_rw_intr_type_for(rwtype) &&
        (q->intr.seq == 0 || lsk_rw_is_seqlower_eq(lseq, q->intr.seq))) {
        q->intr.count--;
        *retval = q->intr.returnbits;
        if (q->intr.returnbits == 0) {
            lsk_rw_clear_interrupted(q);
        }
        return 1;
    }
    return 0;
}

/* Overlap grant for a second reader arriving while a read grant is
 * outstanding (kern_synch.c _kwq_handle_overlap). Readers only.
 * Returns 1 and fills *retval when granted. Caller must hold
 * kwq_lock. */
static int lsk_rw_handle_overlap(struct lsk_rw_queue *q, uint32_t rwtype, uint32_t lgenval, uint32_t rw_wc, uint32_t *retval) {
    if (rwtype != PTH_RW_TYPE_READ) {
        return 0;
    }
    if (q->kw_overlap_guard != 0 &&
        !lsk_rw_is_rws_savemask_set(rw_wc) && !lsk_rw_is_rwl_wbit_set(lgenval)) {
        if (lsk_rw_is_seqlower_eq(rw_wc, q->kw_nextseqword) ||
            lsk_rw_is_seqhigher_eq(q->kw_lastseqword, rw_wc)) {
            q->kw_nextseqword += PTHRW_INC;
            *retval = PTHRW_INC | ((q->kw_nextseqword & PTHRW_BIT_MASK) | PTH_RWL_MBIT);
            return 1;
        }
    }
    return 0;
}

/* Find the lowest waiter across both queues (kern_synch.c
 * kwq_find_rw_lowest). flags carries KW_UNLOCK_PREPOST_READLOCK /
 * KW_UNLOCK_PREPOST_WRLOCK when the caller is a preposting
 * reader/writer; premgen is its seq (count field). Fills *typep
 * (low byte: lowest type, high byte: PTH_RWSHFT_TYPE_* presence
 * bits) and lowest[LSK_RW_Q_READ] / lowest[LSK_RW_Q_WRITE]. When
 * nothing is present, *typep is 0. Lowest seq wins, so a writer
 * with a lower seq than queued readers is granted first; on a tie
 * the reader wins (it is checked first and only a strictly lower
 * seq replaces it). Caller must hold kwq_lock. */
static void lsk_rw_find_lowest(struct lsk_rw_queue *q, int flags, uint32_t premgen, int *typep, uint32_t lowest[2]) {
    int type = 0;
    int count = 0;
    int i;
    uint32_t numbers[2];
    int typenum[2];
    uint32_t kw_fr = 0;
    uint32_t kw_fwr = 0;
    uint32_t low;
    int lowtype;
    if (q->read_count != 0 || (flags & KW_UNLOCK_PREPOST_READLOCK) != 0) {
        type |= PTH_RWSHFT_TYPE_READ;
        if (q->read_count != 0) {
            kw_fr = q->read_first;
            if ((flags & KW_UNLOCK_PREPOST_READLOCK) != 0 &&
                lsk_rw_is_seqlower(premgen, kw_fr)) {
                kw_fr = premgen;
            }
        } else {
            kw_fr = premgen;
        }
        lowest[LSK_RW_Q_READ] = kw_fr;
        numbers[count] = kw_fr;
        typenum[count] = PTH_RW_TYPE_READ;
        count++;
    } else {
        lowest[LSK_RW_Q_READ] = 0;
    }
    if (q->write_count != 0 || (flags & KW_UNLOCK_PREPOST_WRLOCK) != 0) {
        type |= PTH_RWSHFT_TYPE_WRITE;
        if (q->write_count != 0) {
            kw_fwr = q->write_first;
            if ((flags & KW_UNLOCK_PREPOST_WRLOCK) != 0 &&
                lsk_rw_is_seqlower(premgen, kw_fwr)) {
                kw_fwr = premgen;
            }
        } else {
            kw_fwr = premgen;
        }
        lowest[LSK_RW_Q_WRITE] = kw_fwr;
        numbers[count] = kw_fwr;
        typenum[count] = PTH_RW_TYPE_WRITE;
        count++;
    } else {
        lowest[LSK_RW_Q_WRITE] = 0;
    }
    if (count == 0) {
        if (typep != 0) {
            *typep = 0;
        }
        return;
    }
    low = numbers[0];
    lowtype = typenum[0];
    for (i = 1; i < count; i++) {
        if (lsk_rw_is_seqlower(numbers[i], low)) {
            low = numbers[i];
            lowtype = typenum[i];
        }
    }
    type |= lowtype;
    if (typep != 0) {
        *typep = type;
    }
}

/* Grant the read updatebits to queued readers, in SEQFIT order,
 * while allreaders is set or the first reader seq is strictly below
 * limitread (kern_synch.c ksyn_wakeupreaders). Each granted waiter
 * is removed from the queue, its grant stored, and the queue futex
 * word bumped. Returns the number of failed wakes, which is 0 in
 * this design: a queued waiter is always genuinely waiting on its
 * own stack element, so a grant never misses. *wokenp counts the
 * granted waiters. Caller must hold kwq_lock. */
static int lsk_rw_wakeup_readers(struct lsk_rw_queue *q, uint32_t limitread, int allreaders, uint32_t updatebits, int *wokenp) {
    int numwoken = 0;
    while (q->read_count != 0 &&
        (allreaders || lsk_rw_is_seqlower(q->read_first, limitread))) {
        struct lsk_rw_waiter *w = q->read_head;
        lsk_rw_queue_remove(q, LSK_RW_Q_READ, w);
        w->grant = updatebits;
        w->granted = 1;
        lsk_rw_bump(q);
        numwoken++;
    }
    if (wokenp != 0) {
        *wokenp = numwoken;
    }
    return 0;
}

/* Grant the next lock(s) after an unlock, or on arrival of the last
 * preposted waiter (kern_synch.c kwq_handle_unlock). Downgrade
 * calls it as a preposting reader, and upgrade builds on the same
 * queues.
 *
 * flags: 0, or KW_UNLOCK_PREPOST plus KW_UNLOCK_PREPOST_READLOCK /
 * KW_UNLOCK_PREPOST_WRLOCK when the caller is a preposting
 * reader/writer. premgen is the preposting caller's seq (count
 * field). On return *updatep holds the updatebits for the caller
 * (0 when nothing was granted to the caller); *blockp, when
 * non-NULL, is 0 when the preposting caller was granted and must
 * return updatebits, 1 when it must block; *wokenp counts queued
 * waiters that were granted (their futex wake is the caller's job,
 * after dropping kwq_lock). Caller must hold kwq_lock.
 *
 * Invariants kept here: a pending writer requires WBIT in
 * updatebits; exactly one writer is ever granted per call; readers
 * are never granted past the lowest pending writer. */
static int lsk_rw_handle_unlock(struct lsk_rw_queue *q, uint32_t mgen, uint32_t rw_wc, uint32_t *updatep, int flags, int *blockp, uint32_t premgen, int *wokenp) {
    uint32_t low_writer = 0;
    uint32_t limitrdnum = 0;
    int rwtype = 0;
    int allreaders = 0;
    int nfailed = 0;
    uint32_t updatebits = 0;
    uint32_t numneeded = 0;
    uint32_t lowest[2];
    int woken = 0;
    int block = 1;
    (void)mgen;

    q->kw_lastseqword = rw_wc;
    q->kw_lastunlockseq = rw_wc & PTHRW_COUNT_MASK;
    q->kw_overlap_guard = 0;

    lsk_rw_find_lowest(q, flags, premgen, &rwtype, lowest);
    if ((rwtype & PTH_RW_TYPE_MASK) == 0) {
        /* No waiter and no preposting caller: nothing to grant. The
         * kernel panics here; return 0 instead. Callers guarantee
         * this is unreachable (unlock returns early when both queues
         * are empty and diff is 0; the prepost path always has a
         * preposting caller). */
        if (updatep != 0) {
            *updatep = 0;
        }
        if (blockp != 0) {
            *blockp = 1;
        }
        if (wokenp != 0) {
            *wokenp = 0;
        }
        return 0;
    }
    low_writer = lowest[LSK_RW_Q_WRITE];

    if ((rwtype & PTH_RW_TYPE_MASK) == PTH_RW_TYPE_READ) {
        if ((rwtype & PTH_RWSHFT_TYPE_MASK) != 0 &&
            (rwtype & PTH_RWSHFT_TYPE_WRITE) != 0) {
            updatebits |= (PTH_RWL_WBIT | PTH_RWL_KBIT);
        }
        limitrdnum = 0;
        if ((rwtype & PTH_RWSHFT_TYPE_WRITE) != 0) {
            limitrdnum = low_writer;
        } else {
            allreaders = 1;
        }
        numneeded = 0;
        if ((rwtype & PTH_RWSHFT_TYPE_WRITE) != 0) {
            limitrdnum = low_writer;
            numneeded = lsk_rw_count_tolowest(q->read_head, q->read_count, limitrdnum);
            if ((flags & KW_UNLOCK_PREPOST_READLOCK) != 0 &&
                lsk_rw_is_seqlower(premgen, limitrdnum)) {
                /* Preposting reader is below the lowest writer. */
                block = 0;
                numneeded += 1;
            }
        } else {
            /* No writers at all: all queued readers, plus the
             * preposting reader when there is one. */
            q->kw_overlap_guard = 1;
            numneeded += q->read_count;
            if ((flags & KW_UNLOCK_PREPOST_READLOCK) != 0) {
                block = 0;
                numneeded += 1;
            }
        }
        updatebits += (numneeded << PTHRW_COUNT_SHIFT);
        q->kw_nextseqword = (rw_wc & PTHRW_COUNT_MASK) + updatebits;
        nfailed = lsk_rw_wakeup_readers(q, limitrdnum, allreaders, updatebits, &woken);
        if (nfailed != 0) {
            lsk_rw_mark_interrupted(q, (uint32_t)LSK_RW_INTR_READ, (uint32_t)nfailed, limitrdnum, updatebits);
        }
    } else if ((rwtype & PTH_RW_TYPE_MASK) == PTH_RW_TYPE_WRITE) {
        /* Only one writer is ever granted. */
        updatebits |= PTHRW_INC;
        updatebits |= PTH_RWL_KBIT | PTH_RWL_EBIT;
        if ((flags & KW_UNLOCK_PREPOST_WRLOCK) != 0 && low_writer == premgen) {
            /* Grant to the preposting writer itself. */
            block = 0;
            if (q->write_count != 0) {
                updatebits |= PTH_RWL_WBIT;
            }
        } else {
            /* Not granting to the preposting writer: WBIT when more
             * writers are queued or a preposting writer exists. */
            if (q->write_count > 1 || (flags & KW_UNLOCK_PREPOST_WRLOCK) != 0) {
                updatebits |= PTH_RWL_WBIT;
            }
            /* Wake exactly the lowest writer. */
            if (q->write_head != 0) {
                struct lsk_rw_waiter *w = q->write_head;
                lsk_rw_queue_remove(q, LSK_RW_Q_WRITE, w);
                w->grant = updatebits;
                w->granted = 1;
                lsk_rw_bump(q);
                woken = 1;
            } else {
                /* No queued writer but the lowest is a preposter
                 * that is not the caller: unreachable (find_lowest
                 * only reports a preposter when the flag is set,
                 * and then low_writer equals premgen). Record it
                 * as an interrupted wakeup rather than dropping it. */
                lsk_rw_mark_interrupted(q, (uint32_t)LSK_RW_INTR_WRITE, 1, low_writer, updatebits);
            }
        }
        q->kw_nextseqword = (rw_wc & PTHRW_COUNT_MASK) + updatebits;
    } else {
        /* Invalid type: the kernel panics. Return 0 without granting. */
        if (updatep != 0) {
            *updatep = 0;
        }
        if (blockp != 0) {
            *blockp = 1;
        }
        if (wokenp != 0) {
            *wokenp = 0;
        }
        return 0;
    }

    if (updatep != 0) {
        *updatep = updatebits;
    }
    if (blockp != 0) {
        *blockp = block;
    }
    if (wokenp != 0) {
        *wokenp = woken;
    }
    return 0;
}

/* Handle a preposted wakeup (kern_synch.c
 * _kwq_handle_preposted_wakeup). Returns 1 and fills *retval when
 * the caller is the last preposted waiter and handle_unlock granted
 * it; returns 0 when the caller must block (no prepost, not the
 * last preposted waiter, or handle_unlock says block). *wokenp
 * counts queued waiters granted by the handle_unlock call. Caller
 * must hold kwq_lock. */
static int lsk_rw_handle_preposted(struct lsk_rw_queue *q, uint32_t rwtype, uint32_t lseq, uint32_t *retval, int *wokenp) {
    int prepost_type = rwtype == PTH_RW_TYPE_READ ? KW_UNLOCK_PREPOST_READLOCK : KW_UNLOCK_PREPOST_WRLOCK;
    uint32_t pp_lseq;
    uint32_t pp_sseq;
    uint32_t updatebits = 0;
    int should_block = 1;
    if (q->prepost.count == 0 || !lsk_rw_is_seqlower_eq(lseq, q->prepost.lseq)) {
        return 0;
    }
    q->prepost.count--;
    if (q->prepost.count > 0) {
        /* Not the last preposted waiter: fall through to blocking. */
        return 0;
    }
    pp_lseq = q->prepost.lseq;
    pp_sseq = q->prepost.sseq;
    q->prepost.count = 0;
    q->prepost.lseq = 0;
    q->prepost.sseq = PTHRW_RWS_INIT;
    q->kw_initcleared = 0;
    lsk_rw_handle_unlock(q, pp_lseq, pp_sseq, &updatebits,
        prepost_type | KW_UNLOCK_PREPOST, &should_block, lseq, wokenp);
    if (should_block) {
        return 0;
    }
    *retval = updatebits;
    return 1;
}

/* Shared rdlock/wrlock slow path (kern_synch.c __psynch_rw_lock).
 * is_read selects the read or write queue. Returns the grant
 * updatebits, or (uint32_t)-1 with Darwin errno set (EINTR when a
 * signal interrupted the wait; the waiter is dequeued, libpthread
 * retries and re-enqueues with the same seq). */
static uint32_t lsk_rw_lock_common(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags, int is_read) {
    struct lsk_rw_queue *q;
    uint32_t rwtype = is_read ? (uint32_t)PTH_RW_TYPE_READ : (uint32_t)PTH_RW_TYPE_WRITE;
    uint32_t lockseq = lgenval & PTHRW_COUNT_MASK;
    uint32_t retval = 0;
    int kqi = is_read ? LSK_RW_Q_READ : LSK_RW_Q_WRITE;
    int woke = 0;
    struct lsk_rw_waiter waiter;
    uint32_t futex_val;
    (void)ugenval;
    (void)flags;
    if (rwlock == 0) {
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    q = lsk_rw_get(rwlock);
    if (q == 0) {
        set_errno(35); /* EAGAIN: queue table full of live queues */
        return (uint32_t)-1;
    }
    lsk_rw_spin_lock(&q->kwq_lock);
    lsk_rw_check_init(q, lgenval);
    if (lsk_rw_handle_interrupted(q, rwtype, lockseq, &retval) ||
        lsk_rw_handle_overlap(q, rwtype, lgenval, rw_wc, &retval)) {
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        return retval;
    }
    if (lsk_rw_handle_preposted(q, rwtype, lockseq, &retval, &woke)) {
        lsk_rw_spin_unlock(&q->kwq_lock);
        if (woke > 0) {
            lsk_rw_wake_all(q);
        }
        lsk_rw_put(q);
        return retval;
    }
    /* Block: enqueue on this thread's stack element and futex-wait.
     * The grant is stored in the element by the grantor under
     * kwq_lock; the wait loop re-checks it after every wake. A
     * prepost-triggered handle_unlock may have granted other queued
     * waiters while this caller was not itself granted (woke > 0
     * with handle_preposted returning 0): those waiters are asleep
     * on the futex word and nobody else will wake them, so wake
     * them here before this caller blocks. */
    waiter.next = 0;
    waiter.lockseq = lockseq;
    waiter.grant = 0;
    waiter.granted = 0;
    lsk_rw_queue_insert(q, kqi, &waiter);
    futex_val = q->futex_word;
    lsk_rw_spin_unlock(&q->kwq_lock);
    if (woke > 0) {
        lsk_rw_wake_all(q);
    }
    for (;;) {
        long ret;
        ret = sys4(SYS_futex, (long)&q->futex_word, FUTEX_WAIT_PRIVATE, (long)futex_val, 0L);
#ifdef THREAD_HOST_TEST
        /* Host shim only: the futex call goes through libc syscall(),
         * which returns -1 and sets errno instead of the raw -errno
         * the target svc returns. Normalize so the EINTR/EAGAIN
         * checks below see the raw values. errno is visible here
         * through the shim linux.h's <errno.h>. */
        if (ret == -1) {
            if (errno == 4) {
                ret = -4L; /* EINTR */
            } else if (errno == 11) {
                ret = -11L; /* EAGAIN */
            }
        }
#endif
        if (ret < 0 && ret != -11L && ret != -4L) {
            /* Unexpected futex error (not a wake, not EAGAIN, not
             * EINTR): yield once so a persistent error cannot spin
             * the queue lock, then re-check the grant. */
            sys0(SYS_sched_yield);
        }
        lsk_rw_spin_lock(&q->kwq_lock);
        if (waiter.granted) {
            retval = waiter.grant;
            lsk_rw_spin_unlock(&q->kwq_lock);
            lsk_rw_put(q);
            return retval;
        }
        if (ret == -4L) {
            /* -EINTR: a signal arrived. Dequeue and let libpthread
             * retry with the same seq. A grant that lands first
             * wins instead (checked above). */
            lsk_rw_queue_remove(q, kqi, &waiter);
            lsk_rw_spin_unlock(&q->kwq_lock);
            lsk_rw_put(q);
            set_errno(4); /* EINTR */
            return (uint32_t)-1;
        }
        futex_val = q->futex_word;
        lsk_rw_spin_unlock(&q->kwq_lock);
    }
}

/* Shared unlock/unlock2 path (kern_synch.c _psynch_rw_unlock).
 * Returns the updatebits from kwq_handle_unlock (0 when nothing
 * was granted, lgenval on the prepost path); libpthread ignores the
 * value except for the -1 check, but wake correctness is the whole
 * point. Returns (uint32_t)-1 with errno set only on a NULL address
 * or a full queue table. */
static uint32_t lsk_rw_unlock_common(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    struct lsk_rw_queue *q;
    uint32_t updatebits = 0;
    uint32_t curgen = lgenval & PTHRW_COUNT_MASK;
    uint32_t count = 0;
    int diff = 0;
    int isinit = 0;
    int clearedkflags = 0;
    int woke = 0;
    (void)flags;
    if (rwlock == 0) {
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    q = lsk_rw_get(rwlock);
    if (q == 0) {
        set_errno(35); /* EAGAIN: queue table full of live queues */
        return (uint32_t)-1;
    }
    lsk_rw_spin_lock(&q->kwq_lock);
    isinit = lsk_rw_check_init(q, lgenval);
    /* Stale unlock: this generation was already unlocked. */
    if (q->kw_lastunlockseq != PTHRW_RWL_INIT &&
        lsk_rw_is_seqlower(ugenval, q->kw_lastunlockseq)) {
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        return 0;
    }
    diff = lsk_rw_find_diff(lgenval, ugenval);
    if (diff == 0 && q->read_count == 0 && q->write_count == 0) {
        /* Required deviation: no waiters at all and no inbound
         * waiters. The kernel would panic in kwq_find_rw_lowest
         * here; return 0 without doing anything. */
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        return 0;
    }
    count = lsk_rw_find_seq_till(q, curgen, (uint32_t)diff);
    if (count < (uint32_t)diff) {
        /* Waiters are expected but not queued yet: prepost. */
        if (lsk_rw_is_rws_sbit_set(q->prepost.sseq) ||
            lsk_rw_is_seqhigher_eq(rw_wc, q->prepost.sseq)) {
            q->prepost.count = (uint32_t)diff - count;
            q->prepost.lseq = curgen;
            q->prepost.sseq = rw_wc;
            lsk_rw_bump(q);
            updatebits = lgenval; /* no unlock handling happened */
            lsk_rw_spin_unlock(&q->kwq_lock);
            lsk_rw_put(q);
            return updatebits;
        }
        /* Contract deviation from the public snapshot, which
         * returns 0 here: clear the stale prepost and run the
         * unlock handling so queued waiters are granted. */
        q->prepost.count = 0;
        q->prepost.lseq = 0;
        q->prepost.sseq = PTHRW_RWS_INIT;
    }
    /* No prepost and all waiters are in place. */
    if (isinit != 0 && q->kw_initcleared != 0) {
        q->kw_initcleared = 0;
        clearedkflags = 1;
    }
    q->prepost.count = 0;
    q->prepost.lseq = 0;
    q->prepost.sseq = PTHRW_RWS_INIT;
    lsk_rw_handle_unlock(q, lgenval, rw_wc, &updatebits, 0, 0, 0, &woke);
    if (clearedkflags != 0 && q->intr.count > 0) {
        /* An interrupted wakeup is pending: keep the reset state
         * until the retrying thread has consumed it. */
        q->kw_initcleared = 1;
    }
    lsk_rw_spin_unlock(&q->kwq_lock);
    if (woke > 0) {
        lsk_rw_wake_all(q);
    }
    lsk_rw_put(q);
    return updatebits;
}

uint32_t lsk_psynch_rw_rdlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_rdlock");
uint32_t lsk_psynch_rw_rdlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    return lsk_rw_lock_common(rwlock, lgenval, ugenval, rw_wc, flags, 1);
}

uint32_t lsk_psynch_rw_wrlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_wrlock");
uint32_t lsk_psynch_rw_wrlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    return lsk_rw_lock_common(rwlock, lgenval, ugenval, rw_wc, flags, 0);
}

uint32_t lsk_psynch_rw_unlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_unlock");
uint32_t lsk_psynch_rw_unlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    return lsk_rw_unlock_common(rwlock, lgenval, ugenval, rw_wc, flags);
}

uint32_t lsk_psynch_rw_unlock2(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_unlock2");
uint32_t lsk_psynch_rw_unlock2(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    return lsk_psynch_rw_unlock(rwlock, lgenval, ugenval, rw_wc, flags);
}


/* ------------------------------------------------------------------ */
/* psynch rwlock upgrade / downgrade / longrdlock / yieldwrlock       */
/* (contract sections 7.4, 7.5).                    */
/*                                                                     */
/* Built on the shared queue and helpers without modifying them.       */
/* Arg convention [guess]: lgenval is the CURRENT L word, as for       */
/* rdlock/wrlock/unlock (libpthread passes the current seq words).     */
/* The caller's own lock generation is therefore not separately        */
/* available; upgrade uses lockseq = lgenval & COUNT_MASK.             */
/*                                                                     */
/* upgrade: reader-to-writer conversion, blocking. The caller keeps    */
/* its read share while it waits, so the grant must wait until it is   */
/* the SOLE remaining reader. Sole-reader test, under kwq_lock:        */
/*   other = find_diff(lgenval, ugenval)                               */
/*         - queued waiters with seq <= lgenval (both queues) - 1     */
/* diff counts lock generations not yet accounted for by an unlock;    */
/* queued waiters account for their own generations; the remaining 1   */
/* is the caller's own read share. other == 0 means no other holder.   */
/* The test is conservative: in-flight waiters and the range-counted   */
/* phantom generations left by out-of-order unlocks inflate other,     */
/* so it delays the grant rather than granting early.                  */
/*   - other == 0: grant immediately (0x103, or 0x107 when writers    */
/*     are queued behind, so L keeps WBIT). No unlock will ever come   */
/*     to grant a sole reader, so waiting would deadlock. A pending    */
/*     prepost is cleared: its expected waiters arrive later, enqueue  */
/*     normally and wait for the next unlock instead of consuming it   */
/*     and being granted while this writer holds.                      */
/*   - an earlier writer is pending (lowest write seq <= lockseq)      */
/*     and other != 0: return -1/EBUSY. This is a deliberate           */
/*     deviation from the contract's "Do NOT return EBUSY": blocking   */
/*     here is unsafe, because the next unlock's handle_unlock would   */
/*     grant that earlier writer while the caller still holds its      */
/*     read share (reader plus writer corruption),                     */
/*     and no later unlock can rescue it. EBUSY means "you             */
/*     still hold the read lock; retry after the writer finishes".     */
/*   - otherwise: consume a preposted wakeup if there is one, then     */
/*     enqueue in the write queue and futex-wait for a write grant,    */
/*     exactly as wrlock does. Later unlocks grant the lowest writer;  */
/*     the caller is the lowest writer once no earlier writer is       */
/*     pending, so the last other reader's unlock grants it.           */
/* Known limit [guess]: if a prepost is pending at entry, an           */
/* in-flight writer can still arrive, consume the prepost and be       */
/* granted while the caller holds read. The queue cannot tell an       */
/* in-flight waiter from a live holder (range-counted diff), so no     */
/* safe behaviour exists inside this function; a queue-side marker     */
/* for the upgrading holder would fix it. No caller exists in 23G90    */
/* libpthread, so this is unobservable until one appears.              */
/*                                                                     */
/* downgrade: writer-to-reader conversion, non-blocking. Runs the      */
/* READ branch of kwq_handle_unlock as if the caller were              */
/* a preposting reader. premgen is 0: the caller's own generation is   */
/* not recoverable from the downgrade args, and 0 makes the caller     */
/* the lowest reader, which yields exactly the contract's grant        */
/* (itself plus queued readers below the lowest writer) in every       */
/* reachable state, because queued writers always carry a higher seq   */
/* than the current holder's generation. Queued writers stay blocked   */
/* behind WBIT as in a normal read grant.                              */
/*                                                                     */
/* longrdlock / yieldwrlock: the public kern_synch.c snapshot just     */
/* returns ESRCH for both, and no 23G90 libpthread caller exists.      */
/* Return -1 with errno ESRCH.                                         */
/* ------------------------------------------------------------------ */

/* Futex wait for a waiter the caller already enqueued (upgrade      */
/* only). lock_common carries its own copy of this loop;               */
/* it must not be refactored to share this one. futex_val is the      */
/* queue futex word read under kwq_lock right after the insert, so    */
/* a grant stored between the insert and the first wait is not        */
/* missed. Returns the stored grant. On futex EINTR the waiter is     */
/* dequeued and (uint32_t)-1 is returned with errno EINTR. Never      */
/* sleeps holding kwq_lock. The caller owns the queue reference. */
static uint32_t lsk_rw_upgrade_wait(struct lsk_rw_queue *q, struct lsk_rw_waiter *w, int kqi, uint32_t futex_val) {
    uint32_t retval;
    for (;;) {
        long ret;
        ret = sys4(SYS_futex, (long)&q->futex_word, FUTEX_WAIT_PRIVATE, (long)futex_val, 0L);
#ifdef THREAD_HOST_TEST
        /* Host shim only: the futex call goes through libc syscall(),
         * which returns -1 and sets errno instead of the raw -errno
         * the target svc returns. Normalize so the EINTR/EAGAIN
         * checks below see the raw values. */
        if (ret == -1) {
            if (errno == 4) {
                ret = -4L; /* EINTR */
            } else if (errno == 11) {
                ret = -11L; /* EAGAIN */
            }
        }
#endif
        if (ret < 0 && ret != -11L && ret != -4L) {
            /* Unexpected futex error (not a wake, not EAGAIN, not
             * EINTR): yield once so a persistent error cannot spin
             * the queue lock, then re-check the grant. */
            sys0(SYS_sched_yield);
        }
        lsk_rw_spin_lock(&q->kwq_lock);
        if (w->granted) {
            retval = w->grant;
            lsk_rw_spin_unlock(&q->kwq_lock);
            return retval;
        }
        if (ret == -4L) {
            /* -EINTR: a signal arrived. Dequeue and let the caller
             * retry with the same seq. A grant that lands first
             * wins instead (checked above). */
            lsk_rw_queue_remove(q, kqi, w);
            lsk_rw_spin_unlock(&q->kwq_lock);
            set_errno(4); /* EINTR */
            return (uint32_t)-1;
        }
        futex_val = q->futex_word;
        lsk_rw_spin_unlock(&q->kwq_lock);
    }
}

uint32_t lsk_psynch_rw_upgrade(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_upgrade");
uint32_t lsk_psynch_rw_upgrade(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    struct lsk_rw_queue *q;
    uint32_t lockseq = lgenval & PTHRW_COUNT_MASK;
    uint32_t curgen = lockseq;
    uint32_t retval = 0;
    int diff = 0;
    int other = 0;
    int earlier_writer = 0;
    int woke = 0;
    struct lsk_rw_waiter waiter;
    uint32_t futex_val;
    (void)flags;
    if (rwlock == 0) {
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    q = lsk_rw_get(rwlock);
    if (q == 0) {
        set_errno(35); /* EAGAIN: queue table full of live queues */
        return (uint32_t)-1;
    }
    lsk_rw_spin_lock(&q->kwq_lock);
    lsk_rw_check_init(q, lgenval);
    if (lsk_rw_handle_interrupted(q, (uint32_t)PTH_RW_TYPE_WRITE, lockseq, &retval)) {
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        return retval;
    }
    diff = lsk_rw_find_diff(lgenval, ugenval);
    if (diff == 0) {
        /* The caller holds no read share to convert. */
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    other = diff;
    other -= (int)lsk_rw_count_tolowest(q->read_head, q->read_count, curgen);
    other -= (int)lsk_rw_count_tolowest(q->write_head, q->write_count, curgen);
    other -= 1; /* the caller's own read share */
    earlier_writer = q->write_count != 0 &&
        lsk_rw_is_seqlower_eq(q->write_first, lockseq);
    if (earlier_writer && other != 0) {
        /* An earlier writer is pending and other readers still
         * hold: blocking would let the next unlock grant that
         * writer while the caller still holds its read share.
         * Refuse instead; the caller keeps its read lock. See the
         * block comment above. */
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        set_errno(16); /* EBUSY */
        return (uint32_t)-1;
    }
    if (other == 0) {
        /* Sole remaining reader: grant immediately. No unlock will
         * ever come to grant a sole reader, so waiting would
         * deadlock. Jump ahead of any queued writer (they stay
         * queued behind WBIT) rather than deadlocking behind one. */
        retval = PTHRW_INC | PTH_RWL_KBIT | PTH_RWL_EBIT;
        if (q->write_count != 0) {
            retval |= PTH_RWL_WBIT;
        }
        q->kw_nextseqword = (rw_wc & PTHRW_COUNT_MASK) + retval;
        q->kw_overlap_guard = 0;
        q->prepost.count = 0;
        q->prepost.lseq = 0;
        q->prepost.sseq = PTHRW_RWS_INIT;
        lsk_rw_spin_unlock(&q->kwq_lock);
        lsk_rw_put(q);
        return retval;
    }
    if (lsk_rw_handle_preposted(q, (uint32_t)PTH_RW_TYPE_WRITE, lockseq, &retval, &woke)) {
        lsk_rw_spin_unlock(&q->kwq_lock);
        if (woke > 0) {
            lsk_rw_wake_all(q);
        }
        lsk_rw_put(q);
        return retval;
    }
    /* Block: enqueue in the write queue and futex-wait for a write
     * grant, exactly as wrlock does. A prepost-triggered
     * handle_unlock may have granted other queued waiters while
     * this caller was not itself granted (woke > 0 with
     * handle_preposted returning 0): wake them here before this
     * caller blocks, as lock_common does. */
    waiter.next = 0;
    waiter.lockseq = lockseq;
    waiter.grant = 0;
    waiter.granted = 0;
    lsk_rw_queue_insert(q, LSK_RW_Q_WRITE, &waiter);
    futex_val = q->futex_word;
    lsk_rw_spin_unlock(&q->kwq_lock);
    if (woke > 0) {
        lsk_rw_wake_all(q);
    }
    retval = lsk_rw_upgrade_wait(q, &waiter, LSK_RW_Q_WRITE, futex_val);
    lsk_rw_put(q);
    return retval;
}

uint32_t lsk_psynch_rw_downgrade(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_downgrade");
uint32_t lsk_psynch_rw_downgrade(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    struct lsk_rw_queue *q;
    uint32_t updatebits = 0;
    int block = 1;
    int woke = 0;
    (void)ugenval;
    (void)flags;
    if (rwlock == 0) {
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    q = lsk_rw_get(rwlock);
    if (q == 0) {
        set_errno(35); /* EAGAIN: queue table full of live queues */
        return (uint32_t)-1;
    }
    lsk_rw_spin_lock(&q->kwq_lock);
    lsk_rw_check_init(q, lgenval);
    /* Convert the held write share into a read share by running the
     * READ branch of kwq_handle_unlock as if the caller were a
     * preposting reader. premgen is 0 (see the block comment above):
     * it makes the caller the lowest reader, so the grant covers
     * itself plus queued readers below the lowest writer, and
     * queued writers stay blocked behind WBIT. */
    lsk_rw_handle_unlock(q, lgenval, rw_wc, &updatebits,
        KW_UNLOCK_PREPOST | KW_UNLOCK_PREPOST_READLOCK, &block, 0, &woke);
    lsk_rw_spin_unlock(&q->kwq_lock);
    if (woke > 0) {
        lsk_rw_wake_all(q);
    }
    if (block != 0) {
        /* Unreachable with premgen 0: the READ branch always grants
         * the preposting reader. Defensive only. */
        lsk_rw_put(q);
        set_errno(22); /* EINVAL */
        return (uint32_t)-1;
    }
    lsk_rw_put(q);
    return updatebits;
}

uint32_t lsk_psynch_rw_yieldwrlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_yieldwrlock");
uint32_t lsk_psynch_rw_yieldwrlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    (void)rwlock; (void)lgenval; (void)ugenval; (void)rw_wc; (void)flags;
    set_errno(3); /* ESRCH */
    return (uint32_t)-1;
}

uint32_t lsk_psynch_rw_longrdlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) __asm__("___psynch_rw_longrdlock");
uint32_t lsk_psynch_rw_longrdlock(uintptr_t rwlock, uint32_t lgenval, uint32_t ugenval, uint32_t rw_wc, int flags) {
    (void)rwlock; (void)lgenval; (void)ugenval; (void)rw_wc; (void)flags;
    set_errno(3); /* ESRCH */
    return (uint32_t)-1;
}

/* Semwait */
int lsk_semwait_signal(int cond_sem, int mutex_sem, int timeout, int relative, int64_t tv_sec, int32_t tv_nsec) __asm__("___semwait_signal");
int lsk_semwait_signal(int cond_sem, int mutex_sem, int timeout, int relative, int64_t tv_sec, int32_t tv_nsec) {
    (void)cond_sem; (void)mutex_sem; (void)relative;
    if (timeout) {
        struct {
            long tv_sec;
            long tv_nsec;
        } req, rem;
        req.tv_sec = (long)tv_sec;
        req.tv_nsec = (long)tv_nsec;
        sys2(SYS_nanosleep, (long)&req, (long)&rem);
        set_errno(60); /* ETIMEDOUT */
        return -1;
    }
    return 0;
}

int lsk_semwait_signal_nocancel(int cond_sem, int mutex_sem, int timeout, int relative, int64_t tv_sec, int32_t tv_nsec) __asm__("___semwait_signal_nocancel");
int lsk_semwait_signal_nocancel(int cond_sem, int mutex_sem, int timeout, int relative, int64_t tv_sec, int32_t tv_nsec) {
    return lsk_semwait_signal(cond_sem, mutex_sem, timeout, relative, tv_sec, tv_nsec);
}

#define WQ_FLAG_THREAD_KEVENT   0x00080000u
#define WQ_FLAG_THREAD_WORKLOOP 0x00400000u
#define WQ_DISPATCH_WLH_ANON    ((uint64_t)-4)

void lsk_wq_thread_on_spawn_wlh(void *pthread, uint32_t kport, uint32_t flags, uint64_t wlh) {
    long tid = thread_gettid();
    uint64_t wq_fn = (uint64_t)g_wqthread;
#if defined(__aarch64__) && !defined(THREAD_HOST_TEST)
    __asm__ volatile("xpaci %0" : "+r"(wq_fn));
#endif
    lsk_thread_reg_add(tid, (void *)(uintptr_t)wq_fn, (void *)(uintptr_t)wlh, pthread, kport, flags);
    if (g_ipc_trace) {
        struct thread_buf b;
        tb_init(&b);
        tb_str(&b, "[thread] wq-create tid=0x");
        tb_hex(&b, (uint64_t)tid);
        tb_str(&b, " prio=0x");
        tb_hex(&b, flags);
        if ((flags & WQ_FLAG_THREAD_KEVENT) || wlh == WQ_DISPATCH_WLH_ANON) {
            tb_str(&b, " kevent ");
        } else if ((flags & WQ_FLAG_THREAD_WORKLOOP) || wlh != 0) {
            tb_str(&b, " workloop ");
        } else {
            tb_str(&b, " workq ");
        }
        tb_str(&b, "id=0x");
        tb_hex(&b, wlh);
        tb_str(&b, " pthread=0x");
        tb_hex(&b, (uint64_t)pthread);
        tb_str(&b, " kport=0x");
        tb_hex(&b, kport);
        tb_str(&b, "\n");
        tb_flush(&b);
    }
}

void lsk_wq_thread_on_spawn(void *pthread, uint32_t kport, uint32_t flags) {
    /* workq.c:wq_run_task_trampoline stores bound_wlh (or 0) at self - 0x488
     * right before calling workq_get_wqthread(). */
    uint64_t wlh = 0;
    if (pthread) {
        wlh = *(volatile uint64_t *)((uintptr_t)pthread - 0x488);
    }
    lsk_wq_thread_on_spawn_wlh(pthread, kport, flags, wlh);
}

#if defined(__aarch64__) && !defined(THREAD_HOST_TEST)
__asm__(
".text\n"
".globl ___iosw_wqthread_trampoline\n"
".p2align 2\n"
"___iosw_wqthread_trampoline:\n"
"    // x0 = self, x1 = kport, x2 = stackbottom, x3 = keventlist\n"
"    // w4 = flags, w5 = nevents\n"
"    tbnz w4, #17, 1f\n"      // WQ_FLAG_THREAD_REUSE (0x20000): already registered
"    stp  x0, x1, [sp, #-80]!\n"
"    stp  x2, x3, [sp, #16]\n"
"    stp  x4, x5, [sp, #32]\n"
"    stp  x6, x7, [sp, #48]\n"
"    str  x30,    [sp, #64]\n"
"    mov  w2, w4\n"
"    bl   _lsk_wq_thread_on_spawn\n"
"    ldp  x2, x3, [sp, #16]\n"
"    ldp  x4, x5, [sp, #32]\n"
"    ldp  x6, x7, [sp, #48]\n"
"    ldr  x30,    [sp, #64]\n"
"    ldp  x0, x1, [sp], #80\n"
"1:\n"
"    adrp x16, _g_wqthread@PAGE\n"
"    ldr  x16, [x16, _g_wqthread@PAGEOFF]\n"
"    xpaci x16\n"
"    br   x16\n"
);
extern char __iosw_wqthread_trampoline[];
#endif

void *workq_get_wqthread(void) {
#if defined(__aarch64__) && !defined(THREAD_HOST_TEST)
    if (!g_wqthread) return 0;
    return (void *)__iosw_wqthread_trampoline;
#else
    return g_wqthread;
#endif
}

int lsk_bsdthread_ctl(void *arg1, void *arg2, void *arg3) __asm__("___bsdthread_ctl");
int lsk_bsdthread_ctl(void *arg1, void *arg2, void *arg3) {
    uintptr_t cmd = (uintptr_t)arg1;
    if (cmd == 0x800) {
        return 4; /* QOS_MAX_PARALLELISM */
    }
    (void)arg2; (void)arg3;
    return 0;
}

int lsk_thread_selfusage(void *buf) __asm__("___thread_selfusage");
int lsk_thread_selfusage(void *buf) {
    (void)buf;
    return 0;
}

int lsk_pthread_chdir(const char *path) __asm__("___pthread_chdir");
int lsk_pthread_chdir(const char *path) {
    (void)path;
    return 0;
}

int lsk_pthread_fchdir(int fd) __asm__("___pthread_fchdir");
int lsk_pthread_fchdir(int fd) {
    (void)fd;
    return 0;
}

int lsk_thread_switch(uint32_t new_thread, int option, uint32_t timeout_ms) __asm__("_thread_switch");
int lsk_thread_switch(uint32_t new_thread, int option, uint32_t timeout_ms) {
    (void)new_thread;
    sys0(SYS_sched_yield);
    if ((option == 2 /* SWITCH_OPTION_WAIT */ || (option == 1 && timeout_ms > 0)) && timeout_ms > 0) {
        struct {
            long tv_sec;
            long tv_nsec;
        } req;
        req.tv_sec = (long)(timeout_ms / 1000);
        req.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
        sys2(SYS_nanosleep, (long)&req, 0);
    }
    return 0; /* KERN_SUCCESS */
}

int lsk_syscall_thread_switch(uint32_t new_thread, int option, uint32_t timeout_ms) __asm__("_syscall_thread_switch");
int lsk_syscall_thread_switch(uint32_t new_thread, int option, uint32_t timeout_ms) {
    return lsk_thread_switch(new_thread, option, timeout_ms);
}
