/* sim_test.c: host simulator test for the fake FrontBoard / RunningBoard
 * services on the REAL BSXPC wire.
 *
 * A fake "client" built with xpcwire replays the exact device messages
 * measured on 2026-11-12 against fakeboard + fakerb
 * in-process and checks every reply and callout. Nine scenarios; each
 * prints a PASS line, and a TOTAL line at the end. The process returns
 * nonzero on any failed check.
 *
 * The device vectors (golden, measured on device 2026-11-12):
 *   (a) w00t (msgh_id 0x77303074, MOVE_RECEIVE of the client's
 *       connection port + MOVE_SEND of its endpoint, no XPC payload,
 *       consumed by ipc.c, no reply) then "connect" to
 *       com.apple.frontboard.systemappservices: bsxpc "connect",
 *       bsxpc_SEL "initializeClientWithParameters:completion:",
 *       bsxpc_CID uint64 1, bsxpc_CIDr false, bsxpc_context {...}.
 *   (b) the same initialize message with msgh_id 0x40000000 (expects
 *       a reply): the reply echoes bsxpc_CID 1 with bsxpc_CIDr true.
 *   (c) RBS handshakeWithRequest: (RBSHandshakeRequest,
 *       _assertionDescriptors, _savedEndowments) to RunningBoard.
 *   (d) scene-create callout via fakeboard_next_callout
 *       (server-initiated): real keys, create selector, scene
 *       identity "sceneID:<bundle>-default", settings 1440x3200
 *       @ scale 4.
 *
 * The "connect" reply rule (resolving a crash where
 * libxpc crashes in its async-reply hook after fakeboard's reply to
 * "connect" arrives): reply ONLY when the incoming msgh_id is
 * 0x40000000 (expects a reply); NO reply for 0x10000000 (one-way
 * XPC message). Both cases are asserted explicitly. The test sends
 * the same connect+initialize vector twice on purpose (once one-way,
 * once with-reply) to pin the rule; a conforming fake answers both.
 *
 * How the fakes are driven on host: there is no real ipc.c here, so
 * this file stubs the ipc_* symbols the .c files reference (found
 * with `nm -u` on the compiled objects: g_ipc_trace, lsk_getenv,
 * ipc_task_self, ipc_mach_reply_port, ipc_svchost_port_create,
 * ipc_port_from_name, ipc_port_deallocate, ipc_port_destroy,
 * ipc_msg). ipc_msg emulates ONLY the bootstrap_register2 path
 * (msgh_id 0x100000cc) that svchost_register's self-registration
 * uses: it records name -> port in a small static table and builds
 * a reply {"error": 0} with msgh_id 0x20000000 in the same buffer.
 * Any other msgh_id returns MACH_SEND_INVALID_DEST. fakeboard_init
 * and fakerb_init are then called for real, service ports are found
 * with svchost_lookup_port, and fakeboard_process_msg /
 * fakerb_process_msg are driven directly with hand-built Mach
 * messages (header + XPC body from xw_builder). Every reply is
 * parsed with xw_reader and the fields are checked. The w00t is
 * NOT delivered to a fake (ipc.c consumes it); the simulator builds
 * it only to assert its shape, and starts driving the fake from the
 * "connect" message that follows it.
 *
 * API used (bsxpc.h): bsxpc_envelope_keys (key_bsxpc, key_context, key_cid,
 * key_cidr, key_sel), bsxpc_message / bsxpc_message_parse (tolerant:
 * a key that is absent clears its has_* flag and leaves the field
 * at its zero value; a key with the wrong type, or a root that is
 * not a dictionary, returns XW_ERR_BAD_TYPE), bsxpc_reply_build
 * (opens one dict, writes bsxpc_CID + bsxpc_CIDr true, leaves it
 * OPEN for the caller to add result fields, then one xw_dict_end
 * closes it). Replies are therefore FLAT: {bsxpc_CID, bsxpc_CIDr:
 * true, error, ...} with the result fields at the top level, not
 * nested under bsxpc_context; the test asserts exactly that. The
 * parsed message's context dict is in msg.ctx (with has_ctx), not
 * msg.context. The old envelope keys ("message", "arguments",
 * "reply") and bsxpc_message_begin / begin_arguments / end are gone
 * and are not used anywhere in this file.
 *
 * Build (run from tests/host).
 * The target linux.h is aarch64-only, so the build needs a host
 * shim. The shim lives in the tree at tests/host/host-shim/linux.h
 * (build scaffolding, not a deliverable). A plain -Ihost-shim does
 * NOT shadow it for .c files compiled from the tree directory
 * (quote-include finds the .c file's own directory first), so the
 * build stages the sources into a private dir and overwrites
 * linux.h there, the way the tree's tools/host_tests.sh does:
 *
 *   B=/tmp/fbw-build/src; mkdir -p $B
 *   Copy the tree's libsystem_kernel headers and sources into $B,
 *   then overwrite the aarch64 linux.h with the host shim:
 *   cp tests/host/host-shim/linux.h $B/linux.h
 *   cp ../../src/bsxpc.h ../../src/bsxpc.c ../../src/fakeboard.h \
 *      ../../src/fakeboard.c ../../src/fakerb.h ../../src/fakerb.c \
 *      ../../src/fakeboard_names.h $B/
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -iquote $B \
 *        -o /tmp/sim_test sim_test.c \
 *        $B/bsxpc.c $B/fakeboard.c $B/fakerb.c $B/xpcwire.c $B/svchost.c
 *     /tmp/sim_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -iquote $B -o /tmp/sim_test_asan sim_test.c \
 *        $B/bsxpc.c $B/fakeboard.c $B/fakerb.c $B/xpcwire.c $B/svchost.c
 *     /tmp/sim_test_asan
 *
 * No em dashes are used in this file.
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
#include "svchost.h"
#include "fakeboard.h"
#include "fakerb.h"
#include "fakeboard_names.h"
#include "launchd.h"

/* ------------------------------------------------------------------ */
/* Test constants (fake client identity).                              */
/* ------------------------------------------------------------------ */

#define SIM_PID 4242u
#define SIM_BUNDLE "com.example.Aidoku"
#define SIM_CLIENT_PORT ((mach_port_name_t)0x4000)
#define SIM_ENDPOINT_PORT ((mach_port_name_t)0x4002)
#define SIM_FB_REPLY_PORT ((mach_port_name_t)0x3000)
#define SIM_RB_REPLY_PORT ((mach_port_name_t)0x3001)

/* The w00t message id (0x77303074, ASCII "w00t"). */
#define SIM_MSGH_ID_W00T 0x77303074u
/* XPC message ids on connection ports (measured on device 2026-11-12). */
#define SIM_MSGH_ID_XPC_NO_REPLY 0x10000000u
#define SIM_MSGH_ID_XPC_WANTS_REPLY 0x40000000u
#define SIM_MSGH_ID_REPLY 0x20000000u

/* RBS handshake argument keys. These are the argument names from the
 * protocol trace (RBSHandshakeRequest, _assertionDescriptors,
 * _savedEndowments); they are client-side vector data, not entries
 * in the fakeboard names table, so they live here as literals. */
#define SIM_RBS_KEY_REQUEST "RBSHandshakeRequest"
#define SIM_RBS_KEY_ASSERTIONS "_assertionDescriptors"
#define SIM_RBS_KEY_ENDOWMENTS "_savedEndowments"

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

static uint32_t g_sim_next_port = 0x1000;

mach_port_name_t ipc_svchost_port_create(void) {
    return g_sim_next_port++;
}

