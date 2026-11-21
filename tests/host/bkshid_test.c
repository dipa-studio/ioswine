/* bkshid_test.c: host unit test for the BackBoard HID fake
 * (bkshid.c: in-process com.apple.backboard.hid-services.xpc).
 *
 * What it covers:
 *   1. Connect handshake: success reply, CID echoed, CIDr true,
 *      "error" 0 (fakeboard connect shape); state connected.
 *   2. Unknown selector: benign success reply (CID echoed, CIDr
 *      true, NO "error" key), never a bare {"error": ...} dict.
 *   3. Transaction-begin-like selector sets txn_open; a
 *      transaction-end-like selector clears it; a record-like
 *      selector counts and still replies success when the flag is
 *      clear (the reply must never depend on the diagnostic flag).
 *   4. Teardown kind ("invalidate") clears connected and txn_open.
 *   5. A second connect re-marks the connection and
 *      counts again without disturbing state.
 *   6. Malformed input: array root gets SVCHOST_ERR_BAD_MESSAGE;
 *      empty dict gets an empty-dict success reply.
 *   7. Connect without a CID still replies success (plain dict +
 *      "error" 0).
 *   8. Env gate: IOSWINE_BKSHID=0 disables registration;
 *      bkshid_init registers the service through the real
 *      svchost_register (bootstrap_register2 emulated by the
 *      ipc_msg stub below, after sim_test.c), and
 *      svchost_lookup_port finds it.
 *   9. Never-drop-a-reply at the svchost layer: a raw Mach message
 *      carrying a connect dict and one carrying an unknown selector
 *      both produce a reply (msgh_id 0x20000000, right dict);
 *      a one-way message (no reply port) produces none.
 *  10. Fuzz the direct handler (1000 mutated/truncated inputs):
 *      no crash, return code always 0 or SVCHOST_ERR_*.
 *
 * The selector spellings in scenario 3 are heuristic inputs,
 * not facts: the real spellings come from the first device trace.
 * The test pins the fallback behaviour, not the
 * spellings: any selector must get a benign success reply.
 *
 * Build (from the tree root; host-shim linux.h first on -I path):
 *   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DBKSHID_HOST_TEST=1 \
 *      -I tests/host/host-shim -I libsystem_kernel/src \
 *      -o /tmp/bkshid_test tests/host/bkshid_test.c \
 *      libsystem_kernel/src/bkshid.c libsystem_kernel/src/bsxpc.c \
 *      libsystem_kernel/src/xpcwire.c libsystem_kernel/src/svchost.c
 *   /tmp/bkshid_test
 * Plain cc without sanitizers works the same way.
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

/* ------------------------------------------------------------------ */
/* Stubs for the ipc_* symbols bkshid.c / svchost.c reference.        */
/* ipc_msg emulates ONLY the bootstrap_register2 path, after          */
/* sim_test.c: it records the registered name and answers            */
/* {"error": 0} in place with msgh_id 0x20000000.                     */
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
/* Request builders and reply readers.                               */
/* ------------------------------------------------------------------ */

/* Build a BSXPC request dict. kind/sel may be NULL (key omitted).
 * ctx_kind: 0 = no bsxpc_context, 1 = empty context, 2 = context
 * with message.s = the instance name. */
