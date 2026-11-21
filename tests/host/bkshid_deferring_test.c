/* bkshid_deferring_test.c: host test for the fake
 * BackBoard HID event delivery service (bkshid.c).
 *
 * Covers event-deferring flows and wire checks:
 *
 *   1. Registration negative: bkshid_init() publishes ONLY
 *      "com.apple.backboard.hid-services.xpc". The raw
 *      "com.apple.backboard.hid.services" Mach port is NOT
 *      registered by it (never touch the raw event port).
 *      Env gate IOSWINE_BKSHID=0 registers nothing; a second
 *      init is idempotent.
 *   2. The reply rule, end to end through
 *      svchost_process_msg: a one-way connect (msgh_id 0x10000000)
 *      WITH a reply port still gets a reply (refuting the earlier
 *      assumption that 0x10000000 never replies); with no reply port
 *      there is no reply.
 *   3. Event-deferring flow, simulated end to end with a mock ObjC
 *      layer (snapshot_test mock pattern): manager init ->
 *      connection established by the connect reply (observed
 *      through bkshid_get_state, not just the mock) -> transaction
 *      available -> _setLocalRecord succeeds with no assertion.
 *      The same flow with no live connection fires exactly the
 *      device assertion ("Invalid state. Saving records without a
 *      transaction") and saves nothing. A record-like selector sent
 *      through the fake is counted server-side (record_saves) while
 *      the mock save succeeds.
 *   4. Malformed input at the svchost / Mach layer: NULL header,
 *      short send size, truncated XPC body, garbage bytes, and
 *      reply buffers too small for the minimal error dict never
 *      crash and never write past the reply buffer.
 *   5. Fuzz through the full Mach / svchost path (10000 mutated
 *      connect messages plus 1000 random-id messages):
 *      return is always 0 or 1, out_reply_size never exceeds the
 *      reply cap.
 *
 * Protocol details:
 *   service "com.apple.backboard.hid-services.xpc";
 *   raw port "com.apple.backboard.hid.services" is NOT served;
 *   BSXPC keys "bsxpc", "bsxpc_context", "bsxpc_CID", "bsxpc_CIDr",
 *   "bsxpc_SEL"; message ids 0x10000000 / 0x40000000 / 0x20000000;
 *   w00t 0x77303074 [device 2026-11-12];
 *   reply iff a reply port is present;
 *   unknown selector gets a benign success-shaped reply (CID
 *   echoed, CIDr true, NO "error" key), never a bare
 *   {"error": ...} dict;
 *   deferring assertion string.
 *
 * The selector spellings used for the record / transaction traffic
 * ("setLocalRecord:forEnvironment:", "beginTransaction:",
 * "endTransaction:") are heuristic inputs, not facts
 * (the real spellings come from the first device trace).
 * This file pins the behaviour around them, not the
 * spellings: any selector gets a benign success reply, and the
 * mock save succeeds exactly when the mock transaction exists.
 *
 * Build (from the tree root; host-shim linux.h first on -I path):
 *   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DBKSHID_HOST_TEST=1 \
 *      -I tests/host/host-shim -I libsystem_kernel/src \
 *      -o /tmp/bkshid_deferring_test \
 *      tests/host/bkshid_deferring_test.c \
 *      libsystem_kernel/src/bkshid.c libsystem_kernel/src/bsxpc.c \
 *      libsystem_kernel/src/xpcwire.c libsystem_kernel/src/svchost.c
 *   /tmp/bkshid_deferring_test
 * Plain cc without sanitizers works the same way. Or simply:
 * tools/host_tests.sh (wires bkshid_test and this file).
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "xpcwire.h"
#include "bsxpc.h"
#include "svchost.h"
#include "launchd.h"
#include "bkshid.h"

/* ------------------------------------------------------------------ */
/* Contract literals (see the file header for sources).                */
/* ------------------------------------------------------------------ */

#define DEF_XPC_SERVICE "com.apple.backboard.hid-services.xpc"
#define DEF_MACH_SERVICE "com.apple.backboard.hid.services"

