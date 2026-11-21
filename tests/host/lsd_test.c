/* lsd_test.c: host test for the fake LaunchServices daemon.
 *
 * What it covers:
 *   1. lsd_init(): the three services register (xpc, mapdb,
 *      modifydb), idempotent second call; the five unregistered
 *      names (open, openurl, plugin, icons, advertisingidentifiers)
 *      have no port.
 *   2. Own-bundle record lookup (Hypothesis A) through
 *      lsd_process_msg: error 0, bundleIdentifier, bundleURL and
 *      dataContainerURL from the container fixture, the two UUIDs
 *      different, teamID and the entitlements dict from the csops
 *      fixture.
 *   3. The same lookup through svchost_process_msg (registered xpc
 *      port) and through the mapdb / modifydb ports.
 *   4. Unknown bundle id: error reply, never a crash or a hang.
 *   5. URL-scheme lookup: own scheme answers the record, an
 *      unclaimed scheme answers LSD_ERR_NO_HANDLER (2), a full URL
 *      under the URL key works too.
 *   6. allApplications: single-element records list with only the
 *      running app. Plug-in query: empty records list.
 *      Registration message: success, no state change. Open: success
 *      false. Database-change observation: success.
 *   7. Malformed input: truncated XPC body, a dict with a non-string
 *      bundle id, an empty dict, and a body that is an array instead
 *      of a dict each get {"error": 22} and never crash.
 *   8. NSXPC placeholder (synthetic): an archiver-shaped dict
 *      without an operation key but carrying the bundle id string
 *      answers the own record; one without the bundle id answers
 *      {"error": 1}. The byte string is hand-built here, NOT a real
 *      capture; the test is marked synthetic until a real device
 *      trace lands.
 *   9. No-csops-info case: the record omits teamID and entitlements
 *      but still answers error 0.
 *  10. Verbatim trace log behind IOSWINE_SVC_TRACE (bsxpc_set_trace
 *      override, fd 2 redirected to a file).
 *  11. Reply size: the largest reply (full own-bundle record with
 *      entitlements) fits in SVCHOST_MAX_REPLY_BODY.
 *  12. Fuzz lsd_process_msg (10000 iterations): mutated/truncated
 *      client messages must never crash, never hang, and always get
 *      a parseable reply or no reply for a one-way message.
 *
 * How the service is driven on host: there is no real ipc.c here,
 * so this file stubs the ipc_* symbols the .c files reference
 * (found with `nm -u` on the compiled objects: g_ipc_trace,
 * lsk_getenv, ipc_task_self, ipc_mach_reply_port,
 * ipc_svchost_port_create, ipc_port_from_name, ipc_port_deallocate,
 * ipc_port_destroy, ipc_msg). ipc_msg emulates ONLY the
 * bootstrap_register2 path (msgh_id 0x100000cc) that
 * svchost_register's self-registration uses: it records name ->
 * port in a small static table and builds a reply {"error": 0}
 * with msgh_id 0x20000000 in the same buffer. Any other msgh_id
 * returns MACH_SEND_INVALID_DEST.
 *
 * The test links src/container.c and src/csops.c. The csops
 * fixture is a hand-built superblob (build_cd / build_blob /
 * build_superblob below, same pattern as csops_test.c): slot 0 is
 * a CodeDirectory version 0x20500, ident "app.aidoku.Aidoku",
 * team "TEAMID1234"; slot 5 is an XML entitlements plist with
 * com.apple.developer.team-identifier, application-identifier and
 * get-task-allow false. The parsed csops_info is handed to the
 * service with lsd_set_csops_info, so the record's teamID and
 * nested entitlements dict come from the real parser.
 *
 * Test-side wire keys come from lsd_names.h (LSD_* defines), the
 * same placeholders the service uses. The fixture values below are
 * TEST_* defines, not wire strings.
 *
 * Build (run from the daemons directory). The target linux.h
 * is aarch64-only, so the build needs the host shim first on the
 * -I path (tests/host/host-shim/linux.h); it translates the
 * aarch64 syscall numbers the target code uses to host libc calls.
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/lsd_test tests/host/lsd_test.c src/lsd.c \
 *        src/svchost.c src/xpcwire.c src/bsxpc.c src/bplist.c \
 *        src/container.c src/csops.c
 *     /tmp/lsd_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/lsd_test_asan \
 *        tests/host/lsd_test.c src/lsd.c src/svchost.c src/xpcwire.c \
 *        src/bsxpc.c src/bplist.c src/container.c src/csops.c
 *     /tmp/lsd_test_asan
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
#include "lsd.h"
#include "lsd_names.h"
#include "launchd.h"
#include "bplist.h"
#include "container.h"
#include "csops.h"

/* ------------------------------------------------------------------ */
/* Test fixture values.                                                */
/* ------------------------------------------------------------------ */

/* Two different deterministic container UUIDs: the data container
 * and the bundle container must never share one. */