mach_port_name_t ipc_mach_reply_port(void) {
    return g_sim_next_port++;
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
#define SIM_MAX_REG 16
static char g_sim_reg_names[SIM_MAX_REG][128];
static mach_port_name_t g_sim_reg_ports[SIM_MAX_REG];
static int g_sim_reg_count = 0;

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
    for (i = 0; i < g_sim_reg_count; i++) {
        if (strcmp(g_sim_reg_names[i], name) == 0) {
            g_sim_reg_ports[i] = (mach_port_name_t)env.descs[0].obj;
            break;
        }
    }
    if (i == g_sim_reg_count && g_sim_reg_count < SIM_MAX_REG) {
        strncpy(g_sim_reg_names[g_sim_reg_count], name, 127);
        g_sim_reg_names[g_sim_reg_count][127] = '\0';
        g_sim_reg_ports[g_sim_reg_count] =
            (mach_port_name_t)env.descs[0].obj;
        g_sim_reg_count++;
    }
    /* Reply {"error": 0} in place, msgh_id 0x20000000. */
    if (xw_builder_init(&b, body, sizeof(body)) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_begin(&b) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_add_int64(&b, FB_ENV_ERROR, 0) != XW_OK)
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

/* The real envelope keys, from the fakeboard names table. */
static const bsxpc_envelope_keys g_env_keys = {
    .key_bsxpc = FB_BSXPC_KEY_BSXPC,
    .key_context = FB_BSXPC_KEY_CONTEXT,
    .key_cid = FB_BSXPC_KEY_CID,
    .key_cidr = FB_BSXPC_KEY_CIDR,
    .key_sel = FB_BSXPC_KEY_SEL,
};

/* ------------------------------------------------------------------ */
/* Real BSXPC vector builders (the fake client's messages).            */
/* ------------------------------------------------------------------ */

/* Context filler for the generic envelope builder. NULL means an
 * empty context dict. */
typedef int (*sim_ctx_fn_t)(xw_builder *b);

/* initializeClientWithParameters context: the client identity the
 * workspace service needs to know who is connecting, plus the
 * client's endpoint port (a mach_send under "endpoint") so the fake
 * learns a PERSISTENT endpoint for server-initiated callouts (rule
 * 1 in fakeboard.h; a reply-port endpoint is one-shot and cannot
 * feed several callouts). The identity keys are [guess] (the device
 * trace shows bsxpc_context is non-empty but not its contents); the
 * endpoint port is a simulator stand-in for the connection port
 * the w00t delivers to ipc.c, which the host simulator cannot
 * reproduce (there is no ipc.c here). The caller must deliver this
 * vector WITH a Mach port descriptor for the endpoint (COPY_SEND),
 * or the reader rejects the mach_send value. */
static int sim_ctx_initialize(xw_builder *b) {
    int r = xw_dict_add_string(b, FB_ENV_BUNDLE_ID, SIM_BUNDLE);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_ENV_PID, SIM_PID);
    if (r == XW_OK)
        r = xw_dict_add_mach_send(b, FB_KEY_ENDPOINT,
                                  (uint64_t)SIM_CLIENT_PORT,
                                  MACH_MSG_TYPE_COPY_SEND);
    return r;
}

/* RBS handshake context: RBSHandshakeRequest (the process identity),
 * _assertionDescriptors (empty: no assertions held yet) and
 * _savedEndowments (empty: endowments are saved later). The
 * argument names are from the protocol trace; the request dict shape
 * (pid + bundle id) is [guess]. */
static int sim_ctx_handshake(xw_builder *b) {
    int r = xw_dict_add_dict_begin(b, SIM_RBS_KEY_REQUEST);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_ENV_PID, SIM_PID);
    if (r == XW_OK)
        r = xw_dict_add_string(b, FB_ENV_BUNDLE_ID, SIM_BUNDLE);
    if (r == XW_OK) r = xw_dict_end(b);
    if (r == XW_OK)
        r = xw_dict_add_array_begin(b, SIM_RBS_KEY_ASSERTIONS);
    if (r == XW_OK) r = xw_array_end(b);
    if (r == XW_OK)
        r = xw_dict_add_array_begin(b, SIM_RBS_KEY_ENDOWMENTS);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

/* Build a real BSXPC envelope dict:
 *   { "bsxpc": kind, "bsxpc_context": {...}, "bsxpc_CID": cid,
 *     "bsxpc_CIDr": cidr, "bsxpc_SEL": sel }
 * The key order follows the protocol trace listing. XPC dicts are
 * unordered, so the order is cosmetic. */
static int build_bsxpc_msg(uint8_t *body, uint32_t cap,
                           const char *kind, const char *sel,
                           uint64_t cid, int cidr,
                           sim_ctx_fn_t ctx_fn, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, FB_BSXPC_KEY_BSXPC, kind);
    if (r == XW_OK)
        r = xw_dict_add_dict_begin(&b, FB_BSXPC_KEY_CONTEXT);
    if (r == XW_OK && ctx_fn != NULL) r = ctx_fn(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, FB_BSXPC_KEY_CID, cid);
    if (r == XW_OK)
        r = xw_dict_add_bool(&b, FB_BSXPC_KEY_CIDR, cidr);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, FB_BSXPC_KEY_SEL, sel);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* The connect+initialize vector from the device trace: bsxpc
 * "connect", SEL initializeClientWithParameters:completion:,
 * CID 1, CIDr false, identity context. */
static int build_connect_initialize(uint8_t *body, uint32_t cap,
                                    uint32_t *out_len) {
    return build_bsxpc_msg(body, cap, FB_MSG_CONNECT,
                           FB_MSG_INITIALIZE_CLIENT, 1, 0,
                           sim_ctx_initialize, out_len);
}

/* The RBS handshake vector: bsxpc "connect" (first message on the
 * RBS connection), SEL handshakeWithRequest:, CID 1, CIDr false,
 * handshake context. The kind "connect" for the RBS first message
 * is [guess]: the dispatch is by SEL, and the first message on any
 * BSXPC connection establishes it. */
static int build_rbs_handshake(uint8_t *body, uint32_t cap,
                               uint32_t *out_len) {
    return build_bsxpc_msg(body, cap, FB_MSG_CONNECT,
                           FAKERB_MSG_HANDSHAKE, 1, 0,
                           sim_ctx_handshake, out_len);
}

/* ------------------------------------------------------------------ */
/* Mach message delivery.                                              */
/* ------------------------------------------------------------------ */

typedef int (*sim_entry_fn_t)(const mach_msg_header_t *, uint32_t,
                              uint8_t *, uint32_t, uint32_t *);

/* Write one 12-byte port descriptor at p: name, pad1 0, then
 * pad2 0, unused 0, disposition, type 0 (port). The disposition
 * byte is at offset 10 and the type byte at offset 11, matching
 * the layout xpcwire.h documents and the w00t check in ipc.c
 * reads ([24+4+10] for the first descriptor's disposition). */
static void put_port_desc(uint8_t *p, uint32_t name, uint32_t disp) {
    put_u32le(p + 0, name);
    put_u32le(p + 4, 0);
    p[8] = 0;
    p[9] = 0;
    p[10] = (uint8_t)disp;
    p[11] = 0;
}

/* Build a Mach message in buf: 24-byte header, optional one COPY_SEND
 * port descriptor, then the XPC body. msgh_id is a parameter (the
 * reply rule depends on it). Returns the total size, or 0 when the
 * buffer is too small. */
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

/* Build a w00t message in buf: 24-byte header, two port
 * descriptors, no XPC payload (ipc.c: "w00t carries no XPC payload
 * and gets no reply on XNU"). Descriptor 0 is MOVE_RECEIVE of the
 * client's connection port; descriptor 1 is MOVE_SEND of its
 * endpoint. Returns the total size (52), or 0 when too small. */
static uint32_t build_w00t(uint8_t *buf, uint32_t cap,
                           mach_port_name_t dest) {
    uint32_t total = (uint32_t)sizeof(mach_msg_header_t) + 4 + 24;
    uint8_t *p = buf;
    if (cap < total) return 0;
    put_u32le(p + 0, 0x80000013u); /* COMPLEX | COPY_SEND remote */
    put_u32le(p + 4, total);
    put_u32le(p + 8, dest);
    put_u32le(p + 12, 0); /* no reply port: w00t gets no reply */
    put_u32le(p + 16, 0);
    put_u32le(p + 20, SIM_MSGH_ID_W00T);
    p += sizeof(mach_msg_header_t);
    put_u32le(p + 0, 2); /* descriptor count */
    put_port_desc(p + 4, SIM_CLIENT_PORT, MACH_MSG_TYPE_MOVE_RECEIVE);
    put_port_desc(p + 16, SIM_ENDPOINT_PORT, MACH_MSG_TYPE_MOVE_SEND);
    return total;
}

