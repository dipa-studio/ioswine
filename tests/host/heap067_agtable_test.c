/*
 * heap067_agtable_test.c -- host test for the AG table grow_region
 * remap defect.
 *
 * The defect: lsk_mach_vm_remap unconditionally ended with
 * sys_mprotect(target, PROT_READ | PROT_EXEC) and reported
 * cur/max_protection = VM_PROT_READ | VM_PROT_EXECUTE, and it
 * ignored the `copy` flag (always copied, never shared). The
 * AttributeGraph data::table ctor reserves its region with
 * mmap(0, 0x100000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
 * -1, 0), and grow_region (AG 0x1b6cf49f0) grows it 4x by mmaping a
 * new region and calling mach_vm_remap(target=&new_base,
 * size=old_size, mask=0, flags=VM_FLAGS_OVERWRITE, src_task=task,
 * src_address=old_base, copy=FALSE, cur/max_protection out,
 * inheritance=VM_INHERIT_NONE) to move the old pages into the new
 * region. After the remap the table's current region was R-X, so
 * the next table write faulted with SIGSEGV on a 0x5... address
 * (AG::Graph::add_input, add_output_edge, UpdateStack::update).
 *
 * Correction (2026-11-19): the original checks 6 and 7 asserted page
 * SHARING (a write through the old address visible at the new address
 * and vice versa). That is not XNU semantics: copy=FALSE is a MOVE
 * (XNU vm_map_remap passes !copy as src_destroy to vm_map_copyin, so
 * the source mapping is removed), and no Apple caller can depend on
 * the source staying mapped after a copy=FALSE remap, because real
 * XNU unmaps it. The checks below assert the move (old address
 * unmapped, new region keeps the data). The protection checks (2, 3,
 * 5) are unchanged and discriminate the AG root cause; the failure
 * profile on the unpatched tree is identical (7 failures).
 *
 * This test replays grow_region's exact call sequence with the real
 * vm.c entry points and checks, in a forked child (so the SIGSEGV
 * on the unpatched tree kills only the child and is reported as
 * failed checks instead of crashing the suite), 17 checks total:
 *   1. remap returns KERN_SUCCESS
 *   2. cur_protection is VM_PROT_READ | VM_PROT_WRITE
 *   3. max_protection is VM_PROT_READ | VM_PROT_WRITE
 *   4. the remapped region holds the source bytes (copy/move
 *      carried the data)
 *   5. a write to the remapped region succeeds and reads back
 *      (the AG post-grow write; RED gate: SIGSEGV on base)
 *   6. move old->gone: the old address is unmapped after the
 *      copy=FALSE remap (copy=FALSE is a move on XNU)
 *   7. move keeps the target: the post-grow writes from check 5
 *      are still in the new region, and a fresh store through the
 *      new address reads back
 *   8. the child exited normally (not killed by a signal)
 *   9. the child exit status is 0
 *  10. madvise(Darwin 7, MADV_FREE_REUSABLE) on a scratch region
 *      does not unmap it: a write afterwards still succeeds
 *      (pins the negative finding that the madvise path cannot
 *      fault on the table address; it can only zero reusable
 *      pages, never unmap them)
 * plus setup checks (both regions mapped, pipe/fork/waitpid ok).
 *
 * RED check: on the unpatched tree checks 2, 3 fail (cur/max
 * report R-X), check 5 faults (child dies of SIGSEGV, so 6, 7 are
 * never reported and 8, 9 fail): 10 of 17 pass, 7 fail. On the
 * fixed tree all 17 pass, in both the plain and the ASan/UBSan
 * variants.
 * The test never assumes where the kernel placed a mapping: the
 * regions come from lsk_mmap (the same anywhere path AG uses) and
 * every check is address-agnostic, so it is meaningful under ASan
 * too (where the vm.c hint range is occupied and the fallback path
 * places the mappings elsewhere).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* vm.c entry points, by their assembler symbols. */