#define DEF_KEY_BSXPC "bsxpc"
#define DEF_KEY_CONTEXT "bsxpc_context"
#define DEF_KEY_CID "bsxpc_CID"
#define DEF_KEY_CIDR "bsxpc_CIDr"
#define DEF_KEY_SEL "bsxpc_SEL"
#define DEF_KEY_ERROR "error"
#define DEF_MSG_CONNECT "connect"
#define DEF_KEY_BUNDLE_ID "bundleID"
#define DEF_KEY_PID "pid"
#define DEF_KEY_ENDPOINT "endpoint"

/* Heuristic selector spellings. */
#define DEF_SEL_RECORD "setLocalRecord:forEnvironment:"
#define DEF_SEL_TXN_BEGIN "beginTransaction:"
#define DEF_SEL_TXN_END "endTransaction:"
#define DEF_SEL_UNKNOWN "someFutureSelector:withArg:"

#define DEF_SIM_BUNDLE "com.example.Aidoku"
#define DEF_SIM_PID 4242u
#define DEF_CLIENT_PORT ((mach_port_name_t)0x4000)
#define DEF_ENDPOINT_PORT ((mach_port_name_t)0x4002)
#define DEF_REPLY_PORT ((mach_port_name_t)0x3000)

#define DEF_MSGH_ID_W00T 0x77303074u
#define DEF_MSGH_ID_XPC_NO_REPLY 0x10000000u
#define DEF_MSGH_ID_XPC_WANTS_REPLY 0x40000000u
#define DEF_MSGH_ID_REPLY 0x20000000u

#define DEF_DEFERRING_ASSERT_REASON \
    "Invalid state. Saving records without a transaction"

/* ------------------------------------------------------------------ */
/* Harness (sim_test / bkshid_test pattern).                           */
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

/* ------------------------------------------------------------------ */
/* ipc stubs (no real ipc.c on host). After sim_test.c / bkshid_test.c:
 * ipc_msg emulates ONLY the bootstrap_register2 path.               */
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

static char g_reg_names[16][128];
static mach_port_name_t g_reg_ports[16];
static int g_reg_count = 0;

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
    for (i = 0; i < g_reg_count; i++) {
        if (strcmp(g_reg_names[i], name) == 0) {
            g_reg_ports[i] = (mach_port_name_t)env.descs[0].obj;
            break;
        }
    }
    if (i == g_reg_count && g_reg_count < 16) {
        strncpy(g_reg_names[g_reg_count], name, 127);
        g_reg_names[g_reg_count][127] = '\0';
        g_reg_ports[g_reg_count] =
            (mach_port_name_t)env.descs[0].obj;
        g_reg_count++;
    }
    if (xw_builder_init(&b, body, sizeof(body)) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_begin(&b) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_add_int64(&b, "error", 0) != XW_OK)
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
/* BSXPC vector builders and Mach delivery (sim_test pattern).         */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

typedef int (*def_ctx_fn_t)(xw_builder *b);

/* Client identity context: bundle id, pid, endpoint port as a
 * mach_send (sim_test's initialize context shape). */
static int def_ctx_connect(xw_builder *b) {
    int r = xw_dict_add_string(b, DEF_KEY_BUNDLE_ID, DEF_SIM_BUNDLE);
    if (r == XW_OK) r = xw_dict_add_uint64(b, DEF_KEY_PID, DEF_SIM_PID);
    if (r == XW_OK)
        r = xw_dict_add_mach_send(b, DEF_KEY_ENDPOINT,
                                  (uint64_t)DEF_CLIENT_PORT,
                                  MACH_MSG_TYPE_COPY_SEND);
    return r;
}

/* Build a BSXPC envelope dict. kind/sel may be NULL (key omitted). */
static int build_bsxpc_msg(uint8_t *body, uint32_t cap,
                           const char *kind, const char *sel,
                           uint64_t cid, int cidr,
                           def_ctx_fn_t ctx_fn, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK && kind)
        r = xw_dict_add_string(&b, DEF_KEY_BSXPC, kind);
    if (r == XW_OK)
        r = xw_dict_add_dict_begin(&b, DEF_KEY_CONTEXT);
    if (r == XW_OK && ctx_fn != NULL) r = ctx_fn(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, DEF_KEY_CID, cid);
    if (r == XW_OK)
        r = xw_dict_add_bool(&b, DEF_KEY_CIDR, cidr);
    if (r == XW_OK && sel)
        r = xw_dict_add_string(&b, DEF_KEY_SEL, sel);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* The connect handshake: kind "connect", CID 1, CIDr false, no
 * bsxpc_SEL key. No selector spelling is asserted here:
 * the init selector set is [open], and the protocol
 * fixes the connect KIND, not a selector. */
