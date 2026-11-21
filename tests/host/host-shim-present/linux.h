/*
 * Host shim for the aarch64-only linux.h in
 * libsystem_kernel/src/linux.h.
 *
 * present.c uses raw syscalls through sys0/sys1/.../sys6 with
 * aarch64 syscall numbers and inline svc asm, which do not compile
 * on the x86_64 host. This shim provides the same names and the
 * same aarch64 syscall numbers, but implements sys6 by translating
 * each arm64 number to the x86_64 number and calling libc
 * syscall(). The argument order of every syscall used here is the
 * same on both arches, and the structures present.c passes
 * (sockaddr_un, msghdr, pollfd, timespec) have the same 64-bit
 * layout on both, so the translation is a plain number map.
 * memfd_create is 319 on both arches and passes through unchanged.
 *
 * This file is build scaffolding only. It must come FIRST on the -I path so it
 * shadows the target linux.h; see the build commands in
 * present_test.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* syscall() is declared in unistd.h only under _GNU_SOURCE, which
 * the including .c file may not define, so declare it directly. */
extern long syscall(long number, ...);

/* aarch64 syscall numbers used by present.c. */
#define SYS_socket        198
#define SYS_connect       203
#define SYS_sendmsg       211
#define SYS_recvmsg       212
#define SYS_shutdown      210
#define SYS_close         57
#define SYS_ftruncate     46
#define SYS_mmap          222
#define SYS_munmap        215
#define SYS_clock_gettime 113
#define SYS_ppoll         73
#define SYS_write         64

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01

static inline long shim_sys6(long n, long a, long b, long c, long d,
                             long e, long f) {
    long host;
    switch (n) {
        case 198: host = 41; break;   /* socket */
        case 203: host = 42; break;   /* connect */
        case 211: host = 46; break;   /* sendmsg */
        case 212: host = 47; break;   /* recvmsg */
        case 210: host = 48; break;   /* shutdown */
        case 57:  host = 3; break;    /* close */
        case 46:  host = 77; break;   /* ftruncate */
        case 222: host = 9; break;    /* mmap */
        case 215: host = 11; break;   /* munmap */
        case 113: host = 228; break;  /* clock_gettime */
        case 73:  host = 271; break;  /* ppoll */
        case 64:  host = 1; break;    /* write */
        case 319: host = 319; break;  /* memfd_create (same number) */
        default: return -38;          /* -ENOSYS */
    }
    return syscall(host, a, b, c, d, e, f);
}

#define sys6(n, a, b, c, d, e, f) \
    shim_sys6((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), \
              (long)(e), (long)(f))
#define sys0(n) sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a) sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b) sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d) \
    sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sys5(n, a, b, c, d, e) \
    sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

static inline long sys_munmap(void *addr, size_t len) {
    return sys2(SYS_munmap, (long)addr, (long)len);
}

static inline long sys_write(int fd, const void *buf, size_t count) {
    return sys3(SYS_write, fd, buf, count);
}

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags,
                             int fd, long off) {
    return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd,
                        off);
}
