/* notifyd_test.c: host test for the notifyd MIG server (S2).
 *
 * What it covers (scenarios H1-H18, adapted from the research doc
 * section 11 to the server-check fallback: there is no shared-memory
 * page, so register_check_2 always answers size -1, slot -1):
 *   H1  notifyd_init: service registered, pre-seed present, idempotent.
 *   H2  register_check_2 fallback: size -1, slot -1, real name_id.
 *   H3  post_4 then check: check 1 after a post, then 0.
 *   H4  register_mach_port_3 then post: delivery on the send name.
 *   H5  generate_common_port + register_common_port then post.
 *   H6  set_state_3 / get_state_3 / set_state_2 / get_state_2 round trip.
 *   H7  cancel then post: no delivery; check returns INVALID_TOKEN.
 *   H8  suspend / post / resume: exactly one delivery after resume.
 *   H9  filtered checkin answers like checkin.
 *   H10 unknown id (1099, 1000, 1001): MIG_BAD_ID reply.
 *   H11 600-byte name: MIG_BAD_ARGUMENTS, no registration.
 *   H12 fuzz: mutated/truncated messages, no crash, always a reply
 *       or a clean MIG error.
 *   H13 access control: uid 501 may not post com.apple.system.*,
 *       root may; registration (READ) is allowed.
 *   H14 signal client: post dispatches (no suspension, no pending).
 *       The kill syscall is not implemented in the host shim, so the
 *       host test asserts the dispatch only; the real kill is a
 *       target-only behaviour, flagged for the first device test.
 *   H15 file client: post writes htonl(token) to the fd.
 *   H16 regenerate: PLAIN re-registers, MEMORY fails, duplicate
 *       fails with DUP_CLIENT.
 *   H17 suspend_pid/resume_pid: non-root is a no-op, root suspends.
 *   H18 trace: IOSWINE_SVC_TRACE=1 prints a dict, off prints nothing.
 *
 * How the service is driven on host: there is no real ipc.c here, so
 * this file stubs the ipc_* symbols notifyd.c references (found with
 * `nm -u` on the compiled objects): lsk_getenv, ipc_task_self,
 * ipc_mach_reply_port, ipc_msg, ipc_port_deallocate,
 * ipc_port_destroy, ipc_notifyd_port_create,
 * ipc_notifyd_gen_port_locked, ipc_notifyd_deliver_locked,
 * ipc_port_from_name. ipc_msg emulates ONLY the bootstrap_register2
 * path (msgh_id 0x100000cc) that notifyd_init's self-registration
 * uses: it records name -> port in a small static table and builds
 * a reply {"error": 0} with msgh_id 0x20000000 in the same buffer.
 * The gen-port stub keeps a table of (receive name, send name)
 * pairs and a table of live port names; ipc_port_from_name returns
 * a distinct stable pointer per live name, so the server's pointer
 * equality check for register_mach_port_2 works. The deliver stub
 * records (dest, msgh_id) pairs for the fanout assertions.
 *
 * The real bsxpc.c / xpcwire.c / bplist.c are linked, so the trace
 * path (bsxpc_log_dict) is exercised for real.
 *
 * Build (run from ~/workspace/ioswine/daemons). The target linux.h
 * is aarch64-only, so the build needs the host shim first on the
 * -I path (tests/host/host-shim/linux.h).
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/notifyd_test tests/host/notifyd_test.c \
 *        src/notifyd.c src/xpcwire.c src/bplist.c src/bsxpc.c
 *     /tmp/notifyd_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/notifyd_test_asan \
 *        tests/host/notifyd_test.c src/notifyd.c src/xpcwire.c \
 *        src/bplist.c src/bsxpc.c
 *     /tmp/notifyd_test_asan
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
#include <signal.h>
#include "ipc.h"
#include "launchd.h"
#include "notifyd.h"
#include "xpcwire.h"
#include "bsxpc.h"

/* ------------------------------------------------------------------ */
/* Check harness.                                                     */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long _a = (long)(a); \
    long _b = (long)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, \
               __LINE__, #a, _a, _b); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, expected %llu\n", __FILE__, \
               __LINE__, #a, _a, _b); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* ipc stubs (no real ipc.c on host).                                 */
/* ------------------------------------------------------------------ */

const char *lsk_getenv(const char *name) {
    return getenv(name);
}

mach_port_name_t ipc_task_self(void) {
    return 1;
}

static uint32_t g_test_next_port = 0x1000;

mach_port_name_t ipc_mach_reply_port(void) {
    return g_test_next_port++;
}

/* Live port names and their stable fake pointers, for
 * ipc_port_from_name. Each live name gets one slot; the pointer is
 * the slot address, distinct per name. */
#define TEST_MAX_PORTS 512
static mach_port_name_t g_live_names[TEST_MAX_PORTS];
static uint8_t g_live_slots[TEST_MAX_PORTS][16];
static int g_live_count = 0;