static int build_connect(uint8_t *body, uint32_t cap, uint32_t *out_len) {
    return build_bsxpc_msg(body, cap, DEF_MSG_CONNECT, NULL, 1, 0,
                           def_ctx_connect, out_len);
}

static void put_port_desc(uint8_t *p, uint32_t name, uint32_t disp) {
    put_u32le(p + 0, name);
    put_u32le(p + 4, 0);
    p[8] = 0;
    p[9] = 0;
    p[10] = (uint8_t)disp;
    p[11] = 0;
}

/* Build a Mach message: 24-byte header, optional one COPY_SEND port
 * descriptor, then the XPC body. Returns total size, or 0. */
static uint32_t build_mach_msg(uint8_t *buf, uint32_t cap,
                               mach_port_name_t dest,
                               mach_port_name_t reply_port,
                               uint32_t msgh_id,
                               const uint8_t *body, uint32_t body_len,
                               int with_desc, mach_port_name_t desc_name) {
    uint32_t total = (uint32_t)sizeof(mach_msg_header_t) + body_len;
    uint8_t *p = buf;
    if (with_desc) total += 4 + 12;
    if (cap < total) return 0;
    if (with_desc) {
        put_u32le(p + 0, 0x80001513u); /* COMPLEX | COPY_SEND | MAKE_SEND_ONCE */
    } else {
        put_u32le(p + 0, 0x1513u); /* COPY_SEND | MAKE_SEND_ONCE<<8 */
    }
    put_u32le(p + 4, total);
    put_u32le(p + 8, dest);
    put_u32le(p + 12, reply_port);
    put_u32le(p + 16, 0);
    put_u32le(p + 20, msgh_id);
    p += sizeof(mach_msg_header_t);
    if (with_desc) {
        put_u32le(p + 0, 1); /* descriptor count */
        put_port_desc(p + 4, desc_name, MACH_MSG_TYPE_COPY_SEND);
        p += 4 + 12;
    }
    memcpy(p, body, body_len);
    return total;
}

typedef int (*def_entry_fn_t)(const mach_msg_header_t *, uint32_t,
                              uint8_t *, uint32_t, uint32_t *);

static int deliver(def_entry_fn_t entry, mach_port_name_t dest,
                   mach_port_name_t reply_port, uint32_t msgh_id,
                   const uint8_t *body, uint32_t body_len,
                   int with_desc, mach_port_name_t desc_name,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    uint8_t msg[4096];
    uint32_t size = build_mach_msg(msg, sizeof(msg), dest, reply_port,
                                   msgh_id, body, body_len,
                                   with_desc, desc_name);
    if (size == 0) return -1;
    return entry((const mach_msg_header_t *)msg, size,
                 reply_buf, reply_cap, out_reply_size);
}

/* ------------------------------------------------------------------ */
/* Reply parsing.                                                      */
/* ------------------------------------------------------------------ */

static int parse_msg(const uint8_t *buf, uint32_t size,
                     xw_envelope *env, xw_reader *r) {
    int rc;
    if (size < sizeof(mach_msg_header_t)) return XW_ERR_TRUNCATED;
    rc = xw_envelope_parse(buf, size, env);
    if (rc != XW_OK) return rc;
    memset(r, 0, sizeof(*r));
    return xw_reader_init(r, env->payload, env->payload_len,
                          env->descs, env->ndescs);
}