#define TEST_DATA_UUID "11111111-2222-3333-4444-555555555555"
#define TEST_BUNDLE_UUID "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"
#define TEST_DATA_HOME "/private/var/mobile/Containers/Data/Application/" TEST_DATA_UUID
#define TEST_BUNDLE_ROOT "/private/var/containers/Bundle/Application/" TEST_BUNDLE_UUID
#define TEST_BUNDLE_URL "file:///private/var/containers/Bundle/Application/" TEST_BUNDLE_UUID "/Aidoku.app/"
#define TEST_BUNDLE_CONTAINER_URL "file:///private/var/containers/Bundle/Application/" TEST_BUNDLE_UUID "/"
#define TEST_DATA_CONTAINER_URL "file:///private/var/mobile/Containers/Data/Application/" TEST_DATA_UUID "/"
#define TEST_TEAM_ID "TEAMID1234"
#define TEST_ENT_APP_ID "application-identifier"
#define TEST_ENT_GET_TASK_ALLOW "get-task-allow"
#define TEST_UNKNOWN_BUNDLE "com.example.NoSuchApp"

#define TEST_XPC_PORT ((mach_port_name_t)0x5000)
#define TEST_REPLY_PORT ((mach_port_name_t)0x5001)

/* ------------------------------------------------------------------ */
/* ipc stubs (no real ipc.c on host). See the file header.             */
/* ------------------------------------------------------------------ */

int g_ipc_trace = 0;

/* container.c wants the uid through the asm symbol _getuid
 * (lsk_getuid on target). The fixture user is uid 501. */
uint32_t _getuid(void) {
    return 501;
}

const char *lsk_getenv(const char *name) {
    return getenv(name);
}

mach_port_name_t ipc_task_self(void) {
    return 1;
}

static uint32_t g_tst_next_port = 0x1000;

mach_port_name_t ipc_svchost_port_create(void) {
    return g_tst_next_port++;
}

