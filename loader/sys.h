/* Raw Linux arm64 syscalls for the freestanding loader. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SYS_openat        56
#define SYS_close         57
#define SYS_lseek         62
#define SYS_read          63
#define SYS_write         64
#define SYS_pread64       67
#define SYS_fstat         80
#define SYS_exit          93
#define SYS_exit_group    94
#define SYS_kill         129
#define SYS_rt_sigaction 134
#define SYS_rt_sigprocmask 135
#define SYS_getpid       172
#define SYS_gettid       178
#define SYS_munmap       215
#define SYS_mmap         222
#define SYS_mprotect     226
#define SYS_prctl        167
#define SYS_seccomp      277

#define AT_FDCWD   (-100)
#define O_RDONLY   0
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED_P(p) ((unsigned long)(p) >= (unsigned long)-4095)

static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
#define sys0(n)                sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a)             sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)          sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)       sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)    sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sys5(n, a, b, c, d, e) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
