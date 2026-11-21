#include "linux.h"
#include "errno.h"
#include "pathmap.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef int64_t ssize_t;

#ifndef SYS_flock
#define SYS_flock 32
#endif
#ifndef SYS_truncate
#define SYS_truncate 45
#endif
#ifndef SYS_fallocate
#define SYS_fallocate 47
#endif
#ifndef SYS_sync
#define SYS_sync 81
#endif

struct darwin_flock {
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int16_t l_type;
    int16_t l_whence;
};

struct linux_flock {
    short int l_type;
    short int l_whence;
    int32_t   _pad;
    int64_t   l_start;
    int64_t   l_len;
    int32_t   l_pid;
    int32_t   _pad2;
};

/* String comparison helper for freestanding C */
static int mystrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static size_t mystrlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

/* ---------- kqueue / kevent ---------- */

int lsk_kqueue(void) __asm__("_kqueue");
int lsk_kqueue(void) {
    long r = sys1(20 /* SYS_epoll_create1 */, 0x80000);
    if (r < 0) r = sys1(20, 0);
    if (r < 0) {
        set_errno(-(int)r);
        return -1;
    }
    return (int)r;
}

int lsk_guarded_kqueue_np(const void *guard, unsigned int guardflags) __asm__("_guarded_kqueue_np");
int lsk_guarded_kqueue_np(const void *guard, unsigned int guardflags) {
    (void)guard; (void)guardflags;
    return lsk_kqueue();
}

int lsk_kqueue_workloop_ctl(void *arg1, void *arg2, void *arg3) __asm__("___kqueue_workloop_ctl");
int lsk_kqueue_workloop_ctl(void *arg1, void *arg2, void *arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    return 0;
}

int lsk_sigsuspend_nocancel(const void *sigmask) __asm__("_sigsuspend$NOCANCEL");
int lsk_sigsuspend_nocancel(const void *sigmask) {
    long ret = sys2(130 /* SYS_rt_sigsuspend */, (long)sigmask, 8);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_sigsuspend_nocancel2(const void *sigmask) __asm__("___sigsuspend_nocancel");
int lsk_sigsuspend_nocancel2(const void *sigmask) {
    return lsk_sigsuspend_nocancel(sigmask);
}

int lsk_sigsuspend(const void *sigmask) __asm__("_sigsuspend");
int lsk_sigsuspend(const void *sigmask) {
    return lsk_sigsuspend_nocancel(sigmask);
}

int lsk_sigsuspend2(const void *sigmask) __asm__("___sigsuspend");
int lsk_sigsuspend2(const void *sigmask) {
    return lsk_sigsuspend_nocancel(sigmask);
}

int lsk_kevent(int kq, const void *changelist, int nchanges,
               void *eventlist, int nevents, const void *timeout) __asm__("_kevent");
int lsk_kevent(int kq, const void *changelist, int nchanges,
               void *eventlist, int nevents, const void *timeout) {
    (void)kq; (void)changelist; (void)nchanges; (void)eventlist; (void)nevents; (void)timeout;
    return 0;
}

int lsk_kevent64(int kq, const void *changelist, int nchanges,
                 void *eventlist, int nevents, unsigned int flags,
                 const void *timeout) __asm__("_kevent64");
int lsk_kevent64(int kq, const void *changelist, int nchanges,
                 void *eventlist, int nevents, unsigned int flags,
                 const void *timeout) {
    (void)kq; (void)changelist; (void)nchanges; (void)eventlist; (void)nevents;
    (void)flags; (void)timeout;
    return 0;
}

/* ---------- sysctl ---------- */

static int copy_sysctl_val(void *oldp, size_t *oldlenp, const void *src, size_t src_len) {
    if (oldp && oldlenp) {
        size_t to_copy = *oldlenp < src_len ? *oldlenp : src_len;
        for (size_t i = 0; i < to_copy; i++) {
            ((char *)oldp)[i] = ((const char *)src)[i];
        }
        *oldlenp = src_len;
    } else if (oldlenp) {
        *oldlenp = src_len;
    }
    return 0;
}

int lsk_sysctlbyname(const char *name, size_t namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) __asm__("___sysctlbyname");
int lsk_sysctlbyname(const char *name, size_t namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)namelen;
    (void)newp;
    (void)newlen;
    if (!name) {
        set_errno(22); /* EINVAL */
        return -1;
    }

    if (mystrcmp(name, "hw.ncpu") == 0 ||
        mystrcmp(name, "hw.activecpu") == 0 ||
        mystrcmp(name, "hw.physicalcpu") == 0 ||
        mystrcmp(name, "hw.physicalcpu_max") == 0 ||
        mystrcmp(name, "hw.logicalcpu") == 0 ||
        mystrcmp(name, "hw.logicalcpu_max") == 0) {
        int ncpu = 8;
        return copy_sysctl_val(oldp, oldlenp, &ncpu, sizeof(ncpu));
    }
    if (mystrcmp(name, "hw.pagesize") == 0) {
        int pagesize = 16384;
        return copy_sysctl_val(oldp, oldlenp, &pagesize, sizeof(pagesize));
    }
    if (mystrcmp(name, "hw.memsize") == 0) {
        uint64_t mem = 8ULL * 1024 * 1024 * 1024;
        return copy_sysctl_val(oldp, oldlenp, &mem, sizeof(mem));
    }
    if (mystrcmp(name, "hw.machine") == 0 || mystrcmp(name, "hw.model") == 0) {
        static const char mach[] = "iPhone17,3";
        return copy_sysctl_val(oldp, oldlenp, mach, sizeof(mach));
    }
    if (mystrcmp(name, "hw.targettype") == 0) {
        static const char tt[] = "D47";
        return copy_sysctl_val(oldp, oldlenp, tt, sizeof(tt));
    }
    if (mystrcmp(name, "hw.cputype") == 0) {
        int cputype = 0x0100000c; /* CPU_TYPE_ARM64 */
        return copy_sysctl_val(oldp, oldlenp, &cputype, sizeof(cputype));
    }
    if (mystrcmp(name, "hw.cpusubtype") == 0) {
        int subtype = 2; /* CPU_SUBTYPE_ARM64E */
        return copy_sysctl_val(oldp, oldlenp, &subtype, sizeof(subtype));
    }
    if (mystrcmp(name, "hw.byteorder") == 0) {
        int bo = 1234;
        return copy_sysctl_val(oldp, oldlenp, &bo, sizeof(bo));
    }
    if (mystrcmp(name, "kern.osrelease") == 0) {
        static const char rel[] = "23.6.0";
        return copy_sysctl_val(oldp, oldlenp, rel, sizeof(rel));
    }
    if (mystrcmp(name, "kern.osversion") == 0) {
        static const char ver[] = "23G90";
        return copy_sysctl_val(oldp, oldlenp, ver, sizeof(ver));
    }
    if (mystrcmp(name, "kern.version") == 0) {
        static const char kver[] = "Darwin Kernel Version 23.6.0";
        return copy_sysctl_val(oldp, oldlenp, kver, sizeof(kver));
    }
    if (mystrcmp(name, "kern.hostname") == 0) {
        static const char host[] = "localhost";
        return copy_sysctl_val(oldp, oldlenp, host, sizeof(host));
    }
    if (mystrcmp(name, "kern.saved_ids") == 0) {
        int saved = 1;
        return copy_sysctl_val(oldp, oldlenp, &saved, sizeof(saved));
    }
    if (mystrcmp(name, "kern.usrstack64") == 0) {
        uint64_t stk = 0x7000000000ULL;
        return copy_sysctl_val(oldp, oldlenp, &stk, sizeof(stk));
    }
    if (mystrcmp(name, "kern.wq_limit_cooperative_threads") == 0) {
        int limit = 16;
        return copy_sysctl_val(oldp, oldlenp, &limit, sizeof(limit));
    }
    if (mystrcmp(name, "hw.cachelinesize") == 0) {
        int cls = 64;
        return copy_sysctl_val(oldp, oldlenp, &cls, sizeof(cls));
    }
    if (mystrcmp(name, "hw.l2cachesize") == 0) {
        int l2 = 4 * 1024 * 1024;
        return copy_sysctl_val(oldp, oldlenp, &l2, sizeof(l2));
    }
    if (mystrcmp(name, "hw.optional.FEAT_FP16") == 0) {
        int fp16 = 1;
        return copy_sysctl_val(oldp, oldlenp, &fp16, sizeof(fp16));
    }
    if (mystrcmp(name, "hw.perflevel0.physicalcpu_max") == 0) {
        int cpu = 4;
        return copy_sysctl_val(oldp, oldlenp, &cpu, sizeof(cpu));
    }
    if (mystrcmp(name, "kern.memorystatus_vm_pressure_level") == 0) {
        int press = 0;
        return copy_sysctl_val(oldp, oldlenp, &press, sizeof(press));
    }
    if (mystrcmp(name, "kern.bootargs") == 0) {
        static const char args[] = "";
        return copy_sysctl_val(oldp, oldlenp, args, sizeof(args));
    }

    set_errno(2); /* ENOENT */
    return -1;
}

static char g_sysctl_names[64][64];
static int g_num_sysctl_names = 0;

static int sysctl_name_to_oid(const char *name) {
    for (int i = 0; i < g_num_sysctl_names; i++) {
        if (mystrcmp(g_sysctl_names[i], name) == 0) return i + 1;
    }
    if (g_num_sysctl_names < 64) {
        int idx = g_num_sysctl_names++;
        size_t l = 0;
        while (name[l] && l < 63) {
            g_sysctl_names[idx][l] = name[l];
            l++;
        }
        g_sysctl_names[idx][l] = '\0';
        return idx + 1;
    }
    return -1;
}

int lsk_sysctl(int *name, uint32_t namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) __asm__("___sysctl");
int lsk_sysctl(int *name, uint32_t namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!name || namelen < 2) {
        set_errno(22); /* EINVAL */
        return -1;
    }

    int top = name[0];
    int sub = name[1];

    if (top == 0 /* CTL_SYSCTL */ && sub == 3 /* CTL_SYSCTL_NAME2OID */) {
        if (!newp || newlen == 0) {
            set_errno(22); /* EINVAL */
            return -1;
        }
        char sname[128];
        size_t slen = newlen < sizeof(sname) - 1 ? newlen : sizeof(sname) - 1;
        for (size_t i = 0; i < slen; i++) sname[i] = ((const char *)newp)[i];
        sname[slen] = '\0';
        while (slen > 0 && sname[slen - 1] == '\0') {
            slen--;
            sname[slen] = '\0';
        }

        size_t test_len = 0;
        int check = lsk_sysctlbyname(sname, slen, NULL, &test_len, NULL, 0);
        if (check != 0) {
            set_errno(2); /* ENOENT */
            return -1;
        }

        int id = sysctl_name_to_oid(sname);
        if (id < 0) {
            set_errno(2); /* ENOENT */
            return -1;
        }
        int oid[2] = { 1000, id };
        return copy_sysctl_val(oldp, oldlenp, oid, sizeof(oid));
    }

    if (top == 1000) {
        int id = sub - 1;
        if (id >= 0 && id < g_num_sysctl_names) {
            return lsk_sysctlbyname(g_sysctl_names[id], 0, oldp, oldlenp, newp, newlen);
        }
        set_errno(2); /* ENOENT */
        return -1;
    }

    if (top == 6 /* CTL_HW */) {
        if (sub == 3 /* HW_NCPU */ || sub == 25 /* HW_AVAILCPU */) {
            int ncpu = 8;
            return copy_sysctl_val(oldp, oldlenp, &ncpu, sizeof(ncpu));
        }
        if (sub == 7 /* HW_PAGESIZE */) {
            int pagesize = 16384;
            return copy_sysctl_val(oldp, oldlenp, &pagesize, sizeof(pagesize));
        }
        if (sub == 24 /* HW_MEMSIZE */) {
            uint64_t mem = 8ULL * 1024 * 1024 * 1024;
            return copy_sysctl_val(oldp, oldlenp, &mem, sizeof(mem));
        }
        if (sub == 4 /* HW_BYTEORDER */) {
            int bo = 1234;
            return copy_sysctl_val(oldp, oldlenp, &bo, sizeof(bo));
        }
        if (sub == 1 /* HW_MACHINE */ || sub == 2 /* HW_MODEL */) {
            static const char mach[] = "iPhone17,3";
            return copy_sysctl_val(oldp, oldlenp, mach, sizeof(mach));
        }
    } else if (top == 1 /* CTL_KERN */) {
        if (sub == 2 /* KERN_OSRELEASE */) {
            static const char rel[] = "23.6.0";
            return copy_sysctl_val(oldp, oldlenp, rel, sizeof(rel));
        }
        if (sub == 4 /* KERN_VERSION */) {
            static const char kver[] = "Darwin Kernel Version 23.6.0";
            return copy_sysctl_val(oldp, oldlenp, kver, sizeof(kver));
        }
        if (sub == 10 /* KERN_HOSTNAME */) {
            static const char host[] = "localhost";
            return copy_sysctl_val(oldp, oldlenp, host, sizeof(host));
        }
        if (sub == 65 /* KERN_OSVERSION */) {
            static const char ver[] = "23G90";
            return copy_sysctl_val(oldp, oldlenp, ver, sizeof(ver));
        }
    }

    set_errno(2); /* ENOENT */
    return -1;
}

/* ---------- I/O (write, read, open, close, etc.) ---------- */