static int parse_reply(const uint8_t *reply_buf, uint32_t reply_size,
                       xw_envelope *env, xw_reader *r) {
    int rc = parse_msg(reply_buf, reply_size, env, r);
    if (rc != XW_OK) return rc;
    if (env->id != DEF_MSGH_ID_REPLY) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

static int reply_error_code(const uint8_t *reply_buf, uint32_t reply_size,
                            int64_t *err_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v, ctx;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, DEF_KEY_CONTEXT, &ctx);
    if (rc == XW_OK && ctx.type == XW_TYPE_DICTIONARY) {
        rc = xw_dict_find_v(&ctx, DEF_KEY_ERROR, &v);
        if (rc == XW_OK) return xw_as_int64(&v, err_out);
    }
    rc = xw_dict_find(&rd, DEF_KEY_ERROR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

static int reply_cid(const uint8_t *reply_buf, uint32_t reply_size,
                     uint64_t *cid_out, int *cidr_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, DEF_KEY_CID, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_uint64(&v, cid_out);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, DEF_KEY_CIDR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, cidr_out);
}

/* True when the reply dict has an "error" key at the top level. */
static int reply_has_error(const uint8_t *reply_buf, uint32_t reply_size) {
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    if (parse_reply(reply_buf, reply_size, &env, &rd) != XW_OK)
        return 0;
    return xw_dict_find(&rd, DEF_KEY_ERROR, &v) == XW_OK;
}

/* ------------------------------------------------------------------ */
/* Mock ObjC event-deferring layer (snapshot_test mock pattern).       */
/*                                                                     */
/* Models the device chain at the ObjC level, without any real ObjC:   */
/*   BKSHIDEventDeliveryManager.sharedInstance -> manager object;      */
/*   the manager holds a remote (BSXPC) connection; the connection is  */
/*   established exactly when the fake service answers the connect    */
/*   message (observed through bkshid_get_state, not just the mock);  */
/*   a transaction exists exactly when the connection is established  */
/*   and begun; _UIEventDeferringManager._setLocalRecord: succeeds    */
/*   exactly when a transaction exists, and otherwise calls           */
/*   NSAssertionHandler with the exact device reason string.           */
/*                                                                     */
/* The old snapshot.c hacks short-circuited this chain twice: the BKS */
/* nil hook made sharedInstance return nil (so UIKit never asked for  */
/* a transaction), and IOSWINE_OBJC_NIL silenced the assertion when   */
/* it fired anyway. This simulation proves the fake service removes   */
/* the need for both: with a live connection the record saves and no  */
/* assertion fires; without one the assertion fires, which is the    */
/* behaviour the fake must prevent on device.                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int connection_established; /* set after a connect reply */
    void *transaction;          /* non-NULL token once begun */
} mock_bks_manager_t;

typedef struct {
    void *transaction; /* transaction handed over by the manager */
    int assertion_fired;
    char assertion_reason[128];
    int records_saved;
} mock_deferring_mgr_t;

/* The transaction token is opaque; any non-NULL value stands in for
 * the real transaction object. */
#define MOCK_TRANSACTION_TOKEN ((void *)(uintptr_t)0x5452414eu) /* "TRAN" */

static void *mock_bks_begin_transaction(mock_bks_manager_t *mgr) {
    if (!mgr || !mgr->connection_established) return NULL;
    if (!mgr->transaction) mgr->transaction = MOCK_TRANSACTION_TOKEN;
    return mgr->transaction;
}

static int mock_deferring_set_local_record(mock_deferring_mgr_t *dmgr,
                                           const char *record) {
    (void)record;
    if (!dmgr) return 0;
    if (!dmgr->transaction) {
        dmgr->assertion_fired++;
        strncpy(dmgr->assertion_reason, DEF_DEFERRING_ASSERT_REASON,
                sizeof(dmgr->assertion_reason) - 1);
        dmgr->assertion_reason[sizeof(dmgr->assertion_reason) - 1] = '\0';
        return 0;
    }
    dmgr->records_saved++;
    return 1;
}

static mach_port_name_t g_xpc_port = MACH_PORT_NULL;

/* Send one BSXPC message through svchost_process_msg and return its
 * return code. The caller checks the reply. */
static int send_via_svchost(const uint8_t *body, uint32_t body_len,
                            uint32_t msgh_id, mach_port_name_t reply_port,
                            int with_desc,
                            uint8_t *reply_buf, uint32_t reply_cap,
                            uint32_t *out_reply_size) {
    return deliver(svchost_process_msg, g_xpc_port, reply_port, msgh_id,
                   body, body_len, with_desc, DEF_CLIENT_PORT,
                   reply_buf, reply_cap, out_reply_size);
}

/* ------------------------------------------------------------------ */
/* Scenario 1: registration, including the negative for the raw port.  */
/* ------------------------------------------------------------------ */

