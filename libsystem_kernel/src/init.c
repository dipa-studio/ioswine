#include "linux.h"
#include <stdint.h>
#include <stddef.h>

extern void *iosw_ref____kernelVersionString;
extern void *iosw_ref____kernelVersionNumber;
extern void *iosw_ref__mach_errors;
extern uint32_t *iosw_ref_port_obj_table_size;
extern void *iosw_ref_important_boost_assertion_token;
extern void *iosw_ref_normal_boost_assertion_token;
extern void *iosw_ref_non_boost_assertion_token;
extern void *iosw_ref_denap_boost_assertion_token;
extern void *iosw_ref_NDR_record;
extern void *iosw_ref_update_tpidr;
extern void *iosw_ref_vprintf_stderr_func;
extern void *iosw_ref_mach_error_full_diag;
extern void *iosw_ref_port_obj_table;
extern uint32_t *iosw_ref__cpu_capabilities;
extern uint32_t *iosw_ref__cpu_has_altivec;
extern void *iosw_ref___syscall_logger;
extern int32_t *iosw_ref__current_pid;
extern uint64_t *iosw_ref_vm_page_size;
extern uint32_t *iosw_ref_vm_page_shift;
extern uint64_t *iosw_ref_vm_page_mask;
extern uint64_t *iosw_ref_vm_kernel_page_size;
extern uint64_t *iosw_ref_vm_kernel_page_mask;
extern uint32_t *iosw_ref_mach_task_self_;
extern uint32_t *iosw_ref_vm_kernel_page_shift;
extern uint32_t *iosw_ref_bootstrap_port;
extern void *iosw_ref__os_alloc_once_table;
extern int32_t *iosw_ref_errno;
extern void *iosw_ref_internal_catch_exc_subsystem;

/* Main thread TSD buffer.
 * In Darwin arm64, struct _pthread is at (tpidr_el0 - 224).
 * By placing tpidr_el0 at offset 0x1000 within a 16 KiB buffer,
 * (tpidr_el0 - 224) and all TSD slots [0..256] are safely resident.
 */
static uint8_t g_main_thread_buf[16384] __attribute__((aligned(16384)));
static int g_main_errno = 0;
const void *g_kernel_funcs = 0;

/* Signal handler for debugging early crashes */
__asm__(
".text\n"
".globl _lsk_sigreturn\n"
"_lsk_sigreturn:\n"
"    mov x8, #139\n"
"    svc #0\n"
"    brk #0\n"
);
void lsk_sigreturn(void);

struct k_sigaction {
    void (*handler)(int, void *, void *);
    unsigned long flags;
    void (*restorer)(void);
    uint64_t mask;
};

struct sigcontext_a64 { uint64_t fault_address, regs[31], sp, pc, pstate; };

static void put_hex(uint64_t val) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[18] = '\0';
    sys_write(2, buf, 18);
}

