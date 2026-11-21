/* bitcode_test.c - host unit test for the freestanding bitcode reader.
 *
 * Plain cc, no NDK. The library itself uses no libc; this harness does
 * (stdio, stdlib, string) only to drive it and to report results.
 * Builds synthetic LLVM bitcode in memory with a small bit writer,
 * then checks the reader walks the exact expected event sequence
 * (blocks, abbrev definitions, records, arrays, blobs, BLOCKINFO
 * seeding, per-block abbrev scoping), plus error paths and a fuzz loop.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../src \
 *      -o bitcode_test bitcode_test.c ../../src/bitcode.c
 *   ./bitcode_test [fuzz_iterations]
 *
 * Sanitizers (fuzz must be clean under these):
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../src \
 *      -o bitcode_test_asan bitcode_test.c ../../src/bitcode.c
 *   ./bitcode_test_asan [fuzz_iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bitcode.h"

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
               __FILE__, __LINE__, #call, _rc, bc_error_string(_rc), \
               (want)); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Bit writer (test only). Bits are appended least significant bit
 * first within each byte, matching the reader. */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bitpos;
} bw_t;

static void bw_grow(bw_t *w, size_t nbits)
{
    size_t need = (w->bitpos + nbits + 7) / 8;
    if (need > w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 64;
        uint8_t *nb;
        while (ncap < need)
            ncap *= 2;
        nb = (uint8_t *)realloc(w->buf, ncap);
        if (nb == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        memset(nb + w->cap, 0, ncap - w->cap);
        w->buf = nb;
        w->cap = ncap;
    }
}

static void bw_bits(bw_t *w, uint64_t v, uint32_t n)
{
    uint32_t i;
    bw_grow(w, n);
    for (i = 0; i < n; i++) {
        size_t byte = w->bitpos / 8;
        uint32_t bit = (uint32_t)(w->bitpos % 8);
        if ((v >> i) & 1)
            w->buf[byte] |= (uint8_t)(1u << bit);
        w->bitpos++;
    }
}

static void bw_vbr(bw_t *w, uint64_t v, uint32_t width)
{
    uint64_t mask = (width - 1 >= 64) ? ~0ULL : ((1ULL << (width - 1)) - 1);
    for (;;) {
        uint64_t chunk = v & mask;
        v >>= (width - 1);
        if (v != 0) {
            bw_bits(w, chunk | (1ULL << (width - 1)), width);
        } else {
            bw_bits(w, chunk, width);
            break;
        }
    }
}

static void bw_align32(bw_t *w)
{
    uint32_t rem = (uint32_t)(w->bitpos % 32);
    if (rem != 0)
        bw_bits(w, 0, 32 - rem);
}

static void bw_bytes(bw_t *w, const uint8_t *p, size_t n)
{
    size_t i;
    /* Caller must have aligned to a byte boundary. */
    for (i = 0; i < n; i++) {
        bw_grow(w, 8);
        w->buf[w->bitpos / 8] = p[i];
        w->bitpos += 8;
    }
}

static void bw_u32_at(bw_t *w, size_t off, uint32_t v)
{
    w->buf[off] = (uint8_t)(v & 0xff);
    w->buf[off + 1] = (uint8_t)((v >> 8) & 0xff);
    w->buf[off + 2] = (uint8_t)((v >> 16) & 0xff);
    w->buf[off + 3] = (uint8_t)((v >> 24) & 0xff);
}

/* One abbrev operand for the test writer. */
typedef struct {
    uint8_t lit; /* 1 = literal, 0 = encoding */
    uint8_t enc; /* BC_OP_* when not a literal */
    uint64_t val; /* literal value, or Fixed/VBR width */
} tabbrev_op_t;

static void bw_abbrev(bw_t *w, uint32_t width, const tabbrev_op_t *ops,
                      uint32_t n)
{
    uint32_t i;
    bw_bits(w, BC_DEFINE_ABBREV, width);
    bw_vbr(w, n, 5);
    for (i = 0; i < n; i++) {
        bw_bits(w, ops[i].lit ? 1 : 0, 1);
        if (ops[i].lit) {
            bw_vbr(w, ops[i].val, 8);
        } else {
            bw_bits(w, ops[i].enc, 3);
            if (ops[i].enc == BC_OP_FIXED || ops[i].enc == BC_OP_VBR)
                bw_vbr(w, ops[i].val, 5);
        }
    }
}

static void bw_unabbrev(bw_t *w, uint32_t width, uint32_t code,
                        const uint64_t *ops, uint32_t n)
{
    uint32_t i;
    bw_bits(w, BC_UNABBREV_RECORD, width);
    bw_vbr(w, code, 6);
    bw_vbr(w, n, 6);
    for (i = 0; i < n; i++)
        bw_vbr(w, ops[i], 6);
}

static void bw_end_block(bw_t *w, uint32_t width)
{
    bw_bits(w, BC_END_BLOCK, width);
    bw_align32(w);
}

/* Wrap already-aligned content bits as a subblock into w. */
static void bw_subblock(bw_t *w, uint32_t cur_width, uint32_t blockid,
                        uint32_t newwidth, bw_t *content)
{
    bw_bits(w, BC_ENTER_SUBBLOCK, cur_width);
    bw_vbr(w, blockid, 8);
    bw_vbr(w, newwidth, 4);
    bw_align32(w);
    bw_bits(w, (uint64_t)(content->bitpos / 32), 32);
    bw_bytes(w, content->buf, content->bitpos / 8);
}

/* ------------------------------------------------------------------ */
/* Synthetic stream. */

static uint8_t *build_stream(size_t *out_len)
{
    bw_t stream;
    bw_t bi;
    bw_t mod;
    bw_t fn;
    uint64_t setbid_op[1] = {BC_MODULE_BLOCK_ID};
    uint64_t version_op[1] = {2};
    tabbrev_op_t bi_abbrev[2] = {
        {1, 0, 7},              /* literal record code 7 (GLOBALVAR) */
        {0, BC_OP_FIXED, 2},    /* one Fixed(2) operand */
    };
    tabbrev_op_t fn_abbrev[3] = {
        {1, 0, 8},              /* literal record code 8 (FUNCTION) */
        {0, BC_OP_FIXED, 4},
        {0, BC_OP_VBR, 6},
    };
    tabbrev_op_t ret_abbrev[3] = {
        {1, 0, 10},             /* literal record code 10 (RET) */
        {0, BC_OP_FIXED, 3},
        {0, BC_OP_VBR, 6},
    };
    tabbrev_op_t triple_abbrev[3] = {
        {1, 0, 2},              /* literal record code 2 (TRIPLE) */
        {0, BC_OP_ARRAY, 0},
        {0, BC_OP_CHAR6, 0},
    };
    tabbrev_op_t blob_abbrev[2] = {
        {1, 0, 3},              /* literal record code 3 */
        {0, BC_OP_BLOB, 0},
    };
    size_t total;

    memset(&stream, 0, sizeof(stream));
    memset(&bi, 0, sizeof(bi));
    memset(&mod, 0, sizeof(mod));
    memset(&fn, 0, sizeof(fn));

    /* BLOCKINFO block: SETBID 8, then an abbrev for the MODULE block. */
    bw_unabbrev(&bi, 2, BC_BI_SETBID, setbid_op, 1);
    bw_abbrev(&bi, 2, bi_abbrev, 2);
    bw_end_block(&bi, 2);

    /* FUNCTION sub-block: its own RET abbrev, one RET record. */
    bw_abbrev(&fn, 3, ret_abbrev, 3);
    bw_bits(&fn, 4, 3); /* abbrev id 4 */
    bw_bits(&fn, 1, 3); /* Fixed(3) operand */
    bw_vbr(&fn, 42, 6); /* VBR(6) operand */
    bw_end_block(&fn, 3);

    /* MODULE block. */
    bw_bits(&mod, 4, 4); /* seeded BLOCKINFO abbrev id 4 (GLOBALVAR) */
    bw_bits(&mod, 3, 2); /* Fixed(2) operand */
    bw_abbrev(&mod, 4, fn_abbrev, 3); /* FUNCTION abbrev, id 5 */
    bw_bits(&mod, 5, 4); /* abbrev id 5 */
    bw_bits(&mod, 5, 4); /* Fixed(4) operand */
    bw_vbr(&mod, 300, 6); /* VBR(6) operand */
    bw_unabbrev(&mod, 4, BC_MOD_VERSION, version_op, 1);
    bw_subblock(&mod, 4, BC_FUNCTION_BLOCK_ID, 3, &fn);
    bw_abbrev(&mod, 4, triple_abbrev, 3); /* TRIPLE abbrev, id 6 */
    bw_bits(&mod, 6, 4); /* abbrev id 6 */
    bw_vbr(&mod, 3, 6); /* array count */
    bw_bits(&mod, 0, 6); /* 'a' */
    bw_bits(&mod, 1, 6); /* 'b' */
    bw_bits(&mod, 2, 6); /* 'c' */
    bw_abbrev(&mod, 4, blob_abbrev, 2); /* blob abbrev, id 7 */
    bw_bits(&mod, 7, 4); /* abbrev id 7 */
    bw_vbr(&mod, 5, 6); /* blob length */
    bw_align32(&mod);
    bw_bytes(&mod, (const uint8_t *)"hello", 5);
    bw_align32(&mod);
    bw_end_block(&mod, 4);

    /* Top level: wrapper + "BC" magic + BLOCKINFO + MODULE. */
    bw_bytes(&stream, (const uint8_t *)
             "\xde\xc0\x17\x0b" /* wrapper magic */
             "\x00\x00\x00\x00" /* version 0 */
             "\x14\x00\x00\x00" /* offset 20 */
             "\x00\x00\x00\x00" /* size, patched below */
             "\xff\xff\xff\xff", /* cputype -1 */
             20);
    bw_bytes(&stream, (const uint8_t *)"\x42\x43\xc0\xde", 4); /* "BC" */
    bw_subblock(&stream, 2, BC_BLOCKINFO_BLOCK_ID, 2, &bi);
    bw_subblock(&stream, 2, BC_MODULE_BLOCK_ID, 4, &mod);

    total = stream.bitpos / 8;
    bw_u32_at(&stream, 12, (uint32_t)(total - 20)); /* wrapper size */

    free(bi.buf);
    free(mod.buf);
    free(fn.buf);
    *out_len = total;
    return stream.buf;
}

/* ------------------------------------------------------------------ */
/* Expected event sequence. */

typedef struct {
    bc_event_kind_t kind;
    uint32_t block_id;
    uint32_t code;
    uint32_t abbrev_id;
    uint32_t op_count;
    uint64_t ops[6];
    const char *blob;
    uint32_t blob_len;
} expect_t;

static const expect_t k_expect[] = {
    {BC_EV_BLOCK_ENTER, BC_BLOCKINFO_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_RECORD, 0, BC_BI_SETBID, 0, 1, {8}, NULL, 0},
    {BC_EV_BLOCK_END, BC_BLOCKINFO_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_BLOCK_ENTER, BC_MODULE_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_RECORD, 0, 7, 4, 1, {3}, NULL, 0},
    {BC_EV_RECORD, 0, 8, 5, 2, {5, 300}, NULL, 0},
    {BC_EV_RECORD, 0, BC_MOD_VERSION, 0, 1, {2}, NULL, 0},
    {BC_EV_BLOCK_ENTER, BC_FUNCTION_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_RECORD, 0, 10, 4, 2, {1, 42}, NULL, 0},
    {BC_EV_BLOCK_END, BC_FUNCTION_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_RECORD, 0, 2, 6, 4, {3, 0, 1, 2}, NULL, 0},
    {BC_EV_RECORD, 0, 3, 7, 1, {5}, "hello", 5},
    {BC_EV_BLOCK_END, BC_MODULE_BLOCK_ID, 0, 0, 0, {0}, NULL, 0},
    {BC_EV_EOF, 0, 0, 0, 0, {0}, NULL, 0},
};

static bc_reader_t g_reader; /* ~10 KiB; keep off the stack */

static void test_walk(void)
{
    size_t len = 0;
    uint8_t *stream = build_stream(&len);
    size_t n_expect = sizeof(k_expect) / sizeof(k_expect[0]);
    size_t i;
    int rc = bc_open(stream, (uint32_t)len, &g_reader);
    CHECK_ERR(rc, BC_OK);
    if (rc != BC_OK) {
        free(stream);
        return;
    }
    for (i = 0; i < n_expect; i++) {
        bc_event_t ev;
        const expect_t *ex = &k_expect[i];
        rc = bc_next(&g_reader, &ev);
        CHECK_ERR(rc, BC_OK);
        if (rc != BC_OK)
            break;
        CHECK(ev.kind == ex->kind);
        if (ex->kind == BC_EV_BLOCK_ENTER ||
            ex->kind == BC_EV_BLOCK_END) {
            CHECK(ev.block_id == ex->block_id);
        } else if (ex->kind == BC_EV_RECORD) {
            uint32_t j;
            CHECK(ev.code == ex->code);
            CHECK(ev.abbrev_id == ex->abbrev_id);
            CHECK(ev.op_count == ex->op_count);
            for (j = 0; j < ex->op_count && j < ev.op_count; j++)
                CHECK(ev.ops[j] == ex->ops[j]);
            if (ex->blob != NULL) {
                CHECK(ev.has_blob == 1);
                CHECK(ev.blob_len == ex->blob_len);
                CHECK(ev.blob != NULL &&
                      memcmp(ev.blob, ex->blob, ex->blob_len) == 0);
            } else {
                CHECK(ev.has_blob == 0);
            }
        }
    }
    free(stream);
}

static void test_error_paths(void)
{
    size_t len = 0;
    uint8_t *stream = build_stream(&len);
    uint8_t *bad;
    int rc;
    bc_event_t ev;

    /* Bad magic. */
    bad = (uint8_t *)malloc(len);
    memcpy(bad, stream, len);
    bad[0] = 'X';
    CHECK_ERR(bc_open(bad, (uint32_t)len, &g_reader), BC_ERR_BAD_MAGIC);
    free(bad);

    /* Truncated buffer: wrapper says more than we have. */
    CHECK_ERR(bc_open(stream, (uint32_t)len - 4, &g_reader),
              BC_ERR_TRUNCATED);

    /* Tiny buffer. */
    CHECK_ERR(bc_open(stream, 2, &g_reader), BC_ERR_TOO_SMALL);

    /* Unknown abbrev id: a module block with a record using id 9. */
    {
        bw_t s;
        bw_t m;
        uint64_t vop[1] = {2};
        memset(&s, 0, sizeof(s));
        memset(&m, 0, sizeof(m));
        bw_unabbrev(&m, 4, BC_MOD_VERSION, vop, 1);
        bw_bits(&m, 9, 4); /* abbrev id 9, never defined */
        bw_bits(&m, 0, 4);
        bw_end_block(&m, 4);
        bw_bytes(&s, (const uint8_t *)"\x42\x43\xc0\xde", 4);
        bw_subblock(&s, 2, BC_MODULE_BLOCK_ID, 4, &m);
        rc = bc_open(s.buf, (uint32_t)(s.bitpos / 8), &g_reader);
        CHECK_ERR(rc, BC_OK);
        if (rc == BC_OK) {
            rc = bc_next(&g_reader, &ev); /* enter module */
            CHECK_ERR(rc, BC_OK);
            rc = bc_next(&g_reader, &ev); /* version record */
            CHECK_ERR(rc, BC_OK);
            rc = bc_next(&g_reader, &ev); /* bad abbrev */
            CHECK_ERR(rc, BC_ERR_BAD_ABBREV);
        }
        free(m.buf);
        free(s.buf);
    }

    /* Missing END_BLOCK: the block content must end exactly at a
     * record boundary, otherwise trailing zero padding parses as an
     * END_BLOCK record (code 0 + alignment). DEFINE_ABBREV [literal 1,
     * Fixed(1)] is 27 bits; one abbreviated record is 5 bits; together
     * exactly one 32-bit word, with no END_BLOCK and no padding. */
    {
        bw_t s;
        bw_t m;
        tabbrev_op_t one_abbrev[2] = {
            {1, 0, 1},              /* literal record code 1 */
            {0, BC_OP_FIXED, 1},    /* one Fixed(1) operand */
        };
        memset(&s, 0, sizeof(s));
        memset(&m, 0, sizeof(m));
        bw_abbrev(&m, 4, one_abbrev, 2);
        bw_bits(&m, 4, 4); /* abbrev id 4 */
        bw_bits(&m, 0, 1); /* Fixed(1) operand */
        /* No END_BLOCK, no alignment: content is exactly 32 bits. */
        bw_bytes(&s, (const uint8_t *)"\x42\x43\xc0\xde", 4);
        bw_subblock(&s, 2, BC_MODULE_BLOCK_ID, 4, &m);
        rc = bc_open(s.buf, (uint32_t)(s.bitpos / 8), &g_reader);
        CHECK_ERR(rc, BC_OK);
        if (rc == BC_OK) {
            rc = bc_next(&g_reader, &ev); /* enter module */
            CHECK_ERR(rc, BC_OK);
            rc = bc_next(&g_reader, &ev); /* the one record */
            CHECK_ERR(rc, BC_OK);
            CHECK(ev.kind == BC_EV_RECORD);
            rc = bc_next(&g_reader, &ev); /* missing END_BLOCK */
            CHECK_ERR(rc, BC_ERR_NO_END_BLOCK);
        }
        free(m.buf);
        free(s.buf);
    }

    free(stream);
}

/* ------------------------------------------------------------------ */

static uint32_t g_seed = 0xabcdef01u;

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
        n = rnd32() % (len + 1);
    } else if (mode == 1) {
        uint32_t flips = 1 + rnd32() % 8;
        for (i = 0; i < flips; i++)
            scratch[rnd32() % len] = (uint8_t)rnd32();
    } else if (mode == 2) {
        size_t off = rnd32() % (len - 4);
        uint32_t v = rnd32();
        memcpy(scratch + off, &v, 4);
    } else {
        scratch[rnd32() % 64] = (uint8_t)rnd32();
    }
    rc = bc_open(scratch, (uint32_t)n, &g_reader);
    if (rc == BC_OK) {
        /* Walk to EOF or error, bounded so a corrupt stream cannot
         * spin forever (each event consumes at least one bit, so the
         * walk is bounded by the stream length in practice; the cap
         * is a belt-and-braces guard). */
        uint32_t steps = 0;
        for (;;) {
            bc_event_t ev;
            rc = bc_next(&g_reader, &ev);
            if (rc != BC_OK || ev.kind == BC_EV_EOF)
                break;
            if (++steps > 100000)
                break;
        }
    }
}

int main(int argc, char **argv)
{
    size_t len = 0;
    uint8_t *good = build_stream(&len);
    uint8_t *scratch = (uint8_t *)malloc(len);
    uint32_t iterations = 20000;
    uint32_t i;

    if (argc > 1)
        iterations = (uint32_t)atoi(argv[1]);

    test_walk();
    test_error_paths();

    for (i = 0; i < iterations; i++)
        fuzz_once(good, len, scratch);

    printf("bitcode_test: %d checks, %d failures, %u fuzz iterations\n",
           g_checks, g_failures, iterations);
    free(good);
    free(scratch);
    return g_failures == 0 ? 0 : 1;
}
