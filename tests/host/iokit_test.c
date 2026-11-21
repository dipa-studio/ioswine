/* iokit_test.c: host test for the IOKit + IOSurface emulation
 *
 *
 * A fake client issues the MIG messages IOSurface.framework would
 * send, against the emulation in-process: OSSerializeBinary
 * round-trips, the registry flow (matching, iterator, open), a
 * full surface lifecycle (create 1080x2400 BGRA, map, write,
 * lock/unlock, lookup from a second connection, use counts,
 * release), error shapes, and fuzz. Each scenario prints a PASS
 * line, and a TOTAL line at the end. The process returns nonzero
 * on any failed check.
 *
 * Build (run from ~/workspace/ioswine/iosurface/deliver/tests/host).
 * The test is built with -DIOSURFACE_HOST, so iosurface.c uses the
 * host backing hook below (plain heap buffer) instead of memfd,
 * and neither .c file includes the aarch64-only target linux.h on
 * the x86_64 host (iokit.c includes it only under __aarch64__,
 * iosurface.c only under __aarch64__ && !IOSURFACE_HOST), so no
 * host-shim linux.h is needed for this test. If you build on an
 * aarch64 host, add -Ihost-shim with a copy of
 * fakeboard/deliver/tests/host/host-shim/linux.h.
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -DIOSURFACE_HOST \
 *        -I../../libsystem_kernel/src \
 *        -I../../../../xpc/deliver/libsystem_kernel/src \
 *        -I../../../../work/ioswine/libsystem_kernel/src \
 *        -o /tmp/iokit_test iokit_test.c \
 *        ../../libsystem_kernel/src/iokit.c \
 *        ../../libsystem_kernel/src/iosurface.c
 *     /tmp/iokit_test [fuzz_iterations]
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g -DIOSURFACE_HOST \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I../../libsystem_kernel/src \
 *        -I../../../../xpc/deliver/libsystem_kernel/src \
 *        -I../../../../work/ioswine/libsystem_kernel/src \
 *        -o /tmp/iokit_test_asan iokit_test.c \
 *        ../../libsystem_kernel/src/iokit.c \
 *        ../../libsystem_kernel/src/iosurface.c
 *     /tmp/iokit_test_asan [fuzz_iterations]
 *
 * Host stubs (in this file, following sim_test.c): ipc_iokit_port_
 * create returns incrementing fake port names starting at 0x100;
 * lsk_getenv reads the real environment; iosurface_host_alloc/free
 * use a plain heap buffer (fd -1: no fd exists on the host).
 *
 * WIRE NOTE: the request builder below follows the real 64-bit
 * Mach descriptor layout (xnu osfmk/mach/message.h,
 * mach_msg_ool_descriptor64_t; the in-tree ipc.h structs agree):
 * port descriptors are 12 bytes (name at slot+0, disposition at
 * slot+10, type 0 at slot+11) and OOL descriptors are 16 bytes
 * (address at slot+0, deallocate at slot+8, copy at slot+9, type
 * 1 at slot+11, size at slot+12). An earlier version of iokit.c
 * read the type at slot+15 and the port disposition at slot+14
 * and advanced 16 bytes per descriptor; the host test caught
 * that (a 40-byte OOL blob came back as size 0x01000028, and a
 * real port descriptor would push the body pointer 4 bytes
 * late). Both the implementation and this builder were fixed on
 * 2026-11-12 to the layout above; the reply parser reads reply
 * descriptors the same way (type at slot+11, port slots 12
 * bytes, OOL slots 16 bytes, OOL size at slot+12).
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "iokit.h"
#include "iosurface.h"

/* ------------------------------------------------------------------ */
/* Host stubs for the ipc_* / platform symbols the .c files need.     */
/* ------------------------------------------------------------------ */

/* lsk_getenv, as defined by init.c on the target. On the host this
 * stub reads the real environment, so IOSWINE_SVC_TRACE=1 works. */
const char *lsk_getenv(const char *name) {
    return getenv(name);
}

/* ipc_iokit_port_create: the ipc.c hook (see iokit.h) does not
 * exist yet, so the host test hands out incrementing fake names.
 * 0x100 is the first IOKit port; the test's own reply/owning-task
 * ports use 0x500 and up, so the two ranges never collide. */
static mach_port_name_t g_next_fake_port = 0x100;
mach_port_name_t ipc_iokit_port_create(void) {
    mach_port_name_t p = g_next_fake_port;
    g_next_fake_port++;
    if (g_next_fake_port == 0) g_next_fake_port = 0x100;
    return p;
}

/* iosurface_host_alloc/free: plain heap buffer on the host.
 * The fd is -1 (no memfd exists here); the pointer is the buffer.
 * The buffer is zero-filled, like a fresh memfd. */
int iosurface_host_alloc(uint64_t size, int *fd_out, void **ptr_out) {
    void *p;
    if (!fd_out || !ptr_out) return -1;
    if (size == 0 || size > 0x40000000ULL) return -1;
    p = malloc((size_t)size);
    if (!p) return -1;
    memset(p, 0, (size_t)size);
    *fd_out = -1;
    *ptr_out = p;
    return 0;
}

void iosurface_host_free(int fd, void *ptr, uint64_t size) {
    (void)fd;
    (void)size;
    free(ptr);
}

/* ------------------------------------------------------------------ */
/* Harness.                                                            */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

/* Per-scenario verdict: record the failure count at scenario
 * entry; the scenario passes when no new failure appeared. (A
 * global check would print FAIL for every later scenario after
 * the first failure anywhere.) */
static int g_scen_failures = 0;
#define SCEN_BEGIN() do { g_scen_failures = g_failures; } while (0)
#define SCEN_END() \
    printf("%s\n", g_failures == g_scen_failures ? "PASS" : "FAIL")

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

#define CHECK_RC(rc) CHECK((rc) == 0)

/* Deterministic xorshift PRNG (same shape as the xpcwire test). */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* ------------------------------------------------------------------ */
/* Little-endian helpers and osser pool setup.                         */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)(v & 0xffffffffu));
    wr32(p + 4, (uint32_t)(v >> 32));
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* One osser pool, sized per the header's sizing rule. The pool
 * arrays must stay alive while a decoded tree is used. */
static osser_value g_pool_vals[128];
static osser_key g_pool_keys[128];
static osser_value *g_pool_objs[128];
static uint8_t g_pool_text[2048];

static void pool_reset(osser_pool *pool) {
    pool->vals = g_pool_vals;
    pool->vals_cap = 128;
    pool->vals_used = 0;
    pool->keys = g_pool_keys;
    pool->keys_cap = 128;
    pool->keys_used = 0;
    pool->objs = g_pool_objs;
    pool->objs_cap = 128;
    pool->objs_used = 0;
    pool->text = g_pool_text;
    pool->text_cap = 2048;
    pool->text_used = 0;
}

/* Decode a blob into the shared pool. Returns the decode result. */
static int pool_decode(const uint8_t *buf, uint32_t len,
                       osser_value **root_out) {
    osser_pool pool;
    pool_reset(&pool);
    return osser_decode(buf, len, &pool, root_out);
}

/* Decode with auto-detection into the shared pool. */
static int pool_decode_auto(const uint8_t *buf, uint32_t len,
                            osser_value **root_out) {
    osser_pool pool;
    pool_reset(&pool);
    return osser_decode_auto(buf, len, &pool, root_out);
}

/* Find a numeric dict entry and compare it to want. */
static int dict_num_eq(const osser_value *dict, const char *key,
                       uint64_t want) {
    const osser_value *v = NULL;
    uint64_t n = 0;
    if (osser_dict_find(dict, key, &v) != OSSER_ERR_OK) return 0;
    if (osser_as_uint64(v, &n) != OSSER_ERR_OK) return 0;
    return n == want;
}

