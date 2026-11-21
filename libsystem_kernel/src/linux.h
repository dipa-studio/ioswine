/* Raw Linux arm64 syscall inlines and constants for freestanding libsystem_kernel. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SYS_io_setup             0
#define SYS_setxattr             5
#define SYS_dup                 23
#define SYS_fcntl               25
#define SYS_ioctl               29
#define SYS_mkdirat             34
#define SYS_unlinkat            35
#define SYS_renameat            38
#define SYS_statfs              43
#define SYS_fstatfs             44
#define SYS_ftruncate           46
#define SYS_faccessat           48
#define SYS_fchmod              52
#define SYS_fchmodat            53
#define SYS_fchownat            54
#define SYS_fchown              55
#define SYS_openat              56
#define SYS_close               57
#define SYS_pipe2               59
#define SYS_getdents64          61
#define SYS_lseek               62
#define SYS_read                63
#define SYS_write               64
#define SYS_readv               65
#define SYS_writev              66
#define SYS_pread64             67
#define SYS_pwrite64            68
#define SYS_pselect6            72
#define SYS_ppoll               73
#define SYS_readlinkat          78
#define SYS_newfstatat          79
#define SYS_fstat               80
#define SYS_fsync               82
#define SYS_fdatasync           83
#define SYS_exit                93
#define SYS_exit_group          94
#define SYS_futex               98
#define SYS_set_robust_list     99
#define SYS_get_robust_list    100
#define SYS_nanosleep          101
#define SYS_clock_gettime      113
#define SYS_sched_yield        124
#define SYS_kill               129
#define SYS_rt_sigaction       134
#define SYS_rt_sigprocmask     135
#define SYS_rt_sigreturn       139
#define SYS_prctl              167
#define SYS_gettimeofday       169
#define SYS_getpid             172
#define SYS_getppid            173
#define SYS_getuid             174
#define SYS_geteuid            175
#define SYS_getgid             176
#define SYS_getegid            177
#define SYS_gettid             178
#define SYS_sysinfo            179
#define SYS_socket             198
#define SYS_socketpair         199
#define SYS_bind               200
#define SYS_listen             201
#define SYS_accept             202
#define SYS_connect            203
#define SYS_getsockname        204
#define SYS_getpeername        205
#define SYS_sendto             206
#define SYS_recvfrom           207
#define SYS_setsockopt         208
#define SYS_getsockopt         209
#define SYS_shutdown           210
#define SYS_sendmsg            211
#define SYS_recvmsg            212
#define SYS_brk                214
#define SYS_munmap             215
#define SYS_mremap             216
#define SYS_clone              220
#define SYS_mmap               222
#define SYS_mprotect           226
#define SYS_msync              227
#define SYS_mincore            232
#define SYS_madvise            233
#define SYS_prlimit64          261
#define SYS_renameat2          276
#define SYS_getrandom          278
#define SYS_memfd_create       279

#define AT_FDCWD               (-100)
#define O_RDONLY               00
#define O_WRONLY               01
#define O_RDWR                 02

#define PROT_NONE              0x0
#define PROT_READ              0x1
#define PROT_WRITE             0x2
#define PROT_EXEC              0x4

#define MAP_SHARED             0x01
#define MAP_PRIVATE            0x02
#define MAP_FIXED              0x10
#define MAP_ANONYMOUS          0x20
#define MAP_FIXED_NOREPLACE    0x100000

#define FUTEX_WAIT             0
#define FUTEX_WAKE             1
#define FUTEX_PRIVATE_FLAG     128
#define FUTEX_WAIT_PRIVATE     (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE     (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

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

static inline long sys_write(int fd, const void *buf, size_t count) {
    return sys3(SYS_write, fd, buf, count);
}

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}

static inline long sys_munmap(void *addr, size_t len) {
    return sys2(SYS_munmap, (long)addr, (long)len);
}

static inline long sys_mprotect(void *addr, size_t len, int prot) {
    return sys3(SYS_mprotect, (long)addr, (long)len, prot);
}

static inline void __attribute__((noreturn)) sys_exit_group(int status) {
    sys1(SYS_exit_group, status);
    while (1) {}
}