static int test_port_is_live(mach_port_name_t name) {
    int i = 0;
    while (i < g_live_count) {
        if (g_live_names[i] == name) return 1;
        i++;
    }
    return 0;
}

static void test_port_add(mach_port_name_t name) {
    if (test_port_is_live(name)) return;
    if (g_live_count >= TEST_MAX_PORTS) return;
    g_live_names[g_live_count] = name;
    g_live_count++;
}

static mach_port_name_t test_port_alloc(void) {
    mach_port_name_t n = g_test_next_port++;
    test_port_add(n);
    return n;
}

struct ipc_port *ipc_port_from_name(mach_port_name_t name) {
    int i = 0;
    while (i < g_live_count) {
        if (g_live_names[i] == name)
            return (struct ipc_port *)(void *)g_live_slots[i];
        i++;
    }
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

#define TEST_NOTIFYD_PORT ((mach_port_name_t)0x4000)

mach_port_name_t ipc_notifyd_port_create(void) {
    test_port_add(TEST_NOTIFYD_PORT);
    return TEST_NOTIFYD_PORT;
}

/* Generated-port pairs: receive name -> send name. */
#define TEST_MAX_GEN 256
static mach_port_name_t g_gen_recv[TEST_MAX_GEN];
static mach_port_name_t g_gen_send[TEST_MAX_GEN];
static int g_gen_count = 0;

mach_port_name_t ipc_notifyd_gen_port_locked(uint32_t qlimit,
                                            mach_port_name_t *send_name_out) {
    mach_port_name_t recv;
    mach_port_name_t send;
    (void)qlimit;
    if (g_gen_count >= TEST_MAX_GEN) return MACH_PORT_NULL;
    recv = test_port_alloc();
    send = test_port_alloc();
    g_gen_recv[g_gen_count] = recv;
    g_gen_send[g_gen_count] = send;
    g_gen_count++;
    if (send_name_out) *send_name_out = send;
    return recv;
}

static int test_gen_send_for_recv(mach_port_name_t recv,
                                  mach_port_name_t *send_out) {
    int i = 0;
    while (i < g_gen_count) {
        if (g_gen_recv[i] == recv) {
            *send_out = g_gen_send[i];
            return 1;
        }
        i++;
    }
    return 0;
}

/* Recorded deliveries (dest, msgh_id). */
#define TEST_MAX_DELIVER 1024
static mach_port_name_t g_del_dest[TEST_MAX_DELIVER];
static uint32_t g_del_id[TEST_MAX_DELIVER];
static int g_del_count = 0;

int ipc_notifyd_deliver_locked(mach_port_name_t dest, uint32_t msgh_id) {
    if (!test_port_is_live(dest)) return 0;
    if (g_del_count < TEST_MAX_DELIVER) {
        g_del_dest[g_del_count] = dest;
        g_del_id[g_del_count] = msgh_id;
        g_del_count++;
    }
    return 1;
}

static int test_deliveries_to(mach_port_name_t dest, uint32_t id) {
    int n = 0;
    int i = 0;
    while (i < g_del_count) {
        if (g_del_dest[i] == dest && g_del_id[i] == id) n++;
        i++;
    }
    return n;
}

/* bootstrap_register2 table for the ipc_msg stub. */
#define TEST_MAX_REG 16
static char g_test_reg_names[TEST_MAX_REG][128];
static mach_port_name_t g_test_reg_ports[TEST_MAX_REG];
static int g_test_reg_count = 0;
static int g_test_register_calls = 0;

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
    g_test_register_calls++;
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
/* MIG message builder + reply readers.                               */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)(v & 0xffffffffULL));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rr_u32(const uint8_t *r, uint32_t off) {
    return (uint32_t)r[off] | ((uint32_t)r[off + 1] << 8) |
           ((uint32_t)r[off + 2] << 16) | ((uint32_t)r[off + 3] << 24);
}

static uint64_t rr_u64(const uint8_t *r, uint32_t off) {
    return (uint64_t)rr_u32(r, off) |
           ((uint64_t)rr_u32(r, off + 4) << 32);
}

static int32_t rr_i32(const uint8_t *r, uint32_t off) {
    return (int32_t)rr_u32(r, off);
}

typedef struct {
    uint8_t buf[4096];
    uint32_t off;
    uint32_t id;
    int is_complex;
    mach_port_name_t reply_port;
    uint32_t audit_pid;
    uint32_t audit_uid;
    uint32_t audit_gid;
    uint32_t audit_euid;
} test_msg;

static void tm_init(test_msg *m, uint32_t id, int ndescs,
                    mach_port_name_t reply_port) {
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->is_complex = (ndescs > 0);
    m->reply_port = reply_port;
    m->audit_pid = 100;
    m->audit_uid = 501;
    m->audit_gid = 501;
    m->audit_euid = 501;
    m->off = 24;
    if (ndescs > 0) {
        put_u32(m->buf + m->off, (uint32_t)ndescs);
        m->off += 4;
    }
}

