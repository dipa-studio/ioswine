/*
 * heap067_vm_test.c -- host test for the libsystem_kernel VM layer
 * (libsystem_kernel/src/vm.c), VM aliasing / early-free / zeroing audit.
 *
 * Question: does the VM layer alias, early-free, or zero live
 * memory? Four defects, all in vm.c, each with a check below
 * (the check fails on the unpatched tree and passes with the fix):
 *
 *  D1. mach_vm_region fabricated bounds. Any address below
 *      0x100000000 got size 0x10000, anything else got 0x1000000
 *      (16 MiB), regardless of the actual mapping. A guest that
 *      sizes a deallocate from that answer unmaps up to 16 MiB,
 *      and an oversized munmap destroys neighbouring live mappings
 *      (a 16 KiB neighbour was unmapped by a 32 KiB munmap in the
 *      host probe). The fix tracks every range vm.c maps and
 *      reports its exact bounds; an untracked address gets a
 *      conservative single guest page. (test_vm_region_truthful)
 *  D2. mach_vm_remap ignored its `copy` parameter (always copied,
 *      never unmapped the source) and, for a FIXED target, just
 *      mprotected whatever was there and copied into it: a target
 *      smaller than `size` overflowed into the neighbouring live
 *      mapping, and an unmapped target faulted inside our own copy
 *      loop. The fix replaces a FIXED target with fresh pages
 *      (MAP_FIXED) and unmaps the source when copy == FALSE (a
 *      move, per XNU). (test_remap_copy_false_moves,
 *      test_remap_fixed_unmapped)
 *  D3. vm_copy / mach_vm_copy copied blindly with no range
 *      validation, so a stale destination faulted inside our own
 *      loop. The fix validates both ranges with an msync probe and
 *      returns KERN_INVALID_ADDRESS instead. (test_vm_copy_unmapped_dest)
 *  D4. Rounding inconsistency: allocate / deallocate / mmap /
 *      munmap rounded to the 16 KiB guest page, but mach_vm_protect
 *      / mprotect / madvise / msync rounded to 4 KiB, and
 *      deallocate / munmap did not round the address down at all
 *      (an unaligned address failed with EINVAL, a leak). The fix
 *      rounds every size and address to 16 KiB, matching XNU.
 *      (test_protect_rounding_16k, test_dealloc_round_down)
 *
 *  D5 (investigated, no defect). libsystem_kernel maps Darwin
 *      MADV_FREE (5) and MADV_FREE_REUSABLE (7) to Linux MADV_FREE
 *      (8), so those pages are now really freed instead of failing
 *      EINVAL. No in-tree caller madvises a live region with 5/7,
 *      and Apple callers (libmalloc, AttributeGraph) use
 *      FREE_REUSABLE only for logically-dead pages, so the
 *      translation is correct in principle; a discard under phone
 *      memory pressure reads back as zero, which is the documented
 *      contract. test_madvise_free pins it: the range stays mapped
 *      and readable after the call. On a host shim without
 *      msync / mincore / madvise dispatch, those calls fall
 *      through to sim_sys6 and return -1.
 *
 * Regression check: D1/D2/D3/D4 fail on the unpatched tree
 * (vm_region returns 0x1000000, the remap source stays mapped,
 * the fixed-target remap and the unmapped-dest vm_copy crash the
 * child, the mprotect leaves the second guest page writable, the
 * round-down deallocate leaves 0x4000 bytes mapped, madvise
 * returns -1). The vm_copy valid-dest check and the perms-at-base
 * check pass on the unpatched tree too; they pin the contract.
 *
 * The live-bytes probe reads /proc/self/maps and measures the
 * mapped bytes overlapping a specific [lo, hi) range
 * (mapped_bytes_in_range). It never assumes where the kernel
 * placed a mapping: under ASan the vm.c hint range is occupied by
 * ASan's own reservation, so the fallback path places mappings far
 * outside it, and a hint-range sum would read 0 vacuously. Every
 * check is range-targeted, so it is meaningful in both variants.
 * The two crash probes run in a forked child (run_in_child); a
 * child that dies by signal or exits non-zero reports the hole.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
extern int mach_vm_protect(uint32_t target, uint64_t address, uint64_t size, int set_maximum, int new_protection) __asm__("_mach_vm_protect");
extern int mach_vm_region(uint32_t target, uint64_t *address, uint64_t *size, int flavor,
                          int *info, uint32_t *infoCnt, uint32_t *object_name) __asm__("_mach_vm_region");
extern int mach_vm_remap(uint32_t target_task, uint64_t *target_address, uint64_t size, uint64_t mask,
                         int flags, uint32_t src_task, uint64_t src_address, int copy,
                         int *cur_protection, int *max_protection, int inheritance) __asm__("_mach_vm_remap");
extern int vm_copy_fn(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) __asm__("_vm_copy");
extern void *lsk_mmap_fn(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) __asm__("_mmap");
extern int lsk_munmap_fn(void *addr, size_t len) __asm__("_munmap");
extern int lsk_mprotect_fn(void *addr, size_t len, int prot) __asm__("_mprotect");
extern int lsk_madvise_fn(void *addr, size_t len, int advice) __asm__("_madvise");

/* errno.c is aarch64-only (tpidr_el0) and is not linked here; vm.c
 * only needs these two names for the mmap error path. */
