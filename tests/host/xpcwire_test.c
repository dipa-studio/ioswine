/* xpcwire_test.c - host unit test for the freestanding XPC wire library.
 *
 * Plain cc, no NDK. The library itself uses no libc; this harness does
 * (stdio, stdlib, string) only to drive it and to report results.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../libsystem_kernel/src \
 *      -o xpcwire_test xpcwire_test.c ../../libsystem_kernel/src/xpcwire.c
 *   ./xpcwire_test [fuzz_iterations]
 *
 * Sanitizers (fuzz must be clean under these):
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../libsystem_kernel/src \
 *      -o xpcwire_test_asan xpcwire_test.c ../../libsystem_kernel/src/xpcwire.c
 *   ./xpcwire_test_asan [fuzz_iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xpcwire.h"

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
        printf("FAIL %s:%d: %s returned %d, want %d\n", \
               __FILE__, __LINE__, #call, _rc, (want)); \
    } \
} while (0)

static void dump_hex(const char *tag, const uint8_t *p, uint32_t n) {
    uint32_t i;
    printf("%s (%u bytes):", tag, n);
    for (i = 0; i < n; i++) {
        if (i % 16 == 0) printf("\n  ");
        printf("%02x ", p[i]);
    }
    printf("\n");
}

static int buf_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    return memcmp(a, b, n) == 0;
}

/* ------------------------------------------------------------------ */
/* Golden byte vectors.       */
/* ------------------------------------------------------------------ */

/* {error: int64 0} reply, 40 bytes (launchd.c xpc_serialize_error_reply). */
static const uint8_t kGoldenErrorReply[40] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x65, 0x72, 0x72, 0x6f, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Empty dictionary reply, 20 bytes (launchd.c xpc_serialize_empty_reply). */
static const uint8_t kGoldenEmptyReply[20] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* Bootstrap look_up reply, 96 bytes: {error:0, req_pid:1234,
 * rec_execcnt:1, port:mach_send}. Shape from bootstrap-register.md
 * section 1d/3 and launchd.c xpc_serialize_lookup_dict (req_pid is
 * getpid() on a real reply; 1234 is the test stand-in).
 */
static const uint8_t kGoldenLookupReply[96] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00,
    0x65, 0x72, 0x72, 0x6f, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x72, 0x65, 0x71, 0x5f, 0x70, 0x69, 0x64, 0x00,
    0x00, 0x40, 0x00, 0x00,
    0xd2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x72, 0x65, 0x63, 0x5f, 0x65, 0x78, 0x65, 0x63,
    0x63, 0x6e, 0x74, 0x00,
    0x00, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0x6f, 0x72, 0x74, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xd0, 0x00, 0x00
};

/* {"key": "value"}, 40 bytes. Dict portion is the iOS 9 slides
 * "Simple Dictionary" hex dump, with the current magic ("CPX@") and
 * version 5 in place of the iOS 9 "!CPX" and version 4.
 */
static const uint8_t kGoldenSimpleDict[40] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x6b, 0x65, 0x79, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00, 0x00
};

/* {"auint64": 0x4141414141414141, "key": "value"}, 60 bytes. Dict
 * portion is the iOS 9 slides "Bigger Dictionary" hex dump, same
 * magic/version update as above. Entry order is insertion order.
 */
static const uint8_t kGoldenBiggerDict[60] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x61, 0x75, 0x69, 0x6e, 0x74, 0x36, 0x34, 0x00,
    0x00, 0x40, 0x00, 0x00,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x6b, 0x65, 0x79, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x00, 0x00
};

/* {"ABC": xpc_connection}, 28 bytes. Dict portion is the iOS 9 slides
 * "Dictionary with port" hex dump (connection type 0x13000, no body
 * payload, one port descriptor in the envelope). Hand built here
 * because the builder has no connection adder; the reader must parse it.
 */
static const uint8_t kGoldenConnectionDict[28] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x41, 0x42, 0x43, 0x00,
    0x00, 0x30, 0x01, 0x00
};

static const uint8_t kTestUuid[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

/* Dummy port objects for tests (stand-ins for ipc_port* values). */
#define PORT_FD        ((uint64_t)0x2001)
#define PORT_SEND      ((uint64_t)0x2002)
#define PORT_RECV      ((uint64_t)0x2003)
#define PORT_ONCE      ((uint64_t)0x2004)
#define PORT_SHMEM     ((uint64_t)0x2005)
#define PORT_OOL       ((uint64_t)0x2006)
#define PORT_LOOKUP    ((uint64_t)0x1003)

static void make_dummy_descs(xw_desc *descs, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        descs[i].dtype = XW_DESC_PORT;
        descs[i].disposition = 0x13; /* COPY_SEND */
        descs[i].obj = 0x2000 + i;
        descs[i].size = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Golden vectors                                                      */
/* ------------------------------------------------------------------ */

static void test_golden_vectors(void) {
    uint8_t buf[256];
    xw_builder b;
    xw_reader r;
    xw_value v;
    xw_desc descs[4];
    int64_t i64;
    uint64_t u64;
    int boolean;
    double dbl;
    const char *str;
    uint32_t slen, desc_idx, dlen;
    const uint8_t *ptr;

    /* {error: 0} via the builder must be byte identical to launchd.c. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "error", 0), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_error(&b) == XW_OK);
    CHECK(xw_builder_len(&b) == sizeof(kGoldenErrorReply));
    if (!buf_equal(buf, kGoldenErrorReply, sizeof(kGoldenErrorReply))) {
        g_failures++;
        printf("FAIL golden error reply bytes differ\n");
        dump_hex("got", buf, xw_builder_len(&b));
        dump_hex("want", kGoldenErrorReply, sizeof(kGoldenErrorReply));
    }

    /* The golden bytes parse back to error == 0. */
    CHECK_ERR(xw_reader_init(&r, kGoldenErrorReply, sizeof(kGoldenErrorReply),
                             NULL, 0), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "error", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == 0);
    CHECK_ERR(xw_dict_find(&r, "nope", &v), XW_ERR_NOT_FOUND);

    /* Empty reply golden bytes. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_len(&b) == sizeof(kGoldenEmptyReply));
    CHECK(buf_equal(buf, kGoldenEmptyReply, sizeof(kGoldenEmptyReply)));
    CHECK_ERR(xw_reader_init(&r, kGoldenEmptyReply, sizeof(kGoldenEmptyReply),
                             NULL, 0), XW_OK);

    /* Lookup reply golden bytes, then parse them with a descriptor. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "error", 0), XW_OK);
    CHECK_ERR(xw_dict_add_uint64(&b, "req_pid", 1234), XW_OK);
    CHECK_ERR(xw_dict_add_uint64(&b, "rec_execcnt", 1), XW_OK);
    CHECK_ERR(xw_dict_add_mach_send(&b, "port", PORT_LOOKUP, 0x13), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_len(&b) == sizeof(kGoldenLookupReply));
    if (!buf_equal(buf, kGoldenLookupReply, sizeof(kGoldenLookupReply))) {
        g_failures++;
        printf("FAIL golden lookup reply bytes differ\n");
        dump_hex("got", buf, xw_builder_len(&b));
        dump_hex("want", kGoldenLookupReply, sizeof(kGoldenLookupReply));
    }
    CHECK(b.ndescs == 1);
    CHECK(b.descs[0].obj == PORT_LOOKUP);
    CHECK(b.descs[0].disposition == 0x13);
    CHECK(b.descs[0].dtype == XW_DESC_PORT);

    make_dummy_descs(descs, 1);
    descs[0].obj = PORT_LOOKUP;
    CHECK_ERR(xw_reader_init(&r, kGoldenLookupReply, sizeof(kGoldenLookupReply),
                             descs, 1), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "error", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == 0);
    CHECK_ERR(xw_dict_find(&r, "req_pid", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 1234);
    CHECK_ERR(xw_dict_find(&r, "rec_execcnt", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 1);
    CHECK_ERR(xw_dict_find(&r, "port", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);

    /* iOS 9 simple dict via the builder. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "key", "value"), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_len(&b) == sizeof(kGoldenSimpleDict));
    CHECK(buf_equal(buf, kGoldenSimpleDict, sizeof(kGoldenSimpleDict)));
    CHECK_ERR(xw_reader_init(&r, kGoldenSimpleDict, sizeof(kGoldenSimpleDict),
                             NULL, 0), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "key", &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(slen == 6); /* includes the NUL */
    CHECK(strcmp(str, "value") == 0);

    /* iOS 9 bigger dict via the builder (insertion order matters). */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_uint64(&b, "auint64", 0x4141414141414141ULL), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "key", "value"), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_len(&b) == sizeof(kGoldenBiggerDict));
    CHECK(buf_equal(buf, kGoldenBiggerDict, sizeof(kGoldenBiggerDict)));
    CHECK_ERR(xw_reader_init(&r, kGoldenBiggerDict, sizeof(kGoldenBiggerDict),
                             NULL, 0), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "auint64", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 0x4141414141414141ULL);

    /* Connection dict: hand built bytes, reader must parse it. */
    make_dummy_descs(descs, 1);
    CHECK_ERR(xw_reader_init(&r, kGoldenConnectionDict,
                             sizeof(kGoldenConnectionDict), descs, 1), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "ABC", &v), XW_OK);
    CHECK(v.type == XW_TYPE_CONNECTION);
    CHECK_ERR(xw_as_connection(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);

    /* Old iOS 9 framing ("!CPX", version 4) must be rejected. */
    {
        uint8_t old[sizeof(kGoldenSimpleDict)];
        memcpy(old, kGoldenSimpleDict, sizeof(old));
        old[0] = 0x21; old[1] = 0x43; old[2] = 0x50; old[3] = 0x58; /* "!CPX" */
        old[4] = 4; old[5] = 0; old[6] = 0; old[7] = 0;           /* version 4 */
        CHECK_ERR(xw_reader_init(&r, old, sizeof(old), NULL, 0),
                  XW_ERR_BAD_MAGIC);
        memcpy(old, kGoldenSimpleDict, sizeof(old));
        old[4] = 4; /* version 4 with the new magic */
        CHECK_ERR(xw_reader_init(&r, old, sizeof(old), NULL, 0),
                  XW_ERR_BAD_VERSION);
        memcpy(old, kGoldenSimpleDict, sizeof(old));
        old[4] = 6; /* version 6 */
        CHECK_ERR(xw_reader_init(&r, old, sizeof(old), NULL, 0),
                  XW_ERR_BAD_VERSION);
    }

    /* Unused variable guards for the getters exercised elsewhere. */
    (void)boolean; (void)dbl; (void)ptr; (void)dlen;
}