static void tm_set_audit(test_msg *m, uint32_t pid, uint32_t uid,
                         uint32_t gid, uint32_t euid) {
    m->audit_pid = pid;
    m->audit_uid = uid;
    m->audit_gid = gid;
    m->audit_euid = euid;
}

static void tm_desc(test_msg *m, mach_port_name_t name, uint32_t disp) {
    put_u32(m->buf + m->off, name);
    m->off += 4;
    memset(m->buf + m->off, 0, 6);
    m->off += 6;
    m->buf[m->off] = (uint8_t)disp;
    m->buf[m->off + 1] = (uint8_t)MACH_MSG_PORT_DESCRIPTOR;
    m->off += 2;
}

static void tm_ndr(test_msg *m) {
    memset(m->buf + m->off, 0, 4);
    m->off += 4;
    m->buf[m->off] = 1;
    m->buf[m->off + 1] = 0;
    m->buf[m->off + 2] = 0;
    m->buf[m->off + 3] = 0;
    m->off += 4;
}

static void tm_u32(test_msg *m, uint32_t v) {
    put_u32(m->buf + m->off, v);
    m->off += 4;
}

static void tm_u64(test_msg *m, uint64_t v) {
    m->off = (m->off + 7u) & ~7u;
    put_u64(m->buf + m->off, v);
    m->off += 8;
}

static void tm_cstring(test_msg *m, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    uint32_t cnt = len + 1;
    uint32_t padded = (cnt + 3u) & ~3u;
    uint32_t i;
    put_u32(m->buf + m->off, cnt);
    m->off += 4;
    i = 0;
    while (i < len) {
        m->buf[m->off + i] = (uint8_t)s[i];
        i++;
    }
    m->buf[m->off + len] = 0;
    i = cnt;
    while (i < padded) {
        m->buf[m->off + i] = 0;
        i++;
    }
    m->off += padded;
}

static void tm_bytes(test_msg *m, const uint8_t *data, uint32_t len) {
    uint32_t padded = (len + 3u) & ~3u;
    uint32_t i;
    put_u32(m->buf + m->off, len);
    m->off += 4;
    i = 0;
    while (i < len) {
        m->buf[m->off + i] = data[i];
        i++;
    }
    while (i < padded) {
        m->buf[m->off + i] = 0;
        i++;
    }
    m->off += padded;
}

/* Drive notifyd_process_msg with the built message. The reply goes
 * into reply (NOTIFYD_MAX_REPLY bytes); *out_size gets its size. */
static int tm_deliver(test_msg *m, uint8_t *reply, uint32_t *out_size) {
    uint32_t bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                   MACH_MSG_TYPE_MAKE_SEND_ONCE);
    uint32_t toff;
    if (m->is_complex) bits |= MACH_MSGH_BITS_COMPLEX;
    put_u32(m->buf + 0, bits);
    put_u32(m->buf + 4, m->off);
    put_u32(m->buf + 8, TEST_NOTIFYD_PORT);
    put_u32(m->buf + 12, m->reply_port);
    put_u32(m->buf + 16, MACH_PORT_NULL);
    put_u32(m->buf + 20, m->id);
    toff = (m->off + 3u) & ~3u;
    while (m->off < toff) m->buf[m->off++] = 0;
    put_u32(m->buf + toff + 0, 0);
    put_u32(m->buf + toff + 4, 52);
    put_u32(m->buf + toff + 8, 0);
    put_u32(m->buf + toff + 12, m->audit_uid);
    put_u32(m->buf + toff + 16, m->audit_uid);
    put_u32(m->buf + toff + 20, m->audit_uid);
    put_u32(m->buf + toff + 24, m->audit_euid);
    put_u32(m->buf + toff + 28, m->audit_uid);
    put_u32(m->buf + toff + 32, m->audit_uid);
    put_u32(m->buf + toff + 36, m->audit_gid);
    put_u32(m->buf + toff + 40, m->audit_pid);
    put_u32(m->buf + toff + 44, 0);
    put_u32(m->buf + toff + 48, 1);
    return notifyd_process_msg((const mach_msg_header_t *)m->buf,
                               toff + 52, reply, NOTIFYD_MAX_REPLY,
                               out_size);
}

/* Reply field offsets (header 24 + NDR 8 + RetCode 4, then outs;
 * uint64 outs are 8-aligned). */
static int32_t reply_retcode(const uint8_t *r) {
    return rr_i32(r, 32);
}

static uint32_t reply_id(const uint8_t *r) {
    return rr_u32(r, 20);
}

/* ------------------------------------------------------------------ */
/* Scenario helpers.                                                  */
/* ------------------------------------------------------------------ */

static uint64_t do_register_check(const char *name, int token,
                                  int *status_out) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    tm_init(&m, 1012, 0, rp);
    tm_ndr(&m);
    tm_cstring(&m, name);
    tm_u32(&m, (uint32_t)token);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_U64(reply_id(reply), 1012 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    CHECK_EQ_U64(rsize, 64);
    CHECK_EQ_INT(rr_i32(reply, 36), -1);
    CHECK_EQ_INT(rr_i32(reply, 40), -1);
    *status_out = rr_i32(reply, 56);
    return rr_u64(reply, 48);
}

