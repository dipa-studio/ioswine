/*
 * Host shim for the aarch64-only linux.h in
 * libsystem_kernel/src/linux.h (ioswine-src-1112b reference tree).
 *
 * ipc.c includes "linux.h" and uses sys0/sys3/sys4 (getpid, futex),
 * sys_write, sys_mmap and sys_munmap with aarch64 syscall numbers and
 * inline svc asm, which do not compile on the x86_64 host. This shim
 * provides the same names and the same aarch64 syscall numbers, but
 * implements them with libc: getpid via getpid(), write via write(),
 * futex via the x86_64 futex syscall (202), mmap/munmap via the
 * x86_64 mmap (9) / munmap (11) syscalls. The FUTEX op numbers
 * and the PROT and MAP constants are the same on both arches.
 *
 * This file is host-only build scaffolding. It must come FIRST on
 * the -I path so it shadows the target linux.h; see build.sh. The
 * target linux.h is never on the include path for the harness.
 * Pattern follows tests/host/host-shim/linux.h, extended
 * with mmap/munmap which ipc.c needs for its large-message fallback.
 *
 * No em dashes are used in this file.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <unistd.h> /* getpid, write */
/* syscall() is declared in unistd.h only under _GNU_SOURCE, which
 * the including .c files may not define (their own headers come
 * first), so declare it directly. */
extern long syscall(long number, ...);

#undef SYS_getpid
#undef SYS_futex
#undef SYS_write
#undef SYS_mmap
#undef SYS_munmap
#define SYS_getpid 172
#define SYS_futex 98
#define SYS_write 64
#define SYS_mmap 222
#define SYS_munmap 215

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

static inline long harness_sys6(long n, long a, long b, long c, long d,
                                long e, long f) {
    if (n == 172) return (long)getpid();
    if (n == 64) return (long)write((int)a, (const void *)b, (size_t)c);
    if (n == 98) {
        return syscall(202, (void *)a, (int)b, (int)c, (void *)d,
                       (void *)e, (int)f);
    }
    if (n == 222) {
        return syscall(9, (void *)a, (size_t)b, (int)c, (int)d,
                       (int)e, (long)f);
    }
    if (n == 215) {
        return syscall(11, (void *)a, (size_t)b);
    }
    return -1;
}

#define sys6(n, a, b, c, d, e, f) \
    harness_sys6((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), \
                 (long)(e), (long)(f))
#define sys0(n) sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a) sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b) sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d) \
    sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sys5(n, a, b, c, d, e) \
    sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

static inline long sys_write(int fd, const void *buf, size_t count) {
    return (long)write(fd, buf, count);
}

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}

static inline long sys_munmap(void *addr, size_t len) {
    return sys2(SYS_munmap, (long)addr, (long)len);
}