ssize_t lsk_write(int fd, const void *buf, size_t count) __asm__("_write");
ssize_t lsk_write(int fd, const void *buf, size_t count) {
    long ret = sys_write(fd, buf, count);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_write_nocancel(int fd, const void *buf, size_t count) __asm__("_write$NOCANCEL");
ssize_t lsk_write_nocancel(int fd, const void *buf, size_t count) {
    return lsk_write(fd, buf, count);
}
ssize_t lsk_write_nocancel_m(int fd, const void *buf, size_t count) __asm__("___write_nocancel");
ssize_t lsk_write_nocancel_m(int fd, const void *buf, size_t count) {
    return lsk_write(fd, buf, count);
}

ssize_t lsk_writev(int fd, const void *iov, int iovcnt) __asm__("_writev");
ssize_t lsk_writev(int fd, const void *iov, int iovcnt) {
    long ret = sys3(SYS_writev, fd, (long)iov, iovcnt);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_writev_nocancel(int fd, const void *iov, int iovcnt) __asm__("_writev$NOCANCEL");
ssize_t lsk_writev_nocancel(int fd, const void *iov, int iovcnt) {
    return lsk_writev(fd, iov, iovcnt);
}
ssize_t lsk_writev_nocancel_m(int fd, const void *iov, int iovcnt) __asm__("___writev_nocancel");
ssize_t lsk_writev_nocancel_m(int fd, const void *iov, int iovcnt) {
    return lsk_writev(fd, iov, iovcnt);
}

ssize_t lsk_pwrite(int fd, const void *buf, size_t count, int64_t offset) __asm__("_pwrite");
ssize_t lsk_pwrite(int fd, const void *buf, size_t count, int64_t offset) {
    long ret = sys4(SYS_pwrite64, fd, (long)buf, count, offset);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_pwrite_nocancel(int fd, const void *buf, size_t count, int64_t offset) __asm__("_pwrite$NOCANCEL");
ssize_t lsk_pwrite_nocancel(int fd, const void *buf, size_t count, int64_t offset) {
    return lsk_pwrite(fd, buf, count, offset);
}
ssize_t lsk_pwrite_nocancel_m(int fd, const void *buf, size_t count, int64_t offset) __asm__("___pwrite_nocancel");
ssize_t lsk_pwrite_nocancel_m(int fd, const void *buf, size_t count, int64_t offset) {
    return lsk_pwrite(fd, buf, count, offset);
}

ssize_t lsk_read(int fd, void *buf, size_t count) __asm__("_read");
ssize_t lsk_read(int fd, void *buf, size_t count) {
    long ret = sys3(SYS_read, fd, (long)buf, count);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_read_nocancel(int fd, void *buf, size_t count) __asm__("_read$NOCANCEL");
ssize_t lsk_read_nocancel(int fd, void *buf, size_t count) {
    return lsk_read(fd, buf, count);
}
ssize_t lsk_read_nocancel_m(int fd, void *buf, size_t count) __asm__("___read_nocancel");
ssize_t lsk_read_nocancel_m(int fd, void *buf, size_t count) {
    return lsk_read(fd, buf, count);
}

ssize_t lsk_readv(int fd, const void *iov, int iovcnt) __asm__("_readv");
ssize_t lsk_readv(int fd, const void *iov, int iovcnt) {
    long ret = sys3(SYS_readv, fd, (long)iov, iovcnt);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_readv_nocancel(int fd, const void *iov, int iovcnt) __asm__("_readv$NOCANCEL");
ssize_t lsk_readv_nocancel(int fd, const void *iov, int iovcnt) {
    return lsk_readv(fd, iov, iovcnt);
}
ssize_t lsk_readv_nocancel_m(int fd, const void *iov, int iovcnt) __asm__("___readv_nocancel");
ssize_t lsk_readv_nocancel_m(int fd, const void *iov, int iovcnt) {
    return lsk_readv(fd, iov, iovcnt);
}

ssize_t lsk_pread(int fd, void *buf, size_t count, int64_t offset) __asm__("_pread");
ssize_t lsk_pread(int fd, void *buf, size_t count, int64_t offset) {
    long ret = sys4(SYS_pread64, fd, (long)buf, count, offset);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_pread_nocancel(int fd, void *buf, size_t count, int64_t offset) __asm__("_pread$NOCANCEL");
ssize_t lsk_pread_nocancel(int fd, void *buf, size_t count, int64_t offset) {
    return lsk_pread(fd, buf, count, offset);
}
ssize_t lsk_pread_nocancel_m(int fd, void *buf, size_t count, int64_t offset) __asm__("___pread_nocancel");
ssize_t lsk_pread_nocancel_m(int fd, void *buf, size_t count, int64_t offset) {
    return lsk_pread(fd, buf, count, offset);
}

#define DARWIN_AT_FDCWD            (-2)
#define DARWIN_AT_SYMLINK_NOFOLLOW 0x0020
#define DARWIN_AT_SYMLINK_FOLLOW   0x0040
#define DARWIN_AT_REMOVEDIR        0x0080

static inline int translate_at_fd(int fd) {
    return (fd == DARWIN_AT_FDCWD) ? AT_FDCWD : fd;
}

static inline int translate_at_flags(int darwin_flags) {
    int lflags = 0;
    if (darwin_flags & DARWIN_AT_SYMLINK_NOFOLLOW)
        lflags |= 0x100; /* Linux AT_SYMLINK_NOFOLLOW */
    if (darwin_flags & DARWIN_AT_REMOVEDIR)
        lflags |= 0x200; /* Linux AT_REMOVEDIR */
    if (darwin_flags & DARWIN_AT_SYMLINK_FOLLOW)
        lflags |= 0x400; /* Linux AT_SYMLINK_FOLLOW */
    return lflags;
}

static int darwin_open_flags_to_linux(int flags) {
    int lflags = 0;
    if ((flags & 3) == 0) lflags |= 00;      /* O_RDONLY */
    else if ((flags & 3) == 1) lflags |= 01; /* O_WRONLY */
    else if ((flags & 3) == 2) lflags |= 02; /* O_RDWR */
    if (flags & 0x0004) lflags |= 04000;    /* O_NONBLOCK */
    if (flags & 0x0008) lflags |= 02000;    /* O_APPEND */
    if (flags & 0x0040) lflags |= 020000;   /* O_ASYNC */
    if (flags & 0x0080) lflags |= 04010000; /* O_FSYNC / O_SYNC */
    if (flags & 0x0100) lflags |= 0100000;  /* O_NOFOLLOW (arm64) */
    if (flags & 0x0200) lflags |= 0100;     /* O_CREAT */
    if (flags & 0x0400) lflags |= 01000;    /* O_TRUNC */
    if (flags & 0x0800) lflags |= 0200;     /* O_EXCL */
    if (flags & 0x20000) lflags |= 0400;    /* O_NOCTTY */
    if (flags & 0x100000) lflags |= 040000; /* O_DIRECTORY (arm64; 0200000 is O_DIRECT here) */
    if (flags & 0x1000000) lflags |= 02000000;/* O_CLOEXEC */
    return lflags;
}

static int internal_openat(int dirfd, const char *path, int flags, int mode) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    if ((flags & 0x0200 /* O_CREAT */) && mode == 0) {
        mode = 0666;
    }
    int lflags = darwin_open_flags_to_linux(flags);
    long ret = sys4(SYS_openat, dirfd, (long)path, lflags, mode);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (flags & 0x0010 /* O_SHLOCK */) {
        sys2(SYS_flock, ret, 1 /* LOCK_SH */);
    } else if (flags & 0x0020 /* O_EXLOCK */) {
        sys2(SYS_flock, ret, 2 /* LOCK_EX */);
    }
    return (int)ret;
}

int lsk_open(const char *path, int flags, ...) __asm__("_open");
int lsk_open(const char *path, int flags, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    int mode = 0;
    if (flags & 0x0200 /* O_CREAT */) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x2 != 0) {
            mode = (int)captured_x2;
        }
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}
int lsk_open_m(const char *path, int flags, ...) __asm__("___open");
int lsk_open_m(const char *path, int flags, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    int mode = 0;
    if (flags & 0x0200 /* O_CREAT */) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x2 != 0) {
            mode = (int)captured_x2;
        }
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_open_nocancel(const char *path, int flags, ...) __asm__("_open$NOCANCEL");
int lsk_open_nocancel(const char *path, int flags, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x2 != 0) mode = (int)captured_x2;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}
int lsk_open_nocancel_m(const char *path, int flags, ...) __asm__("___open_nocancel");
int lsk_open_nocancel_m(const char *path, int flags, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x2 != 0) mode = (int)captured_x2;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_openat(int fd, const char *path, int flags, ...) __asm__("_openat");
int lsk_openat(int fd, const char *path, int flags, ...) {
    register long reg_x3 __asm__("x3");
    long captured_x3 = reg_x3;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x3 != 0) mode = (int)captured_x3;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}
int lsk_openat_m(int fd, const char *path, int flags, ...) __asm__("___openat");
int lsk_openat_m(int fd, const char *path, int flags, ...) {
    register long reg_x3 __asm__("x3");
    long captured_x3 = reg_x3;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x3 != 0) mode = (int)captured_x3;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}

int lsk_openat_nocancel(int fd, const char *path, int flags, ...) __asm__("_openat$NOCANCEL");
int lsk_openat_nocancel(int fd, const char *path, int flags, ...) {
    register long reg_x3 __asm__("x3");
    long captured_x3 = reg_x3;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x3 != 0) mode = (int)captured_x3;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}
int lsk_openat_nocancel_m(int fd, const char *path, int flags, ...) __asm__("___openat_nocancel");
int lsk_openat_nocancel_m(int fd, const char *path, int flags, ...) {
    register long reg_x3 __asm__("x3");
    long captured_x3 = reg_x3;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x3 != 0) mode = (int)captured_x3;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}

int lsk_open_dprotected_np(const char *path, int flags, int class, int dpflags, ...) __asm__("_open_dprotected_np");
int lsk_open_dprotected_np(const char *path, int flags, int class, int dpflags, ...) {
    (void)class; (void)dpflags;
    register long reg_x4 __asm__("x4");
    long captured_x4 = reg_x4;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x4 != 0) mode = (int)captured_x4;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}
int lsk_open_dprotected_np_m(const char *path, int flags, int class, int dpflags, ...) __asm__("___open_dprotected_np");
int lsk_open_dprotected_np_m(const char *path, int flags, int class, int dpflags, ...) {
    (void)class; (void)dpflags;
    register long reg_x4 __asm__("x4");
    long captured_x4 = reg_x4;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x4 != 0) mode = (int)captured_x4;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_openat_dprotected_np(int fd, const char *path, int flags, int class, int dpflags, ...) __asm__("_openat_dprotected_np");
int lsk_openat_dprotected_np(int fd, const char *path, int flags, int class, int dpflags, ...) {
    (void)class; (void)dpflags;
    register long reg_x5 __asm__("x5");
    long captured_x5 = reg_x5;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x5 != 0) mode = (int)captured_x5;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}
int lsk_openat_dprotected_np_m(int fd, const char *path, int flags, int class, int dpflags, ...) __asm__("___openat_dprotected_np");
int lsk_openat_dprotected_np_m(int fd, const char *path, int flags, int class, int dpflags, ...) {
    (void)class; (void)dpflags;
    register long reg_x5 __asm__("x5");
    long captured_x5 = reg_x5;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x5 != 0) mode = (int)captured_x5;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}

int lsk_guarded_open_np(const char *path, const void *guard, unsigned int guardflags, int flags, ...) __asm__("_guarded_open_np");
int lsk_guarded_open_np(const char *path, const void *guard, unsigned int guardflags, int flags, ...) {
    (void)guard; (void)guardflags;
    register long reg_x4 __asm__("x4");
    long captured_x4 = reg_x4;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x4 != 0) mode = (int)captured_x4;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}
int lsk_guarded_open_np_m(const char *path, const void *guard, unsigned int guardflags, int flags, ...) __asm__("___guarded_open_np");
int lsk_guarded_open_np_m(const char *path, const void *guard, unsigned int guardflags, int flags, ...) {
    (void)guard; (void)guardflags;
    register long reg_x4 __asm__("x4");
    long captured_x4 = reg_x4;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x4 != 0) mode = (int)captured_x4;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_guarded_open_dprotected_np(const char *path, const void *guard, unsigned int guardflags, int flags, int class, int dpflags, ...) __asm__("_guarded_open_dprotected_np");
int lsk_guarded_open_dprotected_np(const char *path, const void *guard, unsigned int guardflags, int flags, int class, int dpflags, ...) {
    (void)guard; (void)guardflags; (void)class; (void)dpflags;
    register long reg_x6 __asm__("x6");
    long captured_x6 = reg_x6;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x6 != 0) mode = (int)captured_x6;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}
int lsk_guarded_open_dprotected_np_m(const char *path, const void *guard, unsigned int guardflags, int flags, int class, int dpflags, ...) __asm__("___guarded_open_dprotected_np");
int lsk_guarded_open_dprotected_np_m(const char *path, const void *guard, unsigned int guardflags, int flags, int class, int dpflags, ...) {
    (void)guard; (void)guardflags; (void)class; (void)dpflags;
    register long reg_x6 __asm__("x6");
    long captured_x6 = reg_x6;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, dpflags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x6 != 0) mode = (int)captured_x6;
    }
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_open_extended(const char *path, int flags, int uid, int gid, int mode, void *xsecurity) __asm__("___open_extended");
int lsk_open_extended(const char *path, int flags, int uid, int gid, int mode, void *xsecurity) {
    (void)uid; (void)gid; (void)xsecurity;
    return internal_openat(AT_FDCWD, path, flags, mode);
}

int lsk_openat_authenticated_np(int fd, const char *path, int flags, ...) __asm__("_openat_authenticated_np");
int lsk_openat_authenticated_np(int fd, const char *path, int flags, ...) {
    register long reg_x3 __asm__("x3");
    long captured_x3 = reg_x3;
    int mode = 0;
    if (flags & 0x0200) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        if (mode == 0 && captured_x3 != 0) mode = (int)captured_x3;
    }
    return internal_openat(translate_at_fd(fd), path, flags, mode);
}