mach_port_name_t ipc_mach_reply_port(void) {
    return g_tst_next_port++;
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
#define TST_MAX_REG 16
static char g_tst_reg_names[TST_MAX_REG][128];
static mach_port_name_t g_tst_reg_ports[TST_MAX_REG];
static int g_tst_reg_count = 0;

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
    for (i = 0; i < g_tst_reg_count; i++) {
        if (strcmp(g_tst_reg_names[i], name) == 0) {
            g_tst_reg_ports[i] = (mach_port_name_t)env.descs[0].obj;
            break;
        }
    }
    if (i == g_tst_reg_count && g_tst_reg_count < TST_MAX_REG) {
        strncpy(g_tst_reg_names[g_tst_reg_count], name, 127);
        g_tst_reg_names[g_tst_reg_count][127] = '\0';
        g_tst_reg_ports[g_tst_reg_count] =
            (mach_port_name_t)env.descs[0].obj;
        g_tst_reg_count++;
    }
    /* Reply {"error": 0} in place, msgh_id 0x20000000. */
    if (xw_builder_init(&b, body, sizeof(body)) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_begin(&b) != XW_OK)
        return MACH_SEND_INVALID_DEST;
    if (xw_dict_add_int64(&b, LSD_KEY_ERROR, 0) != XW_OK)
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
/* Csops fixture: hand-built superblob (same pattern as             */
/* csops_test.c). Slot 0 is a CodeDirectory version 0x20500, ident  */
/* "app.aidoku.Aidoku", team "TEAMID1234"; slot 5 is an XML         */
/* entitlements plist. Parsed once in main() with the real          */
/* csops_parse; the resulting csops_info is handed to the service.  */
/* ------------------------------------------------------------------ */

static csops_info g_tst_csops;
static uint8_t g_tst_cd[1024];
static uint32_t g_tst_cd_len;
static uint8_t g_tst_ent_blob[2048];
static uint32_t g_tst_ent_blob_len;
static uint8_t g_tst_sb[4096];
static uint32_t g_tst_sb_len;

static const char k_tst_ent_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>com.apple.developer.team-identifier</key>\n"
    "\t<string>TEAMID1234</string>\n"
    "\t<key>application-identifier</key>\n"
    "\t<string>TEAMID1234.app.aidoku.Aidoku</string>\n"
    "\t<key>get-task-allow</key>\n"
    "\t<false/>\n"
    "</dict>\n"
    "</plist>\n";

static void tst_put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void tst_put_u64be(uint8_t *p, uint64_t v) {
    tst_put_u32be(p, (uint32_t)(v >> 32));
    tst_put_u32be(p + 4, (uint32_t)v);
}

typedef struct {
    uint32_t version;
    uint32_t hash_type;
    uint32_t hash_size;
    uint32_t n_code;
    uint32_t n_special;
    uint32_t code_limit;
    uint32_t page_size;
    uint32_t flags;
    const char *ident;
    const char *team;
} tst_cd_params;

static uint32_t tst_cd_header_size(uint32_t version) {
    uint32_t s = 44;
    if (version >= 0x20100) s += 4;
    if (version >= 0x20200) s += 4;
    if (version >= 0x20300) s += 12;
    if (version >= 0x20400) s += 24;
    if (version >= 0x20500) s += 8;
    return s;
}

/* Build a CodeDirectory. Returns its length, or 0 when the buffer
 * is too small. Hash bytes are sequential (j & 0xff). */
static uint32_t tst_build_cd(uint8_t *buf, uint32_t cap,
                             const tst_cd_params *p) {
    uint32_t hdr = tst_cd_header_size(p->version);
    uint32_t hash_off = hdr;
    uint32_t hash_len = (p->n_special + p->n_code) * p->hash_size;
    uint32_t ident_off = hash_off + hash_len;
    uint32_t ident_len = (uint32_t)strlen(p->ident) + 1;
    uint32_t team_off = 0;
    uint32_t total;
    uint32_t o = 0;
    uint32_t j;
    if (p->team != NULL) {
        team_off = ident_off + ident_len;
        total = team_off + (uint32_t)strlen(p->team) + 1;
    } else {
        total = ident_off + ident_len;
    }
    if (cap < total) return 0;
    tst_put_u32be(buf + o, 0xfade0c02u); o += 4;
    tst_put_u32be(buf + o, total); o += 4;
    tst_put_u32be(buf + o, p->version); o += 4;
    tst_put_u32be(buf + o, p->flags); o += 4;
    tst_put_u32be(buf + o, hash_off); o += 4;
    tst_put_u32be(buf + o, ident_off); o += 4;
    tst_put_u32be(buf + o, p->n_special); o += 4;
    tst_put_u32be(buf + o, p->n_code); o += 4;
    tst_put_u32be(buf + o, p->code_limit); o += 4;
    buf[o++] = (uint8_t)p->hash_size;
    buf[o++] = (uint8_t)p->hash_type;
    buf[o++] = 0; /* platform */
    buf[o++] = (uint8_t)p->page_size;
    tst_put_u32be(buf + o, 0); o += 4; /* spare2 */
    if (p->version >= 0x20100) {
        tst_put_u32be(buf + o, 0); o += 4; /* scatterOffset */
    }
    if (p->version >= 0x20200) {
        tst_put_u32be(buf + o, team_off); o += 4; /* teamOffset */
    }
    if (p->version >= 0x20300) {
        tst_put_u32be(buf + o, 0); o += 4; /* spare3 */
        tst_put_u64be(buf + o, 0); o += 8; /* codeLimit64 */
    }
    if (p->version >= 0x20400) {
        tst_put_u64be(buf + o, 0); o += 8; /* execSegBase */
        tst_put_u64be(buf + o, 0); o += 8; /* execSegLimit */
        tst_put_u64be(buf + o, 0); o += 8; /* execSegFlags */
    }
    if (p->version >= 0x20500) {
        tst_put_u32be(buf + o, 0); o += 4; /* runtime */
        tst_put_u32be(buf + o, 0); o += 4; /* preEncryptOffset */
    }
    if (o != hdr) return 0;
    for (j = 0; j < hash_len; j++) buf[o++] = (uint8_t)(j & 0xff);
    memcpy(buf + o, p->ident, ident_len); o += ident_len;
    if (p->team != NULL) {
        memcpy(buf + o, p->team, strlen(p->team) + 1);
        o += (uint32_t)strlen(p->team) + 1;
    }
    if (o != total) return 0;
    return total;
}

/* Build a generic blob: magic + length + payload. */
static uint32_t tst_build_blob(uint8_t *buf, uint32_t cap, uint32_t magic,
                               const uint8_t *payload,
                               uint32_t payload_len) {
    if (cap < 8 + payload_len) return 0;
    tst_put_u32be(buf, magic);
    tst_put_u32be(buf + 4, 8 + payload_len);
    if (payload_len > 0) memcpy(buf + 8, payload, payload_len);
    return 8 + payload_len;
}

typedef struct {
    uint32_t type;
    const uint8_t *data;
    uint32_t len;
} tst_slot;

/* Build a superblob from prebuilt blobs, in index order. */
static uint32_t tst_build_superblob(uint8_t *out, uint32_t cap,
                                    const tst_slot *slots,
                                    uint32_t nslots) {
    uint32_t hdr = 12 + 8 * nslots;
    uint32_t o = hdr;
    uint32_t i;
    uint32_t total = hdr;
    for (i = 0; i < nslots; i++) total += slots[i].len;
    if (cap < total) return 0;
    tst_put_u32be(out, 0xfade0cc0u);
    tst_put_u32be(out + 4, total);
    tst_put_u32be(out + 8, nslots);
    for (i = 0; i < nslots; i++) {
        tst_put_u32be(out + 12 + 8 * i, slots[i].type);
        tst_put_u32be(out + 12 + 8 * i + 4, o);
        memcpy(out + o, slots[i].data, slots[i].len);
        o += slots[i].len;
    }
    return total;
}

/* Build the fixture superblob and parse it with the real
 * csops_parse. Returns 0 on success. */
static int tst_build_csops_fixture(void) {
    static const tst_cd_params cd_params = {
        0x20500, 2, 32, 2, 7, 8192, 12, 0,
        "app.aidoku.Aidoku", "TEAMID1234"
    };
    tst_slot slots[2];
    g_tst_cd_len = tst_build_cd(g_tst_cd, sizeof(g_tst_cd), &cd_params);
    if (g_tst_cd_len == 0) return -1;
    g_tst_ent_blob_len = tst_build_blob(g_tst_ent_blob,
                                        sizeof(g_tst_ent_blob),
                                        0xfade7171u,
                                        (const uint8_t *)k_tst_ent_xml,
                                        (uint32_t)strlen(k_tst_ent_xml));
    if (g_tst_ent_blob_len == 0) return -1;
    slots[0].type = 0;
    slots[0].data = g_tst_cd;
    slots[0].len = g_tst_cd_len;
    slots[1].type = 5;
    slots[1].data = g_tst_ent_blob;
    slots[1].len = g_tst_ent_blob_len;
    g_tst_sb_len = tst_build_superblob(g_tst_sb, sizeof(g_tst_sb),
                                       slots, 2);
    if (g_tst_sb_len == 0) return -1;
    if (csops_parse(g_tst_sb, g_tst_sb_len, &g_tst_csops) != 0) return -1;
    return 0;
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

/* ------------------------------------------------------------------ */
/* XPC body builders (the fake client's messages).                     */
/* ------------------------------------------------------------------ */

/* {"operation": op} */
static int build_op_only(uint8_t *body, uint32_t cap, int64_t op,
                         uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_OPERATION, op);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* {"operation": op, "bundleIdentifier": bundle_id} */
static int build_op_bundle(uint8_t *body, uint32_t cap, int64_t op,
                           const char *bundle_id, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_OPERATION, op);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, LSD_KEY_BUNDLE_ID, bundle_id);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* {"operation": op, "bundleIdentifier": <int64>} (wrong type). */
static int build_op_bundle_int(uint8_t *body, uint32_t cap, int64_t op,
                               uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_OPERATION, op);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_BUNDLE_ID, 42);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* {"operation": op, "scheme": scheme} */