static void test_registration(void) {
    SCEN_BEGIN(1, "registration (only the .xpc name is served)");
    /* Disabled first: init succeeds but registers nothing. */
    setenv("IOSWINE_BKSHID", "0", 1);
    CHECK(bkshid_is_enabled() == 0);
    CHECK(bkshid_init() == KERN_SUCCESS);
    CHECK(svchost_lookup_port(DEF_XPC_SERVICE) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(DEF_MACH_SERVICE) == MACH_PORT_NULL);
    unsetenv("IOSWINE_BKSHID");
    CHECK(bkshid_is_enabled() == 1);

    /* The literals here are the contract names from the file header,
     * not bkshid.h's macro, so this independently verifies the
     * header spells the service name the same. */
    CHECK(strcmp(DEF_XPC_SERVICE, BKSHID_SERVICE_NAME) == 0);
    CHECK(svchost_init() == KERN_SUCCESS);
    CHECK(bkshid_init() == KERN_SUCCESS);
    g_xpc_port = svchost_lookup_port(DEF_XPC_SERVICE);
    CHECK(g_xpc_port != MACH_PORT_NULL);
    /* The raw HID event port is intentionally NOT served by this
     * fake. If another component fakes it, it gets its own service
     * and its own test; this fake must not claim the name. */
    CHECK(svchost_lookup_port(DEF_MACH_SERVICE) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port("com.apple.backboard.no-such-service") ==
          MACH_PORT_NULL);
    /* Second init is idempotent: same port, still resolvable. */
    CHECK(bkshid_init() == KERN_SUCCESS);
    CHECK(svchost_lookup_port(DEF_XPC_SERVICE) == g_xpc_port);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: reply rule through svchost.                             */
/* ------------------------------------------------------------------ */

static void test_reply_rule(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    uint64_t cid = 0;
    int cidr = -1;
    const mach_msg_header_t *rh;
    SCEN_BEGIN(2, "reply rule (reply iff a reply port exists)");
    CHECK(g_xpc_port != MACH_PORT_NULL);
    CHECK_RC(build_connect(body, sizeof(body), &body_len));

    /* Wants-reply connect: full success shape. */
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 1,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    rh = (const mach_msg_header_t *)reply;
    CHECK(rh->msgh_id == DEF_MSGH_ID_REPLY);
    CHECK(rh->msgh_remote_port == DEF_REPLY_PORT);
    CHECK(rh->msgh_local_port == MACH_PORT_NULL);
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 1);
    CHECK(cidr == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);

    /* A one-way connect (0x10000000) WITH a reply port still gets a reply.
     * The earlier rule (no reply for 0x10000000) was refuted on device
     * and must not come back. */
    reply_size = 0;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_NO_REPLY, DEF_REPLY_PORT, 1,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 1);
    CHECK(cidr == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);

    /* No reply port at all: handled, no reply built. */
    reply_size = 999;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_NO_REPLY, MACH_PORT_NULL, 1,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size == 0);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: event-deferring flow, simulated end to end.             */
/* ------------------------------------------------------------------ */

