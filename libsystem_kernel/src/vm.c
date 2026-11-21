#include "linux.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

#define KERN_SUCCESS          0
#define KERN_INVALID_ADDRESS  1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE         3
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE          5

#define VM_FLAGS_FIXED        0x0000
#define VM_FLAGS_ANYWHERE     0x0001
#define VM_FLAGS_OVERWRITE    0x4000

#define VM_PROT_NONE          0x00
#define VM_PROT_READ          0x01
#define VM_PROT_WRITE         0x02
#define VM_PROT_EXECUTE       0x04
#define VM_PROT_ALL           (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)

/* Size invariant: the iOS page is 16 KiB, so every VM
 * size in this file is rounded up to a multiple of 16 KiB, on both
 * the allocate and the free side. A free must unmap exactly what its
 * allocate mapped: the old code rounded allocate to 16 KiB but
 * deallocate/munmap to 4 KiB, so every alloc/free cycle whose size
 * was not a multiple of 16 KiB leaked the tail (up to 12 KiB) of the
 * mapping forever, and the hint below marched on so the leaked range
 * was never reused. That leak is what kills the process: the guest
 * malloc (xzone, 4 MiB segments + 512 KiB metadata slabs) and the
 * IPC out-of-line path allocate and free constantly, and once enough
 * address space / RSS has leaked away, the next segment allocation
 * fails and libmalloc traps in __xzm_segment_group_alloc_segment. */

#define VM_PAGE_ROUND_UP(x)   (((x) + 16383ULL) & ~16383ULL)

/* Hint pointer for anywhere-allocations: keep allocations in [0x500000000, VM_USER_TOP = 0xFC0000000)
 * well away from shared cache (0x180000000..0x2C2544000), our dylib (0x300000000), and loader (0x7000000000).
 */
/* iOS user VA ends at 0xFC0000000 (MACH_VM_MAX_ADDRESS); libmalloc xzone traps on
 * segments at or above 1<<36 and indexes its segment table by address bits. */
#define VM_USER_TOP 0xFC0000000ULL
static uintptr_t g_mmap_hint = 0x500000000ULL;

static int vm_prot_to_linux(int prot) {
    int lp = 0;
    if (prot & VM_PROT_READ) lp |= PROT_READ;
    if (prot & VM_PROT_WRITE) lp |= PROT_WRITE;
    if (prot & VM_PROT_EXECUTE) lp |= PROT_EXEC;
    return lp;
}

static void *alloc_aligned_anywhere(size_t size, size_t align, int lprot) {
    if (align < 16384) align = 16384;
    /* Round size up to multiple of 16 KiB (see the invariant above). */
    size = (size_t)VM_PAGE_ROUND_UP(size);

    /* Primary path: place exactly `size` bytes at an aligned address
     * with MAP_FIXED_NOREPLACE, walking candidates from g_mmap_hint.
     * This never over-allocates and never leaves head/tail slivers:
     * the old oversize-mmap-and-trim path transiently mapped
     * size + align bytes (8 MiB for a 4 MiB xzone segment body with
     * a 4 MiB mask), which under launch-time memory pressure could
     * turn a satisfiable segment allocation into ENOMEM, and it left
     * the trimmed slivers unmapped only if both munmaps succeeded.
     * A candidate that is already occupied is skipped, never
     * clobbered; a non-EEXIST error (e.g. ENOMEM) falls through to
     * the fallback below instead of burning retries. */
    uintptr_t cand = (g_mmap_hint + (align - 1)) & ~(align - 1);
    for (int tries = 0; tries < 512; tries++) {
        if (cand >= VM_USER_TOP) cand = 0x500000000ULL;
        void *p = sys_mmap((void *)cand, size, lprot,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
        if ((uintptr_t)p < (uintptr_t)-4095) {
            g_mmap_hint = cand + size + 0x200000ULL;
            if (g_mmap_hint >= VM_USER_TOP) g_mmap_hint = 0x500000000ULL;
            return p;
        }
        if ((long)p != -17 /* EEXIST */) break;
        cand += align;
    }

    /* Fallback: the old oversize-mmap-and-trim path, for hosts where
     * the hint range is fragmented and no aligned candidate is free.
     * Kept as a best effort; the trim below unmaps exactly the head
     * and tail slivers, so the live mapping is still `size` bytes. */
    size_t alloc_sz = size + align;

    uintptr_t hint = g_mmap_hint;
    void *raw = sys_mmap((void *)hint, alloc_sz, lprot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uintptr_t)raw >= (uintptr_t)-4095) {
        raw = sys_mmap(0, alloc_sz, lprot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((uintptr_t)raw >= (uintptr_t)-4095) return 0;
    }

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (align - 1)) & ~(align - 1);
    if (aligned > addr) {
        sys_munmap((void *)addr, aligned - addr);
    }
    size_t tail = (addr + alloc_sz) - (aligned + size);
    if (tail > 0) {
        sys_munmap((void *)(aligned + size), tail);
    }

    g_mmap_hint = (aligned + size + 0x200000ULL);
    if (g_mmap_hint >= VM_USER_TOP) g_mmap_hint = 0x500000000ULL;

    return (void *)aligned;
}