static int build_request(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                         const char *kind, const char *sel, uint64_t cid,
                         int has_cid, int ctx_kind) {
    xw_builder b;
    int r = xw_builder_init(&b, buf, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK && kind)
        r = xw_dict_add_string(&b, "bsxpc", kind);
    if (r == XW_OK && sel)
        r = xw_dict_add_string(&b, "bsxpc_SEL", sel);
    if (r == XW_OK && has_cid)
        r = xw_dict_add_uint64(&b, "bsxpc_CID", cid);
    if (r == XW_OK && has_cid)
        r = xw_dict_add_bool(&b, "bsxpc_CIDr", 0);
    if (r == XW_OK && ctx_kind == 1) {
        r = xw_dict_add_dict_begin(&b, "bsxpc_context");
        if (r == XW_OK) r = xw_dict_end(&b);
    }
    if (r == XW_OK && ctx_kind == 2) {
        r = xw_dict_add_dict_begin(&b, "bsxpc_context");
        if (r == XW_OK) r = xw_dict_add_dict_begin(&b, "message");
        if (r == XW_OK)
            r = xw_dict_add_string(&b, "s",
                                   "com.apple.backboard.hid.services");
        if (r == XW_OK) r = xw_dict_end(&b);
        if (r == XW_OK) r = xw_dict_end(&b);
    }
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Drive the handler directly on a built dict. Returns the handler
 * return code and the reply length. */
static int drive_handler(const uint8_t *req, uint32_t req_len,
                         uint8_t *reply_buf, uint32_t reply_cap,
                         uint32_t *reply_len) {
    xw_reader rd;
    xw_builder b;
    int hr;
    memset(&rd, 0, sizeof(rd));
    if (xw_reader_init(&rd, req, req_len, NULL, 0) != XW_OK)
        return -999;
    if (xw_builder_init(&b, reply_buf, reply_cap) != XW_OK)
        return -998;
    hr = bkshid_svchost_handler(&rd, &b, NULL);
    *reply_len = xw_builder_len(&b);
    return hr;
}

/* Read CID / CIDr out of a reply dict. Returns XW_OK or negative. */
static int reply_cid(const uint8_t *reply, uint32_t reply_len,
                     uint64_t *cid_out, int *cidr_out) {
    xw_reader rd;
    xw_value v;
    int rc;
    memset(&rd, 0, sizeof(rd));
    rc = xw_reader_init(&rd, reply, reply_len, NULL, 0);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, "bsxpc_CID", &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_uint64(&v, cid_out);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, "bsxpc_CIDr", &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, cidr_out);
}

/* True when the reply dict has an "error" key. */
static int reply_has_error(const uint8_t *reply, uint32_t reply_len) {
    xw_reader rd;
    xw_value v;
    if (xw_reader_init(&rd, reply, reply_len, NULL, 0) != XW_OK)
        return 0;
    return xw_dict_find(&rd, "error", &v) == XW_OK;
}

/* Read the "error" int64 out of a reply dict. */
static int reply_error(const uint8_t *reply, uint32_t reply_len,
                       int64_t *err_out) {
    xw_reader rd;
    xw_value v;
    int rc;
    memset(&rd, 0, sizeof(rd));
    rc = xw_reader_init(&rd, reply, reply_len, NULL, 0);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, "error", &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

/* Parse a full Mach reply message into a reader. Returns XW_OK or
 * negative; also checks the reply id is 0x20000000. The envelope
 * and reader must outlive the reads. */
static int parse_reply_msg(const uint8_t *reply_buf, uint32_t reply_size,
                           xw_envelope *env, xw_reader *rd) {
    int rc;
    if (reply_size < sizeof(mach_msg_header_t)) return XW_ERR_TRUNCATED;
    rc = xw_envelope_parse(reply_buf, reply_size, env);
    if (rc != XW_OK) return rc;
    if (env->id != 0x20000000u) return XW_ERR_BAD_TYPE;
    memset(rd, 0, sizeof(*rd));
    return xw_reader_init(rd, env->payload, env->payload_len,
                          env->descs, env->ndescs);
}

/* Read CID / CIDr / "error" out of a parsed reply reader. */
static int reply_rd_cid(xw_reader *rd, uint64_t *cid_out, int *cidr_out) {
    xw_value v;
    int rc = xw_dict_find(rd, "bsxpc_CID", &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_uint64(&v, cid_out);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(rd, "bsxpc_CIDr", &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, cidr_out);
}

static int reply_rd_error(xw_reader *rd, int64_t *err_out) {
    xw_value v;
    int rc = xw_dict_find(rd, "error", &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

static int reply_rd_has_error(xw_reader *rd) {
    xw_value v;
    return xw_dict_find(rd, "error", &v) == XW_OK;
}

/* ------------------------------------------------------------------ */
/* Scenarios.                                                          */
/* ------------------------------------------------------------------ */

static void scenario_connect(void) {
    uint8_t req[512], rep[512];
    uint32_t req_len = 0, rep_len = 0;
    uint64_t cid = 0;
    int cidr = 0;
    int64_t err = -1;
    bkshid_state_t st;
    printf("Scenario 1: connect handshake\n");
    bkshid_test_reset();
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "connect", NULL, 1, 1, 2));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    CHECK(rep_len > 8);
    CHECK_RC(reply_cid(rep, rep_len, &cid, &cidr));
    CHECK(cid == 1);
    CHECK(cidr == 1);
    CHECK(reply_has_error(rep, rep_len));
    CHECK_RC(reply_error(rep, rep_len, &err));
    CHECK(err == 0);
    bkshid_get_state(&st);
    CHECK(st.connected == 1);
    CHECK(st.connects == 1);
    CHECK(st.messages == 1);
    CHECK(st.txn_open == 0);
    CHECK(st.record_saves == 0);
}

static void scenario_unknown_selector(void) {
    uint8_t req[512], rep[512];
    uint32_t req_len = 0, rep_len = 0;
    uint64_t cid = 0;
    int cidr = 0;
    bkshid_state_t st;
    printf("Scenario 2: unknown selector gets a benign success reply\n");
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "someFutureSelector:withArg:", 7, 1, 1));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    CHECK_RC(reply_cid(rep, rep_len, &cid, &cidr));
    CHECK(cid == 7);
    CHECK(cidr == 1);
    CHECK(!reply_has_error(rep, rep_len));
    bkshid_get_state(&st);
    CHECK(st.connected == 1);
    CHECK(st.messages == 2);
    CHECK(strcmp(st.last_sel, "someFutureSelector:withArg:") == 0);
}