extern int mach_vm_remap(uint32_t target_task, uint64_t *target_address,
                         uint64_t size, uint64_t mask, int flags,
                         uint32_t src_task, uint64_t src_address, int copy,
                         int *cur_protection, int *max_protection,
                         int inheritance) __asm__("_mach_vm_remap");
extern void *lsk_mmap_fn(void *addr, size_t len, int prot, int flags, int fd,
                         int64_t offset) __asm__("_mmap");
extern int lsk_madvise_fn(void *addr, size_t len, int advice) __asm__("_madvise");

/* errno.c is aarch64-only (tpidr_el0) and is not linked here; vm.c
 * only needs these two names for the mmap error path. */
void set_errno(int err) { (void)err; }
int linux_to_darwin_errno(long linux_err) {
    return linux_err < 0 ? (int)(-linux_err) : 0;
}

#define KERN_SUCCESS 0

#define VM_FLAGS_OVERWRITE 0x4000

#define VM_PROT_READ 0x01
#define VM_PROT_WRITE 0x02
#define VM_PROT_EXECUTE 0x04

/* Darwin mmap flags, as translated by lsk_mmap. */
#define DARWIN_MAP_PRIVATE 0x02
#define DARWIN_MAP_ANONYMOUS 0x1000

/* Darwin madvise advice: 7 is MADV_FREE_REUSABLE. */
#define DARWIN_MADV_FREE_REUSABLE 7

/* The AG table region sizes: ctor reserves 1 MiB, grow_region maps
 * 4x the old size. */
#define TABLE_SIZE 0x100000ULL
#define GROWN_SIZE 0x400000ULL

/* Child check ids, in pipe order. */
#define N_CHILD_CHECKS 7

static void fill_pattern(volatile uint8_t *p, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(seed + (i & 0xff));
}

static int check_pattern(volatile uint8_t *p, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (uint8_t)(seed + (i & 0xff))) return 0;
    }
    return 1;
}

/* Mapped bytes overlapping [lo, hi), from /proc/self/maps. Used by
 * the child to probe whether the old address is still mapped after
 * the remap, without dereferencing it (it would fault once the move
 * unmapped it). */
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

/* The remap sequence, run in the forked child. Each check result is
 * written to pipe_fd the moment it is known, so a fault mid-sequence
 * still leaves the earlier results in the pipe; the parent counts
 * bytes never received as failures. Uses only raw write() and
 * _exit() so no parent stdio buffer is touched. */
static void child_run(int pipe_fd, uint64_t old_base, uint64_t new_base) {
    char b;
    uint64_t target = new_base;
    int cur_prot = -1, max_prot = -1;
    int kr = mach_vm_remap(0, &target, TABLE_SIZE, 0, VM_FLAGS_OVERWRITE,
                           0, old_base, 0 /* copy=FALSE: move */,
                           &cur_prot, &max_prot, 2 /* VM_INHERIT_NONE */);
    b = (kr == KERN_SUCCESS) ? '1' : '0';
    write(pipe_fd, &b, 1);
    b = (cur_prot == (VM_PROT_READ | VM_PROT_WRITE)) ? '1' : '0';
    write(pipe_fd, &b, 1);
    b = (max_prot == (VM_PROT_READ | VM_PROT_WRITE)) ? '1' : '0';
    write(pipe_fd, &b, 1);
    b = check_pattern((volatile uint8_t *)target, TABLE_SIZE, 0xa5)
            ? '1' : '0';
    write(pipe_fd, &b, 1);

    /* 5. The AG post-grow write: add_input / add_output_edge /
     * UpdateStack::update store into the table right after the
     * grow. On the unpatched tree the region is R-X and this store
     * faults with SIGSEGV. */
    volatile uint8_t *w = (volatile uint8_t *)target;
    fill_pattern(w, 0x1000, 0x5a);
    b = check_pattern(w, 0x1000, 0x5a) ? '1' : '0';
    write(pipe_fd, &b, 1);

    /* 6. Move old->gone: copy=FALSE is a move on XNU, so the old
     * address must be unmapped after the remap. Probe
     * /proc/self/maps without dereferencing the old address (it
     * would fault once the move unmapped it). */
    b = (mapped_bytes_in_range(old_base, old_base + TABLE_SIZE) == 0)
            ? '1' : '0';
    write(pipe_fd, &b, 1);

    /* 7. Move keeps the target: the post-grow writes from check 5
     * are still in the new region, and a fresh store through the
     * new address reads back. */
    w[0x2001] = 0x3c;
    b = (check_pattern(w, 0x1000, 0x5a) && w[0x2001] == 0x3c) ? '1' : '0';
    write(pipe_fd, &b, 1);

    _exit(0);
}