/* Find a string dict entry and compare it to want. */
static int dict_str_eq(const osser_value *dict, const char *key,
                       const char *want) {
    const osser_value *v = NULL;
    const uint8_t *s = NULL;
    uint32_t len = 0;
    uint32_t i;
    if (osser_dict_find(dict, key, &v) != OSSER_ERR_OK) return 0;
    if (osser_as_string(v, &s, &len) != OSSER_ERR_OK) return 0;
    if (len != (uint32_t)strlen(want)) return 0;
    for (i = 0; i < len; i++) {
        if (s[i] != (uint8_t)want[i]) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* MIG client: request builder and reply parser.                       */
/*                                                                     */
/* Request layout: 24-byte header, optional descriptor body
 * (u32 count, then port descriptors of 12 bytes and OOL
 * descriptors of 16 bytes; port descriptors carry the name at
 * slot+0, the disposition at slot+10 and the type (0) at
 * slot+11; OOL descriptors carry the address at slot+0, the
 * deallocate flag at slot+8, the copy flag at slot+9, the type
 * (1) at slot+11 and the size at slot+12), then the 8-byte NDR
 * record [0,0,0,0,1,0,0,0], then the routine fields.
 *
 * Reply layout: 24-byte header, optional descriptor body (u32
 * count, then 12-byte port slots and 16-byte OOL slots, type
 * byte at slot+11, OOL size at slot+12), then the NDR record,
 * then RetCode (u32), then the out parameters. Reply id =
 * request + 100.
 */
/* ------------------------------------------------------------------ */

#define TEST_REPLY_PORT 0x500u
#define TEST_TASK_PORT  0x777u

typedef struct {
    uint32_t kind; /* 0 = port descriptor, 1 = OOL descriptor */
    uint32_t name; /* port descriptor: port name */
    uint32_t disp; /* port descriptor: disposition */
    uint64_t addr; /* OOL descriptor: address */
    uint32_t size; /* OOL descriptor: byte size */
} mig_desc;

typedef struct {
    uint32_t id;         /* reply msgh_id */
    uint32_t retcode;    /* RetCode */
    uint32_t n_descs;
    uint32_t port_name[4]; /* port descriptor names, in order */
    uint64_t ool_addr[2];  /* OOL descriptor addresses, in order */
    uint32_t ool_size[2];  /* OOL descriptor sizes, in order */
    const uint8_t *payload; /* first byte after RetCode */
    uint32_t payload_len;
} mig_reply;

/* Build one MIG request. Returns the total size, or 0 when the
 * buffer is too small. */
static uint32_t build_mig_msg(uint8_t *buf, uint32_t cap,
                              mach_port_name_t dest, uint32_t id,
                              const mig_desc *descs, uint32_t ndescs,
                              const uint8_t *body, uint32_t body_len) {
    uint32_t total = 24u + 8u + body_len;
    uint32_t pos = 24;
    uint32_t i;
    if (ndescs > 0) {
        total += 4u; /* descriptor count */
        for (i = 0; i < ndescs; i++)
            total += (descs[i].kind == 0 ? 12u : 16u);
    }
    if (cap < total) return 0;
    wr32(buf + 0, ndescs > 0 ? 0x80001513u : 0x1513u);
    wr32(buf + 4, total);
    wr32(buf + 8, dest);
    wr32(buf + 12, TEST_REPLY_PORT);
    wr32(buf + 16, 0);
    wr32(buf + 20, id);
    if (ndescs > 0) {
        wr32(buf + pos, ndescs);
        pos += 4;
        for (i = 0; i < ndescs; i++) {
            uint8_t *d = buf + pos;
            if (descs[i].kind == 0) {
                memset(d, 0, 12);
                wr32(d + 0, descs[i].name);
                d[10] = (uint8_t)descs[i].disp;
                d[11] = 0; /* MACH_MSG_PORT_DESCRIPTOR */
                pos += 12;
            } else {
                memset(d, 0, 16);
                wr64(d + 0, descs[i].addr);
                d[8] = 0; /* deallocate */
                d[9] = 1; /* copy */
                d[11] = 1; /* MACH_MSG_OOL_DESCRIPTOR */
                wr32(d + 12, descs[i].size);
                pos += 16;
            }
        }
    }
    /* NDR record: int_rep 1, the rest 0. */
    memset(buf + pos, 0, 8);
    buf[pos + 4] = 1;
    pos += 8;
    if (body_len > 0) memcpy(buf + pos, body, body_len);
    return total;
}

/* Parse one MIG reply. Returns 0 on success, -1 on a malformed
 * reply. The payload and OOL addresses point into buf (for OOL
 * the address is a host pointer the test may dereference). */
static int parse_mig_reply(const uint8_t *buf, uint32_t size,
                           uint32_t req_id, mig_reply *out) {
    uint32_t ndr_off;
    uint32_t i;
    memset(out, 0, sizeof(*out));
    if (size < 36) return -1;
    out->id = rd32(buf + 20);
    if (out->id != req_id + 100) return -1;
    if (rd32(buf + 0) & 0x80000000u) {
        uint32_t off = 28;
        out->n_descs = rd32(buf + 24);
        if (out->n_descs > 4) return -1;
        for (i = 0; i < out->n_descs; i++) {
            const uint8_t *d;
            if (size < off + 12) return -1;
            d = buf + off;
            if (d[11] == 0) {
                out->port_name[i] = rd32(d + 0);
                off += 12;
            } else if (d[11] == 1) {
                /* OOL slots are counted separately; record them
                 * in the first free OOL slot. */
                uint32_t j = 0;
                if (size < off + 16) return -1;
                while (j < 2 && out->ool_size[j] != 0) j++;
                if (j >= 2) return -1;
                out->ool_addr[j] = rd64(d + 0);
                out->ool_size[j] = rd32(d + 12);
                off += 16;
            } else {
                return -1;
            }
        }
        ndr_off = off; /* first byte after the descriptors: NDR */
    } else {
        ndr_off = 24;
    }
    if (size < ndr_off + 12) return -1;
    out->retcode = rd32(buf + ndr_off + 8);
    out->payload = buf + ndr_off + 12;
    out->payload_len = size - (ndr_off + 12);
    return 0;
}

/* Deliver one built message to iokit_process_msg. The request
 * buffer and reply buffer are the caller's; the reply is parsed
 * into *rep_out when rep_out is not NULL. Returns the entry's
 * return value (0 or 1). */
static int mig_deliver(const uint8_t *msg, uint32_t msg_len,
                       uint8_t *reply_buf, uint32_t reply_cap,
                       uint32_t req_id, mig_reply *rep_out,
                       uint32_t *reply_size_out) {
    uint32_t reply_size = 0;
    int rc = iokit_process_msg((const mach_msg_header_t *)msg, msg_len,
                               reply_buf, reply_cap, &reply_size);
    if (reply_size_out) *reply_size_out = reply_size;
    if (rc != 1) return rc;
    if (reply_size == 0) return 1; /* one-way: no reply port */
    if (!rep_out) return 1;
    if (parse_mig_reply(reply_buf, reply_size, req_id, rep_out) != 0) {
        return -1;
    }
    return 1;
}

/* Build a counted-string body (u32 count including the NUL, then
 * the bytes, no padding: mig_cur_string does not skip padding). */
static uint32_t build_string_body(uint8_t *buf, uint32_t cap,
                                  const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (cap < 4 + len) return 0;
    wr32(buf + 0, len);
    memcpy(buf + 4, s, len);
    return 4 + len;
}

/* One routine call with no descriptors and a raw body. Fills
 * *rep_out (when not NULL) and returns the deliver result. */
static int mig_call(mach_port_name_t dest, uint32_t id,
                    const uint8_t *body, uint32_t body_len,
                    mig_reply *rep_out, uint32_t *reply_size_out) {
    uint8_t msg[IOKIT_MAX_MIG_MSG];
    static uint8_t reply[IOKIT_MAX_REPLY];
    uint32_t msg_len = build_mig_msg(msg, sizeof(msg), dest, id,
                                     NULL, 0, body, body_len);
    if (msg_len == 0) return -1;
    return mig_deliver(msg, msg_len, reply, sizeof(reply), id,
                       rep_out, reply_size_out);
}

/* One routine call with descriptors. */
static int mig_call_descs(mach_port_name_t dest, uint32_t id,
                          const mig_desc *descs, uint32_t ndescs,
                          const uint8_t *body, uint32_t body_len,
                          mig_reply *rep_out, uint32_t *reply_size_out) {
    uint8_t msg[IOKIT_MAX_MIG_MSG];
    static uint8_t reply[IOKIT_MAX_REPLY];
    uint32_t msg_len = build_mig_msg(msg, sizeof(msg), dest, id,
                                     descs, ndescs, body, body_len);
    if (msg_len == 0) return -1;
    return mig_deliver(msg, msg_len, reply, sizeof(reply), id,
                       rep_out, reply_size_out);
}

/* Read a u32 out of a reply payload at offset off. */
static int rep_u32(const mig_reply *rep, uint32_t off, uint32_t *out) {
    if (off + 4 > rep->payload_len) return -1;
    *out = rd32(rep->payload + off);
    return 0;
}

/* Read a u64 out of a reply payload at offset off. */
static int rep_u64(const mig_reply *rep, uint32_t off, uint64_t *out) {
    if (off + 8 > rep->payload_len) return -1;
    *out = rd64(rep->payload + off);
    return 0;
}

/* ------------------------------------------------------------------ */
/* io_connect_method (2865) client.                                    */
/*                                                                     */
/* Request body: selector u32, scalar_in_cnt u32, scalars (u64       */
/* each), struct_in_len u32, struct bytes padded to 4, ool_in u64,   */
/* ool_in_size u64, inband_outputCnt u32, scalar_outputCnt u32,     */
/* ool_output u64, ool_output_size u64. Reply payload: RetCode,     */
/* inband_outputCnt u32, inband bytes (NOT padded),                  */
/* scalar_outputCnt u32, scalars (u64 each), ool_output_size u64.   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t retcode;
    const uint8_t *inband;
    uint32_t inband_len;
    uint64_t scalars[IOKIT_MAX_SCALARS];
    uint32_t scalar_cnt;
    uint64_t ool_size;
} method_result;

static uint32_t build_method_body(uint8_t *buf, uint32_t cap,
                                  uint32_t selector,
                                  const uint64_t *scalars,
                                  uint32_t nscalars,
                                  const uint8_t *struct_in,
                                  uint32_t struct_len,
                                  uint32_t struct_out_cap,
                                  uint32_t scalar_out_cap) {
    uint32_t pos = 0;
    uint32_t pad;
    uint32_t i;
    uint32_t need = 8u + 8u * nscalars + 4u + struct_len +
                    ((4u - (struct_len & 3u)) & 3u) + 16u + 16u;
    if (cap < need) return 0;
    wr32(buf + pos, selector);
    pos += 4;
    wr32(buf + pos, nscalars);
    pos += 4;
    for (i = 0; i < nscalars; i++) {
        wr64(buf + pos, scalars[i]);
        pos += 8;
    }
    wr32(buf + pos, struct_len);
    pos += 4;
    if (struct_len > 0) {
        memcpy(buf + pos, struct_in, struct_len);
        pos += struct_len;
    }
    pad = (4u - (struct_len & 3u)) & 3u;
    memset(buf + pos, 0, pad);
    pos += pad;
    wr64(buf + pos, 0); /* ool_in */
    pos += 8;
    wr64(buf + pos, 0); /* ool_in_size */
    pos += 8;
    wr32(buf + pos, struct_out_cap); /* inband_outputCnt */
    pos += 4;
    wr32(buf + pos, scalar_out_cap); /* scalar_outputCnt */
    pos += 4;
    wr64(buf + pos, 0); /* ool_output */
    pos += 8;
    wr64(buf + pos, 0); /* ool_output_size */
    pos += 8;
    return pos;
}

/* Call io_connect_method on conn and parse the reply into res.
 * Returns the deliver result (1 on a parsed reply, -1 on a
 * malformed reply, 0 when no reply could be built). */
/* A method reply is RetCode plus the output fields. A handler
 * that returns early (unknown connection, bad destination) builds
 * a bare reply with NO output fields at all: an empty payload is
 * a valid method reply, not a malformed one. */
static int call_method(mach_port_name_t conn, uint32_t selector,
                       const uint64_t *scalars, uint32_t nscalars,
                       const uint8_t *struct_in, uint32_t struct_len,
                       uint32_t struct_out_cap, uint32_t scalar_out_cap,
                       method_result *res) {
    uint8_t body[IOKIT_MAX_MIG_MSG];
    uint32_t body_len;
    mig_reply rep;
    uint32_t pos = 0;
    uint32_t i;
    int rc;
    memset(res, 0, sizeof(*res));
    body_len = build_method_body(body, sizeof(body), selector,
                                 scalars, nscalars, struct_in,
                                 struct_len, struct_out_cap,
                                 scalar_out_cap);
    if (body_len == 0) return -1;
    rc = mig_call(conn, IOKIT_MIG_CONNECT_METHOD, body, body_len,
                  &rep, NULL);
    if (rc != 1) return rc;
    res->retcode = rep.retcode;
    if (rep.payload_len == 0) return 1; /* bare error reply */
    if (rep.payload_len < 4) return -1;
    res->inband_len = rd32(rep.payload + pos);
    pos += 4;
    if (pos + res->inband_len > rep.payload_len) return -1;
    res->inband = rep.payload + pos;
    pos += res->inband_len;
    if (pos + 4 > rep.payload_len) return -1;
    res->scalar_cnt = rd32(rep.payload + pos);
    pos += 4;
    if (res->scalar_cnt > IOKIT_MAX_SCALARS) return -1;
    if (pos + 8u * res->scalar_cnt > rep.payload_len) return -1;
    for (i = 0; i < res->scalar_cnt; i++) {
        res->scalars[i] = rd64(rep.payload + pos);
        pos += 8;
    }
    if (pos + 8 > rep.payload_len) return -1;
    res->ool_size = rd64(rep.payload + pos);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Scenario 1: OSSerializeBinary round-trip, worked vector, XML,     */
/* malformed blobs.                                                    */
/* ------------------------------------------------------------------ */

/* Part 1 section 2.3 worked example: {"AAAA": 0x193, "BBBB": 0x193}.
 * 44 bytes, little-endian. Note the EndCollection bit on the
 * dictionary tag and on the first value's tag. */
static const uint8_t g_worked_vec[] = {
    0xD3, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x81,
    0x04, 0x00, 0x00, 0x09,
    0x41, 0x41, 0x41, 0x41,
    0x20, 0x00, 0x00, 0x84,
    0x93, 0x01, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x09,
    0x42, 0x42, 0x42, 0x42,
    0x20, 0x00, 0x00, 0x04,
    0x93, 0x01, 0x00, 0x00
};

/* The 1080x2400 BGRA create properties as an XML plist, the form
 * IOSurface.framework sends (Part 1 section 2.4). PixelFormat
 * 1095911234 is 0x41524742 ("BGRA"). AllocSize 10368000 is
 * 2400 * 4320, the pre-alignment row size; the emulation grows it
 * to cover the 64-byte aligned stride (see scenario 3). */
static const char g_create_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>Width</key><integer>1080</integer>\n"
    "<key>Height</key><integer>2400</integer>\n"
    "<key>BytesPerRow</key><integer>4320</integer>\n"
    "<key>BytesPerElement</key><integer>4</integer>\n"
    "<key>PixelFormat</key><integer>1095911234</integer>\n"
    "<key>AllocSize</key><integer>10368000</integer>\n"
    "<key>CacheMode</key><integer>0</integer>\n"
    "<key>IOSurfaceClass</key><string>IOSurface</string>\n"
    "</dict>\n"
    "</plist>\n";

static void scen_1_osser(void) {
    osser_value *root = NULL;
    uint8_t blob[1024];
    osser_writer w;
    int rc;
    printf("SCENARIO 1: OSSerializeBinary round-trip + XML ... ");
    SCEN_BEGIN();
    /* Worked vector: decode and check every value. */
    rc = pool_decode(g_worked_vec, sizeof(g_worked_vec), &root);
    CHECK(rc == OSSER_ERR_OK);
    if (rc == OSSER_ERR_OK && root) {
        CHECK(root->type == OSSER_DICT);
        CHECK(root->count == 2);
        CHECK(dict_num_eq(root, "AAAA", 0x193));
        CHECK(dict_num_eq(root, "BBBB", 0x193));
    }
    /* Writer round-trip: every scalar and container type. */
    rc = osser_w_init(&w, blob, sizeof(blob));
    CHECK(rc == OSSER_ERR_OK);
    CHECK(osser_w_begin_dict(&w, 7) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Name") == OSSER_ERR_OK);
    CHECK(osser_w_string(&w, "IOSurfaceRoot") == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Count") == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 1080) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Big") == OSSER_ERR_OK);
    CHECK(osser_w_u64(&w, 0x1122334455667788ULL) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Flag") == OSSER_ERR_OK);
    CHECK(osser_w_bool(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Nested") == OSSER_ERR_OK);
    CHECK(osser_w_begin_dict(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Inner") == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 7) == OSSER_ERR_OK);
    CHECK(osser_w_end_dict(&w) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "List") == OSSER_ERR_OK);
    CHECK(osser_w_begin_array(&w, 3) == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 2) == OSSER_ERR_OK);
    CHECK(osser_w_string(&w, "x") == OSSER_ERR_OK);
    CHECK(osser_w_end_array(&w) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "Blob") == OSSER_ERR_OK);
    {
        static const uint8_t data3[3] = {0x01, 0x02, 0x03};
        CHECK(osser_w_data(&w, data3, 3) == OSSER_ERR_OK);
    }
    CHECK(osser_w_end_dict(&w) == OSSER_ERR_OK);
    CHECK(osser_w_error(&w) == OSSER_ERR_OK);
    CHECK(osser_w_len(&w) > 4);
    CHECK(rd32(blob) == OSSER_SIGNATURE);
    rc = pool_decode(blob, osser_w_len(&w), &root);
    CHECK(rc == OSSER_ERR_OK);
    if (rc == OSSER_ERR_OK && root) {
        const osser_value *v = NULL;
        const uint8_t *s = NULL;
        uint32_t slen = 0;
        uint64_t n = 0;
        int b = 0;
        CHECK(root->type == OSSER_DICT);
        CHECK(root->count == 7);
        CHECK(dict_str_eq(root, "Name", "IOSurfaceRoot"));
        CHECK(dict_num_eq(root, "Count", 1080));
        CHECK(dict_num_eq(root, "Big", 0x1122334455667788ULL));
        CHECK(osser_dict_find(root, "Flag", &v) == OSSER_ERR_OK);
        if (v) {
            CHECK(osser_as_bool(v, &b) == OSSER_ERR_OK);
            CHECK(b == 1);
        }
        CHECK(osser_dict_find(root, "Nested", &v) == OSSER_ERR_OK);
        if (v) CHECK(dict_num_eq(v, "Inner", 7));
        CHECK(osser_dict_find(root, "List", &v) == OSSER_ERR_OK);
        if (v) {
            CHECK(v->type == OSSER_ARRAY);
            CHECK(v->count == 3);
            CHECK(osser_as_uint64(&v->items[0], &n) == OSSER_ERR_OK);
            CHECK(n == 1);
            CHECK(osser_as_uint64(&v->items[1], &n) == OSSER_ERR_OK);
            CHECK(n == 2);
            CHECK(osser_as_string(&v->items[2], &s, &slen) ==
                  OSSER_ERR_OK);
            CHECK(slen == 1 && s[0] == 'x');
        }
        CHECK(osser_dict_find(root, "Blob", &v) == OSSER_ERR_OK);
        if (v) {
            CHECK(v->type == OSSER_DATA);
            CHECK(v->len == 3);
            CHECK(v->bytes[0] == 0x01 && v->bytes[2] == 0x03);
        }
        /* A missing key is NOT_FOUND, and a typed getter on the
         * wrong type is BAD_TYPE. */
        CHECK(osser_dict_find(root, "Nope", &v) == OSSER_ERR_NOT_FOUND);
        CHECK(osser_dict_find(root, "Name", &v) == OSSER_ERR_OK);
        if (v) CHECK(osser_as_uint64(v, &n) == OSSER_ERR_BAD_TYPE);
        /* Negative numbers round-trip as bit patterns. */
        CHECK(osser_w_init(&w, blob, sizeof(blob)) == OSSER_ERR_OK);
        CHECK(osser_w_s64(&w, -42) == OSSER_ERR_OK);
        rc = pool_decode(blob, osser_w_len(&w), &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            int64_t sn = 0;
            CHECK(osser_as_int64(root, &sn) == OSSER_ERR_OK);
            CHECK(sn == -42);
        }
    }
    /* Writer error paths: the count is enforced at write time
     * (osser_w_value_ok fails once remaining hits 0), and
     * end_dict fails when entries are missing. */
    CHECK(osser_w_init(&w, blob, sizeof(blob)) == OSSER_ERR_OK);
    CHECK(osser_w_begin_dict(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "A") == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "B") != OSSER_ERR_OK); /* count exceeded */
    CHECK(osser_w_init(&w, blob, sizeof(blob)) == OSSER_ERR_OK);
    CHECK(osser_w_begin_dict(&w, 2) == OSSER_ERR_OK);
    CHECK(osser_w_key(&w, "A") == OSSER_ERR_OK);
    CHECK(osser_w_u32(&w, 1) == OSSER_ERR_OK);
    CHECK(osser_w_end_dict(&w) != OSSER_ERR_OK); /* one entry short */
    CHECK(osser_w_init(&w, blob, 4) == OSSER_ERR_OK);
    CHECK(osser_w_begin_dict(&w, 1) != OSSER_ERR_OK);
    /* XML decode of the create plist, then binary re-encode. */
    {
        osser_pool pool;
        osser_value *xroot = NULL;
        uint8_t bin[1024];
        osser_writer w2;
        osser_value *broot = NULL;
        pool_reset(&pool);
        rc = osser_xml_decode((const uint8_t *)g_create_xml,
                              (uint32_t)strlen(g_create_xml), &pool,
                              &xroot);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && xroot) {
            CHECK(xroot->type == OSSER_DICT);
            CHECK(dict_num_eq(xroot, "Width", 1080));
            CHECK(dict_num_eq(xroot, "Height", 2400));
            CHECK(dict_num_eq(xroot, "BytesPerRow", 4320));
            CHECK(dict_num_eq(xroot, "PixelFormat", 1095911234u));
            CHECK(dict_num_eq(xroot, "AllocSize", 10368000));
            CHECK(dict_str_eq(xroot, "IOSurfaceClass", "IOSurface"));
            CHECK(osser_w_init(&w2, bin, sizeof(bin)) == OSSER_ERR_OK);
            CHECK(osser_w_value(&w2, xroot) == OSSER_ERR_OK);
            CHECK(osser_w_len(&w2) > 4);
            rc = pool_decode(bin, osser_w_len(&w2), &broot);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && broot) {
                CHECK(dict_num_eq(broot, "Width", 1080));
                CHECK(dict_num_eq(broot, "Height", 2400));
                CHECK(dict_num_eq(broot, "PixelFormat", 1095911234u));
            }
        }
        /* Auto-detect picks the binary path for a binary blob
         * and the XML path for a plist. */
        rc = pool_decode_auto(g_worked_vec, sizeof(g_worked_vec),
                              &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            CHECK(dict_num_eq(root, "AAAA", 0x193));
        }
        rc = pool_decode_auto((const uint8_t *)g_create_xml,
                              (uint32_t)strlen(g_create_xml), &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            CHECK(dict_num_eq(root, "Width", 1080));
        }
        /* A real decodes from XML but has no binary form. */
        {
            static const char real_xml[] =
                "<plist version=\"1.0\"><dict>"
                "<key>Scale</key><real>2.5</real>"
                "</dict></plist>";
            osser_value *rroot = NULL;
            double d = 0.0;
            pool_reset(&pool);
            rc = osser_xml_decode((const uint8_t *)real_xml,
                                  (uint32_t)strlen(real_xml), &pool,
                                  &rroot);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && rroot) {
                const osser_value *rv = NULL;
                CHECK(osser_dict_find(rroot, "Scale", &rv) ==
                      OSSER_ERR_OK);
                if (rv) {
                    CHECK(osser_as_double(rv, &d) == OSSER_ERR_OK);
                    CHECK(d == 2.5);
                }
                CHECK(osser_w_init(&w2, bin, sizeof(bin)) ==
                      OSSER_ERR_OK);
                CHECK(osser_w_value(&w2, rroot) == OSSER_ERR_BAD_TYPE);
            }
        }
    }
    /* Malformed blobs fail cleanly, never crash. */
    {
        static const uint8_t bad_sig[4] = {0x00, 0x00, 0x00, 0x00};
        static const uint8_t bad_tag[8] = {
            0xD3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05
        };
        /* dict(1), key "A", object reference 99 (out of range). */
        static const uint8_t bad_ref[] = {
            0xD3, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x81,
            0x01, 0x00, 0x00, 0x08,
            0x41, 0x00, 0x00, 0x00,
            0x63, 0x00, 0x00, 0x8c
        };
        /* dict(1), key "A", number claiming 0x800 bits (the
         * CVE-2016-4655 shape): the reader rejects non-32/64
         * bit lengths instead of over-reading. */
        static const uint8_t big_num[] = {
            0xD3, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x81,
            0x01, 0x00, 0x00, 0x08,
            0x41, 0x00, 0x00, 0x00,
            0x00, 0x08, 0x00, 0x84,
            0x00, 0x00, 0x00, 0x00
        };
        static const char bad_xml[] =
            "<plist version=\"1.0\"><dict><key>X</key></dict></plist>";
        rc = pool_decode(bad_sig, sizeof(bad_sig), &root);
        CHECK(rc == OSSER_ERR_BAD_SIG);
        rc = pool_decode(g_worked_vec, 20, &root);
        CHECK(rc != OSSER_ERR_OK);
        rc = pool_decode(bad_tag, sizeof(bad_tag), &root);
        CHECK(rc == OSSER_ERR_BAD_TAG);
        rc = pool_decode(bad_ref, sizeof(bad_ref), &root);
        CHECK(rc == OSSER_ERR_BAD_REF);
        rc = pool_decode(big_num, sizeof(big_num), &root);
        CHECK(rc == OSSER_ERR_BAD_TAG);
        rc = pool_decode(NULL, 0, &root);
        CHECK(rc != OSSER_ERR_OK);
        {
            osser_pool pool;
            osser_value *xroot = NULL;
            pool_reset(&pool);
            rc = osser_xml_decode((const uint8_t *)bad_xml,
                                  (uint32_t)strlen(bad_xml), &pool,
                                  &xroot);
            CHECK(rc != OSSER_ERR_OK);
            rc = osser_xml_decode(NULL, 0, &pool, &xroot);
            CHECK(rc != OSSER_ERR_OK);
        }
    }
    SCEN_END();
}

