/*
 * Host shim for the aarch64-only linux.h in
 * libsystem_kernel/src/linux.h.
 *
 * svchost.c and fakeboard.c include "linux.h" and use sys0/sys3/sys4
 * (getpid, futex, write) with aarch64 syscall numbers and inline
 * svc asm, which do not compile on the x86_64 host. This shim
 * provides the same names and the same aarch64 syscall numbers, but
 * implements them with libc: getpid via getpid(), write via write(),
 * futex via the x86_64 futex syscall (202; the FUTEX_* op numbers
 * are the same on both arches). The test is single-threaded, so the
 * futex wait path is never taken; the wake is a real syscall.
 *
 * This file is build scaffolding only. It must come FIRST on the -I path
 * so it shadows the target linux.h; see the build commands in sim_test.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <unistd.h> /* getpid, write */
#include <errno.h> /* errno, for the mmap error translation below */
/* syscall() is declared in unistd.h only under _GNU_SOURCE, which
 * the including .c files may not define (their own headers come
 * first), so declare it directly. */
extern long syscall(long number, ...);

#undef SYS_getpid
#undef SYS_futex
#undef SYS_write
#define SYS_getpid 172
#define SYS_futex 98
#define SYS_write 64

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

static inline long sim_sys6(long n, long a, long b, long c, long d,
                            long e, long f) {
    if (n == 172) return (long)getpid();
    if (n == 64) return (long)write((int)a, (const void *)b, (size_t)c);
    if (n == 98) {
        return syscall(202, (void *)a, (int)b, (int)c, (void *)d,
                       (void *)e, (int)f);
    }
    return -1;
}

#define sys6(n, a, b, c, d, e, f) \
    sim_sys6((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), \
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

/* ------------------------------------------------------------------ */
/* Syscall extension (kevent_pset_test): kevent.c and ipc.c need
 * more syscalls than the original shim provided. Same approach:
 * aarch64 numbers in, x86_64 syscall() numbers out. Argument order
 * is the same on both arches for every call below.
 *   aarch64 -> x86_64: mmap 222->9, read 63->0, close 57->3,
 *   clock_gettime 113->228, epoll_create1 20->291, eventfd2 19->290,
 *   epoll_ctl 21->233, epoll_pwait 22->281.
 * PROT and MAP constants match the target linux.h values. kevent.c defines its
 * own SYS_eventfd2 and SYS_epoll numbers, so the defines here are
 * ifndef-guarded to avoid redefinition warnings.
 */
#ifndef SYS_mmap
#define SYS_mmap 222
#endif
#ifndef SYS_munmap
#define SYS_munmap 215
#endif
#ifndef SYS_read
#define SYS_read 63
#endif
#ifndef SYS_close
#define SYS_close 57
#endif
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 113
#endif
#ifndef SYS_epoll_create1
#define SYS_epoll_create1 20
#endif
#ifndef SYS_eventfd2
#define SYS_eventfd2 19
#endif
#ifndef SYS_epoll_ctl
#define SYS_epoll_ctl 21
#endif
#ifndef SYS_epoll_pwait
#define SYS_epoll_pwait 22
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

#ifndef SYS_mprotect
#define SYS_mprotect 226
#endif
#ifndef SYS_msync
#define SYS_msync 227
#endif
#ifndef SYS_mincore
#define SYS_mincore 232
#endif
#ifndef SYS_madvise
#define SYS_madvise 233
#endif
#ifndef SYS_dup
#define SYS_dup 23
#endif
#ifndef SYS_ftruncate
#define SYS_ftruncate 46
#endif
#ifndef SYS_openat
#define SYS_openat 56
#endif
#ifndef SYS_pwrite64
#define SYS_pwrite64 68
#endif
#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef O_RDONLY
#define O_RDONLY 00
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif

static inline long sys_mprotect(void *addr, size_t len, int prot) {
    return syscall(10, addr, len, prot);
}

/* The original shim's sim_sys6 is defined above; extend it here by
 * wrapping: host_sys6 handles the extra numbers, and anything else
 * falls through to sim_sys6. This keeps the original function
 * untouched. */
static inline long host_sys6(long n, long a, long b, long c, long d,
                             long e, long f) {
    /* mmap must return the raw kernel value (-errno on failure),
     * not glibc's -1: vm.c distinguishes EEXIST (-17) from other
     * errors to decide between the MAP_FIXED_NOREPLACE retry and the
     * OVERWRITE fallback. */
    if (n == 222) {
        long r = syscall(9, (void *)a, (size_t)b, (int)c, (int)d, (int)e, (long)f);
        if (r < 0) return -(long)errno;
        return r;
    }
    if (n == 215) return syscall(11, (void *)a, (size_t)b);
    if (n == 63) return (long)read((int)a, (void *)b, (size_t)c);
    if (n == 57) return (long)close((int)a);
    if (n == 113) return syscall(228, (int)a, (void *)b);
    if (n == 20) return syscall(291, (int)a);
    if (n == 19) return syscall(290, (unsigned int)a, (int)b);
    if (n == 21) return syscall(233, (int)a, (int)b, (int)c, (void *)d);
    if (n == 22) return syscall(281, (int)a, (void *)b, (int)c, (int)d, (void *)e, (int)f);
    if (n == 23) {
        long r = syscall(32, (int)a);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 46) {
        long r = syscall(77, (int)a, (long)b);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 56) {
        long r = syscall(257, (int)a, (const char *)b, (int)c, (int)d);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 68) {
        long r = syscall(18, (int)a, (const void *)b, (size_t)c, (long)d);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 279) {
        long r = syscall(319, (const char *)a, (unsigned int)b);
        return r < 0 ? -(long)errno : r;
    }
    /* vm.c's msync / mincore / madvise (and the
     * vm_copy range validator built on msync) need real syscalls,
     * not the sim_sys6 -1 fallback. aarch64 -> x86_64: msync
     * 227->26, mincore 232->21, madvise 233->28. Raw kernel style
     * (-errno on failure) like mmap above, so vm.c's
     * linux_to_darwin_errno translates the real errno. */
    if (n == 227) {
        long r = syscall(26, (void *)a, (size_t)b, (int)c);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 232) {
        long r = syscall(21, (void *)a, (size_t)b, (void *)c);
        return r < 0 ? -(long)errno : r;
    }
    if (n == 233) {
        long r = syscall(28, (void *)a, (size_t)b, (int)c);
        return r < 0 ? -(long)errno : r;
    }
    return sim_sys6(n, a, b, c, d, e, f);
}
#undef sys6
#define sys6(n, a, b, c, d, e, f) \
    host_sys6((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), \
              (long)(e), (long)(f))

static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}

static inline long sys_munmap(void *addr, size_t len) {
    return sys2(SYS_munmap, (long)addr, (long)len);
}
