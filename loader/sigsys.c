/* Seccomp "Darwin syscall trap" (ARCHITECTURE.md §5).
   Syscalls issued from allowed ranges (loader text, later our libsystem_kernel) run natively; any other
   svc raises SIGSYS and is emulated here using the Darwin ABI (x16 = number, carry = error). */
#include "sigsys.h"
#include "rt.h"
#include "sys.h"

#define SIGSYS_NO       31
#define SA_SIGINFO      0x00000004
#define SA_RESTORER     0x04000000
#define PR_SET_NO_NEW_PRIVS 38
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define AUDIT_ARCH_AARCH64       0xc00000b7u
#define PSTATE_C                 (1ull << 29)
#define SYS_rt_sigreturn 139

struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sock_fprog  { uint16_t len; struct sock_filter *filter; };
#define BPF_LD_W_ABS 0x20
#define BPF_JEQ_K    0x15
#define BPF_JGE_K    0x35
#define BPF_RET_K    0x06
#define STMT(c, k)        ((struct sock_filter){ (c), 0, 0, (k) })
#define JUMP(c, k, t, f)  ((struct sock_filter){ (c), (t), (f), (k) })
/* struct seccomp_data: nr@0 arch@4 ip@8 (lo), ip_hi@12, args@16 */

struct k_sigaction { void *handler; unsigned long flags; void *restorer; uint64_t mask; };

/* arm64 struct ucontext: mcontext (struct sigcontext) at offset 176 */
struct sigcontext_a64 { uint64_t fault_address, regs[31], sp, pc, pstate; };
#define UC_MCONTEXT(uc) ((struct sigcontext_a64 *)((uint8_t *)(uc) + 176))

static struct { uint64_t lo, hi; } g_allow[SIGSYS_MAX_RANGES];
static int g_nallow;

void sigsys_allow_range(uint64_t lo, uint64_t hi) {
    if (g_nallow == SIGSYS_MAX_RANGES) die("too many seccomp allow ranges");
    if ((lo >> 32) != ((hi - 1) >> 32)) die("allow range 0x%lx-0x%lx crosses a 4 GiB boundary", lo, hi);
    g_allow[g_nallow].lo = lo;
    g_allow[g_nallow].hi = hi;
    g_nallow++;
}

/* restorer for rt_sigreturn; lives in loader text so the filter allows it */
__asm__(".text\n.p2align 2\n.globl ioswine_sigreturn\nioswine_sigreturn:\n"
        "\tmov x8, #139\n\tsvc #0\n\tbrk #0\n");
void ioswine_sigreturn(void);

/* Darwin errno == Linux errno for 1..34; above that they differ. Minimal map, extended later. */
static long darwin_errno(long linux_err) {
    switch (linux_err) {
    case 11: return 35;   /* EAGAIN */
    case 38: return 78;   /* ENOSYS */
    case 36: return 63;   /* ENAMETOOLONG */
    case 39: return 66;   /* ENOTEMPTY */
    case 40: return 62;   /* ELOOP */
    default: return linux_err <= 34 ? linux_err : 5 /* EIO */;
    }
}

static void darwin_syscall(struct sigcontext_a64 *mc) {
    long n = (long)mc->regs[16];
    uint64_t *x = mc->regs;
    long r;
    switch (n) {
    case 1:  sys1(SYS_exit_group, x[0]); __builtin_unreachable();      /* exit */
    case 3:  r = sys3(SYS_read, x[0], x[1], x[2]); break;              /* read */
    case 4:  r = sys3(SYS_write, x[0], x[1], x[2]); break;             /* write */
    case 6:  r = sys1(SYS_close, x[0]); break;                         /* close */
    case 20: r = sys0(SYS_getpid); break;                              /* getpid */
    case (long)0x80000000:                                             /* machdep */
        if (x[3] == 2) { __asm__ volatile("msr tpidr_el0, %0" ::"r"(x[0])); r = 0; break; } /* thread_set_tsd_base */
        ldprintf(2, "[loader] unimplemented machdep call %lu at pc 0x%lx\n", x[3], mc->pc - 4);
        r = -38;
        break;
    default:
        ldprintf(2, "[loader] unimplemented Darwin syscall %ld at pc 0x%lx\n", n, mc->pc - 4);
        r = -38;
        break;
    }
    if (r < 0 && r > -4096) {
        x[0] = darwin_errno(-r);
        mc->pstate |= PSTATE_C;
    } else {
        x[0] = r;
        x[1] = 0;
        mc->pstate &= ~PSTATE_C;
    }
}

static void sigsys_handler(int sig, void *info, void *uc) {
    (void)sig;
    /* siginfo: si_signo, si_errno, si_code, pad, then _sigsys {call_addr, syscall, arch} */
    struct sigcontext_a64 *mc = UC_MCONTEXT(uc);
    (void)info;
    darwin_syscall(mc);
}

void sigsys_install(void) {
    struct k_sigaction sa = { (void *)sigsys_handler, SA_SIGINFO | SA_RESTORER, (void *)ioswine_sigreturn, 0 };
    long r = sys4(SYS_rt_sigaction, SIGSYS_NO, &sa, 0, 8);
    if (r < 0) die("rt_sigaction(SIGSYS): %ld", r);

    struct sock_filter f[3 + 6 * SIGSYS_MAX_RANGES + 1];
    int n = 0;
    f[n++] = STMT(BPF_LD_W_ABS, 4);
    f[n++] = JUMP(BPF_JEQ_K, AUDIT_ARCH_AARCH64, 1, 0);
    f[n++] = STMT(BPF_RET_K, SECCOMP_RET_KILL_PROCESS);
    for (int i = 0; i < g_nallow; i++) {
        /* if (ip_hi == H && ip_lo >= L && ip_lo < Hlo) ALLOW; else fall through to next block */
        f[n++] = STMT(BPF_LD_W_ABS, 12);
        f[n++] = JUMP(BPF_JEQ_K, (uint32_t)(g_allow[i].lo >> 32), 0, 4);
        f[n++] = STMT(BPF_LD_W_ABS, 8);
        f[n++] = JUMP(BPF_JGE_K, (uint32_t)g_allow[i].lo, 0, 2);
        /* hi ending exactly on a 4 GiB boundary: low word 0, compare against 0xffffffff instead */
        f[n++] = JUMP(BPF_JGE_K, (uint32_t)g_allow[i].hi ? (uint32_t)g_allow[i].hi : 0xffffffffu, 1, 0);
        f[n++] = STMT(BPF_RET_K, SECCOMP_RET_ALLOW);
    }
    f[n++] = STMT(BPF_RET_K, SECCOMP_RET_TRAP);
    struct sock_fprog prog = { (uint16_t)n, f };

    if ((r = sys5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) < 0) die("PR_SET_NO_NEW_PRIVS: %ld", r);
    if ((r = sys3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) < 0) {
        if (r == -38) { LOG("seccomp: unsupported (qemu-user?), stray Darwin svc will not be trapped\n"); return; }
        die("seccomp: %ld", r);
    }
    LOG("seccomp trap installed, %d allow ranges, %d insns\n", g_nallow, n);
}