/* ------------------------------------------------------------------ */
/* Round-trip: every type in a dictionary and in an array.             */
/* ------------------------------------------------------------------ */

static void test_roundtrip_all_types(void) {
    uint8_t buf[2048];
    xw_builder b;
    xw_reader r;
    xw_value v;
    xw_desc descs[8];
    int64_t i64;
    uint64_t u64;
    int boolean;
    double dbl;
    const char *str;
    uint32_t slen, desc_idx, dlen;
    const uint8_t *ptr;
    static const uint8_t data100[100] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
        0x60, 0x61, 0x62, 0x63
    };

    /* Build a dictionary holding one of every type. Port values are
     * inserted in a known order so descriptor indices are predictable:
     * fd 0, mach_send 1, mach_recv 2, send_once 3, shmem 4, data_ool 5.
     */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_null(&b, "null"), XW_OK);
    CHECK_ERR(xw_dict_add_bool(&b, "true", 1), XW_OK);
    CHECK_ERR(xw_dict_add_bool(&b, "false", 0), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "neg", -1234567890123456789LL), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "min", (-9223372036854775807LL - 1)), XW_OK);
    CHECK_ERR(xw_dict_add_uint64(&b, "max", 0xffffffffffffffffULL), XW_OK);
    CHECK_ERR(xw_dict_add_uint64(&b, "zero", 0), XW_OK);
    CHECK_ERR(xw_dict_add_double(&b, "pi", 3.141592653589793), XW_OK);
    CHECK_ERR(xw_dict_add_double(&b, "negzero", -0.0), XW_OK);
    CHECK_ERR(xw_dict_add_double(&b, "big", 1e300), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "hello", "hello"), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "empty", ""), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "pad7", "1234567"), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "pad6", "123456"), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data0", "", 0), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data1", "A", 1), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data3", "ABC", 3), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data4", "ABCD", 4), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data5", "ABCDE", 5), XW_OK);
    CHECK_ERR(xw_dict_add_data(&b, "data100", data100, 100), XW_OK);
    CHECK_ERR(xw_dict_add_uuid(&b, "uuid", kTestUuid), XW_OK);
    CHECK_ERR(xw_dict_add_date(&b, "epoch", 0), XW_OK);
    CHECK_ERR(xw_dict_add_date(&b, "now", 1700000000000000000LL), XW_OK);
    CHECK_ERR(xw_dict_add_date(&b, "before", -1), XW_OK);
    CHECK_ERR(xw_dict_add_fd(&b, "fd", PORT_FD, 0x13), XW_OK);
    CHECK_ERR(xw_dict_add_mach_send(&b, "send", PORT_SEND, 0x13), XW_OK);
    CHECK_ERR(xw_dict_add_mach_recv(&b, "recv", PORT_RECV, 0x10), XW_OK);
    CHECK_ERR(xw_dict_add_mach_send_once(&b, "once", PORT_ONCE, 0x12), XW_OK);
    CHECK_ERR(xw_dict_add_shmem(&b, "shmem", PORT_SHMEM, 0x13, 4096), XW_OK);
    CHECK_ERR(xw_dict_add_data_ool(&b, "ool", 8192, PORT_OOL, 0x13), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_error(&b) == XW_OK);
    CHECK(b.ndescs == 6);
    CHECK(b.descs[0].obj == PORT_FD);
    CHECK(b.descs[1].obj == PORT_SEND);
    CHECK(b.descs[2].obj == PORT_RECV);
    CHECK(b.descs[3].obj == PORT_ONCE);
    CHECK(b.descs[4].obj == PORT_SHMEM);
    CHECK(b.descs[4].size == 4096);
    CHECK(b.descs[5].obj == PORT_OOL);
    CHECK(b.descs[5].size == 8192);

    make_dummy_descs(descs, 6);
    descs[0].obj = PORT_FD;
    descs[1].obj = PORT_SEND;
    descs[2].obj = PORT_RECV;
    descs[3].obj = PORT_ONCE;
    descs[4].obj = PORT_SHMEM;
    descs[5].obj = PORT_OOL;
    CHECK_ERR(xw_reader_init(&r, buf, xw_builder_len(&b), descs, 6), XW_OK);

    /* Look keys up in REVERSE insertion order: descriptor indices
     * must still come out right, which proves the scan threads the
     * descriptor cursor in wire order, not lookup order.
     */
    CHECK_ERR(xw_dict_find(&r, "ool", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(ptr == NULL);
    CHECK(dlen == 8192);
    CHECK(desc_idx == 5);
    CHECK_ERR(xw_dict_find(&r, "shmem", &v), XW_OK);
    CHECK_ERR(xw_as_shmem(&v, &desc_idx, &dlen), XW_OK);
    CHECK(desc_idx == 4);
    CHECK(dlen == 4096);
    CHECK_ERR(xw_dict_find(&r, "once", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send_once(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 3);
    CHECK_ERR(xw_dict_find(&r, "recv", &v), XW_OK);
    CHECK_ERR(xw_as_mach_recv(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 2);
    CHECK_ERR(xw_dict_find(&r, "send", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 1);
    CHECK_ERR(xw_dict_find(&r, "fd", &v), XW_OK);
    CHECK_ERR(xw_as_fd(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);

    CHECK_ERR(xw_dict_find(&r, "null", &v), XW_OK);
    CHECK(v.type == XW_TYPE_NULL);
    CHECK_ERR(xw_dict_find(&r, "true", &v), XW_OK);
    CHECK_ERR(xw_as_bool(&v, &boolean), XW_OK);
    CHECK(boolean == 1);
    CHECK_ERR(xw_dict_find(&r, "false", &v), XW_OK);
    CHECK_ERR(xw_as_bool(&v, &boolean), XW_OK);
    CHECK(boolean == 0);
    CHECK_ERR(xw_dict_find(&r, "neg", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == -1234567890123456789LL);
    CHECK_ERR(xw_dict_find(&r, "min", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == (-9223372036854775807LL - 1));
    CHECK_ERR(xw_dict_find(&r, "max", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 0xffffffffffffffffULL);
    CHECK_ERR(xw_dict_find(&r, "zero", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 0);
    CHECK_ERR(xw_dict_find(&r, "pi", &v), XW_OK);
    CHECK_ERR(xw_as_double(&v, &dbl), XW_OK);
    CHECK(dbl == 3.141592653589793);
    CHECK_ERR(xw_dict_find(&r, "negzero", &v), XW_OK);
    CHECK_ERR(xw_as_double(&v, &dbl), XW_OK);
    CHECK(dbl == 0.0);
    CHECK_ERR(xw_dict_find(&r, "big", &v), XW_OK);
    CHECK_ERR(xw_as_double(&v, &dbl), XW_OK);
    CHECK(dbl == 1e300);
    CHECK_ERR(xw_dict_find(&r, "hello", &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(slen == 6);
    CHECK(strcmp(str, "hello") == 0);
    CHECK_ERR(xw_dict_find(&r, "empty", &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(slen == 1);
    CHECK(str[0] == '\0');
    CHECK_ERR(xw_dict_find(&r, "pad7", &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(slen == 8);
    CHECK(strcmp(str, "1234567") == 0);
    CHECK_ERR(xw_dict_find(&r, "pad6", &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(slen == 7);
    CHECK(strcmp(str, "123456") == 0);
    CHECK_ERR(xw_dict_find(&r, "data0", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 0);
    CHECK(desc_idx == XW_NO_DESC);
    CHECK_ERR(xw_dict_find(&r, "data1", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 1 && ptr[0] == 'A');
    CHECK_ERR(xw_dict_find(&r, "data3", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 3 && memcmp(ptr, "ABC", 3) == 0);
    CHECK_ERR(xw_dict_find(&r, "data4", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 4 && memcmp(ptr, "ABCD", 4) == 0);
    CHECK_ERR(xw_dict_find(&r, "data5", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 5 && memcmp(ptr, "ABCDE", 5) == 0);
    CHECK_ERR(xw_dict_find(&r, "data100", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 100 && memcmp(ptr, data100, 100) == 0);
    CHECK_ERR(xw_dict_find(&r, "uuid", &v), XW_OK);
    CHECK_ERR(xw_as_uuid(&v, &ptr), XW_OK);
    CHECK(memcmp(ptr, kTestUuid, 16) == 0);
    CHECK_ERR(xw_dict_find(&r, "epoch", &v), XW_OK);
    CHECK_ERR(xw_as_date(&v, &i64), XW_OK);
    CHECK(i64 == 0);
    CHECK_ERR(xw_dict_find(&r, "now", &v), XW_OK);
    CHECK_ERR(xw_as_date(&v, &i64), XW_OK);
    CHECK(i64 == 1700000000000000000LL);
    CHECK_ERR(xw_dict_find(&r, "before", &v), XW_OK);
    CHECK_ERR(xw_as_date(&v, &i64), XW_OK);
    CHECK(i64 == -1);

    /* Type mismatches must fail, not coerce. */
    CHECK_ERR(xw_dict_find(&r, "hello", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_bool(&v, &boolean), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_double(&v, &dbl), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_uuid(&v, &ptr), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_date(&v, &i64), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_fd(&v, &desc_idx), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_ERR_BAD_TYPE);

    /* Iterator over the whole dictionary, in insertion order. */
    {
        xw_dict_iter it;
        const char *k = NULL;
        int seen = 0;
        CHECK_ERR(xw_dict_iter_init(&r, &it), XW_OK);
        while (1) {
            int rc = xw_dict_next(&it, &k, &v);
            if (rc == 0) break;
            CHECK(rc == 1);
            seen++;
            if (seen == 1) CHECK(strcmp(k, "null") == 0);
            if (seen == 30) CHECK(strcmp(k, "ool") == 0);
        }
        CHECK(seen == 30);
    }

    /* Same types again, unkeyed, in a root array. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_array_begin(&b), XW_OK);
    CHECK_ERR(xw_add_null(&b), XW_OK);
    CHECK_ERR(xw_add_bool(&b, 1), XW_OK);
    CHECK_ERR(xw_add_int64(&b, -7), XW_OK);
    CHECK_ERR(xw_add_uint64(&b, 7), XW_OK);
    CHECK_ERR(xw_add_double(&b, 2.5), XW_OK);
    CHECK_ERR(xw_add_string(&b, "s"), XW_OK);
    CHECK_ERR(xw_add_data(&b, "xy", 2), XW_OK);
    CHECK_ERR(xw_add_uuid(&b, kTestUuid), XW_OK);
    CHECK_ERR(xw_add_date(&b, 42), XW_OK);
    CHECK_ERR(xw_add_fd(&b, PORT_FD, 0x13), XW_OK);
    CHECK_ERR(xw_add_mach_send(&b, PORT_SEND, 0x13), XW_OK);
    CHECK_ERR(xw_add_mach_recv(&b, PORT_RECV, 0x10), XW_OK);
    CHECK_ERR(xw_add_mach_send_once(&b, PORT_ONCE, 0x12), XW_OK);
    CHECK_ERR(xw_add_shmem(&b, PORT_SHMEM, 0x13, 512), XW_OK);
    CHECK_ERR(xw_add_data_ool(&b, 1024, PORT_OOL, 0x13), XW_OK);
    CHECK_ERR(xw_array_end(&b), XW_OK);
    CHECK(b.ndescs == 6);

    make_dummy_descs(descs, 6);
    CHECK_ERR(xw_reader_init(&r, buf, xw_builder_len(&b), descs, 6), XW_OK);
    CHECK_ERR(xw_array_len(&r, &slen), XW_OK);
    CHECK(slen == 15);

    CHECK_ERR(xw_array_at(&r, 0, &v), XW_OK);
    CHECK(v.type == XW_TYPE_NULL);
    CHECK_ERR(xw_array_at(&r, 1, &v), XW_OK);
    CHECK_ERR(xw_as_bool(&v, &boolean), XW_OK);
    CHECK(boolean == 1);
    CHECK_ERR(xw_array_at(&r, 2, &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == -7);
    CHECK_ERR(xw_array_at(&r, 3, &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 7);
    CHECK_ERR(xw_array_at(&r, 4, &v), XW_OK);
    CHECK_ERR(xw_as_double(&v, &dbl), XW_OK);
    CHECK(dbl == 2.5);
    CHECK_ERR(xw_array_at(&r, 5, &v), XW_OK);
    CHECK_ERR(xw_as_string(&v, &str, &slen), XW_OK);
    CHECK(strcmp(str, "s") == 0);
    CHECK_ERR(xw_array_at(&r, 6, &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(dlen == 2 && memcmp(ptr, "xy", 2) == 0);
    CHECK_ERR(xw_array_at(&r, 7, &v), XW_OK);
    CHECK_ERR(xw_as_uuid(&v, &ptr), XW_OK);
    CHECK(memcmp(ptr, kTestUuid, 16) == 0);
    CHECK_ERR(xw_array_at(&r, 8, &v), XW_OK);
    CHECK_ERR(xw_as_date(&v, &i64), XW_OK);
    CHECK(i64 == 42);
    CHECK_ERR(xw_array_at(&r, 9, &v), XW_OK);
    CHECK_ERR(xw_as_fd(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);
    CHECK_ERR(xw_array_at(&r, 10, &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 1);
    CHECK_ERR(xw_array_at(&r, 11, &v), XW_OK);
    CHECK_ERR(xw_as_mach_recv(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 2);
    CHECK_ERR(xw_array_at(&r, 12, &v), XW_OK);
    CHECK_ERR(xw_as_mach_send_once(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 3);
    CHECK_ERR(xw_array_at(&r, 13, &v), XW_OK);
    CHECK_ERR(xw_as_shmem(&v, &desc_idx, &dlen), XW_OK);
    CHECK(desc_idx == 4);
    CHECK(dlen == 512);
    CHECK_ERR(xw_array_at(&r, 14, &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(ptr == NULL && dlen == 1024 && desc_idx == 5);

    /* Out of range and wrong-type access on the array. */
    CHECK_ERR(xw_array_at(&r, 15, &v), XW_ERR_NOT_FOUND);
    CHECK_ERR(xw_array_at(&r, 1000, &v), XW_ERR_NOT_FOUND);
    CHECK_ERR(xw_array_at(&r, 0, &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_dict_find(&r, "x", &v), XW_ERR_BAD_TYPE); /* root is an array */
}

/* ------------------------------------------------------------------ */
/* Nested containers, 3+ levels, with ports at several depths.        */
/* ------------------------------------------------------------------ */

static void test_nested_containers(void) {
    uint8_t buf[1024];
    xw_builder b;
    xw_reader r;
    xw_value v, outer, mid, elem, leaf;
    xw_desc descs[4];
    uint32_t desc_idx, slen, dlen;
    const uint8_t *ptr;
    int64_t i64;
    const char *str;

    /* root { "outer": { "mid": [ { "leaf": "value" }, 42 ] },
     *        "a": mach_send, "nested": { "p": mach_recv },
     *        "b": mach_send }
     * Descriptor order in wire order: a 0, p 1, b 2.
     */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_dict_begin(&b, "outer"), XW_OK);
    CHECK_ERR(xw_dict_add_array_begin(&b, "mid"), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_string(&b, "leaf", "value"), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK_ERR(xw_add_int64(&b, 42), XW_OK);
    CHECK_ERR(xw_array_end(&b), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK_ERR(xw_dict_add_mach_send(&b, "a", PORT_SEND, 0x13), XW_OK);
    CHECK_ERR(xw_dict_add_dict_begin(&b, "nested"), XW_OK);
    CHECK_ERR(xw_dict_add_mach_recv(&b, "p", PORT_RECV, 0x10), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK_ERR(xw_dict_add_mach_send(&b, "b", PORT_SEND, 0x13), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_error(&b) == XW_OK);
    CHECK(b.ndescs == 3);

    make_dummy_descs(descs, 3);
    CHECK_ERR(xw_reader_init(&r, buf, xw_builder_len(&b), descs, 3), XW_OK);

    /* Walk down three levels: root dict -> "outer" dict -> "mid"
     * array -> element 0 dict -> "leaf" string. */
    CHECK_ERR(xw_dict_find(&r, "outer", &outer), XW_OK);
    CHECK(outer.type == XW_TYPE_DICTIONARY);
    CHECK_ERR(xw_dict_find_v(&outer, "mid", &mid), XW_OK);
    CHECK(mid.type == XW_TYPE_ARRAY);
    CHECK_ERR(xw_array_len_v(&mid, &slen), XW_OK);
    CHECK(slen == 2);
    CHECK_ERR(xw_array_at_v(&mid, 0, &elem), XW_OK);
    CHECK(elem.type == XW_TYPE_DICTIONARY);
    CHECK_ERR(xw_dict_find_v(&elem, "leaf", &leaf), XW_OK);
    CHECK_ERR(xw_as_string(&leaf, &str, &slen), XW_OK);
    CHECK(strcmp(str, "value") == 0);
    CHECK_ERR(xw_array_at_v(&mid, 1, &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
    CHECK(i64 == 42);
    CHECK_ERR(xw_array_at_v(&mid, 2, &v), XW_ERR_NOT_FOUND);
    CHECK_ERR(xw_dict_find_v(&elem, "missing", &v), XW_ERR_NOT_FOUND);
    CHECK_ERR(xw_dict_find_v(&mid, "x", &v), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_array_at_v(&outer, 0, &v), XW_ERR_BAD_TYPE);

    /* Ports at different depths, looked up in an order that is not
     * wire order: "b" first, then the nested "p", then "a". */
    CHECK_ERR(xw_dict_find(&r, "b", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 2);
    CHECK_ERR(xw_dict_find(&r, "nested", &v), XW_OK);
    CHECK_ERR(xw_dict_find_v(&v, "p", &v), XW_OK);
    CHECK_ERR(xw_as_mach_recv(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 1);
    CHECK_ERR(xw_dict_find(&r, "a", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);

    /* Iterator over the nested dict, and nested iterator state. */
    {
        xw_dict_iter it;
        const char *k = NULL;
        int seen = 0;
        CHECK_ERR(xw_dict_find(&r, "outer", &outer), XW_OK);
        CHECK_ERR(xw_dict_iter_init_v(&outer, &it), XW_OK);
        while (1) {
            int rc = xw_dict_next(&it, &k, &v);
            if (rc == 0) break;
            CHECK(rc == 1);
            seen++;
            if (seen == 1) {
                CHECK(strcmp(k, "mid") == 0);
                CHECK(v.type == XW_TYPE_ARRAY);
            }
        }
        CHECK(seen == 1);
    }

    /* OOL data inside a nested dict: the reader must still report
     * ptr NULL with the right descriptor index and length. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_dict_begin(&b, "inner"), XW_OK);
    CHECK_ERR(xw_dict_add_data_ool(&b, "blob", 4096, PORT_OOL, 0x13), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(b.ndescs == 1);
    make_dummy_descs(descs, 1);
    CHECK_ERR(xw_reader_init(&r, buf, xw_builder_len(&b), descs, 1), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "inner", &v), XW_OK);
    CHECK_ERR(xw_dict_find_v(&v, "blob", &v), XW_OK);
    CHECK_ERR(xw_as_data(&v, &ptr, &dlen, &desc_idx), XW_OK);
    CHECK(ptr == NULL && dlen == 4096 && desc_idx == 0);

    /* Depth limit: 17 nested dicts must fail at the 17th begin. */
    {
        int i;
        CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
        CHECK_ERR(xw_dict_begin(&b), XW_OK);
        for (i = 0; i < 15; i++) {
            CHECK_ERR(xw_dict_add_dict_begin(&b, "d"), XW_OK);
        }
        /* 1 root + 15 nested = 16 frames, the limit. One more fails. */
        CHECK_ERR(xw_dict_add_dict_begin(&b, "d"), XW_ERR_OVERFLOW);
        CHECK(xw_builder_error(&b) == XW_ERR_OVERFLOW);
    }
}


/* ------------------------------------------------------------------ */
/* Mach envelope: parse, OOL body, build_reply.                        */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u64le(uint8_t *p, uint64_t v) {
    uint32_t i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

/* Build a full Mach message in buf holding the lookup reply body and
 * one port descriptor (name 0x1003, disposition COPY_SEND 0x13).
 * Returns the message size. */
static uint32_t make_lookup_message(uint8_t *buf, uint32_t cap) {
    uint32_t body_len = (uint32_t)sizeof(kGoldenLookupReply);
    uint32_t size = 24 + 4 + 12 + body_len;
    uint8_t *p = buf;
    if (cap < size) return 0;
    put_u32le(p + 0, 0x80000012u);       /* bits: COMPLEX | MOVE_SEND_ONCE */
    put_u32le(p + 4, size);
    put_u32le(p + 8, 0x1003);           /* remote: requester's local port */
    put_u32le(p + 12, 0);               /* local */
    put_u32le(p + 16, 0);               /* voucher */
    put_u32le(p + 20, 0x20000000u);     /* msgh_id: reply */
    put_u32le(p + 24, 1);               /* descriptor count */
    put_u32le(p + 28, 0x1003);          /* port name */
    put_u32le(p + 32, 0);               /* pad1 */
    put_u32le(p + 36, 0x00130000u);     /* pad2 0, disposition 0x13, type 0 */
    memcpy(p + 40, kGoldenLookupReply, body_len);
    return size;
}

/* Build a look_up request message: bits COPY_SEND | MAKE_SEND_ONCE
 * << 8, remote is the service port, local is the reply port, id
 * 0xcf, empty-dict body, no descriptors. Returns the message size. */
static uint32_t make_request_message(uint8_t *buf, uint32_t cap) {
    uint32_t size = 24 + sizeof(kGoldenEmptyReply);
    uint8_t *p = buf;
    if (cap < size) return 0;
    put_u32le(p + 0, 0x1513u);          /* COPY_SEND | MAKE_SEND_ONCE<<8 */
    put_u32le(p + 4, size);
    put_u32le(p + 8, 0x200);            /* remote: service port */
    put_u32le(p + 12, 0x1003);          /* local: reply port */
    put_u32le(p + 16, 0);               /* voucher */
    put_u32le(p + 20, 0xcf);            /* msgh_id: look_up */
    memcpy(p + 24, kGoldenEmptyReply, sizeof(kGoldenEmptyReply));
    return size;
}

static void test_envelope(void) {
    uint8_t buf[512];
    uint8_t reply[512];
    xw_envelope env;
    xw_reader r;
    xw_value v;
    uint32_t desc_idx, size, rlen;
    int64_t i64;
    uint64_t u64;

    size = make_lookup_message(buf, sizeof(buf));
    CHECK(size == 136);
    CHECK_ERR(xw_envelope_parse(buf, size, &env), XW_OK);
    CHECK(env.bits == 0x80000012u);
    CHECK(env.size == size);
    CHECK(env.remote == 0x1003);
    CHECK(env.local == 0);
    CHECK(env.id == 0x20000000u);
    CHECK(env.ndescs == 1);
    CHECK(env.descs[0].dtype == XW_DESC_PORT);
    CHECK(env.descs[0].disposition == 0x13);
    CHECK(env.descs[0].obj == 0x1003);
    CHECK(env.body_is_ool == 0);
    CHECK(env.payload_len == sizeof(kGoldenLookupReply));
    CHECK(buf_equal(env.payload, kGoldenLookupReply,
                    sizeof(kGoldenLookupReply)));

    /* The payload parses as the lookup reply with the parsed descs. */
    CHECK_ERR(xw_reader_init(&r, env.payload, env.payload_len,
                             env.descs, env.ndescs), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "req_pid", &v), XW_OK);
    CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
    CHECK(u64 == 1234);
    CHECK_ERR(xw_dict_find(&r, "port", &v), XW_OK);
    CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
    CHECK(desc_idx == 0);

    /* build_reply from a real request envelope: id 0x20000000,
     * remote is the request's local (reply) port, MOVE_SEND_ONCE,
     * no complex, no descs. */
    {
        uint8_t body[64];
        xw_builder b;
        xw_envelope req, rep;
        CHECK_ERR(xw_envelope_parse(buf, make_request_message(buf, sizeof(buf)),
                                    &req), XW_OK);
        CHECK(req.bits == 0x1513u);
        CHECK(req.remote == 0x200);
        CHECK(req.local == 0x1003);
        CHECK(req.id == 0xcf);
        CHECK(req.ndescs == 0);
        CHECK(req.payload_len == sizeof(kGoldenEmptyReply));
        CHECK_ERR(xw_builder_init(&b, body, sizeof(body)), XW_OK);
        CHECK_ERR(xw_dict_begin(&b), XW_OK);
        CHECK_ERR(xw_dict_add_int64(&b, "error", 0), XW_OK);
        CHECK_ERR(xw_dict_end(&b), XW_OK);
        CHECK_ERR(xw_envelope_build_reply(&req, reply, sizeof(reply),
                                          body, xw_builder_len(&b),
                                          NULL, 0, &rlen), XW_OK);
        CHECK(rlen == 24 + 40);
        CHECK_ERR(xw_envelope_parse(reply, rlen, &rep), XW_OK);
        CHECK(rep.bits == 0x12); /* MOVE_SEND_ONCE, no COMPLEX */
        CHECK(rep.remote == req.local);
        CHECK(rep.local == 0);
        CHECK(rep.id == 0x20000000u);
        CHECK(rep.ndescs == 0);
        CHECK(rep.payload_len == 40);
        CHECK_ERR(xw_reader_init(&r, rep.payload, rep.payload_len,
                                 NULL, 0), XW_OK);
        CHECK_ERR(xw_dict_find(&r, "error", &v), XW_OK);
        CHECK_ERR(xw_as_int64(&v, &i64), XW_OK);
        CHECK(i64 == 0);
    }

    /* build_reply with a port descriptor must set COMPLEX and the
     * descriptor count. */
    {
        uint8_t body[128];
        xw_builder b;
        xw_envelope req, rep;
        CHECK_ERR(xw_envelope_parse(buf, make_request_message(buf, sizeof(buf)),
                                    &req), XW_OK);
        CHECK_ERR(xw_builder_init(&b, body, sizeof(body)), XW_OK);
        CHECK_ERR(xw_dict_begin(&b), XW_OK);
        CHECK_ERR(xw_dict_add_int64(&b, "error", 0), XW_OK);
        CHECK_ERR(xw_dict_add_uint64(&b, "req_pid", 1234), XW_OK);
        CHECK_ERR(xw_dict_add_uint64(&b, "rec_execcnt", 1), XW_OK);
        CHECK_ERR(xw_dict_add_mach_send(&b, "port", PORT_LOOKUP, 0x13), XW_OK);
        CHECK_ERR(xw_dict_end(&b), XW_OK);
        CHECK(buf_equal(body, kGoldenLookupReply, sizeof(kGoldenLookupReply)));
        CHECK_ERR(xw_envelope_build_reply(&req, reply, sizeof(reply),
                                          body, xw_builder_len(&b),
                                          b.descs, b.ndescs, &rlen), XW_OK);
        CHECK(rlen == 24 + 4 + 12 + 96);
        CHECK_ERR(xw_envelope_parse(reply, rlen, &rep), XW_OK);
        CHECK(rep.bits == 0x80000012u);
        CHECK(rep.ndescs == 1);
        CHECK(rep.descs[0].disposition == 0x13);
        CHECK(rep.descs[0].obj == PORT_LOOKUP);
        CHECK(rep.payload_len == 96);
        CHECK(buf_equal(rep.payload, kGoldenLookupReply,
                        sizeof(kGoldenLookupReply)));
    }

    /* OOL body: first descriptor is an OOL descriptor pointing at a
     * real test buffer (the 96-byte lookup reply); a port descriptor
     * follows it. There are no inline body bytes: when the body is
     * OOL, the whole body is the OOL region. The OOL descriptor is
     * consumed by the envelope (payload + body_is_ool) and is NOT
     * listed in env->descs; descs holds only port descriptors, in
     * wire order, which is exactly what the reader consumes. */
    {
        uint8_t msg[256];
        uint8_t *p = msg;
        uint32_t msize = 24 + 4 + 16 + 12;
        put_u32le(p + 0, 0x80000012u);
        put_u32le(p + 4, msize);
        put_u32le(p + 8, 0x1003);
        put_u32le(p + 12, 0);
        put_u32le(p + 16, 0);
        put_u32le(p + 20, 0x20000000u);
        put_u32le(p + 24, 2);
        put_u64le(p + 28, (uint64_t)(uintptr_t)kGoldenLookupReply);
        p[36] = 0; p[37] = 0; p[38] = 0; p[39] = 1; /* type OOL */
        put_u32le(p + 40, 96);                      /* size */
        put_u32le(p + 44, 0x1003);                  /* port name */
        put_u32le(p + 48, 0);
        put_u32le(p + 52, 0x00130000u);             /* disp 0x13, type 0 */
        CHECK_ERR(xw_envelope_parse(msg, msize, &env), XW_OK);
        CHECK(env.body_is_ool == 1);
        CHECK(env.ndescs == 1);
        CHECK(env.descs[0].dtype == XW_DESC_PORT);
        CHECK(env.descs[0].disposition == 0x13);
        CHECK(env.descs[0].obj == 0x1003);
        CHECK(env.payload_len == 96);
        CHECK(env.payload == kGoldenLookupReply);
        CHECK_ERR(xw_reader_init(&r, env.payload, env.payload_len,
                                 env.descs, env.ndescs), XW_OK);
        CHECK_ERR(xw_dict_find(&r, "req_pid", &v), XW_OK);
        CHECK_ERR(xw_as_uint64(&v, &u64), XW_OK);
        CHECK(u64 == 1234);
        CHECK_ERR(xw_dict_find(&r, "port", &v), XW_OK);
        CHECK_ERR(xw_as_mach_send(&v, &desc_idx), XW_OK);
        CHECK(desc_idx == 0);
    }

    /* Envelope error paths. */
    CHECK_ERR(xw_envelope_parse(NULL, 100, &env), XW_ERR_BAD_ENVELOPE);
    CHECK_ERR(xw_envelope_parse(buf, 23, &env), XW_ERR_BAD_ENVELOPE);
    CHECK_ERR(xw_envelope_parse(buf, 0, &env), XW_ERR_BAD_ENVELOPE);
    {
        uint8_t short_msg[256];
        uint32_t m = make_lookup_message(short_msg, sizeof(short_msg));
        /* Claimed size larger than the buffer handed in. */
        CHECK_ERR(xw_envelope_parse(short_msg, m - 10, &env),
                  XW_ERR_BAD_ENVELOPE);
        /* Truncated descriptor table. */
        put_u32le(short_msg + 4, m);
        CHECK_ERR(xw_envelope_parse(short_msg, 24 + 4 + 6, &env),
                  XW_ERR_BAD_ENVELOPE);
        /* OOL descriptor not first: port first, OOL second. */
        {
            uint8_t msg2[256];
            uint8_t *p = msg2;
            uint32_t msize = 24 + 4 + 12 + 16;
            put_u32le(p + 0, 0x80000012u);
            put_u32le(p + 4, msize);
            put_u32le(p + 8, 0x1003);
            put_u32le(p + 12, 0);
            put_u32le(p + 16, 0);
            put_u32le(p + 20, 0x20000000u);
            put_u32le(p + 24, 2);
            put_u32le(p + 28, 0x1003);
            put_u32le(p + 32, 0);
            put_u32le(p + 36, 0x00130000u);
            put_u64le(p + 40, 0x1000);
            p[48] = 0; p[49] = 0; p[50] = 0; p[51] = 1;
            put_u32le(p + 52, 40);
            CHECK_ERR(xw_envelope_parse(msg2, msize, &env),
                      XW_ERR_BAD_ENVELOPE);
        }
        /* OOL_PORTS descriptor is rejected. */
        {
            uint8_t msg3[256];
            uint8_t *p = msg3;
            uint32_t msize = 24 + 4 + 16;
            put_u32le(p + 0, 0x80000012u);
            put_u32le(p + 4, msize);
            put_u32le(p + 8, 0x1003);
            put_u32le(p + 12, 0);
            put_u32le(p + 16, 0);
            put_u32le(p + 20, 0x20000000u);
            put_u32le(p + 24, 1);
            put_u64le(p + 28, 0x1000);
            p[36] = 0; p[37] = 0; p[38] = 0; p[39] = 2; /* OOL_PORTS */
            put_u32le(p + 40, 4);
            CHECK_ERR(xw_envelope_parse(msg3, msize, &env),
                      XW_ERR_BAD_ENVELOPE);
        }
        /* build_reply with too small an output buffer. */
        CHECK_ERR(xw_envelope_build_reply(&env, reply, 10,
                                          kGoldenErrorReply,
                                          sizeof(kGoldenErrorReply),
                                          NULL, 0, &rlen),
                  XW_ERR_NOSPACE);
    }
}

/* ------------------------------------------------------------------ */
/* Builder and reader error paths.                                     */
/* ------------------------------------------------------------------ */

static void test_error_paths(void) {
    uint8_t buf[64];
    xw_builder b;
    xw_reader r;
    xw_value v;
    int64_t i64;

    /* Builder: buffer too small for even the 8-byte header. init
     * itself reports NOSPACE, and the error is sticky. */
    CHECK_ERR(xw_builder_init(&b, buf, 4), XW_ERR_NOSPACE);
    CHECK_ERR(xw_dict_begin(&b), XW_ERR_NOSPACE);
    CHECK(xw_builder_error(&b) == XW_ERR_NOSPACE);
    CHECK(xw_builder_len(&b) == 0);

    /* Builder: error is sticky; later calls keep failing. */
    CHECK_ERR(xw_dict_add_int64(&b, "x", 1), XW_ERR_NOSPACE);

    /* Builder: wrong container. Errors are sticky, so each case gets
     * a fresh builder. A bare begin inside a dict needs a key (use
     * dict_add_dict_begin / dict_add_array_begin); a bare begin
     * inside an array is an element. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "x", 1), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_add_int64(&b, 1), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_array_end(&b), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_add_int64(&b, 1), XW_ERR_BAD_TYPE); /* unkeyed in dict */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_ERR_BAD_TYPE); /* bare begin in dict */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_array_begin(&b), XW_ERR_BAD_TYPE); /* bare begin in dict */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_array_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "x", 1), XW_ERR_BAD_TYPE); /* keyed in array */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_array_begin(&b, "a"), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK); /* dict element of the array */
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK_ERR(xw_array_end(&b), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_OK);
    CHECK(xw_builder_error(&b) == XW_OK);
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_array_end(&b), XW_ERR_BAD_TYPE); /* end the wrong kind */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_array_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_end(&b), XW_ERR_BAD_TYPE); /* end the wrong kind */

    /* Builder: a key that does not fit reports NOSPACE. */
    CHECK_ERR(xw_builder_init(&b, buf, sizeof(buf)), XW_OK);
    CHECK_ERR(xw_dict_begin(&b), XW_OK);
    CHECK_ERR(xw_dict_add_int64(&b, "this-key-is-far-too-long-for-64-bytes",
                                1), XW_ERR_NOSPACE);

    /* Builder: NULL arguments. */
    CHECK_ERR(xw_builder_init(NULL, buf, sizeof(buf)), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, NULL, sizeof(buf)), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_builder_init(&b, buf, 0), XW_ERR_NOSPACE);

    /* Reader: NULL / too short / bad magic / bad version. */
    CHECK_ERR(xw_reader_init(&r, NULL, 40, NULL, 0), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_reader_init(&r, kGoldenErrorReply, 0, NULL, 0),
              XW_ERR_TRUNCATED);
    CHECK_ERR(xw_reader_init(&r, kGoldenErrorReply, 11, NULL, 0),
              XW_ERR_TRUNCATED);
    {
        uint8_t bad[40];
        memcpy(bad, kGoldenErrorReply, 40);
        bad[0] = 0x00;
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_BAD_MAGIC);
        memcpy(bad, kGoldenErrorReply, 40);
        put_u32le(bad + 8, 0x99999999u); /* root type word */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_BAD_TYPE);
    }

    /* Reader: truncated body (cut at every length) must never accept
     * a partial message, and must never crash. */
    {
        uint32_t cut;
        for (cut = 0; cut < sizeof(kGoldenLookupReply); cut++) {
            xw_desc d[1];
            make_dummy_descs(d, 1);
            int rc = xw_reader_init(&r, kGoldenLookupReply, cut, d, 1);
            CHECK(rc != XW_OK);
        }
    }

    /* Reader: unknown type codes are rejected (0x6000 is launchd.c's
     * wrong date code; 0x11000, 0x14000, 0x16000-0x19000, 0x1a000). */
    {
        static const uint32_t bad_types[] = {
            0x6000, 0x11000, 0x14000, 0x16000, 0x17000, 0x18000, 0x19000,
            0x1a000, 0x2000 + 1, 0x12345
        };
        uint32_t i;
        for (i = 0; i < sizeof(bad_types) / sizeof(bad_types[0]); i++) {
            uint8_t bad[40];
            memcpy(bad, kGoldenSimpleDict, sizeof(kGoldenSimpleDict));
            /* Root dict, one entry, key "key": overwrite the value
             * type word at offset 24 with the bad code. */
            put_u32le(bad + 24, bad_types[i]);
            CHECK_ERR(xw_reader_init(&r, bad, sizeof(bad), NULL, 0),
                      XW_ERR_BAD_TYPE);
        }
    }

    /* Reader: a port value with no descriptor available is an
     * envelope problem, not a truncation. */
    CHECK_ERR(xw_reader_init(&r, kGoldenLookupReply,
                             sizeof(kGoldenLookupReply), NULL, 0),
              XW_ERR_BAD_ENVELOPE);

    /* Reader: extra descriptors left over after the message is an
     * error (exact descriptor accounting). */
    {
        xw_desc d[2];
        make_dummy_descs(d, 2);
        CHECK_ERR(xw_reader_init(&r, kGoldenLookupReply,
                                 sizeof(kGoldenLookupReply), d, 2),
                  XW_ERR_BAD_ENVELOPE);
    }

    /* Reader: trailing bytes after the root value are rejected. */
    {
        uint8_t trailing[48];
        memcpy(trailing, kGoldenErrorReply, 40);
        memset(trailing + 40, 0, 8);
        CHECK_ERR(xw_reader_init(&r, trailing, 48, NULL, 0),
                  XW_ERR_BAD_ENVELOPE);
    }

    /* Reader: a string whose last byte is not NUL is rejected, and
     * an embedded NUL is rejected too (stricter than real libxpc).
     * Layout of the simple dict: "value" is at offsets 32-37
     * (v=32, a=33, l=34, u=35, e=36, NUL=37), its length word at 28.
     */
    {
        uint8_t bad[40];
        memcpy(bad, kGoldenSimpleDict, 40);
        bad[37] = 'X'; /* the NUL terminator of "value" */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_BAD_TYPE);
        memcpy(bad, kGoldenSimpleDict, 40);
        bad[33] = '\0'; /* embedded NUL inside "value" */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_BAD_TYPE);
        memcpy(bad, kGoldenSimpleDict, 40);
        bad[28] = 20; /* string length 20 but only 8 bytes left */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_TRUNCATED);
    }

    /* Reader: dict size word that disagrees with the entries. */
    {
        uint8_t bad[40];
        memcpy(bad, kGoldenSimpleDict, 40);
        put_u32le(bad + 12, 23); /* dict size 23 instead of 24 */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_TRUNCATED);
        memcpy(bad, kGoldenSimpleDict, 40);
        put_u32le(bad + 12, 25); /* dict size 25 instead of 24 */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_TRUNCATED);
        memcpy(bad, kGoldenSimpleDict, 40);
        put_u32le(bad + 16, 2); /* count 2 instead of 1 */
        CHECK_ERR(xw_reader_init(&r, bad, 40, NULL, 0), XW_ERR_TRUNCATED);
    }

    /* Getter NULL out pointers are rejected. */
    CHECK_ERR(xw_reader_init(&r, kGoldenErrorReply, 40, NULL, 0), XW_OK);
    CHECK_ERR(xw_dict_find(&r, "error", &v), XW_OK);
    CHECK_ERR(xw_as_int64(&v, NULL), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_as_int64(NULL, &i64), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_dict_find(&r, "error", NULL), XW_ERR_BAD_TYPE);
    CHECK_ERR(xw_dict_find(NULL, "error", &v), XW_ERR_BAD_TYPE);
}

/* ------------------------------------------------------------------ */
/* Malformed-input fuzz.                                               */
/*                                                                     */
/* Deterministic xorshift PRNG, fixed seed, so a failure reproduces.   */
/* Three fuzzers run per iteration:                                    */
/*   1. reader fuzz: mutated XPC bodies fed to xw_reader_init, then   */
/*      dict/array access on any message that validates.               */
/*   2. envelope fuzz: mutated Mach messages fed to                  */
/*      xw_envelope_parse; inline payloads are then fed to the        */
/*      reader. OOL payloads are NOT dereferenced: the OOL address    */
/*      in a mutated message is attacker controlled and may point     */
/*      anywhere, so only the parse result is checked there.          */
/*   3. builder fuzz: random builder call sequences on a small       */
/*      buffer; the builder must never write past cap (ASan would    */
/*      catch it) and must report an error instead of overflowing.    */
/*                                                                     */
/* Run under ASan+UBSan: any out-of-bounds read/write, any undefined */
/* behavior, or any crash fails the run.                               */
/* ------------------------------------------------------------------ */

static uint32_t g_rng = 0x12345678u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static void mutate_bytes(uint8_t *buf, uint32_t len) {
    uint32_t n, i;
    if (len == 0) return;
    n = 1 + (rng_next() % 8);
    for (i = 0; i < n; i++) {
        uint32_t kind = rng_next() % 4;
        uint32_t at = rng_next() % len;
        switch (kind) {
        case 0: /* random byte */
            buf[at] = (uint8_t)rng_next();
            break;
        case 1: /* single bit flip */
            buf[at] ^= (uint8_t)(1u << (rng_next() % 8));
            break;
        case 2: /* random 4-byte word */
            if (at + 4 <= len) put_u32le(buf + at, rng_next());
            break;
        default: /* zero a byte */
            buf[at] = 0;
            break;
        }
    }
}

static uint32_t g_fuzz_reader_ok = 0;
static uint32_t g_fuzz_envelope_ok = 0;
static uint32_t g_fuzz_builder_ok = 0;

static void fuzz_reader_once(void) {
    /* Corpus: the golden lookup reply, the simple dict, the bigger
     * dict, and the error reply. */
    static const uint8_t *corpus[4];
    static const uint32_t corpus_len[4] = {
        sizeof(kGoldenLookupReply), sizeof(kGoldenSimpleDict),
        sizeof(kGoldenBiggerDict), sizeof(kGoldenErrorReply)
    };
    uint8_t scratch[256];
    uint32_t which, len, cut;
    xw_reader r;
    xw_value v;
    xw_desc descs[4];

    corpus[0] = kGoldenLookupReply;
    corpus[1] = kGoldenSimpleDict;
    corpus[2] = kGoldenBiggerDict;
    corpus[3] = kGoldenErrorReply;

    which = rng_next() % 4;
    len = corpus_len[which];
    memcpy(scratch, corpus[which], len);
    /* One iteration in ten feeds a valid message unmutated, so the
     * accessor paths below also run under the sanitizers. */
    if ((rng_next() % 10) != 0) mutate_bytes(scratch, len);
    /* Sometimes truncate the buffer instead of (or as well as)
     * mutating it. */
    cut = len;
    if ((rng_next() % 3) == 0) cut = rng_next() % (len + 1);

    make_dummy_descs(descs, 4);
    /* Only the lookup reply carries a port value (1 descriptor);
     * the other corpus messages carry none, and the reader requires
     * exact descriptor accounting. */
    if (xw_reader_init(&r, scratch, cut, descs,
                       (which == 0) ? 1u : 0u) == XW_OK) {
        g_fuzz_reader_ok++;
        /* Exercise the accessors on a message that validated. */
        (void)xw_dict_find(&r, "error", &v);
        (void)xw_dict_find(&r, "key", &v);
        (void)xw_dict_find(&r, "port", &v);
        (void)xw_dict_find(&r, "auint64", &v);
        (void)xw_array_at(&r, 0, &v);
        (void)xw_array_at(&r, 1, &v);
        {
            xw_dict_iter it;
            const char *k = NULL;
            int guard = 0;
            if (xw_dict_iter_init(&r, &it) == XW_OK) {
                while (guard++ < 64) {
                    int rc = xw_dict_next(&it, &k, &v);
                    if (rc <= 0) break;
                }
            }
        }
    }
}

static void fuzz_envelope_once(void) {
    uint8_t scratch[256];
    uint32_t size, cut;
    xw_envelope env;

    size = make_lookup_message(scratch, sizeof(scratch));
    mutate_bytes(scratch, size);
    cut = size;
    if ((rng_next() % 4) == 0) cut = rng_next() % (size + 1);

    if (xw_envelope_parse(scratch, cut, &env) == XW_OK) {
        g_fuzz_envelope_ok++;
        if (!env.body_is_ool && env.payload_len > 0) {
            /* Inline payload: safe to hand to the reader. */
            xw_reader r;
            xw_value v;
            if (xw_reader_init(&r, env.payload, env.payload_len,
                               env.descs, env.ndescs) == XW_OK) {
                (void)xw_dict_find(&r, "error", &v);
                (void)xw_dict_find(&r, "port", &v);
            }
        }
        /* OOL payload: deliberately NOT dereferenced. The address in
         * a mutated message is attacker controlled. */
    }
}

static void fuzz_builder_once(void) {
    uint8_t buf[128];
    xw_builder b;
    uint32_t i, nops, cap;
    int rc;

    cap = 16 + (rng_next() % (sizeof(buf) - 16));
    if (xw_builder_init(&b, buf, cap) != XW_OK) return;
    nops = 1 + (rng_next() % 20);
    for (i = 0; i < nops; i++) {
        uint32_t op = rng_next() % 10;
        switch (op) {
        case 0: rc = xw_dict_begin(&b); break;
        case 1: rc = xw_array_begin(&b); break;
        case 2: rc = xw_dict_end(&b); break;
        case 3: rc = xw_array_end(&b); break;
        case 4: rc = xw_dict_add_int64(&b, "k", (int64_t)rng_next()); break;
        case 5: rc = xw_dict_add_string(&b, "s", "fuzz"); break;
        case 6: rc = xw_add_uint64(&b, rng_next()); break;
        case 7: rc = xw_add_data(&b, "abcd", 4); break;
        case 8: rc = xw_dict_add_mach_send(&b, "p", PORT_SEND, 0x13); break;
        default: rc = xw_add_bool(&b, (int)(rng_next() & 1)); break;
        }
        (void)rc;
        /* The builder must never claim more bytes than the cap. */
        if (xw_builder_len(&b) > cap) {
            g_failures++;
            printf("FAIL fuzz builder: len %u > cap %u\n",
                   xw_builder_len(&b), cap);
            return;
        }
    }
    /* A finished, error-free build must parse back. */
    if (xw_builder_error(&b) == XW_OK && b.depth == 0 &&
        xw_builder_len(&b) >= 8) {
        xw_reader r;
        if (xw_reader_init(&r, buf, xw_builder_len(&b),
                           b.descs, b.ndescs) == XW_OK)
            g_fuzz_builder_ok++;
    }
}

static void run_fuzz(uint32_t iters) {
    uint32_t i;
    printf("fuzz: %u iterations (reader + envelope + builder each)\n", iters);
    for (i = 0; i < iters; i++) {
        fuzz_reader_once();
        fuzz_envelope_once();
        fuzz_builder_once();
    }
    printf("fuzz: reader accepted %u, envelope accepted %u, "
           "builder built+parsed %u\n",
           g_fuzz_reader_ok, g_fuzz_envelope_ok, g_fuzz_builder_ok);
}

int main(int argc, char **argv) {
    uint32_t iters = 100000;
    if (argc > 1) {
        long v = atol(argv[1]);
        if (v > 0 && v <= 10000000) iters = (uint32_t)v;
    }

    printf("xpcwire test\n");
    test_golden_vectors();
    printf("golden vectors done: %d checks, %d failures\n",
           g_checks, g_failures);
    test_roundtrip_all_types();
    printf("round-trip done: %d checks, %d failures\n",
           g_checks, g_failures);
    test_nested_containers();
    printf("nested containers done: %d checks, %d failures\n",
           g_checks, g_failures);
    test_envelope();
    printf("envelope done: %d checks, %d failures\n",
           g_checks, g_failures);
    test_error_paths();
    printf("error paths done: %d checks, %d failures\n",
           g_checks, g_failures);

    run_fuzz(iters);

    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