static void test_deferring_flow(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int64_t err = -1;
    int ret = 0;
    uint64_t cid = 0;
    int cidr = -1;
    mock_bks_manager_t mgr;
    mock_deferring_mgr_t dmgr;
    void *txn;
    bkshid_state_t st;
    SCEN_BEGIN(3, "event-deferring flow (mock ObjC)");
    memset(&mgr, 0, sizeof(mgr));
    memset(&dmgr, 0, sizeof(dmgr));
    bkshid_test_reset();

    /* No live connection: no transaction, and _setLocalRecord fires
     * exactly the device assertion and saves nothing. This is the
     * failure the old IOSWINE_OBJC_NIL hack silenced. */
    txn = mock_bks_begin_transaction(&mgr);
    CHECK(txn == NULL);
    dmgr.transaction = txn;
    CHECK(mock_deferring_set_local_record(&dmgr, "key-window-record") == 0);
    CHECK(dmgr.assertion_fired == 1);
    CHECK(strcmp(dmgr.assertion_reason,
                 DEF_DEFERRING_ASSERT_REASON) == 0);
    CHECK(dmgr.records_saved == 0);

    /* The manager's connection is established by the fake service
     * answering the connect message. The server side observes it
     * too: bkshid_get_state reports connected. */
    CHECK_RC(build_connect(body, sizeof(body), &body_len));
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 1,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    bkshid_get_state(&st);
    CHECK(st.connected == 1);
    CHECK(st.connects == 1);
    CHECK(st.messages == 1);
    mgr.connection_established = 1;

    /* With a live connection the transaction exists, the record
     * saves, and no assertion fires: neither the BKS nil hook nor
     * the NSAssertionHandler nil-ing is needed any more. */
    memset(&dmgr, 0, sizeof(dmgr));
    txn = mock_bks_begin_transaction(&mgr);
    CHECK(txn != NULL);
    CHECK(txn == MOCK_TRANSACTION_TOKEN);
    dmgr.transaction = txn;
    CHECK(mock_deferring_set_local_record(&dmgr, "key-window-record") == 1);
    CHECK(dmgr.assertion_fired == 0);
    CHECK(dmgr.records_saved == 1);

    /* Record traffic through the fake is counted server-side while
     * the mock save succeeds: the client's save and the server's
     * count agree that a record went through. The selector spelling
     * is a heuristic input, not a fact; what is pinned is
     * that the reply is success-shaped and the save is counted. */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_RECORD,
                             11, 0, NULL, &body_len));
    reply_size = 0;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 11);
    CHECK(cidr == 1);
    CHECK(!reply_has_error(reply, reply_size));
    bkshid_get_state(&st);
    CHECK(st.record_saves == 1);
    CHECK(mock_deferring_set_local_record(&dmgr, "second-record") == 1);
    CHECK(dmgr.records_saved == 2);
    CHECK(dmgr.assertion_fired == 0);

    /* Transaction begin/end flags track the traffic (internal
     * diagnostic flags, asserted here only as the setup for the
     * save above, not as a wire contract). */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_TXN_BEGIN,
                             12, 0, NULL, &body_len));
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    bkshid_get_state(&st);
    CHECK(st.txn_open == 1);
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_TXN_END,
                             13, 0, NULL, &body_len));
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    bkshid_get_state(&st);
    CHECK(st.txn_open == 0);
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4 (new): a valid transaction, end to end.        */
/* The exact device failure removed here is                            */
/* -[_UIEventDeferringManager _setLocalRecord:forEnvironment:] firing */
/* "Invalid state. Saving records without a transaction" during      */
/* key-window evaluation. This scenario drives the whole chain       */
/* through the REAL bkshid handler (not just the mock): connect,     */
/* then a transaction-begin-like selector, then the mock             */
/* _setLocalRecord with the transaction the live connection makes    */
/* available, then a record selector through the handler, then a     */
/* transaction-end-like selector. Every mock save must succeed with  */
/* zero assertions, and the server must count the record save. The   */
/* selector spellings are heuristic inputs, not facts; what is      */
/* pinned is the sequence and the success shape, not the spellings.  */
/* ------------------------------------------------------------------ */