static void scenario_txn_and_record(void) {
    uint8_t req[512], rep[512];
    uint32_t req_len = 0, rep_len = 0;
    uint64_t cid = 0;
    int cidr = 0;
    bkshid_state_t st;
    printf("Scenario 3: txn begin / record / txn end (heuristic flags)\n");
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "beginTransaction:", 10, 1, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    CHECK_RC(reply_cid(rep, rep_len, &cid, &cidr));
    CHECK(cid == 10 && cidr == 1);
    CHECK(!reply_has_error(rep, rep_len));
    bkshid_get_state(&st);
    CHECK(st.txn_open == 1);
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "setLocalRecord:forEnvironment:", 11, 1, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    bkshid_get_state(&st);
    CHECK(st.record_saves == 1);
    CHECK(st.txn_open == 1);
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "endTransaction:", 12, 1, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    bkshid_get_state(&st);
    CHECK(st.txn_open == 0);
    /* Record save with the flag clear still replies success: the
     * reply must never depend on the diagnostic flag. */
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "setLocalRecord:forEnvironment:", 13, 1, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    CHECK_RC(reply_cid(rep, rep_len, &cid, &cidr));
    CHECK(cid == 13 && cidr == 1);
    CHECK(!reply_has_error(rep, rep_len));
    bkshid_get_state(&st);
    CHECK(st.record_saves == 2);
    CHECK(st.txn_open == 0);
}

static void scenario_teardown_and_reconnect(void) {
    uint8_t req[512], rep[512];
    uint32_t req_len = 0, rep_len = 0;
    bkshid_state_t st;
    printf("Scenario 4: teardown clears state, second connect (O5)\n");
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "invalidate", NULL, 9, 1, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    bkshid_get_state(&st);
    CHECK(st.connected == 0);
    CHECK(st.txn_open == 0);
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "connect", NULL, 2, 1, 1));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    bkshid_get_state(&st);
    CHECK(st.connected == 1);
    CHECK(st.connects == 2);
}