static void do_post_4(const char *name) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    tm_init(&m, 1010, 0, MACH_PORT_NULL);
    tm_ndr(&m);
    tm_cstring(&m, name);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
}

static int do_check(int token, int *status_out) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    tm_init(&m, 1002, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, (uint32_t)token);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_U64(reply_id(reply), 1002 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    *status_out = rr_i32(reply, 40);
    return rr_i32(reply, 36);
}

static void do_cancel(int token) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    tm_init(&m, 1016, 0, MACH_PORT_NULL);
    tm_ndr(&m);
    tm_u32(&m, (uint32_t)token);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
}

/* post_2 with an explicit audit uid; returns the status out. */
static int do_post_2(const char *name, uint32_t uid) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    tm_init(&m, 1008, 0, rp);
    tm_set_audit(&m, 100, uid, 501, uid);
    tm_ndr(&m);
    tm_cstring(&m, name);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_U64(reply_id(reply), 1008 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    return rr_i32(reply, 48);
}

static void test_h1_init(void) {
    int kr;
    uint64_t state = 0;
    printf("H1 init\n");
    notifyd_test_reset();
    kr = notifyd_init();
    CHECK_EQ_INT(kr, 0);
    CHECK_EQ_U64(notifyd_lookup_port(), TEST_NOTIFYD_PORT);
    CHECK_EQ_INT(g_test_reg_count, 1);
    CHECK(strcmp(g_test_reg_names[0],
                 "com.apple.system.notification_center") == 0);
    CHECK_EQ_U64(g_test_reg_ports[0], TEST_NOTIFYD_PORT);
    CHECK_EQ_INT(g_test_register_calls, 1);
    CHECK_EQ_INT(notifyd_test_name_count(), 8);
    kr = notifyd_init();
    CHECK_EQ_INT(kr, 0);
    CHECK_EQ_INT(g_test_register_calls, 1);
    CHECK(notifyd_test_find_name("com.apple.system.notify.ipc_version",
                                 NULL, &state, NULL, NULL) == 1);
    CHECK_EQ_U64(state, ((uint64_t)(uint32_t)getpid() << 32) | 3);
}

static void test_h2_register_check(void) {
    int status = -1;
    uint64_t nid;
    uint64_t nid2 = 0;
    int type = -1;
    printf("H2 register_check_2\n");
    notifyd_test_reset();
    nid = do_register_check("com.example.test", 1, &status);
    CHECK_EQ_INT(status, 0);
    CHECK(nid != 0xffffffffffffffffULL);
    CHECK(notifyd_test_find_name("com.example.test", &nid2, NULL, NULL,
                                 NULL) == 1);
    CHECK_EQ_U64(nid, nid2);
    CHECK(notifyd_test_client_info(100, 1, &type, NULL, NULL, NULL) == 1);
    CHECK_EQ_INT(type, 2);
}

static void test_h3_post_check(void) {
    int status = -1;
    int check = -1;
    int st = -1;
    printf("H3 post_4 + check\n");
    notifyd_test_reset();
    (void)do_register_check("com.example.test", 1, &st);
    CHECK_EQ_INT(st, 0);
    check = do_check(1, &status);
    CHECK_EQ_INT(status, 0);
    CHECK_EQ_INT(check, 0);
    do_post_4("com.example.test");
    check = do_check(1, &status);
    CHECK_EQ_INT(status, 0);
    CHECK_EQ_INT(check, 1);
    check = do_check(1, &status);
    CHECK_EQ_INT(status, 0);
    CHECK_EQ_INT(check, 0);
}

static void test_h4_mach_port_3(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    uint32_t st;
    mach_port_name_t recv;
    mach_port_name_t send = MACH_PORT_NULL;
    int d0;
    printf("H4 register_mach_port_3 + post\n");
    notifyd_test_reset();
    tm_init(&m, 1027, 0, rp);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.port");
    tm_u32(&m, 7);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_U64(reply_id(reply), 1027 + 100);
    CHECK_EQ_U64(rsize, 56);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    st = rr_u32(reply, 52);
    CHECK_EQ_U64(st, 0);
    recv = rr_u32(reply, 28);
    CHECK(recv != MACH_PORT_NULL);
    CHECK_EQ_INT(reply[38], MACH_MSG_TYPE_MOVE_RECEIVE);
    CHECK_EQ_INT(reply[39], MACH_MSG_PORT_DESCRIPTOR);
    CHECK(test_gen_send_for_recv(recv, &send) == 1);
    d0 = g_del_count;
    do_post_4("com.example.port");
    CHECK_EQ_INT(g_del_count - d0, 1);
    CHECK_EQ_INT(test_deliveries_to(send, 7), 1);
}