static void crash_handler(int sig, void *info, void *uc) {
    /* Log the crashing thread id and the fault address:
     * which thread the AG update ran on (SwiftUI runs graph updates
     * on its own AsyncRenderer thread, not the main thread), and
     * whether the fault address is 0 (a nil deref, x8=0)
     * or garbage (heap corruption). si_addr is at byte offset 16 in
     * the Linux siginfo_t (si_signo/si_errno/si_code, then the
     * _sigfault union). Both fields are raw syscalls / plain reads,
     * safe in a signal handler. */
    uint64_t fault_addr = 0;
    long crash_tid = 0;
    if (info) fault_addr = *(const uint64_t *)((const uint8_t *)info + 16);
    crash_tid = sys0(SYS_gettid);
    struct sigcontext_a64 *mc = (struct sigcontext_a64 *)((uint8_t *)uc + 176);
    sys_write(2, "\n[lsk CRASH] sig=", 17);
    put_hex(sig);
    sys_write(2, " pc=", 4);
    put_hex(mc->pc);
    sys_write(2, " lr=", 4);
    put_hex(mc->regs[30]);
    sys_write(2, " sp=", 4);
    put_hex(mc->sp);
    sys_write(2, " fp=", 4);
    put_hex(mc->regs[29]);
    sys_write(2, " tid=", 5);
    put_hex((uint64_t)crash_tid);
    sys_write(2, " fault=", 7);
    put_hex(fault_addr);
    sys_write(2, " x0=", 4);
    put_hex(mc->regs[0]);
    sys_write(2, " x8=", 4);
    put_hex(mc->regs[8]);
    sys_write(2, " x9=", 4);
    put_hex(mc->regs[9]);
    sys_write(2, " x16=", 5);
    put_hex(mc->regs[16]);
    sys_write(2, "\n[lsk REGS] x19=", 16);
    put_hex(mc->regs[19]);
    sys_write(2, " x20=", 5);
    put_hex(mc->regs[20]);
    sys_write(2, " x21=", 5);
    put_hex(mc->regs[21]);
    sys_write(2, " x22=", 5);
    put_hex(mc->regs[22]);
    sys_write(2, " x23=", 5);
    put_hex(mc->regs[23]);
    sys_write(2, " x24=", 5);
    put_hex(mc->regs[24]);
    sys_write(2, " x25=", 5);
    put_hex(mc->regs[25]);
    sys_write(2, "\n[lsk ALLREGS]", 14);
    for (int r = 0; r < 31; r++) {
        char nm[6] = " x00="; nm[2] = (char)('0' + r / 10); nm[3] = (char)('0' + r % 10);
        sys_write(2, nm, 5); put_hex(mc->regs[r]);
        if (r % 8 == 7) sys_write(2, "\n[lsk ALLREGS]", 14);
    }
    sys_write(2, "\n[lsk STACK]", 13);
    { uint64_t *sp = (uint64_t *)mc->sp;
      for (int k = 0; k < 24; k++) { sys_write(2, " ", 1); put_hex(sp[k]); if (k % 6 == 5) sys_write(2, "\n[lsk STACK]", 13); } }
    sys_write(2, "\n", 1);

    uint64_t *frame = (uint64_t *)mc->regs[29];
    for (int i = 0; i < 16 && frame; i++) {
        sys_write(2, "  [lsk frame] lr=", 17);
        put_hex(frame[1]);
        sys_write(2, " fp=", 4);
        put_hex((uint64_t)frame);
        sys_write(2, "\n", 1);
        uint64_t next_fp = frame[0];
        if (next_fp <= (uint64_t)frame || next_fp >= 0x7fffffffff || (next_fp & 7))
            break;
        frame = (uint64_t *)next_fp;
    }

    if (mc->pc == 0x1be046260 || (mc->pc >= 0x1be000000 && mc->pc < 0x1be100000)) {
        const char *dispatch_msg = *(const char * const *)(0x1eb635238 + 8);
        if (dispatch_msg && (uint64_t)dispatch_msg > 0x100000000 && (uint64_t)dispatch_msg < 0x7fffffffff) {
            sys_write(2, "  [libdispatch crash] ", 22);
            int len = 0;
            while (dispatch_msg[len] && len < 256) len++;
            sys_write(2, dispatch_msg, len);
            sys_write(2, "\n", 1);
        }
        uint64_t q = mc->regs[19];
        if (q > 0x100000000 && q < 0x7fffffffff) {
            const char *label = *(const char * const *)(q + 72);
            if (!label || (uint64_t)label < 0x100000000 || (uint64_t)label >= 0x7fffffffff) {
                label = *(const char * const *)(q + 40);
            }
            if (label && (uint64_t)label > 0x100000000 && (uint64_t)label < 0x7fffffffff) {
                sys_write(2, "  [queue label] ", 16);
                int len = 0;
                while (label[len] && len < 128) len++;
                sys_write(2, label, len);
                sys_write(2, "\n", 1);
            }
        }
        extern void workq_dump_state(void);
        workq_dump_state();
    }

    if (sig == 5) {
        uint64_t cand[4] = { mc->regs[0], mc->regs[19], mc->regs[20], mc->regs[21] };
        for (int c = 0; c < 4; c++) {
            const char *str = (const char *)cand[c];
            if ((uint64_t)str < 0x100000000ULL || (uint64_t)str > 0x7fffffffffULL) continue;
            int len = 0, ok = 1;
            while (len < 300 && str[len]) { if ((unsigned char)str[len] < 9 || (unsigned char)str[len] > 126) { ok = 0; break; } len++; }
            if (ok && len >= 8) { sys_write(2, "[lsk TRAP msg] ", 15); sys_write(2, str, len); sys_write(2, "\n", 1); }
        }
    }
    {
        const uint64_t *hp = *(const uint64_t * const *)0x1ecc9e608ULL;
        sys_write(2, "[lsk HOOKS] dispatch xpc hooks=", 31); put_hex((uint64_t)hp);
        if ((uint64_t)hp > 0x100000000ULL && (uint64_t)hp < 0x7fffffffffULL) {
            for (int i = 0; i < 6; i++) { sys_write(2, " ", 1); put_hex(hp[i] & 0xffffffffffffULL); }
        }
        sys_write(2, "\n", 1);
    }
    sys1(SYS_exit_group, 128 + sig);
}