/* Deliver a built message to an entry point. Returns the entry's
 * return value (0 or 1), or -1 when the message did not fit. */
static int deliver(sim_entry_fn_t entry, mach_port_name_t dest,
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
/* Reply parsing and nested getters.                                   */
/* ------------------------------------------------------------------ */

/* Parse a full Mach message (reply or callout) into a reader.
 * Returns XW_OK or a negative XW_ERR_*. */
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

/* Parse a full reply Mach message into a reader. Returns XW_OK or a
 * negative XW_ERR_*. Also checks the reply id is 0x20000000. */
static int parse_reply(const uint8_t *reply_buf, uint32_t reply_size,
                       xw_envelope *env, xw_reader *r) {
    int rc = parse_msg(reply_buf, reply_size, env, r);
    if (rc != XW_OK) return rc;
    if (env->id != 0x20000000u) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Read the "error" int64 out of a reply. Replies are flat
 * (bsxpc_reply_build puts the result fields, including "error", at
 * the top level); a flat error dict (the fake's answer when the
 * request failed to parse) also carries "error" at the top level.
 * For robustness this tries bsxpc_context first, then the top
 * level. Returns XW_OK on success. */
static int reply_error_code(const uint8_t *reply_buf, uint32_t reply_size,
                            int64_t *err_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v, ctx;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, FB_BSXPC_KEY_CONTEXT, &ctx);
    if (rc == XW_OK && ctx.type == XW_TYPE_DICTIONARY) {
        rc = xw_dict_find_v(&ctx, FB_ENV_ERROR, &v);
        if (rc == XW_OK) return xw_as_int64(&v, err_out);
    }
    rc = xw_dict_find(&rd, FB_ENV_ERROR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

/* Read bsxpc_CID / bsxpc_CIDr out of a reply (top level). Returns
 * XW_OK on success. */
static int reply_cid(const uint8_t *reply_buf, uint32_t reply_size,
                     uint64_t *cid_out, int *cidr_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, FB_BSXPC_KEY_CID, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_uint64(&v, cid_out);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, FB_BSXPC_KEY_CIDR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, cidr_out);
}

/* Nested getters on an xw_value dictionary. */
static int find_dbl(const xw_value *dict, const char *key, double *out) {
    xw_value v;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_double(&v, out);
}

static int find_flag(const xw_value *dict, const char *key, int *out) {
    xw_value v;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, out);
}

static int find_int(const xw_value *dict, const char *key, int64_t *out) {
    xw_value v;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, out);
}

static int find_uint(const xw_value *dict, const char *key, uint64_t *out) {
    xw_value v;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_uint64(&v, out);
}

static int find_str(const xw_value *dict, const char *key,
                    const char **out) {
    xw_value v;
    uint32_t len = 0;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_string(&v, out, &len);
}