/* ------------------------------------------------------------------ */
/* Scenario 2: MIG registry flow.                                      */
/* ------------------------------------------------------------------ */

static mach_port_name_t g_master = MACH_PORT_NULL;
static mach_port_name_t g_root_port = MACH_PORT_NULL;
static mach_port_name_t g_conn1 = MACH_PORT_NULL;
static mach_port_name_t g_conn2 = MACH_PORT_NULL;

/* The 2804 matching dict as an XML plist (the original routine
 * takes an io_string_t, max 512 bytes). */
static const char g_match_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "<key>IOProviderClass</key><string>IOSurfaceRoot</string>\n"
    "</dict>\n"
    "</plist>\n";

static const char g_match_empty_xml[] =
    "<plist version=\"1.0\"><dict></dict></plist>";

static const char g_match_none_xml[] =
    "<plist version=\"1.0\"><dict>"
    "<key>IOProviderClass</key><string>NoSuchClass</string>"
    "</dict></plist>";

/* Read a counted string out of a reply payload (mig_rep_string
 * form: u32 count incl NUL, bytes, NUL, pad to 4). */
static int rep_string(const mig_reply *rep, char *out, uint32_t cap) {
    uint32_t count = 0;
    uint32_t copy;
    if (rep_u32(rep, 0, &count) != 0) return -1;
    if (count == 0 || count > rep->payload_len - 4) return -1;
    copy = count;
    if (copy > cap - 1) copy = cap - 1;
    memcpy(out, rep->payload + 4, copy);
    out[copy] = '\0';
    return 0;
}