static void test_setlocalrecord_valid_transaction(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int64_t err = -1;
    int ret = 0;
    uint64_t cid = 0;
    int cidr = -1;
    mock_bks_manager_t mgr;
    mock_deferring_mgr_t dmgr;
    void *txn;
    bkshid_state_t st;
    SCEN_BEGIN(4, "valid transaction: _setLocalRecord must not throw");
    memset(&mgr, 0, sizeof(mgr));
    memset(&dmgr, 0, sizeof(dmgr));
    bkshid_test_reset();

    /* Step 1: connect through the real handler. The manager's
     * connection is established only when the fake answers. */
    CHECK_RC(build_connect(body, sizeof(body), &body_len));
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 1,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    bkshid_get_state(&st);
    CHECK(st.connected == 1);
    CHECK(st.connects == 1);
    mgr.connection_established = 1;

    /* Step 2: a transaction-begin-like selector through the real
     * handler. Server-side txn flag opens; reply is success-shaped
     * with no error key. */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_TXN_BEGIN,
                             20, 0, NULL, &body_len));
    reply_size = 0;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 20);
    CHECK(cidr == 1);
    CHECK(!reply_has_error(reply, reply_size));
    bkshid_get_state(&st);
    CHECK(st.txn_open == 1);

    /* Step 3: with a live connection the transaction exists, so
     * _setLocalRecord saves and the device assertion never fires. */
    txn = mock_bks_begin_transaction(&mgr);
    CHECK(txn != NULL);
    CHECK(txn == MOCK_TRANSACTION_TOKEN);
    dmgr.transaction = txn;
    CHECK(mock_deferring_set_local_record(&dmgr, "key-window-record") == 1);
    CHECK(dmgr.assertion_fired == 0);
    CHECK(dmgr.records_saved == 1);

    /* Step 4: the record itself goes through the fake as well: the
     * server counts it and answers success, and a second mock save
     * still succeeds with no assertion. */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_RECORD,
                             21, 0, NULL, &body_len));
    reply_size = 0;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 21);
    CHECK(cidr == 1);
    CHECK(!reply_has_error(reply, reply_size));
    bkshid_get_state(&st);
    CHECK(st.record_saves == 1);
    CHECK(mock_deferring_set_local_record(&dmgr, "second-record") == 1);
    CHECK(dmgr.records_saved == 2);
    CHECK(dmgr.assertion_fired == 0);

    /* Step 5: transaction end closes the server-side flag. A later
     * save still succeeds: the flag is diagnostic only, and the
     * fake never refuses a record save (that refusal would
     * re-create the failure). */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), NULL, DEF_SEL_TXN_END,
                             22, 0, NULL, &body_len));
    reply_size = 0;
    ret = send_via_svchost(body, body_len,
                           DEF_MSGH_ID_XPC_WANTS_REPLY, DEF_REPLY_PORT, 0,
                           reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    bkshid_get_state(&st);
    CHECK(st.txn_open == 0);
    CHECK(mock_deferring_set_local_record(&dmgr, "after-end-record") == 1);
    CHECK(dmgr.records_saved == 3);
    CHECK(dmgr.assertion_fired == 0);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: malformed input at the svchost / Mach layer.            */
/* ------------------------------------------------------------------ */

static void test_malformed(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint8_t garbage[256];
    uint32_t body_len = 0, reply_size = 999;
    int ret = 0;
    int64_t err = 0;
    size_t i;
    SCEN_BEGIN(5, "malformed input at the svchost layer");
    CHECK(g_xpc_port != MACH_PORT_NULL);
    CHECK_RC(build_connect(body, sizeof(body), &body_len));

    /* NULL header, short send size, NULL reply buffer: reject. */
    CHECK(svchost_process_msg(NULL, 24, reply, sizeof(reply),
                              &reply_size) == 0);
    {
        uint8_t msg[4096];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_xpc_port,
                                       DEF_REPLY_PORT,
                                       DEF_MSGH_ID_XPC_WANTS_REPLY,
                                       body, body_len, 0, 0);
        CHECK(size > 0);
        CHECK(svchost_process_msg((const mach_msg_header_t *)msg, 8,
                                  reply, sizeof(reply), &reply_size) == 0);
        CHECK(svchost_process_msg((const mach_msg_header_t *)msg, size,
                                  NULL, sizeof(reply), &reply_size) == 0);
        CHECK(svchost_process_msg((const mach_msg_header_t *)msg, size,
                                  reply, sizeof(reply), NULL) == 0);
        /* Reply buffer too small for even the minimal error dict. */
        CHECK(svchost_process_msg((const mach_msg_header_t *)msg, size,
                                  reply, 63, &reply_size) == 0);
    }

    /* Truncated XPC body: svchost answers {"error": 22}
     * (SVCHOST_ERR_BAD_MESSAGE), never a crash and never a dropped
     * send-once. */
    {
        uint8_t msg[4096];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_xpc_port,
                                       DEF_REPLY_PORT,
                                       DEF_MSGH_ID_XPC_WANTS_REPLY,
                                       body, body_len / 2, 0, 0);
        reply_size = 0;
        ret = svchost_process_msg((const mach_msg_header_t *)msg, size,
                                  reply, sizeof(reply), &reply_size);
        CHECK(ret == 1);
        CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    }

    /* Garbage bytes as the whole message body. */
    for (i = 0; i < sizeof(garbage); i++) garbage[i] = (uint8_t)rng_next();
    reply_size = 0;
    ret = deliver(svchost_process_msg, g_xpc_port, DEF_REPLY_PORT,
                  DEF_MSGH_ID_XPC_WANTS_REPLY, garbage, sizeof(garbage),
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 0 || ret == 1);
    CHECK(reply_size <= sizeof(reply));
    if (ret == 1 && reply_size > (uint32_t)sizeof(mach_msg_header_t)) {
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    }

    /* Empty body (header only): same treatment. */
    reply_size = 0;
    {
        static const uint8_t empty_body[1] = { 0 };
        ret = deliver(svchost_process_msg, g_xpc_port, DEF_REPLY_PORT,
                      DEF_MSGH_ID_XPC_WANTS_REPLY, empty_body, 0,
                      0, 0, reply, sizeof(reply), &reply_size);
    }
    CHECK(ret == 0 || ret == 1);
    CHECK(reply_size <= sizeof(reply));

    /* An unregistered destination port gets the launchd-style 124
     * error dict, not a crash and not a hang. */
    reply_size = 0;
    ret = deliver(svchost_process_msg, (mach_port_name_t)0x7777,
                  DEF_REPLY_PORT, DEF_MSGH_ID_XPC_WANTS_REPLY,
                  body, body_len, 0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 124);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: fuzz through the full Mach / svchost path.              */
