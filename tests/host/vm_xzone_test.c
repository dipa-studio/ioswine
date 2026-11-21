/*
 * vm_xzone_test.c -- host test for the libsystem_kernel VM layer
 * (libsystem_kernel/src/vm.c), addressing the xzone segment exhaustion defect.
 *
 * The defect: after ~70 s / 449 snapshot frames the app dies with
 * SIGTRAP in __xzm_segment_group_alloc_segment -- the guest malloc
 * (xzone) cannot allocate a new segment. xzone segments are 4 MiB
 * mach_vm_allocate'd ranges (slice 16 KiB) plus 512 KiB metadata
 * slabs mapped with an alignment mask; both go through this file's
 * mach_vm_allocate / mach_vm_map, and the IPC out-of-line path in
 * ipc.c allocates and frees constantly through the same entry
 * points. Three defects in vm.c conspired to exhaust the process:
 *
 *  1. Allocate rounded sizes to the 16 KiB iOS page but deallocate /
 *     munmap rounded to 4 KiB, so every alloc/free cycle whose size
 *     was not a multiple of 16 KiB leaked the tail (up to 12 KiB) of
 *     the mapping forever. The hint marched on, so the leaked range
 *     was never reused. (test_dealloc_leak, test_mmap_munmap_leak)
 *  2. The FIXED branch used plain MAP_FIXED, silently replacing
 *     whatever lived at the requested address instead of failing
 *     with KERN_NO_SPACE unless VM_FLAGS_OVERWRITE was set.
 *     (test_fixed_no_overwrite)
 *  3. Aligned allocation transiently mapped size + align bytes
 *     (8 MiB for a 4 MiB xzone segment body with a 4 MiB mask),
 *     which under launch-time memory pressure could turn a
 *     satisfiable segment allocation into ENOMEM. The new path
 *     places exactly `size` bytes with MAP_FIXED_NOREPLACE.
 *     (test_xzone_segment_pattern pins the exact live size)
 *
 * RED check: the leak and fixed-branch checks FAIL on the unpatched
 * tree. Baseline for the RED run is the pristine vm.c with only the
 * aarch64 cache-flush asm in lsk_mach_vm_remap guarded by
 * #if defined(__aarch64__) (a host-build enabler, not a behaviour
 * fix: the unpatched file does not compile on the x86_64 host at
 * all). All three behaviour fixes reverted, the leak checks report
 * 2,457,600 (200 x 0x3000) and 819,200 (100 x 0x2000) leaked bytes,
 * the roundtrip check reports 57,344, and the fixed-branch checks
 * report success-where-failure-was-required. The segment-pattern
 * live-size checks pass on the unpatched tree too (the old path
 * trimmed its slivers correctly); they pin the expected behavior, they are
 * not RED gates.
 *
 * The live-bytes probe reads /proc/self/maps and measures the mapped
 * bytes overlapping a specific [lo, hi) range (mapped_bytes_in_range).
 * It never assumes where the kernel placed a mapping: under ASan the
 * vm.c hint range is occupied by ASan's own reservation, so the
 * fallback path places mappings far outside it, and a hint-range
 * sum would read 0 vacuously. Every check is range-targeted, so it
 * is meaningful in both variants.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* vm.c entry points, by their assembler symbols (vm.c defines them
 * with __asm__ labels, e.g. _mach_vm_allocate). */
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
extern int mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                       uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("_mach_vm_map");
extern void *lsk_mmap_fn(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) __asm__("_mmap");
extern int lsk_munmap_fn(void *addr, size_t len) __asm__("_munmap");

/* errno.c is aarch64-only (tpidr_el0) and is not linked here; vm.c
 * only needs these two names for the mmap error path. */
void set_errno(int err) { (void)err; }
int linux_to_darwin_errno(long linux_err) {
    return linux_err < 0 ? (int)(-linux_err) : 0;
}

#define KERN_SUCCESS          0
#define KERN_INVALID_ADDRESS  1
#define KERN_NO_SPACE         3
#define KERN_INVALID_ARGUMENT 4

#define VM_FLAGS_FIXED        0x0000
#define VM_FLAGS_ANYWHERE     0x0001
#define VM_FLAGS_OVERWRITE    0x4000

#define VM_PROT_READ          0x01
#define VM_PROT_WRITE         0x02

/* Darwin mmap flags, as translated by lsk_mmap. */
#define DARWIN_MAP_SHARED     0x01
#define DARWIN_MAP_PRIVATE    0x02
#define DARWIN_MAP_ANONYMOUS  0x1000

