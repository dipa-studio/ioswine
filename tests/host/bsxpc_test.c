/* bsxpc_test.c: host unit test for BSXPC.
 *
 * Focused on the envelope API: bsxpc_message_parse and
 * bsxpc_reply_build, plus a smoke test that the kept functions
 * (BSSettings, BSAction, process handle, audit token, trace logger)
 * still behave. The old envelope API (bsxpc_message_begin /
 * bsxpc_message_begin_arguments / bsxpc_message_end, built on the
 * wrong "message" / "arguments" / "reply" keys) is removed and
 * is NOT tested here.
 *
 * Plain cc, libc only in this harness. Builds the patched bsxpc.c
 * from ../../src and the unchanged xpcwire.c from the reference
 * tree. Run (from this directory):
 *   cc -std=c99 -Wall -Wextra -O1 -I../../src \
 *      -I../../../src-1112c/libsystem_kernel/src \
 *      -o bsxpc_test bsxpc_test.c \
 *      ../../src/bsxpc.c \
 *      ../../../src-1112c/libsystem_kernel/src/xpcwire.c
 *   ./bsxpc_test [fuzz_iterations]
 * Sanitizers:
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../src \
 *      -I../../../src-1112c/libsystem_kernel/src \
 *      -o bsxpc_test_asan bsxpc_test.c \
 *      ../../src/bsxpc.c \
 *      ../../../src-1112c/libsystem_kernel/src/xpcwire.c
 *   ./bsxpc_test_asan [fuzz_iterations]
 *
 * The lsk_getenv stub below reads the real process environment, so
 * the IOSWINE_SVC_TRACE path can be tested on the host.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include "xpcwire.h"
#include "bsxpc.h"
#include "fakeboard_names.h"

/* lsk_getenv, as defined by init.c on the target. On the host this
 * stub reads the real environment. */
const char *lsk_getenv(const char *name) {
    return getenv(name);
}

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

#define CHECK_RC(rc) CHECK((rc) == XW_OK)

/* Deterministic xorshift PRNG (same shape as the xpcwire test). */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* Zero-init a reader, then init it. Returns the init result. */
static int init_reader(xw_reader *r, const uint8_t *buf, uint32_t len,
                       const xw_desc *descs, uint32_t ndescs) {
    memset(r, 0, sizeof(*r));
    return xw_reader_init(r, buf, len, descs, ndescs);
}

/* ------------------------------------------------------------------ */
/* Envelope keys, built from the ONE names table (fakeboard_names.h). */
/* This also validates that the table and bsxpc agree on the struct   */
/* field order.                                                        */
/* ------------------------------------------------------------------ */

static const bsxpc_envelope_keys g_env_keys = {
    FB_BSXPC_KEY_BSXPC, FB_BSXPC_KEY_CONTEXT, FB_BSXPC_KEY_CID,
    FB_BSXPC_KEY_CIDR, FB_BSXPC_KEY_SEL
};

/* ------------------------------------------------------------------ */
/* Builders for the device-shaped messages.                            */
/* ------------------------------------------------------------------ */

/* The device request, as measured: bsxpc
 * "connect", bsxpc_context with parameters, bsxpc_CID 1,
 * bsxpc_CIDr false, bsxpc_SEL
 * "initializeClientWithParameters:completion:". The context
 * parameters here are simple stand-ins (identifier / foreground /
 * count) so the test can prove the context reader works.
 * Returns the length written, or 0 on builder failure. */