/* ------------------------------------------------------------------ */

static void test_fuzz(void) {
    uint8_t seed_body[1024];
    uint8_t seed_msg[4096];
    uint8_t msg[4096];
    uint8_t reply[2048];
    uint32_t seed_body_len = 0, seed_size = 0, reply_size = 0;
    uint32_t i;
    int ret;
    SCEN_BEGIN(6, "fuzz via svchost (10000 mutated + 1000 random-id)");
    CHECK_RC(build_connect(seed_body, sizeof(seed_body), &seed_body_len));
    seed_size = build_mach_msg(seed_msg, sizeof(seed_msg), g_xpc_port,
                               DEF_REPLY_PORT,
                               DEF_MSGH_ID_XPC_WANTS_REPLY,
                               seed_body, seed_body_len, 1, DEF_CLIENT_PORT);
    CHECK(seed_size > 0);

    for (i = 0; i < 10000; i++) {
        uint32_t nflips = 1 + (rng_next() % 8);
        uint32_t j;
        uint32_t send_size = seed_size;
        memcpy(msg, seed_msg, seed_size);
        /* Flip bytes only where the service is the trust boundary:
         * the 24-byte header and the XPC body (offsets >= 40). The
         * descriptor table (offsets 24..39) is left intact: on the
         * real path ipc.c receives the Mach message and an OOL
         * descriptor's address is kernel-filled, so a sender cannot
         * forge it (xw_envelope_parse documents this trust); forging
         * a port descriptor into an OOL descriptor here would hand
         * the reader a bogus payload pointer, which is ipc.c's
         * boundary to police, not the service's. */
        for (j = 0; j < nflips; j++) {
            uint32_t off;
            if ((rng_next() % 3) == 0) {
                off = rng_next() % 24;
            } else {
                off = 40 + (rng_next() % (seed_size - 40));
            }
            msg[off] ^= (uint8_t)(1u << (rng_next() % 8));
        }
        /* Sometimes truncate the message. */
        if ((rng_next() % 4) == 0) {
            send_size = rng_next() % (seed_size + 1);
        }
        reply_size = 0;
        memset(reply, 0, sizeof(reply));
        ret = svchost_process_msg((const mach_msg_header_t *)msg, send_size,
                                  reply, sizeof(reply), &reply_size);
        CHECK(ret == 0 || ret == 1);
        CHECK(reply_size <= sizeof(reply));
    }

    for (i = 0; i < 1000; i++) {
        uint32_t id = rng_next();
        memcpy(msg, seed_msg, seed_size);
        /* Random msgh_id, random reply port (including 0). */
        memcpy(msg + 20, &id, 4);
        {
            uint32_t rp = rng_next() % 0x9000;
            memcpy(msg + 12, &rp, 4);
        }
        reply_size = 0;
        ret = svchost_process_msg((const mach_msg_header_t *)msg, seed_size,
                                  reply, sizeof(reply), &reply_size);
        CHECK(ret == 0 || ret == 1);
        CHECK(reply_size <= sizeof(reply));
    }
    SCEN_END(5);
}

int main(void) {
    test_registration();
    test_reply_rule();
    test_deferring_flow();
    test_setlocalrecord_valid_transaction();
    test_malformed();
    test_fuzz();
    printf("bkshid_deferring_test: %d checks, %d failures\n",
           g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS: bkshid_deferring_test all scenarios passed\n");
        return 0;
    }
    printf("FAIL: bkshid_deferring_test\n");
    return 1;
}
