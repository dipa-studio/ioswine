/* daemons_test.c: host test for daemons (combined simulator skeleton).
 *
 * What it covers:
 *   1. bplist round-trip: a nested dict with every supported type.
 *   2. bplist scalar edges: int/uint size boundaries, reals, empty
 *      containers, dates, unicode strings, invalid UTF-8.
 *   3. bplist malformed inputs: bad magic, short buffers, bad
 *      trailer fields, truncated objects, unknown markers, UID,
 *      non-string dict keys, out-of-range refs, bad size forms.
 *   4. bplist arena exhaustion and hand-built deep nesting.
 *   5. daemons_init(): all four service inits called in order, the
 *      ping service port registered, idempotent second call.
 *   6. svchost ping round-trip through svchost_process_msg.
 *   7. Fuzz svchost_process_msg (10000 iterations).
 *   8. Fuzz bp_parse (5000 iterations).
 *
 * How the services are driven on host: there is no real ipc.c here,
 * so this file stubs the ipc_* symbols the .c files reference
 * (found with `nm -u` on the compiled objects: g_ipc_trace,
 * lsk_getenv, ipc_task_self, ipc_mach_reply_port,
 * ipc_svchost_port_create, ipc_port_from_name, ipc_port_deallocate,
 * ipc_port_destroy, ipc_msg). ipc_msg emulates ONLY the
 * bootstrap_register2 path (msgh_id 0x100000cc) that
 * svchost_register's self-registration uses: it records name ->
 * port in a small static table and builds a reply {"error": 0}
 * with msgh_id 0x20000000 in the same buffer. Any other msgh_id
 * returns MACH_SEND_INVALID_DEST. daemons_init() is then called
 * for real (against the test stubs for the service inits)
 * and the ping service is driven directly with hand-built Mach
 * messages (header + XPC body from xw_builder). Every reply is
 * parsed with xw_reader and the fields are checked.
 *
 * Test-side wire keys: the ping service's keys ("ping", "payload",
 * "pong", "error") are defined here as TEST_* defines. The service .c files themselves never carry literals.
 * The "error" key value is DAEMON_KEY_ERROR ("error", see
 * daemon_names.h).
 *
 * Build: The target linux.h
 * is aarch64-only, so the build needs the host shim first on the
 * -I path (tests/host/host-shim/linux.h); it translates the
 * aarch64 syscall numbers the target code uses to host libc calls.
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/daemons_test tests/host/daemons_test.c \
 *        src/bplist.c src/daemons.c src/svchost.c src/xpcwire.c
 *     /tmp/daemons_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/daemons_test_asan \
 *        tests/host/daemons_test.c src/bplist.c src/daemons.c \
 *        src/svchost.c src/xpcwire.c
 *     /tmp/daemons_test_asan
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "xpcwire.h"
#include "svchost.h"
#include "bplist.h"
#include "daemons.h"
#include "launchd.h"

/* ------------------------------------------------------------------ */
/* Test-side wire keys (see the file header).                          */
/* ------------------------------------------------------------------ */

#define TEST_ERROR_KEY "error"
#define TEST_PING_KEY "ping"
#define TEST_PONG_KEY "pong"
#define TEST_PAYLOAD_KEY "payload"
#define TEST_MESSAGE_KEY "message"

#define SIM_PING_PORT ((mach_port_name_t)0x3000)

/* ------------------------------------------------------------------ */
/* ipc stubs (no real ipc.c on host). See the file header.             */
/* ------------------------------------------------------------------ */

int g_ipc_trace = 0;

const char *lsk_getenv(const char *name) {
    return getenv(name);
}

mach_port_name_t ipc_task_self(void) {
    return 1;
}

static uint32_t g_test_next_port = 0x1000;

mach_port_name_t ipc_svchost_port_create(void) {
    return g_test_next_port++;
}

mach_port_name_t ipc_mach_reply_port(void) {
    return g_test_next_port++;
}

struct ipc_port *ipc_port_from_name(mach_port_name_t name) {
    (void)name;
    return NULL;
}

int ipc_port_deallocate(mach_port_t task, mach_port_name_t name) {
    (void)task;
    (void)name;
    return KERN_SUCCESS;
}

int ipc_port_destroy(mach_port_t task, mach_port_name_t name) {
    (void)task;
    (void)name;
    return KERN_SUCCESS;
}

/* bootstrap_register2 table for the ipc_msg stub. */
#define TEST_MAX_REG 16
static char g_test_reg_names[TEST_MAX_REG][128];
static mach_port_name_t g_test_reg_ports[TEST_MAX_REG];
static int g_test_reg_count = 0;