/* Diagnostics: SIGQUIT dumps every thread's pc/lr and frame chain without exiting (send it from outside,
 * e.g. the devq "*-dump" tests). The receiving thread re-signals all other threads with SIGUSR2. */
struct init_buf {
    char data[256];
    size_t len;
};
static void ib_init(struct init_buf *b) { b->len = 0; }
static void ib_str(struct init_buf *b, const char *s) {
    while (*s && b->len < sizeof(b->data) - 1) b->data[b->len++] = *s++;
}
static void ib_hex0x(struct init_buf *b, uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    ib_str(b, "0x");
    for (int i = 15; i >= 0 && b->len < sizeof(b->data) - 1; i--) {
        b->data[b->len++] = hex[(val >> (i * 4)) & 0xf];
    }
}
static void ib_flush(struct init_buf *b) {
    if (b->len > 0) {
        sys_write(2, b->data, b->len);
        b->len = 0;
    }
}

/* Async-signal-safe mapping check using raw SYS_mincore so signal dumps
 * never fault on unmapped addresses (e.g. CF lock when CF is not loaded,
 * corrupted frame pointers, or unmapped ulock addresses). */
static int addr_readable(uint64_t addr, size_t len) {
    if (len == 0 || addr < 0x10000ULL || addr >= 0xffffffffffffULL) return 0;
    if (addr + len < addr) return 0;
    uint64_t page_sz = 4096;
    uint64_t p = addr & ~(page_sz - 1);
    unsigned char vec[4];
    long r = sys3(SYS_mincore, (long)p, (long)page_sz, (long)vec);
    if (r == -22 /* EINVAL: 16K page kernel */) {
        page_sz = 16384;
        p = addr & ~(page_sz - 1);
        r = sys3(SYS_mincore, (long)p, (long)page_sz, (long)vec);
    }
    if (r != 0) return 0;
    uint64_t p2 = (addr + len - 1) & ~(page_sz - 1);
    if (p2 != p && sys3(SYS_mincore, (long)p2, (long)page_sz, (long)vec) != 0) return 0;
    return 1;
}