static void test_h5_common_port(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    uint32_t st;
    mach_port_name_t recv;
    mach_port_name_t send = MACH_PORT_NULL;
    int d0;
    printf("H5 common port\n");
    notifyd_test_reset();
    rp = test_port_alloc();
    tm_init(&m, 1025, 0, rp);
    tm_ndr(&m);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_U64(reply_id(reply), 1025 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    st = rr_u32(reply, 52);
    CHECK_EQ_U64(st, 0);
    recv = rr_u32(reply, 28);
    CHECK(recv != MACH_PORT_NULL);
    CHECK(test_gen_send_for_recv(recv, &send) == 1);
    CHECK_EQ_U64(notifyd_test_proc_common_port(100), send);
    rp = test_port_alloc();
    tm_init(&m, 1026, 0, MACH_PORT_NULL);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.common");
    tm_u32(&m, 11);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
    d0 = g_del_count;
    do_post_4("com.example.common");
    CHECK_EQ_INT(g_del_count - d0, 1);
    CHECK_EQ_INT(test_deliveries_to(send, 11), 1);
}

static void test_h6_state(void) {
    int st = -1;
    uint64_t nid;
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    printf("H6 set/get state\n");
    notifyd_test_reset();
    nid = do_register_check("com.example.state", 5, &st);
    CHECK_EQ_INT(st, 0);
    rp = test_port_alloc();
    tm_init(&m, 1020, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, 5);
    tm_u64(&m, 0x1122334455667788ULL);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(reply_id(reply), 1020 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    CHECK_EQ_U64(rr_u64(reply, 40), nid);
    CHECK_EQ_INT(rr_i32(reply, 48), 0);
    rp = test_port_alloc();
    tm_init(&m, 1018, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, 5);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(reply_id(reply), 1018 + 100);
    CHECK_EQ_U64(rr_u64(reply, 40), 0x1122334455667788ULL);
    CHECK_EQ_U64(rr_u64(reply, 48), nid);
    CHECK_EQ_INT(rr_i32(reply, 56), 0);
    rp = test_port_alloc();
    tm_init(&m, 1017, 0, rp);
    tm_ndr(&m);
    tm_u64(&m, nid);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rr_u64(reply, 40), 0x1122334455667788ULL);
    CHECK_EQ_INT(rr_i32(reply, 48), 0);
    tm_init(&m, 1019, 0, MACH_PORT_NULL);
    tm_ndr(&m);
    tm_u64(&m, nid);
    tm_u64(&m, 42);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
    rp = test_port_alloc();
    tm_init(&m, 1017, 0, rp);
    tm_ndr(&m);
    tm_u64(&m, nid);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rr_u64(reply, 40), 42);
    CHECK_EQ_INT(rr_i32(reply, 48), 0);
}

static void test_h7_cancel(void) {
    int st = -1;
    int status = -1;
    int check = -1;
    int d0;
    printf("H7 cancel\n");
    notifyd_test_reset();
    (void)do_register_check("com.example.test", 1, &st);
    CHECK_EQ_INT(st, 0);
    do_cancel(1);
    CHECK_EQ_INT(notifyd_test_client_count(), 0);
    d0 = g_del_count;
    do_post_4("com.example.test");
    CHECK_EQ_INT(g_del_count - d0, 0);
    check = do_check(1, &status);
    CHECK_EQ_INT(status, 2);
    CHECK_EQ_INT(check, 0);
}

static void test_h8_suspend_resume(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    mach_port_name_t recv;
    mach_port_name_t send = MACH_PORT_NULL;
    int d0;
    int suspended = -1;
    int pending = -1;
    printf("H8 suspend/post/resume\n");
    notifyd_test_reset();
    rp = test_port_alloc();
    tm_init(&m, 1027, 0, rp);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.susp");
    tm_u32(&m, 9);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_u32(reply, 52), 0);
    recv = rr_u32(reply, 28);
    CHECK(test_gen_send_for_recv(recv, &send) == 1);
    rp = test_port_alloc();
    tm_init(&m, 1004, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, 9);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 36), 0);
    d0 = g_del_count;
    do_post_4("com.example.susp");
    CHECK_EQ_INT(g_del_count - d0, 0);
    CHECK(notifyd_test_client_info(100, 9, NULL, &suspended, &pending,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 1);
    CHECK_EQ_INT(pending, 1);
    rp = test_port_alloc();
    tm_init(&m, 1005, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, 9);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 36), 0);
    CHECK_EQ_INT(g_del_count - d0, 1);
    CHECK_EQ_INT(test_deliveries_to(send, 9), 1);
    CHECK(notifyd_test_client_info(100, 9, NULL, &suspended, &pending,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 0);
    CHECK_EQ_INT(pending, 0);
}