int ipc_msg(void *msg, uint64_t option, uint32_t send_size,
            uint32_t rcv_size, mach_port_name_t rcv_name,
            uint32_t timeout, mach_port_name_t notify,
            void *scatter, uint32_t scatter_size) {
    mach_msg_header_t *hdr = (mach_msg_header_t *)msg;
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    const char *name = NULL;
    uint32_t nlen = 0;
    uint8_t body[64];
    xw_builder b;
    uint32_t blen = 0;
    mach_port_name_t reply_port;
    int i;
    (void)option;
    (void)rcv_size;
    (void)rcv_name;
    (void)timeout;
    (void)notify;
    (void)scatter;
    (void)scatter_size;
    /* Only the bootstrap_register2 path is emulated. */
    if ((hdr->msgh_id & 0xffffffu) != LAUNCHD_ROUTINE_REGISTER)
        return MACH_SEND_INVALID_DEST;
    if (xw_envelope_parse(hdr, send_size, &env) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (env.ndescs < 1)
        return MACH_SEND_INVALID_DEST;
    memset(&rd, 0, sizeof(rd));
    if (xw_reader_init(&rd, env.payload, env.payload_len,
                       env.descs, env.ndescs) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_find(&rd, "name", &v) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_as_string(&v, &name, &nlen) != XW_OK || name == NULL)
        return MACH_SEND_INVALID_DEST;
    for (i = 0; i < g_test_reg_count; i++) {
        if (strcmp(g_test_reg_names[i], name) == 0) {
            g_test_reg_ports[i] = (mach_port_name_t)env.descs[0].obj;
            break;
        }
    }
    if (i == g_test_reg_count && g_test_reg_count < TEST_MAX_REG) {
        strncpy(g_test_reg_names[g_test_reg_count], name, 127);
        g_test_reg_names[g_test_reg_count][127] = '\0';
        g_test_reg_ports[g_test_reg_count] =
            (mach_port_name_t)env.descs[0].obj;
        g_test_reg_count++;
    }
    /* Reply {"error": 0} in place, msgh_id 0x20000000. */
    if (xw_builder_init(&b, body, sizeof(body)) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_begin(&b) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_add_int64(&b, TEST_ERROR_KEY, 0) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_end(&b) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    blen = xw_builder_len(&b);
    reply_port = hdr->msgh_local_port;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    hdr->msgh_size = (uint32_t)sizeof(mach_msg_header_t) + blen;
    hdr->msgh_remote_port = reply_port;
    hdr->msgh_local_port = MACH_PORT_NULL;
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = 0x20000000u;
    memcpy((uint8_t *)msg + sizeof(mach_msg_header_t), body, blen);
    return MACH_MSG_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* TEST STUBS for the service inits.                                  */
/*                                                                     */
/* These are NOT the real services. Each stub records that it was     */
/* called (in call order) and returns KERN_SUCCESS, so the test        */
/* can prove daemons_init() wires the services in expected order.     */
/*                                                                     */
/*                                                                     */
/* ------------------------------------------------------------------ */

static int g_stub_calls = 0;
static int g_stub_cfprefsd_calls = 0;
static int g_stub_notifyd_calls = 0;
static int g_stub_mg_calls = 0;
static int g_stub_lsd_calls = 0;
static int g_stub_cfprefsd_order = 0;
static int g_stub_notifyd_order = 0;
static int g_stub_mg_order = 0;
static int g_stub_lsd_order = 0;

int cfprefsd_init(void) {
    g_stub_calls++;
    g_stub_cfprefsd_calls++;
    g_stub_cfprefsd_order = g_stub_calls;
    return KERN_SUCCESS;
}

int notifyd_init(void) {
    g_stub_calls++;
    g_stub_notifyd_calls++;
    g_stub_notifyd_order = g_stub_calls;
    return KERN_SUCCESS;
}

int mg_init(void) {
    g_stub_calls++;
    g_stub_mg_calls++;
    g_stub_mg_order = g_stub_calls;
    return KERN_SUCCESS;
}

int lsd_init(void) {
    g_stub_calls++;
    g_stub_lsd_calls++;
    g_stub_lsd_order = g_stub_calls;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Harness.                                                            */
/* ------------------------------------------------------------------ */

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

static int g_scen_failures_before = 0;

#define SCEN_BEGIN(n, name) do { \
    g_scen_failures_before = g_failures; \
    printf("SCENARIO %d: %s ... ", (n), (name)); \
    fflush(stdout); \
} while (0)

#define SCEN_END(n) do { \
    if (g_failures == g_scen_failures_before) { \
        printf("PASS\n"); \
    } else { \
        printf("FAIL (%d failed checks)\n", \
               g_failures - g_scen_failures_before); \
    } \
    fflush(stdout); \
} while (0)

/* Deterministic xorshift PRNG (same shape as the xpcwire test). */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* Little-endian u32 writer for hand-built Mach headers. */
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Big-endian u64 writer for hand-built bplist trailers. */
static void put_u64be(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)((v >> 56) & 0xffu);
    p[1] = (uint8_t)((v >> 48) & 0xffu);
    p[2] = (uint8_t)((v >> 40) & 0xffu);
    p[3] = (uint8_t)((v >> 32) & 0xffu);
    p[4] = (uint8_t)((v >> 24) & 0xffu);
    p[5] = (uint8_t)((v >> 16) & 0xffu);
    p[6] = (uint8_t)((v >> 8) & 0xffu);
    p[7] = (uint8_t)(v & 0xffu);
}

/* ------------------------------------------------------------------ */
/* Mach message delivery.                                              */
/* ------------------------------------------------------------------ */

/* Build a Mach message in buf: 24-byte header, then the XPC body.
 * Returns the total size, or 0 when the buffer is too small. */
static uint32_t build_mach_msg(uint8_t *buf, uint32_t cap,
                               mach_port_name_t dest,
                               mach_port_name_t reply_port,
                               const uint8_t *body, uint32_t body_len) {
    uint32_t total = (uint32_t)sizeof(mach_msg_header_t) + body_len;
    uint8_t *p = buf;
    if (cap < total) return 0;
    put_u32le(p + 0, 0x1513u); /* COPY_SEND | MAKE_SEND_ONCE<<8 */
    put_u32le(p + 4, total);
    put_u32le(p + 8, dest);
    put_u32le(p + 12, reply_port);
    put_u32le(p + 16, 0);
    put_u32le(p + 20, XW_MSGH_ID_XPC);
    p += sizeof(mach_msg_header_t);
    memcpy(p, body, body_len);
    return total;
}

/* Deliver a built message to svchost_process_msg. Returns the
 * entry's return value (0 or 1), or -1 when the buffer is too small. */
static int deliver(mach_port_name_t dest, mach_port_name_t reply_port,
                   const uint8_t *body, uint32_t body_len,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    uint8_t msg[4096];
    uint32_t size = build_mach_msg(msg, sizeof(msg), dest, reply_port,
                                   body, body_len);
    if (size == 0) return -1;
    return svchost_process_msg((const mach_msg_header_t *)msg, size,
                               reply_buf, reply_cap, out_reply_size);
}

/* Parse a full Mach message (reply) into a reader. Returns XW_OK
 * or a negative XW_ERR_*. Also checks the reply id is 0x20000000. */
static int parse_reply(const uint8_t *reply_buf, uint32_t reply_size,
                       xw_envelope *env, xw_reader *r) {
    int rc;
    if (reply_size < sizeof(mach_msg_header_t)) return XW_ERR_TRUNCATED;
    rc = xw_envelope_parse(reply_buf, reply_size, env);
    if (rc != XW_OK) return rc;
    memset(r, 0, sizeof(*r));
    rc = xw_reader_init(r, env->payload, env->payload_len,
                        env->descs, env->ndescs);
    if (rc != XW_OK) return rc;
    if (env->id != 0x20000000u) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Read the "error" int64 out of a reply. Returns XW_OK on success. */
static int reply_error_code(const uint8_t *reply_buf, uint32_t reply_size,
                            int64_t *err_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, TEST_ERROR_KEY, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

/* Build an XPC body for a ping request: {"ping": true} plus an
 * optional "payload" string. */
static int build_ping_body(uint8_t *body, uint32_t cap,
                           const char *payload, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_bool(&b, TEST_PING_KEY, 1);
    if (r == XW_OK && payload != NULL)
        r = xw_dict_add_string(&b, TEST_PAYLOAD_KEY, payload);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Build an XPC body for an unknown message: {"nope": 1}. */
static int build_unknown_body(uint8_t *body, uint32_t cap,
                              uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_int64(&b, "nope", 1);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* bplist helpers.                                                     */
/* ------------------------------------------------------------------ */

/* Bounded tree walk used after fuzz parses: checks every node is
 * well-formed (type in range, strings NUL-terminated in the arena,
 * data pointers present when len > 0). Returns 0 or -1. */
static int walk_tree(const bp_value *v, uint32_t depth, uint32_t *count) {
    const bp_value *c;
    if (v == NULL) return -1;
    if (depth > 40u) return -1;
    (*count)++;
    if (*count > 10000u) return -1;
    if (v->type > (uint32_t)BP_DICT) return -1;
    if (v->type == BP_STRING) {
        if (v->s == NULL || v->len == 0) return -1;
        if (v->s[v->len - 1u] != 0) return -1;
    }
    if (v->type == BP_DATA) {
        if (v->len > 0 && v->s == NULL) return -1;
    }
    for (c = v->child; c != NULL; c = c->next) {
        if (walk_tree(c, depth + 1u, count) != 0) return -1;
    }
    return 0;
}

/* Wrap raw object bytes into a minimal bplist: header, the objects
 * in order, an offset table (offsetIntSize 1) and a trailer
 * (refSize 1, top object 0). objs[i] points at the i-th object's
 * bytes, lens[i] at its length. Returns the total length, or 0
 * when the buffer is too small. */
static uint32_t build_raw_bplist(uint8_t *out, uint32_t cap,
                                 const uint8_t **objs,
                                 const uint32_t *lens, uint32_t nobj) {
    uint32_t offs[64];
    uint32_t pos = 8;
    uint32_t table_off;
    uint32_t i;
    if (nobj > 64u) return 0;
    if (cap < 8u + 32u) return 0;
    memcpy(out, "bplist00", 8);
    for (i = 0; i < nobj; i++) {
        if (pos + lens[i] > cap) return 0;
        offs[i] = pos;
        memcpy(out + pos, objs[i], lens[i]);
        pos += lens[i];
    }
    table_off = pos;
    if (table_off + nobj + 32u > cap) return 0;
    for (i = 0; i < nobj; i++) out[table_off + i] = (uint8_t)offs[i];
    pos = table_off + nobj;
    memset(out + pos, 0, 6);
    out[pos + 6] = 1; /* offsetIntSize */
    out[pos + 7] = 1; /* objectRefSize */
    put_u64be(out + pos + 8, (uint64_t)nobj);
    put_u64be(out + pos + 16, 0); /* top object */
    put_u64be(out + pos + 24, (uint64_t)table_off);
    return pos + 32u;
}

/* Build a bplist of N nested single-element arrays ending in an
 * int 42: object i (0..N-1) is an array holding ref i+1, object N
 * is the int. Returns the total length, or 0 when the buffer is
 * too small. */
static uint32_t build_deep_bplist(uint8_t *out, uint32_t cap,
                                  uint32_t narrays) {
    uint32_t nobj = narrays + 1u;
    uint32_t pos = 8;
    uint32_t table_off;
    uint32_t i;
    if (cap < 8u + 2u * nobj + nobj + 32u) return 0;
    memcpy(out, "bplist00", 8);
    for (i = 0; i < narrays; i++) {
        out[pos++] = 0xa1;
        out[pos++] = (uint8_t)(i + 1u);
    }
    out[pos++] = 0x10;
    out[pos++] = 42;
    table_off = pos;
    for (i = 0; i < nobj; i++) out[table_off + i] = (uint8_t)(8u + 2u * i);
    pos = table_off + nobj;
    memset(out + pos, 0, 6);
    out[pos + 6] = 1;
    out[pos + 7] = 1;
    put_u64be(out + pos + 8, (uint64_t)nobj);
    put_u64be(out + pos + 16, 0);
    put_u64be(out + pos + 24, (uint64_t)table_off);
    return pos + 32u;
}

/* ------------------------------------------------------------------ */
/* Scenario 1: bplist round-trip, every type.                          */
/* ------------------------------------------------------------------ */

static uint8_t g_rt_bplist[4096];
static uint32_t g_rt_bplist_len = 0;

static void scen_1_bplist_roundtrip(void) {
    static uint8_t arena_buf[8192];
    static uint8_t arena2_buf[8192];
    bp_arena a, a2;
    bp_value *root, *arr, *sub, *v;
    uint8_t out[4096];
    uint32_t out_len = 0;
    bp_value *parsed = NULL;
    int rc;
    SCEN_BEGIN(1, "bplist round-trip");
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new(&a, BP_DICT);
    CHECK(root != NULL);
    CHECK_RC(bp_dict_set(&a, root, "name",
                         bp_new_string(&a, "com.example.App")));
    CHECK_RC(bp_dict_set(&a, root, "count", bp_new_int(&a, -123456789)));
    CHECK_RC(bp_dict_set(&a, root, "big", bp_new_uint(&a, 4000000000u)));
    CHECK_RC(bp_dict_set(&a, root, "enabled", bp_new_bool(&a, 1)));
    CHECK_RC(bp_dict_set(&a, root, "ratio", bp_new_real(&a, 3.25)));
    CHECK_RC(bp_dict_set(&a, root, "blob",
                         bp_new_data(&a, "\x01\x02\x03\xff", 4)));
    CHECK_RC(bp_dict_set(&a, root, "when",
                         bp_new(&a, BP_DATE)));
    CHECK(bp_dict_get(root, "when") != NULL);
    bp_dict_get(root, "when")->i = 1700000000000000000LL;
    CHECK_RC(bp_dict_set(&a, root, "nothing", bp_new(&a, BP_NULL)));
    CHECK_RC(bp_dict_set(&a, root, "uni",
                         bp_new_string(&a, "caf\xC3\xA9 \xE2\x98\x83 "
                                           "\xF0\x9F\x98\x80")));
    arr = bp_new(&a, BP_ARRAY);
    CHECK(arr != NULL);
    CHECK_RC(bp_array_append(&a, arr, bp_new_int(&a, 1)));
    CHECK_RC(bp_array_append(&a, arr, bp_new_string(&a, "two")));
    CHECK_RC(bp_array_append(&a, arr, bp_new_bool(&a, 0)));
    CHECK_RC(bp_dict_set(&a, root, "items", arr));
    sub = bp_new(&a, BP_DICT);
    CHECK(sub != NULL);
    CHECK_RC(bp_dict_set(&a, sub, "x", bp_new_int(&a, 7)));
    CHECK_RC(bp_dict_set(&a, root, "sub", sub));
    /* Overwrite an existing key: the new value wins. */
    CHECK_RC(bp_dict_set(&a, root, "count", bp_new_int(&a, 99)));
    CHECK(bp_dict_get(root, "count") != NULL);
    CHECK(bp_dict_get(root, "count")->i == 99);
    CHECK(bp_dict_get(root, "missing") == NULL);
    /* Serialise. */
    rc = bp_write(root, out, sizeof(out), &out_len);
    CHECK_RC(rc);
    CHECK(out_len > 40u);
    CHECK(out_len <= sizeof(out));
    CHECK(memcmp(out, "bplist00", 8) == 0);
    memcpy(g_rt_bplist, out, out_len);
    g_rt_bplist_len = out_len;
    /* Parse back into a fresh arena. */
    bp_arena_init(&a2, arena2_buf, sizeof(arena2_buf));
    rc = bp_parse(out, out_len, &a2, &parsed);
    CHECK_RC(rc);
    CHECK(parsed != NULL);
    if (parsed == NULL) {
        SCEN_END(1);
        return;
    }
    CHECK(parsed->type == (uint32_t)BP_DICT);
    v = bp_dict_get(parsed, "name");
    CHECK(v != NULL && v->type == (uint32_t)BP_STRING);
    if (v != NULL) {
        CHECK(v->s != NULL && strcmp((const char *)v->s,
                                     "com.example.App") == 0);
    }
    v = bp_dict_get(parsed, "count");
    CHECK(v != NULL && v->type == (uint32_t)BP_INT && v->i == 99);
    v = bp_dict_get(parsed, "big");
    /* BP_UINT writes as a bplist int and reads back as BP_INT. */
    CHECK(v != NULL && v->type == (uint32_t)BP_INT && v->i == 4000000000LL);
    v = bp_dict_get(parsed, "enabled");
    CHECK(v != NULL && v->type == (uint32_t)BP_BOOL && v->b == 1);
    v = bp_dict_get(parsed, "ratio");
    CHECK(v != NULL && v->type == (uint32_t)BP_REAL && v->d == 3.25);
    v = bp_dict_get(parsed, "blob");
    CHECK(v != NULL && v->type == (uint32_t)BP_DATA && v->len == 4);
    if (v != NULL && v->len == 4) {
        CHECK(v->s[0] == 0x01 && v->s[1] == 0x02 && v->s[2] == 0x03 &&
              v->s[3] == 0xff);
    }
    v = bp_dict_get(parsed, "when");
    CHECK(v != NULL && v->type == (uint32_t)BP_DATE);
    if (v != NULL) {
        int64_t diff = v->i - 1700000000000000000LL;
        if (diff < 0) diff = -diff;
        CHECK(diff < 1000000LL); /* within 1 ms, see bplist.h */
    }
    v = bp_dict_get(parsed, "nothing");
    CHECK(v != NULL && v->type == (uint32_t)BP_NULL);
    v = bp_dict_get(parsed, "uni");
    CHECK(v != NULL && v->type == (uint32_t)BP_STRING);
    if (v != NULL) {
        CHECK(v->s != NULL && strcmp((const char *)v->s,
                                     "caf\xC3\xA9 \xE2\x98\x83 "
                                     "\xF0\x9F\x98\x80") == 0);
    }
    v = bp_dict_get(parsed, "items");
    CHECK(v != NULL && v->type == (uint32_t)BP_ARRAY);
    if (v != NULL && v->child != NULL) {
        CHECK(v->child->type == (uint32_t)BP_INT && v->child->i == 1);
        CHECK(v->child->next != NULL &&
              v->child->next->type == (uint32_t)BP_STRING);
        if (v->child->next != NULL) {
            CHECK(strcmp((const char *)v->child->next->s, "two") == 0);
        }
        CHECK(v->child->next != NULL &&
              v->child->next->next != NULL &&
              v->child->next->next->type == (uint32_t)BP_BOOL &&
              v->child->next->next->b == 0);
    }
    v = bp_dict_get(parsed, "sub");
    CHECK(v != NULL && v->type == (uint32_t)BP_DICT);
    if (v != NULL) {
        bp_value *x = bp_dict_get(v, "x");
        CHECK(x != NULL && x->type == (uint32_t)BP_INT && x->i == 7);
    }
    /* Re-serialise the parsed tree: byte-identical to the first write
     * except the date, which lost sub-microsecond precision. */
    {
        uint8_t out2[4096];
        uint32_t out2_len = 0;
        rc = bp_write(parsed, out2, sizeof(out2), &out2_len);
        CHECK_RC(rc);
        CHECK(out2_len == out_len);
    }
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: bplist scalar edges.                                    */
/* ------------------------------------------------------------------ */

static void check_int_roundtrip(int64_t val) {
    static uint8_t arena_buf[1024];
    static uint8_t arena2_buf[1024];
    bp_arena a, a2;
    bp_value *root;
    uint8_t out[256];
    uint32_t out_len = 0;
    bp_value *parsed = NULL;
    int rc;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new_int(&a, val);
    CHECK(root != NULL);
    rc = bp_write(root, out, sizeof(out), &out_len);
    CHECK_RC(rc);
    bp_arena_init(&a2, arena2_buf, sizeof(arena2_buf));
    rc = bp_parse(out, out_len, &a2, &parsed);
    CHECK_RC(rc);
    CHECK(parsed != NULL);
    if (parsed != NULL) {
        CHECK(parsed->type == (uint32_t)BP_INT);
        CHECK(parsed->i == val);
    }
}

static void check_uint_roundtrip(uint64_t val) {
    static uint8_t arena_buf[1024];
    static uint8_t arena2_buf[1024];
    bp_arena a, a2;
    bp_value *root;
    uint8_t out[256];
    uint32_t out_len = 0;
    bp_value *parsed = NULL;
    int rc;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new_uint(&a, val);
    CHECK(root != NULL);
    rc = bp_write(root, out, sizeof(out), &out_len);
    CHECK_RC(rc);
    bp_arena_init(&a2, arena2_buf, sizeof(arena2_buf));
    rc = bp_parse(out, out_len, &a2, &parsed);
    CHECK_RC(rc);
    CHECK(parsed != NULL);
    if (parsed != NULL) {
        /* Reads back as BP_INT (bplist has no uint type). */
        CHECK(parsed->type == (uint32_t)BP_INT);
        CHECK((uint64_t)parsed->i == val);
    }
}

static void scen_2_bplist_scalars(void) {
    static uint8_t arena_buf[1024];
    static uint8_t arena2_buf[1024];
    bp_arena a, a2;
    bp_value *root, *parsed = NULL;
    uint8_t out[512];
    uint32_t out_len = 0;
    int rc;
    SCEN_BEGIN(2, "bplist scalar edges");
    check_int_roundtrip(0);
    check_int_roundtrip(127);
    check_int_roundtrip(128);
    check_int_roundtrip(-129);
    check_int_roundtrip(32767);
    check_int_roundtrip(32768);
    check_int_roundtrip(-32769);
    check_int_roundtrip(2147483647LL);
    check_int_roundtrip(-2147483648LL);
    check_int_roundtrip(2147483648LL);
    check_int_roundtrip(9223372036854775807LL);
    check_int_roundtrip(-9223372036854775807LL - 1);
    check_uint_roundtrip(0u);
    check_uint_roundtrip(255u);
    check_uint_roundtrip(256u);
    check_uint_roundtrip(65535u);
    check_uint_roundtrip(4294967295u);
    check_uint_roundtrip(9223372036854775807u);
    check_uint_roundtrip(18446744073709551615u);
    /* Reals round-trip exactly through the 8-byte encoding. */
    {
        double reals[4];
        int i;
        reals[0] = 0.0;
        reals[1] = -1.5;
        reals[2] = 1e300;
        reals[3] = 3.141592653589793;
        for (i = 0; i < 4; i++) {
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            root = bp_new_real(&a, reals[i]);
            CHECK(root != NULL);
            rc = bp_write(root, out, sizeof(out), &out_len);
            CHECK_RC(rc);
            bp_arena_init(&a2, arena2_buf, sizeof(arena2_buf));
            rc = bp_parse(out, out_len, &a2, &parsed);
            CHECK_RC(rc);
            CHECK(parsed != NULL);
            if (parsed != NULL) {
                CHECK(parsed->type == (uint32_t)BP_REAL);
                CHECK(parsed->d == reals[i]);
            }
        }
    }
    /* Empty string, empty data, empty array, empty dict. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new(&a, BP_DICT);
    CHECK(root != NULL);
    CHECK_RC(bp_dict_set(&a, root, "empty_str",
                         bp_new_string(&a, "")));
    CHECK_RC(bp_dict_set(&a, root, "empty_data",
                         bp_new_data(&a, "", 0)));
    CHECK_RC(bp_dict_set(&a, root, "empty_arr", bp_new(&a, BP_ARRAY)));
    CHECK_RC(bp_dict_set(&a, root, "empty_dict", bp_new(&a, BP_DICT)));
    rc = bp_write(root, out, sizeof(out), &out_len);
    CHECK_RC(rc);
    bp_arena_init(&a2, arena2_buf, sizeof(arena2_buf));
    rc = bp_parse(out, out_len, &a2, &parsed);
    CHECK_RC(rc);
    CHECK(parsed != NULL);
    if (parsed != NULL) {
        bp_value *v = bp_dict_get(parsed, "empty_str");
        CHECK(v != NULL && v->type == (uint32_t)BP_STRING &&
              v->len == 1 && v->s[0] == 0);
        v = bp_dict_get(parsed, "empty_data");
        CHECK(v != NULL && v->type == (uint32_t)BP_DATA && v->len == 0);
        v = bp_dict_get(parsed, "empty_arr");
        CHECK(v != NULL && v->type == (uint32_t)BP_ARRAY &&
              v->child == NULL);
        v = bp_dict_get(parsed, "empty_dict");
        CHECK(v != NULL && v->type == (uint32_t)BP_DICT &&
              v->child == NULL);
    }
    /* Invalid UTF-8 is rejected by the writer. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new_string(&a, "bad\xff""utf8");
    CHECK(root != NULL);
    rc = bp_write(root, out, sizeof(out), &out_len);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Output buffer too small. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = bp_new_string(&a, "this string does not fit in eight bytes");
    CHECK(root != NULL);
    rc = bp_write(root, out, 8, &out_len);
    CHECK(rc == XW_ERR_NOSPACE);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: bplist malformed inputs.                                */
/* ------------------------------------------------------------------ */

static void scen_3_bplist_malformed(void) {
    static uint8_t arena_buf[8192];
    static uint8_t work[4096];
    bp_arena a;
    bp_value *parsed = NULL;
    int rc;
    SCEN_BEGIN(3, "bplist malformed inputs");
    CHECK(g_rt_bplist_len > 40u);
    /* Bad magic. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[0] = 'X';
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_MAGIC);
    /* Short buffers. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(g_rt_bplist, 20, &a, &parsed);
    CHECK(rc == XW_ERR_TRUNCATED);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(g_rt_bplist, 39, &a, &parsed);
    CHECK(rc == XW_ERR_TRUNCATED);
    /* Bad offsetIntSize in the trailer. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[g_rt_bplist_len - 32u + 6u] = 3;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Bad objectRefSize in the trailer. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[g_rt_bplist_len - 32u + 7u] = 3;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Top object out of range. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    put_u64be(work + g_rt_bplist_len - 16u, 999999u);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Offset table past the end. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    put_u64be(work + g_rt_bplist_len - 8u, (uint64_t)g_rt_bplist_len);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Offset table entry pointing outside the object region. The
     * table offset is the low 32 bits of the trailer's last u64. */
    {
        uint32_t table_off = 0;
        uint32_t i;
        for (i = 0; i < 4; i++) {
            table_off = (table_off << 8) |
                (uint32_t)g_rt_bplist[g_rt_bplist_len - 4u + i];
        }
        memcpy(work, g_rt_bplist, g_rt_bplist_len);
        work[table_off + 0] = 0xff;
        work[table_off + 1] = 0xff;
        work[table_off + 2] = 0xff;
        work[table_off + 3] = 0xff;
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
        CHECK(rc == XW_ERR_BAD_TYPE);
    }
    /* Unknown root marker. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[8] = 0xf0;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* UID root marker. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[8] = 0x80;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Fill marker as root. */
    memcpy(work, g_rt_bplist, g_rt_bplist_len);
    work[8] = 0x0f;
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(work, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Hand-built minimal files. */
    {
        uint8_t raw[256];
        uint32_t raw_len;
        const uint8_t *objs[4];
        uint32_t lens[4];
        /* Int with an impossible size (0x14: 16 bytes). */
        static const uint8_t bad_int[] = { 0x14, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0, 0, 0 };
        objs[0] = bad_int;
        lens[0] = (uint32_t)sizeof(bad_int);
        raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
        CHECK(raw_len > 0);
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        rc = bp_parse(raw, raw_len, &a, &parsed);
        CHECK(rc == XW_ERR_BAD_TYPE);
        /* Real with an impossible size (0x21: 2 bytes). */
        {
            static const uint8_t bad_real[] = { 0x21, 0, 0 };
            objs[0] = bad_real;
            lens[0] = (uint32_t)sizeof(bad_real);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK(rc == XW_ERR_BAD_TYPE);
        }
        /* Date with the wrong size marker (0x30, not 0x33). */
        {
            static const uint8_t bad_date[] = { 0x30, 0, 0, 0, 0,
                                                0, 0, 0, 0 };
            objs[0] = bad_date;
            lens[0] = (uint32_t)sizeof(bad_date);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK(rc == XW_ERR_BAD_TYPE);
        }
        /* Dict whose key is an int, not a string. */
        {
            static const uint8_t dict_obj[] = { 0xd1, 0x01, 0x02 };
            static const uint8_t int_key[] = { 0x10, 0x05 };
            static const uint8_t str_val[] = { 0x53, 'a', 'b', 'c' };
            objs[0] = dict_obj;
            lens[0] = (uint32_t)sizeof(dict_obj);
            objs[1] = int_key;
            lens[1] = (uint32_t)sizeof(int_key);
            objs[2] = str_val;
            lens[2] = (uint32_t)sizeof(str_val);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 3);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK(rc == XW_ERR_BAD_TYPE);
        }
        /* Array ref out of range. */
        {
            static const uint8_t arr_obj[] = { 0xa1, 0x05 };
            objs[0] = arr_obj;
            lens[0] = (uint32_t)sizeof(arr_obj);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK(rc == XW_ERR_TRUNCATED);
        }
        /* 0xF size form with the payload missing. */
        {
            static const uint8_t data_obj[] = { 0x4f, 0x10, 0x05 };
            objs[0] = data_obj;
            lens[0] = (uint32_t)sizeof(data_obj);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK(rc == XW_ERR_TRUNCATED);
        }
        /* 0xF size form that works: 20-byte data. */
        {
            static const uint8_t data20[] = {
                0x4f, 0x10, 0x14,
                1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                11, 12, 13, 14, 15, 16, 17, 18, 19, 20
            };
            objs[0] = data20;
            lens[0] = (uint32_t)sizeof(data20);
            raw_len = build_raw_bplist(raw, sizeof(raw), objs, lens, 1);
            CHECK(raw_len > 0);
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            rc = bp_parse(raw, raw_len, &a, &parsed);
            CHECK_RC(rc);
            CHECK(parsed != NULL);
            if (parsed != NULL) {
                CHECK(parsed->type == (uint32_t)BP_DATA);
                CHECK(parsed->len == 20);
                CHECK(parsed->s != NULL && parsed->s[0] == 1 &&
                      parsed->s[19] == 20);
            }
        }
    }
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: arena exhaustion and deep nesting.                      */
/* ------------------------------------------------------------------ */

static void scen_4_bplist_limits(void) {
    static uint8_t arena_buf[8192];
    static uint8_t tiny_buf[64];
    static uint8_t deep_buf[1024];
    bp_arena a;
    bp_value *v;
    bp_value *parsed = NULL;
    uint32_t deep_len;
    int rc;
    SCEN_BEGIN(4, "bplist arena exhaustion + deep nesting");
    /* Tiny arena: a long string does not fit. */
    bp_arena_init(&a, tiny_buf, sizeof(tiny_buf));
    v = bp_new_string(&a, "this string is definitely longer than "
                          "sixty-four bytes in total, so it will not "
                          "fit in the tiny arena");
    CHECK(v == NULL);
    /* Tiny arena: parsing a real plist runs out of space. */
    bp_arena_init(&a, tiny_buf, sizeof(tiny_buf));
    rc = bp_parse(g_rt_bplist, g_rt_bplist_len, &a, &parsed);
    CHECK(rc == XW_ERR_NOSPACE);
    /* 10 nested arrays parse fine. */
    deep_len = build_deep_bplist(deep_buf, sizeof(deep_buf), 10);
    CHECK(deep_len > 0);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(deep_buf, deep_len, &a, &parsed);
    CHECK_RC(rc);
    CHECK(parsed != NULL);
    if (parsed != NULL) {
        uint32_t count = 0;
        CHECK(walk_tree(parsed, 0, &count) == 0);
        CHECK(count == 11u); /* 10 arrays + 1 int */
    }
    /* 40 nested arrays exceed BP_MAX_DEPTH. */
    deep_len = build_deep_bplist(deep_buf, sizeof(deep_buf), 40);
    CHECK(deep_len > 0);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    rc = bp_parse(deep_buf, deep_len, &a, &parsed);
    CHECK(rc == XW_ERR_OVERFLOW);
    /* bp_dict_set / bp_array_append reject the wrong container. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    v = bp_new(&a, BP_ARRAY);
    CHECK(v != NULL);
    rc = bp_dict_set(&a, v, "k", bp_new_int(&a, 1));
    CHECK(rc == XW_ERR_BAD_TYPE);
    v = bp_new(&a, BP_DICT);
    CHECK(v != NULL);
    rc = bp_array_append(&a, v, bp_new_int(&a, 1));
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(bp_dict_get(NULL, "k") == NULL);
    CHECK(bp_dict_get(v, NULL) == NULL);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: daemons_init() wiring.                                  */
/* ------------------------------------------------------------------ */

static mach_port_name_t g_ping_port = MACH_PORT_NULL;

static void scen_5_daemons_init(void) {
    int rc;
    SCEN_BEGIN(5, "daemons_init wiring");
    rc = daemons_init();
    CHECK(rc == KERN_SUCCESS);
    CHECK(g_stub_cfprefsd_calls == 1);
    CHECK(g_stub_notifyd_calls == 1);
    CHECK(g_stub_mg_calls == 1);
    CHECK(g_stub_lsd_calls == 1);
    /* Call order: cfprefsd, notifyd, mg, lsd. */
    CHECK(g_stub_cfprefsd_order == 1);
    CHECK(g_stub_notifyd_order == 2);
    CHECK(g_stub_mg_order == 3);
    CHECK(g_stub_lsd_order == 4);
    g_ping_port = svchost_lookup_port(SVCHOST_PING_SERVICE);
    CHECK(g_ping_port != MACH_PORT_NULL);
    CHECK(svchost_lookup_port("no.such.service") == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(NULL) == MACH_PORT_NULL);
    /* Second call is idempotent: stubs run again, ping keeps its port. */
    rc = daemons_init();
    CHECK(rc == KERN_SUCCESS);
    CHECK(g_stub_cfprefsd_calls == 2);
    CHECK(g_stub_notifyd_calls == 2);
    CHECK(g_stub_mg_calls == 2);
    CHECK(g_stub_lsd_calls == 2);
    CHECK(svchost_lookup_port(SVCHOST_PING_SERVICE) == g_ping_port);
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: ping round-trip through svchost_process_msg.            */
/* ------------------------------------------------------------------ */

static void scen_6_ping(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    int flag = 0;
    const char *s = NULL;
    uint32_t slen = 0;
    SCEN_BEGIN(6, "ping round-trip");
    CHECK(g_ping_port != MACH_PORT_NULL);
    /* {"ping": true, "payload": "hello"} -> error 0, pong, echo. */
    CHECK_RC(build_ping_body(body, sizeof(body), "hello", &body_len));
    ret = deliver(g_ping_port, SIM_PING_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(xw_dict_find(&rd, TEST_PONG_KEY, &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);
    CHECK_RC(xw_dict_find(&rd, TEST_PAYLOAD_KEY, &v));
    CHECK_RC(xw_as_string(&v, &s, &slen));
    CHECK(s != NULL && strcmp(s, "hello") == 0);
    /* Bootstrap-style shape: {"type": 7, "handle": 0, "message": "ping"}.
     * Note: ping handler (svchost.c) checks slen == 5 (length includes
     * the NUL), so the shape answers {"error": 0, "pong": true}. */
    {
        xw_builder b;
        int r = xw_builder_init(&b, body, sizeof(body));
        if (r == XW_OK) r = xw_dict_begin(&b);
        if (r == XW_OK) r = xw_dict_add_uint64(&b, "type", 7);
        if (r == XW_OK) r = xw_dict_add_uint64(&b, "handle", 0);
        if (r == XW_OK)
            r = xw_dict_add_string(&b, TEST_MESSAGE_KEY, "ping");
        if (r == XW_OK) r = xw_dict_end(&b);
        CHECK_RC(r);
        body_len = xw_builder_len(&b);
    }
    ret = deliver(g_ping_port, SIM_PING_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0); /* fixed handler: bootstrap shape now answers 0 */
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(xw_dict_find(&rd, TEST_PONG_KEY, &v));
    CHECK_RC(xw_as_bool(&v, &flag));
    CHECK(flag == 1);
    /* Unknown message -> {"error": 1}, never silence. */
    CHECK_RC(build_unknown_body(body, sizeof(body), &body_len));
    ret = deliver(g_ping_port, SIM_PING_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 1);
    /* Unknown destination port -> {"error": 124}. */
    CHECK_RC(build_ping_body(body, sizeof(body), NULL, &body_len));
    ret = deliver((mach_port_name_t)0x9999, SIM_PING_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 124);
    /* Truncated XPC body -> {"error": 22}. */
    CHECK_RC(build_ping_body(body, sizeof(body), NULL, &body_len));
    {
        uint8_t msg[4096];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_ping_port,
                                       SIM_PING_PORT, body, body_len);
        CHECK(size > 24 + 10);
        ret = svchost_process_msg((const mach_msg_header_t *)msg, 24 + 10,
                                  reply, sizeof(reply), &reply_size);
        CHECK(ret == 1);
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err == 22);
    }
    /* One-way message (no reply port): ret 1, reply size 0. */
    CHECK_RC(build_ping_body(body, sizeof(body), NULL, &body_len));
    ret = deliver(g_ping_port, MACH_PORT_NULL, body, body_len,
                  reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size == 0);
    /* Degenerate inputs return 0 without touching the reply. */
    reply_size = 0xdead;
    ret = svchost_process_msg(NULL, 100, reply, sizeof(reply),
                              &reply_size);
    CHECK(ret == 0);
    CHECK(reply_size == 0xdead);
    ret = svchost_process_msg((const mach_msg_header_t *)body, 10, reply,
                              sizeof(reply), &reply_size);
    CHECK(ret == 0);
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: fuzz svchost_process_msg.                               */
/* ------------------------------------------------------------------ */

static void scen_7_fuzz_ipc(void) {
    uint8_t valid[2048];
    uint8_t scratch[4096];
    uint8_t reply[2048];
    uint32_t valid_size = 0;
    uint32_t body_len = 0;
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(7, "fuzz svchost_process_msg (10000 iterations)");
    CHECK_RC(build_ping_body(valid + 24, sizeof(valid) - 24, "fuzz",
                             &body_len));
    valid_size = build_mach_msg(valid, sizeof(valid), g_ping_port,
                                SIM_PING_PORT, valid + 24, body_len);
    CHECK(valid_size > 24);
    for (i = 0; i < 10000; i++) {
        uint32_t send_size = valid_size;
        uint32_t reply_size = 0;
        int ret = 0;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        memcpy(scratch, valid, valid_size);
        /* 1 to 8 random byte mutations. */
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % valid_size;
            scratch[off] = (uint8_t)rng_next();
        }
        /* Sometimes truncate the message. */
        if ((rng_next() % 8u) == 0) {
            send_size = rng_next() % (valid_size + 1);
        }
        /* Sometimes drop the reply port (one-way message). */
        if ((rng_next() % 16u) == 0) {
            put_u32le(scratch + 12, 0);
        }
        ret = svchost_process_msg((const mach_msg_header_t *)scratch,
                                  send_size, reply, sizeof(reply),
                                  &reply_size);
        if (ret != 0 && ret != 1) {
            if (violations < 5) {
                printf("\n  fuzz iter %d: bad return %d\n", i, ret);
            }
            violations++;
            continue;
        }
        if (ret == 1 && reply_size > 0) {
            if (reply_size < (uint32_t)sizeof(mach_msg_header_t) + 40) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: short reply %u\n",
                           i, reply_size);
                }
                violations++;
                continue;
            }
            {
                xw_envelope env;
                xw_reader rd;
                if (parse_reply(reply, reply_size, &env, &rd) != XW_OK) {
                    if (violations < 5) {
                        printf("\n  fuzz iter %d: reply does not parse\n",
                               i);
                    }
                    violations++;
                }
            }
        }
    }
    printf("  fuzz: 10000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: fuzz bp_parse.                                          */
/* ------------------------------------------------------------------ */

static void scen_8_fuzz_bplist(void) {
    static uint8_t arena_buf[8192];
    uint8_t scratch[4096];
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(8, "fuzz bp_parse (5000 iterations)");
    CHECK(g_rt_bplist_len > 40u);
    CHECK(g_rt_bplist_len <= sizeof(scratch));
    for (i = 0; i < 5000; i++) {
        uint32_t len = g_rt_bplist_len;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        bp_arena a;
        bp_value *parsed = NULL;
        int rc;
        memcpy(scratch, g_rt_bplist, g_rt_bplist_len);
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % g_rt_bplist_len;
            scratch[off] = (uint8_t)rng_next();
        }
        if ((rng_next() % 8u) == 0) {
            len = rng_next() % (g_rt_bplist_len + 1);
        }
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        rc = bp_parse(scratch, len, &a, &parsed);
        if (rc > 0) {
            if (violations < 5) {
                printf("\n  fuzz iter %d: positive return %d\n", i, rc);
            }
            violations++;
            continue;
        }
        if (rc == XW_OK) {
            uint32_t count = 0;
            if (parsed == NULL ||
                walk_tree(parsed, 0, &count) != 0) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: parsed tree does not "
                           "walk\n", i);
                }
                violations++;
            }
        }
    }
    printf("  fuzz: 5000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("daemons_test: host test\n");
    scen_1_bplist_roundtrip();
    scen_2_bplist_scalars();
    scen_3_bplist_malformed();
    scen_4_bplist_limits();
    scen_5_daemons_init();
    scen_6_ping();
    scen_7_fuzz_ipc();
    scen_8_fuzz_bplist();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