static volatile int g_dump_lock;
static void dump_frames(const char *tag, void *uc) {
    (void)tag;
    int got_lock = 0;
    for (int spin = 0; spin < 10000; spin++) {
        if (!__atomic_exchange_n(&g_dump_lock, 1, __ATOMIC_ACQUIRE)) {
            got_lock = 1;
            break;
        }
        sys0(SYS_sched_yield);
    }

    uint64_t t = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t));
    uint32_t tsd3 = 0xdead;
    if (addr_readable(t + 24, 4)) {
        tsd3 = *(volatile uint32_t *)(t + 24);
    }

    struct init_buf b;
    ib_init(&b);
    ib_str(&b, "[lsk DUMP] tid=");
    ib_hex0x(&b, (uint64_t)sys0(SYS_gettid));
    ib_str(&b, " tsd3=");
    ib_hex0x(&b, tsd3);

    uint64_t fp_val = 0;
    if (uc && addr_readable((uint64_t)uc + 176, sizeof(struct sigcontext_a64))) {
        struct sigcontext_a64 *mc = (struct sigcontext_a64 *)((uint8_t *)uc + 176);
        ib_str(&b, " pc="); ib_hex0x(&b, mc->pc);
        ib_str(&b, " lr="); ib_hex0x(&b, mc->regs[30]);
        ib_str(&b, " x0="); ib_hex0x(&b, mc->regs[0]);
        ib_str(&b, " x1="); ib_hex0x(&b, mc->regs[1]);
        ib_str(&b, " x16="); ib_hex0x(&b, mc->regs[16]);
        fp_val = mc->regs[29];
    }
    ib_str(&b, "\n");
    ib_flush(&b);

    uint64_t *fp = (uint64_t *)fp_val;
    for (int f = 0; f < 24 && fp && ((uintptr_t)fp > 0x100000) && (((uintptr_t)fp & 7) == 0); f++) {
        if (!addr_readable((uint64_t)fp, 16)) break;
        ib_init(&b);
        ib_str(&b, "  lr=");
        ib_hex0x(&b, fp[1] & 0xffffffffffffULL);
        ib_str(&b, "\n");
        ib_flush(&b);
        uint64_t next_fp = fp[0];
        if (next_fp <= (uint64_t)fp || next_fp >= 0xffffffffffffULL || (next_fp & 7)) break;
        fp = (uint64_t *)next_fp;
    }

    if (got_lock) {
        __atomic_store_n(&g_dump_lock, 0, __ATOMIC_RELEASE);
    }
}

static void usr2_handler(int sig, void *info, void *uc) {
    (void)sig;
    (void)info;
    dump_frames("", uc);
}

static void quit_handler(int sig, void *info, void *uc) {
    (void)sig;
    (void)info;
    /* Dump the live-thread registry first so it prints even if
     * later frame walking or optional state probes encounter anything odd. */
    {
        extern void lsk_thread_dump(void);
        lsk_thread_dump();
    }
    {
        struct ulock_dbg { long tid; uint64_t addr; uint64_t val; uint32_t op; uint32_t sleeping; };
        extern struct ulock_dbg g_ulock_dbg[64];
        struct init_buf b;
        for (int i = 0; i < 64; i++) {
            if (!g_ulock_dbg[i].sleeping) continue;
            ib_init(&b);
            ib_str(&b, "[lsk ULOCK] tid="); ib_hex0x(&b, (uint64_t)g_ulock_dbg[i].tid);
            ib_str(&b, " op="); ib_hex0x(&b, g_ulock_dbg[i].op);
            ib_str(&b, " addr="); ib_hex0x(&b, g_ulock_dbg[i].addr);
            ib_str(&b, " val="); ib_hex0x(&b, g_ulock_dbg[i].val);
            if (addr_readable(g_ulock_dbg[i].addr, 8)) {
                ib_str(&b, " now="); ib_hex0x(&b, *(volatile uint64_t *)g_ulock_dbg[i].addr);
            }
            ib_str(&b, "\n");
            ib_flush(&b);
        }
    }
    if (addr_readable(0x1eccb3e78ULL, 4)) {
        struct init_buf b;
        ib_init(&b);
        ib_str(&b, "[lsk DUMP] CF lock 0x1eccb3e78 = ");
        ib_hex0x(&b, *(volatile uint32_t *)0x1eccb3e78ULL);
        ib_str(&b, "\n");
        ib_flush(&b);
    }
    dump_frames("", uc);
    long self = sys0(SYS_gettid);
    long pid = sys0(SYS_getpid);
    int fd = (int)sys4(SYS_openat, -100, (long)"/proc/self/task", 0x4000 /* O_DIRECTORY (arm64) */, 0);
    if (fd < 0) return;
    char buf[2048];
    for (;;) {
        long n = sys3(SYS_getdents64, fd, (long)buf, sizeof(buf));
        if (n <= 0) break;
        for (long off = 0; off < n;) {
            uint16_t reclen = *(uint16_t *)(buf + off + 16);
            const char *name = buf + off + 19;
            long tid = 0;
            for (int i = 0; name[i] >= '0' && name[i] <= '9'; i++) tid = tid * 10 + (name[i] - '0');
            if (tid > 0 && tid != self) sys3(131 /* tgkill */, pid, tid, 12 /* SIGUSR2 */);
            off += reclen;
        }
    }
    sys1(SYS_close, fd);
}