static void test_remap_grow(void) {
    /* The ctor's reservation: mmap(0, 1 MiB, RW, ANON|PRIVATE). */
    void *oldp = lsk_mmap_fn(NULL, TABLE_SIZE,
                             VM_PROT_READ | VM_PROT_WRITE,
                             DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS,
                             -1, 0);
    CHECK(oldp != NULL && oldp != (void *)-1);
    if (!oldp || oldp == (void *)-1) return;
    fill_pattern((volatile uint8_t *)oldp, TABLE_SIZE, 0xa5);

    /* grow_region's new region: mmap(0, 4 MiB, RW, ANON|PRIVATE). */
    void *newp = lsk_mmap_fn(NULL, GROWN_SIZE,
                             VM_PROT_READ | VM_PROT_WRITE,
                             DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS,
                             -1, 0);
    CHECK(newp != NULL && newp != (void *)-1);
    if (!newp || newp == (void *)-1) return;

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        close(pipefd[0]);
        child_run(pipefd[1], (uint64_t)oldp, (uint64_t)newp);
        _exit(1); /* unreachable */
    }
    close(pipefd[1]);
    char got[N_CHILD_CHECKS];
    size_t got_n = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], got + got_n, N_CHILD_CHECKS - got_n);
        if (n <= 0) break;
        got_n += (size_t)n;
        if (got_n >= (size_t)N_CHILD_CHECKS) break;
    }
    close(pipefd[0]);
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    CHECK(w == pid);
    /* Child checks 1-7, in pipe order; bytes never received count
     * as failures (the child died before reporting them). */
    for (int i = 0; i < N_CHILD_CHECKS; i++) {
        g_checks++;
        int pass = (i < (int)got_n && got[i] == '1');
        if (!pass) {
            g_failures++;
            printf("FAIL child check %d: %s\n", i + 1,
                   i < (int)got_n ? "reported fail" : "not reported (child died)");
        }
    }
    /* 8. The child must have exited normally, not by a signal
     * (SIGSEGV on the unpatched tree). */
    CHECK(WIFEXITED(status));
    if (WIFSIGNALED(status)) {
        printf("FAIL child killed by signal %d (%s)\n", WTERMSIG(status),
               strsignal(WTERMSIG(status)));
    }
    /* 9. The child exit status must be 0. */
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* The madvise path cannot fault: MADV_FREE_REUSABLE only marks pages
 * freeable, it never unmaps them. A write after the advice must
 * still succeed. */
static void test_madvise_no_fault(void) {
    void *p = lsk_mmap_fn(NULL, 0x8000, VM_PROT_READ | VM_PROT_WRITE,
                          DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS, -1, 0);
    CHECK(p != NULL && p != (void *)-1);
    if (!p || p == (void *)-1) return;
    fill_pattern((volatile uint8_t *)p, 0x8000, 0x11);
    int rc = lsk_madvise_fn(p, 0x8000, DARWIN_MADV_FREE_REUSABLE);
    CHECK(rc == 0);
    fill_pattern((volatile uint8_t *)p, 0x8000, 0x22);
    CHECK(check_pattern((volatile uint8_t *)p, 0x8000, 0x22));
}

int main(void) {
    test_madvise_no_fault();
    test_remap_grow();
    printf("heap067_agtable: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