#if defined(__aarch64__) && defined(__APPLE__)
extern const char *lsk_getenv(const char *name);
#define LSK_GETENV(n) lsk_getenv(n)
#else
extern const char *lsk_getenv(const char *name) __attribute__((weak));
#define LSK_GETENV(n) (lsk_getenv ? lsk_getenv(n) : 0)
#endif
static void mm_hex(char *b, int *n, uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) b[(*n)++] = "0123456789abcdef"[(v >> i) & 15];
}
/* IOSWINE_MMAP_TRACE=1: log every file mapping with its path. */
/* Fixed-address mapping that honours the Mach contract: VM_FLAGS_FIXED
 * without VM_FLAGS_OVERWRITE must fail (KERN_NO_SPACE) when the range
 * is already occupied, instead of silently replacing whatever lives
 * there (the old code used plain MAP_FIXED, which would have clobbered
 * the shared cache, our dylib, or a live xzone segment if a guest
 * caller ever asked for an occupied fixed address). Returns NULL on
 * failure and sets *err to the kern_return_t to report. */
static void *map_fixed_checked(uint64_t address, uint64_t size, int lprot,
                               int flags, int *err) {
    if ((address & 16383ULL) != 0) {
        *err = KERN_INVALID_ADDRESS;
        return NULL;
    }
    uint64_t rsize = VM_PAGE_ROUND_UP(size);
    void *p = sys_mmap((void *)address, (size_t)rsize, lprot,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if ((uintptr_t)p >= (uintptr_t)-4095) {
        if ((long)p == -17 /* EEXIST */ && (flags & VM_FLAGS_OVERWRITE)) {
            {
                static int on = -1;
                if (on < 0) { const char *e = LSK_GETENV("IOSWINE_MMAP_TRACE"); on = (e && e[0] == '1'); }
                if (on) {
                    char b[96]; int n = 0; const char *h = "[mmap] OVERWRITE 0x";
                    while (*h) b[n++] = *h++;
                    mm_hex(b, &n, address); h = " len=0x"; while (*h) b[n++] = *h++;
                    mm_hex(b, &n, rsize); b[n++] = '\n';
                    sys3(SYS_write, 2, (long)b, n);
                }
            }
            p = sys_mmap((void *)address, (size_t)rsize, lprot,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if ((uintptr_t)p >= (uintptr_t)-4095) {
                *err = KERN_NO_SPACE;
                return NULL;
            }
        } else {
            *err = KERN_NO_SPACE;
            return NULL;
        }
    }
    *err = KERN_SUCCESS;
    return p;
}

/* mach_vm_allocate */
int lsk_mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
int lsk_mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target;
    if (!address || size == 0) return KERN_INVALID_ARGUMENT;

    int lprot = PROT_READ | PROT_WRITE;
    if (flags & VM_FLAGS_ANYWHERE || *address == 0) {
        void *p = alloc_aligned_anywhere(size, 16384, lprot);
        if (!p) return KERN_NO_SPACE;
        *address = (uint64_t)p;
        return KERN_SUCCESS;
    } else {
        int err = KERN_SUCCESS;
        void *p = map_fixed_checked(*address, size, lprot, flags, &err);
        if (!p) return err;
        *address = (uint64_t)p;
        return KERN_SUCCESS;
    }
}

int lsk_krpc_mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("__kernelrpc_mach_vm_allocate");
int lsk_krpc_mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    return lsk_mach_vm_allocate(target, address, size, flags);
}