static inline void *raw_code_ptr(uint64_t v) {
    /* Strip PAC bits so Linux rt_sigaction receives a canonical handler/restorer. */
    __asm__ volatile("xpaci %0" : "+r"(v));
    return (void *)v;
}

static void ensure_quit_signal(void) {
    struct k_sigaction sa;
    void (*qfn)(int, void *, void *) = (void (*)(int, void *, void *))raw_code_ptr((uint64_t)quit_handler);
    void (*ufn)(int, void *, void *) = (void (*)(int, void *, void *))raw_code_ptr((uint64_t)usr2_handler);
    void (*rfn)(void) = (void (*)(void))raw_code_ptr((uint64_t)lsk_sigreturn);

    sa.flags = 0x00000004 | 0x04000000 | 0x10000000; /* SA_SIGINFO | SA_RESTORER | SA_RESTART */
    sa.restorer = rfn;
    sa.mask = 0;

    struct k_sigaction cur;
    cur.handler = 0;
    long qret = sys4(SYS_rt_sigaction, 3, 0, &cur, 8);
    if (qret != 0 || cur.handler != qfn) {
        sa.handler = qfn;
        long r = sys4(SYS_rt_sigaction, 3, &sa, 0, 8);
        if (r < 0) sys_write(2, "[lsk] warning: rt_sigaction(SIGQUIT) failed\n", 44);
    }

    cur.handler = 0;
    long uret = sys4(SYS_rt_sigaction, 12, 0, &cur, 8);
    if (uret != 0 || cur.handler != ufn) {
        sa.handler = ufn;
        long r = sys4(SYS_rt_sigaction, 12, &sa, 0, 8);
        if (r < 0) sys_write(2, "[lsk] warning: rt_sigaction(SIGUSR2) failed\n", 44);
    }

    uint64_t unblock = (1ULL << (3 - 1)) | (1ULL << (12 - 1)) |
                       (1ULL << (4 - 1)) | (1ULL << (5 - 1)) |
                       (1ULL << (7 - 1)) | (1ULL << (11 - 1));
    long mret = sys4(SYS_rt_sigprocmask, 1 /* SIG_UNBLOCK */, (long)&unblock, 0, 8);
    if (mret < 0) sys_write(2, "[lsk] warning: rt_sigprocmask unblock failed\n", 45);
}

static void install_crash_handlers(void) {
    struct k_sigaction sa;
    sa.handler = (void (*)(int, void *, void *))raw_code_ptr((uint64_t)crash_handler);
    sa.flags = 0x00000004 | 0x04000000; /* SA_SIGINFO | SA_RESTORER */
    sa.restorer = (void (*)(void))raw_code_ptr((uint64_t)lsk_sigreturn);
    sa.mask = 0;

    sys4(SYS_rt_sigaction, 4, &sa, 0, 8);  /* SIGILL */
    sys4(SYS_rt_sigaction, 5, &sa, 0, 8);  /* SIGTRAP */
    sys4(SYS_rt_sigaction, 7, &sa, 0, 8);  /* SIGBUS */
    sys4(SYS_rt_sigaction, 11, &sa, 0, 8); /* SIGSEGV */
    ensure_quit_signal();
}