static int find_dict(const xw_value *dict, const char *key,
                     xw_value *out) {
    int rc = xw_dict_find_v(dict, key, out);
    if (rc != XW_OK) return rc;
    if (out->type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Root-level string lookup on a reader (xw_as_string needs a
 * non-NULL length pointer, so this wraps it). */
static int find_str_r(xw_reader *r, const char *key, const char **out) {
    xw_value v;
    uint32_t len = 0;
    int rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_string(&v, out, &len);
}

static int find_flag_in_arr(const xw_value *arr, uint64_t id, int *val_out) {
    uint32_t n = 0, i;
    int rc = xw_array_len_v(arr, &n);
    if (rc != XW_OK) return rc;
    for (i = 0; i < n; i++) {
        xw_value item, k, v;
        uint64_t got_id = 0;
        if (xw_array_at_v(arr, i, &item) != XW_OK) continue;
        if (xw_array_at_v(&item, 0, &k) == XW_OK &&
            xw_as_uint64(&k, &got_id) == XW_OK && got_id == id &&
            xw_array_at_v(&item, 1, &v) == XW_OK) {
            return xw_as_bool(&v, val_out);
        }
    }
    return XW_ERR_NOT_FOUND;
}

static int find_obj_in_arr(const xw_value *arr, uint64_t id,
                           const char *want_cls, xw_value *sec_val_out) {
    uint32_t n = 0, i;
    int rc = xw_array_len_v(arr, &n);
    if (rc != XW_OK) return rc;
    for (i = 0; i < n; i++) {
        xw_value item, k, wrap, coder;
        const char *cls = NULL;
        uint64_t got_id = 0;
        int64_t ctype = 0;
        if (xw_array_at_v(arr, i, &item) != XW_OK) continue;
        if (xw_array_at_v(&item, 0, &k) == XW_OK &&
            xw_as_uint64(&k, &got_id) == XW_OK && got_id == id) {
            rc = xw_array_at_v(&item, 1, &wrap);
            if (rc != XW_OK) return rc;
            rc = xw_array_at_v(&item, 2, &coder);
            if (rc != XW_OK) return rc;
            rc = xw_as_int64(&coder, &ctype);
            if (rc != XW_OK || ctype != FB_BSX_CODER_TYPE_SECURE)
                return XW_ERR_BAD_TYPE;
            rc = find_str(&wrap, FB_BSX_KEY_SEC_CLASS, &cls);
            if (rc != XW_OK || cls == NULL || strcmp(cls, want_cls) != 0)
                return XW_ERR_BAD_TYPE;
            return xw_dict_find_v(&wrap, FB_BSX_KEY_SEC_VALUE, sec_val_out);
        }
    }
    return XW_ERR_NOT_FOUND;
}

/* Check an FBSSceneSettingsDiff dict for the 1440x3200 @ scale 4 shape. */
static void check_settings_diff(const xw_value *diff, int fg) {
    xw_value main_diff, main_changes, bskeys, flags, objects;
    xw_value other_diff, other_changes;
    xw_value frame_val, disp_val, disp_mode, px_arr, bnd_arr, v;
    const char *s = NULL;
    const uint8_t *ptr = NULL;
    uint32_t len = 0, desc = 0, n = 0;
    double d0 = 0, d1 = 0, d2 = 0, d3 = 0, rect[4];
    int flag = -1;
    int64_t iv = 0;
    uint64_t uv = 0;

    CHECK_RC(find_str(diff, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_SCENE_SETTINGS_DIFF) == 0);
    CHECK_RC(find_str(diff, FB_DIFF_SETTINGS_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_UI_SCENE_SETTINGS) == 0);

    CHECK_RC(find_dict(diff, FB_DIFF_MAIN, &main_diff));
    CHECK_RC(find_dict(&main_diff, FB_DIFF_CHANGES, &main_changes));
    CHECK_RC(find_str(&main_changes, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_KEYED_SETTINGS) == 0);

    CHECK_RC(xw_dict_find_v(&main_changes, FB_SETTINGS_BSKEYS, &bskeys));
    CHECK_RC(xw_array_len_v(&bskeys, &n));
    CHECK(n >= 5);

    CHECK_RC(xw_dict_find_v(&main_changes, FB_SETTINGS_FLAGS, &flags));
    CHECK_RC(find_flag_in_arr(&flags, FB_CORE_HASH_FOREGROUND, &flag));
    CHECK(flag == fg);

    CHECK_RC(xw_dict_find_v(&main_changes, FB_SETTINGS_OBJECTS, &objects));

    /* frame -> NSValue(CGRect) */
    CHECK_RC(find_obj_in_arr(&objects, FB_CORE_HASH_FRAME, FB_CLS_NSVALUE,
                             &frame_val));
    CHECK_RC(find_str(&frame_val, FB_NSVALUE_TYPE, &s));
    CHECK(s != NULL && strcmp(s, FB_OBJC_TYPE_CGRECT) == 0);
    CHECK_RC(xw_dict_find_v(&frame_val, FB_NSVALUE_DATA, &v));
    CHECK_RC(xw_as_data(&v, &ptr, &len, &desc));
    CHECK(ptr != NULL && len == sizeof(rect));
    memcpy(rect, ptr, sizeof(rect));
    CHECK(rect[0] == 0.0 && rect[1] == 0.0 &&
          rect[2] == 360.0 && rect[3] == 800.0);

    /* displayConfiguration -> FBSDisplayConfiguration */
    CHECK_RC(find_obj_in_arr(&objects, FB_CORE_HASH_DISPLAY_CONFIG,
                             FB_CLS_DISPLAY_CONFIG, &disp_val));
    CHECK_RC(find_str(&disp_val, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_DISPLAY_CONFIG) == 0);
    CHECK_RC(xw_dict_find_v(&disp_val, FB_DISP_PIXEL_SIZE, &px_arr));
    CHECK_RC(xw_array_len_v(&px_arr, &n));
    CHECK(n == 2);
    CHECK_RC(xw_array_at_v(&px_arr, 0, &v)); CHECK_RC(xw_as_double(&v, &d0));
    CHECK_RC(xw_array_at_v(&px_arr, 1, &v)); CHECK_RC(xw_as_double(&v, &d1));
    CHECK(d0 == 1440.0 && d1 == 3200.0);
    CHECK_RC(xw_dict_find_v(&disp_val, FB_DISP_BOUNDS, &bnd_arr));
    CHECK_RC(xw_array_len_v(&bnd_arr, &n));
    CHECK(n == 4);
    CHECK_RC(xw_array_at_v(&bnd_arr, 0, &v)); CHECK_RC(xw_as_double(&v, &d0));
    CHECK_RC(xw_array_at_v(&bnd_arr, 1, &v)); CHECK_RC(xw_as_double(&v, &d1));
    CHECK_RC(xw_array_at_v(&bnd_arr, 2, &v)); CHECK_RC(xw_as_double(&v, &d2));
    CHECK_RC(xw_array_at_v(&bnd_arr, 3, &v)); CHECK_RC(xw_as_double(&v, &d3));
    CHECK(d0 == 0.0 && d1 == 0.0 && d2 == 360.0 && d3 == 800.0);
    CHECK_RC(find_dict(&disp_val, FB_DISP_CURRENT_MODE, &disp_mode));
    CHECK_RC(find_uint(&disp_mode, FB_DISPMODE_PREF_SCALE, &uv));
    CHECK(uv == 4);

    /* level + interfaceOrientation */
    CHECK_RC(find_obj_in_arr(&objects, FB_CORE_HASH_LEVEL, FB_CLS_NSNUMBER, &v));
    CHECK_RC(xw_as_double(&v, &d0));
    CHECK(d0 == FB_SETTINGS_LEVEL);
    CHECK_RC(find_obj_in_arr(&objects, FB_CORE_HASH_INTERFACE_ORIENTATION,
                             FB_CLS_NSNUMBER, &v));
    CHECK_RC(xw_as_int64(&v, &iv));
    CHECK(iv == FB_ORIENTATION_PORTRAIT);

    /* otherDiff -> BSSettingsDiff -> BSSettings */
    CHECK_RC(find_dict(diff, FB_DIFF_OTHER, &other_diff));
    CHECK_RC(find_dict(&other_diff, FB_DIFF_CHANGES, &other_changes));
    CHECK_RC(find_str(&other_changes, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_SETTINGS) == 0);
}

/* Expected scene identifier "sceneID:<bundle>-default". */
static void expected_scene_id(char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s", FB_SCENE_ID_PREFIX, SIM_BUNDLE,
             FB_SCENE_ID_SUFFIX);
}

/* Shared ports and the stashed create callout (scenario 4 drains
 * it, scenario 5 parses it). */
static mach_port_name_t g_ws_port = MACH_PORT_NULL;
static mach_port_name_t g_rb_port = MACH_PORT_NULL;
static mach_port_name_t g_workspace_port = MACH_PORT_NULL;
static uint8_t g_create_callout[8192];
static uint32_t g_create_callout_len = 0;
static uint32_t g_create_callout_id = 0;

/* ------------------------------------------------------------------ */
/* Scenario 1: w00t shape + "connect" one-way (no reply).              */
/* ------------------------------------------------------------------ */

static void scen_1_connect_oneway(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint8_t w00t[64];
    uint32_t body_len = 0, reply_size = 0, w00t_len = 0;
    int ret = 0;
    xw_envelope env;
    SCEN_BEGIN(1, "w00t shape + connect one-way (no reply)");
    /* The w00t is consumed by ipc.c, not by the fake; assert its
     * shape only. Two port descriptors, no XPC payload. */
    w00t_len = build_w00t(w00t, sizeof(w00t), g_ws_port);
    CHECK(w00t_len == 52);
    CHECK_RC(xw_envelope_parse(w00t, w00t_len, &env));
    CHECK(env.id == SIM_MSGH_ID_W00T);
    CHECK(env.ndescs == 2);
    CHECK(env.descs[0].dtype == XW_DESC_PORT);
    CHECK(env.descs[0].disposition == MACH_MSG_TYPE_MOVE_RECEIVE);
    CHECK((uint32_t)env.descs[0].obj == SIM_CLIENT_PORT);
    CHECK(env.descs[1].dtype == XW_DESC_PORT);
    CHECK(env.descs[1].disposition == MACH_MSG_TYPE_MOVE_SEND);
    CHECK((uint32_t)env.descs[1].obj == SIM_ENDPOINT_PORT);
    CHECK(env.payload_len == 0);
    /* The "connect" that follows it, sent one-way (msgh_id
     * 0x10000000) with a reply port present: the fake must process
     * it (and learn the endpoint from the "endpoint" port in the
     * context, rule 1, persistent) but build NO reply. This is the
     * rule that prevents the libxpc async-reply crash. Delivered
     * with the endpoint port descriptor (COPY_SEND) that the
     * mach_send value in the context requires. */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_NO_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size == 0);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: connect+initialize expects a reply.                     */
/* ------------------------------------------------------------------ */

static void scen_2_connect_initialize_reply(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    uint64_t cid = 0;
    int cidr = -1;
    SCEN_BEGIN(2, "connect+initialize expects reply (CID echo)");
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    /* The reply echoes the call id and marks itself a reply. */
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 1);
    CHECK(cidr == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: RBS handshakeWithRequest:.                              */
/* ------------------------------------------------------------------ */

static void scen_3_rbs_handshake(void) {
    uint8_t body[2048];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    uint64_t cid = 0, pid = 0;
    int cidr = -1;
    xw_envelope env;
    xw_reader rd;
    xw_value handle;
    const char *s = NULL;
    SCEN_BEGIN(3, "RBS handshakeWithRequest:");
    CHECK_RC(build_rbs_handshake(body, sizeof(body), &body_len));
    ret = deliver(fakerb_process_msg, g_rb_port, SIM_RB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_cid(reply, reply_size, &cid, &cidr));
    CHECK(cid == 1);
    CHECK(cidr == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    /* The handshake reply carries the process handle for self, at
     * the top level of the flat reply dict (bsxpc_reply_build does
     * not open a bsxpc_context result dict). */
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(xw_dict_find(&rd, FAKERB_KEY_HANDLE, &handle));
    CHECK_RC(find_uint(&handle, FB_BSXPC_KEY_HANDLE_PID, &pid));
    CHECK(pid == SIM_PID);
    CHECK_RC(find_str(&handle, FB_BSXPC_KEY_HANDLE_BUNDLE_ID, &s));
    CHECK(s != NULL && strcmp(s, SIM_BUNDLE) == 0);
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: scene-create callout (server-initiated).                */
/* ------------------------------------------------------------------ */

static void scen_4_scene_create_callout(void) {
    uint8_t callout[8192];
    uint32_t callout_len = 0, callout_id = 0;
    int ret = 0;
    xw_envelope env;
    xw_reader rd;
    xw_value identity, params, settings, tctx, v;
    const char *s = NULL;
    uint64_t cid = 0;
    int cidr = -1;
    mach_msg_header_t *hdr;
    char want_id[256];
    SCEN_BEGIN(4, "scene-create callout (server-initiated)");
    expected_scene_id(want_id, sizeof(want_id));
    /* The manual trigger (the client never creates a scene on its
     * own). The endpoint was learned from the "endpoint" port in
     * scenario 1's connect context (rule 1, persistent). */
    CHECK(fakeboard_scene_create(SIM_BUNDLE) == KERN_SUCCESS);
    CHECK(fakeboard_scene_state() == FB_SCENE_CREATING);
    ret = fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                 &callout_id);
    CHECK(ret == 1);
    CHECK(callout_len > (uint32_t)sizeof(mach_msg_header_t));
    CHECK(callout_len <= sizeof(g_create_callout));
    if (ret == 1 && callout_len <= sizeof(g_create_callout)) {
        memcpy(g_create_callout, callout, callout_len);
        g_create_callout_len = callout_len;
        g_create_callout_id = callout_id;
    }
    hdr = (mach_msg_header_t *)g_create_callout;
    CHECK(hdr->msgh_remote_port == SIM_CLIENT_PORT);
    /* One-way callout (XW_MSGH_ID_XPC): msgh_local_port is MACH_PORT_NULL. */
    CHECK(hdr->msgh_local_port == MACH_PORT_NULL);
    CHECK(g_create_callout_id == SIM_MSGH_ID_XPC_NO_REPLY);
    CHECK_RC(parse_msg(g_create_callout, g_create_callout_len, &env,
                       &rd));
    CHECK(env.id == SIM_MSGH_ID_XPC_NO_REPLY);
    /* Verified BSXPC callout envelope: "bsxpc" is OMITTED, CID matches
     * workspace connect CID (1 here), CIDr == true (routes via
     * childContextWithRemoteIdentifier:), and args are keyed "1".."3"
     * at the root. */
    CHECK(xw_dict_find(&rd, FB_BSXPC_KEY_BSXPC, &v) == XW_ERR_NOT_FOUND);
    CHECK_RC(find_str_r(&rd, FB_BSXPC_KEY_SEL, &s));
    CHECK(s != NULL && strcmp(s, FB_MSG_CREATE_SCENE) == 0);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CID, &v));
    CHECK_RC(xw_as_uint64(&v, &cid));
    CHECK(cid == 1);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CIDR, &v));
    CHECK_RC(xw_as_bool(&v, &cidr));
    CHECK(cidr == 1);

    /* "1": FBSSceneIdentity */
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_ARG_1, &identity));
    CHECK_RC(find_str(&identity, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_SCENE_IDENTITY) == 0);
    CHECK_RC(find_str(&identity, FB_ID_KEY_I, &s));
    CHECK(s != NULL && strcmp(s, want_id) == 0);

    /* "2": FBSSceneParameters ("specification" is a raw xpc_string). */
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_ARG_2, &params));
    CHECK_RC(find_str(&params, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_SCENE_PARAMETERS) == 0);
    CHECK_RC(find_str(&params, FB_ENV_SPECIFICATION, &s));
    CHECK(s != NULL && strcmp(s, FB_SPEC_UIAPPLICATION_SCENE) == 0);
    CHECK_RC(find_dict(&params, FB_ENV_SETTINGS, &settings));
    check_settings_diff(&settings, 1);

    /* "3": UIApplicationSceneTransitionContext */
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_ARG_3, &tctx));
    CHECK_RC(find_str(&tctx, FB_BSX_KEY_CLASS, &s));
    CHECK(s != NULL && strcmp(s, FB_CLS_TRANSITION_CONTEXT) == 0);
    SCEN_END(4);
}