static uint32_t build_device_request(uint8_t *buf, uint32_t cap) {
    xw_builder b;
    if (xw_builder_init(&b, buf, cap) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_add_string(&b, FB_BSXPC_KEY_BSXPC, FB_MSG_CONNECT) != XW_OK)
        return 0;
    if (xw_dict_add_dict_begin(&b, FB_BSXPC_KEY_CONTEXT) != XW_OK) return 0;
    if (xw_dict_add_string(&b, "identifier",
                           "sceneID:com.example.Aidoku-default") != XW_OK)
        return 0;
    if (xw_dict_add_bool(&b, "foreground", 1) != XW_OK) return 0;
    if (xw_dict_add_uint64(&b, "count", 7) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    if (xw_dict_add_uint64(&b, FB_BSXPC_KEY_CID, 1) != XW_OK) return 0;
    if (xw_dict_add_bool(&b, FB_BSXPC_KEY_CIDR, 0) != XW_OK) return 0;
    if (xw_dict_add_string(&b, FB_BSXPC_KEY_SEL,
                           FB_MSG_INITIALIZE_CLIENT) != XW_OK)
        return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* The same request with an EMPTY bsxpc_context, used for the golden
 * byte vector below. */
static uint32_t build_device_request_empty_ctx(uint8_t *buf, uint32_t cap) {
    xw_builder b;
    if (xw_builder_init(&b, buf, cap) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_add_string(&b, FB_BSXPC_KEY_BSXPC, FB_MSG_CONNECT) != XW_OK)
        return 0;
    if (xw_dict_add_dict_begin(&b, FB_BSXPC_KEY_CONTEXT) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    if (xw_dict_add_uint64(&b, FB_BSXPC_KEY_CID, 1) != XW_OK) return 0;
    if (xw_dict_add_bool(&b, FB_BSXPC_KEY_CIDR, 0) != XW_OK) return 0;
    if (xw_dict_add_string(&b, FB_BSXPC_KEY_SEL,
                           FB_MSG_INITIALIZE_CLIENT) != XW_OK)
        return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* ------------------------------------------------------------------ */
/* Golden byte vector. The device request with an empty context,      */
/* hand-computed from the wire layout: magic "CPX@", version 5,       */
/* dict size/count words, 4-byte key padding, string length           */
/* including the NUL. 180 bytes total.                                */
/* ------------------------------------------------------------------ */

static const uint8_t g_golden_request[] = {
    0x43, 0x50, 0x58, 0x40, 0x05, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00,
    /* "bsxpc": "connect" */
    0x62, 0x73, 0x78, 0x70, 0x63, 0x00, 0x00, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x63, 0x6f, 0x6e, 0x6e, 0x65, 0x63, 0x74, 0x00,
    /* "bsxpc_context": {} */
    0x62, 0x73, 0x78, 0x70, 0x63, 0x5f, 0x63, 0x6f,
    0x6e, 0x74, 0x65, 0x78, 0x74, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    /* "bsxpc_CID": uint64 1 */
    0x62, 0x73, 0x78, 0x70, 0x63, 0x5f, 0x43, 0x49,
    0x44, 0x00, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    /* "bsxpc_CIDr": bool false */
    0x62, 0x73, 0x78, 0x70, 0x63, 0x5f, 0x43, 0x49,
    0x44, 0x72, 0x00, 0x00,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* "bsxpc_SEL": "initializeClientWithParameters:completion:" */
    0x62, 0x73, 0x78, 0x70, 0x63, 0x5f, 0x53, 0x45,
    0x4c, 0x00, 0x00, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00,
    0x69, 0x6e, 0x69, 0x74, 0x69, 0x61, 0x6c, 0x69,
    0x7a, 0x65, 0x43, 0x6c, 0x69, 0x65, 0x6e, 0x74,
    0x57, 0x69, 0x74, 0x68, 0x50, 0x61, 0x72, 0x61,
    0x6d, 0x65, 0x74, 0x65, 0x72, 0x73, 0x3a, 0x63,
    0x6f, 0x6d, 0x70, 0x6c, 0x65, 0x74, 0x69, 0x6f,
    0x6e, 0x3a, 0x00, 0x00
};

/* ------------------------------------------------------------------ */
/* Parse the device request.                                           */
/* ------------------------------------------------------------------ */

static void test_parse_device(void) {
    static uint8_t buf[1024];
    uint32_t len;
    xw_reader r;
    bsxpc_message m;
    xw_value v;
    const char *s = NULL;
    uint32_t slen = 0;
    uint64_t uv = 0;
    int flag = 0;

    printf("parse device request...\n");
    len = build_device_request(buf, sizeof(buf));
    CHECK(len > 0);
    CHECK_RC(init_reader(&r, buf, len, NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.kind != NULL && strcmp(m.kind, "connect") == 0);
    CHECK(m.sel != NULL &&
          strcmp(m.sel, "initializeClientWithParameters:completion:") == 0);
    CHECK(m.cid == 1);
    CHECK(m.cidr == 0);
    CHECK(m.has_kind == 1 && m.has_sel == 1);
    CHECK(m.has_cid == 1 && m.has_cidr == 1);
    CHECK(m.has_ctx == 1);
    CHECK(m.ctx.type == XW_TYPE_DICTIONARY);
    /* The parsed kind/SEL are the names-table strings. */
    CHECK(strcmp(m.kind, FB_MSG_CONNECT) == 0);
    CHECK(strcmp(m.sel, FB_MSG_INITIALIZE_CLIENT) == 0);
    /* Parameters are readable from the context value. */
    CHECK_RC(xw_dict_find_v(&m.ctx, "identifier", &v));
    CHECK_RC(xw_as_string(&v, &s, &slen));
    CHECK(s != NULL && strcmp(s, "sceneID:com.example.Aidoku-default") == 0);
    CHECK_RC(xw_dict_find_v(&m.ctx, "foreground", &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);
    CHECK_RC(xw_dict_find_v(&m.ctx, "count", &v));
    CHECK_RC(xw_as_uint64(&v, &uv));
    CHECK(uv == 7);
    /* A missing parameter is NOT_FOUND, not a crash. */
    CHECK(xw_dict_find_v(&m.ctx, "nosuch", &v) == XW_ERR_NOT_FOUND);

    /* The empty-context form parses too, with an empty dict. */
    len = build_device_request_empty_ctx(buf, sizeof(buf));
    CHECK(len > 0);
    CHECK_RC(init_reader(&r, buf, len, NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.cid == 1 && m.cidr == 0);
    CHECK(m.has_kind == 1 && m.has_sel == 1);
    CHECK(m.has_cid == 1 && m.has_cidr == 1);
    CHECK(m.has_ctx == 1);
    CHECK(m.ctx.type == XW_TYPE_DICTIONARY);
    CHECK(xw_dict_find_v(&m.ctx, "anything", &v) == XW_ERR_NOT_FOUND);
    printf("parse device request done: %d checks, %d failures\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* Golden byte vector.                                                 */
/* ------------------------------------------------------------------ */

static void test_golden(void) {
    static uint8_t buf[1024];
    uint32_t len;
    xw_reader r;
    bsxpc_message m;

    printf("golden vector...\n");
    len = build_device_request_empty_ctx(buf, sizeof(buf));
    CHECK(len == sizeof(g_golden_request));
    CHECK(memcmp(buf, g_golden_request, sizeof(g_golden_request)) == 0);
    /* The hand-computed bytes parse as the device request. */
    CHECK_RC(init_reader(&r, g_golden_request, sizeof(g_golden_request),
                         NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(strcmp(m.kind, "connect") == 0);
    CHECK(strcmp(m.sel, "initializeClientWithParameters:completion:") == 0);
    CHECK(m.cid == 1 && m.cidr == 0);
    CHECK(m.has_kind == 1 && m.has_sel == 1);
    CHECK(m.has_cid == 1 && m.has_cidr == 1);
    CHECK(m.has_ctx == 1);
    CHECK(m.ctx.type == XW_TYPE_DICTIONARY);
    printf("golden vector done: %d checks, %d failures\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* Reply build.                                                        */
/* ------------------------------------------------------------------ */

static void test_reply(void) {
    static uint8_t buf[1024];
    xw_builder b;
    xw_reader r;
    xw_value v;
    uint64_t uv = 0;
    int flag = 0;
    const char *s = NULL;
    uint32_t slen = 0;
    bsxpc_message m;
    int rc;

    printf("reply build...\n");
    /* A completion reply for CID 1, with result fields at the TOP
     * LEVEL [guess]: bsxpc_context is the handshake dict, not a general
     * result container. One xw_dict_end closes the reply dict. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(bsxpc_reply_build(&b, &g_env_keys, 1));
    CHECK_RC(bs_settings_set_flag(&b, "success", 1));
    CHECK_RC(bs_settings_set_string(&b, "identifier",
                                    "sceneID:com.example.Aidoku-default"));
    CHECK_RC(xw_dict_end(&b)); /* the reply dict: one close */

    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    /* The reply carries CID 1, CIDr true, and the result fields at
     * the top level; there is no bsxpc_context in a reply. */
    CHECK_RC(xw_dict_find(&r, "bsxpc_CID", &v));
    CHECK_RC(xw_as_uint64(&v, &uv));
    CHECK(uv == 1);
    CHECK_RC(xw_dict_find(&r, "bsxpc_CIDr", &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);
    CHECK(xw_dict_find(&r, "bsxpc_context", &v) == XW_ERR_NOT_FOUND);
    CHECK_RC(xw_dict_find(&r, "success", &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);
    CHECK_RC(xw_dict_find(&r, "identifier", &v));
    CHECK_RC(xw_as_string(&v, &s, &slen));
    CHECK(s != NULL && strcmp(s, "sceneID:com.example.Aidoku-default") == 0);
    /* The reply carries no bsxpc kind or SEL by design (contract
     * section 2: CIDr=true, same CID, top-level result fields), so
     * a lenient parse succeeds with only CID/CIDr present. */
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_cid == 1 && m.cid == 1);
    CHECK(m.has_cidr == 1 && m.cidr == 1);
    CHECK(m.has_kind == 0 && m.kind == NULL);
    CHECK(m.has_sel == 0 && m.sel == NULL);
    CHECK(m.has_ctx == 0);
    /* A reply for a different CID echoes that CID. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(bsxpc_reply_build(&b, &g_env_keys, 42));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    CHECK_RC(xw_dict_find(&r, "bsxpc_CID", &v));
    CHECK_RC(xw_as_uint64(&v, &uv));
    CHECK(uv == 42);
    CHECK_RC(xw_dict_find(&r, "bsxpc_CIDr", &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);

    /* Error paths. */
    CHECK(bsxpc_reply_build(NULL, &g_env_keys, 1) == XW_ERR_BAD_TYPE);
    CHECK(bsxpc_reply_build(&b, NULL, 1) == XW_ERR_BAD_TYPE);
    {
        bsxpc_envelope_keys bad = g_env_keys;
        bad.key_cid = NULL;
        CHECK(bsxpc_reply_build(&b, &bad, 1) == XW_ERR_BAD_TYPE);
        bad = g_env_keys;
        bad.key_context = NULL;
        CHECK(bsxpc_reply_build(&b, &bad, 1) == XW_ERR_BAD_TYPE);
    }
    /* Tiny buffer: the builder must fail cleanly, never overflow. */
    {
        uint8_t tiny[16];
        xw_builder tb;
        CHECK_RC(xw_builder_init(&tb, tiny, sizeof(tiny)));
        rc = bsxpc_reply_build(&tb, &g_env_keys, 1);
        CHECK(rc == XW_ERR_NOSPACE);
        CHECK(xw_builder_error(&tb) == XW_ERR_NOSPACE);
    }
    printf("reply build done: %d checks, %d failures\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* Parse error cases.                                                  */
/* ------------------------------------------------------------------ */

static void test_parse_errors(void) {
    static uint8_t buf[1024];
    xw_builder b;
    xw_reader r;
    bsxpc_message m;

    printf("parse errors...\n");
    /* Missing bsxpc_SEL: lenient parse succeeds, the SEL flag is
     * clear and the other fields still parse. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_dict_begin(&b, "bsxpc_context"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(xw_dict_add_uint64(&b, "bsxpc_CID", 1));
    CHECK_RC(xw_dict_add_bool(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_sel == 0 && m.sel == NULL);
    CHECK(m.has_kind == 1 && strcmp(m.kind, "connect") == 0);
    CHECK(m.has_cid == 1 && m.cid == 1);
    CHECK(m.has_cidr == 1 && m.cidr == 0);
    CHECK(m.has_ctx == 1);
    /* Wrong type for bsxpc_CID (string, not uint64). */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_dict_begin(&b, "bsxpc_context"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_CID", "one"));
    CHECK_RC(xw_dict_add_bool(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK(bsxpc_message_parse(&r, &g_env_keys, &m) == XW_ERR_BAD_TYPE);
    /* bsxpc_context is a string, not a dict. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_context", "notadict"));
    CHECK_RC(xw_dict_add_uint64(&b, "bsxpc_CID", 1));
    CHECK_RC(xw_dict_add_bool(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK(bsxpc_message_parse(&r, &g_env_keys, &m) == XW_ERR_BAD_TYPE);
    /* Wrong type for bsxpc_CIDr (uint64, not bool). */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_dict_begin(&b, "bsxpc_context"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(xw_dict_add_uint64(&b, "bsxpc_CID", 1));
    CHECK_RC(xw_dict_add_uint64(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK(bsxpc_message_parse(&r, &g_env_keys, &m) == XW_ERR_BAD_TYPE);
    /* Missing bsxpc_CID: lenient parse succeeds, the CID flag is
     * clear and the CID is 0, the other fields still parse. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_dict_begin(&b, "bsxpc_context"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(xw_dict_add_bool(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_cid == 0 && m.cid == 0);
    CHECK(m.has_kind == 1 && strcmp(m.kind, "connect") == 0);
    CHECK(m.has_sel == 1 && strcmp(m.sel, "x:") == 0);
    CHECK(m.has_cidr == 1 && m.cidr == 0);
    CHECK(m.has_ctx == 1);
    /* Empty dict: lenient parse succeeds with all flags clear. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_kind == 0 && m.kind == NULL);
    CHECK(m.has_sel == 0 && m.sel == NULL);
    CHECK(m.has_cid == 0 && m.cid == 0);
    CHECK(m.has_cidr == 0 && m.cidr == 0);
    CHECK(m.has_ctx == 0);
    /* Missing bsxpc kind: lenient parse succeeds, kind flag clear. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_dict_begin(&b, "bsxpc_context"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(xw_dict_add_uint64(&b, "bsxpc_CID", 1));
    CHECK_RC(xw_dict_add_bool(&b, "bsxpc_CIDr", 0));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_kind == 0 && m.kind == NULL);
    CHECK(m.has_sel == 1 && strcmp(m.sel, "x:") == 0);
    CHECK(m.has_cid == 1 && m.cid == 1);
    CHECK(m.has_ctx == 1);
    /* Missing bsxpc_context: lenient parse succeeds, ctx flag
     * clear; a one-way message without a CID/CIDr is accepted. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc", "connect"));
    CHECK_RC(xw_dict_add_string(&b, "bsxpc_SEL", "x:"));
    CHECK_RC(xw_dict_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK_RC(bsxpc_message_parse(&r, &g_env_keys, &m));
    CHECK(m.has_ctx == 0);
    CHECK(m.has_cid == 0 && m.cid == 0);
    CHECK(m.has_cidr == 0 && m.cidr == 0);
    CHECK(m.has_kind == 1 && strcmp(m.kind, "connect") == 0);
    CHECK(m.has_sel == 1 && strcmp(m.sel, "x:") == 0);
    /* Root is an array, not a dict. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(xw_array_begin(&b));
    CHECK_RC(xw_add_int64(&b, 1));
    CHECK_RC(xw_array_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    memset(&m, 0, sizeof(m));
    CHECK(bsxpc_message_parse(&r, &g_env_keys, &m) == XW_ERR_BAD_TYPE);
    /* NULL arguments. */
    CHECK(bsxpc_message_parse(NULL, &g_env_keys, &m) == XW_ERR_BAD_TYPE);
    CHECK(bsxpc_message_parse(&r, NULL, &m) == XW_ERR_BAD_TYPE);
    CHECK(bsxpc_message_parse(&r, &g_env_keys, NULL) == XW_ERR_BAD_TYPE);
    {
        bsxpc_envelope_keys bad = g_env_keys;
        bad.key_sel = NULL;
        CHECK(bsxpc_message_parse(&r, &bad, &m) == XW_ERR_BAD_TYPE);
    }
    printf("parse errors done: %d checks, %d failures\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* Smoke test: the kept functions still behave.                       */
/*                                                                     */
/* ------------------------------------------------------------------ */

static void test_kept_smoke(void) {
    static uint8_t buf[1024];
    xw_builder b;
    xw_reader r;
    char kind[32], target[32], bundle[128];
    uint64_t timeout = 0;
    bsxpc_process_handle h, out;
    int flag = 0;

    printf("kept functions smoke...\n");
    /* Settings round-trip. */
    CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
    CHECK_RC(bs_settings_begin(&b));
    CHECK_RC(bs_settings_set_flag(&b, "foreground", 1));
    CHECK_RC(bs_settings_set_int(&b, "level", -42));
    CHECK_RC(bs_settings_set_string(&b, "name", "Aidoku"));
    CHECK_RC(bs_settings_end(&b));
    CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
    CHECK_RC(bs_settings_get_flag(&r, "foreground", &flag));
    CHECK(flag == 1);
    /* Action build/parse. */
    {
        static const bsxpc_action_keys akeys = {
            "kind", "info", "target", "timeout", "response"
        };
        CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
        CHECK_RC(bsxpc_action_build(&b, &akeys, "activate", "scene", 5000));
        CHECK_RC(xw_dict_end(&b));
        CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
        CHECK_RC(bsxpc_action_parse(&r, &akeys, kind, sizeof(kind),
                                    target, sizeof(target), &timeout));
        CHECK(strcmp(kind, "activate") == 0);
        CHECK(strcmp(target, "scene") == 0);
        CHECK(timeout == 5000);
    }
    /* Handle build/parse. */
    {
        static const bsxpc_handle_keys hkeys = {
            "pid", "bundleID", "auditToken", "isRunning",
            "isForeground", "isValid", "isApplication"
        };
        memset(&h, 0, sizeof(h));
        memset(&out, 0, sizeof(out));
        CHECK_RC(bsxpc_audit_token_forge(&h.audit, 1234));
        h.pid = 1234;
        h.bundle_id = "com.example.Aidoku";
        h.is_running = 1;
        h.is_foreground = 1;
        h.is_valid = 1;
        h.is_application = 1;
        CHECK_RC(xw_builder_init(&b, buf, sizeof(buf)));
        CHECK_RC(bsxpc_handle_build(&b, &hkeys, &h));
        CHECK_RC(init_reader(&r, buf, xw_builder_len(&b), NULL, 0));
        CHECK_RC(bsxpc_handle_parse(&r, &hkeys, &out, bundle, sizeof(bundle)));
        CHECK(out.pid == 1234);
        CHECK(strcmp(bundle, "com.example.Aidoku") == 0);
        CHECK(out.audit.pid == 1234 && out.audit.pidversion == 1);
    }
    printf("kept functions smoke done: %d checks, %d failures\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* Fuzz: random mutations of valid buffers.                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t buf[2048];
    uint32_t len;
} corpus_t;

static uint32_t build_corpus(corpus_t *c) {
    uint32_t n = 0;
    xw_builder b;
    memset(c, 0, sizeof(*c));

    /* 0: device request (non-empty context). */
    c[0].len = build_device_request(c[0].buf, sizeof(c[0].buf));
    if (c[0].len > 0) n++;
    /* 1: device request, empty context (the golden vector). */
    c[1].len = build_device_request_empty_ctx(c[1].buf, sizeof(c[1].buf));
    if (c[1].len > 0) n++;
    /* 2: a completion reply for CID 1 with a result field at the
     * top level (one xw_dict_end). */
    if (xw_builder_init(&b, c[2].buf, sizeof(c[2].buf)) == XW_OK) {
        (void)bsxpc_reply_build(&b, &g_env_keys, 1);
        (void)bs_settings_set_flag(&b, "success", 1);
        (void)xw_dict_end(&b);
        c[2].len = xw_builder_len(&b);
        if (c[2].len > 0) n++;
    }
    /* 3: settings dict with every setter. */
    if (xw_builder_init(&b, c[3].buf, sizeof(c[3].buf)) == XW_OK) {
        static const uint8_t d4[4] = { 1, 2, 3, 4 };
        (void)bs_settings_begin(&b);
        (void)bs_settings_set_flag(&b, "foreground", 1);
        (void)bs_settings_set_int(&b, "level", -99);
        (void)bs_settings_set_uint(&b, "count", 42);
        (void)bs_settings_set_double(&b, "scale", 3.0);
        (void)bs_settings_set_string(&b, "name", "Aidoku");
        (void)bs_settings_set_data(&b, "blob", d4, 4);
        (void)bs_settings_set_size(&b, "pixelSize", 1080.0, 2400.0);
        (void)bs_settings_end(&b);
        c[3].len = xw_builder_len(&b);
        if (c[3].len > 0) n++;
    }
    /* 4: action dict with info. */
    if (xw_builder_init(&b, c[4].buf, sizeof(c[4].buf)) == XW_OK) {
        static const bsxpc_action_keys akeys = {
            "kind", "info", "target", "timeout", "response"
        };
        (void)bsxpc_action_build(&b, &akeys, "activate", "scene", 5000);
        (void)xw_dict_add_dict_begin(&b, "info");
        (void)bs_settings_set_string(&b, "reason", "foreground");
        (void)xw_dict_end(&b);
        (void)xw_dict_end(&b);
        c[4].len = xw_builder_len(&b);
        if (c[4].len > 0) n++;
    }
    /* 5: array root with mixed types and a nested dict. */
    if (xw_builder_init(&b, c[5].buf, sizeof(c[5].buf)) == XW_OK) {
        (void)xw_array_begin(&b);
        (void)xw_add_int64(&b, -1);
        (void)xw_add_string(&b, "two");
        (void)xw_add_double(&b, 2.5);
        (void)xw_dict_begin(&b);
        (void)xw_dict_add_bool(&b, "deep", 1);
        (void)xw_dict_end(&b);
        (void)xw_array_end(&b);
        c[5].len = xw_builder_len(&b);
        if (c[5].len > 0) n++;
    }
    return n;
}

static void fuzz(uint32_t iterations) {
    static corpus_t corpus[6];
    static uint8_t work[2048];
    uint32_t ncorpus, i;
    uint32_t accepted = 0, parsed = 0, logged = 0, bad_logged = 0;
    int devnull = -1, saved = -1;

    printf("fuzz: %u iterations...\n", iterations);
    ncorpus = build_corpus(corpus);
    CHECK(ncorpus == 6);

    /* Send all logger output to /dev/null during the fuzz loop. */
    devnull = open("/dev/null", O_WRONLY);
    CHECK(devnull >= 0);
    saved = dup(2);
    CHECK(saved >= 0);
    CHECK(dup2(devnull, 2) >= 0);
    close(devnull);
    bsxpc_set_trace(1);

    for (i = 0; i < iterations; i++) {
        const corpus_t *c = &corpus[i % ncorpus];
        uint32_t len = c->len;
        uint32_t mode = rng_next() % 5;
        xw_reader r;
        int rc;
        if (len == 0 || len > sizeof(work)) continue;
        memcpy(work, c->buf, len);
        switch (mode) {
        case 0: /* flip one random byte */
            work[rng_next() % len] ^= (uint8_t)(1u << (rng_next() % 8));
            break;
        case 1: /* overwrite one random u32 */
            if (len >= 4) {
                uint32_t off = rng_next() % (len - 3);
                uint32_t w = rng_next();
                work[off] = (uint8_t)w;
                work[off + 1] = (uint8_t)(w >> 8);
                work[off + 2] = (uint8_t)(w >> 16);
                work[off + 3] = (uint8_t)(w >> 24);
            }
            break;
        case 2: /* zero a random span */
            if (len >= 8) {
                uint32_t off = rng_next() % (len - 7);
                uint32_t span = 1 + (rng_next() % 8);
                if (off + span > len) span = len - off;
                memset(work + off, 0, span);
            }
            break;
        case 3: /* truncate */
            len = rng_next() % (len + 1);
            break;
        default: /* leave valid: 1 in 5 messages is unmutated */
            break;
        }
        rc = init_reader(&r, work, len, NULL, 0);
        if (rc == XW_OK) {
            bsxpc_message m;
            accepted++;
            /* The new parse must never crash or read out of
             * bounds on a mutated-but-valid message; its return
             * value is unchecked (any XW_OK or XW_ERR_* is fine).
             * When it succeeds, reading a parameter from the
             * context must also be safe. */
            memset(&m, 0, sizeof(m));
            rc = bsxpc_message_parse(&r, &g_env_keys, &m);
            if (rc == XW_OK) {
                xw_value v;
                const char *s = NULL;
                uint32_t slen = 0;
                parsed++;
                (void)xw_dict_find_v(&m.ctx, "identifier", &v);
                (void)xw_as_string(&v, &s, &slen);
            }
            rc = bsxpc_log_dict(&r, "[fuzz]");
            if (rc == XW_OK) logged++;
        } else {
            /* Invalid reader: the logger must print <malformed>
             * and return negative, never crash. */
            rc = bsxpc_log_dict(&r, "[fuzz]");
            if (rc < 0) bad_logged++;
        }
        /* Builder fuzz: random setter sequence on a small buffer. */
        {
            uint8_t small[64];
            xw_builder b;
            xw_reader br;
            uint32_t k;
            if (xw_builder_init(&b, small, sizeof(small)) == XW_OK) {
                (void)bs_settings_begin(&b);
                for (k = 0; k < 4; k++) {
                    switch (rng_next() % 6) {
                    case 0:
                        (void)bs_settings_set_flag(&b, "f", 1);
                        break;
                    case 1:
                        (void)bs_settings_set_int(&b, "i", -7);
                        break;
                    case 2:
                        (void)bs_settings_set_string(&b, "s", "x");
                        break;
                    case 3:
                        (void)bs_settings_set_double(&b, "d", 1.5);
                        break;
                    case 4:
                        (void)bs_settings_set_size(&b, "sz", 1.0, 2.0);
                        break;
                    default:
                        (void)bs_settings_set_uint(&b, "u", 9);
                        break;
                    }
                }
                (void)bs_settings_end(&b);
                if (xw_builder_error(&b) == XW_OK) {
                    (void)init_reader(&br, small, xw_builder_len(&b), NULL, 0);
                }
            }
        }
    }

    dup2(saved, 2);
    close(saved);
    bsxpc_set_trace(-1);
    printf("fuzz: %u iterations, reader accepted %u, parsed ok %u, "
           "logger ok %u, malformed logged %u\n",
           iterations, accepted, parsed, logged, bad_logged);
    CHECK(bad_logged > 0); /* the mutations must have produced failures */
    printf("fuzz done: %d checks, %d failures\n", g_checks, g_failures);
}

int main(int argc, char **argv) {
    uint32_t iterations = 50000;
    if (argc > 1) iterations = (uint32_t)strtoul(argv[1], NULL, 10);
    test_parse_device();
    test_golden();
    test_reply();
    test_parse_errors();
    test_kept_smoke();
    fuzz(iterations);
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