static uint64_t dummy_tpro_stub(void) {
    return 0;
}

static char s_munge_token[] = "PTHREAD_PTR_MUNGE_TOKEN=0x12345678";
static const char **g_lsk_envp = NULL;

const char *lsk_getenv(const char *name) {
    if (!g_lsk_envp || !name) return NULL;
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    for (int i = 0; g_lsk_envp[i]; i++) {
        const char *s = g_lsk_envp[i];
        size_t j = 0;
        while (j < nlen && s[j] == name[j]) j++;
        if (j == nlen && s[j] == '=') {
            return &s[j + 1];
        }
    }
    return NULL;
}

/* Mach-O: ___libkernel_init */
void __libkernel_init(const void *funcs, const char *envp[], const char *apple[], const void *vars) {
    g_lsk_envp = envp;
    /* Version banner: printed unconditionally as the very first
     * log line, before any gated logging, so a device log proves which
     * binary ran even when IOSWINE_LOG=1 was not set. */
    sys_write(2, "[lsk] ioswine-067 2026-11-18\n", 29);
    int lsk_log = (lsk_getenv("IOSWINE_LOG") != NULL);
    if (lsk_log) sys_write(2, "[lsk] __libkernel_init entered\n", 31);
    install_crash_handlers();
    (void)apple;
    (void)vars;

    /* Populate __TPRO_CONST early runtime stub slots (0x1ec954000 .. 0x1ec954028).
     * These slots are called by QuartzCore and libdispatch (e.g. __dispatch_continuation_alloc_init).
     */
    if (sys_mprotect((void *)0x1ec954000ull, 16384, 3 /* PROT_READ|PROT_WRITE */) == 0) {
        for (int i = 0; i < 6; i++) {
            uint64_t addr = 0x1ec954000ull + (uint64_t)i * 8;
            uint64_t fn = (uint64_t)&dummy_tpro_stub;
            __asm__ volatile("pacia %0, %1" : "+r"(fn) : "r"(addr));
            *(uint64_t *)addr = fn;
        }
        sys_mprotect((void *)0x1ec954000ull, 16384, 1 /* PROT_READ */);
        if (lsk_log) sys_write(2, "[lsk] __TPRO_CONST stubs initialized\n", 37);
    } else {
        if (lsk_log) sys_write(2, "[lsk] warning: mprotect __TPRO_CONST failed\n", 44);
    }

    /* Check envp for IOSWINE_IPC_TRACE=1 and inject PTHREAD_PTR_MUNGE_TOKEN for libpthread */
    if (envp) {
        char **ep = (char **)envp;
        int replaced = 0;
        for (int i = 0; ep[i]; i++) {
            const char *s = ep[i];
            if (s[0] == 'I' && s[1] == 'O' && s[2] == 'S' && s[3] == 'W' &&
                s[4] == 'I' && s[5] == 'N' && s[6] == 'E' && s[7] == '_') {
                if ((s[8] == 'I' && s[9] == 'P' && s[10] == 'C' && s[11] == '_' &&
                     s[12] == 'T' && s[13] == 'R' && s[14] == 'A' && s[15] == 'C' &&
                     s[16] == 'E' && s[17] == '=' && s[18] == '1') ||
                    (s[8] == 'L' && s[9] == 'O' && s[10] == 'G' && s[11] == '=' && s[12] == '1')) {
                    extern void ipc_set_trace(int trace);
                    ipc_set_trace(1);
                }
                if (!replaced && s[8] == 'D' && s[9] == 'S' && s[10] == 'C') {
                    ep[i] = s_munge_token;
                    replaced = 1;
                }
            }
        }
        if (!replaced) {
            for (int i = 0; ep[i]; i++) {
                const char *s = ep[i];
                if (s[0] == 'I' && s[1] == 'O' && s[2] == 'S' && s[3] == 'W' &&
                    s[4] == 'I' && s[5] == 'N' && s[6] == 'E' && s[7] == '_') {
                    ep[i] = s_munge_token;
                    replaced = 1;
                    break;
                }
            }
        }
        if (!replaced && ep[0]) {
            ep[0] = s_munge_token;
        }
    }

    g_kernel_funcs = funcs;

    /* Set up main thread TSD base in tpidr_el0 */
    uint64_t tsd_base = (uint64_t)&g_main_thread_buf[0x1000];
    uint64_t *slots = (uint64_t *)tsd_base;
    slots[0] = tsd_base;                    /* slot 0: self TSD base */
    slots[1] = (uint64_t)&g_main_errno;    /* slot 1: int *errno */
    slots[2] = 0;                           /* slot 2: mach reply port */
    slots[3] = 0x303;                       /* slot 3: mach_thread_self */
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tsd_base));

    extern void ipc_init(void);
    ipc_init();

    /* System daemons: cfprefsd, notifyd, MobileGestalt, lsd, container, csops.
     * IOSWINE_NO_DAEMONS=1 disables them (A/B debugging). daemons_init() also runs svchost_init(). */
    if (!lsk_getenv("IOSWINE_NO_DAEMONS")) {
        extern int daemons_init(void);
        daemons_init();
    } else {
        const char *svc_ping = lsk_getenv("IOSWINE_SVC_PING");
        if (svc_ping && svc_ping[0] == '1' && svc_ping[1] == '\0') {
            extern int svchost_init(void);
            svchost_init();
        }
    }

    /* Fake FrontBoard + RunningBoard, opt-in until verified on device. */
    const char *fb = lsk_getenv("IOSWINE_FAKEBOARD");
    if (fb && fb[0] == '1') {
        extern int fakeboard_init(void);
        extern int fakerb_init(void);
        extern void fakerb_set_self_pid(uint32_t pid);
        fakeboard_init();
        fakerb_init();
        fakerb_set_self_pid((uint32_t)sys0(SYS_getpid));
        /* Fake BackBoardServices HID event delivery manager (* folded in from 018): serves com.apple.backboard.hid-services.xpc
         * over svchost so +[BKSHIDEventDeliveryManager sharedInstance] can
         * stay live and _UIEventDeferringManager can save records.
         * Same init path as fakeboard; IOSWINE_BKSHID=0 opts it out
         * (bkshid_is_enabled), the A/B control for the device run. */
        extern int bkshid_init(void);
        bkshid_init();
    }

    /* Initialize Apple data exports */
    if (iosw_ref_mach_task_self_) *iosw_ref_mach_task_self_ = 0x103;
    if (iosw_ref_bootstrap_port) *iosw_ref_bootstrap_port = 0x203;
    if (iosw_ref_vm_page_size) *iosw_ref_vm_page_size = 16384;
    if (iosw_ref_vm_page_shift) *iosw_ref_vm_page_shift = 14;
    if (iosw_ref_vm_page_mask) *iosw_ref_vm_page_mask = 0x3fff;
    if (iosw_ref_vm_kernel_page_size) *iosw_ref_vm_kernel_page_size = 16384;
    if (iosw_ref_vm_kernel_page_shift) *iosw_ref_vm_kernel_page_shift = 14;
    if (iosw_ref_vm_kernel_page_mask) *iosw_ref_vm_kernel_page_mask = 0x3fff;
    if (iosw_ref__current_pid) *iosw_ref__current_pid = (int32_t)sys0(SYS_getpid);
    if (iosw_ref_errno) *iosw_ref_errno = 0;

    /* Map Darwin _COMM_PAGE64_RO_ADDRESS (0xfffff4000, 32 KiB up to 0xfffffc000)
     * Apple libraries read _COMM_PAGE_DEV_FIRM at 0xfffff4084.
     */
    void *ro_page = sys_mmap((void *)0xfffff4000, 0x8000, 3 /* PROT_READ|PROT_WRITE */,
                             0x22 /* MAP_PRIVATE|MAP_ANON|MAP_FIXED */, -1, 0);
    if ((long)ro_page > 0) {
        const char *cp = (const char *)0xfffffc000;
        char *dst = (char *)ro_page;
        for (int i = 0; i < 4096; i++) dst[i] = cp[i];
    }

    if (lsk_log) sys_write(2, "[lsk] __libkernel_init returning\n", 33);
}