static void test_h9_filtered_checkin(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    printf("H9 filtered checkin\n");
    notifyd_test_reset();
    tm_init(&m, 1028, 0, rp);
    tm_ndr(&m);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(reply_id(reply), 1028 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    CHECK_EQ_U64(rr_u32(reply, 36), 3);
    CHECK_EQ_U64(rr_u32(reply, 40), (uint32_t)getpid());
    CHECK_EQ_INT(rr_i32(reply, 44), 0);
}

static void test_h10_bad_id(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    uint32_t ids[3] = { 1099, 1000, 1001 };
    int k;
    printf("H10 bad id\n");
    notifyd_test_reset();
    for (k = 0; k < 3; k++) {
        rp = test_port_alloc();
        tm_init(&m, ids[k], 0, rp);
        tm_ndr(&m);
        CHECK(tm_deliver(&m, reply, &rsize) == 1);
        CHECK(rsize > 0);
        CHECK_EQ_U64(reply_id(reply), ids[k] + 100);
        CHECK_EQ_INT(reply_retcode(reply), -301);
        CHECK_EQ_U64(rsize, 36);
    }
}

static void test_h11_long_name(void) {
    char name[601];
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp = test_port_alloc();
    printf("H11 long name\n");
    notifyd_test_reset();
    memset(name, 'a', 600);
    name[600] = '\0';
    tm_init(&m, 1012, 0, rp);
    tm_ndr(&m);
    tm_cstring(&m, name);
    tm_u32(&m, 1);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(rsize > 0);
    CHECK_EQ_INT(reply_retcode(reply), -300);
    CHECK(notifyd_test_find_name(name, NULL, NULL, NULL, NULL) == 0);
}

static void test_h13_access(void) {
    int st = -1;
    uint32_t val = 0;
    printf("H13 access control\n");
    notifyd_test_reset();
    CHECK_EQ_INT(do_post_2("com.apple.system.timezone", 501), 7);
    CHECK(notifyd_test_find_name("com.apple.system.timezone", NULL,
                                 NULL, &val, NULL) == 1);
    CHECK_EQ_U64(val, 1);
    CHECK_EQ_INT(do_post_2("com.apple.system.timezone", 0), 0);
    CHECK(notifyd_test_find_name("com.apple.system.timezone", NULL,
                                 NULL, &val, NULL) == 1);
    CHECK_EQ_U64(val, 2);
    (void)do_register_check("com.apple.system.timezone", 21, &st);
    CHECK_EQ_INT(st, 0);
}

static void test_h14_signal(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    int suspended = -1;
    int pending = -1;
    uint32_t val = 0;
    printf("H14 signal client\n");
    notifyd_test_reset();
    tm_init(&m, 1013, 0, MACH_PORT_NULL);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.sig");
    tm_u32(&m, 13);
    tm_u32(&m, 10);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
    do_post_4("com.example.sig");
    CHECK(notifyd_test_client_info(100, 13, NULL, &suspended, &pending,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 0);
    CHECK_EQ_INT(pending, 0);
    CHECK(notifyd_test_find_name("com.example.sig", NULL, NULL, &val,
                                 NULL) == 1);
    CHECK_EQ_U64(val, 2);
}

static void test_h15_file(void) {
    int fds[2];
    mach_port_name_t fp;
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    uint8_t got[4];
    ssize_t n;
    printf("H15 file client\n");
    notifyd_test_reset();
    CHECK(pipe(fds) == 0);
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fp = test_port_alloc();
    CHECK(notifyd_fileport_register(fp, fds[1]) == 1);
    tm_init(&m, 1014, 1, MACH_PORT_NULL);
    tm_desc(&m, fp, MACH_MSG_TYPE_COPY_SEND);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.file");
    tm_u32(&m, 17);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
    do_post_4("com.example.file");
    n = read(fds[0], got, 4);
    CHECK_EQ_INT(n, 4);
    CHECK_EQ_U64(got[0], 0);
    CHECK_EQ_U64(got[1], 0);
    CHECK_EQ_U64(got[2], 0);
    CHECK_EQ_U64(got[3], 17);
    close(fds[0]);
    close(fds[1]);
}

static void test_h16_regenerate(void) {
    int st = -1;
    uint64_t nid;
    uint64_t new_nid = 0;
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    mach_port_name_t dummy;
    uint8_t empty_path[1] = { 0 };
    printf("H16 regenerate\n");
    notifyd_test_reset();
    nid = do_register_check("com.example.regen", 3, &st);
    CHECK_EQ_INT(st, 0);
    rp = test_port_alloc();
    tm_init(&m, 1020, 0, rp);
    tm_ndr(&m);
    tm_u32(&m, 3);
    tm_u64(&m, 111);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 48), 0);
    do_cancel(3);
    dummy = test_port_alloc();
    rp = test_port_alloc();
    tm_init(&m, 1022, 1, rp);
    tm_desc(&m, dummy, MACH_MSG_TYPE_MAKE_SEND);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.regen");
    tm_u32(&m, 3);
    tm_u32(&m, 2);
    tm_u32(&m, 0);
    tm_u32(&m, -1);
    tm_u64(&m, 222);
    tm_u64(&m, 5);
    tm_bytes(&m, empty_path, 0);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(reply_id(reply), 1022 + 100);
    CHECK_EQ_INT(reply_retcode(reply), 0);
    CHECK_EQ_U64(rsize, 56);
    CHECK_EQ_INT(rr_i32(reply, 36), -1);
    /* The cancel freed the name (refcount 0), so the regenerate
     * re-created it with a fresh name id; the reply carries the
     * new id, which is what the state check below must use. */
    new_nid = rr_u64(reply, 40);
    CHECK(new_nid != nid);
    CHECK_EQ_INT(rr_i32(reply, 48), 0);
    rp = test_port_alloc();
    tm_init(&m, 1017, 0, rp);
    tm_ndr(&m);
    tm_u64(&m, new_nid);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rr_u64(reply, 40), 222);
    dummy = test_port_alloc();
    rp = test_port_alloc();
    tm_init(&m, 1022, 1, rp);
    tm_desc(&m, dummy, MACH_MSG_TYPE_MAKE_SEND);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.regen");
    tm_u32(&m, 4);
    tm_u32(&m, 1);
    tm_u32(&m, 0);
    tm_u32(&m, -1);
    tm_u64(&m, 0);
    tm_u64(&m, 0);
    tm_bytes(&m, empty_path, 0);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 48), 6);
    rp = test_port_alloc();
    tm_init(&m, 1022, 1, rp);
    tm_desc(&m, dummy, MACH_MSG_TYPE_MAKE_SEND);
    tm_ndr(&m);
    tm_cstring(&m, "com.example.regen");
    tm_u32(&m, 3);
    tm_u32(&m, 2);
    tm_u32(&m, 0);
    tm_u32(&m, -1);
    tm_u64(&m, 0);
    tm_u64(&m, 0);
    tm_bytes(&m, empty_path, 0);
    tm_u32(&m, 0);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 48), 56);
}