int lsk_krpc_mach_vm_allocate_trap(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("__kernelrpc_mach_vm_allocate_trap");
int lsk_krpc_mach_vm_allocate_trap(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    return lsk_mach_vm_allocate(target, address, size, flags);
}

/* mach_vm_deallocate */
int lsk_mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
int lsk_mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    (void)target;
    if (size == 0) return KERN_SUCCESS;
    /* Same 16 KiB rounding as the allocate side (see the invariant
     * above): unmapping fewer bytes than were mapped leaked the tail
     * of every non-16K-sized allocation. */
    uint64_t rsize = VM_PAGE_ROUND_UP(size);
    sys_munmap((void *)address, rsize);
    return KERN_SUCCESS;
}

int lsk_krpc_mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("__kernelrpc_mach_vm_deallocate");
int lsk_krpc_mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    return lsk_mach_vm_deallocate(target, address, size);
}

int lsk_krpc_mach_vm_deallocate_trap(uint32_t target, uint64_t address, uint64_t size) __asm__("__kernelrpc_mach_vm_deallocate_trap");
int lsk_krpc_mach_vm_deallocate_trap(uint32_t target, uint64_t address, uint64_t size) {
    return lsk_mach_vm_deallocate(target, address, size);
}

/* mach_vm_protect */
int lsk_mach_vm_protect(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) __asm__("_mach_vm_protect");
int lsk_mach_vm_protect(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) {
    (void)target;
    (void)set_maximum;
    int lp = vm_prot_to_linux(new_protection);
    uint64_t rsize = (size + 4095) & ~4095;
    long ret = sys_mprotect((void *)address, rsize, lp);
    return ret == 0 ? KERN_SUCCESS : KERN_PROTECTION_FAILURE;
}

int lsk_krpc_mach_vm_protect(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) __asm__("__kernelrpc_mach_vm_protect");
int lsk_krpc_mach_vm_protect(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) {
    return lsk_mach_vm_protect(target, address, size, set_maximum, new_protection);
}

int lsk_krpc_mach_vm_protect_trap(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) __asm__("__kernelrpc_mach_vm_protect_trap");
int lsk_krpc_mach_vm_protect_trap(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) {
    return lsk_mach_vm_protect(target, address, size, set_maximum, new_protection);
}

/* mach_vm_map */
int lsk_mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                    uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("_mach_vm_map");
int lsk_mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                    uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) {
    (void)target;
    (void)object;
    (void)offset;
    (void)copy;
    (void)max_prot;
    (void)inherit;

    if (!address || size == 0) return KERN_INVALID_ARGUMENT;

    int lp = vm_prot_to_linux(cur_prot);
    if (lp == 0) lp = PROT_READ | PROT_WRITE;

    size_t align = 16384;
    if (mask > 0x3fff) {
        align = (size_t)(mask + 1);
    }

    if ((flags & VM_FLAGS_ANYWHERE) || *address == 0) {
        void *p = alloc_aligned_anywhere(size, align, lp);
        if (!p) return KERN_NO_SPACE;
        *address = (uint64_t)p;
        return KERN_SUCCESS;
    } else {
        int err = KERN_SUCCESS;
        void *p = map_fixed_checked(*address, size, lp, flags, &err);
        if (!p) return err;
        *address = (uint64_t)p;
        return KERN_SUCCESS;
    }
}

int lsk_krpc_mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                         uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("__kernelrpc_mach_vm_map");
int lsk_krpc_mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                         uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) {
    return lsk_mach_vm_map(target, address, size, mask, flags, object, offset, copy, cur_prot, max_prot, inherit);
}