/* Mach-O: ___libkernel_init_late */
void __libkernel_init_late(const void *arg) {
    (void)arg;
    /* iOS 26's libSystem never calls this; ipc.c calls it once libdispatch is live. */
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    ensure_quit_signal();
    const char *snap = lsk_getenv("IOSWINE_SNAPSHOT");
    if (snap && snap[0] == '1' && snap[1] == '\0') {
        extern void snapshot_init(void);
        snapshot_init();
    }
    /* carender: vsync-driven render engine. Strict
     * no-op unless IOSWINE_CARENDER=1. Runs after snapshot_init so
     * the snapshot fallback path is available to the engine. */
    {
        const char *ca = lsk_getenv("IOSWINE_CARENDER");
        if (ca && ca[0] == '1' && ca[1] == '\0') {
            extern void carender_init(void);
            carender_init();
        }
    }
    /* cadisplay-link: CADisplayLink hooks. Runs
     * unconditionally (the app can create a display link on any
     * run) and before the app's main thread starts, so the first
     * factory call already goes to the hook. The install itself
     * is all-or-nothing and no-ops when the class or a runtime
     * symbol is missing; IOSWINE_NO_CADISPLAYLINK=1 disables it. */
    {
        extern int cadisplay_link_install_hooks(void);
        cadisplay_link_install_hooks();
    }
    /* tuikb-validate (second round): validate
     * TUIKBGraphSerialization's _deserializationCache after every
     * objectForOffset: call and log the first corrupt step. This is
     * a DEBUG bisect tool, OFF by default; it only installs when
     * IOSWINE_TUIKB_VALIDATE=1, and IOSWINE_NO_TUIKB_VALIDATE=1
     * disables it. */
    {
        extern int tuikb_validate_install_hooks(void);
        tuikb_validate_install_hooks();
    }
    /* tuikb-lock: serialize TUIKBGraphSerialization's
     * _deserializationCache (the decoded-object cache behind the
     * keyboard layout decode). Runs unconditionally (the keyboard
     * can decode a layout on any run) and before the app's main
     * thread starts, so the first decode already goes through the
     * wrapper. The install is all-or-nothing and no-ops when the
     * class or a runtime symbol is missing;
     * IOSWINE_NO_TUIKB_LOCK=1 disables it. */
    {
        extern int tuikb_lock_install_hooks(void);
        tuikb_lock_install_hooks();
    }
    /* mainscreen: retry the +[UIScreen mainScreen]
     * hook install (first attempted in __libkernel_init). The
     * install is a no-op when already installed. */
    {
        extern int mainscreen_install_hooks(void);
        mainscreen_install_hooks();
    }
}

/* Mach-O: ___libkernel_init_after_boot_tasks */
void __libkernel_init_after_boot_tasks(const void *arg) {
    (void)arg;
}

/* Mach-O: ___libkernel_platform_init */
void __libkernel_platform_init(const void *arg) {
    (void)arg;
}

/* Mach-O: ___libkernel_voucher_init */
extern const uintptr_t *g_voucher_table;
int __libkernel_voucher_init(const void *arg) {
    g_voucher_table = (const uintptr_t *)arg;
    return 0;
}