/* Read an action's kind/target/timeout out of a dictionary value
 * (the bsxpc_context of an enveloped callout, or the root value of
 * a bare action callout). Returns XW_OK or a negative XW_ERR_*. */
static int action_fields(const xw_value *dict, char *kind, size_t kind_cap,
                         char *target, size_t target_cap,
                         uint64_t *timeout_out) {
    const char *s = NULL;
    uint32_t len = 0;
    xw_value v;
    int rc = xw_dict_find_v(dict, FB_BSXPC_KEY_ACTION_KIND, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_string(&v, &s, &len);
    if (rc != XW_OK || s == NULL) return rc;
    if (len > kind_cap) return XW_ERR_NOSPACE;
    memcpy(kind, s, len);
    rc = xw_dict_find_v(dict, FB_BSXPC_KEY_ACTION_TARGET, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_string(&v, &s, &len);
    if (rc != XW_OK || s == NULL) return rc;
    if (len > target_cap) return XW_ERR_NOSPACE;
    memcpy(target, s, len);
    rc = xw_dict_find_v(dict, FB_BSXPC_KEY_ACTION_TIMEOUT, &v);
    if (rc != XW_OK) return rc;
    return xw_as_uint64(&v, timeout_out);
}

/* Parse a lifecycle action out of a parsed callout. Tries the
 * bsxpc_context dict first (enveloped action), then the root (bare
 * action dict). The kind and target values are pinned either way. */
static int callout_action(xw_reader *r, char *kind, size_t kind_cap,
                          char *target, size_t target_cap,
                          uint64_t *timeout_out) {
    xw_value ctx, root;
    int rc = xw_dict_find(r, FB_BSXPC_KEY_CONTEXT, &ctx);
    if (rc == XW_OK && ctx.type == XW_TYPE_DICTIONARY) {
        rc = action_fields(&ctx, kind, kind_cap, target, target_cap,
                           timeout_out);
        if (rc == XW_OK) return XW_OK;
    }
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    return action_fields(&root, kind, kind_cap, target, target_cap,
                         timeout_out);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: lifecycle callouts use the real keys too.               */
/* ------------------------------------------------------------------ */

static void scen_5_lifecycle_callouts(void) {
    uint8_t callout[8192];
    uint32_t callout_len = 0, callout_id = 0;
    int ret = 0;
    xw_envelope env;
    xw_reader rd;
    xw_value identity, settings, v;
    const char *s = NULL;
    int cidr = -1;
    char want_id[256];
    char kind[64];
    char target[256];
    uint64_t timeout = 0;
    SCEN_BEGIN(5, "lifecycle callouts (real keys)");
    expected_scene_id(want_id, sizeof(want_id));
    CHECK(fakeboard_scene_update_settings() == KERN_SUCCESS);
    CHECK(fakeboard_scene_activate() == KERN_SUCCESS);
    /* First drained callout: the settings update. */
    ret = fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                 &callout_id);
    CHECK(ret == 1);
    CHECK(callout_id == SIM_MSGH_ID_XPC_NO_REPLY);
    CHECK_RC(parse_msg(callout, callout_len, &env, &rd));
    CHECK(xw_dict_find(&rd, FB_BSXPC_KEY_BSXPC, &v) == XW_ERR_NOT_FOUND);
    CHECK_RC(find_str_r(&rd, FB_BSXPC_KEY_SEL, &s));
    CHECK(s != NULL && strcmp(s, FB_MSG_UPDATE_SETTINGS_DIFF) == 0);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CIDR, &v));
    CHECK_RC(xw_as_bool(&v, &cidr));
    CHECK(cidr == 1);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_ARG_1, &identity));
    CHECK_RC(find_str(&identity, FB_ID_KEY_I, &s));
    CHECK(s != NULL && strcmp(s, want_id) == 0);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_ARG_2, &settings));
    check_settings_diff(&settings, 1);
    /* Second drained callout: the bare activate action. */
    ret = fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                 &callout_id);
    CHECK(ret == 1);
    CHECK(callout_id == SIM_MSGH_ID_XPC_NO_REPLY);
    CHECK_RC(parse_msg(callout, callout_len, &env, &rd));
    CHECK_RC(callout_action(&rd, kind, sizeof(kind),
                            target, sizeof(target), &timeout));
    CHECK(strcmp(kind, FB_ACTION_ACTIVATE) == 0);
    CHECK(strcmp(target, want_id) == 0);
    CHECK(fakeboard_scene_state() == FB_SCENE_FOREGROUND);
    /* Invalidation. */
    CHECK(fakeboard_scene_invalidate() == KERN_SUCCESS);
    ret = fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                 &callout_id);
    CHECK(ret == 1);
    CHECK(callout_id == SIM_MSGH_ID_XPC_NO_REPLY);
    CHECK_RC(parse_msg(callout, callout_len, &env, &rd));
    CHECK_RC(callout_action(&rd, kind, sizeof(kind),
                            target, sizeof(target), &timeout));
    CHECK(strcmp(kind, FB_ACTION_DESTROY) == 0);
    CHECK(strcmp(target, want_id) == 0);
    CHECK(fakeboard_scene_state() == FB_SCENE_INVALIDATED);
    /* Workspace connect ("bsxpc": "connect", "SEL": "handshakeWithRemnants:")
     * carrying CID=3 must answer with CID=3/CIDr=true and push createScene
     * with CID=3/CIDr=true and no "bsxpc" key. */
    {
        uint8_t cbody[1024], creply[1024];
        uint32_t cbody_len = 0, creply_len = 0;
        uint64_t rcid = 0;
        int rcidr = -1;
        xw_builder cb;
        CHECK_RC(xw_builder_init(&cb, cbody, sizeof(cbody)));
        CHECK_RC(xw_dict_begin(&cb));
        CHECK_RC(xw_dict_add_string(&cb, FB_BSXPC_KEY_BSXPC, FB_MSG_CONNECT));
        CHECK_RC(xw_dict_add_string(&cb, FB_BSXPC_KEY_SEL,
                                    FB_MSG_HANDSHAKE_WITH_REMNANTS));
        CHECK_RC(xw_dict_add_uint64(&cb, FB_BSXPC_KEY_CID, 3));
        CHECK_RC(xw_dict_add_bool(&cb, FB_BSXPC_KEY_CIDR, 0));
        CHECK_RC(xw_dict_add_dict_begin(&cb, FB_BSXPC_KEY_CONTEXT));
        CHECK_RC(xw_dict_add_mach_send(&cb, FB_KEY_ENDPOINT,
                                      (uint64_t)SIM_CLIENT_PORT,
                                      MACH_MSG_TYPE_COPY_SEND));
        CHECK_RC(xw_dict_add_dict_begin(&cb, FB_CTX_KEY_MESSAGE));
        CHECK_RC(xw_dict_add_string(&cb, FB_CTX_KEY_SERVICE,
                                    FB_SVC_WORKSPACE_SERVICE));
        CHECK_RC(xw_dict_end(&cb));
        CHECK_RC(xw_dict_end(&cb));
        CHECK_RC(xw_dict_end(&cb));
        cbody_len = xw_builder_len(&cb);
        ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                      SIM_MSGH_ID_XPC_WANTS_REPLY, cbody, cbody_len,
                      1, SIM_CLIENT_PORT, creply, sizeof(creply), &creply_len);
        CHECK(ret == 1);
        CHECK_RC(reply_cid(creply, creply_len, &rcid, &rcidr));
        CHECK(rcid == 3 && rcidr == 1);
        ret = fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                     &callout_id);
        CHECK(ret == 1);
        CHECK_RC(parse_msg(callout, callout_len, &env, &rd));
        CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CID, &v));
        CHECK_RC(xw_as_uint64(&v, &rcid));
        CHECK(rcid == 3);
        CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CIDR, &v));
        CHECK_RC(xw_as_bool(&v, &rcidr));
        CHECK(rcidr == 1);
        CHECK(fakeboard_scene_invalidate() == KERN_SUCCESS);
        CHECK(fakeboard_next_callout(callout, sizeof(callout), &callout_len,
                                     &callout_id) == 1);
    }
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: BSXPC API self-check (parse + reply build).             */
/* ------------------------------------------------------------------ */