int lsk_krpc_mach_vm_map_trap(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                              uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("__kernelrpc_mach_vm_map_trap");
int lsk_krpc_mach_vm_map_trap(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                              uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) {
    return lsk_mach_vm_map(target, address, size, mask, flags, object, offset, copy, cur_prot, max_prot, inherit);
}

/* vm_allocate */
int lsk_vm_allocate(uint32_t target, uintptr_t *address, size_t size, int flags) __asm__("_vm_allocate");
int lsk_vm_allocate(uint32_t target, uintptr_t *address, size_t size, int flags) {
    uint64_t a = address ? (uint64_t)*address : 0;
    int ret = lsk_mach_vm_allocate(target, &a, size, flags);
    if (address) *address = (uintptr_t)a;
    return ret;
}

/* vm_deallocate */
int lsk_vm_deallocate(uint32_t target, uintptr_t address, size_t size) __asm__("_vm_deallocate");
int lsk_vm_deallocate(uint32_t target, uintptr_t address, size_t size) {
    return lsk_mach_vm_deallocate(target, address, size);
}

/* vm_protect */
int lsk_vm_protect(uint32_t target, uintptr_t address, size_t size, int set_maximum, int new_protection) __asm__("_vm_protect");
int lsk_vm_protect(uint32_t target, uintptr_t address, size_t size, int set_maximum, int new_protection) {
    return lsk_mach_vm_protect(target, address, size, set_maximum, new_protection);
}

/* vm_map */
int lsk_vm_map(uint32_t target, uintptr_t *address, size_t size, uintptr_t mask, int flags,
               uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("_vm_map");
int lsk_vm_map(uint32_t target, uintptr_t *address, size_t size, uintptr_t mask, int flags,
               uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) {
    uint64_t a = address ? (uint64_t)*address : 0;
    int ret = lsk_mach_vm_map(target, &a, size, mask, flags, object, offset, copy, cur_prot, max_prot, inherit);
    if (address) *address = (uintptr_t)a;
    return ret;
}

int lsk_krpc_vm_map(uint32_t target, uintptr_t *address, size_t size, uintptr_t mask, int flags,
                    uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("__kernelrpc_vm_map");
int lsk_krpc_vm_map(uint32_t target, uintptr_t *address, size_t size, uintptr_t mask, int flags,
                    uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) {
    return lsk_vm_map(target, address, size, mask, flags, object, offset, copy, cur_prot, max_prot, inherit);
}

/* mach_vm_region */
struct vm_region_basic_info_64 {
    int32_t  protection;
    int32_t  max_protection;
    int32_t  inheritance;
    uint32_t shared;
    uint32_t reserved;
    uint64_t offset;
    int32_t  behavior;
    uint16_t user_wired_count;
};

int lsk_mach_vm_region(uint32_t target, uint64_t *address, uint64_t *size, int flavor,
                       int *info, uint32_t *infoCnt, uint32_t *object_name) __asm__("_mach_vm_region");
int lsk_mach_vm_region(uint32_t target, uint64_t *address, uint64_t *size, int flavor,
                       int *info, uint32_t *infoCnt, uint32_t *object_name) {
    (void)target;
    (void)flavor;
    if (!address || !size) return KERN_INVALID_ARGUMENT;
    if (*address >= 0x7000000000ULL) return KERN_INVALID_ADDRESS;

    if (*address < 0x100000000ULL) {
        *address = 0x100000000ULL;
        *size = 0x10000ULL;
    } else {
        *size = 0x1000000ULL;
    }
    if (info && infoCnt && *infoCnt >= 8) {
        struct vm_region_basic_info_64 *b = (struct vm_region_basic_info_64 *)info;
        b->protection = VM_PROT_READ | VM_PROT_WRITE;
        b->max_protection = VM_PROT_ALL;
        b->inheritance = 1;
        b->shared = 0;
        b->reserved = 0;
        b->offset = 0;
        b->behavior = 0;
        b->user_wired_count = 0;
    }
    if (object_name) *object_name = 0;
    return KERN_SUCCESS;
}

int lsk_mach_vm_region_recurse(uint32_t target, uint64_t *address, uint64_t *size, uint32_t *nesting_depth,
                              int *info, uint32_t *infoCnt) __asm__("_mach_vm_region_recurse");
int lsk_mach_vm_region_recurse(uint32_t target, uint64_t *address, uint64_t *size, uint32_t *nesting_depth,
                              int *info, uint32_t *infoCnt) {
    uint32_t obj = 0;
    if (nesting_depth) *nesting_depth = 0;
    return lsk_mach_vm_region(target, address, size, 0, info, infoCnt, &obj);
}

int lsk_vm_region_64(uint32_t target, uintptr_t *address, size_t *size, int flavor,
                     int *info, uint32_t *infoCnt, uint32_t *object_name) __asm__("_vm_region_64");
int lsk_vm_region_64(uint32_t target, uintptr_t *address, size_t *size, int flavor,
                     int *info, uint32_t *infoCnt, uint32_t *object_name) {
    uint64_t a = address ? *address : 0;
    uint64_t s = size ? *size : 0;
    int ret = lsk_mach_vm_region(target, &a, &s, flavor, info, infoCnt, object_name);
    if (address) *address = (uintptr_t)a;
    if (size) *size = (size_t)s;
    return ret;
}

int lsk_vm_region_recurse_64(uint32_t target, uintptr_t *address, size_t *size, uint32_t *nesting_depth,
                            int *info, uint32_t *infoCnt) __asm__("_vm_region_recurse_64");
int lsk_vm_region_recurse_64(uint32_t target, uintptr_t *address, size_t *size, uint32_t *nesting_depth,
                            int *info, uint32_t *infoCnt) {
    uint64_t a = address ? *address : 0;
    uint64_t s = size ? *size : 0;
    int ret = lsk_mach_vm_region_recurse(target, &a, &s, nesting_depth, info, infoCnt);
    if (address) *address = (uintptr_t)a;
    if (size) *size = (size_t)s;
    return ret;
}

/* POSIX mmap */
static void mmap_trace(int fd, uint64_t len, int64_t off, int flags, int prot, void *r) {
    static int on = -1;
    if (on < 0) { const char *e = LSK_GETENV("IOSWINE_MMAP_TRACE"); on = (e && e[0] == '1'); }
    if (!on || fd < 0) return;
    char b[512]; int n = 0;
    const char *h = "[mmap] fd ";
    while (*h) b[n++] = *h++;
    char pth[32] = "/proc/self/fd/"; int k = 14;
    if (fd >= 100) pth[k++] = '0' + fd / 100;
    if (fd >= 10) pth[k++] = '0' + (fd / 10) % 10;
    pth[k++] = '0' + fd % 10; pth[k] = 0;
#ifdef SYS_readlinkat
    long l = sys4(SYS_readlinkat, -100, (long)pth, (long)(b + n), 300);
    if (l > 0) n += (int)l;
#endif
    h = " len=0x"; while (*h) b[n++] = *h++; mm_hex(b, &n, len);
    h = " off=0x"; while (*h) b[n++] = *h++; mm_hex(b, &n, (uint64_t)off);
    h = " fl=0x"; while (*h) b[n++] = *h++; mm_hex(b, &n, (uint64_t)flags);
    h = " pr=0x"; while (*h) b[n++] = *h++; mm_hex(b, &n, (uint64_t)prot);
    h = " -> 0x"; while (*h) b[n++] = *h++; mm_hex(b, &n, (uint64_t)r);
    b[n++] = '\n';
    sys3(SYS_write, 2, (long)b, n);
}

void *lsk_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) __asm__("_mmap");
void *lsk_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) {
    int lflags = 0;
    if (flags & 0x01) lflags |= MAP_SHARED;
    if (flags & 0x02) lflags |= MAP_PRIVATE;
    if (flags & 0x10) lflags |= MAP_FIXED;
    if (flags & 0x1000) lflags |= MAP_ANONYMOUS;

    if (addr == 0 && (lflags & MAP_ANONYMOUS)) {
        void *p = alloc_aligned_anywhere(len, 16384, prot);
        if (!p) {
            set_errno(12); /* ENOMEM */
            return (void *)-1;
        }
        return p;
    }

    /* Same 16 KiB rounding as the allocate side, so munmap below
     * unmaps exactly what was mapped here. */
    uint64_t rlen = VM_PAGE_ROUND_UP(len);
    if (!(lflags & MAP_FIXED)) {
        /* Linux would place a hint-less file mapping near 0x7f..., far above
         * the iOS user VA top. Reserve a slot inside the iOS range and map
         * the file over it. */
        void *slot = alloc_aligned_anywhere(rlen, 16384, PROT_NONE);
        if (slot) {
            void *r = sys_mmap(slot, rlen, prot, lflags | MAP_FIXED, fd, offset);
            mmap_trace(fd, len, offset, flags, prot, r);
            if ((uintptr_t)r < (uintptr_t)-4095) return r;
            sys_munmap(slot, rlen);
            set_errno(linux_to_darwin_errno((long)r));
            return (void *)-1;
        }
    }
    void *ret = sys_mmap(addr, rlen, prot, lflags, fd, offset);
    mmap_trace(fd, len, offset, flags, prot, ret);
    if ((uintptr_t)ret >= (uintptr_t)-4095) {
        set_errno(linux_to_darwin_errno((long)ret));
        return (void *)-1;
    }
    return ret;
}

void *lsk_mmap_m(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) __asm__("___mmap");
void *lsk_mmap_m(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) {
    return lsk_mmap(addr, len, prot, flags, fd, offset);
}

/* POSIX munmap */
int lsk_munmap(void *addr, size_t len) __asm__("_munmap");
int lsk_munmap(void *addr, size_t len) {
    uint64_t rlen = VM_PAGE_ROUND_UP(len);
    long ret = sys_munmap(addr, rlen);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_munmap_m(void *addr, size_t len) __asm__("___munmap");
int lsk_munmap_m(void *addr, size_t len) {
    return lsk_munmap(addr, len);
}

/* POSIX mprotect */
int lsk_mprotect(void *addr, size_t len, int prot) __asm__("_mprotect");
int lsk_mprotect(void *addr, size_t len, int prot) {
    uint64_t rlen = (len + 4095) & ~4095;
    long ret = sys_mprotect(addr, rlen, prot);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_mprotect_m(void *addr, size_t len, int prot) __asm__("___mprotect");
int lsk_mprotect_m(void *addr, size_t len, int prot) {
    return lsk_mprotect(addr, len, prot);
}

/* POSIX madvise */
int lsk_madvise(void *addr, size_t len, int advice) __asm__("_madvise");
int lsk_madvise(void *addr, size_t len, int advice) {
    uint64_t rlen = (len + 4095) & ~4095;
    /* Darwin and Linux advice numbers differ from 4 up: Darwin 8 (FREE_REUSE)
     * is Linux MADV_FREE and 9 (CAN_REUSE) is MADV_REMOVE. Translate. */
    int ladv;
    switch (advice) {
    case 0: case 1: case 2: case 3: ladv = advice; break;
    case 4:  /* Darwin MADV_DONTNEED keeps contents; Linux DONTNEED zeroes them. */
        return 0;
    case 5:  /* MADV_FREE */
    case 7:  /* MADV_FREE_REUSABLE */ ladv = 8; /* Linux MADV_FREE */ break;
    default: return 0; /* ZERO_WIRED, FREE_REUSE, CAN_REUSE, PAGEOUT, ...: no-op */
    }
    long ret = sys3(SYS_madvise, (long)addr, rlen, ladv);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

/* POSIX msync */
int lsk_msync(void *addr, size_t len, int flags) __asm__("_msync");
int lsk_msync(void *addr, size_t len, int flags) {
    uint64_t rlen = (len + 4095) & ~4095;
    long ret = sys3(SYS_msync, (long)addr, rlen, flags);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_msync_m(void *addr, size_t len, int flags) __asm__("___msync");
int lsk_msync_m(void *addr, size_t len, int flags) {
    return lsk_msync(addr, len, flags);
}

/* mach_vm_reclaim_* */
uint32_t lsk_mach_vm_reclaim_round_capacity(uint32_t num_entries) __asm__("_mach_vm_reclaim_round_capacity");
uint32_t lsk_mach_vm_reclaim_round_capacity(uint32_t num_entries) {
    if (num_entries > 0x7ffff8) return 0x7ffff8;
    uint64_t bytes = ((uint64_t)num_entries << 4) + 0x80;
    uint64_t mask = 0x3fff;
    bytes = (bytes + mask) & ~mask;
    bytes -= 0x80;
    uint64_t rounded = bytes >> 4;
    if ((bytes >> 36) != 0) return 0xffffffff;
    return (uint32_t)rounded;
}

int lsk_mach_vm_reclaim_ring_allocate(void *ring, uint32_t num_entries) __asm__("_mach_vm_reclaim_ring_allocate");
int lsk_mach_vm_reclaim_ring_allocate(void *ring, uint32_t num_entries) {
    (void)ring; (void)num_entries;
    return 46; /* KERN_NOT_SUPPORTED */
}

int lsk_mach_vm_reclaim_ring_resize(void *ring, uint32_t num_entries) __asm__("_mach_vm_reclaim_ring_resize");
int lsk_mach_vm_reclaim_ring_resize(void *ring, uint32_t num_entries) {
    (void)ring; (void)num_entries;
    return 46; /* KERN_NOT_SUPPORTED */
}

int lsk_mach_vm_reclaim_update_kernel_accounting(void *ring) __asm__("_mach_vm_reclaim_update_kernel_accounting");
int lsk_mach_vm_reclaim_update_kernel_accounting(void *ring) {
    (void)ring;
    return KERN_SUCCESS;
}

int lsk_mach_vm_reclaim_update_kernel_accounting_trap(void *ring) __asm__("_mach_vm_reclaim_update_kernel_accounting_trap");
int lsk_mach_vm_reclaim_update_kernel_accounting_trap(void *ring) {
    (void)ring;
    return KERN_SUCCESS;
}

/* POSIX mincore */
int lsk_mincore(void *addr, size_t len, char *vec) __asm__("_mincore");
int lsk_mincore(void *addr, size_t len, char *vec) {
    uint64_t rlen = (len + 4095) & ~4095;
    long ret = sys3(SYS_mincore, (long)addr, rlen, (long)vec);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

/* mach_vm_purgable_control */
int lsk_mach_vm_purgable_control(uint32_t target, uint64_t address, int control, int *state) __asm__("_mach_vm_purgable_control");
int lsk_mach_vm_purgable_control(uint32_t target, uint64_t address, int control, int *state) {
    (void)target;
    (void)address;
    (void)control;
    if (state) *state = 0;
    return KERN_SUCCESS;
}

int lsk_vm_purgable_control(uint32_t target, uintptr_t address, int control, int *state) __asm__("_vm_purgable_control");
int lsk_vm_purgable_control(uint32_t target, uintptr_t address, int control, int *state) {
    return lsk_mach_vm_purgable_control(target, address, control, state);
}

int lsk_vm_copy(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) __asm__("_vm_copy");
int lsk_vm_copy(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) {
    (void)target_task;
    char *dst = (char *)dest_address;
    const char *src = (const char *)source_address;
    if (dst < src) {
        for (uint64_t i = 0; i < size; i++) dst[i] = src[i];
    } else if (dst > src) {
        for (uint64_t i = size; i > 0; i--) dst[i - 1] = src[i - 1];
    }
    return KERN_SUCCESS;
}

int lsk_mach_vm_copy(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) __asm__("_mach_vm_copy");
int lsk_mach_vm_copy(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) {
    return lsk_vm_copy(target_task, source_address, size, dest_address);
}

/* Protection of the mapping that contains addr, from /proc/self/maps.
 * Returns a PROT_* mask, or -1 if not found. */
static int vm_query_prot(uint64_t addr) {
    long fd = sys4(SYS_openat, -100, (long)"/proc/self/maps", 0, 0);
    if (fd < 0) return -1;
    char buf[4096];
    char line[256];
    size_t ll = 0;
    int found = -1;
    for (;;) {
        long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n && found < 0; i++) {
            char c = buf[i];
            if (c != '\n') { if (ll < sizeof(line) - 1) line[ll++] = c; continue; }
            line[ll] = 0; ll = 0;
            uint64_t lo = 0, hi = 0; const char *q = line;
            while (*q && *q != '-') { lo = lo * 16 + (uint64_t)(*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
            if (*q == '-') q++;
            while (*q && *q != ' ') { hi = hi * 16 + (uint64_t)(*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
            if (*q == ' ') q++;
            if (addr >= lo && addr < hi && q[0] && q[1] && q[2]) {
                found = (q[0] == 'r' ? PROT_READ : 0) | (q[1] == 'w' ? PROT_WRITE : 0) |
                        (q[2] == 'x' ? PROT_EXEC : 0);
            }
        }
        if (found >= 0) break;
    }
    sys1(SYS_close, fd);
    return found;
}

/* mach_vm_remap and vm_remap */
int lsk_mach_vm_remap(uint32_t target_task,
                      uint64_t *target_address,
                      uint64_t size,
                      uint64_t mask,
                      int flags,
                      uint32_t src_task,
                      uint64_t src_address,
                      int copy,
                      int *cur_protection,
                      int *max_protection,
                      int inheritance) __asm__("_mach_vm_remap");
int lsk_mach_vm_remap(uint32_t target_task,
                      uint64_t *target_address,
                      uint64_t size,
                      uint64_t mask,
                      int flags,
                      uint32_t src_task,
                      uint64_t src_address,
                      int copy,
                      int *cur_protection,
                      int *max_protection,
                      int inheritance) {
    (void)target_task;
    (void)src_task;
    (void)copy;
    (void)inheritance;
    if (!target_address || size == 0 || !src_address) return KERN_INVALID_ARGUMENT;

    uint64_t rsize = (size + 4095) & ~4095;

    if ((flags & VM_FLAGS_ANYWHERE) || *target_address == 0) {
        size_t align = 16384;
        if (mask > 0x3fff) align = (size_t)(mask + 1);
        void *p = alloc_aligned_anywhere(size, align, PROT_READ | PROT_WRITE);
        if (!p) return KERN_NO_SPACE;
        *target_address = (uint64_t)p;
    }

    /* Target is mapped RW to copy source */
    sys_mprotect((void *)*target_address, rsize, PROT_READ | PROT_WRITE);
    char *dst = (char *)*target_address;
    const char *src = (const char *)src_address;
    for (uint64_t i = 0; i < size; i++) dst[i] = src[i];

    /* Flush cache (arm64 only; the host build has no such instructions
     * and the memcpy above is all it needs). */
#if defined(__aarch64__)
    uintptr_t addr = (uintptr_t)*target_address;
    uintptr_t end = addr + rsize;
    for (uintptr_t p = addr & ~63ULL; p < end; p += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t p = addr & ~63ULL; p < end; p += 64) {
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish; isb" ::: "memory");
#endif

    /* Keep the source's protection: trampoline pages stay R-X, data
     * moved by libmalloc stays R-W. */
    int sp = vm_query_prot(src_address);
    if (sp < 0) sp = PROT_READ | PROT_WRITE;
    int vp = ((sp & PROT_READ) ? VM_PROT_READ : 0) | ((sp & PROT_WRITE) ? VM_PROT_WRITE : 0) |
             ((sp & PROT_EXEC) ? VM_PROT_EXECUTE : 0);
    sys_mprotect((void *)*target_address, rsize, sp);

    if (cur_protection) *cur_protection = vp;
    if (max_protection) *max_protection = vp;

    return KERN_SUCCESS;
}

int lsk_vm_remap(uint32_t target_task,
                 uintptr_t *target_address,
                 size_t size,
                 uintptr_t mask,
                 int flags,
                 uint32_t src_task,
                 uintptr_t src_address,
                 int copy,
                 int *cur_protection,
                 int *max_protection,
                 int inheritance) __asm__("_vm_remap");
int lsk_vm_remap(uint32_t target_task,
                 uintptr_t *target_address,
                 size_t size,
                 uintptr_t mask,
                 int flags,
                 uint32_t src_task,
                 uintptr_t src_address,
                 int copy,
                 int *cur_protection,
                 int *max_protection,
                 int inheritance) {
    uint64_t a = target_address ? (uint64_t)*target_address : 0;
    int ret = lsk_mach_vm_remap(target_task, &a, size, mask, flags,
                                src_task, (uint64_t)src_address,
                                copy, cur_protection, max_protection, inheritance);
    if (target_address) *target_address = (uintptr_t)a;
    return ret;
}

int lsk_krpc_mach_vm_remap(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                           uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                           int inheritance) __asm__("__kernelrpc_mach_vm_remap");
int lsk_krpc_mach_vm_remap(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                           uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                           int inheritance) {
    return lsk_mach_vm_remap(target_task, target_address, size, mask, flags, src_task, src_address, copy, cur_protection, max_protection, inheritance);
}

int lsk_krpc_mach_vm_remap_trap(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                                uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                                int inheritance) __asm__("__kernelrpc_mach_vm_remap_trap");
int lsk_krpc_mach_vm_remap_trap(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                                uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                                int inheritance) {
    return lsk_mach_vm_remap(target_task, target_address, size, mask, flags, src_task, src_address, copy, cur_protection, max_protection, inheritance);
}

int lsk_krpc_vm_remap(uint32_t target_task, uintptr_t *target_address, size_t size, uintptr_t mask, int flags,
                      uint32_t src_task, uintptr_t src_address, int copy, int *cur_protection, int *max_protection,
                      int inheritance) __asm__("__kernelrpc_vm_remap");
int lsk_krpc_vm_remap(uint32_t target_task, uintptr_t *target_address, size_t size, uintptr_t mask, int flags,
                      uint32_t src_task, uintptr_t src_address, int copy, int *cur_protection, int *max_protection,
                      int inheritance) {
    return lsk_vm_remap(target_task, target_address, size, mask, flags, src_task, src_address, copy, cur_protection, max_protection, inheritance);
}

int lsk_vm_remap_new(uint32_t target_task, uintptr_t *target_address, size_t size, uintptr_t mask, int flags,
                     uint32_t src_task, uintptr_t src_address, int copy, int *cur_protection, int *max_protection,
                     int inheritance) __asm__("_vm_remap_new");
int lsk_vm_remap_new(uint32_t target_task, uintptr_t *target_address, size_t size, uintptr_t mask, int flags,
                     uint32_t src_task, uintptr_t src_address, int copy, int *cur_protection, int *max_protection,
                     int inheritance) {
    return lsk_vm_remap(target_task, target_address, size, mask, flags, src_task, src_address, copy, cur_protection, max_protection, inheritance);
}

int lsk_mach_vm_remap_new(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                          uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                          int inheritance) __asm__("_mach_vm_remap_new");
int lsk_mach_vm_remap_new(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask, int flags,
                          uint32_t src_task, uint64_t src_address, int copy, int *cur_protection, int *max_protection,
                          int inheritance) {
    return lsk_mach_vm_remap(target_task, target_address, size, mask, flags, src_task, src_address, copy, cur_protection, max_protection, inheritance);
}