void set_errno(int err) { (void)err; }
int linux_to_darwin_errno(long linux_err) {
    return linux_err < 0 ? (int)(-linux_err) : 0;
}

#define KERN_SUCCESS          0
#define KERN_INVALID_ADDRESS  1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE         3
#define KERN_INVALID_ARGUMENT 4

#define VM_FLAGS_FIXED        0x0000
#define VM_FLAGS_ANYWHERE     0x0001
#define VM_FLAGS_OVERWRITE    0x4000

#define VM_PROT_READ          0x01
#define VM_PROT_WRITE         0x02
#define VM_PROT_EXECUTE       0x04

/* Darwin mmap flags, as translated by lsk_mmap. */
#define DARWIN_MAP_SHARED     0x01
#define DARWIN_MAP_PRIVATE    0x02
#define DARWIN_MAP_ANONYMOUS  0x1000

/* vm_region info, same layout as vm.c's vm_region_basic_info_64. */
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

/* Protection string (e.g. "rw-p") of the VMA containing addr, or 0
 * when addr is not in any mapping. */
static int perms_at(uint64_t addr, char *out, size_t outsz) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t start = 0, end = 0;
        char perms[8] = { 0 };
        if (sscanf(line, "%llx-%llx %7s", (unsigned long long *)&start,
                   (unsigned long long *)&end, perms) != 3) {
            continue;
        }
        if (addr >= start && addr < end) {
            strncpy(out, perms, outsz - 1);
            out[outsz - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Run fn in a forked child. Returns 1 when the child exits 0, 0
 * when it dies by signal, exits non-zero, or fork/wait fails. The
 * child redirects stderr to /dev/null so a crash report does not
 * pollute the test output. */
static int run_in_child(int (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        _exit(fn());
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 0;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
}

#define ROUND4(x)  (((x) + 4095ULL) & ~4095ULL)
#define ROUND16(x) (((x) + 16383ULL) & ~16383ULL)

/* An untracked address for the vm_region fallback: a static
 * variable lives in this binary's own data segment, which vm.c
 * never mapped, so the allocation table has no record for it. */
static int g_untracked_marker = 0;

static void test_vm_region_truthful(void) {
    uint64_t a = 0;
    int kr = mach_vm_allocate(0x103, &a, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(a != 0);
    if (!a) return;
    *(volatile uint8_t *)a = 0x5a;

    uint64_t b = 0;
    kr = mach_vm_allocate(0x103, &b, 0x10000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(b != 0);
    if (!b) { mach_vm_deallocate(0x103, a, 0x4000); return; }
    *(volatile uint8_t *)b = 0xa5;

    struct vm_region_basic_info_64 info;
    uint32_t infoCnt = 8;
    uint32_t obj = 0;

    /* A 16 KiB allocation reports exactly 16 KiB, not 16 MiB. */
    uint64_t qa = a, sa = 0;
    memset(&info, 0, sizeof(info));
    infoCnt = 8;
    kr = mach_vm_region(0x103, &qa, &sa, 0, (int *)&info, &infoCnt, &obj);
    printf("vm_region(A): base 0x%llx size 0x%llx prot %d\n",
           (unsigned long long)qa, (unsigned long long)sa, info.protection);
    CHECK(kr == KERN_SUCCESS);
    CHECK(qa == a);
    CHECK(sa == 0x4000ULL);
    CHECK(info.protection == (VM_PROT_READ | VM_PROT_WRITE));

    /* A 64 KiB allocation reports exactly 64 KiB. */
    uint64_t qb = b, sb = 0;
    memset(&info, 0, sizeof(info));
    infoCnt = 8;
    kr = mach_vm_region(0x103, &qb, &sb, 0, (int *)&info, &infoCnt, &obj);
    printf("vm_region(B): base 0x%llx size 0x%llx\n",
           (unsigned long long)qb, (unsigned long long)sb);
    CHECK(kr == KERN_SUCCESS);
    CHECK(qb == b);
    CHECK(sb == 0x10000ULL);

    /* A query inside a region reports that region's base and size. */
    uint64_t qi = a + 0x2000, si = 0;
    memset(&info, 0, sizeof(info));
    infoCnt = 8;
    kr = mach_vm_region(0x103, &qi, &si, 0, (int *)&info, &infoCnt, &obj);
    CHECK(kr == KERN_SUCCESS);
    CHECK(qi == a);
    CHECK(si == 0x4000ULL);

    /* An untracked address gets a conservative single guest page. */
    uint64_t qu = (uint64_t)&g_untracked_marker, su = 0;
    memset(&info, 0, sizeof(info));
    infoCnt = 8;
    kr = mach_vm_region(0x103, &qu, &su, 0, (int *)&info, &infoCnt, &obj);
    printf("vm_region(untracked): base 0x%llx size 0x%llx\n",
           (unsigned long long)qu, (unsigned long long)su);
    CHECK(kr == KERN_SUCCESS);
    CHECK(qu == ((uint64_t)&g_untracked_marker & ~16383ULL));
    CHECK(su == 0x4000ULL);

    /* After freeing, the address is untracked again. */
    kr = mach_vm_deallocate(0x103, a, 0x4000);
    CHECK(kr == KERN_SUCCESS);
    uint64_t qf = a, sf = 0;
    memset(&info, 0, sizeof(info));
    infoCnt = 8;
    kr = mach_vm_region(0x103, &qf, &sf, 0, (int *)&info, &infoCnt, &obj);
    CHECK(kr == KERN_SUCCESS);
    CHECK(qf == a);
    CHECK(sf == 0x4000ULL);
    CHECK(mapped_bytes_in_range(a, a + 0x4000) == 0);

    mach_vm_deallocate(0x103, b, 0x10000);
}

static void test_remap_copy_false_moves(void) {
    uint64_t src = 0;
    int kr = mach_vm_allocate(0x103, &src, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(src != 0);
    if (!src) return;
    *(volatile uint8_t *)src = 0x5a;
    *(volatile uint8_t *)(src + 0x3fff) = 0xa5;

    /* copy == FALSE is a move: the target gets the data and the
     * source mapping goes away. On the unpatched tree the source
     * stays mapped (copy is ignored), so the mapped-bytes check
     * below reports 0x4000. */
    uint64_t tgt = 0;
    kr = mach_vm_remap(0x103, &tgt, 0x4000, 0, VM_FLAGS_ANYWHERE,
                       0x103, src, 0, NULL, NULL, 1);
    CHECK(kr == KERN_SUCCESS);
    CHECK(tgt != 0);
    CHECK(tgt != src);
    if (tgt) {
        CHECK(*(volatile uint8_t *)tgt == 0x5a);
        CHECK(*(volatile uint8_t *)(tgt + 0x3fff) == 0xa5);
    }
    uint64_t src_live = mapped_bytes_in_range(src, src + 0x4000);
    printf("remap copy=FALSE: source still mapped %llu bytes (want 0)\n",
           (unsigned long long)src_live);
    CHECK(src_live == 0);

    if (tgt) mach_vm_deallocate(0x103, tgt, 0x4000);
}

/* Child probe: remap onto a free FIXED address. On the unpatched
 * tree the target is unmapped, so the copy loop faults inside our
 * own code and the child dies by signal. */
static int probe_remap_fixed_unmapped(void) {
    uint64_t src = 0;
    int kr = mach_vm_allocate(0x103, &src, 0x4000, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS || !src) return 1;
    *(volatile uint8_t *)src = 0x5a;
    *(volatile uint8_t *)(src + 0x3fff) = 0xa5;

    /* A free address: allocate then free a scratch region. */
    uint64_t scratch = 0;
    kr = mach_vm_allocate(0x103, &scratch, 0x4000, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS || !scratch) return 1;
    kr = mach_vm_deallocate(0x103, scratch, 0x4000);
    if (kr != KERN_SUCCESS) return 1;

    uint64_t tgt = scratch;
    kr = mach_vm_remap(0x103, &tgt, 0x4000, 0, VM_FLAGS_FIXED,
                       0x103, src, 1, NULL, NULL, 1);
    if (kr != KERN_SUCCESS) return 1;
    if (tgt != scratch) return 1;
    if (*(volatile uint8_t *)tgt != 0x5a) return 1;
    if (*(volatile uint8_t *)(tgt + 0x3fff) != 0xa5) return 1;
    /* copy == TRUE: the source stays mapped. */
    if (mapped_bytes_in_range(src, src + 0x4000) != 0x4000) return 1;
    mach_vm_deallocate(0x103, tgt, 0x4000);
    mach_vm_deallocate(0x103, src, 0x4000);
    return 0;
}

static void test_remap_fixed_unmapped(void) {
    CHECK(run_in_child(probe_remap_fixed_unmapped) == 1);
}

/* Child probe: vm_copy to an unmapped destination. On the unpatched
 * tree the byte loop faults inside our own code and the child dies
 * by signal; with the fix it returns KERN_INVALID_ADDRESS. */
static int probe_vm_copy_unmapped_dest(void) {
    uint64_t src = 0;
    int kr = mach_vm_allocate(0x103, &src, 0x4000, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS || !src) return 1;
    *(volatile uint8_t *)src = 0x5a;

    uint64_t scratch = 0;
    kr = mach_vm_allocate(0x103, &scratch, 0x4000, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS || !scratch) return 1;
    kr = mach_vm_deallocate(0x103, scratch, 0x4000);
    if (kr != KERN_SUCCESS) return 1;

    kr = vm_copy_fn(0x103, src, 0x4000, scratch);
    if (kr != KERN_INVALID_ADDRESS) return 1;
    /* The source is untouched and the destination is still unmapped. */
    if (*(volatile uint8_t *)src != 0x5a) return 1;
    if (mapped_bytes_in_range(scratch, scratch + 0x4000) != 0) return 1;
    mach_vm_deallocate(0x103, src, 0x4000);
    return 0;
}

static void test_vm_copy_unmapped_dest(void) {
    CHECK(run_in_child(probe_vm_copy_unmapped_dest) == 1);

    /* Positive control: a valid destination copies and succeeds. */
    uint64_t src = 0, dst = 0;
    int kr = mach_vm_allocate(0x103, &src, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    kr = mach_vm_allocate(0x103, &dst, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    if (!src || !dst) {
        if (src) mach_vm_deallocate(0x103, src, 0x4000);
        if (dst) mach_vm_deallocate(0x103, dst, 0x4000);
        return;
    }
    *(volatile uint8_t *)src = 0x5a;
    *(volatile uint8_t *)(src + 0x3fff) = 0xa5;
    kr = vm_copy_fn(0x103, src, 0x4000, dst);
    CHECK(kr == KERN_SUCCESS);
    CHECK(*(volatile uint8_t *)dst == 0x5a);
    CHECK(*(volatile uint8_t *)(dst + 0x3fff) == 0xa5);
    mach_vm_deallocate(0x103, src, 0x4000);
    mach_vm_deallocate(0x103, dst, 0x4000);
}

static void test_protect_rounding_16k(void) {
    uint64_t a = 0;
    int kr = mach_vm_allocate(0x103, &a, 0x8000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(a != 0);
    if (!a) return;
    *(volatile uint8_t *)a = 0x5a;

    /* Protect 0x5000 bytes read-only. With 16 KiB rounding the whole
     * second guest page (up to a + 0x8000) is read-only; with the
     * old 4 KiB rounding only [a, a + 0x6000) is, and a + 0x6000
     * stays writable. */
    int rc = lsk_mprotect_fn((void *)a, 0x5000, VM_PROT_READ);
    CHECK(rc == 0);
    char perms[8] = { 0 };
    CHECK(perms_at(a, perms, sizeof(perms)) == 1);
    CHECK(strchr(perms, 'w') == NULL);
    memset(perms, 0, sizeof(perms));
    CHECK(perms_at(a + 0x6000, perms, sizeof(perms)) == 1);
    printf("protect 0x5000: perms at a+0x6000 are %s (want no w)\n", perms);
    CHECK(strchr(perms, 'w') == NULL);

    rc = lsk_mprotect_fn((void *)a, 0x8000, VM_PROT_READ | VM_PROT_WRITE);
    CHECK(rc == 0);
    memset(perms, 0, sizeof(perms));
    CHECK(perms_at(a + 0x6000, perms, sizeof(perms)) == 1);
    CHECK(strchr(perms, 'w') != NULL);

    mach_vm_deallocate(0x103, a, 0x8000);
}

static void test_dealloc_round_down(void) {
    uint64_t a = 0;
    int kr = mach_vm_allocate(0x103, &a, 0x8000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(a != 0);
    if (!a) return;
    *(volatile uint8_t *)a = 0x5a;
    *(volatile uint8_t *)(a + 0x7fff) = 0xa5;

    /* Deallocate from a 4 KiB-aligned, non-16K-aligned address.
     * XNU rounds the address down to the page and extends the size,
     * so the whole 32 KiB allocation goes away. On the unpatched
     * tree munmap(a + 0x1000, 0x4000) leaves 0x4000 bytes mapped. */
    kr = mach_vm_deallocate(0x103, a + 0x1000, 0x4000);
    CHECK(kr == KERN_SUCCESS);
    uint64_t live = mapped_bytes_in_range(a, a + 0x8000);
    printf("dealloc round-down: %llu bytes still mapped (want 0)\n",
           (unsigned long long)live);
    CHECK(live == 0);
}

static void test_madvise_free(void) {
    uint64_t a = 0;
    int kr = mach_vm_allocate(0x103, &a, 0x4000, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS);
    CHECK(a != 0);
    if (!a) return;
    *(volatile uint8_t *)a = 0x5a;
    *(volatile uint8_t *)(a + 0x3fff) = 0xa5;

    /* Darwin MADV_FREE (5) must succeed and must not unmap the
     * range: the pages stay mapped and readable (a discard under
     * memory pressure reads back as zero, which is the contract).
     * On the base shim the call fails with -1 (no dispatch). */
    int rc = lsk_madvise_fn((void *)a, 0x4000, 5);
    printf("madvise FREE (5): rc %d (want 0)\n", rc);
    CHECK(rc == 0);
    CHECK(mapped_bytes_in_range(a, a + 0x4000) == 0x4000);
    CHECK(*(volatile uint8_t *)a == 0x5a);
    CHECK(*(volatile uint8_t *)(a + 0x3fff) == 0xa5);

    mach_vm_deallocate(0x103, a, 0x4000);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_vm_region_truthful();
    test_remap_copy_false_moves();
    test_remap_fixed_unmapped();
    test_vm_copy_unmapped_dest();
    test_protect_rounding_16k();
    test_dealloc_round_down();
    test_madvise_free();
    printf("heap067_vm_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