int lsk_close(int fd) __asm__("_close");
int lsk_close(int fd) {
    long ret = sys1(SYS_close, fd);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_close_nocancel(int fd) __asm__("_close$NOCANCEL");
int lsk_close_nocancel(int fd) {
    return lsk_close(fd);
}
int lsk_close_nocancel_m(int fd) __asm__("___close_nocancel");
int lsk_close_nocancel_m(int fd) {
    return lsk_close(fd);
}

/* ---------- unlink / rename / truncate / sync / chmod / mkdir ---------- */

int lsk_unlink(const char *path) __asm__("_unlink");
int lsk_unlink(const char *path) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys3(SYS_unlinkat, AT_FDCWD, (long)path, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_unlink_m(const char *path) __asm__("___unlink");
int lsk_unlink_m(const char *path) {
    return lsk_unlink(path);
}

int lsk_unlinkat(int fd, const char *path, int flag) __asm__("_unlinkat");
int lsk_unlinkat(int fd, const char *path, int flag) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys3(SYS_unlinkat, translate_at_fd(fd), (long)path, translate_at_flags(flag));
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_unlinkat_m(int fd, const char *path, int flag) __asm__("___unlinkat");
int lsk_unlinkat_m(int fd, const char *path, int flag) {
    return lsk_unlinkat(fd, path, flag);
}

int lsk_rename(const char *oldpath, const char *newpath) __asm__("_rename");
int lsk_rename(const char *oldpath, const char *newpath) {
    char rbuf1[LSK_PATH_MAX], rbuf2[LSK_PATH_MAX];
    oldpath = lsk_path_remap(oldpath, rbuf1, sizeof(rbuf1));
    newpath = lsk_path_remap(newpath, rbuf2, sizeof(rbuf2));
    long ret = sys4(SYS_renameat, AT_FDCWD, (long)oldpath, AT_FDCWD, (long)newpath);
    if (ret < 0) {
        /* Fallback to renameat2 */
        long ret2 = sys5(SYS_renameat2, AT_FDCWD, (long)oldpath, AT_FDCWD, (long)newpath, 0);
        if (ret2 < 0) {
            set_errno(linux_to_darwin_errno(ret2));
            return -1;
        }
        return 0;
    }
    return 0;
}
int lsk_rename_m(const char *oldpath, const char *newpath) __asm__("___rename");
int lsk_rename_m(const char *oldpath, const char *newpath) {
    return lsk_rename(oldpath, newpath);
}

int lsk_renameat(int oldfd, const char *oldpath, int newfd, const char *newpath) __asm__("_renameat");
int lsk_renameat(int oldfd, const char *oldpath, int newfd, const char *newpath) {
    char rbuf1[LSK_PATH_MAX], rbuf2[LSK_PATH_MAX];
    oldpath = lsk_path_remap(oldpath, rbuf1, sizeof(rbuf1));
    newpath = lsk_path_remap(newpath, rbuf2, sizeof(rbuf2));
    long ret = sys4(SYS_renameat, translate_at_fd(oldfd), (long)oldpath, translate_at_fd(newfd), (long)newpath);
    if (ret < 0) {
        long ret2 = sys5(SYS_renameat2, translate_at_fd(oldfd), (long)oldpath, translate_at_fd(newfd), (long)newpath, 0);
        if (ret2 < 0) {
            set_errno(linux_to_darwin_errno(ret2));
            return -1;
        }
        return 0;
    }
    return 0;
}
int lsk_renameat_m(int oldfd, const char *oldpath, int newfd, const char *newpath) __asm__("___renameat");
int lsk_renameat_m(int oldfd, const char *oldpath, int newfd, const char *newpath) {
    return lsk_renameat(oldfd, oldpath, newfd, newpath);
}

int lsk_renameatx_np(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned int flags) __asm__("_renameatx_np");
int lsk_renameatx_np(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned int flags) {
    char rbuf1[LSK_PATH_MAX], rbuf2[LSK_PATH_MAX];
    oldpath = lsk_path_remap(oldpath, rbuf1, sizeof(rbuf1));
    newpath = lsk_path_remap(newpath, rbuf2, sizeof(rbuf2));
    unsigned int lflags = 0;
    if (flags & 2 /* RENAME_SWAP */) lflags |= 2 /* RENAME_EXCHANGE */;
    if (flags & 4 /* RENAME_EXCL */) lflags |= 1 /* RENAME_NOREPLACE */;
    long ret = sys5(SYS_renameat2, translate_at_fd(oldfd), (long)oldpath, translate_at_fd(newfd), (long)newpath, lflags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_renameatx_np_m(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned int flags) __asm__("___renameatx_np");
int lsk_renameatx_np_m(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned int flags) {
    return lsk_renameatx_np(oldfd, oldpath, newfd, newpath, flags);
}

int lsk_ftruncate(int fd, int64_t length) __asm__("_ftruncate");
int lsk_ftruncate(int fd, int64_t length) {
    long ret = sys2(SYS_ftruncate, fd, length);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_fsync(int fd) __asm__("_fsync");
int lsk_fsync(int fd) {
    long ret = sys1(SYS_fsync, fd);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_fsync_nocancel(int fd) __asm__("_fsync$NOCANCEL");
int lsk_fsync_nocancel(int fd) {
    return lsk_fsync(fd);
}
int lsk_fsync_nocancel_m(int fd) __asm__("___fsync_nocancel");
int lsk_fsync_nocancel_m(int fd) {
    return lsk_fsync(fd);
}

int lsk_truncate(const char *path, int64_t length) __asm__("_truncate");
int lsk_truncate(const char *path, int64_t length) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys2(SYS_truncate, (long)path, length);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_fdatasync(int fd) __asm__("_fdatasync");
int lsk_fdatasync(int fd) {
    long ret = sys1(SYS_fdatasync, fd);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

void lsk_sync(void) __asm__("_sync");
void lsk_sync(void) {
    sys0(SYS_sync);
}

int lsk_flock(int fd, int operation) __asm__("_flock");
int lsk_flock(int fd, int operation) {
    long ret = sys2(SYS_flock, fd, operation);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_fsctl(const char *path, unsigned long cmd, void *data, unsigned int options) __asm__("_fsctl");
int lsk_fsctl(const char *path, unsigned long cmd, void *data, unsigned int options) {
    (void)path; (void)cmd; (void)data; (void)options;
    set_errno(45 /* ENOTSUP */);
    return -1;
}

int lsk_ffsctl(int fd, unsigned long cmd, void *data, unsigned int options) __asm__("_ffsctl");
int lsk_ffsctl(int fd, unsigned long cmd, void *data, unsigned int options) {
    (void)fd; (void)cmd; (void)data; (void)options;
    set_errno(45 /* ENOTSUP */);
    return -1;
}

int lsk_fchmod(int fd, int mode) __asm__("_fchmod");
int lsk_fchmod(int fd, int mode) {
    long ret = sys2(SYS_fchmod, fd, mode);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_fchmod_m(int fd, int mode) __asm__("___fchmod");
int lsk_fchmod_m(int fd, int mode) {
    return lsk_fchmod(fd, mode);
}

int lsk_fchmodat(int fd, const char *path, int mode, int flag) __asm__("_fchmodat");
int lsk_fchmodat(int fd, const char *path, int mode, int flag) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys4(SYS_fchmodat, translate_at_fd(fd), (long)path, mode, translate_at_flags(flag));
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_mkdir(const char *path, int mode) __asm__("_mkdir");
int lsk_mkdir(const char *path, int mode) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys3(SYS_mkdirat, AT_FDCWD, (long)path, mode);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_mkdirat(int fd, const char *path, int mode) __asm__("_mkdirat");
int lsk_mkdirat(int fd, const char *path, int mode) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys3(SYS_mkdirat, translate_at_fd(fd), (long)path, mode);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_chdir(const char *path) __asm__("_chdir");
int lsk_chdir(const char *path) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys1(49 /* SYS_chdir */, (long)path);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_fchdir(int fd) __asm__("_fchdir");
int lsk_fchdir(int fd) {
    long ret = sys1(50 /* SYS_fchdir */, fd);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int64_t lsk_lseek(int fd, int64_t offset, int whence) __asm__("_lseek");
int64_t lsk_lseek(int fd, int64_t offset, int whence) {
    long ret = sys3(SYS_lseek, fd, offset, whence);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

int64_t lsk_lseek_m(int fd, int64_t offset, int whence) __asm__("___lseek");
int64_t lsk_lseek_m(int fd, int64_t offset, int whence) {
    return lsk_lseek(fd, offset, whence);
}

int64_t lsk_lseek_nocancel(int fd, int64_t offset, int whence) __asm__("_lseek$NOCANCEL");
int64_t lsk_lseek_nocancel(int fd, int64_t offset, int whence) {
    return lsk_lseek(fd, offset, whence);
}

int lsk_fcntl(int fd, int cmd, ...) __asm__("_fcntl");
int lsk_fcntl(int fd, int cmd, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    va_list ap;
    va_start(ap, cmd);
    void *varg = va_arg(ap, void *);
    va_end(ap);
    long arg = (long)(varg ? varg : (void *)captured_x2);

    if (cmd == 50 /* F_GETPATH */ || cmd == 102 /* F_GETPATH_NOFIRMLINK */) {
        if (!arg) {
            set_errno(14); /* EFAULT */
            return -1;
        }
        if (fd < 0) {
            set_errno(9); /* EBADF */
            return -1;
        }
        char procfd[40] = "/proc/self/fd/";
        int pos = 14;
        char num[12];
        int npos = 0;
        int temp = fd;
        if (temp == 0) {
            num[npos++] = '0';
        } else {
            while (temp > 0) {
                num[npos++] = '0' + (temp % 10);
                temp /= 10;
            }
        }
        for (int i = npos - 1; i >= 0; i--) {
            procfd[pos++] = num[i];
        }
        procfd[pos] = '\0';

        char *out = (char *)arg;
        long len = sys4(SYS_readlinkat, AT_FDCWD, (long)procfd, (long)out, 1023);
        if (len < 0) {
            set_errno(linux_to_darwin_errno(len));
            return -1;
        }
        out[len] = '\0';
        lsk_path_unmap_inplace(out);
        return 0;
    }

    if (cmd == 3 /* F_GETFL */) {
        long ret = sys3(SYS_fcntl, fd, 3, 0);
        if (ret < 0) {
            set_errno(linux_to_darwin_errno(ret));
            return -1;
        }
        int dflags = 0;
        if ((ret & 3) == 0) dflags |= 0;
        else if ((ret & 3) == 1) dflags |= 1;
        else if ((ret & 3) == 2) dflags |= 2;
        if (ret & 04000) dflags |= 0x0004;       /* O_NONBLOCK */
        if (ret & 02000) dflags |= 0x0008;       /* O_APPEND */
        if (ret & 020000) dflags |= 0x0040;      /* O_ASYNC */
        if (ret & 04010000) dflags |= 0x0080;    /* O_FSYNC / O_SYNC */
        if (ret & 02000000) dflags |= 0x1000000; /* O_CLOEXEC */
        return dflags;
    }

    if (cmd == 4 /* F_SETFL */) {
        int larg = 0;
        if ((arg & 3) == 0) larg |= 0;
        else if ((arg & 3) == 1) larg |= 1;
        else if ((arg & 3) == 2) larg |= 2;
        if (arg & 0x0004) larg |= 04000;
        if (arg & 0x0008) larg |= 02000;
        if (arg & 0x0040) larg |= 020000;
        if (arg & 0x0080) larg |= 04010000;
        if (arg & 0x1000000) larg |= 02000000;
        long ret = sys3(SYS_fcntl, fd, 4, larg);
        if (ret < 0) {
            set_errno(linux_to_darwin_errno(ret));
            return -1;
        }
        return 0;
    }

    if (cmd == 7 /* F_GETLK */ || cmd == 8 /* F_SETLK */ || cmd == 9 /* F_SETLKW */ ||
        cmd == 90 /* F_OFD_SETLK */ || cmd == 91 /* F_OFD_SETLKW */ || cmd == 92 /* F_OFD_GETLK */) {
        if (!arg) {
            set_errno(14); /* EFAULT */
            return -1;
        }
        struct darwin_flock *df = (struct darwin_flock *)arg;
        struct linux_flock lf;
        lf._pad = 0;
        lf._pad2 = 0;
        lf.l_whence = df->l_whence;
        lf.l_start = df->l_start;
        lf.l_len = df->l_len;
        lf.l_pid = df->l_pid;
        if (df->l_type == 1 /* Darwin F_RDLCK */) lf.l_type = 0 /* Linux F_RDLCK */;
        else if (df->l_type == 3 /* Darwin F_WRLCK */) lf.l_type = 1 /* Linux F_WRLCK */;
        else if (df->l_type == 2 /* Darwin F_UNLCK */) lf.l_type = 2 /* Linux F_UNLCK */;
        else lf.l_type = df->l_type;

        int lcmd;
        if (cmd == 7) lcmd = 5;           /* Linux F_GETLK */
        else if (cmd == 8) lcmd = 6;      /* Linux F_SETLK */
        else if (cmd == 9) lcmd = 7;      /* Linux F_SETLKW */
        else if (cmd == 90) lcmd = 37;    /* Darwin F_OFD_SETLK  -> Linux F_OFD_SETLK */
        else if (cmd == 91) lcmd = 38;    /* Darwin F_OFD_SETLKW -> Linux F_OFD_SETLKW */
        else lcmd = 36;                   /* Darwin F_OFD_GETLK  -> Linux F_OFD_GETLK */
        if (cmd >= 90) lf.l_pid = 0;      /* Linux OFD locks require l_pid == 0 (EINVAL otherwise) */

        long ret = sys3(SYS_fcntl, fd, lcmd, (long)&lf);
        if (ret < 0) {
            set_errno(linux_to_darwin_errno(ret));
            return -1;
        }
        if (cmd == 7 /* F_GETLK */ || cmd == 92 /* F_OFD_GETLK */) {
            df->l_whence = lf.l_whence;
            df->l_start = lf.l_start;
            df->l_len = lf.l_len;
            df->l_pid = lf.l_pid;
            if (lf.l_type == 0 /* Linux F_RDLCK */) df->l_type = 1 /* Darwin F_RDLCK */;
            else if (lf.l_type == 1 /* Linux F_WRLCK */) df->l_type = 3 /* Darwin F_WRLCK */;
            else if (lf.l_type == 2 /* Linux F_UNLCK */) df->l_type = 2 /* Darwin F_UNLCK */;
            else df->l_type = lf.l_type;
        }
        return 0;
    }

    if (cmd == 51 /* F_FULLFSYNC */ || cmd == 85 /* F_BARRIERFSYNC */) {
        long ret = sys1(SYS_fsync, fd);
        if (ret < 0) {
            set_errno(linux_to_darwin_errno(ret));
            return -1;
        }
        return 0;
    }

    if (cmd == 42 /* F_PREALLOCATE */) {
        if (arg) {
            struct darwin_fstore {
                uint32_t fst_flags;
                int32_t fst_posmode;
                int64_t fst_offset;
                int64_t fst_length;
                int64_t fst_bytesalloc;
            } *fst = (struct darwin_fstore *)arg;
            long ret = sys4(SYS_fallocate, fd, 0, fst->fst_offset, fst->fst_length);
            if (ret == 0) {
                fst->fst_bytesalloc = fst->fst_length;
                return 0;
            }
        }
        return 0;
    }

    if (cmd == 67 /* F_DUPFD_CLOEXEC */) {
        long ret = sys3(SYS_fcntl, fd, 1030 /* Linux F_DUPFD_CLOEXEC */, arg);
        if (ret < 0) {
            set_errno(linux_to_darwin_errno(ret));
            return -1;
        }
        return (int)ret;
    }

    if (cmd == 73 /* F_SETNOSIGPIPE */ || cmd == 74 /* F_GETNOSIGPIPE */ ||
        cmd == 48 /* F_NOCACHE */ || cmd == 55 /* F_GLOBAL_NOCACHE */ || cmd == 44 /* F_RDADVISE */) {
        return 0;
    }

    long ret = sys3(SYS_fcntl, fd, cmd, arg);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_fcntl_m(int fd, int cmd, ...) __asm__("___fcntl");
int lsk_fcntl_m(int fd, int cmd, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    va_list ap;
    va_start(ap, cmd);
    void *varg = va_arg(ap, void *);
    va_end(ap);
    long arg = (long)(varg ? varg : (void *)captured_x2);
    return lsk_fcntl(fd, cmd, arg);
}

int lsk_fcntl_nocancel(int fd, int cmd, ...) __asm__("_fcntl$NOCANCEL");
int lsk_fcntl_nocancel(int fd, int cmd, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    va_list ap;
    va_start(ap, cmd);
    void *varg = va_arg(ap, void *);
    va_end(ap);
    long arg = (long)(varg ? varg : (void *)captured_x2);
    return lsk_fcntl(fd, cmd, arg);
}
int lsk_fcntl_nocancel_m(int fd, int cmd, ...) __asm__("___fcntl_nocancel");
int lsk_fcntl_nocancel_m(int fd, int cmd, ...) {
    register long reg_x2 __asm__("x2");
    long captured_x2 = reg_x2;
    va_list ap;
    va_start(ap, cmd);
    void *varg = va_arg(ap, void *);
    va_end(ap);
    long arg = (long)(varg ? varg : (void *)captured_x2);
    return lsk_fcntl(fd, cmd, arg);
}

int lsk_ioctl(int fd, unsigned long req, void *arg) __asm__("_ioctl");
int lsk_ioctl(int fd, unsigned long req, void *arg) {
    long ret = sys3(SYS_ioctl, fd, req, (long)arg);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_ioctl_m(int fd, unsigned long req, void *arg) __asm__("___ioctl");
int lsk_ioctl_m(int fd, unsigned long req, void *arg) {
    return lsk_ioctl(fd, req, arg);
}

int lsk_dup(int fd) __asm__("_dup");
int lsk_dup(int fd) {
    long ret = sys3(SYS_fcntl, fd, 0 /* F_DUPFD */, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_dup2(int oldfd, int newfd) __asm__("_dup2");
int lsk_dup2(int oldfd, int newfd) {
    long ret = sys3(SYS_fcntl, oldfd, 0 /* F_DUPFD */, newfd);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_pipe(int fds[2]) __asm__("_pipe");
int lsk_pipe(int fds[2]) {
    long ret = sys2(SYS_pipe2, (long)fds, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_pipe_m(int fds[2]) __asm__("___pipe");
int lsk_pipe_m(int fds[2]) {
    return lsk_pipe(fds);
}

/* POSIX shm */
int lsk_shm_open(const char *name, int oflag, int mode) __asm__("_shm_open");
int lsk_shm_open(const char *name, int oflag, int mode) {
    {   /* In-process cfprefsd generation page (memfd when /dev/shm is absent). */
        extern int cfprefsd_shm_fd(const char *name);
        const char *n = (name && name[0] == '/') ? name + 1 : name;
        int sfd = n ? cfprefsd_shm_fd(n) : -1;
        if (sfd >= 0) {
            long d = sys3(SYS_fcntl, sfd, 1030 /* F_DUPFD_CLOEXEC */, 0);
            if (d >= 0) return (int)d;
        }
    }
    int lflags = darwin_open_flags_to_linux(oflag);
    char path[128];
    const char prefix[] = "/dev/shm/";
    int i = 0;
    while (prefix[i] && i < 100) { path[i] = prefix[i]; i++; }
    if (name && name[0] == '/') name++;
    int j = 0;
    while (name && name[j] && i < 120) { path[i++] = name[j++]; }
    path[i] = '\0';

    long ret = sys4(SYS_openat, AT_FDCWD, (long)path, lflags, mode);
    if (ret < 0) {
        ret = sys2(279 /* SYS_memfd_create */, (long)(name ? name : "shm"), 0);
    }
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_shm_open_m(const char *name, int oflag, int mode) __asm__("___shm_open");
int lsk_shm_open_m(const char *name, int oflag, int mode) {
    return lsk_shm_open(name, oflag, mode);
}

int lsk_shm_unlink(const char *name) __asm__("_shm_unlink");
int lsk_shm_unlink(const char *name) {
    (void)name;
    return 0;
}

/* ---------- Sockets ---------- */

static int darwin_to_linux_af(int af) {
    if (af == 30) return 10; /* AF_INET6 */
    return af;
}

static int linux_to_darwin_af(int af) {
    if (af == 10) return 30; /* AF_INET6 */
    return af;
}

static unsigned int darwin_to_linux_sockaddr(const void *addr, unsigned int len, uint8_t *out, unsigned int out_max) {
    if (!addr || len == 0 || out_max < 2) return 0;
    unsigned int to_copy = len < out_max ? len : out_max;
    const uint8_t *in = (const uint8_t *)addr;
    for (unsigned int i = 0; i < to_copy; i++) out[i] = in[i];

    uint8_t b0 = in[0];
    uint8_t b1 = (len > 1) ? in[1] : 0;
    uint16_t family;

    if (b1 == 0 && (b0 == 1 || b0 == 2 || b0 == 30)) {
        family = darwin_to_linux_af(b0);
    } else {
        family = darwin_to_linux_af(b1);
    }
    *(uint16_t *)out = family;
    return to_copy;
}

static void linux_to_darwin_sockaddr(const uint8_t *in, unsigned int len, void *addr) {
    if (!addr || len < 2) return;
    uint8_t *out = (uint8_t *)addr;
    uint16_t linux_family = *(const uint16_t *)in;
    uint8_t darwin_family = (uint8_t)linux_to_darwin_af(linux_family);
    out[0] = (uint8_t)len; /* sa_len */
    out[1] = darwin_family; /* sa_family */
}

int lsk_socket(int domain, int type, int protocol) __asm__("_socket");
int lsk_socket(int domain, int type, int protocol) {
    int ldomain = darwin_to_linux_af(domain);
    long ret = sys3(SYS_socket, ldomain, type, protocol);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_socketpair(int domain, int type, int protocol, int sv[2]) __asm__("_socketpair");
int lsk_socketpair(int domain, int type, int protocol, int sv[2]) {
    int ldomain = darwin_to_linux_af(domain);
    long ret = sys4(SYS_socketpair, ldomain, type, protocol, (long)sv);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_socketpair_m(int domain, int type, int protocol, int sv[2]) __asm__("___socketpair");
int lsk_socketpair_m(int domain, int type, int protocol, int sv[2]) {
    return lsk_socketpair(domain, type, protocol, sv);
}

int lsk_bind(int fd, const void *addr, unsigned int len) __asm__("_bind");
int lsk_bind(int fd, const void *addr, unsigned int len) {
    uint8_t laddr[128];
    unsigned int llen = darwin_to_linux_sockaddr(addr, len, laddr, sizeof(laddr));
    long ret = sys3(SYS_bind, fd, (long)laddr, llen ? llen : len);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_bind_m(int fd, const void *addr, unsigned int len) __asm__("___bind");
int lsk_bind_m(int fd, const void *addr, unsigned int len) {
    return lsk_bind(fd, addr, len);
}

int lsk_connect(int fd, const void *addr, unsigned int len) __asm__("_connect");
int lsk_connect(int fd, const void *addr, unsigned int len) {
    uint8_t laddr[128];
    unsigned int llen = darwin_to_linux_sockaddr(addr, len, laddr, sizeof(laddr));
    long ret = sys3(SYS_connect, fd, (long)laddr, llen ? llen : len);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_connect_m(int fd, const void *addr, unsigned int len) __asm__("___connect");
int lsk_connect_m(int fd, const void *addr, unsigned int len) {
    return lsk_connect(fd, addr, len);
}
int lsk_connect_nocancel(int fd, const void *addr, unsigned int len) __asm__("_connect$NOCANCEL");
int lsk_connect_nocancel(int fd, const void *addr, unsigned int len) {
    return lsk_connect(fd, addr, len);
}
int lsk_connect_nocancel_m(int fd, const void *addr, unsigned int len) __asm__("___connect_nocancel");
int lsk_connect_nocancel_m(int fd, const void *addr, unsigned int len) {
    return lsk_connect(fd, addr, len);
}

int lsk_listen(int fd, int backlog) __asm__("_listen");
int lsk_listen(int fd, int backlog) {
    long ret = sys2(SYS_listen, fd, backlog);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}
int lsk_listen_m(int fd, int backlog) __asm__("___listen");
int lsk_listen_m(int fd, int backlog) {
    return lsk_listen(fd, backlog);
}

int lsk_accept(int fd, void *addr, unsigned int *len) __asm__("_accept");
int lsk_accept(int fd, void *addr, unsigned int *len) {
    uint8_t laddr[128];
    unsigned int llen = sizeof(laddr);
    long ret = sys3(SYS_accept, fd, addr ? (long)laddr : 0, addr ? (long)&llen : 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (addr && len) {
        unsigned int out_len = *len < llen ? *len : llen;
        for (unsigned int i = 0; i < out_len; i++) ((uint8_t *)addr)[i] = laddr[i];
        linux_to_darwin_sockaddr(laddr, llen, addr);
        *len = llen;
    }
    return (int)ret;
}
int lsk_accept_m(int fd, void *addr, unsigned int *len) __asm__("___accept");
int lsk_accept_m(int fd, void *addr, unsigned int *len) {
    return lsk_accept(fd, addr, len);
}
int lsk_accept_nocancel(int fd, void *addr, unsigned int *len) __asm__("_accept$NOCANCEL");
int lsk_accept_nocancel(int fd, void *addr, unsigned int *len) {
    return lsk_accept(fd, addr, len);
}
int lsk_accept_nocancel_m(int fd, void *addr, unsigned int *len) __asm__("___accept_nocancel");
int lsk_accept_nocancel_m(int fd, void *addr, unsigned int *len) {
    return lsk_accept(fd, addr, len);
}

ssize_t lsk_sendto(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) __asm__("_sendto");
ssize_t lsk_sendto(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) {
    uint8_t laddr[128];
    unsigned int llen = to ? darwin_to_linux_sockaddr(to, tolen, laddr, sizeof(laddr)) : 0;
    long ret = sys6(SYS_sendto, fd, (long)buf, len, flags, to ? (long)laddr : 0, llen ? llen : tolen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}
ssize_t lsk_sendto_m(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) __asm__("___sendto");
ssize_t lsk_sendto_m(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) {
    return lsk_sendto(fd, buf, len, flags, to, tolen);
}
ssize_t lsk_sendto_nocancel(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) __asm__("_sendto$NOCANCEL");
ssize_t lsk_sendto_nocancel(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) {
    return lsk_sendto(fd, buf, len, flags, to, tolen);
}
ssize_t lsk_sendto_nocancel_m(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) __asm__("___sendto_nocancel");
ssize_t lsk_sendto_nocancel_m(int fd, const void *buf, size_t len, int flags, const void *to, unsigned int tolen) {
    return lsk_sendto(fd, buf, len, flags, to, tolen);
}

ssize_t lsk_recvfrom(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) __asm__("_recvfrom");
ssize_t lsk_recvfrom(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) {
    uint8_t laddr[128];
    unsigned int llen = sizeof(laddr);
    long ret = sys6(SYS_recvfrom, fd, (long)buf, len, flags, from ? (long)laddr : 0, from ? (long)&llen : 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (from && fromlen) {
        unsigned int out_len = *fromlen < llen ? *fromlen : llen;
        for (unsigned int i = 0; i < out_len; i++) ((uint8_t *)from)[i] = laddr[i];
        linux_to_darwin_sockaddr(laddr, llen, from);
        *fromlen = llen;
    }
    return ret;
}
ssize_t lsk_recvfrom_m(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) __asm__("___recvfrom");
ssize_t lsk_recvfrom_m(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) {
    return lsk_recvfrom(fd, buf, len, flags, from, fromlen);
}
ssize_t lsk_recvfrom_nocancel(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) __asm__("_recvfrom$NOCANCEL");
ssize_t lsk_recvfrom_nocancel(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) {
    return lsk_recvfrom(fd, buf, len, flags, from, fromlen);
}
ssize_t lsk_recvfrom_nocancel_m(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) __asm__("___recvfrom_nocancel");
ssize_t lsk_recvfrom_nocancel_m(int fd, void *buf, size_t len, int flags, void *from, unsigned int *fromlen) {
    return lsk_recvfrom(fd, buf, len, flags, from, fromlen);
}

ssize_t lsk_sendmsg(int fd, const void *msg, int flags) __asm__("_sendmsg");
ssize_t lsk_sendmsg(int fd, const void *msg, int flags) {
    long ret = sys3(SYS_sendmsg, fd, (long)msg, flags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}
ssize_t lsk_sendmsg_m(int fd, const void *msg, int flags) __asm__("___sendmsg");
ssize_t lsk_sendmsg_m(int fd, const void *msg, int flags) {
    return lsk_sendmsg(fd, msg, flags);
}
ssize_t lsk_sendmsg_nocancel(int fd, const void *msg, int flags) __asm__("_sendmsg$NOCANCEL");
ssize_t lsk_sendmsg_nocancel(int fd, const void *msg, int flags) {
    return lsk_sendmsg(fd, msg, flags);
}
ssize_t lsk_sendmsg_nocancel_m(int fd, const void *msg, int flags) __asm__("___sendmsg_nocancel");
ssize_t lsk_sendmsg_nocancel_m(int fd, const void *msg, int flags) {
    return lsk_sendmsg(fd, msg, flags);
}

ssize_t lsk_recvmsg(int fd, void *msg, int flags) __asm__("_recvmsg");
ssize_t lsk_recvmsg(int fd, void *msg, int flags) {
    long ret = sys3(SYS_recvmsg, fd, (long)msg, flags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}
ssize_t lsk_recvmsg_m(int fd, void *msg, int flags) __asm__("___recvmsg");
ssize_t lsk_recvmsg_m(int fd, void *msg, int flags) {
    return lsk_recvmsg(fd, msg, flags);
}
ssize_t lsk_recvmsg_nocancel(int fd, void *msg, int flags) __asm__("_recvmsg$NOCANCEL");
ssize_t lsk_recvmsg_nocancel(int fd, void *msg, int flags) {
    return lsk_recvmsg(fd, msg, flags);
}
ssize_t lsk_recvmsg_nocancel_m(int fd, void *msg, int flags) __asm__("___recvmsg_nocancel");
ssize_t lsk_recvmsg_nocancel_m(int fd, void *msg, int flags) {
    return lsk_recvmsg(fd, msg, flags);
}

int lsk_setsockopt(int fd, int level, int optname, const void *optval, unsigned int optlen) __asm__("_setsockopt");
int lsk_setsockopt(int fd, int level, int optname, const void *optval, unsigned int optlen) {
    long ret = sys5(SYS_setsockopt, fd, level, optname, (long)optval, optlen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_getsockopt(int fd, int level, int optname, void *optval, unsigned int *optlen) __asm__("_getsockopt");
int lsk_getsockopt(int fd, int level, int optname, void *optval, unsigned int *optlen) {
    long ret = sys5(SYS_getsockopt, fd, level, optname, (long)optval, (long)optlen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_getsockname(int fd, void *addr, unsigned int *len) __asm__("_getsockname");
int lsk_getsockname(int fd, void *addr, unsigned int *len) {
    uint8_t laddr[128];
    unsigned int llen = sizeof(laddr);
    long ret = sys3(SYS_getsockname, fd, (long)laddr, (long)&llen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (addr && len) {
        unsigned int out_len = *len < llen ? *len : llen;
        for (unsigned int i = 0; i < out_len; i++) ((uint8_t *)addr)[i] = laddr[i];
        linux_to_darwin_sockaddr(laddr, llen, addr);
        *len = llen;
    }
    return 0;
}
int lsk_getsockname_m(int fd, void *addr, unsigned int *len) __asm__("___getsockname");
int lsk_getsockname_m(int fd, void *addr, unsigned int *len) {
    return lsk_getsockname(fd, addr, len);
}

int lsk_getpeername(int fd, void *addr, unsigned int *len) __asm__("_getpeername");
int lsk_getpeername(int fd, void *addr, unsigned int *len) {
    uint8_t laddr[128];
    unsigned int llen = sizeof(laddr);
    long ret = sys3(SYS_getpeername, fd, (long)laddr, (long)&llen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (addr && len) {
        unsigned int out_len = *len < llen ? *len : llen;
        for (unsigned int i = 0; i < out_len; i++) ((uint8_t *)addr)[i] = laddr[i];
        linux_to_darwin_sockaddr(laddr, llen, addr);
        *len = llen;
    }
    return 0;
}
int lsk_getpeername_m(int fd, void *addr, unsigned int *len) __asm__("___getpeername");
int lsk_getpeername_m(int fd, void *addr, unsigned int *len) {
    return lsk_getpeername(fd, addr, len);
}

int lsk_shutdown(int fd, int how) __asm__("_shutdown");
int lsk_shutdown(int fd, int how) {
    long ret = sys2(SYS_shutdown, fd, how);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

/* ---------- Stat ---------- */

struct darwin_stat64 {
    int32_t   st_dev;
    uint16_t  st_mode;
    uint16_t  st_nlink;
    uint64_t  st_ino;
    uint32_t  st_uid;
    uint32_t  st_gid;
    int32_t   st_rdev;
    int32_t   __pad1;
    int64_t   st_atime;
    int64_t   st_atimensec;
    int64_t   st_mtime;
    int64_t   st_mtimensec;
    int64_t   st_ctime;
    int64_t   st_ctimensec;
    int64_t   st_birthtime;
    int64_t   st_birthtimensec;
    int64_t   st_size;
    int64_t   st_blocks;
    int32_t   st_blksize;
    uint32_t  st_flags;
    uint32_t  st_gen;
    int32_t   st_lspare;
    int64_t   st_qspare[2];
};

struct linux_stat64 {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned int   st_mode;
    unsigned int   st_nlink;
    unsigned int   st_uid;
    unsigned int   st_gid;
    unsigned long  st_rdev;
    unsigned long  __pad1;
    long           st_size;
    int            st_blksize;
    int            __pad2;
    long           st_blocks;
    long           st_atime;
    unsigned long  st_atimensec;
    long           st_mtime;
    unsigned long  st_mtimensec;
    long           st_ctime;
    unsigned long  st_ctimensec;
    unsigned int   __pad3[2];
};

static void copy_linux_to_darwin_stat(const struct linux_stat64 *src, struct darwin_stat64 *dst) {
    if (!src || !dst) return;
    for (size_t i = 0; i < sizeof(struct darwin_stat64); i++) {
        ((char *)dst)[i] = 0;
    }
    dst->st_dev = (int32_t)src->st_dev;
    dst->st_mode = (uint16_t)src->st_mode;
    dst->st_nlink = (uint16_t)src->st_nlink;
    dst->st_ino = (uint64_t)src->st_ino;
    dst->st_uid = (uint32_t)src->st_uid;
    dst->st_gid = (uint32_t)src->st_gid;
    dst->st_rdev = (int32_t)src->st_rdev;
    dst->st_atime = (int64_t)src->st_atime;
    dst->st_atimensec = (int64_t)src->st_atimensec;
    dst->st_mtime = (int64_t)src->st_mtime;
    dst->st_mtimensec = (int64_t)src->st_mtimensec;
    dst->st_ctime = (int64_t)src->st_ctime;
    dst->st_ctimensec = (int64_t)src->st_ctimensec;
    dst->st_birthtime = (int64_t)src->st_ctime;
    dst->st_birthtimensec = (int64_t)src->st_ctimensec;
    dst->st_size = (int64_t)src->st_size;
    dst->st_blocks = (int64_t)src->st_blocks;
    dst->st_blksize = (int32_t)src->st_blksize;
}

int lsk_fstat(int fd, void *buf) __asm__("_fstat");
int lsk_fstat(int fd, void *buf) {
    struct linux_stat64 lst;
    long ret = sys2(SYS_fstat, fd, (long)&lst);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    copy_linux_to_darwin_stat(&lst, (struct darwin_stat64 *)buf);
    return 0;
}

int lsk_fstat64(int fd, void *buf) __asm__("_fstat64");
int lsk_fstat64(int fd, void *buf) {
    return lsk_fstat(fd, buf);
}

int lsk_stat(const char *path, void *buf) __asm__("_stat");
int lsk_stat(const char *path, void *buf) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    struct linux_stat64 lst;
    long ret = sys4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&lst, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    copy_linux_to_darwin_stat(&lst, (struct darwin_stat64 *)buf);
    return 0;
}

int lsk_stat64(const char *path, void *buf) __asm__("_stat64");
int lsk_stat64(const char *path, void *buf) {
    return lsk_stat(path, buf);
}

int lsk_lstat(const char *path, void *buf) __asm__("_lstat");
int lsk_lstat(const char *path, void *buf) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    struct linux_stat64 lst;
    long ret = sys4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&lst, 0x100 /* AT_SYMLINK_NOFOLLOW */);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    copy_linux_to_darwin_stat(&lst, (struct darwin_stat64 *)buf);
    return 0;
}

int lsk_lstat64(const char *path, void *buf) __asm__("_lstat64");
int lsk_lstat64(const char *path, void *buf) {
    return lsk_lstat(path, buf);
}

int lsk_fstatat(int fd, const char *path, void *buf, int flag) __asm__("_fstatat");
int lsk_fstatat(int fd, const char *path, void *buf, int flag) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    struct linux_stat64 lst;
    long ret = sys4(SYS_newfstatat, translate_at_fd(fd), (long)path, (long)&lst, translate_at_flags(flag));
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    copy_linux_to_darwin_stat(&lst, (struct darwin_stat64 *)buf);
    return 0;
}

int lsk_fstatat_m(int fd, const char *path, void *buf, int flag) __asm__("___fstatat");
int lsk_fstatat_m(int fd, const char *path, void *buf, int flag) {
    return lsk_fstatat(fd, path, buf, flag);
}

int lsk_fstatat64(int fd, const char *path, void *buf, int flag) __asm__("_fstatat64");
int lsk_fstatat64(int fd, const char *path, void *buf, int flag) {
    return lsk_fstatat(fd, path, buf, flag);
}

int lsk_fstatat64_m(int fd, const char *path, void *buf, int flag) __asm__("___fstatat64");
int lsk_fstatat64_m(int fd, const char *path, void *buf, int flag) {
    return lsk_fstatat(fd, path, buf, flag);
}

/* ---------- getattrlist / setattrlist ---------- */

struct darwin_attrlist {
    uint16_t bitmapcount;
    uint16_t reserved;
    uint32_t commonattr;
    uint32_t volattr;
    uint32_t dirattr;
    uint32_t fileattr;
    uint32_t forkattr;
};

struct darwin_timespec {
    int64_t tv_sec;
    long    tv_nsec;
};

static int pack_attrs(const struct linux_stat64 *st, const struct darwin_attrlist *alp, void *buf, size_t bufsize) {
    if (!buf || bufsize < sizeof(uint32_t)) {
        set_errno(22); /* EINVAL */
        return -1;
    }
    uint8_t *ptr = (uint8_t *)buf;
    uint8_t *end = ptr + bufsize;

    uint32_t *length_ptr = (uint32_t *)ptr;
    ptr += sizeof(uint32_t);

    uint32_t cmn = alp->commonattr;
    uint32_t dir = alp->dirattr;
    uint32_t file = alp->fileattr;
    int is_dir = ((st->st_mode & 0170000) == 0040000);

    if (cmn & 0x80000000 /* ATTR_CMN_RETURNED_ATTRS */) {
        if (ptr + sizeof(struct darwin_attrlist) > end) {
            set_errno(22);
            return -1;
        }
        struct darwin_attrlist *ret_alp = (struct darwin_attrlist *)ptr;
        ret_alp->bitmapcount = 5;
        ret_alp->reserved = 0;
        ret_alp->commonattr = cmn;
        ret_alp->volattr = alp->volattr & 0x00020000;
        ret_alp->dirattr = is_dir ? dir : 0;
        ret_alp->fileattr = is_dir ? 0 : file;
        ret_alp->forkattr = 0;
        ptr += sizeof(struct darwin_attrlist);
    }

    if (cmn & 0x00000001 /* ATTR_CMN_NAME */) {
        if (ptr + 8 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = 0;
        *(uint32_t *)(ptr + 4) = 0;
        ptr += 8;
    }
    if (cmn & 0x00000002 /* ATTR_CMN_DEVID */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(int32_t *)ptr = (int32_t)st->st_dev;
        ptr += 4;
    }
    if (cmn & 0x00000004 /* ATTR_CMN_FSID */) {
        if (ptr + 8 > end) { set_errno(22); return -1; }
        *(int32_t *)ptr = (int32_t)st->st_dev;
        *(int32_t *)(ptr + 4) = 0;
        ptr += 8;
    }
    if (cmn & 0x00000008 /* ATTR_CMN_OBJTYPE */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        uint32_t objtype = 0;
        if ((st->st_mode & 0170000) == 0100000) objtype = 1; /* VREG */
        else if ((st->st_mode & 0170000) == 0040000) objtype = 2; /* VDIR */
        else if ((st->st_mode & 0170000) == 0060000) objtype = 3; /* VBLK */
        else if ((st->st_mode & 0170000) == 0020000) objtype = 4; /* VCHR */
        else if ((st->st_mode & 0170000) == 0120000) objtype = 5; /* VLNK */
        else if ((st->st_mode & 0170000) == 0140000) objtype = 6; /* VSOCK */
        else if ((st->st_mode & 0170000) == 0010000) objtype = 7; /* VFIFO */
        *(uint32_t *)ptr = objtype;
        ptr += 4;
    }
    if (cmn & 0x00000010 /* ATTR_CMN_OBJTAG */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = 1;
        ptr += 4;
    }
    if (cmn & 0x00000020 /* ATTR_CMN_OBJID */) {
        if (ptr + 12 > end) { set_errno(22); return -1; }
        *(uint64_t *)ptr = (uint64_t)st->st_ino;
        *(uint32_t *)(ptr + 8) = 0;
        ptr += 12;
    }
    if (cmn & 0x00000040 /* ATTR_CMN_OBJPERMANENTID */) {
        if (ptr + 12 > end) { set_errno(22); return -1; }
        *(uint64_t *)ptr = (uint64_t)st->st_ino;
        *(uint32_t *)(ptr + 8) = 0;
        ptr += 12;
    }
    if (cmn & 0x00000080 /* ATTR_CMN_PAROBJID */) {
        if (ptr + 12 > end) { set_errno(22); return -1; }
        *(uint64_t *)ptr = 2;
        *(uint32_t *)(ptr + 8) = 0;
        ptr += 12;
    }
    if (cmn & 0x00000100 /* ATTR_CMN_SCRIPT */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = 0;
        ptr += 4;
    }
    if (cmn & 0x00000200 /* ATTR_CMN_CRTIME */) {
        if (ptr + sizeof(struct darwin_timespec) > end) { set_errno(22); return -1; }
        struct darwin_timespec *ts = (struct darwin_timespec *)ptr;
        ts->tv_sec = st->st_ctime;
        ts->tv_nsec = (long)st->st_ctimensec;
        ptr += sizeof(struct darwin_timespec);
    }
    if (cmn & 0x00000400 /* ATTR_CMN_MODTIME */) {
        if (ptr + sizeof(struct darwin_timespec) > end) { set_errno(22); return -1; }
        struct darwin_timespec *ts = (struct darwin_timespec *)ptr;
        ts->tv_sec = st->st_mtime;
        ts->tv_nsec = (long)st->st_mtimensec;
        ptr += sizeof(struct darwin_timespec);
    }
    if (cmn & 0x00000800 /* ATTR_CMN_CHGTIME */) {
        if (ptr + sizeof(struct darwin_timespec) > end) { set_errno(22); return -1; }
        struct darwin_timespec *ts = (struct darwin_timespec *)ptr;
        ts->tv_sec = st->st_ctime;
        ts->tv_nsec = (long)st->st_ctimensec;
        ptr += sizeof(struct darwin_timespec);
    }
    if (cmn & 0x00001000 /* ATTR_CMN_ACCTIME */) {
        if (ptr + sizeof(struct darwin_timespec) > end) { set_errno(22); return -1; }
        struct darwin_timespec *ts = (struct darwin_timespec *)ptr;
        ts->tv_sec = st->st_atime;
        ts->tv_nsec = (long)st->st_atimensec;
        ptr += sizeof(struct darwin_timespec);
    }
    if (cmn & 0x00002000 /* ATTR_CMN_BKUPTIME */) {
        if (ptr + sizeof(struct darwin_timespec) > end) { set_errno(22); return -1; }
        struct darwin_timespec *ts = (struct darwin_timespec *)ptr;
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        ptr += sizeof(struct darwin_timespec);
    }
    if (cmn & 0x00004000 /* ATTR_CMN_FNDRINFO */) {
        if (ptr + 32 > end) { set_errno(22); return -1; }
        for (int i = 0; i < 32; i++) ptr[i] = 0;
        ptr += 32;
    }
    if (cmn & 0x00008000 /* ATTR_CMN_OWNERID */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = st->st_uid;
        ptr += 4;
    }
    if (cmn & 0x00010000 /* ATTR_CMN_GRPID */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = st->st_gid;
        ptr += 4;
    }
    if (cmn & 0x00020000 /* ATTR_CMN_ACCESSMASK */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = (st->st_mode & 07777);
        ptr += 4;
    }
    if (cmn & 0x00040000 /* ATTR_CMN_FLAGS */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = 0;
        ptr += 4;
    }
    if (cmn & 0x00200000 /* ATTR_CMN_USERACCESS */) {
        if (ptr + 4 > end) { set_errno(22); return -1; }
        *(uint32_t *)ptr = 0x7;
        ptr += 4;
    }
    if (cmn & 0x02000000 /* ATTR_CMN_FILEID */) {
        if (ptr + 8 > end) { set_errno(22); return -1; }
        *(uint64_t *)ptr = (uint64_t)st->st_ino;
        ptr += 8;
    }
    if (cmn & 0x04000000 /* ATTR_CMN_PARENTID */) {
        if (ptr + 8 > end) { set_errno(22); return -1; }
        *(uint64_t *)ptr = 2;
        ptr += 8;
    }

    if (is_dir) {
        if (dir & 0x00000001 /* ATTR_DIR_LINKCOUNT */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_nlink;
            ptr += 4;
        }
        if (dir & 0x00000002 /* ATTR_DIR_ENTRYCOUNT */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = 2;
            ptr += 4;
        }
        if (dir & 0x00000008 /* ATTR_DIR_ALLOCSIZE */) {
            if (ptr + 8 > end) { set_errno(22); return -1; }
            *(uint64_t *)ptr = (uint64_t)st->st_blocks * 512;
            ptr += 8;
        }
        if (dir & 0x00000010 /* ATTR_DIR_IOBLOCKSIZE */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_blksize;
            ptr += 4;
        }
    } else {
        if (file & 0x00000001 /* ATTR_FILE_LINKCOUNT */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_nlink;
            ptr += 4;
        }
        if (file & 0x00000002 /* ATTR_FILE_TOTALSIZE */) {
            if (ptr + 8 > end) { set_errno(22); return -1; }
            *(uint64_t *)ptr = (uint64_t)st->st_size;
            ptr += 8;
        }
        if (file & 0x00000004 /* ATTR_FILE_ALLOCSIZE */) {
            if (ptr + 8 > end) { set_errno(22); return -1; }
            *(uint64_t *)ptr = (uint64_t)st->st_blocks * 512;
            ptr += 8;
        }
        if (file & 0x00000008 /* ATTR_FILE_IOBLOCKSIZE */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_blksize;
            ptr += 4;
        }
        if (file & 0x00000010 /* ATTR_FILE_CLUMPSIZE */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_blksize;
            ptr += 4;
        }
        if (file & 0x00000020 /* ATTR_FILE_DEVTYPE */) {
            if (ptr + 4 > end) { set_errno(22); return -1; }
            *(uint32_t *)ptr = (uint32_t)st->st_rdev;
            ptr += 4;
        }
        if (file & 0x00000200 /* ATTR_FILE_DATALENGTH */) {
            if (ptr + 8 > end) { set_errno(22); return -1; }
            *(uint64_t *)ptr = (uint64_t)st->st_size;
            ptr += 8;
        }
        if (file & 0x00000400 /* ATTR_FILE_DATAALLOCSIZE */) {
            if (ptr + 8 > end) { set_errno(22); return -1; }
            *(uint64_t *)ptr = (uint64_t)st->st_blocks * 512;
            ptr += 8;
        }
    }

    if (alp->volattr & 0x00020000 /* ATTR_VOL_CAPABILITIES */) {
        if (ptr + 32 > end) { set_errno(22); return -1; }
        uint32_t *caps = (uint32_t *)ptr;
        for (int i = 0; i < 8; i++) caps[i] = 0;
        caps[0] = 0x00000001; /* VOL_CAP_FMT_PERSISTENTOBJECTIDS */
        caps[4] = 0x00000001; /* valid mask */
        ptr += 32;
    }

    *length_ptr = (uint32_t)(ptr - (uint8_t *)buf);
    return 0;
}

int lsk_getattrlist(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("_getattrlist");
int lsk_getattrlist_m(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("___getattrlist");
int lsk_getattrlist(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    if (!path || !attrList) {
        set_errno(22); /* EINVAL */
        return -1;
    }
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    struct linux_stat64 lst;
    int flags = (options & 0x0001) ? 0x100 : 0;
    long ret = sys4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&lst, flags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return pack_attrs(&lst, (const struct darwin_attrlist *)attrList, attrBuf, attrBufSize);
}

int lsk_getattrlist_m(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    return lsk_getattrlist(path, attrList, attrBuf, attrBufSize, options);
}

int lsk_fgetattrlist(int fd, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("_fgetattrlist");
int lsk_fgetattrlist(int fd, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    (void)options;
    if (!attrList) {
        set_errno(22);
        return -1;
    }
    struct linux_stat64 lst;
    long ret = sys2(SYS_fstat, fd, (long)&lst);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return pack_attrs(&lst, (const struct darwin_attrlist *)attrList, attrBuf, attrBufSize);
}

int lsk_getattrlistat(int fd, const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned long options) __asm__("_getattrlistat");
int lsk_getattrlistat(int fd, const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned long options) {
    if (!path || !attrList) {
        set_errno(22);
        return -1;
    }
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    struct linux_stat64 lst;
    int flags = (options & 0x0001) ? 0x100 : 0;
    long ret = sys4(SYS_newfstatat, translate_at_fd(fd), (long)path, (long)&lst, flags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return pack_attrs(&lst, (const struct darwin_attrlist *)attrList, attrBuf, attrBufSize);
}

int lsk_getattrlistbulk(int dirfd, void *attrList, void *attrBuf, size_t attrBufSize, uint64_t options) __asm__("_getattrlistbulk");
int lsk_getattrlistbulk(int dirfd, void *attrList, void *attrBuf, size_t attrBufSize, uint64_t options) {
    /* Minimal: ATTR_CMN_RETURNED_ATTRS (0x80000000), ATTR_CMN_NAME (0x1), ATTR_CMN_OBJTYPE (0x8). Other requested
     * attributes are not filled and not reported in the returned set (callers check it). CoreFoundation's
     * directory iteration (CFBundle Info.plist lookup) uses this; ENOTSUP made bundles look empty. */
    (void)options;
    const uint32_t *al = (const uint32_t *)attrList; /* bitmapcount(u16) reserved(u16) commonattr volattr dirattr fileattr forkattr */
    uint32_t want = al ? al[1] : 0;
    int want_ret = (want & 0x80000000u) != 0, want_name = (want & 0x1u) != 0, want_type = (want & 0x8u) != 0;
    uint8_t dbuf[2048];
    uint8_t *out = (uint8_t *)attrBuf;
    size_t used = 0;
    int count = 0;
    for (;;) {
        long n = sys3(SYS_getdents64, dirfd, (long)dbuf, sizeof(dbuf));
        if (n < 0) { if (count) break; set_errno(linux_to_darwin_errno(n)); return -1; }
        if (n == 0) break;
        long off = 0;
        int64_t last_ok_off = -1;
        int full = 0;
        while (off < n) {
            uint8_t *d = dbuf + off;
            int64_t d_off = *(int64_t *)(d + 8);
            uint16_t reclen = *(uint16_t *)(d + 16);
            uint8_t dtype = d[18];
            const char *name = (const char *)(d + 19);
            off += reclen;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) { last_ok_off = d_off; continue; }
            size_t nlen = 0; while (name[nlen]) nlen++;
            size_t fixed = 4 + (want_ret ? 20 : 0) + (want_name ? 8 : 0) + (want_type ? 4 : 0);
            size_t entry = fixed + (want_name ? nlen + 1 : 0);
            entry = (entry + 7) & ~(size_t)7;
            if (used + entry > attrBufSize) { full = 1; break; }
            uint8_t *e = out + used;
            for (size_t k = 0; k < entry; k++) e[k] = 0;
            *(uint32_t *)e = (uint32_t)entry;
            uint8_t *q = e + 4;
            if (want_ret) {
                uint32_t *rs = (uint32_t *)q;
                rs[0] = (want_name ? 0x1u : 0) | (want_type ? 0x8u : 0) | 0x80000000u;
                q += 20;
            }
            if (want_name) {
                int32_t *ref = (int32_t *)q;
                ref[0] = (int32_t)((e + fixed) - q); /* attr_dataoffset relative to the attrreference */
                ref[1] = (int32_t)(nlen + 1);
                for (size_t k = 0; k <= nlen; k++) e[fixed + k] = (uint8_t)name[k];
                q += 8;
            }
            if (want_type) {
                uint32_t vt = (dtype == 4) ? 2 : (dtype == 10) ? 5 : 1; /* DT_DIR->VDIR, DT_LNK->VLNK, else VREG */
                *(uint32_t *)q = vt;
                q += 4;
            }
            used += entry;
            count++;
            last_ok_off = d_off;
        }
        if (full) {
            if (last_ok_off >= 0) sys3(SYS_lseek, dirfd, last_ok_off, 0);
            if (count == 0) { set_errno(22); return -1; }
            break;
        }
        if (count) break; /* return what we have; the caller calls again */
    }
    return count;
}

int lsk_setattrlist(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("_setattrlist");
int lsk_setattrlist(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    (void)path; (void)attrList; (void)attrBuf; (void)attrBufSize; (void)options;
    return 0;
}

int lsk_setattrlist_m(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("___setattrlist");
int lsk_setattrlist_m(const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    return lsk_setattrlist(path, attrList, attrBuf, attrBufSize, options);
}

int lsk_fsetattrlist(int fd, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("_fsetattrlist");
int lsk_fsetattrlist(int fd, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    (void)fd; (void)attrList; (void)attrBuf; (void)attrBufSize; (void)options;
    return 0;
}

int lsk_setattrlistat(int fd, const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) __asm__("_setattrlistat");
int lsk_setattrlistat(int fd, const char *path, void *attrList, void *attrBuf, size_t attrBufSize, unsigned int options) {
    (void)fd; (void)path; (void)attrList; (void)attrBuf; (void)attrBufSize; (void)options;
    return 0;
}

/* ---------- access / readlink / pathconf ---------- */

int lsk_access(const char *path, int mode) __asm__("_access");
int lsk_access(const char *path, int mode) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys3(SYS_faccessat, AT_FDCWD, (long)path, mode);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_faccessat(int fd, const char *path, int mode, int flag) __asm__("_faccessat");
int lsk_faccessat(int fd, const char *path, int mode, int flag) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys4(SYS_faccessat, translate_at_fd(fd), (long)path, mode, translate_at_flags(flag));
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_accessx_np(const void *desc, int count, int *acls, uint32_t flags) __asm__("_accessx_np");
int lsk_accessx_np(const void *desc, int count, int *acls, uint32_t flags) {
    (void)desc; (void)count; (void)acls; (void)flags;
    return 0;
}

int lsk_access_extended(const void *desc, int count, int *acls, uint32_t flags) __asm__("___access_extended");
int lsk_access_extended(const void *desc, int count, int *acls, uint32_t flags) {
    return lsk_accessx_np(desc, count, acls, flags);
}

ssize_t lsk_readlink(const char *path, char *buf, size_t bufsiz) __asm__("_readlink");
ssize_t lsk_readlink(const char *path, char *buf, size_t bufsiz) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys4(SYS_readlinkat, AT_FDCWD, (long)path, (long)buf, bufsiz);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_readlinkat(int fd, const char *path, char *buf, size_t bufsiz) __asm__("_readlinkat");
ssize_t lsk_readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    long ret = sys4(SYS_readlinkat, translate_at_fd(fd), (long)path, (long)buf, bufsiz);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return ret;
}

ssize_t lsk_freadlink(int fd, char *buf, size_t bufsiz) __asm__("_freadlink");
ssize_t lsk_freadlink(int fd, char *buf, size_t bufsiz) {
    (void)fd; (void)buf; (void)bufsiz;
    set_errno(22); /* EINVAL */
    return -1;
}

long lsk_pathconf(const char *path, int name) __asm__("_pathconf");
long lsk_pathconf(const char *path, int name) {
    (void)path;
    if (name == 4 /* _PC_NAME_MAX */) return 255;
    if (name == 5 /* _PC_PATH_MAX */) return 1024;
    if (name == 6 /* _PC_PIPE_BUF */) return 512;
    return 1024;
}

long lsk_fpathconf(int fd, int name) __asm__("_fpathconf");
long lsk_fpathconf(int fd, int name) {
    (void)fd;
    if (name == 4 /* _PC_NAME_MAX */) return 255;
    if (name == 5 /* _PC_PATH_MAX */) return 1024;
    if (name == 6 /* _PC_PIPE_BUF */) return 512;
    return 1024;
}

/* ---------- Process Identity & Misc ---------- */

int lsk_getpid(void) __asm__("_getpid");
int lsk_getpid(void) {
    return (int)sys0(SYS_getpid);
}

int lsk_getpid_m(void) __asm__("___getpid");
int lsk_getpid_m(void) {
    return lsk_getpid();
}

uint64_t lsk_gettid_m(void) __asm__("___gettid");
uint64_t lsk_gettid_m(void) {
    return sys0(SYS_gettid);
}

int lsk_getppid(void) __asm__("_getppid");
int lsk_getppid(void) {
    return (int)sys0(SYS_getppid);
}

uint32_t lsk_getuid(void) __asm__("_getuid");
uint32_t lsk_getuid(void) {
    return (uint32_t)sys0(SYS_getuid);
}

uint32_t lsk_geteuid(void) __asm__("_geteuid");
uint32_t lsk_geteuid(void) {
    return (uint32_t)sys0(SYS_geteuid);
}

uint32_t lsk_getgid(void) __asm__("_getgid");
uint32_t lsk_getgid(void) {
    return (uint32_t)sys0(SYS_getgid);
}

uint32_t lsk_getegid(void) __asm__("_getegid");
uint32_t lsk_getegid(void) {
    return (uint32_t)sys0(SYS_getegid);
}

int lsk_issetugid(void) __asm__("_issetugid");
int lsk_issetugid(void) {
    return 0;
}

int lsk_getentropy(void *buf, size_t buflen) __asm__("_getentropy");
int lsk_getentropy(void *buf, size_t buflen) {
    long ret = sys3(SYS_getrandom, (long)buf, buflen, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_poll(void *fds, uint32_t nfds, int timeout) __asm__("_poll");
int lsk_poll(void *fds, uint32_t nfds, int timeout) {
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    long ret = sys4(SYS_ppoll, (long)fds, nfds, timeout >= 0 ? (long)&ts : 0, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return (int)ret;
}

int lsk_poll_nocancel(void *fds, uint32_t nfds, int timeout) __asm__("_poll$NOCANCEL");
int lsk_poll_nocancel(void *fds, uint32_t nfds, int timeout) {
    return lsk_poll(fds, nfds, timeout);
}

int lsk_csops(int pid, uint32_t ops, void *useraddr, size_t usersize) __asm__("_csops");
int lsk_csops(int pid, uint32_t ops, void *useraddr, size_t usersize) {
    (void)pid;
    (void)ops;
    if (useraddr && usersize > 0) {
        for (size_t i = 0; i < usersize; i++) ((char *)useraddr)[i] = 0;
    }
    return 0;
}

int lsk_csops_audittoken(int pid, uint32_t ops, void *useraddr, size_t usersize, void *token) __asm__("_csops_audittoken");
int lsk_csops_audittoken(int pid, uint32_t ops, void *useraddr, size_t usersize, void *token) {
    (void)token;
    return lsk_csops(pid, ops, useraddr, usersize);
}

int lsk_proc_info(int callnum, int pid, int flavor, uint64_t arg, void *buffer, int buffersize) __asm__("_proc_info");
int lsk_proc_info(int callnum, int pid, int flavor, uint64_t arg, void *buffer, int buffersize) {
    (void)callnum;
    (void)pid;
    (void)flavor;
    (void)arg;
    (void)buffer;
    (void)buffersize;
    return 0;
}

int lsk_proc_info_m(int callnum, int pid, int flavor, uint64_t arg, void *buffer, int buffersize) __asm__("___proc_info");
int lsk_proc_info_m(int callnum, int pid, int flavor, uint64_t arg, void *buffer, int buffersize) {
    return lsk_proc_info(callnum, pid, flavor, arg, buffer, buffersize);
}

int lsk_proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize) __asm__("_proc_pidinfo");
int lsk_proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize) {
    if (flavor == 17) { /* PROC_PIDUNIQIDENTIFIERINFO */
        if (!buffer || buffersize < 56) return 0;
        char *b = (char *)buffer;
        for (int i = 0; i < 56; i++) b[i] = 0;
        uint64_t *p_uniqueid = (uint64_t *)(b + 16);
        *p_uniqueid = (uint64_t)sys0(SYS_getpid);
        uint64_t *p_puniqueid = (uint64_t *)(b + 24);
        *p_puniqueid = 1;
        int32_t *p_idversion = (int32_t *)(b + 32);
        *p_idversion = 1;
        return 56;
    }
    return lsk_proc_info(0, pid, flavor, arg, buffer, buffersize);
}

int lsk_proc_pidpath(int pid, void *buffer, uint32_t buffersize) __asm__("_proc_pidpath");
int lsk_proc_pidpath(int pid, void *buffer, uint32_t buffersize) {
    (void)pid;
    if (!buffer || buffersize == 0) return 0;
    long ret = sys4(SYS_readlinkat, AT_FDCWD, (long)"/proc/self/exe", (long)buffer, buffersize - 1);
    if (ret < 0) {
        ((char *)buffer)[0] = '\0';
        return 0;
    }
    ((char *)buffer)[ret] = '\0';
    return (int)ret;
}

int lsk_getdirentries64(int fd, void *buf, size_t bufsize, int64_t *basep) __asm__("___getdirentries64");
int lsk_getdirentries64(int fd, void *buf, size_t bufsize, int64_t *basep) {
    /* Convert Linux dirent64 {ino u64, off s64, reclen u16, type u8, name} to Darwin struct dirent
     * (64-bit inode): {d_ino u64, d_seekoff u64, d_reclen u16, d_namlen u16, d_type u8, d_name[]},
     * records 8-byte aligned. Darwin records are 2 bytes longer, so read into a smaller temp buffer. */
    uint8_t tmp[4096];
    size_t cap = bufsize < sizeof(tmp) ? bufsize : sizeof(tmp);
    if (cap > 64) cap -= cap / 8; /* headroom for the larger Darwin records */
    long ret = sys3(SYS_getdents64, fd, (long)tmp, cap);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t used = 0;
    long off = 0;
    int64_t last_off = 0;
    while (off < ret) {
        uint8_t *d = tmp + off;
        uint64_t ino = *(uint64_t *)d;
        int64_t doff = *(int64_t *)(d + 8);
        uint16_t lrec = *(uint16_t *)(d + 16);
        uint8_t type = d[18];
        const char *name = (const char *)(d + 19);
        size_t nlen = 0; while (name[nlen]) nlen++;
        size_t rec = (21 + nlen + 1 + 7) & ~(size_t)7;
        if (used + rec > bufsize) { sys3(SYS_lseek, fd, last_off, 0); break; }
        uint8_t *o = out + used;
        for (size_t k = 0; k < rec; k++) o[k] = 0;
        *(uint64_t *)o = ino;
        *(uint64_t *)(o + 8) = (uint64_t)doff;
        *(uint16_t *)(o + 16) = (uint16_t)rec;
        *(uint16_t *)(o + 18) = (uint16_t)nlen;
        o[20] = type; /* DT_* values are the same on Linux and Darwin */
        for (size_t k = 0; k < nlen; k++) o[21 + k] = (uint8_t)name[k];
        used += rec;
        last_off = doff;
        off += lrec;
    }
    if (basep) *basep = last_off;
    return (int)used;
}

/* kdebug traces (no-op) */
int lsk_kdebug_trace(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) __asm__("_kdebug_trace");
int lsk_kdebug_trace(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    (void)code; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return 0;
}

int lsk_kdebug_trace_m(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) __asm__("___kdebug_trace");
int lsk_kdebug_trace_m(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    return 0;
}

int lsk_kdebug_trace64(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) __asm__("_kdebug_trace64");
int lsk_kdebug_trace64(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    (void)code; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return 0;
}

int lsk_kdebug_trace64_m(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) __asm__("___kdebug_trace64");
int lsk_kdebug_trace64_m(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    return 0;
}

int lsk_kdebug_trace_string(uint32_t code, uint64_t str_id, const char *str) __asm__("_kdebug_trace_string");
int lsk_kdebug_trace_string(uint32_t code, uint64_t str_id, const char *str) {
    (void)code; (void)str_id; (void)str;
    return 0;
}

int lsk_kdebug_trace_string_m(uint32_t code, uint64_t str_id, const char *str) __asm__("___kdebug_trace_string");
int lsk_kdebug_trace_string_m(uint32_t code, uint64_t str_id, const char *str) {
    return 0;
}

int lsk_kdebug_typefilter(void *addr, size_t size) __asm__("_kdebug_typefilter");
int lsk_kdebug_typefilter(void *addr, size_t size) {
    (void)addr; (void)size;
    return 0;
}

int lsk_kdebug_typefilter_m(void *addr, size_t size) __asm__("___kdebug_typefilter");
int lsk_kdebug_typefilter_m(void *addr, size_t size) {
    return 0;
}

uint32_t lsk_kdebug_is_enabled(uint32_t debugid) __asm__("_kdebug_is_enabled");
uint32_t lsk_kdebug_is_enabled(uint32_t debugid) {
    (void)debugid;
    return 0;
}

int lsk_kdebug_signpost(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) __asm__("_kdebug_signpost");
int lsk_kdebug_signpost(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
    (void)code; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return 0;
}

int lsk_kdebug_signpost_start(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) __asm__("_kdebug_signpost_start");
int lsk_kdebug_signpost_start(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
    (void)code; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return 0;
}

int lsk_kdebug_signpost_end(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) __asm__("_kdebug_signpost_end");
int lsk_kdebug_signpost_end(uint32_t code, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
    (void)code; (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return 0;
}

int lsk_kdebug_using_continuous_time(void) __asm__("_kdebug_using_continuous_time");
int lsk_kdebug_using_continuous_time(void) {
    return 0;
}

uint64_t lsk_kdebug_timestamp(void) __asm__("_kdebug_timestamp");
uint64_t lsk_kdebug_timestamp(void) {
    uint64_t val;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(val));
    return val;
}

uint64_t lsk_kdebug_timestamp_from_absolute(uint64_t abs) __asm__("_kdebug_timestamp_from_absolute");
uint64_t lsk_kdebug_timestamp_from_absolute(uint64_t abs) {
    return abs;
}

uint64_t lsk_kdebug_timestamp_from_continuous(uint64_t cont) __asm__("_kdebug_timestamp_from_continuous");
uint64_t lsk_kdebug_timestamp_from_continuous(uint64_t cont) {
    return cont;
}

/* Termination */
void lsk_abort_with_payload(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                            uint32_t payload_size, const char *reason_string, uint64_t reason_flags) __asm__("_abort_with_payload");
void lsk_abort_with_payload(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                            uint32_t payload_size, const char *reason_string, uint64_t reason_flags) {
    (void)reason_namespace; (void)reason_code; (void)payload; (void)payload_size; (void)reason_flags;
    static const char msg[] = "[lsk] abort_with_payload: ";
    sys_write(2, msg, sizeof(msg) - 1);
    if (reason_string) {
        sys_write(2, reason_string, mystrlen(reason_string));
    }
    sys_write(2, "\n", 1);
    sys_exit_group(134);
    while (1) {}
}

void lsk_abort_with_payload_m(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                              uint32_t payload_size, const char *reason_string, uint64_t reason_flags) __asm__("___abort_with_payload");
void lsk_abort_with_payload_m(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                              uint32_t payload_size, const char *reason_string, uint64_t reason_flags) {
    lsk_abort_with_payload(reason_namespace, reason_code, payload, payload_size, reason_string, reason_flags);
}

void lsk_abort_with_reason(uint32_t reason_namespace, uint64_t reason_code, const char *reason_string, uint64_t reason_flags) __asm__("_abort_with_reason");
void lsk_abort_with_reason(uint32_t reason_namespace, uint64_t reason_code, const char *reason_string, uint64_t reason_flags) {
    lsk_abort_with_payload(reason_namespace, reason_code, 0, 0, reason_string, reason_flags);
}

void lsk_terminate_with_payload(int pid, uint32_t reason_namespace, uint64_t reason_code,
                                void *payload, uint32_t payload_size, const char *reason_string, uint64_t reason_flags) __asm__("_terminate_with_payload");
void lsk_terminate_with_payload(int pid, uint32_t reason_namespace, uint64_t reason_code,
                                void *payload, uint32_t payload_size, const char *reason_string, uint64_t reason_flags) {
    (void)pid;
    lsk_abort_with_payload(reason_namespace, reason_code, payload, payload_size, reason_string, reason_flags);
}

void lsk_terminate_with_payload_m(int pid, uint32_t reason_namespace, uint64_t reason_code,
                                  void *payload, uint32_t payload_size, const char *reason_string, uint64_t reason_flags) __asm__("___terminate_with_payload");
void lsk_terminate_with_payload_m(int pid, uint32_t reason_namespace, uint64_t reason_code,
                                  void *payload, uint32_t payload_size, const char *reason_string, uint64_t reason_flags) {
    (void)pid;
    lsk_abort_with_payload(reason_namespace, reason_code, payload, payload_size, reason_string, reason_flags);
}

void lsk_os_fault_with_payload(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                               uint32_t payload_size, const char *reason_string, uint64_t reason_flags) __asm__("_os_fault_with_payload");
void lsk_os_fault_with_payload(uint32_t reason_namespace, uint64_t reason_code, void *payload,
                               uint32_t payload_size, const char *reason_string, uint64_t reason_flags) {
    lsk_abort_with_payload(reason_namespace, reason_code, payload, payload_size, reason_string, reason_flags);
}

void lsk_exit(int status) __asm__("_exit");
void lsk_exit(int status) {
    sys_exit_group(status);
}

void lsk_exit_c(int status) __asm__("___exit");
void lsk_exit_c(int status) {
    sys_exit_group(status);
}

void lsk_exit_2(int status) __asm__("__exit");
void lsk_exit_2(int status) {
    sys_exit_group(status);
}

int lsk_kill(int pid, int sig) __asm__("_kill");
int lsk_kill(int pid, int sig) {
    long ret = sys2(SYS_kill, pid, sig);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_kill_c(int pid, int sig) __asm__("___kill");
int lsk_kill_c(int pid, int sig) {
    return lsk_kill(pid, sig);
}

int lsk_sigprocmask(int how, const void *set, void *oset) __asm__("_sigprocmask");
int lsk_sigprocmask(int how, const void *set, void *oset) {
    /* Same translation as __pthread_sigmask (thread.c); it returns an errno. */
    extern int lsk_pthread_sigmask(int how, const void *set, void *oset) __asm__("___pthread_sigmask");
    int rc = lsk_pthread_sigmask(how, set, oset);
    if (rc) { set_errno(rc); return -1; }
    return 0;
}

int lsk_sigaction(int sig, const void *act, void *oact) __asm__("_sigaction");
int lsk_sigaction(int sig, const void *act, void *oact) {
    (void)sig; (void)act; (void)oact;
    return 0;
}

int lsk_sigaction_c(int sig, const void *act, void *oact) __asm__("___sigaction");
int lsk_sigaction_c(int sig, const void *act, void *oact) {
    return lsk_sigaction(sig, act, oact);
}

int lsk_sigaltstack(const void *ss, void *oss) __asm__("_sigaltstack");
int lsk_sigaltstack(const void *ss, void *oss) {
    (void)ss; (void)oss;
    return 0;
}

int lsk_sigaltstack_m(const void *ss, void *oss) __asm__("___sigaltstack");
int lsk_sigaltstack_m(const void *ss, void *oss) {
    return lsk_sigaltstack(ss, oss);
}

/* ---------- MAC / Sandbox ---------- */

int lsk_mac_syscall(const char *policy, int call, void *arg) __asm__("___mac_syscall");
int lsk_mac_syscall(const char *policy, int call, void *arg) {
    (void)call; (void)arg;
    return 0;
}

int lsk_sandbox_ms(const char *policy, int call, void *arg) __asm__("___sandbox_ms");
int lsk_sandbox_ms(const char *policy, int call, void *arg) {
    return lsk_mac_syscall(policy, call, arg);
}

int lsk_mac_execve(const char *fname, char **argv, char **envv, void *mac_p) __asm__("___mac_execve");
int lsk_mac_execve(const char *fname, char **argv, char **envv, void *mac_p) {
    (void)fname; (void)argv; (void)envv; (void)mac_p;
    set_errno(45); /* ENOTSUP */
    return -1;
}

int lsk_sandbox_me(const char *fname, char **argv, char **envv, void *mac_p) __asm__("___sandbox_me");
int lsk_sandbox_me(const char *fname, char **argv, char **envv, void *mac_p) {
    return lsk_mac_execve(fname, argv, envv, mac_p);
}

int lsk_mac_get_fd(int fd, void *mac_p) __asm__("___mac_get_fd");
int lsk_mac_get_fd(int fd, void *mac_p) {
    (void)fd; (void)mac_p;
    return 0;
}

int lsk_mac_get_file(const char *path_p, void *mac_p) __asm__("___mac_get_file");
int lsk_mac_get_file(const char *path_p, void *mac_p) {
    (void)path_p; (void)mac_p;
    return 0;
}

int lsk_mac_get_link(const char *path_p, void *mac_p) __asm__("___mac_get_link");
int lsk_mac_get_link(const char *path_p, void *mac_p) {
    (void)path_p; (void)mac_p;
    return 0;
}

int lsk_mac_get_mount(const char *path_p, void *mac_p) __asm__("___mac_get_mount");
int lsk_mac_get_mount(const char *path_p, void *mac_p) {
    (void)path_p; (void)mac_p;
    return 0;
}

int lsk_mac_get_pid(int pid, void *mac_p) __asm__("___mac_get_pid");
int lsk_mac_get_pid(int pid, void *mac_p) {
    (void)pid; (void)mac_p;
    return 0;
}

int lsk_mac_get_proc(void *mac_p) __asm__("___mac_get_proc");
int lsk_mac_get_proc(void *mac_p) {
    (void)mac_p;
    return 0;
}

int lsk_mac_getfsstat(void *buf, size_t bufsize, void *mac_p, int flags) __asm__("___mac_getfsstat");
int lsk_mac_getfsstat(void *buf, size_t bufsize, void *mac_p, int flags) {
    (void)buf; (void)bufsize; (void)mac_p; (void)flags;
    return 0;
}

int lsk_mac_mount(const char *type, const char *path, int flags, void *data, void *mac_p) __asm__("___mac_mount");
int lsk_mac_mount(const char *type, const char *path, int flags, void *data, void *mac_p) {
    (void)type; (void)path; (void)flags; (void)data; (void)mac_p;
    set_errno(45);
    return -1;
}

int lsk_sandbox_mm(const char *type, const char *path, int flags, void *data, void *mac_p) __asm__("___sandbox_mm");
int lsk_sandbox_mm(const char *type, const char *path, int flags, void *data, void *mac_p) {
    return lsk_mac_mount(type, path, flags, data, mac_p);
}

int lsk_mac_set_fd(int fd, void *mac_p) __asm__("___mac_set_fd");
int lsk_mac_set_fd(int fd, void *mac_p) {
    (void)fd; (void)mac_p;
    return 0;
}

int lsk_mac_set_file(const char *path_p, void *mac_p) __asm__("___mac_set_file");
int lsk_mac_set_file(const char *path_p, void *mac_p) {
    (void)path_p; (void)mac_p;
    return 0;
}

int lsk_mac_set_link(const char *path_p, void *mac_p) __asm__("___mac_set_link");
int lsk_mac_set_link(const char *path_p, void *mac_p) {
    (void)path_p; (void)mac_p;
    return 0;
}

int lsk_mac_set_proc(void *mac_p) __asm__("___mac_set_proc");
int lsk_mac_set_proc(void *mac_p) {
    (void)mac_p;
    return 0;
}

int lsk_sandbox_msp(void *mac_p) __asm__("___sandbox_msp");
int lsk_sandbox_msp(void *mac_p) {
    return lsk_mac_set_proc(mac_p);
}

/* ---------- Memorystatus ---------- */

int lsk_memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize) __asm__("_memorystatus_control");
int lsk_memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize) {
    (void)command; (void)pid; (void)flags; (void)buffer; (void)buffersize;
    return 0;
}

uint64_t lsk_memorystatus_available_memory(void) __asm__("___memorystatus_available_memory");
uint64_t lsk_memorystatus_available_memory(void) {
    return 4ULL * 1024 * 1024 * 1024;
}

int lsk_memorystatus_get_level(uint32_t *level) __asm__("_memorystatus_get_level");
int lsk_memorystatus_get_level(uint32_t *level) {
    if (level) *level = 0;
    return 0;
}

/* ---------- Coalition ---------- */

int lsk_coalition_create(uint64_t *cid_out, uint32_t flags) __asm__("_coalition_create");
int lsk_coalition_create(uint64_t *cid_out, uint32_t flags) {
    (void)flags;
    if (cid_out) *cid_out = 1;
    return 0;
}

int lsk_coalition_terminate(uint64_t cid, uint32_t flags) __asm__("_coalition_terminate");
int lsk_coalition_terminate(uint64_t cid, uint32_t flags) {
    (void)cid; (void)flags;
    return 0;
}

int lsk_coalition_reap(uint64_t cid, uint32_t flags) __asm__("_coalition_reap");
int lsk_coalition_reap(uint64_t cid, uint32_t flags) {
    (void)cid; (void)flags;
    return 0;
}

int lsk_coalition(uint32_t operation, uint64_t *cid, uint32_t flags) __asm__("___coalition");
int lsk_coalition(uint32_t operation, uint64_t *cid, uint32_t flags) {
    (void)operation; (void)flags;
    if (cid && *cid == 0) *cid = 1;
    return 0;
}

int lsk_coalition_info(uint32_t flavor, uint64_t *cid, void *buffer, size_t *bufsize) __asm__("___coalition_info");
int lsk_coalition_info(uint32_t flavor, uint64_t *cid, void *buffer, size_t *bufsize) {
    (void)flavor; (void)cid; (void)buffer; (void)bufsize;
    return 0;
}

int lsk_coalition_ledger(uint32_t operation, uint64_t cid, void *param) __asm__("___coalition_ledger");
int lsk_coalition_ledger(uint32_t operation, uint64_t cid, void *param) {
    (void)operation; (void)cid; (void)param;
    return 0;
}

int lsk_proc_listcoalitions(int flavor, int type, void *buffer, size_t buffersize) __asm__("_proc_listcoalitions");
int lsk_proc_listcoalitions(int flavor, int type, void *buffer, size_t buffersize) {
    (void)flavor; (void)type; (void)buffer; (void)buffersize;
    return 0;
}

int lsk_posix_spawnattr_setcoalition_np(void *attr, uint64_t cid, int type, int role) __asm__("_posix_spawnattr_setcoalition_np");
int lsk_posix_spawnattr_setcoalition_np(void *attr, uint64_t cid, int type, int role) {
    (void)attr; (void)cid; (void)type; (void)role;
    return 0;
}

int lsk_kpersona_pidinfo(int pid, void *info) __asm__("_kpersona_pidinfo");
int lsk_kpersona_pidinfo(int pid, void *info) {
    (void)pid;
    (void)info;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_info(uint64_t id, void *info) __asm__("_kpersona_info");
int lsk_kpersona_info(uint64_t id, void *info) {
    (void)id;
    (void)info;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_get(void *id) __asm__("_kpersona_get");
int lsk_kpersona_get(void *id) {
    (void)id;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_find(void *info, size_t *idcnt, void *idlist) __asm__("_kpersona_find");
int lsk_kpersona_find(void *info, size_t *idcnt, void *idlist) {
    (void)info; (void)idcnt; (void)idlist;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_find_by_type(int type, size_t *idcnt, void *idlist) __asm__("_kpersona_find_by_type");
int lsk_kpersona_find_by_type(int type, size_t *idcnt, void *idlist) {
    (void)type; (void)idcnt; (void)idlist;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_alloc(void *info, void *id) __asm__("_kpersona_alloc");
int lsk_kpersona_alloc(void *info, void *id) {
    (void)info; (void)id;
    set_errno(1); /* EPERM */
    return -1;
}

int lsk_kpersona_palloc(void *info, void *id) __asm__("_kpersona_palloc");
int lsk_kpersona_palloc(void *info, void *id) {
    (void)info; (void)id;
    set_errno(1); /* EPERM */
    return -1;
}

int lsk_kpersona_dealloc(uint64_t id) __asm__("_kpersona_dealloc");
int lsk_kpersona_dealloc(uint64_t id) {
    (void)id;
    set_errno(3); /* ESRCH */
    return -1;
}

int lsk_kpersona_getpath(uint64_t id, char *path, size_t *pathlen) __asm__("_kpersona_getpath");
int lsk_kpersona_getpath(uint64_t id, char *path, size_t *pathlen) {
    (void)id; (void)path; (void)pathlen;
    set_errno(3); /* ESRCH */
    return -1;
}

struct darwin_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

static int darwin_to_linux_rlimit(int which) {
    which &= 0xff;
    switch (which) {
        case 0: return 0; /* RLIMIT_CPU */
        case 1: return 1; /* RLIMIT_FSIZE */
        case 2: return 2; /* RLIMIT_DATA */
        case 3: return 3; /* RLIMIT_STACK */
        case 4: return 4; /* RLIMIT_CORE */
        case 5: return 9; /* RLIMIT_AS */
        case 6: return 8; /* RLIMIT_MEMLOCK */
        case 7: return 6; /* RLIMIT_NPROC */
        case 8: return 7; /* RLIMIT_NOFILE */
        default: return which;
    }
}

int lsk_getrlimit(int which, struct darwin_rlimit *rlp) __asm__("_getrlimit");
int lsk_getrlimit(int which, struct darwin_rlimit *rlp) {
    if (!rlp) {
        set_errno(14); /* EFAULT */
        return -1;
    }
    int lres = darwin_to_linux_rlimit(which);
    long ret = sys4(SYS_prlimit64, 0, lres, 0, (long)rlp);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    if (rlp->rlim_cur == ~0ULL) rlp->rlim_cur = 0x7fffffffffffffffULL;
    if (rlp->rlim_max == ~0ULL) rlp->rlim_max = 0x7fffffffffffffffULL;
    return 0;
}

int lsk_getrlimit_m(int which, struct darwin_rlimit *rlp) __asm__("___getrlimit");
int lsk_getrlimit_m(int which, struct darwin_rlimit *rlp) {
    return lsk_getrlimit(which, rlp);
}

int lsk_setrlimit(int which, const struct darwin_rlimit *rlp) __asm__("_setrlimit");
int lsk_setrlimit(int which, const struct darwin_rlimit *rlp) {
    if (!rlp) {
        set_errno(14); /* EFAULT */
        return -1;
    }
    struct darwin_rlimit lrl = *rlp;
    if (lrl.rlim_cur == 0x7fffffffffffffffULL) lrl.rlim_cur = ~0ULL;
    if (lrl.rlim_max == 0x7fffffffffffffffULL) lrl.rlim_max = ~0ULL;
    int lres = darwin_to_linux_rlimit(which);
    long ret = sys4(SYS_prlimit64, 0, lres, (long)&lrl, 0);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_setrlimit_m(int which, const struct darwin_rlimit *rlp) __asm__("___setrlimit");
int lsk_setrlimit_m(int which, const struct darwin_rlimit *rlp) {
    return lsk_setrlimit(which, rlp);
}

int lsk_proc_rlimit_control(int pid, int flavor, void *arg) __asm__("_proc_rlimit_control");
int lsk_proc_rlimit_control(int pid, int flavor, void *arg) {
    (void)pid; (void)flavor; (void)arg;
    return 0;
}

int lsk_proc_name(int pid, void *buffer, uint32_t buffersize) __asm__("_proc_name");
int lsk_proc_name(int pid, void *buffer, uint32_t buffersize) {
    (void)pid;
    const char name[] = "ioswine";
    uint32_t len = 7;
    if (!buffer || buffersize == 0) return 0;
    if (buffersize <= len) len = buffersize - 1;
    for (uint32_t i = 0; i < len; i++) {
        ((char *)buffer)[i] = name[i];
    }
    ((char *)buffer)[len] = '\0';
    return len;
}


/* statfs / fstatfs: Linux struct statfs (arm64) -> Darwin struct statfs (64-bit inode variant, 2168 bytes). */
static void lsk_fill_darwin_statfs(const uint64_t *ls, uint8_t *d, const char *path) {
    /* Linux arm64 statfs: f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_fsid(2x i32), f_namelen,
     * f_frsize, f_flags, spare[4]  (all 64-bit words except fsid) */
    for (int i = 0; i < 2168; i++) d[i] = 0;
    *(uint32_t *)(d + 0) = (uint32_t)ls[1];          /* f_bsize */
    *(int32_t *)(d + 4) = (int32_t)ls[1];            /* f_iosize */
    *(uint64_t *)(d + 8) = ls[2];                    /* f_blocks */
    *(uint64_t *)(d + 16) = ls[3];                   /* f_bfree */
    *(uint64_t *)(d + 24) = ls[4];                   /* f_bavail */
    *(uint64_t *)(d + 32) = ls[5];                   /* f_files */
    *(uint64_t *)(d + 40) = ls[6];                   /* f_ffree */
    *(uint64_t *)(d + 48) = ls[7];                   /* f_fsid */
    *(uint32_t *)(d + 56) = 501;                     /* f_owner */
    *(uint32_t *)(d + 60) = 0x1c;                    /* f_type: apfs-like */
    *(uint32_t *)(d + 64) = 0x00001080;              /* f_flags: MNT_LOCAL | MNT_CPROTECT */
    *(uint32_t *)(d + 68) = 0;                       /* f_fssubtype */
    const char *fst = "apfs";
    for (int i = 0; fst[i]; i++) d[72 + i] = (uint8_t)fst[i];   /* f_fstypename[16] */
    const char *mnt = "/";
    (void)path;
    d[88] = (uint8_t)mnt[0];                                       /* f_mntonname[1024] */
    const char *from = "/dev/disk1s1";
    for (int i = 0; from[i]; i++) d[1112 + i] = (uint8_t)from[i];   /* f_mntfromname[1024] */
}

int lsk_statfs(const char *path, void *buf) __asm__("_statfs");
int lsk_statfs(const char *path, void *buf) {
    char rbuf[LSK_PATH_MAX];
    path = lsk_path_remap(path, rbuf, sizeof(rbuf));
    uint64_t ls[15];
    long r = sys2(SYS_statfs, (long)path, (long)ls);
    if (r < 0) { set_errno(linux_to_darwin_errno(r)); return -1; }
    lsk_fill_darwin_statfs(ls, (uint8_t *)buf, path);
    return 0;
}
int lsk_statfs64(const char *path, void *buf) __asm__("_statfs64");
int lsk_statfs64(const char *path, void *buf) { return lsk_statfs(path, buf); }

int lsk_fstatfs(int fd, void *buf) __asm__("_fstatfs");
int lsk_fstatfs(int fd, void *buf) {
    uint64_t ls[15];
    long r = sys2(SYS_fstatfs, fd, (long)ls);
    if (r < 0) { set_errno(linux_to_darwin_errno(r)); return -1; }
    lsk_fill_darwin_statfs(ls, (uint8_t *)buf, 0);
    return 0;
}
int lsk_fstatfs64(int fd, void *buf) __asm__("_fstatfs64");
int lsk_fstatfs64(int fd, void *buf) { return lsk_fstatfs(fd, buf); }

int lsk_statfs_ext(const char *path, void *buf, void *ext, unsigned int flags) __asm__("_statfs_ext");
int lsk_statfs_ext(const char *path, void *buf, void *ext, unsigned int flags) {
    (void)ext; (void)flags;
    return lsk_statfs(path, buf);
}

int lsk_fstatfs_ext(int fd, void *buf, void *ext, unsigned int flags) __asm__("_fstatfs_ext");
int lsk_fstatfs_ext(int fd, void *buf, void *ext, unsigned int flags) {
    (void)ext; (void)flags;
    return lsk_fstatfs(fd, buf);
}

/* I/O policy: report IOPOL_DEFAULT (0), accept any set. */
int lsk_getiopolicy_np(int iotype, int scope) __asm__("_getiopolicy_np");
int lsk_getiopolicy_np(int iotype, int scope) { (void)iotype; (void)scope; return 0; }
int lsk_setiopolicy_np(int iotype, int scope, int policy) __asm__("_setiopolicy_np");
int lsk_setiopolicy_np(int iotype, int scope, int policy) { (void)iotype; (void)scope; (void)policy; return 0; }

/* Guarded fd variants (xnu guarded fds): guards are not enforced here, forward to the plain calls. */
extern int lsk_close_for_guarded(int fd) __asm__("_close");
int lsk_guarded_close_np(int fd, const void *guard) __asm__("_guarded_close_np");
int lsk_guarded_close_np(int fd, const void *guard) { (void)guard; return lsk_close_for_guarded(fd); }
ssize_t lsk_guarded_write_np(int fd, const void *guard, const void *buf, size_t n) __asm__("_guarded_write_np");
ssize_t lsk_guarded_write_np(int fd, const void *guard, const void *buf, size_t n) { (void)guard; return lsk_write(fd, buf, n); }
ssize_t lsk_guarded_pwrite_np(int fd, const void *guard, const void *buf, size_t n, int64_t off) __asm__("_guarded_pwrite_np");
ssize_t lsk_guarded_pwrite_np(int fd, const void *guard, const void *buf, size_t n, int64_t off) { (void)guard; return lsk_pwrite(fd, buf, n, off); }
ssize_t lsk_guarded_writev_np(int fd, const void *guard, const void *iov, int cnt) __asm__("_guarded_writev_np");
ssize_t lsk_guarded_writev_np(int fd, const void *guard, const void *iov, int cnt) { (void)guard; return lsk_writev(fd, iov, cnt); }
int lsk_change_fdguard_np(int fd, const void *g, unsigned gf, const void *ng, unsigned ngf, int *fdflagsp) __asm__("_change_fdguard_np");
int lsk_change_fdguard_np(int fd, const void *g, unsigned gf, const void *ng, unsigned ngf, int *fdflagsp) { (void)fd; (void)g; (void)gf; (void)ng; (void)ngf; (void)fdflagsp; return 0; }
