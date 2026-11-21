/*
 * heap067_kblayout_bytes_test.c -- host test for the file-bytes-to-
 * NSData path (Solarium lead).
 *
 * Isolation: with FEATUREFLAGS_DISABLED=SwiftUI/Solarium the
 * tap does not crash; with Solarium on it crashes every time in the
 * TUIKBGraphSerialization decode of a keyboard layout. The decode
 * is pure Apple code over the KBLayouts .dat file bytes, so the
 * question is whether the bytes our tree hands it equal the file
 * bytes. This test maps the REAL KBLayouts .dat files through our
 * lsk_mmap (the real vm.c) and compares byte-for-byte against the
 * file bytes read with pread. It also checks the 16K-vs-4K
 * rounding past EOF (the bytes in the last 4K page past EOF must
 * be zero, not garbage) and a non-page-aligned mmap offset
 * (which Linux must reject).
 *
 * The .dat files live in the sysroot:
 *   System/Library/PrivateFrameworks/TextInputUI.framework/KBLayouts_{iPhone,Dynamic}.dat
 * Override with KB_LAYOUTS_DIR. The files are byte-identical to
 * the IPSW (sha256 checked on device); their sizes are NOT page
 * aligned (iPhone 2,488,416 B, Dynamic 2,099,925 B), so the
 * 16K rounding past EOF is live in every whole-file map.
 *
 * Build: cc this file + vm.c (no kevent.c, no ipc.c, no bsd.c).
 * lsk_mmap is in vm.c; set_errno / linux_to_darwin_errno are
 * stubbed here (errno.c uses inline "mrs tpidr_el0" asm and does
 * not compile on host). The reference bytes come from libc pread.
 *
 * Expected: all checks pass (the mmap path returns the file bytes
 * exactly). A failure is RED: the mapped bytes differ from the
 * file, and the first mismatch offset is printed.
 *
 * Step 7 deallocates a 4K-offset file mapping with mach_vm_deallocate
 * and checks the neighbouring ref buffer survives. This is the
 * regression test for the munmap round-down defect: the 16K
 * round-down in munmap/deallocate unmapped up to 12K of the
 * neighbouring mapping, so the next mmap reused those pages and
 * the ref bytes changed underneath the test.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* lsk_mmap from vm.c (Darwin mmap: addr, len, prot, flags, fd, offset). */
extern void *lsk_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset) __asm__("_mmap");
extern int lsk_munmap(void *addr, size_t len) __asm__("_munmap");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");

/* Stubs needed by vm.c (errno.c is not compiled on host). */
void set_errno(int err) { (void)err; }
int linux_to_darwin_errno(long linux_err) { return linux_err < 0 ? (int)(-linux_err) : 0; }

/* Darwin mmap flags/prot (same values as Linux for these bits). */
#define DARWIN_PROT_READ   0x01
#define DARWIN_MAP_SHARED  0x0001
#define DARWIN_MAP_PRIVATE 0x0002
#define DARWIN_MAP_FIXED   0x0010

static const char *g_dir;

static int read_whole_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf) { close(fd); return -1; }
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, buf + done, size - done, (off_t)done);
        if (n <= 0) { free(buf); close(fd); return -1; }
        done += (size_t)n;
    }
    close(fd);
    *out_buf = buf;
    *out_size = size;
    return 0;
}

/* First mismatch offset between a and b over len bytes, or -1. */
static long first_mismatch(const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) return (long)i;
    }
    return -1;
}

/* 1 if any byte of [lo, hi) is mapped, 0 otherwise (via
 * /proc/self/maps). */
static int range_is_mapped(uint64_t lo, uint64_t hi) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 1;
    char line[512];
    int mapped = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a = 0, b = 0;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2 && b > lo && a < hi) { mapped = 1; break; }
    }
    fclose(f);
    return mapped;
}