static int build_op_scheme(uint8_t *body, uint32_t cap, int64_t op,
                           const char *scheme, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_OPERATION, op);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, LSD_KEY_SCHEME, scheme);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* {"operation": op, "URL": url} */
static int build_op_url(uint8_t *body, uint32_t cap, int64_t op,
                        const char *url, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, LSD_KEY_OPERATION, op);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, LSD_KEY_URL, url);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Empty dict {}. */
static int build_empty_dict(uint8_t *body, uint32_t cap,
                            uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Array root ["x"]. */
static int build_array_root(uint8_t *body, uint32_t cap,
                            uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_array_begin(&b);
    if (r == XW_OK) r = xw_add_string(&b, "x");
    if (r == XW_OK) r = xw_array_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Synthetic NSXPC-shaped dict (NOT a real capture): an archiver
 * dict with no operation key, carrying the bundle id string inside
 * an objects array. include_bundle 0 leaves the bundle id out. */
static int build_archiver(uint8_t *body, uint32_t cap, int include_bundle,
                          uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_string(&b, "$archiver", "NSKeyedArchiver");
    if (r == XW_OK) r = xw_dict_add_array_begin(&b, "$objects");
    if (r == XW_OK) r = xw_add_string(&b, "$null");
    if (r == XW_OK && include_bundle)
        r = xw_add_string(&b, LSD_APP_BUNDLE_ID);
    if (r == XW_OK) r = xw_add_string(&b, "someSelector:");
    if (r == XW_OK) r = xw_array_end(&b);
    if (r == XW_OK) r = xw_dict_add_dict_begin(&b, "$top");
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "root", 1);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Mach message delivery.                                              */
/* ------------------------------------------------------------------ */

typedef int (*tst_entry_fn_t)(const mach_msg_header_t *, uint32_t,
                              uint8_t *, uint32_t, uint32_t *);

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

/* Deliver a built message to an entry point. Returns the entry's
 * return value (0 or 1), or -1 when the message does not fit. */
static int deliver(tst_entry_fn_t entry, mach_port_name_t dest,
                   mach_port_name_t reply_port,
                   const uint8_t *body, uint32_t body_len,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    uint8_t msg[4096];
    uint32_t size = build_mach_msg(msg, sizeof(msg), dest, reply_port,
                                   body, body_len);
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

/* Read the "error" int64 out of a reply. Returns XW_OK on success. */
static int reply_error_code(const uint8_t *reply_buf, uint32_t reply_size,
                            int64_t *err_out) {
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(&rd, LSD_KEY_ERROR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

/* Nested getters on an xw_value dictionary. */
static int find_flag(const xw_value *dict, const char *key, int *out) {
    xw_value v;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, out);
}

static int find_str(const xw_value *dict, const char *key,
                    const char **out) {
    xw_value v;
    uint32_t len = 0;
    int rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_string(&v, out, &len);
}

/* Array lookup on an xw_value dictionary. */
static int find_array(const xw_value *dict, const char *key,
                      xw_value *out) {
    int rc = xw_dict_find_v(dict, key, out);
    if (rc != XW_OK) return rc;
    if (out->type != XW_TYPE_ARRAY) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Root-level string lookup on a reader. */
static int find_str_r(xw_reader *r, const char *key, const char **out) {
    xw_value v;
    uint32_t len = 0;
    int rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_string(&v, out, &len);
}

/* Root-level dict lookup on a reader. */
static int find_dict_r(xw_reader *r, const char *key, xw_value *out) {
    int rc = xw_dict_find(r, key, out);
    if (rc != XW_OK) return rc;
    if (out->type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Root-level array lookup on a reader. */
static int find_array_r(xw_reader *r, const char *key, xw_value *out) {
    int rc = xw_dict_find(r, key, out);
    if (rc != XW_OK) return rc;
    if (out->type != XW_TYPE_ARRAY) return XW_ERR_BAD_TYPE;
    return XW_OK;
}

/* Shared ports. */
static mach_port_name_t g_xpc_port = MACH_PORT_NULL;
static mach_port_name_t g_mapdb_port = MACH_PORT_NULL;
static mach_port_name_t g_modifydb_port = MACH_PORT_NULL;

/* Largest record reply seen, for the reply-size check. */
static uint32_t g_largest_record_reply = 0;
/* Largest record reply without entitlements (scenario 9). */
static uint32_t g_largest_noent_reply = 0;

/* Check one own-bundle record reply in full. Expects the reply
 * already parsed into rd; verifies every field that the fixture
 * can pin down. */
static void check_record_reply(xw_reader *rd, int with_entitlements) {
    const char *s = NULL;
    xw_value v, arr, item;
    int flag = 0;
    uint32_t n = 0;
    CHECK_RC(find_str_r(rd, LSD_KEY_BUNDLE_ID, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_BUNDLE_ID) == 0);
    CHECK_RC(find_str_r(rd, LSD_REC_BUNDLE_URL, &s));
    CHECK(s != NULL && strcmp(s, TEST_BUNDLE_URL) == 0);
    CHECK_RC(find_str_r(rd, LSD_REC_BUNDLE_CONTAINER_URL, &s));
    CHECK(s != NULL && strcmp(s, TEST_BUNDLE_CONTAINER_URL) == 0);
    CHECK_RC(find_str_r(rd, LSD_REC_DATA_CONTAINER_URL, &s));
    CHECK(s != NULL && strcmp(s, TEST_DATA_CONTAINER_URL) == 0);
    /* The two container UUIDs must differ: the bundle URL must not
     * contain the data UUID and vice versa. */
    CHECK_RC(find_str_r(rd, LSD_REC_BUNDLE_URL, &s));
    CHECK(s != NULL && strstr(s, TEST_DATA_UUID) == NULL);
    CHECK_RC(find_str_r(rd, LSD_REC_DATA_CONTAINER_URL, &s));
    CHECK(s != NULL && strstr(s, TEST_BUNDLE_UUID) == NULL);
    CHECK_RC(find_str_r(rd, LSD_REC_LOCALIZED_NAME, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_NAME) == 0);
    CHECK_RC(find_str_r(rd, LSD_REC_CF_BUNDLE_SHORT_VERSION, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_SHORT_VERSION) == 0);
    CHECK_RC(find_str_r(rd, LSD_REC_CF_BUNDLE_VERSION, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_VERSION) == 0);
    /* Bool flags live at the root; read them via a root value. */
    {
        xw_value root;
        xw_value missing;
        CHECK_RC(xw_reader_root(rd, &root));
        CHECK_RC(find_flag(&root, LSD_REC_IS_INSTALLED, &flag));
        CHECK(flag == 1);
        CHECK_RC(find_flag(&root, LSD_REC_IS_LAUNCHABLE, &flag));
        CHECK(flag == 1);
        /* Reply budget trim (see lsd.c): these keys are not sent. */
        CHECK(xw_dict_find_v(&root, "CFBundleName", &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "CFBundleExecutable", &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "MinimumOSVersion", &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "applicationType", &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "isPlaceholder", &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "LSApplicationHasRegistered",
                             &missing) == XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "LSApplicationReadyToBeFrontableKey",
                             &missing) == XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find_v(&root, "UIDeviceFamily", &missing) ==
              XW_ERR_NOT_FOUND);
    }
    /* URL types: one entry, name Aidoku, schemes ["aidoku"]. */
    CHECK_RC(find_array_r(rd, LSD_REC_URL_TYPES, &arr));
    CHECK_RC(xw_array_len_v(&arr, &n));
    CHECK(n == 1);
    if (n == 1) {
        CHECK_RC(xw_array_at_v(&arr, 0, &item));
        CHECK(item.type == XW_TYPE_DICTIONARY);
        CHECK_RC(find_str(&item, LSD_REC_URL_NAME, &s));
        CHECK(s != NULL && strcmp(s, LSD_APP_NAME) == 0);
        CHECK_RC(find_array(&item, LSD_REC_URL_SCHEMES, &v));
        CHECK_RC(xw_array_len_v(&v, &n));
        CHECK(n == 1);
        if (n == 1) {
            const char *scheme = NULL;
            uint32_t slen = 0;
            CHECK_RC(xw_array_at_v(&v, 0, &item));
            CHECK_RC(xw_as_string(&item, &scheme, &slen));
            CHECK(scheme != NULL &&
                  strcmp(scheme, LSD_APP_URL_SCHEME) == 0);
        }
    }
    if (with_entitlements) {
        CHECK_RC(find_str_r(rd, LSD_KEY_TEAM_ID, &s));
        CHECK(s != NULL && strcmp(s, TEST_TEAM_ID) == 0);
        CHECK_RC(find_dict_r(rd, LSD_KEY_ENTITLEMENTS, &v));
        CHECK_RC(find_str(&v, LSD_ENT_TEAM_ID, &s));
        CHECK(s != NULL && strcmp(s, TEST_TEAM_ID) == 0);
        CHECK_RC(find_str(&v, TEST_ENT_APP_ID, &s));
        CHECK(s != NULL &&
              strcmp(s, TEST_TEAM_ID ".app.aidoku.Aidoku") == 0);
    } else {
        xw_value missing;
        CHECK(xw_dict_find(rd, LSD_KEY_TEAM_ID, &missing) ==
              XW_ERR_NOT_FOUND);
        CHECK(xw_dict_find(rd, LSD_KEY_ENTITLEMENTS, &missing) ==
              XW_ERR_NOT_FOUND);
    }
}

/* ------------------------------------------------------------------ */
/* Scenario 1: init and registration.                                  */
/* ------------------------------------------------------------------ */

static void scen_1_init(void) {
    int rc = 0;
    SCEN_BEGIN(1, "init + registration");
    rc = lsd_init();
    CHECK(rc == KERN_SUCCESS);
    rc = lsd_init();
    CHECK(rc == KERN_SUCCESS);
    g_xpc_port = svchost_lookup_port(LSD_SVC_XPC);
    g_mapdb_port = svchost_lookup_port(LSD_SVC_MAPDB);
    g_modifydb_port = svchost_lookup_port(LSD_SVC_MODIFYDB);
    CHECK(g_xpc_port != MACH_PORT_NULL);
    CHECK(g_mapdb_port != MACH_PORT_NULL);
    CHECK(g_modifydb_port != MACH_PORT_NULL);
    CHECK(g_xpc_port != g_mapdb_port);
    CHECK(g_xpc_port != g_modifydb_port);
    CHECK(g_mapdb_port != g_modifydb_port);
    /* The five unregistered names have no port. */
    CHECK(svchost_lookup_port(LSD_SVC_OPEN) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(LSD_SVC_OPENURL) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(LSD_SVC_PLUGIN) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(LSD_SVC_ICONS) == MACH_PORT_NULL);
    CHECK(svchost_lookup_port(LSD_SVC_AD_ID) == MACH_PORT_NULL);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: own-bundle lookup via lsd_process_msg.                  */
/* ------------------------------------------------------------------ */

static void scen_2_record_direct(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    SCEN_BEGIN(2, "own-bundle record via lsd_process_msg");
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    check_record_reply(&rd, 1);
    if (reply_size > g_largest_record_reply)
        g_largest_record_reply = reply_size;
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: same lookup via svchost_process_msg and the other      */
/* two ports.                                                          */
/* ------------------------------------------------------------------ */

static void scen_3_record_svchost(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    SCEN_BEGIN(3, "record via svchost + mapdb/modifydb ports");
    /* Through the svchost dispatcher on the xpc port. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(svchost_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    check_record_reply(&rd, 1);
    if (reply_size > g_largest_record_reply)
        g_largest_record_reply = reply_size;
    /* Through the mapdb port directly. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_mapdb_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    /* Through the modifydb port directly. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_modifydb_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: unknown bundle id.                                      */
/* ------------------------------------------------------------------ */

static void scen_4_unknown_bundle(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    SCEN_BEGIN(4, "unknown bundle id");
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             TEST_UNKNOWN_BUNDLE, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: URL-scheme lookup.                                      */
/* ------------------------------------------------------------------ */

static void scen_5_scheme(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    const char *s = NULL;
    SCEN_BEGIN(5, "URL-scheme lookup");
    /* Own scheme answers the record. */
    CHECK_RC(build_op_scheme(body, sizeof(body),
                             LSD_OP_HANDLER_FOR_SCHEME,
                             LSD_APP_URL_SCHEME, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(find_str_r(&rd, LSD_KEY_BUNDLE_ID, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_BUNDLE_ID) == 0);
    /* An unclaimed scheme answers LSD_ERR_NO_HANDLER. */
    CHECK_RC(build_op_scheme(body, sizeof(body),
                             LSD_OP_HANDLER_FOR_SCHEME,
                             "http", &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == LSD_ERR_NO_HANDLER);
    /* A full URL under the URL key works too. */
    CHECK_RC(build_op_url(body, sizeof(body), LSD_OP_HANDLER_FOR_SCHEME,
                          "aidoku://open", &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: listings, registration, open, observation.             */
/* ------------------------------------------------------------------ */

static void scen_6_misc_ops(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    xw_value arr, item;
    uint32_t n = 0;
    const char *s = NULL;
    int flag = -1;
    SCEN_BEGIN(6, "listings + register + open + observe");
    /* allApplications: one record, the running app. */
    CHECK_RC(build_op_only(body, sizeof(body), LSD_OP_ALL_APPLICATIONS,
                           &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(find_array_r(&rd, LSD_KEY_RECORDS, &arr));
    CHECK_RC(xw_array_len_v(&arr, &n));
    CHECK(n == 1);
    if (n == 1) {
        CHECK_RC(xw_array_at_v(&arr, 0, &item));
        CHECK_RC(find_str(&item, LSD_KEY_BUNDLE_ID, &s));
        CHECK(s != NULL && strcmp(s, LSD_APP_BUNDLE_ID) == 0);
    }
    /* Plug-in query: empty list. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_PLUGINS,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(find_array_r(&rd, LSD_KEY_RECORDS, &arr));
    CHECK_RC(xw_array_len_v(&arr, &n));
    CHECK(n == 0);
    /* Registration: success, no state change. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_REGISTER,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    /* Open another app: false. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_OPEN,
                             TEST_UNKNOWN_BUNDLE, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    {
        xw_value root;
        CHECK_RC(xw_reader_root(&rd, &root));
        CHECK_RC(find_flag(&root, LSD_KEY_SUCCESS, &flag));
        CHECK(flag == 0);
    }
    /* Database-change observation: success, never fires. */
    CHECK_RC(build_op_only(body, sizeof(body), LSD_OP_OBSERVE_DB,
                           &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    /* Unknown operation code: error 1. */
    CHECK_RC(build_op_only(body, sizeof(body), 424242, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    /* Unregistered-service functions on the xpc service: logged,
     * error only. */
    CHECK_RC(build_op_only(body, sizeof(body), LSD_OP_OPEN_URL,
                           &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    CHECK_RC(build_op_only(body, sizeof(body), LSD_OP_ICONS, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    CHECK_RC(build_op_only(body, sizeof(body), LSD_OP_AD_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: malformed input.                                        */
/* ------------------------------------------------------------------ */

static void scen_7_malformed(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint8_t msg[4096];
    uint32_t body_len = 0, reply_size = 0, size = 0;
    int ret = 0;
    int64_t err = -1;
    SCEN_BEGIN(7, "malformed input");
    /* Truncated XPC body: header plus 10 body bytes. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    size = build_mach_msg(msg, sizeof(msg), g_xpc_port, TEST_REPLY_PORT,
                          body, body_len);
    CHECK(size > 24 + 10);
    ret = lsd_process_msg((const mach_msg_header_t *)msg, 24 + 10,
                          reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* Non-string bundle id. */
    CHECK_RC(build_op_bundle_int(body, sizeof(body), LSD_OP_RECORD,
                                 &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* Empty dict. */
    CHECK_RC(build_empty_dict(body, sizeof(body), &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* Array root. */
    CHECK_RC(build_array_root(body, sizeof(body), &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* Operation key with the wrong type (a string, not an int). */
    {
        xw_builder b;
        int r = xw_builder_init(&b, body, sizeof(body));
        if (r == XW_OK) r = xw_dict_begin(&b);
        if (r == XW_OK)
            r = xw_dict_add_string(&b, LSD_KEY_OPERATION, "record");
        if (r == XW_OK) r = xw_dict_end(&b);
        CHECK(r == XW_OK);
        if (r == XW_OK) body_len = xw_builder_len(&b);
        ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                      body, body_len, reply, sizeof(reply), &reply_size);
        CHECK(ret == 1);
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    }
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: NSXPC placeholder (synthetic archiver dict).           */
/* ------------------------------------------------------------------ */

static void scen_8_nsxpc(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    const char *s = NULL;
    SCEN_BEGIN(8, "NSXPC placeholder (synthetic)");
    /* Archiver dict carrying the bundle id: answers the record. */
    CHECK_RC(build_archiver(body, sizeof(body), 1, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    CHECK_RC(find_str_r(&rd, LSD_KEY_BUNDLE_ID, &s));
    CHECK(s != NULL && strcmp(s, LSD_APP_BUNDLE_ID) == 0);
    /* Archiver dict without the bundle id: error 1. */
    CHECK_RC(build_archiver(body, sizeof(body), 0, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: no csops info.                                          */
/* ------------------------------------------------------------------ */

static void scen_9_no_csops(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    xw_envelope env;
    xw_reader rd;
    SCEN_BEGIN(9, "record without csops info");
    lsd_set_csops_info(NULL);
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == 0);
    CHECK_RC(parse_reply(reply, reply_size, &env, &rd));
    check_record_reply(&rd, 0);
    if (reply_size > g_largest_noent_reply)
        g_largest_noent_reply = reply_size;
    lsd_set_csops_info(&g_tst_csops);
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* Scenario 10: verbatim trace log.                                    */
/* ------------------------------------------------------------------ */

static long tst_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size;
}

static int tst_file_contains(const char *path, const char *needle) {
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

static void scen_10_trace(void) {
    const char *log_path = "/tmp/lsd_trace.log";
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int saved_fd = -1;
    int log_fd = -1;
    long size_before = -1, size_after = -1;
    SCEN_BEGIN(10, "verbatim trace log");
    bsxpc_set_trace(1);
    saved_fd = dup(2);
    CHECK(saved_fd >= 0);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(log_fd >= 0);
    if (log_fd >= 0) {
        CHECK(dup2(log_fd, 2) == 2);
        close(log_fd);
    }
    /* One record lookup carrying the bundle id key. */
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    if (saved_fd >= 0) {
        CHECK(dup2(saved_fd, 2) == 2);
        close(saved_fd);
    }
    CHECK(tst_file_contains(log_path, LSD_TRACE_PREFIX) == 1);
    CHECK(tst_file_contains(log_path, LSD_KEY_BUNDLE_ID) == 1);
    /* Trace off: a second message must not grow the log. */
    bsxpc_set_trace(0);
    size_before = tst_file_size(log_path);
    CHECK(size_before > 0);
    CHECK_RC(build_op_bundle(body, sizeof(body), LSD_OP_RECORD,
                             LSD_APP_BUNDLE_ID, &body_len));
    ret = deliver(lsd_process_msg, g_xpc_port, TEST_REPLY_PORT,
                  body, body_len, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    size_after = tst_file_size(log_path);
    CHECK(size_after == size_before);
    bsxpc_set_trace(-1);
    SCEN_END(10);
}

/* ------------------------------------------------------------------ */
/* Scenario 11: reply size.                                            */
/* ------------------------------------------------------------------ */

static void scen_11_reply_size(void) {
    SCEN_BEGIN(11, "reply size budget");
    CHECK(g_largest_record_reply > 0);
    CHECK(g_largest_record_reply <= SVCHOST_MAX_REPLY);
    CHECK(g_largest_record_reply - sizeof(mach_msg_header_t) <=
          SVCHOST_MAX_REPLY_BODY);
    printf("  largest record reply: %u bytes total, %u body\n",
           g_largest_record_reply,
           g_largest_record_reply - (uint32_t)sizeof(mach_msg_header_t));
    CHECK(g_largest_noent_reply > 0);
    CHECK(g_largest_noent_reply <= SVCHOST_MAX_REPLY);
    printf("  largest record reply without entitlements: %u bytes total, "
           "%u body\n",
           g_largest_noent_reply,
           g_largest_noent_reply - (uint32_t)sizeof(mach_msg_header_t));
    SCEN_END(11);
}

/* ------------------------------------------------------------------ */
/* Scenario 12: fuzz lsd_process_msg.                                  */
/* ------------------------------------------------------------------ */

static void scen_12_fuzz(void) {
    uint8_t valid[2048];
    uint8_t scratch[4096];
    uint8_t reply[2048];
    uint32_t valid_size = 0;
    uint32_t body_len = 0;
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(12, "fuzz lsd_process_msg (10000 iterations)");
    CHECK_RC(build_op_bundle(valid + 24, sizeof(valid) - 24,
                             LSD_OP_RECORD, LSD_APP_BUNDLE_ID, &body_len));
    valid_size = build_mach_msg(valid, sizeof(valid), g_xpc_port,
                                TEST_REPLY_PORT, valid + 24, body_len);
    CHECK(valid_size > 24);
    for (i = 0; i < 10000; i++) {
        uint32_t send_size = valid_size;
        uint32_t reply_size = 0;
        int ret = 0;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        mach_msg_header_t *hdr = NULL;
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
        hdr = (mach_msg_header_t *)scratch;
        ret = lsd_process_msg(hdr, send_size, reply,
                              sizeof(reply), &reply_size);
        if (ret != 0 && ret != 1) {
            if (violations < 5) {
                printf("\n  fuzz iter %d: bad return %d\n", i, ret);
            }
            violations++;
            continue;
        }
        if (ret == 1 && reply_size > 0) {
            /* A reply must be at least header plus the 40-byte
             * minimal error dict, and its body must parse. */
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
    SCEN_END(12);
}

/* ------------------------------------------------------------------ */
/* Scenario 13: csops additive API on the parsed fixture.              */
/* ------------------------------------------------------------------ */

static void scen_13_csops_additive(void) {
    uint8_t digest[CS_SHA256_LEN];
    uint8_t cdhash[CS_SHA256_LEN];
    uint8_t expect[CS_SHA256_LEN];
    uint32_t cdhash_len = 0;
    char team[32];
    char ident[64];
    uint32_t flags = 0;
    uint8_t arena_buf[4096];
    bp_arena a;
    SCEN_BEGIN(13, "csops additive API");
    /* cs_sha256: FIPS 180-4 "abc" vector. */
    cs_sha256((const uint8_t *)"abc", 3, digest);
    CHECK(digest[0] == 0xba && digest[1] == 0x78 &&
          digest[31] == 0xad);
    /* cdhash: SHA-256 CD, 32 bytes out, equal to SHA-256 of the CD. */
    CHECK(csops_cdhash(&g_tst_csops, cdhash, sizeof(cdhash),
                       &cdhash_len) == 0);
    CHECK(cdhash_len == CS_SHA256_LEN);
    cs_sha256(g_tst_cd, g_tst_cd_len, expect);
    CHECK(memcmp(cdhash, expect, CS_SHA256_LEN) == 0);
    /* team id and identifier come from the CodeDirectory. */
    CHECK(csops_team_id(&g_tst_csops, team, sizeof(team)) == 0);
    CHECK(strcmp(team, TEST_TEAM_ID) == 0);
    CHECK(csops_identifier(&g_tst_csops, ident, sizeof(ident)) == 0);
    CHECK(strcmp(ident, "app.aidoku.Aidoku") == 0);
    /* status flags: CS_VALID | CS_ADHOC; get-task-allow is false,
     * so no CS_GET_TASK_ALLOW. */
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    flags = csops_status_flags(&g_tst_csops, &a);
    CHECK(flags == (CS_VALID | CS_ADHOC));
    /* has_entitlement: a present string key is held; a false-valued
     * key counts as not held; an absent key is 0. */
    CHECK(csops_has_entitlement(&g_tst_csops, TEST_ENT_APP_ID, &a) == 1);
    CHECK(csops_has_entitlement(&g_tst_csops, TEST_ENT_GET_TASK_ALLOW,
                                &a) == 0);
    CHECK(csops_has_entitlement(&g_tst_csops, "no.such.key", &a) == 0);
    SCEN_END(13);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    int rc = 0;
    printf("lsd_test: fake LaunchServices host test\n");
    /* Container fixture: two different deterministic UUIDs. */
    rc = container_set_home(TEST_DATA_HOME);
    CHECK(rc == 0);
    rc = container_set_bundle(TEST_BUNDLE_ROOT);
    CHECK(rc == 0);
    /* Csops fixture: hand-built superblob, parsed by the real csops. */
    CHECK(tst_build_csops_fixture() == 0);
    CHECK(g_tst_csops.has_code_directory == 1);
    CHECK(g_tst_csops.has_entitlements == 1);
    CHECK(g_tst_csops.entitlements_is_der == 0);
    CHECK(g_tst_csops.entitlements != NULL);
    CHECK(g_tst_csops.entitlements_blob != NULL);
    CHECK(g_tst_csops.code_directory != NULL);
    CHECK(g_tst_csops.code_directory_len == g_tst_cd_len);
    lsd_set_csops_info(&g_tst_csops);
    scen_1_init();
    scen_2_record_direct();
    scen_3_record_svchost();
    scen_4_unknown_bundle();
    scen_5_scheme();
    scen_6_misc_ops();
    scen_7_malformed();
    scen_8_nsxpc();
    scen_9_no_csops();
    scen_10_trace();
    scen_11_reply_size();
    scen_12_fuzz();
    scen_13_csops_additive();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