static void scen_6_contract_api(void) {
    uint8_t body[1024];
    uint8_t reply_body[1024];
    uint32_t body_len = 0;
    xw_reader rd;
    bsxpc_message msg;
    xw_builder b;
    uint32_t rlen = 0;
    xw_value v;
    xw_desc descs[1];
    uint64_t cid = 0;
    int cidr = -1;
    int64_t err = -1;
    const char *s = NULL;
    uint32_t slen = 0;
    SCEN_BEGIN(6, "contract API self-check");
    /* Parse the device vector with bsxpc_message_parse. The body
     * carries the endpoint port, so the reader is given the one
     * descriptor the mach_send value refers to. descs is a function
     * local: the reader keeps a pointer to it, so it must outlive
     * the parse (a block-scoped array would be stack-use-after-scope
     * under ASan). */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    memset(&rd, 0, sizeof(rd));
    descs[0].dtype = XW_DESC_PORT;
    descs[0].disposition = MACH_MSG_TYPE_COPY_SEND;
    descs[0].obj = SIM_CLIENT_PORT;
    descs[0].size = 12;
    CHECK_RC(xw_reader_init(&rd, body, body_len, descs, 1));
    memset(&msg, 0, sizeof(msg));
    CHECK_RC(bsxpc_message_parse(&rd, &g_env_keys, &msg));
    CHECK(msg.kind != NULL && strcmp(msg.kind, FB_MSG_CONNECT) == 0);
    CHECK(msg.sel != NULL &&
          strcmp(msg.sel, FB_MSG_INITIALIZE_CLIENT) == 0);
    CHECK(msg.has_kind == 1);
    CHECK(msg.has_sel == 1);
    CHECK(msg.cid == 1);
    CHECK(msg.has_cid == 1);
    CHECK(msg.cidr == 0);
    CHECK(msg.has_cidr == 1);
    CHECK(msg.has_ctx == 1);
    CHECK(msg.ctx.type == XW_TYPE_DICTIONARY);
    CHECK_RC(xw_dict_find_v(&msg.ctx, FB_ENV_BUNDLE_ID, &v));
    CHECK_RC(xw_as_string(&v, &s, &slen));
    CHECK(s != NULL && strcmp(s, SIM_BUNDLE) == 0);
    /* Build a completion reply with bsxpc_reply_build and check its
     * shape: CID echo + CIDr true at the top level, result (error)
     * at the top level too. The reply is flat: bsxpc_reply_build
     * opens one dict and leaves it open for the result fields, so
     * one xw_dict_end closes it (no bsxpc_context result dict). */
    CHECK_RC(xw_builder_init(&b, reply_body, sizeof(reply_body)));
    CHECK_RC(bsxpc_reply_build(&b, &g_env_keys, 1));
    CHECK_RC(xw_dict_add_int64(&b, FB_ENV_ERROR, 0));
    CHECK_RC(xw_dict_end(&b)); /* reply dict */
    rlen = xw_builder_len(&b);
    CHECK(rlen > 8);
    memset(&rd, 0, sizeof(rd));
    CHECK_RC(xw_reader_init(&rd, reply_body, rlen, NULL, 0));
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CID, &v));
    CHECK_RC(xw_as_uint64(&v, &cid));
    CHECK(cid == 1);
    CHECK_RC(xw_dict_find(&rd, FB_BSXPC_KEY_CIDR, &v));
    CHECK_RC(xw_as_bool(&v, &cidr));
    CHECK(cidr == 1);
    CHECK_RC(xw_dict_find(&rd, FB_ENV_ERROR, &v));
    CHECK_RC(xw_as_int64(&v, &err));
    CHECK(err == 0);
    /* Parse is tolerant: a reply dict parses fine, with the absent
     * kind and selector flags clear (it lacks bsxpc and bsxpc_SEL)
     * and the CID / CIDr / context flags set as present. */
    memset(&msg, 0, sizeof(msg));
    CHECK_RC(bsxpc_message_parse(&rd, &g_env_keys, &msg));
    CHECK(msg.kind == NULL);
    CHECK(msg.has_kind == 0);
    CHECK(msg.sel == NULL);
    CHECK(msg.has_sel == 0);
    CHECK(msg.cid == 1);
    CHECK(msg.has_cid == 1);
    CHECK(msg.cidr == 1);
    CHECK(msg.has_cidr == 1);
    /* A dict missing bsxpc_SEL also parses, with has_sel clear and
     * the other fields present. */
    {
        xw_builder b2;
        uint8_t no_sel[512];
        uint32_t no_sel_len = 0;
        CHECK_RC(xw_builder_init(&b2, no_sel, sizeof(no_sel)));
        CHECK_RC(xw_dict_begin(&b2));
        CHECK_RC(xw_dict_add_string(&b2, FB_BSXPC_KEY_BSXPC,
                                    FB_MSG_CONNECT));
        CHECK_RC(xw_dict_add_dict_begin(&b2, FB_BSXPC_KEY_CONTEXT));
        CHECK_RC(xw_dict_end(&b2));
        CHECK_RC(xw_dict_add_uint64(&b2, FB_BSXPC_KEY_CID, 9));
        CHECK_RC(xw_dict_add_bool(&b2, FB_BSXPC_KEY_CIDR, 0));
        CHECK_RC(xw_dict_end(&b2));
        no_sel_len = xw_builder_len(&b2);
        memset(&rd, 0, sizeof(rd));
        CHECK_RC(xw_reader_init(&rd, no_sel, no_sel_len, NULL, 0));
        memset(&msg, 0, sizeof(msg));
        CHECK_RC(bsxpc_message_parse(&rd, &g_env_keys, &msg));
        CHECK(msg.kind != NULL && strcmp(msg.kind, FB_MSG_CONNECT) == 0);
        CHECK(msg.has_kind == 1);
        CHECK(msg.sel == NULL);
        CHECK(msg.has_sel == 0);
        CHECK(msg.cid == 9);
        CHECK(msg.has_cid == 1);
        CHECK(msg.has_ctx == 1);
    }
    /* A root that is not a dictionary still fails parse. */
    {
        xw_builder b3;
        uint8_t not_dict[64];
        uint32_t not_dict_len = 0;
        CHECK_RC(xw_builder_init(&b3, not_dict, sizeof(not_dict)));
        CHECK_RC(xw_array_begin(&b3));
        CHECK_RC(xw_add_string(&b3, "x"));
        CHECK_RC(xw_array_end(&b3));
        not_dict_len = xw_builder_len(&b3);
        memset(&rd, 0, sizeof(rd));
        CHECK_RC(xw_reader_init(&rd, not_dict, not_dict_len, NULL, 0));
        memset(&msg, 0, sizeof(msg));
        CHECK(bsxpc_message_parse(&rd, &g_env_keys, &msg) != XW_OK);
    }
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: unknown selectors + malformed dicts.                    */
/* ------------------------------------------------------------------ */