static void test_one_file(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    uint8_t *ref = NULL;
    size_t size = 0;
    if (read_whole_file(path, &ref, &size) != 0) {
        /* The .dat files live in the qemu sysroot; if they are not
         * present (e.g. a machine without the sysroot), skip the
         * real-file part instead of failing the suite. The
         * synthetic test below still runs. */
        printf("SKIP: cannot read %s (%s); sysroot .dat files absent\n",
               path, strerror(errno));
        return;
    }
    printf("file %s: %zu bytes\n", name, size);
    CHECK(size > 0);

    /* 1. Whole-file mmap through lsk_mmap, byte-for-byte compare. */
    void *map = lsk_mmap(NULL, size, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, open(path, O_RDONLY), 0);
    /* Note: the fd above is leaked on purpose (the test is short);
     * the mapping holds its own reference. */
    CHECK(map != NULL && map != (void *)-1);
    if (map != NULL && map != (void *)-1) {
        long m = first_mismatch((const uint8_t *)map, ref, size);
        if (m >= 0) {
            printf("FAIL: %s whole-file mmap differs at offset %ld: map=0x%02x ref=0x%02x\n",
                   name, m, ((const uint8_t *)map)[m], ref[m]);
            g_checks++;
            g_failures++;
        } else {
            g_checks++;
        }
        /* 2. Past-EOF zeros: bytes in [size, roundup4K(size)) are in
         * the last 4K page and must be zero, not garbage. Do not
         * read past the 4K page boundary (SIGBUS). */
        size_t r4k = (size + 4095) & ~(size_t)4095;
        int past_eof_zero = 1;
        for (size_t i = size; i < r4k; i++) {
            if (((const uint8_t *)map)[i] != 0) { past_eof_zero = 0; break; }
        }
        CHECK(past_eof_zero);
        /* 3. pread at a non-page-aligned offset matches the mmap. */
        uint8_t tmp[1000];
        int fd2 = open(path, O_RDONLY);
        ssize_t n = pread(fd2, tmp, sizeof tmp, 100);
        CHECK(n == (ssize_t)sizeof tmp);
        if (n == (ssize_t)sizeof tmp) {
            long m2 = first_mismatch(tmp, (const uint8_t *)map + 100, sizeof tmp);
            CHECK(m2 < 0);
        }
        close(fd2);
        CHECK(lsk_munmap(map, size) == 0);
    }

    /* 4. Mmap at a 16K-aligned offset into the file. */
    {
        int fd = open(path, O_RDONLY);
        size_t off = 16384;
        size_t len = size - off;
        void *m2 = lsk_mmap(NULL, len, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, fd, (int64_t)off);
        CHECK(m2 != NULL && m2 != (void *)-1);
        if (m2 != NULL && m2 != (void *)-1) {
            long m = first_mismatch((const uint8_t *)m2, ref + off, len);
            if (m >= 0) {
                printf("FAIL: %s offset-16K mmap differs at +%ld\n", name, m);
                g_checks++;
                g_failures++;
            } else {
                g_checks++;
            }
            CHECK(lsk_munmap(m2, len) == 0);
        }
        close(fd);
    }

    /* 5. Mmap at a 4K-aligned (but not 16K-aligned) offset. Linux
     * allows 4K-aligned offsets; Darwin requires 16K, but the
     * decode could plausibly use a 4K boundary. */
    {
        int fd = open(path, O_RDONLY);
        size_t off = 4096;
        size_t len = size - off;
        void *m3 = lsk_mmap(NULL, len, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, fd, (int64_t)off);
        CHECK(m3 != NULL && m3 != (void *)-1);
        if (m3 != NULL && m3 != (void *)-1) {
            long m = first_mismatch((const uint8_t *)m3, ref + off, len);
            if (m >= 0) {
                printf("FAIL: %s offset-4K mmap differs at +%ld\n", name, m);
                g_checks++;
                g_failures++;
            } else {
                g_checks++;
            }
            CHECK(lsk_munmap(m3, len) == 0);
        }
        close(fd);
    }

    /* 6. A non-page-aligned mmap offset must fail (Linux requires
     * a page-aligned offset). Documents the behaviour: if Apple
     * code ever passes one, mmap fails and the caller must use
     * pread instead. */
    {
        int fd = open(path, O_RDONLY);
        void *m4 = lsk_mmap(NULL, 4096, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, fd, 100);
        CHECK(m4 == (void *)-1);
        close(fd);
    }

    /* 7. mach_vm_deallocate of a 4K-offset file mapping must free
     * exactly that mapping. Regression for the munmap round-down
     * defect: the 16K round-down in deallocate used to unmap up to
     * 12K of the neighbouring mapping (here, the ref buffer's
     * first pages, which sit adjacent to the file mapping), and
     * the next mmap then reused those pages for its own file
     * bytes. The deallocate round-down is clamped to the tracked
     * mapping's start, so ref must survive intact and the mapping
     * must be gone. */
    {
        int fd = open(path, O_RDONLY);
        size_t off = 4096;
        size_t len = size - off;
        void *m5 = lsk_mmap(NULL, len, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, fd, (int64_t)off);
        CHECK(m5 != NULL && m5 != (void *)-1);
        if (m5 != NULL && m5 != (void *)-1) {
            uint8_t head0 = ref[0];
            uint8_t head4096 = ref[4096];
            int kr = mach_vm_deallocate(0x103, (uint64_t)m5, len);
            CHECK(kr == 0);
            CHECK(ref[0] == head0);
            CHECK(ref[4096] == head4096);
            CHECK(!range_is_mapped((uint64_t)m5, (uint64_t)m5 + len));
        }
        close(fd);
    }

    free(ref);
}

static void test_synthetic(void) {
    /* A temp file with a known pattern and a non-page-aligned size
     * (5000 bytes), so the core mmap logic is tested even if the
     * .dat files are absent. */
    char tmpl[] = "/tmp/kblayout-synth-XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    if (fd < 0) return;
    size_t size = 5000;
    uint8_t *ref = (uint8_t *)malloc(size);
    for (size_t i = 0; i < size; i++) ref[i] = (uint8_t)((i * 31 + 7) & 0xff);
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, ref + done, size - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    CHECK(done == size);
    void *map = lsk_mmap(NULL, size, DARWIN_PROT_READ, DARWIN_MAP_PRIVATE, fd, 0);
    CHECK(map != NULL && map != (void *)-1);
    if (map != NULL && map != (void *)-1) {
        long m = first_mismatch((const uint8_t *)map, ref, size);
        if (m >= 0) {
            printf("FAIL: synthetic mmap differs at offset %ld\n", m);
            g_checks++;
            g_failures++;
        } else {
            g_checks++;
        }
        size_t r4k = (size + 4095) & ~(size_t)4095;
        int past_eof_zero = 1;
        for (size_t i = size; i < r4k; i++) {
            if (((const uint8_t *)map)[i] != 0) { past_eof_zero = 0; break; }
        }
        CHECK(past_eof_zero);
        CHECK(lsk_munmap(map, size) == 0);
    }
    free(ref);
    close(fd);
    unlink(tmpl);
}

int main(void) {
    g_dir = getenv("KB_LAYOUTS_DIR");
    if (!g_dir) {
        g_dir = "/data/local/tmp/ioswine/root/System/Library/PrivateFrameworks/TextInputUI.framework";
    }
    test_one_file("KBLayouts_iPhone.dat");
    test_one_file("KBLayouts_Dynamic.dat");
    test_synthetic();
    printf("heap067_kblayout_bytes_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