static void test_h17_suspend_pid(void) {
    int st = -1;
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    int suspended = -1;
    printf("H17 suspend_pid\n");
    notifyd_test_reset();
    (void)do_register_check("com.example.pid", 4, &st);
    CHECK_EQ_INT(st, 0);
    tm_init(&m, 1006, 0, MACH_PORT_NULL);
    tm_set_audit(&m, 100, 501, 501, 501);
    tm_ndr(&m);
    tm_u32(&m, 100);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_U64(rsize, 0);
    CHECK(notifyd_test_client_info(100, 4, NULL, &suspended, NULL,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 0);
    tm_init(&m, 1006, 0, MACH_PORT_NULL);
    tm_set_audit(&m, 100, 0, 0, 0);
    tm_ndr(&m);
    tm_u32(&m, 100);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(notifyd_test_client_info(100, 4, NULL, &suspended, NULL,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 1);
    tm_init(&m, 1007, 0, MACH_PORT_NULL);
    tm_set_audit(&m, 100, 0, 0, 0);
    tm_ndr(&m);
    tm_u32(&m, 100);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK(notifyd_test_client_info(100, 4, NULL, &suspended, NULL,
                                   NULL) == 1);
    CHECK_EQ_INT(suspended, 0);
}

static void test_h18_trace(void) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    printf("H18 trace (one dict line on stderr follows)\n");
    notifyd_test_reset();
    /* bsxpc_trace_enabled caches the env value on first read, and
     * the earlier scenarios already cached 0, so clear the cache
     * first; the env is then re-read. */
    bsxpc_set_trace(-1);
    setenv("IOSWINE_SVC_TRACE", "1", 1);
    rp = test_port_alloc();
    tm_init(&m, 1023, 0, rp);
    tm_ndr(&m);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 44), 0);
    unsetenv("IOSWINE_SVC_TRACE");
    bsxpc_set_trace(-1);
    rp = test_port_alloc();
    tm_init(&m, 1023, 0, rp);
    tm_ndr(&m);
    CHECK(tm_deliver(&m, reply, &rsize) == 1);
    CHECK_EQ_INT(rr_i32(reply, 44), 0);
}

/* ------------------------------------------------------------------ */
/* Fuzz (H12) + main.                                                 */
/* ------------------------------------------------------------------ */

static uint32_t fuzz_rand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Build one valid base message into buf; returns its full length
 * including the trailer. */