/* Mapped bytes overlapping [lo, hi), from /proc/self/maps. */
static uint64_t mapped_bytes_in_range(uint64_t lo, uint64_t hi) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uint64_t total = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t start = 0, end = 0;
        if (sscanf(line, "%llx-%llx", (unsigned long long *)&start,
                   (unsigned long long *)&end) != 2) {
            continue;
        }
        uint64_t olo = start > lo ? start : lo;
        uint64_t ohi = end < hi ? end : hi;
        if (ohi > olo) total += (ohi - olo);
    }
    fclose(f);
    return total;
}

#define ROUND4(x)  (((x) + 4095ULL) & ~4095ULL)
#define ROUND16(x) (((x) + 16383ULL) & ~16383ULL)

static void test_dealloc_leak(void) {
    /* The IPC out-of-line pattern, at xzone-relevant scale: allocate
     * a non-16K size, free it with the same size, repeat. Allocate
     * maps ROUND16(0x4001) = 0x8000 bytes; on the unpatched tree
     * deallocate unmaps only ROUND4(0x4001) = 0x5000, so the tail
     * [addr+0x5000, addr+0x8000) stays mapped after every cycle. */
    uint64_t leaked = 0;
    for (int i = 0; i < 200; i++) {
        uint64_t addr = 0;
        int kr = mach_vm_allocate(0x103, &addr, 0x4001, VM_FLAGS_ANYWHERE);
        CHECK(kr == KERN_SUCCESS);
        CHECK(addr != 0);
        CHECK((addr & 16383ULL) == 0);
        if (addr) {
            *(volatile uint8_t *)addr = 0x5a;
            kr = mach_vm_deallocate(0x103, addr, 0x4001);
            CHECK(kr == KERN_SUCCESS);
            leaked += mapped_bytes_in_range(addr + ROUND4(0x4001),
                                            addr + ROUND16(0x4001));
        }
    }
    printf("dealloc leak: %llu bytes still mapped after 200 cycles\n",
           (unsigned long long)leaked);
    CHECK(leaked == 0);
}

static void test_mmap_munmap_leak(void) {
    /* POSIX pair, same rounding mismatch: mmap 0x1001 maps 0x4000
     * on the unpatched tree, munmap unmaps 0x2000. */
    uint64_t leaked = 0;
    for (int i = 0; i < 100; i++) {
        void *p = lsk_mmap_fn(NULL, 0x1001, VM_PROT_READ | VM_PROT_WRITE,
                              DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS, -1, 0);
        CHECK(p != NULL && p != (void *)-1);
        if (p && p != (void *)-1) {
            *(volatile uint8_t *)p = 0xa5;
            int rc = lsk_munmap_fn(p, 0x1001);
            CHECK(rc == 0);
            leaked += mapped_bytes_in_range((uint64_t)p + ROUND4(0x1001),
                                            (uint64_t)p + ROUND16(0x1001));
        }
    }
    printf("mmap leak: %llu bytes still mapped after 100 cycles\n",
           (unsigned long long)leaked);
    CHECK(leaked == 0);
}