static void scen_7_errors(void) {
    uint8_t body[2048];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    SCEN_BEGIN(7, "unknown selectors + malformed dicts");
    /* Unknown selector to the workspace service: an error reply
     * (error 1), never silence. The kind is the selector itself
     * (not "connect"): a "connect" kind with an unrecognized
     * selector is answered with a handshake ack by the fake's
     * connect branch, so it cannot pin the unknown-selector error;
     * a non-connect kind reaches the selector dispatch, which
     * answers error 1 for a selector it does not implement. The
     * error reply is a flat {"error": 1} dict with no CID echo
     * (the fake builds it without bsxpc_reply_build), so only the
     * error code is pinned here, not the CID. The context carries
     * the endpoint port, so the message is delivered with its
     * descriptor. */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), "noSuchSelector_xyz",
                             "noSuchSelector_xyz", 7, 0,
                             sim_ctx_initialize, &body_len));
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 1);
    /* Unknown selector to RunningBoard. */
    CHECK_RC(build_bsxpc_msg(body, sizeof(body), "noSuchSelector_xyz",
                             "noSuchSelector_xyz", 8, 0,
                             sim_ctx_handshake, &body_len));
    ret = deliver(fakerb_process_msg, g_rb_port, SIM_RB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 1);
    /* Truncated XPC body to the workspace service: the header plus
     * descriptor plus 10 body bytes. The reader cannot validate it;
     * the fake must still answer an error reply, never drop it. The
     * send size is (header + descriptor) + 10, so the truncation
     * cuts the body, not the envelope. */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    {
        uint8_t msg[4096];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_ws_port,
                                       SIM_FB_REPLY_PORT,
                                       SIM_MSGH_ID_XPC_WANTS_REPLY,
                                       body, body_len, 1, SIM_CLIENT_PORT);
        uint32_t head = size - body_len;
        CHECK(size > head + 10);
        ret = fakeboard_process_msg((const mach_msg_header_t *)msg,
                                    head + 10, reply, sizeof(reply),
                                    &reply_size);
        CHECK(ret == 1);
        CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err != 0);
    }
    /* Truncated XPC body to RunningBoard. */
    CHECK_RC(build_rbs_handshake(body, sizeof(body), &body_len));
    {
        uint8_t msg[4096];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_rb_port,
                                       SIM_RB_REPLY_PORT,
                                       SIM_MSGH_ID_XPC_WANTS_REPLY,
                                       body, body_len, 0, 0);
        uint32_t head = size - body_len;
        CHECK(size > head + 10);
        ret = fakerb_process_msg((const mach_msg_header_t *)msg,
                                 head + 10, reply, sizeof(reply),
                                 &reply_size);
        CHECK(ret == 1);
        CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err != 0);
    }
    /* Bad magic: the first body byte flipped. */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    body[0] ^= 0xffu;
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err != 0);
    /* Wrong type for bsxpc_CID (a string, not a uint64). */
    {
        xw_builder b;
        uint8_t wrong[1024];
        uint32_t wrong_len = 0;
        CHECK_RC(xw_builder_init(&b, wrong, sizeof(wrong)));
        CHECK_RC(xw_dict_begin(&b));
        CHECK_RC(xw_dict_add_string(&b, FB_BSXPC_KEY_BSXPC,
                                    FB_MSG_CONNECT));
        CHECK_RC(xw_dict_add_dict_begin(&b, FB_BSXPC_KEY_CONTEXT));
        CHECK_RC(xw_dict_end(&b));
        CHECK_RC(xw_dict_add_string(&b, FB_BSXPC_KEY_CID, "one"));
        CHECK_RC(xw_dict_add_bool(&b, FB_BSXPC_KEY_CIDR, 0));
        CHECK_RC(xw_dict_add_string(&b, FB_BSXPC_KEY_SEL,
                                    FB_MSG_INITIALIZE_CLIENT));
        CHECK_RC(xw_dict_end(&b));
        wrong_len = xw_builder_len(&b);
        ret = deliver(fakeboard_process_msg, g_ws_port,
                      SIM_FB_REPLY_PORT, SIM_MSGH_ID_XPC_WANTS_REPLY,
                      wrong, wrong_len, 0, 0, reply, sizeof(reply),
                      &reply_size);
        CHECK(ret == 1);
        CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err != 0);
    }
    /* Unknown XPC type code at the root (type word patched to
     * 0x0000ffff after building a valid body). */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    body[8] = 0xffu;
    body[9] = 0xffu;
    body[10] = 0x00u;
    body[11] = 0x00u;
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err != 0);
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: verbatim trace log behind IOSWINE_SVC_TRACE.            */
/* ------------------------------------------------------------------ */

static long sim_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size;
}

static int sim_file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    char buf[8192];
    size_t n = 0;
    int found = 0;
    if (f == NULL) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (strstr(buf, needle) != NULL) found = 1;
    return found;
}