static uint32_t fuzz_base(int which, uint8_t *buf) {
    test_msg m;
    uint8_t reply[NOTIFYD_MAX_REPLY];
    uint32_t rsize = 0;
    mach_port_name_t rp;
    mach_port_name_t dummy;
    uint8_t empty_path[1] = { 0 };
    switch (which) {
    case 0:
        rp = test_port_alloc();
        tm_init(&m, 1012, 0, rp);
        tm_ndr(&m);
        tm_cstring(&m, "com.example.fuzz");
        tm_u32(&m, 77);
        break;
    case 1:
        tm_init(&m, 1008, 0, test_port_alloc());
        tm_ndr(&m);
        tm_cstring(&m, "com.example.fuzz");
        tm_u32(&m, 0);
        break;
    case 2:
        dummy = test_port_alloc();
        rp = test_port_alloc();
        tm_init(&m, 1022, 1, rp);
        tm_desc(&m, dummy, MACH_MSG_TYPE_MAKE_SEND);
        tm_ndr(&m);
        tm_cstring(&m, "com.example.fuzz");
        tm_u32(&m, 78);
        tm_u32(&m, 2);
        tm_u32(&m, 0);
        tm_u32(&m, -1);
        tm_u64(&m, 9);
        tm_u64(&m, 9);
        tm_bytes(&m, empty_path, 0);
        tm_u32(&m, 0);
        break;
    case 3:
        rp = test_port_alloc();
        tm_init(&m, 1023, 0, rp);
        tm_ndr(&m);
        break;
    default:
        rp = test_port_alloc();
        tm_init(&m, 1027, 0, rp);
        tm_ndr(&m);
        tm_cstring(&m, "com.example.fuzzport");
        tm_u32(&m, 79);
        break;
    }
    {
        uint32_t bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                       MACH_MSG_TYPE_MAKE_SEND_ONCE);
        uint32_t toff;
        if (m.is_complex) bits |= MACH_MSGH_BITS_COMPLEX;
        put_u32(m.buf + 0, bits);
        put_u32(m.buf + 4, m.off);
        put_u32(m.buf + 8, TEST_NOTIFYD_PORT);
        put_u32(m.buf + 12, m.reply_port);
        put_u32(m.buf + 16, MACH_PORT_NULL);
        put_u32(m.buf + 20, m.id);
        toff = (m.off + 3u) & ~3u;
        while (m.off < toff) m.buf[m.off++] = 0;
        put_u32(m.buf + toff + 0, 0);
        put_u32(m.buf + toff + 4, 52);
        put_u32(m.buf + toff + 8, 0);
        put_u32(m.buf + toff + 12, 501);
        put_u32(m.buf + toff + 16, 501);
        put_u32(m.buf + toff + 20, 501);
        put_u32(m.buf + toff + 24, 501);
        put_u32(m.buf + toff + 28, 501);
        put_u32(m.buf + toff + 32, 501);
        put_u32(m.buf + toff + 36, 501);
        put_u32(m.buf + toff + 40, 100);
        put_u32(m.buf + toff + 44, 0);
        put_u32(m.buf + toff + 48, 1);
        memcpy(buf, m.buf, toff + 52);
        (void)reply;
        (void)rsize;
        return toff + 52;
    }
}

static void test_h12_fuzz(void) {
    uint32_t seed = 0x12345678u;
    uint8_t base[4096];
    uint8_t work[4096];
    uint8_t reply[512];
    uint32_t base_len;
    int iter;
    int which;
    printf("H12 fuzz (20000 iterations)\n");
    notifyd_test_reset();
    for (iter = 0; iter < 20000; iter++) {
        uint32_t len;
        uint32_t nmut;
        uint32_t k;
        uint32_t out_size = 0;
        int rc;
        which = (int)(fuzz_rand(&seed) % 5);
        base_len = fuzz_base(which, base);
        len = base_len;
        memcpy(work, base, base_len);
        nmut = 1 + (fuzz_rand(&seed) % 8);
        k = 0;
        while (k < nmut) {
            uint32_t off = fuzz_rand(&seed) % len;
            work[off] = (uint8_t)(fuzz_rand(&seed) & 0xffu);
            k++;
        }
        if ((fuzz_rand(&seed) % 4) == 0) {
            len = fuzz_rand(&seed) % (base_len + 1);
        }
        memset(reply, 0xAA, sizeof(reply));
        rc = notifyd_process_msg((const mach_msg_header_t *)work, len,
                                 reply, NOTIFYD_MAX_REPLY, &out_size);
        CHECK(rc == 0 || rc == 1);
        if (rc == 1) {
            uint32_t j;
            CHECK(out_size <= NOTIFYD_MAX_REPLY);
            j = NOTIFYD_MAX_REPLY;
            while (j < sizeof(reply)) {
                if (reply[j] != 0xAA) break;
                j++;
            }
            CHECK(j == sizeof(reply));
            if (out_size >= 24) {
                CHECK_EQ_U64(reply_id(reply),
                             rr_u32(work, 20) + 100);
            }
        }
    }
}

int main(void) {
    test_h1_init();
    test_h2_register_check();
    test_h3_post_check();
    test_h4_mach_port_3();
    test_h5_common_port();
    test_h6_state();
    test_h7_cancel();
    test_h8_suspend_resume();
    test_h9_filtered_checkin();
    test_h10_bad_id();
    test_h11_long_name();
    test_h13_access();
    test_h14_signal();
    test_h15_file();
    test_h16_regenerate();
    test_h17_suspend_pid();
    test_h18_trace();
    test_h12_fuzz();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("PASS\n");
    return g_failures ? 1 : 0;
}