static void scenario_malformed(void) {
    uint8_t req[512], rep[512];
    uint32_t req_len = 0, rep_len = 0;
    xw_builder b;
    xw_reader rd;
    printf("Scenario 5: malformed and edge-case inputs\n");
    /* Array root: parse fails, handler returns BAD_MESSAGE (22). */
    CHECK_RC(xw_builder_init(&b, req, sizeof(req)));
    CHECK_RC(xw_array_begin(&b));
    CHECK_RC(xw_add_string(&b, "x"));
    CHECK_RC(xw_array_end(&b));
    req_len = xw_builder_len(&b);
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) ==
          SVCHOST_ERR_BAD_MESSAGE);
    /* Empty dict: parses, gets an empty-dict success reply. */
    CHECK_RC(xw_builder_init(&b, req, sizeof(req)));
    CHECK_RC(xw_dict_begin(&b));
    CHECK_RC(xw_dict_end(&b));
    req_len = xw_builder_len(&b);
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    memset(&rd, 0, sizeof(rd));
    CHECK_RC(xw_reader_init(&rd, rep, rep_len, NULL, 0));
    {
        xw_value root;
        CHECK_RC(xw_reader_root(&rd, &root));
        CHECK(root.type == XW_TYPE_DICTIONARY);
    }
    /* Connect without a CID: success, plain dict + "error" 0. */
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "connect", NULL, 0, 0, 0));
    CHECK(drive_handler(req, req_len, rep, sizeof(rep), &rep_len) == 0);
    {
        int64_t err = -1;
        CHECK_RC(reply_error(rep, rep_len, &err));
        CHECK(err == 0);
    }
    /* NULL reader / NULL builder: BAD_MESSAGE, never a crash. */
    {
        xw_builder b2;
        CHECK_RC(xw_builder_init(&b2, rep, sizeof(rep)));
        CHECK(bkshid_svchost_handler(NULL, &b2, NULL) ==
              SVCHOST_ERR_BAD_MESSAGE);
    }
}

static void scenario_register_and_svchost(void) {
    uint8_t req[512];
    uint32_t req_len = 0;
    mach_port_name_t svc_port;
    uint8_t msg[1024];
    uint8_t rep_buf[1024];
    uint32_t rep_size = 0;
    mach_msg_header_t *hdr;
    uint64_t cid = 0;
    int cidr = 0;
    int64_t err = -1;
    printf("Scenario 6: register via svchost, drive via svchost_process_msg\n");
    /* Disabled first: init succeeds but registers nothing. */
    setenv("IOSWINE_BKSHID", "0", 1);
    CHECK(bkshid_is_enabled() == 0);
    CHECK(bkshid_init() == KERN_SUCCESS);
    CHECK(svchost_lookup_port(BKSHID_SERVICE_NAME) == MACH_PORT_NULL);
    unsetenv("IOSWINE_BKSHID");
    CHECK(bkshid_is_enabled() == 1);
    /* Enabled: svchost_init first (registers ping through the same
     * stub), then bkshid_init registers the HID service. */
    CHECK(svchost_init() == KERN_SUCCESS);
    CHECK(bkshid_init() == KERN_SUCCESS);
    svc_port = svchost_lookup_port(BKSHID_SERVICE_NAME);
    CHECK(svc_port != MACH_PORT_NULL);
    /* Idempotent: a second init keeps the same port. */
    CHECK(bkshid_init() == KERN_SUCCESS);
    CHECK(svchost_lookup_port(BKSHID_SERVICE_NAME) == svc_port);
    /* Raw Mach message carrying a connect dict: svchost must
     * produce a reply (never-drop-a-reply at the svchost layer). */
    bkshid_test_reset();
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "connect", NULL, 1, 1, 1));
    memset(msg, 0, sizeof(msg));
    hdr = (mach_msg_header_t *)msg;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                    MACH_MSG_TYPE_MAKE_SEND_ONCE);
    hdr->msgh_size = (uint32_t)sizeof(mach_msg_header_t) + req_len;
    hdr->msgh_remote_port = svc_port;
    hdr->msgh_local_port = 0x3000;
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = 0x40000000u;
    memcpy(msg + sizeof(mach_msg_header_t), req, req_len);
    CHECK(svchost_process_msg(hdr, hdr->msgh_size, rep_buf,
                              sizeof(rep_buf), &rep_size) == 1);
    CHECK(rep_size > sizeof(mach_msg_header_t));
    {
        mach_msg_header_t *rh = (mach_msg_header_t *)rep_buf;
        xw_envelope env;
        xw_reader rd;
        CHECK(rh->msgh_id == 0x20000000u);
        CHECK(rh->msgh_remote_port == 0x3000);
        CHECK_RC(parse_reply_msg(rep_buf, rep_size, &env, &rd));
        CHECK_RC(reply_rd_cid(&rd, &cid, &cidr));
        CHECK(cid == 1 && cidr == 1);
        CHECK_RC(reply_rd_error(&rd, &err));
        CHECK(err == 0);
    }
    /* Unknown selector through the same layer: still a reply. */
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          NULL, "someFutureSelector:withArg:", 5, 1, 0));
    memset(msg, 0, sizeof(msg));
    hdr = (mach_msg_header_t *)msg;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                    MACH_MSG_TYPE_MAKE_SEND_ONCE);
    hdr->msgh_size = (uint32_t)sizeof(mach_msg_header_t) + req_len;
    hdr->msgh_remote_port = svc_port;
    hdr->msgh_local_port = 0x3000;
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = 0x40000000u;
    memcpy(msg + sizeof(mach_msg_header_t), req, req_len);
    rep_size = 0;
    CHECK(svchost_process_msg(hdr, hdr->msgh_size, rep_buf,
                              sizeof(rep_buf), &rep_size) == 1);
    CHECK(rep_size > sizeof(mach_msg_header_t));
    {
        xw_envelope env;
        xw_reader rd;
        CHECK_RC(parse_reply_msg(rep_buf, rep_size, &env, &rd));
        CHECK_RC(reply_rd_cid(&rd, &cid, &cidr));
        CHECK(cid == 5 && cidr == 1);
        CHECK(!reply_rd_has_error(&rd));
    }
    /* One-way message (no reply port): handled, no reply. */
    hdr->msgh_local_port = MACH_PORT_NULL;
    rep_size = 0;
    CHECK(svchost_process_msg(hdr, hdr->msgh_size, rep_buf,
                              sizeof(rep_buf), &rep_size) == 1);
    CHECK(rep_size == 0);
}

