/* metallib_test.c - host unit test for the freestanding metallib reader.
 *
 * Plain cc, no NDK. The library itself uses no libc; this harness does
 * (stdio, stdlib, string) only to drive it and to report results.
 * Builds synthetic .metallib files in memory (matching the format the
 * reader implements, including the real 2015-file layout where the
 * header's function-list size lands on the last group's ENDT), then
 * checks the reader round-trips them, plus error paths and a fuzz loop.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../src \
 *      -o metallib_test metallib_test.c ../../src/metallib.c
 *   ./metallib_test [fuzz_iterations]
 *
 * Sanitizers (fuzz must be clean under these):
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../src \
 *      -o metallib_test_asan metallib_test.c ../../src/metallib.c
 *   ./metallib_test_asan [fuzz_iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "metallib.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_ERR(call, want) do { \
    int _rc = (call); \
    g_checks++; \
    if (_rc != (want)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s returned %d (%s), want %d\n", \
               __FILE__, __LINE__, #call, _rc, ml_error_string(_rc), \
               (want)); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Tiny byte-buffer builder (test only). */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} tb_t;

static void tb_grow(tb_t *b, size_t need)
{
    if (b->len + need > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        uint8_t *nb;
        while (ncap < b->len + need)
            ncap *= 2;
        nb = (uint8_t *)realloc(b->buf, ncap);
        if (nb == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        b->buf = nb;
        b->cap = ncap;
    }
}

static void tb_u8(tb_t *b, uint8_t v)
{
    tb_grow(b, 1);
    b->buf[b->len++] = v;
}

static void tb_u16(tb_t *b, uint16_t v)
{
    tb_u8(b, (uint8_t)(v & 0xff));
    tb_u8(b, (uint8_t)((v >> 8) & 0xff));
}

static void tb_u32(tb_t *b, uint32_t v)
{
    tb_u8(b, (uint8_t)(v & 0xff));
    tb_u8(b, (uint8_t)((v >> 8) & 0xff));
    tb_u8(b, (uint8_t)((v >> 16) & 0xff));
    tb_u8(b, (uint8_t)((v >> 24) & 0xff));
}

static void tb_bytes(tb_t *b, const void *p, size_t n)
{
    const uint8_t *q = (const uint8_t *)p;
    size_t i;
    tb_grow(b, n);
    for (i = 0; i < n; i++)
        b->buf[b->len++] = q[i];
}

static void tb_patch_u32(tb_t *b, size_t off, uint32_t v)
{
    b->buf[off] = (uint8_t)(v & 0xff);
    b->buf[off + 1] = (uint8_t)((v >> 8) & 0xff);
    b->buf[off + 2] = (uint8_t)((v >> 16) & 0xff);
    b->buf[off + 3] = (uint8_t)((v >> 24) & 0xff);
}

static void tb_patch_u64(tb_t *b, size_t off, uint64_t v)
{
    tb_patch_u32(b, off, (uint32_t)(v & 0xffffffffu));
    tb_patch_u32(b, off + 4, (uint32_t)(v >> 32));
}

static void tb_tag(tb_t *b, const char *code, const void *payload,
                   uint16_t size)
{
    tb_bytes(b, code, 4);
    tb_u16(b, size);
    if (size > 0)
        tb_bytes(b, payload, size);
}

static void tb_endt(tb_t *b)
{
    tb_bytes(b, "ENDT", 4);
}

/* ------------------------------------------------------------------ */
/* Synthetic bitcode blobs: wrapper + "BC" magic + payload bytes. */

static size_t make_blob(uint8_t *out, uint8_t fill, uint32_t payload_len)
{
    uint32_t total = 20 + 4 + payload_len;
    uint32_t i;
    /* Wrapper: magic, version 0, offset 20, size, cputype -1. */
    out[0] = 0xde;
    out[1] = 0xc0;
    out[2] = 0x17;
    out[3] = 0x0b;
    memset(out + 4, 0, 4);
    out[8] = 20;
    out[9] = 0;
    out[10] = 0;
    out[11] = 0;
    out[12] = (uint8_t)((4 + payload_len) & 0xff);
    out[13] = (uint8_t)(((4 + payload_len) >> 8) & 0xff);
    out[14] = 0;
    out[15] = 0;
    memset(out + 16, 0xff, 4);
    out[20] = 0x42; /* 'B' */
    out[21] = 0x43; /* 'C' */
    out[22] = 0xc0;
    out[23] = 0xde;
    for (i = 0; i < payload_len; i++)
        out[24 + i] = fill;
    return total;
}

/* ------------------------------------------------------------------ */
/* Synthetic metallib builder.
 * variant 0: full file, OFFT + MDSZ + HASH + VERS on both functions.
 * variant 1: no OFFT tags (sequential bitcode layout fallback).
 * variant 2: second function without MDSZ (size fallback).
 * Layout mimics the 2015 file: the header's function-list size lands
 * on the last group's ENDT, and public metadata follows it directly. */

static uint8_t g_blob0[64];
static uint8_t g_blob1[64];
static size_t g_blob0_len;
static size_t g_blob1_len;

static uint8_t *build_metallib(int variant, size_t *out_len)
{
    tb_t b;
    uint8_t hash0[32];
    uint8_t hash1[32];
    size_t fl_off;
    size_t last_endt_off = 0;
    size_t pub_off;
    size_t priv_off;
    size_t bc_off;
    uint32_t i;
    int f;

    memset(&b, 0, sizeof(b));
    g_blob0_len = make_blob(g_blob0, 0xaa, 8);
    g_blob1_len = make_blob(g_blob1, 0xbb, 12);
    for (i = 0; i < 32; i++) {
        hash0[i] = (uint8_t)(0x10 + i);
        hash1[i] = (uint8_t)(0x80 + i);
    }

    /* Header placeholder (88 bytes, patched below). */
    for (i = 0; i < ML_HEADER_SIZE; i++)
        tb_u8(&b, 0);

    /* Function list. */
    fl_off = b.len;
    tb_u32(&b, 2); /* count */
    for (f = 0; f < 2; f++) {
        tb_t g;
        const char *name = (f == 0) ? "vertexMain" : "fragmentMain";
        uint8_t type = (uint8_t)f; /* 0 vertex, 1 fragment */
        uint64_t mdsz = (f == 0) ? g_blob0_len : g_blob1_len;
        uint64_t offt0 = 8;
        uint64_t offt1 = 8;
        uint64_t offt2 = (f == 0) ? 0 : g_blob0_len;
        uint16_t vers[4] = {2, 0, 2, 6};
        uint8_t *hash = (f == 0) ? hash0 : hash1;

        memset(&g, 0, sizeof(g));
        tb_tag(&g, "NAME", name, (uint16_t)(strlen(name) + 1));
        tb_tag(&g, "TYPE", &type, 1);
        tb_tag(&g, "HASH", hash, 32);
        if (variant != 2 || f == 0)
            tb_tag(&g, "MDSZ", &mdsz, 8);
        if (variant != 1) {
            uint8_t offt[24];
            size_t k;
            for (k = 0; k < 8; k++) {
                offt[k] = (uint8_t)((offt0 >> (8 * k)) & 0xff);
                offt[8 + k] = (uint8_t)((offt1 >> (8 * k)) & 0xff);
                offt[16 + k] = (uint8_t)((offt2 >> (8 * k)) & 0xff);
            }
            tb_tag(&g, "OFFT", offt, 24);
        }
        tb_tag(&g, "VERS", vers, 8);
        /* The group size includes its own 4-byte size field. */
        tb_u32(&b, (uint32_t)(4 + g.len + 4)); /* size incl. ENDT */
        last_endt_off = b.len + g.len;
        tb_bytes(&b, g.buf, g.len);
        tb_endt(&b);
        free(g.buf);
    }

    /* Public metadata: two empty groups, 2015-file style. */
    pub_off = b.len;
    tb_u32(&b, 4);
    tb_endt(&b);
    tb_u32(&b, 4);
    tb_endt(&b);

    /* Private metadata: same shape. */
    priv_off = b.len;
    tb_u32(&b, 4);
    tb_endt(&b);
    tb_u32(&b, 4);
    tb_endt(&b);

    /* Bitcode section: the two blobs back to back. */
    bc_off = b.len;
    tb_bytes(&b, g_blob0, g_blob0_len);
    tb_bytes(&b, g_blob1, g_blob1_len);

    /* Patch the header. The function-list size lands on the last
     * group's ENDT, exactly like the 2015 file. */
    tb_patch_u32(&b, 0, 0x424c544d); /* "MTLB" */
    tb_patch_u32(&b, 4, 0x00020001u); /* platform iOS, version major 2 */
    tb_patch_u32(&b, 8, 0x00000000u); /* version minor 0, type 0, os 0x82 */
    b.buf[10] = 0;   /* executable */
    b.buf[11] = 0x82; /* iOS */
    tb_patch_u32(&b, 12, 0x0006001au); /* os major 26, minor 6 */
    tb_patch_u64(&b, 16, b.len);
    tb_patch_u64(&b, 24, fl_off);
    tb_patch_u64(&b, 32, last_endt_off - fl_off);
    tb_patch_u64(&b, 40, pub_off);
    tb_patch_u64(&b, 48, priv_off - pub_off);
    tb_patch_u64(&b, 56, priv_off);
    tb_patch_u64(&b, 64, bc_off - priv_off);
    tb_patch_u64(&b, 72, bc_off);
    tb_patch_u64(&b, 80, g_blob0_len + g_blob1_len);

    *out_len = b.len;
    return b.buf;
}

/* ------------------------------------------------------------------ */

static ml_library_t g_lib; /* ~20 KiB; keep off the stack */

static void test_full_file(void)
{
    size_t len = 0;
    uint8_t *file = build_metallib(0, &len);
    const uint8_t *blob;
    uint32_t blob_size;
    int rc;

    rc = ml_open(file, (uint32_t)len, &g_lib);
    CHECK_ERR(rc, ML_OK);
    if (rc != ML_OK) {
        free(file);
        return;
    }
    CHECK(g_lib.count == 2);
    CHECK(g_lib.header.platform == ML_PLATFORM_IOS);
    CHECK(g_lib.header.version_major == 2);
    CHECK(g_lib.header.version_minor == 0);
    CHECK(g_lib.header.lib_type == ML_TYPE_EXECUTABLE);
    CHECK(g_lib.header.target_os == ML_OS_IOS);
    CHECK(g_lib.header.os_major == 26);
    CHECK(g_lib.header.os_minor == 6);
    CHECK(g_lib.header.file_size == len);
    CHECK(strcmp(ml_platform_name(g_lib.header.platform), "iOS") == 0);
    CHECK(strcmp(ml_target_os_name(g_lib.header.target_os), "iOS") == 0);

    CHECK(strcmp(g_lib.funcs[0].name, "vertexMain") == 0);
    CHECK(g_lib.funcs[0].name_len == 10);
    CHECK(g_lib.funcs[0].func_type == ML_FUNC_VERTEX);
    CHECK(strcmp(ml_func_type_name(0), "vertex") == 0);
    CHECK(g_lib.funcs[0].bitcode_size == g_blob0_len);
    CHECK(g_lib.funcs[0].bitcode_off == g_lib.header.bitcode_off);
    CHECK(g_lib.funcs[0].bitcode_ok == 1);
    CHECK(g_lib.funcs[0].has_offt == 1);
    CHECK(g_lib.funcs[0].offt[2] == 0);
    CHECK(g_lib.funcs[0].air_major == 2);
    CHECK(g_lib.funcs[0].lang_major == 2);
    CHECK(g_lib.funcs[0].lang_minor == 6);
    CHECK(g_lib.funcs[0].has_hash == 1);
    CHECK(g_lib.funcs[0].hash[0] == 0x10);
    CHECK(g_lib.funcs[0].hash[31] == 0x2f);

    CHECK(strcmp(g_lib.funcs[1].name, "fragmentMain") == 0);
    CHECK(g_lib.funcs[1].func_type == ML_FUNC_FRAGMENT);
    CHECK(g_lib.funcs[1].bitcode_size == g_blob1_len);
    CHECK(g_lib.funcs[1].bitcode_off ==
          g_lib.header.bitcode_off + g_blob0_len);
    CHECK(g_lib.funcs[1].offt[2] == g_blob0_len);
    CHECK(g_lib.funcs[1].bitcode_ok == 1);

    CHECK(ml_find(&g_lib, "vertexMain") == 0);
    CHECK(ml_find(&g_lib, "fragmentMain") == 1);
    CHECK(ml_find(&g_lib, "nope") == -1);

    rc = ml_function_bitcode(&g_lib, 0, &blob, &blob_size);
    CHECK_ERR(rc, ML_OK);
    CHECK(blob_size == g_blob0_len);
    CHECK(memcmp(blob, g_blob0, g_blob0_len) == 0);
    rc = ml_function_bitcode(&g_lib, 1, &blob, &blob_size);
    CHECK_ERR(rc, ML_OK);
    CHECK(blob_size == g_blob1_len);
    CHECK(memcmp(blob, g_blob1, g_blob1_len) == 0);
    CHECK_ERR(ml_function_bitcode(&g_lib, 2, &blob, &blob_size),
              ML_ERR_BAD_INDEX);

    free(file);
}

static void test_sequential_fallback(void)
{
    size_t len = 0;
    uint8_t *file = build_metallib(1, &len);
    int rc = ml_open(file, (uint32_t)len, &g_lib);
    CHECK_ERR(rc, ML_OK);
    if (rc == ML_OK) {
        CHECK(g_lib.count == 2);
        CHECK(g_lib.funcs[0].has_offt == 0);
        CHECK(g_lib.funcs[0].bitcode_off == g_lib.header.bitcode_off);
        CHECK(g_lib.funcs[1].bitcode_off ==
              g_lib.header.bitcode_off + g_blob0_len);
        CHECK(g_lib.funcs[1].bitcode_ok == 1);
    }
    free(file);
}

static void test_mdsz_fallback(void)
{
    size_t len = 0;
    uint8_t *file = build_metallib(2, &len);
    int rc = ml_open(file, (uint32_t)len, &g_lib);
    CHECK_ERR(rc, ML_OK);
    if (rc == ML_OK) {
        CHECK(g_lib.funcs[1].has_mdsz == 0);
        /* Second function size falls back to the rest of the section. */
        CHECK(g_lib.funcs[1].bitcode_size == g_blob1_len);
        CHECK(g_lib.funcs[1].bitcode_ok == 1);
    }
    free(file);
}

static void test_error_paths(void)
{
    size_t len = 0;
    uint8_t *file = build_metallib(0, &len);
    uint8_t *bad;
    int rc;

    /* Bad magic. */
    bad = (uint8_t *)malloc(len);
    memcpy(bad, file, len);
    bad[0] = 'X';
    CHECK_ERR(ml_open(bad, (uint32_t)len, &g_lib), ML_ERR_BAD_MAGIC);
    free(bad);

    /* Truncated: header file size disagrees with the buffer. */
    CHECK_ERR(ml_open(file, (uint32_t)len - 1, &g_lib),
              ML_ERR_BAD_FILE_SIZE);

    /* Tiny buffer. */
    CHECK_ERR(ml_open(file, 10, &g_lib), ML_ERR_TOO_SMALL);

    /* Bitcode section offset runs past the file. */
    bad = (uint8_t *)malloc(len);
    memcpy(bad, file, len);
    bad[72] = 0xff;
    bad[73] = 0xff;
    bad[74] = 0xff;
    bad[75] = 0xff;
    CHECK_ERR(ml_open(bad, (uint32_t)len, &g_lib), ML_ERR_BAD_SECTION);
    free(bad);

    /* Function count too large. */
    bad = (uint8_t *)malloc(len);
    memcpy(bad, file, len);
    /* count lives at the function list offset (88). */
    bad[88] = 0xff;
    bad[89] = 0xff;
    bad[90] = 0x00;
    bad[91] = 0x00;
    rc = ml_open(bad, (uint32_t)len, &g_lib);
    CHECK(rc == ML_ERR_TOO_MANY);
    free(bad);

    /* Corrupt the first ENDT (at offset 88 + 4 + 4 + group tags...).
     * Simpler: corrupt a NAME tag code so the group has no NAME. */
    bad = (uint8_t *)malloc(len);
    memcpy(bad, file, len);
    /* First group: size at 92, NAME tag at 96. */
    bad[96] = 'X';
    rc = ml_open(bad, (uint32_t)len, &g_lib);
    CHECK(rc == ML_ERR_NO_NAME);
    free(bad);

    free(file);
}

/* ------------------------------------------------------------------ */
/* Fuzz: mutate/truncate the good file; the reader must never crash
 * and must never read out of bounds (ASan/UBSan catch that). */

static uint32_t g_seed = 0x12345678u;

static uint32_t rnd32(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}

static void fuzz_once(const uint8_t *good, size_t len, uint8_t *scratch)
{
    uint32_t mode = rnd32() % 4;
    size_t n = len;
    size_t i;
    int rc;

    memcpy(scratch, good, len);
    if (mode == 0) {
        /* Truncate. */
        n = rnd32() % (len + 1);
    } else if (mode == 1) {
        /* Flip a few random bytes. */
        uint32_t flips = 1 + rnd32() % 8;
        for (i = 0; i < flips; i++)
            scratch[rnd32() % len] = (uint8_t)rnd32();
    } else if (mode == 2) {
        /* Random 4-byte word overwrite. */
        size_t off = rnd32() % (len - 4);
        uint32_t v = rnd32();
        memcpy(scratch + off, &v, 4);
    } else {
        /* Random single-byte zeroing in the header area. */
        scratch[rnd32() % 88] = 0;
    }
    rc = ml_open(scratch, (uint32_t)n, &g_lib);
    (void)rc; /* any return code is fine; crashing is not */
    if (rc == ML_OK) {
        /* If it parsed, extraction must stay in bounds too. */
        uint32_t j;
        for (j = 0; j < g_lib.count; j++) {
            const uint8_t *blob;
            uint32_t blob_size;
            (void)ml_function_bitcode(&g_lib, j, &blob, &blob_size);
        }
    }
}

int main(int argc, char **argv)
{
    size_t len = 0;
    uint8_t *good = build_metallib(0, &len);
    uint8_t *scratch = (uint8_t *)malloc(len);
    uint32_t iterations = 20000;
    uint32_t i;

    if (argc > 1)
        iterations = (uint32_t)atoi(argv[1]);
    if (argc > 2) {
        /* Dump the synthetic metallib for tools/metallib_scan.py. */
        FILE *f = fopen(argv[2], "wb");
        if (f == NULL) {
            printf("cannot write %s\n", argv[2]);
            return 1;
        }
        if (fwrite(good, 1, len, f) != len) {
            printf("short write %s\n", argv[2]);
            fclose(f);
            return 1;
        }
        fclose(f);
        printf("wrote synthetic metallib (%zu bytes) to %s\n",
               len, argv[2]);
    }

    test_full_file();
    test_sequential_fallback();
    test_mdsz_fallback();
    test_error_paths();

    for (i = 0; i < iterations; i++)
        fuzz_once(good, len, scratch);

    printf("metallib_test: %d checks, %d failures, %u fuzz iterations\n",
           g_checks, g_failures, iterations);
    free(good);
    free(scratch);
    return g_failures == 0 ? 0 : 1;
}