static void scen_2_mig_registry(void) {
    mig_reply rep;
    int rc;
    uint8_t body[1024];
    uint32_t body_len;
    printf("SCENARIO 2: MIG registry flow ... ");
    SCEN_BEGIN();
    rc = iosurface_init();
    CHECK(rc == KERN_SUCCESS);
    g_master = iokit_master_port();
    CHECK(g_master != MACH_PORT_NULL);
    g_root_port = iosurface_root_port();
    CHECK(g_root_port != MACH_PORT_NULL);
    CHECK(g_root_port != g_master);
    /* 2804 get_matching_services with the XML dict: one match. */
    body_len = build_string_body(body, sizeof(body), g_match_xml);
    CHECK(body_len > 0);
    rc = mig_call(g_master, IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES,
                  body, body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    CHECK(rep.n_descs == 1);
    {
        mach_port_name_t iter = rep.port_name[0];
        mach_port_name_t svc;
        CHECK(iter != MACH_PORT_NULL);
        /* 2800 get_class on the iterator: "IOIterator". */
        rc = mig_call(iter, IOKIT_MIG_OBJECT_GET_CLASS, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        {
            char cls[64];
            CHECK(rep_string(&rep, cls, sizeof(cls)) == 0);
            CHECK(streq(cls, "IOIterator"));
        }
        /* 2802 iterator_next: the IOSurfaceRoot service, then
         * NULL when exhausted. */
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        CHECK(rep.n_descs == 1);
        svc = rep.port_name[0];
        CHECK(svc == g_root_port);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        CHECK(rep.port_name[0] == MACH_PORT_NULL);
        /* 2834 iterator_is_valid is now false; 2803 reset brings
         * the service back. */
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_IS_VALID, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        {
            uint32_t valid = 99;
            CHECK(rep_u32(&rep, 0, &valid) == 0);
            CHECK(valid == 0);
        }
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_RESET, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.port_name[0] == g_root_port);
    }
    /* 2800 get_class on the root service, and 2801 conforms_to. */
    rc = mig_call(g_root_port, IOKIT_MIG_OBJECT_GET_CLASS, NULL, 0,
                  &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    {
        char cls[64];
        CHECK(rep_string(&rep, cls, sizeof(cls)) == 0);
        CHECK(streq(cls, "IOSurfaceRoot"));
    }
    body_len = build_string_body(body, sizeof(body), "IOSurfaceRoot");
    rc = mig_call(g_root_port, IOKIT_MIG_OBJECT_CONFORMS_TO, body,
                  body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    {
        uint32_t yes = 99;
        CHECK(rep_u32(&rep, 0, &yes) == 0);
        CHECK(yes == 1);
    }
    body_len = build_string_body(body, sizeof(body), "IOService");
    rc = mig_call(g_root_port, IOKIT_MIG_OBJECT_CONFORMS_TO, body,
                  body_len, &rep, NULL);
    CHECK(rc == 1);
    {
        uint32_t yes = 99;
        CHECK(rep_u32(&rep, 0, &yes) == 0);
        CHECK(yes == 1);
    }
    body_len = build_string_body(body, sizeof(body), "Nope");
    rc = mig_call(g_root_port, IOKIT_MIG_OBJECT_CONFORMS_TO, body,
                  body_len, &rep, NULL);
    CHECK(rc == 1);
    {
        uint32_t yes = 99;
        CHECK(rep_u32(&rep, 0, &yes) == 0);
        CHECK(yes == 0);
    }
    /* 2862 open_extended on the root service: a connection, plus
     * a result word of 0 after the port descriptor. */
    {
        mig_desc desc;
        uint32_t type = 0;
        desc.kind = 0;
        desc.name = TEST_TASK_PORT;
        desc.disp = MACH_MSG_TYPE_COPY_SEND;
        desc.addr = 0;
        desc.size = 0;
        wr32(body, type);
        rc = mig_call_descs(g_root_port,
                            IOKIT_MIG_SERVICE_OPEN_EXTENDED, &desc, 1,
                            body, 4, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        CHECK(rep.n_descs == 1);
        g_conn1 = rep.port_name[0];
        CHECK(g_conn1 != MACH_PORT_NULL);
        {
            uint32_t result = 99;
            CHECK(rep_u32(&rep, 0, &result) == 0);
            CHECK(result == 0);
        }
        /* get_class on the connection answers the service class. */
        rc = mig_call(g_conn1, IOKIT_MIG_OBJECT_GET_CLASS, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        {
            char cls[64];
            CHECK(rep_string(&rep, cls, sizeof(cls)) == 0);
            CHECK(streq(cls, "IOSurfaceRoot"));
        }
    }
    /* 2805 get_property "SurfaceCount": an OOL binary blob, read
     * in-process from the reply descriptor's address. */
    body_len = build_string_body(body, sizeof(body), "SurfaceCount");
    rc = mig_call(g_root_port, IOKIT_MIG_REGISTRY_GET_PROPERTY, body,
                  body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    CHECK(rep.n_descs == 1);
    CHECK(rep.ool_size[0] > 0);
    {
        osser_value *root = NULL;
        rc = pool_decode((const uint8_t *)(uintptr_t)rep.ool_addr[0],
                         rep.ool_size[0], &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            uint64_t n = 99;
            CHECK(osser_as_uint64(root, &n) == OSSER_ERR_OK);
            CHECK(n == 0);
        }
    }
    /* 2811 get_properties: the whole property dict as an OOL blob. */
    rc = mig_call(g_root_port, IOKIT_MIG_REGISTRY_GET_PROPERTIES,
                  NULL, 0, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    CHECK(rep.n_descs == 1);
    {
        osser_value *root = NULL;
        rc = pool_decode((const uint8_t *)(uintptr_t)rep.ool_addr[0],
                         rep.ool_size[0], &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            CHECK(root->type == OSSER_DICT);
            CHECK(dict_num_eq(root, "SurfaceCount", 0));
        }
    }
    /* 2805 with an unknown key answers NotFound, not silence. */
    body_len = build_string_body(body, sizeof(body), "NoSuchKey");
    rc = mig_call(g_root_port, IOKIT_MIG_REGISTRY_GET_PROPERTY, body,
                  body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnNotFound);
    /* 2872 _bin matching services: the dict travels as an OOL
     * binary blob. This exercises the OOL request descriptor. */
    {
        uint8_t blob[256];
        osser_writer w;
        mig_desc desc;
        CHECK(osser_w_init(&w, blob, sizeof(blob)) == OSSER_ERR_OK);
        CHECK(osser_w_begin_dict(&w, 1) == OSSER_ERR_OK);
        CHECK(osser_w_key(&w, "IOProviderClass") == OSSER_ERR_OK);
        CHECK(osser_w_string(&w, "IOSurfaceRoot") == OSSER_ERR_OK);
        CHECK(osser_w_end_dict(&w) == OSSER_ERR_OK);
        desc.kind = 1;
        desc.name = 0;
        desc.disp = 0;
        desc.addr = (uint64_t)(uintptr_t)blob;
        desc.size = osser_w_len(&w);
        rc = mig_call_descs(g_master,
                            IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES_BIN,
                            &desc, 1, NULL, 0, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        CHECK(rep.n_descs == 1);
        {
            mach_port_name_t iter = rep.port_name[0];
            CHECK(iter != MACH_PORT_NULL);
            rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                          &rep, NULL);
            CHECK(rc == 1);
            CHECK(rep.port_name[0] == g_root_port);
        }
        /* 2871 singular _bin: the service port directly. */
        rc = mig_call_descs(g_master,
                            IOKIT_MIG_SERVICE_GET_MATCHING_SERVICE_BIN,
                            &desc, 1, NULL, 0, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        CHECK(rep.port_name[0] == g_root_port);
    }
    /* A non-matching class yields an empty iterator. */
    body_len = build_string_body(body, sizeof(body), g_match_none_xml);
    rc = mig_call(g_master, IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES,
                  body, body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    {
        mach_port_name_t iter = rep.port_name[0];
        CHECK(iter != MACH_PORT_NULL);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.port_name[0] == MACH_PORT_NULL);
    }
    /* An empty matching dict matches everything, in registry
     * order: IOResources first, then IOSurfaceRoot. */
    body_len = build_string_body(body, sizeof(body), g_match_empty_xml);
    rc = mig_call(g_master, IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES,
                  body, body_len, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == kIOReturnSuccess);
    {
        mach_port_name_t iter = rep.port_name[0];
        CHECK(iter != MACH_PORT_NULL);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.port_name[0] == g_master);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.port_name[0] == g_root_port);
        rc = mig_call(iter, IOKIT_MIG_ITERATOR_NEXT, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.port_name[0] == MACH_PORT_NULL);
    }
    SCEN_END();
}

/* ------------------------------------------------------------------ */
/* Scenario 3: surface create, map, write, lock/unlock, planes.       */
/*                                                                     */
/* Expected numbers for the 1080x2400 BGRA create: the dict sends    */
/* BytesPerRow 4320 and AllocSize 10368000 (2400 * 4320); the       */
/* emulation aligns the stride up to 64 bytes (4320 -> 4352) and    */
/* grows the allocation to cover it (2400 * 4352 = 10444800).        */
/* ------------------------------------------------------------------ */

static uint32_t g_sid = 0;
static uint64_t g_map_addr = 0;
static uint64_t g_map_size = 0;

static void scen_3_surface_create_map_lock(void) {
    method_result res;
    int rc;
    printf("SCENARIO 3: create 1080x2400 BGRA + map + lock ... ");
    SCEN_BEGIN();
    if (g_conn1 == MACH_PORT_NULL) {
        printf("SKIPPED (no connection)\n");
        return;
    }
    /* Selector 0, create: XML properties in, 3016-byte struct out. */
    rc = call_method(g_conn1, IOSURFACE_SEL_CREATE, NULL, 0,
                     (const uint8_t *)g_create_xml,
                     (uint32_t)strlen(g_create_xml),
                     IOSURFACE_CREATE_OUT_SIZE, 2, &res);
    CHECK(rc == 1);
    CHECK(res.retcode == kIOReturnSuccess);
    CHECK(res.scalar_cnt == 1);
    CHECK(res.inband_len == IOSURFACE_CREATE_OUT_SIZE);
    g_sid = (uint32_t)res.scalars[0];
    CHECK(g_sid != 0);
    if (res.retcode == kIOReturnSuccess && res.inband_len >= 16) {
        uint32_t id_at_0 = rd32(res.inband + 0);
        uint64_t alloc_at_8 = rd64(res.inband + 8);
        osser_value *root = NULL;
        CHECK(id_at_0 == g_sid);
        CHECK(alloc_at_8 == 10444800u);
        /* The properties blob starts at offset 16. Decode and
         * check it now: the decoded tree points into the reply
         * buffer, which the next call overwrites. */
        rc = pool_decode(res.inband + 16, res.inband_len - 16, &root);
        CHECK(rc == OSSER_ERR_OK);
        if (rc == OSSER_ERR_OK && root) {
            const osser_value *planes = NULL;
            CHECK(root->type == OSSER_DICT);
            CHECK(dict_num_eq(root, "Width", 1080));
            CHECK(dict_num_eq(root, "Height", 2400));
            CHECK(dict_num_eq(root, "BytesPerRow", 4352));
            CHECK(dict_num_eq(root, "BytesPerElement", 4));
            CHECK(dict_num_eq(root, "PixelFormat", 1095911234u));
            CHECK(dict_num_eq(root, "AllocSize", 10444800));
            CHECK(dict_num_eq(root, "CacheMode", 0));
            CHECK(dict_num_eq(root, "Offset", 0));
            CHECK(osser_dict_find(root, "PlaneInfo", &planes) ==
                  OSSER_ERR_OK);
            if (planes) {
                CHECK(planes->type == OSSER_ARRAY);
                CHECK(planes->count == 1);
                if (planes->count == 1) {
                    const osser_value *pl = &planes->items[0];
                    CHECK(dict_num_eq(pl, "PlaneWidth", 1080));
                    CHECK(dict_num_eq(pl, "PlaneHeight", 2400));
                    CHECK(dict_num_eq(pl, "PlaneBytesPerRow", 4352));
                    CHECK(dict_num_eq(pl, "PlaneOffset", 0));
                    CHECK(dict_num_eq(pl, "PlaneSize", 10444800));
                }
            }
        }
    }
    CHECK(iosurface_surface_count() == 1);
    {
        iosurface_surface s;
        CHECK(iosurface_get_surface(g_sid, &s) == KERN_SUCCESS);
        CHECK(s.width == 1080);
        CHECK(s.height == 2400);
        CHECK(s.bytes_per_row == 4352);
        CHECK(s.pixel_format == IOSURFACE_PIXEL_FORMAT_BGRA);
        CHECK(s.alloc_size == 10444800u);
        CHECK(s.plane_count == 1);
        CHECK(s.use_count == 1);
        CHECK(s.lock_count == 0);
        CHECK(s.seed == 0);
        CHECK(s.addr != NULL);
        CHECK(s.map_size == 10444800u);
        {
            int fd = 42;
            CHECK(iosurface_fd(g_sid, &fd) == KERN_SUCCESS);
            CHECK(fd == -1); /* host hook: no memfd here */
        }
    }
    /* 2805 SurfaceCount is now 1. */
    {
        uint8_t body[256];
        uint32_t body_len;
        mig_reply rep;
        body_len = build_string_body(body, sizeof(body), "SurfaceCount");
        rc = mig_call(g_root_port, IOKIT_MIG_REGISTRY_GET_PROPERTY,
                      body, body_len, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        {
            osser_value *root = NULL;
            rc = pool_decode(
                (const uint8_t *)(uintptr_t)rep.ool_addr[0],
                rep.ool_size[0], &root);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && root) {
                uint64_t n = 99;
                CHECK(osser_as_uint64(root, &n) == OSSER_ERR_OK);
                CHECK(n == 1);
            }
        }
    }
    /* Selector 13, get_limits. */
    rc = call_method(g_conn1, IOSURFACE_SEL_GET_LIMITS, NULL, 0,
                     NULL, 0, 24, 0, &res);
    CHECK(rc == 1);
    CHECK(res.retcode == kIOReturnSuccess);
    CHECK(res.inband_len == 24);
    if (res.inband_len == 24) {
        CHECK(rd64(res.inband + 0) == IOSURFACE_LIMIT_MAX_WIDTH);
        CHECK(rd64(res.inband + 8) == IOSURFACE_LIMIT_MAX_HEIGHT);
        CHECK(rd64(res.inband + 16) ==
              IOSURFACE_LIMIT_MAX_BYTES_PER_ROW);
    }
    /* 2863 map_memory_into_task, memory_type = surface id. */
    {
        uint8_t body[32];
        mig_reply rep;
        wr32(body + 0, g_sid);
        wr64(body + 4, 0); /* address hint */
        wr64(body + 12, 0); /* size hint */
        wr32(body + 20, 0); /* flags */
        rc = mig_call(g_conn1, IOKIT_MIG_CONNECT_MAP_MEMORY_INTO_TASK,
                      body, 24, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        {
            uint64_t addr = 0;
            uint64_t size = 0;
            CHECK(rep_u64(&rep, 0, &addr) == 0);
            CHECK(rep_u64(&rep, 8, &size) == 0);
            CHECK(addr != 0);
            CHECK(size == 10444800u);
            g_map_addr = addr;
            g_map_size = size;
        }
    }
    /* Write a known pattern through the mapping and read it back. */
    if (g_map_addr != 0 && g_map_size == 10444800u) {
        uint8_t *base = (uint8_t *)(uintptr_t)g_map_addr;
        uint32_t i;
        for (i = 0; i < 256; i++) base[i] = (uint8_t)(i & 0xffu);
        wr64(base + 4096, 0xA5A5A5A5A5A5A5A5ULL);
        wr64(base + (uint32_t)g_map_size - 8, 0x5A5A5A5A5A5A5A5AULL);
        for (i = 0; i < 256; i++) {
            if (base[i] != (uint8_t)(i & 0xffu)) break;
        }
        CHECK(i == 256);
        CHECK(rd64(base + 4096) == 0xA5A5A5A5A5A5A5A5ULL);
        CHECK(rd64(base + (uint32_t)g_map_size - 8) ==
              0x5A5A5A5A5A5A5A5AULL);
        /* The surface struct's addr is the same mapping. */
        {
            iosurface_surface s;
            CHECK(iosurface_get_surface(g_sid, &s) == KERN_SUCCESS);
            CHECK((uint64_t)(uintptr_t)s.addr == g_map_addr);
            CHECK(rd64((const uint8_t *)s.addr + 4096) ==
                  0xA5A5A5A5A5A5A5A5ULL);
        }
    } else {
        CHECK(0); /* mapping failed above */
    }
    /* Lock (36) returns seed 0; unlock (37) bumps it to 1. */
    {
        uint64_t in[2];
        in[0] = g_sid;
        in[1] = 0;
        rc = call_method(g_conn1, IOSURFACE_SEL_LOCK, in, 2, NULL, 0,
                         0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalar_cnt == 1);
        CHECK(res.scalars[0] == 0);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_SEED, in, 1, NULL,
                         0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 0);
        rc = call_method(g_conn1, IOSURFACE_SEL_UNLOCK, in, 2, NULL, 0,
                         0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalars[0] == 1);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_SEED, in, 1, NULL,
                         0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
        /* A second lock/unlock pair moves the seed to 2. */
        rc = call_method(g_conn1, IOSURFACE_SEL_LOCK, in, 2, NULL, 0,
                         0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
        rc = call_method(g_conn1, IOSURFACE_SEL_UNLOCK, in, 2, NULL, 0,
                         0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 2);
    }
    /* Selector 40, get_plane_info: one 20-byte record. */
    {
        uint64_t in[1];
        in[0] = g_sid;
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_PLANE_INFO, in, 1,
                         NULL, 0, 64, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalar_cnt == 1);
        CHECK(res.scalars[0] == 1);
        CHECK(res.inband_len == 20);
        if (res.inband_len == 20) {
            CHECK(rd32(res.inband + 0) == 0); /* offset */
            CHECK(rd32(res.inband + 4) == 10444800u); /* size */
            CHECK(rd32(res.inband + 8) == 4352); /* bytesPerRow */
            CHECK(rd32(res.inband + 12) == 1080); /* width */
            CHECK(rd32(res.inband + 16) == 2400); /* height */
        }
    }
    SCEN_END();
}

/* ------------------------------------------------------------------ */
/* Scenario 4: values, use counts, second client, release.            */
/* ------------------------------------------------------------------ */

static void scen_4_values_use_release(void) {
    method_result res;
    int rc;
    printf("SCENARIO 4: values + use counts + 2nd client + release ... ");
    SCEN_BEGIN();
    if (g_conn1 == MACH_PORT_NULL || g_sid == 0) {
        printf("SKIPPED (no surface)\n");
        return;
    }
    /* Selector 9, set_value: 8-byte id prefix, then an XML array
     * [key, value]. */
    {
        static const char set_xml[] =
            "<plist version=\"1.0\"><array>"
            "<string>TestKey</string><integer>12345</integer>"
            "</array></plist>";
        uint8_t in[512];
        uint32_t in_len = 0;
        wr64(in + 0, g_sid);
        memcpy(in + 8, set_xml, strlen(set_xml));
        in_len = 8u + (uint32_t)strlen(set_xml);
        rc = call_method(g_conn1, IOSURFACE_SEL_SET_VALUE, NULL, 0,
                         in, in_len, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
    }
    /* Selector 10, get_value: id u64 at 0, NUL-terminated key at 8.
     * The reply is a u32 blob length plus a binary blob. */
    {
        uint8_t in[64];
        wr64(in + 0, g_sid);
        memcpy(in + 8, "TestKey", 8); /* includes the NUL */
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_VALUE, NULL, 0,
                         in, 16, 512, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.inband_len >= 4);
        if (res.inband_len >= 4) {
            uint32_t blob_len = rd32(res.inband + 0);
            osser_value *root = NULL;
            CHECK(blob_len == res.inband_len - 4);
            rc = pool_decode(res.inband + 4, blob_len, &root);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && root) {
                uint64_t n = 0;
                CHECK(osser_as_uint64(root, &n) == OSSER_ERR_OK);
                CHECK(n == 12345);
            }
        }
        /* An unknown key answers NotFound. */
        memcpy(in + 8, "NoSuchKey", 10); /* includes the NUL */
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_VALUE, NULL, 0,
                         in, 18, 512, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnNotFound);
    }
    /* Use counts: create started at 1. */
    {
        uint64_t in[1];
        in[0] = g_sid;
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
        rc = call_method(g_conn1, IOSURFACE_SEL_INCREMENT_USE_COUNT,
                         in, 1, NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 2);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 2);
        rc = call_method(g_conn1, IOSURFACE_SEL_DECREMENT_USE_COUNT,
                         in, 1, NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
    }
    /* Selector 35, create_port: a fresh port, use count +1.
     * Selector 34, lookup_from_port, finds the same surface. */
    {
        uint64_t in[1];
        mach_port_name_t sport = MACH_PORT_NULL;
        in[0] = g_sid;
        rc = call_method(g_conn1, IOSURFACE_SEL_CREATE_PORT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalar_cnt == 1);
        sport = (mach_port_name_t)res.scalars[0];
        CHECK(sport != MACH_PORT_NULL);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 2);
        in[0] = sport;
        rc = call_method(g_conn1, IOSURFACE_SEL_LOOKUP_FROM_PORT, in,
                         1, NULL, 0, 4096, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalar_cnt == 1);
        CHECK(res.scalars[0] == g_sid);
        if (res.retcode == kIOReturnSuccess) {
            osser_value *root = NULL;
            rc = pool_decode(res.inband, res.inband_len, &root);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && root) {
                CHECK(dict_num_eq(root, "Width", 1080));
                CHECK(dict_num_eq(root, "Height", 2400));
            }
        }
        in[0] = 0x9999;
        rc = call_method(g_conn1, IOSURFACE_SEL_LOOKUP_FROM_PORT, in,
                         1, NULL, 0, 4096, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnNotFound);
    }
    /* A second connection (plain 2815 open this time) looks the
     * surface up by id (selector 38): the global id table makes
     * the id valid on any connection of the process. */
    {
        mig_desc desc;
        uint32_t type = 0;
        mig_reply rep;
        uint64_t in[1];
        desc.kind = 0;
        desc.name = TEST_TASK_PORT;
        desc.disp = MACH_MSG_TYPE_COPY_SEND;
        desc.addr = 0;
        desc.size = 0;
        {
            uint8_t body[4];
            wr32(body, type);
            rc = mig_call_descs(g_root_port, IOKIT_MIG_SERVICE_OPEN,
                                &desc, 1, body, 4, &rep, NULL);
        }
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        g_conn2 = rep.port_name[0];
        CHECK(g_conn2 != MACH_PORT_NULL);
        CHECK(g_conn2 != g_conn1);
        in[0] = g_sid;
        rc = call_method(g_conn2, IOSURFACE_SEL_LOOKUP, in, 1, NULL,
                         0, 4096, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(res.scalar_cnt == 1);
        CHECK(res.scalars[0] == g_sid);
        if (res.retcode == kIOReturnSuccess) {
            osser_value *root = NULL;
            rc = pool_decode(res.inband, res.inband_len, &root);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && root) {
                CHECK(dict_num_eq(root, "Width", 1080));
            }
        }
    }
    /* Purge (43) is a no-op success; set_notify (17) records
     * nothing and succeeds. */
    {
        uint64_t in[2];
        in[0] = g_sid;
        in[1] = 1;
        rc = call_method(g_conn1, IOSURFACE_SEL_PURGE, in, 1, NULL,
                         0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        rc = call_method(g_conn1, IOSURFACE_SEL_SET_NOTIFY, in, 2,
                         NULL, 0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
    }
    /* Release once: use count 2 -> 1, surface alive. Lock it,
     * release while locked: Busy, count unchanged. Unlock, then
     * release again: count 1 -> 0, surface destroyed. */
    {
        uint64_t in[2];
        in[0] = g_sid;
        in[1] = 0;
        rc = call_method(g_conn1, IOSURFACE_SEL_RELEASE, in, 1, NULL,
                         0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
        rc = call_method(g_conn1, IOSURFACE_SEL_LOCK, in, 2, NULL, 0,
                         0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        rc = call_method(g_conn1, IOSURFACE_SEL_RELEASE, in, 1, NULL,
                         0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnBusy);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 1);
        rc = call_method(g_conn1, IOSURFACE_SEL_UNLOCK, in, 2, NULL,
                         0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.scalars[0] == 3);
        rc = call_method(g_conn1, IOSURFACE_SEL_RELEASE, in, 1, NULL,
                         0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnSuccess);
        CHECK(iosurface_surface_count() == 0);
        /* A later lookup fails, and so do use count and map. */
        rc = call_method(g_conn1, IOSURFACE_SEL_LOOKUP, in, 1, NULL,
                         0, 4096, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnNotFound);
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_USE_COUNT, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnNotFound);
        {
            uint8_t body[32];
            mig_reply rep;
            wr32(body + 0, g_sid);
            wr64(body + 4, 0);
            wr64(body + 12, 0);
            wr32(body + 20, 0);
            rc = mig_call(g_conn1,
                          IOKIT_MIG_CONNECT_MAP_MEMORY_INTO_TASK,
                          body, 24, &rep, NULL);
            CHECK(rc == 1);
            CHECK(rep.retcode == kIOReturnBadArgument);
        }
        /* Releasing a destroyed surface is NotFound, not a crash. */
        rc = call_method(g_conn1, IOSURFACE_SEL_RELEASE, in, 1, NULL,
                         0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnNotFound);
    }
    /* SurfaceCount is back to 0. */
    {
        uint8_t body[256];
        uint32_t body_len;
        mig_reply rep;
        body_len = build_string_body(body, sizeof(body), "SurfaceCount");
        rc = mig_call(g_root_port, IOKIT_MIG_REGISTRY_GET_PROPERTY,
                      body, body_len, &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        {
            osser_value *root = NULL;
            rc = pool_decode(
                (const uint8_t *)(uintptr_t)rep.ool_addr[0],
                rep.ool_size[0], &root);
            CHECK(rc == OSSER_ERR_OK);
            if (rc == OSSER_ERR_OK && root) {
                uint64_t n = 99;
                CHECK(osser_as_uint64(root, &n) == OSSER_ERR_OK);
                CHECK(n == 0);
            }
        }
    }
    SCEN_END();
}

/* ------------------------------------------------------------------ */
/* Scenario 5: error shapes. Never silence, never a crash.            */
/* ------------------------------------------------------------------ */

static void scen_5_errors(void) {
    mig_reply rep;
    method_result res;
    int rc;
    printf("SCENARIO 5: error shapes ... ");
    SCEN_BEGIN();
    /* Unknown MIG routine id: an error reply, never silence. */
    rc = mig_call(g_master, 2999, NULL, 0, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == KERN_INVALID_ARGUMENT);
    CHECK(rep.id == 2999 + 100);
    rc = mig_call(0x9999, 2999, NULL, 0, &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == KERN_INVALID_ARGUMENT);
    /* Unknown selector on a live connection: Unsupported. */
    if (g_conn1 != MACH_PORT_NULL) {
        rc = call_method(g_conn1, 99, NULL, 0, NULL, 0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnUnsupported);
        /* A method on a bogus connection port: InvalidArgument. */
        rc = call_method(0x9999, IOSURFACE_SEL_GET_SEED, NULL, 0,
                         NULL, 0, 0, 0, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == KERN_INVALID_ARGUMENT);
        /* A method with too few scalars: BadArgument. */
        rc = call_method(g_conn1, IOSURFACE_SEL_GET_SEED, NULL, 0,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnBadArgument);
        /* A method for an unknown surface id: NotFound. */
        {
            uint64_t in[1];
            in[0] = 0xFFFFFFu;
            rc = call_method(g_conn1, IOSURFACE_SEL_LOOKUP, in, 1,
                             NULL, 0, 4096, 2, &res);
            CHECK(rc == 1);
            CHECK(res.retcode == kIOReturnNotFound);
            rc = call_method(g_conn1, IOSURFACE_SEL_GET_SEED, in, 1,
                             NULL, 0, 0, 2, &res);
            CHECK(rc == 1);
            CHECK(res.retcode == kIOReturnNotFound);
        }
    }
    /* A header-only message (no NDR record): the parse fails and
     * an error reply is still built. */
    {
        uint8_t msg[24];
        static uint8_t reply[IOKIT_MAX_REPLY];
        uint32_t reply_size = 0;
        wr32(msg + 0, 0x1513u);
        wr32(msg + 4, 24);
        wr32(msg + 8, g_master);
        wr32(msg + 12, TEST_REPLY_PORT);
        wr32(msg + 16, 0);
        wr32(msg + 20, IOKIT_MIG_OBJECT_GET_CLASS);
        rc = iokit_process_msg((const mach_msg_header_t *)msg, 24,
                               reply, sizeof(reply), &reply_size);
        CHECK(rc == 1);
        CHECK(reply_size >= 36);
        CHECK(parse_mig_reply(reply, reply_size,
                              IOKIT_MIG_OBJECT_GET_CLASS, &rep) == 0);
        CHECK(rep.retcode == KERN_INVALID_ARGUMENT);
    }
    /* A message shorter than the header: no reply can be built. */
    {
        uint8_t msg[16];
        static uint8_t reply[IOKIT_MAX_REPLY];
        uint32_t reply_size = 99;
        memset(msg, 0, sizeof(msg));
        rc = iokit_process_msg((const mach_msg_header_t *)msg, 16,
                               reply, sizeof(reply), &reply_size);
        CHECK(rc == 0);
    }
    /* Bad create inputs: missing Width, zero Width, non-plist. */
    if (g_conn1 != MACH_PORT_NULL) {
        static const char no_width[] =
            "<plist version=\"1.0\"><dict>"
            "<key>Height</key><integer>2400</integer>"
            "</dict></plist>";
        static const char zero_width[] =
            "<plist version=\"1.0\"><dict>"
            "<key>Width</key><integer>0</integer>"
            "<key>Height</key><integer>2400</integer>"
            "</dict></plist>";
        rc = call_method(g_conn1, IOSURFACE_SEL_CREATE, NULL, 0,
                         (const uint8_t *)no_width,
                         (uint32_t)strlen(no_width), 3016, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnBadArgument);
        rc = call_method(g_conn1, IOSURFACE_SEL_CREATE, NULL, 0,
                         (const uint8_t *)zero_width,
                         (uint32_t)strlen(zero_width), 3016, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnBadArgument);
        rc = call_method(g_conn1, IOSURFACE_SEL_CREATE, NULL, 0,
                         (const uint8_t *)"hello", 5, 3016, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == kIOReturnBadArgument);
        /* Oversize create: beyond the limits. */
        {
            static const char huge[] =
                "<plist version=\"1.0\"><dict>"
                "<key>Width</key><integer>20000</integer>"
                "<key>Height</key><integer>2400</integer>"
                "</dict></plist>";
            rc = call_method(g_conn1, IOSURFACE_SEL_CREATE, NULL, 0,
                             (const uint8_t *)huge,
                             (uint32_t)strlen(huge), 3016, 2, &res);
            CHECK(rc == 1);
            CHECK(res.retcode == kIOReturnBadArgument);
        }
    }
    /* Open on a bogus service port, and iterator_next on a bogus
     * iterator port: InvalidArgument. */
    rc = mig_call(0x9999, IOKIT_MIG_SERVICE_OPEN_EXTENDED, NULL, 0,
                  &rep, NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == KERN_INVALID_ARGUMENT);
    rc = mig_call(0x9999, IOKIT_MIG_ITERATOR_NEXT, NULL, 0, &rep,
                  NULL);
    CHECK(rc == 1);
    CHECK(rep.retcode == KERN_INVALID_ARGUMENT);
    /* Close the second connection, then use it: InvalidArgument. */
    if (g_conn2 != MACH_PORT_NULL) {
        uint64_t in[1];
        in[0] = 1;
        rc = mig_call(g_conn2, IOKIT_MIG_SERVICE_CLOSE, NULL, 0,
                      &rep, NULL);
        CHECK(rc == 1);
        CHECK(rep.retcode == kIOReturnSuccess);
        rc = call_method(g_conn2, IOSURFACE_SEL_GET_SEED, in, 1,
                         NULL, 0, 0, 2, &res);
        CHECK(rc == 1);
        CHECK(res.retcode == KERN_INVALID_ARGUMENT);
        g_conn2 = MACH_PORT_NULL;
    }
    SCEN_END();
}

/* ------------------------------------------------------------------ */
/* Scenario 6: fuzz.                                                   */
/*                                                                     */
/* MIG fuzz: random mutations of one valid create message. The       */
/* msgh_bits word (offsets 0-3) is never mutated: flipping the       */
/* COMPLEX bit would make the parser read a descriptor count from   */
/* the NDR/body area, and a forged OOL descriptor address would be  */
/* dereferenced in-process. On the target the kernel supplies OOL   */
/* addresses, so a forged address cannot occur there; on the host  */
/* it would be a harness artifact, not an implementation bug.      */
/* Descriptor-area fuzzing is therefore covered by the directed    */
/* cases in scenario 2 (a real OOL descriptor) and scenario 5      */
/* (truncated messages), not by random bytes.                       */
/* OSSerialize fuzz: random mutations (and truncations) of a       */
/* writer-built binary dict and of the XML create plist.            */
/* ------------------------------------------------------------------ */

static void scen_6_fuzz(uint32_t iterations) {
    uint32_t i;
    uint32_t mig_ok = 0;
    uint32_t mig_err = 0;
    uint32_t osser_ok = 0;
    uint32_t osser_err = 0;
    printf("SCENARIO 6: fuzz (%u iterations) ... ", iterations);
    SCEN_BEGIN();
    if (g_conn1 != MACH_PORT_NULL) {
        uint8_t seed[4096];
        uint8_t body[2048];
        uint32_t body_len;
        uint32_t seed_len;
        body_len = build_method_body(body, sizeof(body),
                                     IOSURFACE_SEL_CREATE, NULL, 0,
                                     (const uint8_t *)g_create_xml,
                                     (uint32_t)strlen(g_create_xml),
                                     IOSURFACE_CREATE_OUT_SIZE, 2);
        CHECK(body_len > 0);
        seed_len = build_mig_msg(seed, sizeof(seed), g_conn1,
                                 IOKIT_MIG_CONNECT_METHOD, NULL, 0,
                                 body, body_len);
        CHECK(seed_len > 0);
        for (i = 0; i < iterations; i++) {
            uint8_t msg[4096];
            static uint8_t reply[IOKIT_MAX_REPLY];
            uint32_t reply_size = 0;
            uint32_t nflips;
            uint32_t f;
            int rc;
            memcpy(msg, seed, seed_len);
            nflips = 1 + (rng_next() % 8);
            for (f = 0; f < nflips; f++) {
                uint32_t off = 4 + (rng_next() % (seed_len - 4));
                msg[off] = (uint8_t)rng_next();
            }
            rc = iokit_process_msg((const mach_msg_header_t *)msg,
                                   seed_len, reply, sizeof(reply),
                                   &reply_size);
            CHECK(rc == 0 || rc == 1);
            CHECK(reply_size <= IOKIT_MAX_REPLY);
            if (rc == 1 && reply_size >= 36) {
                mig_reply rep;
                CHECK(parse_mig_reply(reply, reply_size,
                                      rd32(msg + 20), &rep) == 0);
                if (rep.retcode == kIOReturnSuccess) mig_ok++;
                else mig_err++;
            } else if (rc == 1 && reply_size == 0) {
                mig_err++; /* one-way: reply port mutated to NULL */
            }
        }
    } else {
        printf("(MIG fuzz skipped: no connection) ");
    }
    {
        uint8_t seed[512];
        osser_writer w;
        uint32_t seed_len = 0;
        if (osser_w_init(&w, seed, sizeof(seed)) == OSSER_ERR_OK &&
            osser_w_begin_dict(&w, 3) == OSSER_ERR_OK &&
            osser_w_key(&w, "Width") == OSSER_ERR_OK &&
            osser_w_u32(&w, 1080) == OSSER_ERR_OK &&
            osser_w_key(&w, "Name") == OSSER_ERR_OK &&
            osser_w_string(&w, "Test") == OSSER_ERR_OK &&
            osser_w_key(&w, "Nested") == OSSER_ERR_OK &&
            osser_w_begin_array(&w, 2) == OSSER_ERR_OK &&
            osser_w_u64(&w, 42) == OSSER_ERR_OK &&
            osser_w_bool(&w, 1) == OSSER_ERR_OK &&
            osser_w_end_array(&w) == OSSER_ERR_OK &&
            osser_w_end_dict(&w) == OSSER_ERR_OK) {
            seed_len = osser_w_len(&w);
        }
        CHECK(seed_len > 0);
        for (i = 0; i < iterations; i++) {
            uint8_t blob[512];
            uint32_t len = seed_len;
            uint32_t nflips;
            uint32_t f;
            osser_value *root = NULL;
            int rc;
            memcpy(blob, seed, seed_len);
            if ((rng_next() & 3u) == 0 && len > 8) {
                len = 8 + (rng_next() % (len - 8)); /* truncate */
            }
            nflips = 1 + (rng_next() % 8);
            for (f = 0; f < nflips; f++) {
                blob[rng_next() % len] = (uint8_t)rng_next();
            }
            rc = pool_decode(blob, len, &root);
            CHECK(rc == OSSER_ERR_OK || rc < 0);
            if (rc == OSSER_ERR_OK) osser_ok++;
            else osser_err++;
            rc = pool_decode_auto(blob, len, &root);
            CHECK(rc == OSSER_ERR_OK || rc < 0);
            /* The same mutations through the XML path. */
            {
                osser_pool pool;
                osser_value *xroot = NULL;
                uint8_t xblob[1024];
                uint32_t xlen = (uint32_t)strlen(g_create_xml);
                uint32_t xf;
                if (xlen > sizeof(xblob)) xlen = (uint32_t)sizeof(xblob);
                memcpy(xblob, g_create_xml, xlen);
                for (xf = 0; xf < nflips; xf++) {
                    xblob[rng_next() % xlen] = (uint8_t)rng_next();
                }
                pool_reset(&pool);
                rc = osser_xml_decode(xblob, xlen, &pool, &xroot);
                CHECK(rc == OSSER_ERR_OK || rc < 0);
            }
        }
    }
    printf("fuzz: mig ok %u err %u, osser ok %u err %u\n", mig_ok,
           mig_err, osser_ok, osser_err);
    SCEN_END();
}

int main(int argc, char **argv) {
    uint32_t iterations = 10000;
    int rc;
    if (argc > 1) {
        iterations = (uint32_t)atoi(argv[1]);
        if (iterations == 0) iterations = 10000;
    }
    printf("iokit_test: IOKit + IOSurface host test\n");
    rc = iosurface_init();
    CHECK(rc == KERN_SUCCESS);
    if (rc != KERN_SUCCESS) {
        printf("iosurface_init failed: %d\n", rc);
        printf("TOTAL: %d checks, %d failures\n", g_checks,
               g_failures);
        return 1;
    }
    scen_1_osser();
    scen_2_mig_registry();
    scen_3_surface_create_map_lock();
    scen_4_values_use_release();
    scen_5_errors();
    scen_6_fuzz(iterations);
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