static void test_xzone_segment_pattern(void) {
    /* The exact xzone pattern, from the open libmalloc-792 docs:
     * a 4 MiB segment body and a 512 KiB metadata slab, each mapped
     * with an alignment mask, plus a plain 4 MiB mach_vm_allocate.
     * The live mapping while held must be exactly the requested
     * size (no head/tail slivers left behind), the addresses must
     * honour the mask alignment, and freeing must unmap all of it. */
    uint64_t body = 0;
    int kr = mach_vm_map(0x103, &body, 0x400000, 0x3fffff, VM_FLAGS_ANYWHERE,
                         0, 0, 0, VM_PROT_READ | VM_PROT_WRITE,
                         VM_PROT_READ | VM_PROT_WRITE, 1);
    CHECK(kr == KERN_SUCCESS);
    CHECK(body != 0);
    CHECK((body & 0x3fffffULL) == 0);
    if (body) {
        *(volatile uint8_t *)body = 0x11;
        *(volatile uint8_t *)(body + 0x400000 - 1) = 0x22;
    }
    uint64_t live = mapped_bytes_in_range(body, body + 0x400000);
    printf("segment body live: %llu bytes (want 4194304)\n",
           (unsigned long long)live);
    CHECK(live == 0x400000ULL);

    uint64_t slab = 0;
    kr = mach_vm_map(0x103, &slab, 0x80000, 0xfffff, VM_FLAGS_ANYWHERE,
                     0, 0, 0, VM_PROT_READ | VM_PROT_WRITE,
                     VM_PROT_READ | VM_PROT_WRITE, 1);
    CHECK(kr == KERN_SUCCESS);
    CHECK(slab != 0);
    CHECK((slab & 0xfffffULL) == 0);
    if (slab) {
        *(volatile uint8_t *)slab = 0x33;
        *(volatile uint8_t *)(slab + 0x80000 - 1) = 0x44;
    }
    live = mapped_bytes_in_range(slab, slab + 0x80000);
    printf("slab live: %llu bytes (want 524288)\n",
           (unsigned long long)live);
    CHECK(live == 0x80000ULL);

    uint64_t seg = 0;
    kr = mach_vm_allocate(0x103, &seg, 0x400000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(seg != 0);
    CHECK((seg & 16383ULL) == 0);
    if (seg) *(volatile uint8_t *)seg = 0x55;
    live = mapped_bytes_in_range(seg, seg + 0x400000);
    printf("segment live: %llu bytes (want 4194304)\n",
           (unsigned long long)live);
    CHECK(live == 0x400000ULL);

    if (body) { kr = mach_vm_deallocate(0x103, body, 0x400000); CHECK(kr == KERN_SUCCESS); }
    if (slab) { kr = mach_vm_deallocate(0x103, slab, 0x80000); CHECK(kr == KERN_SUCCESS); }
    if (seg) { kr = mach_vm_deallocate(0x103, seg, 0x400000); CHECK(kr == KERN_SUCCESS); }
    live = 0;
    if (body) live += mapped_bytes_in_range(body, body + 0x400000);
    if (slab) live += mapped_bytes_in_range(slab, slab + 0x80000);
    if (seg) live += mapped_bytes_in_range(seg, seg + 0x400000);
    printf("after free: %llu bytes (want 0)\n", (unsigned long long)live);
    CHECK(live == 0);
}

static void test_fixed_no_overwrite(void) {
    uint64_t a = 0;
    int kr = mach_vm_allocate(0x103, &a, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(a != 0);
    if (!a) return;
    *(volatile uint8_t *)a = 0xab;
    mach_vm_deallocate(0x103, a, 0x4000);

    /* First FIXED allocate at the now-free address succeeds. */
    uint64_t fixed = a;
    kr = mach_vm_allocate(0x103, &fixed, 0x4000, VM_FLAGS_FIXED);
    CHECK(kr == KERN_SUCCESS);
    CHECK(fixed == a);
    *(volatile uint8_t *)a = 0xab;

    /* Second FIXED allocate at the same occupied address must fail
     * and must not clobber the live mapping. On the unpatched tree
     * it succeeds (plain MAP_FIXED) and the marker is gone. */
    uint64_t again = a;
    kr = mach_vm_allocate(0x103, &again, 0x4000, VM_FLAGS_FIXED);
    CHECK(kr == KERN_NO_SPACE);
    CHECK(*(volatile uint8_t *)a == 0xab);

    /* With OVERWRITE it succeeds, as the Mach specification allows. */
    uint64_t ow = a;
    kr = mach_vm_allocate(0x103, &ow, 0x4000, VM_FLAGS_OVERWRITE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(ow == a);

    /* Unaligned FIXED address is invalid. */
    uint64_t unal = a + 1;
    kr = mach_vm_allocate(0x103, &unal, 0x4000, VM_FLAGS_FIXED);
    CHECK(kr == KERN_INVALID_ADDRESS);

    mach_vm_deallocate(0x103, a, 0x4000);
}

static void test_alloc_free_roundtrip(void) {
    /* Every size class round-trips to zero live bytes, including
     * sizes just above a 4 KiB and a 16 KiB boundary. */
    static const uint64_t sizes[] = {
        1, 0x1000, 0x1001, 0x4000, 0x4001, 0x10000, 0x10001,
        0x100000, 0x400000,
    };
    uint64_t leaked = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint64_t addr = 0;
        int kr = mach_vm_allocate(0x103, &addr, sizes[i], VM_FLAGS_ANYWHERE);
        CHECK(kr == KERN_SUCCESS);
        CHECK(addr != 0);
        if (addr) {
            *(volatile uint8_t *)addr = (uint8_t)i;
            kr = mach_vm_deallocate(0x103, addr, sizes[i]);
            CHECK(kr == KERN_SUCCESS);
            /* Tail the old code would leak: [addr+ROUND4, addr+ROUND16). */
            leaked += mapped_bytes_in_range(addr + ROUND4(sizes[i]),
                                            addr + ROUND16(sizes[i]));
        }
    }
    printf("roundtrip leak: %llu bytes (want 0)\n",
           (unsigned long long)leaked);
    CHECK(leaked == 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_dealloc_leak();
    test_mmap_munmap_leak();
    test_xzone_segment_pattern();
    test_fixed_no_overwrite();
    test_alloc_free_roundtrip();
    printf("vm_xzone_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