static void scen_8_trace(void) {
    const char *log_path = "/tmp/sim_trace.log";
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int saved_fd = -1;
    int log_fd = -1;
    long size_before = -1, size_after = -1;
    SCEN_BEGIN(8, "verbatim trace log");
    bsxpc_set_trace(1);
    saved_fd = dup(2);
    CHECK(saved_fd >= 0);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(log_fd >= 0);
    if (log_fd >= 0) {
        CHECK(dup2(log_fd, 2) == 2);
        close(log_fd);
    }
    /* One workspace message with the real envelope keys. */
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    if (saved_fd >= 0) {
        CHECK(dup2(saved_fd, 2) == 2);
        close(saved_fd);
    }
    CHECK(sim_file_contains(log_path, FB_BSXPC_KEY_BSXPC) == 1);
    CHECK(sim_file_contains(log_path, FB_BSXPC_KEY_SEL) == 1);
    CHECK(sim_file_contains(log_path, FB_MSG_INITIALIZE_CLIENT) == 1);
    /* Trace off: a second message must not grow the log. */
    bsxpc_set_trace(0);
    size_before = sim_file_size(log_path);
    CHECK(size_before > 0);
    CHECK_RC(build_connect_initialize(body, sizeof(body), &body_len));
    ret = deliver(fakeboard_process_msg, g_ws_port, SIM_FB_REPLY_PORT,
                  SIM_MSGH_ID_XPC_WANTS_REPLY, body, body_len,
                  1, SIM_CLIENT_PORT, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    size_after = sim_file_size(log_path);
    CHECK(size_after == size_before);
    bsxpc_set_trace(-1);
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: fuzz both entry points.                                 */
/* ------------------------------------------------------------------ */

static void scen_9_fuzz(void) {
    uint8_t fb_body[4096];
    uint8_t rb_body[4096];
    uint8_t body_scratch[4096];
    uint8_t msg[8192];
    uint8_t reply[2048];
    uint32_t fb_body_len = 0, rb_body_len = 0;
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(9, "fuzz entries (10000 iterations)");
    /* Two valid bodies. The Mach message is rebuilt fresh around
     * each mutated body, so the header and the endpoint descriptor
     * stay correct and only the XPC body is mutated: a reply is
     * always owed for a 0x40000000 message with a reply port, and
     * the mutation can only corrupt the body, never the envelope.
     * The workspace body carries the endpoint port, so its message
     * is built with the descriptor; the RB body carries none. */
    CHECK_RC(build_connect_initialize(fb_body, sizeof(fb_body),
                                      &fb_body_len));
    CHECK(fb_body_len > 0);
    CHECK_RC(build_rbs_handshake(rb_body, sizeof(rb_body),
                                 &rb_body_len));
    CHECK(rb_body_len > 0);
    for (i = 0; i < 5000; i++) {
        int to_rb = (i % 2) == 1;
        uint32_t orig_body_len = to_rb ? rb_body_len : fb_body_len;
        uint32_t body_send_len = orig_body_len;
        uint32_t msg_len = 0, reply_size = 0;
        int ret = 0;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        memcpy(body_scratch, to_rb ? rb_body : fb_body, orig_body_len);
        /* 1 to 8 random byte mutations, body only. */
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % orig_body_len;
            body_scratch[off] = (uint8_t)rng_next();
        }
        /* Sometimes truncate the body (possibly to zero bytes). */
        if ((rng_next() % 8u) == 0) {
            body_send_len = rng_next() % (orig_body_len + 1);
        }
        if (to_rb) {
            msg_len = build_mach_msg(msg, sizeof(msg), g_rb_port,
                                     SIM_RB_REPLY_PORT,
                                     SIM_MSGH_ID_XPC_WANTS_REPLY,
                                     body_scratch, body_send_len, 0, 0);
        } else {
            msg_len = build_mach_msg(msg, sizeof(msg), g_ws_port,
                                     SIM_FB_REPLY_PORT,
                                     SIM_MSGH_ID_XPC_WANTS_REPLY,
                                     body_scratch, body_send_len, 1,
                                     SIM_CLIENT_PORT);
        }
        CHECK(msg_len > 0);
        if (to_rb) {
            ret = fakerb_process_msg((const mach_msg_header_t *)msg,
                                     msg_len, reply, sizeof(reply),
                                     &reply_size);
        } else {
            ret = fakeboard_process_msg((const mach_msg_header_t *)msg,
                                        msg_len, reply, sizeof(reply),
                                        &reply_size);
        }
        /* A reply is always owed for a 0x40000000 message with a
         * reply port: never a silent drop of a send-once reply. */
        if (ret != 1 || reply_size == 0) {
            if (violations < 5) {
                printf("\n  fuzz iter %d: ret=%d reply_size=%u "
                       "(reply dropped)\n", i, ret, reply_size);
            }
            violations++;
            continue;
        }
        /* A reply must be at least header plus the 40-byte minimal
         * error dict, and its body must parse with reply id. */
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
    /* One-way pass: the same vectors with msgh_id 0x10000000 must
     * never produce a reply and never crash. */
    for (i = 0; i < 5000; i++) {
        int to_rb = (i % 2) == 1;
        uint32_t orig_body_len = to_rb ? rb_body_len : fb_body_len;
        uint32_t msg_len = 0, reply_size = 0;
        int ret = 0;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        memcpy(body_scratch, to_rb ? rb_body : fb_body, orig_body_len);
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % orig_body_len;
            body_scratch[off] = (uint8_t)rng_next();
        }
        if (to_rb) {
            msg_len = build_mach_msg(msg, sizeof(msg), g_rb_port,
                                     SIM_RB_REPLY_PORT,
                                     SIM_MSGH_ID_XPC_WANTS_REPLY,
                                     body_scratch, orig_body_len, 0, 0);
        } else {
            msg_len = build_mach_msg(msg, sizeof(msg), g_ws_port,
                                     SIM_FB_REPLY_PORT,
                                     SIM_MSGH_ID_XPC_WANTS_REPLY,
                                     body_scratch, orig_body_len, 1,
                                     SIM_CLIENT_PORT);
        }
        CHECK(msg_len > 0);
        /* Flip the msgh_id to one-way (offset 20). */
        put_u32le(msg + 20, SIM_MSGH_ID_XPC_NO_REPLY);
        if (to_rb) {
            ret = fakerb_process_msg((const mach_msg_header_t *)msg,
                                     msg_len, reply, sizeof(reply),
                                     &reply_size);
        } else {
            ret = fakeboard_process_msg((const mach_msg_header_t *)msg,
                                        msg_len, reply, sizeof(reply),
                                        &reply_size);
        }
        if (ret != 0 && ret != 1) {
            if (violations < 5) {
                printf("\n  fuzz one-way iter %d: bad return %d\n",
                       i, ret);
            }
            violations++;
            continue;
        }
        if (ret == 1 && reply_size != 0) {
            if (violations < 5) {
                printf("\n  fuzz one-way iter %d: reply built for a "
                       "one-way message (%u bytes)\n", i, reply_size);
            }
            violations++;
        }
    }
    printf("  fuzz: 10000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    int rc = 0;
    printf("sim_test: fake FrontBoard / RunningBoard host simulator "
           "(real BSXPC wire)\n");
    rc = fakeboard_init();
    CHECK(rc == KERN_SUCCESS);
    if (rc != KERN_SUCCESS) {
        printf("fakeboard_init failed: %d\n", rc);
        printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    rc = fakerb_init();
    CHECK(rc == KERN_SUCCESS);
    if (rc != KERN_SUCCESS) {
        printf("fakerb_init failed: %d\n", rc);
        printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    fakeboard_set_self_pid(SIM_PID);
    fakerb_set_self_pid(SIM_PID);
    fakerb_set_self_bundle_id(SIM_BUNDLE);
    g_ws_port = svchost_lookup_port(FB_SVC_SYSTEMAPP_SERVICES);
    g_rb_port = svchost_lookup_port(FAKERB_SERVICE_NAME);
    g_workspace_port = svchost_lookup_port(FB_SVC_WORKSPACE_SERVICE);
    CHECK(g_ws_port != MACH_PORT_NULL);
    CHECK(g_rb_port != MACH_PORT_NULL);
    CHECK(g_workspace_port != MACH_PORT_NULL);
    if (g_ws_port == MACH_PORT_NULL || g_rb_port == MACH_PORT_NULL ||
        g_workspace_port == MACH_PORT_NULL) {
        printf("service lookup failed: ws=0x%x rb=0x%x workspace=0x%x\n",
               g_ws_port, g_rb_port, g_workspace_port);
        printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    scen_1_connect_oneway();
    scen_2_connect_initialize_reply();
    scen_3_rbs_handshake();
    scen_4_scene_create_callout();
    scen_5_lifecycle_callouts();
    scen_6_contract_api();
    scen_7_errors();
    scen_8_trace();
    scen_9_fuzz();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