static void scenario_fuzz(void) {
    uint8_t req[512], rep[512], mut[512];
    uint32_t req_len = 0, rep_len = 0;
    uint32_t seed = 0x12345678u;
    int i;
    printf("Scenario 7: fuzz the handler (1000 iterations)\n");
    CHECK_RC(build_request(req, sizeof(req), &req_len,
                          "connect", "initializeClientWithParameters:completion:",
                          3, 1, 2));
    for (i = 0; i < 1000; i++) {
        uint32_t len = req_len;
        int hr;
        int nmut;
        memcpy(mut, req, req_len);
        seed = seed * 1664525u + 1013904223u;
        nmut = 1 + (int)((seed >> 16) % 4);
        for (int k = 0; k < nmut; k++) {
            uint32_t pos;
            seed = seed * 1664525u + 1013904223u;
            pos = (seed >> 8) % req_len;
            seed = seed * 1664525u + 1013904223u;
            mut[pos] = (uint8_t)(seed & 0xff);
        }
        if ((i % 3) == 0) {
            seed = seed * 1664525u + 1013904223u;
            len = (seed >> 8) % (req_len + 1);
        }
        rep_len = 0;
        hr = drive_handler(mut, len, rep, sizeof(rep), &rep_len);
        /* -999: the reader rejected the mutated bytes before the
         * handler ran (bad magic / truncated envelope); that is a
         * harness rejection, not a handler failure. */
        CHECK(hr == -999 || hr == 0 || hr == SVCHOST_ERR_BAD_MESSAGE ||
              hr == SVCHOST_ERR_NO_MEMORY ||
              hr == SVCHOST_ERR_UNKNOWN_MESSAGE);
        CHECK(rep_len <= sizeof(rep));
    }
    printf("  fuzz done: 1000 iterations\n");
}

int main(void) {
    /* Scenarios 1-5 drive the handler directly; scenario 6 needs
     * the service unregistered at entry, so it runs last before
     * the fuzz (the fuzz only touches the handler, not the
     * registry). State is reset where a scenario depends on it. */
    scenario_connect();
    scenario_unknown_selector();
    scenario_txn_and_record();
    scenario_teardown_and_reconnect();
    scenario_malformed();
    scenario_register_and_svchost();
    scenario_fuzz();
    printf("bkshid_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        printf("FAIL bkshid_test\n");
        return 1;
    }
    printf("PASS bkshid_test\n");
    return 0;
}
