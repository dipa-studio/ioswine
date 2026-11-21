/* mg_test.c: host test for MobileGestalt.
 *
 * What it covers:
 *   1. Obfuscation unit test: base64(md5("MGCopyAnswer" + question))
 *      minus trailing "==" reproduces 49 known question/key pairs,
 *      plus the two keys computed for this delivery (MetalCapability,
 *      BluetoothAddress), and the marketing-name anomaly key is NOT
 *      reproduced by the formula (copied verbatim instead).
 *   2. Cache build and parse: mg_build_cache + bp_parse round-trip;
 *      every launch-time question resolves to its table value, all
 *      four top-level keys are present, CacheVersion is 23G90, every
 *      duplicate key is filled, CacheData is MG_CACHEDATA_LEN zero
 *      bytes, the artwork and GPU sub-dicts match, the 52-byte zero
 *      field is present.
 *   3. Cache file on disk: mg_init writes it at the container path,
 *      mg_cache_valid accepts it, rejects a stale CacheVersion
 *      (23G89), a missing file and a corrupt file, and
 *      mg_ensure_cache restores a good file each time. mg_init is
 *      idempotent.
 *   4. Helper plain-dict messages through mg_process_msg:
 *      {"question": "..."} for launch-time questions, including the
 *      data and array answers and the ArtworkTraits dict.
 *   5. Unknown question gets an error reply, never a crash.
 *   6. Protected questions get a denial (NULL answer + error),
 *      never a fabricated identifier. Dynamic questions get a NULL
 *      answer with error 0.
 *   7. Malformed input: truncated body, non-string question, empty
 *      question, empty dict, array root, non-XPC body: each gets
 *      {"error": 22} and never crashes.
 *   8. NSXPC placeholder: SKIPPED until a real device capture lands
 *      (the capture bytes are pasted in after the first device run).
 *   9. Fuzz mg_process_msg (10000 iterations of mutated/truncated
 *      messages): no crash, no hang, an error reply every time.
 *  10. Fuzz bp_parse over mutated cache bytes (5000 iterations):
 *      no crash, return code 0 or negative.
 *  11. Reply size: the largest replies (CompatibleAppVariants,
 *      MainScreenStaticInfo) fit in SVCHOST_MAX_REPLY.
 *
 * How the service is driven on host: there is no real ipc.c here,
 * so this file stubs the ipc_* symbols the .c files reference
 * (found with `nm -u` on the compiled objects: g_ipc_trace,
 * lsk_getenv, ipc_task_self, ipc_mach_reply_port,
 * ipc_svchost_port_create, ipc_port_from_name, ipc_port_deallocate,
 * ipc_port_destroy, ipc_msg) following tests/host/daemons_test.c.
 * ipc_msg emulates ONLY the bootstrap_register2 path (msgh_id
 * 0x100000cc). The lsk_* file functions (declared in mg.h, defined
 * in the tree's bsd.c) are stubbed here with libc calls, translating
 * Darwin open flags to Linux flags the same way bsd.c does. The
 * The container_* functions come from the real src/container.c,
 * linked in the build below. Groups are pre-registered
 * by the launcher with container_set_group(); the test plays the
 * launcher and registers the MobileGestalt system group under the
 * temp home before mg_init() runs, exactly as the real launcher
 * must do before daemons_init() (see mg.h). The temp dir is
 * created with mkdtemp and removed at the end.
 *
 * The expected values in this file are independent literals,
 * NOT from mg_names.h, so the test independently verifies the
 * service's table.
 *
 * Build (run from the daemons directory). The target linux.h
 * is aarch64-only, so the build needs the host shim first on the
 * -I path (tests/host/host-shim/linux.h). (An earlier revision
 * staged svchost.c and bsxpc.c from /tmp copies because a tree
 * linux.h sat in src/ and shadowed the shim for quote-includes;
 * that copy was removed 2026-11-12 and both files now compile
 * from src/ directly.)
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/mg_test tests/host/mg_test.c src/mg.c \
 *        src/bplist.c src/xpcwire.c src/container.c \
 *        src/svchost.c src/bsxpc.c
 *     /tmp/mg_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/mg_test_asan \
 *        tests/host/mg_test.c src/mg.c src/bplist.c src/xpcwire.c \
 *        src/container.c src/svchost.c src/bsxpc.c
 *     /tmp/mg_test_asan
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
#include <stdarg.h>
#include <sys/stat.h>
#include "xpcwire.h"
#include "svchost.h"
#include "bplist.h"
#include "bsxpc.h"
#include "mg.h"
#include "mg_names.h"
#include "nsxpc.h"
#include "nsxpc.c"
#include "launchd.h"
#include "container.h"

/* ------------------------------------------------------------------ */
/* Test-side wire keys (independent of mg_names.h).                   */
/* ------------------------------------------------------------------ */

#define TEST_ERROR_KEY "error"
#define TEST_QUESTION_KEY "question"
#define TEST_ANSWER_KEY "answer"

#define SIM_REPLY_PORT ((mach_port_name_t)0x3000)

/* ------------------------------------------------------------------ */
/* Harness.                                                            */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("\n  FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

#define CHECK_RC(expr) CHECK((expr) == 0)

#define SCEN_BEGIN(n, name) do { \
    printf("Scenario %d: %s\n", (n), (name)); \
} while (0)

#define SCEN_END(n) do { \
    printf("  scenario %d done\n", (n)); \
} while (0)

#define SKIP(msg) do { \
    g_skipped++; \
    printf("  SKIP: %s\n", (msg)); \
} while (0)

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321), test-local one-shot implementation. Written fresh  */
/* from the spec so the obfuscation test does not depend on any       */
/* library; verified below against the RFC test vectors.              */
/* ------------------------------------------------------------------ */

static const uint32_t md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const int md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t md5_rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void md5_block(const uint8_t *p, uint32_t *a, uint32_t *b,
                      uint32_t *c, uint32_t *d) {
    uint32_t m[16];
    uint32_t aa = *a, bb = *b, cc = *c, dd = *d;
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[i * 4] |
               ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) |
               ((uint32_t)p[i * 4 + 3] << 24);
    }
    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (bb & cc) | (~bb & dd);
            g = i;
        } else if (i < 32) {
            f = (dd & bb) | (~dd & cc);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = bb ^ cc ^ dd;
            g = (3 * i + 5) % 16;
        } else {
            f = cc ^ (bb | ~dd);
            g = (7 * i) % 16;
        }
        f = f + aa + md5_k[i] + m[g];
        aa = dd;
        dd = cc;
        cc = bb;
        bb = bb + md5_rotl(f, md5_s[i]);
    }
    *a += aa;
    *b += bb;
    *c += cc;
    *d += dd;
}

static void test_md5(const uint8_t *msg, size_t len, uint8_t out[16]) {
    uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu,
             d = 0x10325476u;
    uint8_t block[128];
    size_t i = 0;
    uint64_t bits = (uint64_t)len * 8u;
    /* Full 64-byte blocks straight from the message. */
    while (len - i >= 64) {
        md5_block(msg + i, &a, &b, &c, &d);
        i += 64;
    }
    /* Tail: remaining bytes, 0x80, zero padding, 8-byte length. */
    {
        size_t rem = len - i;
        size_t j;
        size_t total;
        for (j = 0; j < rem; j++) block[j] = msg[i + j];
        block[rem] = 0x80;
        total = rem + 1;
        /* Pad with zeros until 8 bytes remain in the last block. */
        while ((total % 64) != 56) block[total++] = 0;
        for (j = 0; j < 8; j++)
            block[total + j] = (uint8_t)((bits >> (8 * j)) & 0xffu);
        total += 8;
        for (j = 0; j < total; j += 64)
            md5_block(block + j, &a, &b, &c, &d);
    }
    {
        uint32_t w[4];
        int k;
        w[0] = a;
        w[1] = b;
        w[2] = c;
        w[3] = d;
        for (k = 0; k < 4; k++) {
            out[k * 4] = (uint8_t)(w[k] & 0xffu);
            out[k * 4 + 1] = (uint8_t)((w[k] >> 8) & 0xffu);
            out[k * 4 + 2] = (uint8_t)((w[k] >> 16) & 0xffu);
            out[k * 4 + 3] = (uint8_t)((w[k] >> 24) & 0xffu);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Base64 (test-local).                                                */
/* ------------------------------------------------------------------ */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void test_b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        out[o++] = b64_alphabet[(v >> 6) & 63];
        out[o++] = b64_alphabet[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8);
        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        out[o++] = b64_alphabet[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_val(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

/* Decode a base64 string (no whitespace) into out. Returns the
 * number of bytes written, or -1 on bad input. */
static int test_b64_decode(const char *in, uint8_t *out, size_t cap) {
    size_t len = strlen(in);
    size_t i = 0;
    size_t o = 0;
    if (len % 4 != 0) return -1;
    while (i < len) {
        int v0 = b64_val((unsigned char)in[i]);
        int v1 = b64_val((unsigned char)in[i + 1]);
        int v2 = in[i + 2] == '=' ? 0 : b64_val((unsigned char)in[i + 2]);
        int v3 = in[i + 3] == '=' ? 0 : b64_val((unsigned char)in[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return -1;
        if (o + 3 > cap) return -1;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (in[i + 2] != '=') out[o++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        if (in[i + 3] != '=') out[o++] = (uint8_t)((v2 << 6) | v3);
        i += 4;
    }
    return (int)o;
}

/* The obfuscation formula: base64(md5("MGCopyAnswer" + question))
 * with trailing "==" stripped. */
static void test_obfuscate(const char *question, char *out) {
    char buf[128];
    uint8_t digest[16];
    size_t qlen = strlen(question);
    size_t olen = 0;
    memcpy(buf, "MGCopyAnswer", 12);
    memcpy(buf + 12, question, qlen + 1);
    test_md5((const uint8_t *)buf, 12 + qlen, digest);
    test_b64_encode(digest, sizeof(digest), out);
    olen = strlen(out);
    while (olen > 0 && out[olen - 1] == '=') out[--olen] = '\0';
}

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
/* lsk file stubs (declared in mg.h, defined in the tree's bsd.c).    */
/* Darwin open flags are translated to Linux flags the same way      */
/* bsd.c's darwin_open_flags_to_linux does.                           */
/* ------------------------------------------------------------------ */

static int darwin_flags_to_linux(int flags) {
    int lf = 0;
    int acc = flags & 3;
    if (acc == 0) lf |= 00;
    else if (acc == 1) lf |= 01;
    else if (acc == 2) lf |= 02;
    if (flags & 0x0004) lf |= 04000; /* O_NONBLOCK */
    if (flags & 0x0008) lf |= 02000; /* O_APPEND */
    if (flags & 0x0200) lf |= 0100;  /* O_CREAT */
    if (flags & 0x0400) lf |= 01000; /* O_TRUNC */
    if (flags & 0x0800) lf |= 0200;  /* O_EXCL */
    return lf;
}

int lsk_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & 0x0200) { /* O_CREAT: a mode argument follows */
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return open(path, darwin_flags_to_linux(flags), mode);
}

int64_t lsk_read(int fd, void *buf, size_t count) {
    return (int64_t)read(fd, buf, count);
}

int64_t lsk_write(int fd, const void *buf, size_t count) {
    return (int64_t)write(fd, buf, count);
}

int lsk_close(int fd) {
    return close(fd);
}

int lsk_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}

/* ------------------------------------------------------------------ */
/* lsk_getuid stub (declared in container.c as _getuid; defined in   */
/* the tree's bsd.c). container_setup() is never called here, so the  */
/* value is never read; it only has to exist for the link.            */
/* ------------------------------------------------------------------ */

uint32_t lsk_getuid(void) __asm__("_getuid");

uint32_t lsk_getuid(void) {
    return 501;
}

/* ------------------------------------------------------------------ */
/* Mach message delivery.                                              */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t g_mg_port = 0;

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

/* Deliver a built message to mg_process_msg. Returns the entry's
 * return value (0 or 1), or -1 when the buffer is too small. */
static int deliver(mach_port_name_t dest, mach_port_name_t reply_port,
                   const uint8_t *body, uint32_t body_len,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    uint8_t msg[4096];
    uint32_t size = build_mach_msg(msg, sizeof(msg), dest, reply_port,
                                   body, body_len);
    if (size == 0) return -1;
    return mg_process_msg((const mach_msg_header_t *)msg, size,
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

/* Build an XPC body for a question request: {"question": "..."}. */
static int build_question_body(uint8_t *body, uint32_t cap,
                               const char *question, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_string(&b, TEST_QUESTION_KEY,
                                          question);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Ask one question through mg_process_msg and return the parsed
 * reply reader. The reply buffer must outlive the reader. */
static int ask(const char *question, uint8_t *reply_buf,
               uint32_t reply_cap, uint32_t *reply_size_out,
               xw_reader *rd_out) {
    uint8_t body[512];
    uint32_t body_len = 0;
    uint32_t reply_size = 0;
    xw_envelope env;
    int r = build_question_body(body, sizeof(body), question, &body_len);
    int ret;
    if (r != XW_OK) return r;
    ret = deliver(g_mg_port, SIM_REPLY_PORT, body, body_len, reply_buf,
                  reply_cap, &reply_size);
    if (ret != 1) return -100;
    if (parse_reply(reply_buf, reply_size, &env, rd_out) != XW_OK)
        return -101;
    if (reply_size_out != NULL) *reply_size_out = reply_size;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Deterministic PRNG for the fuzz loops (reproducible runs).          */
/* ------------------------------------------------------------------ */

static uint32_t g_rng_state = 0x12345678u;

static uint32_t rng_next(void) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state >> 8;
}

/* ------------------------------------------------------------------ */
/* Scenario 1: obfuscation unit test.                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *question;
    const char *key;
} obf_pair_t;

static const obf_pair_t g_obf_pairs[] = {
    { "DeviceClass", "+3Uf0Pm5F8Xy7Onyvko0vA" },
    { "ProductType", "h9jDsbgj7xIVeIQ8S3/X3Q" },
    { "ProductVersion", "qNNddlUK+B/YlooNoymwgA" },
    { "BuildVersion", "mZfUC7qo4pURNhyMHZ62RQ" },
    { "DeviceClassNumber", "mtrAoWJ3gsq+I90ZnQ0vQw" },
    { "HWModelStr", "/YYygAofPDbhrwToVsXdeA" },
    { "main-screen-width", "g7YQ1Djxh4YiKlEeaoGhzg" },
    { "main-screen-height", "OjzOua0LkOegX7pQdgMksw" },
    { "main-screen-scale", "SNfDJgQFV2Xj7+WnozcJPw" },
    { "main-screen-pitch", "0l4wqBtWEAK1tOkeBHkU6Q" },
    { "main-screen-class", "fdh+s6j3VijuyrK7xLjd7g" },
    { "ipad", "uKc7FPnEO++lVhHWHFlGbQ" },
    { "ArtworkTraits", "oPeik/9e8lQWMszEjbPzng" },
    { "CompatibleAppVariants", "VuGdqp8UBpi9vPWHlPluVQ" },
    { "MainScreenCanvasSizes", "ybGkijAwLTwevankfVzsDQ" },
    { "ProductHash", "TZ/0j62wM3D0CuRt+Nc/Lw" },
    { "DeviceName", "rkqlwPcRHwixY4gapPjanw" },
    { "GSDeviceName", "9s45ldrCC1WF+7b6C4H2BA" },
    { "ProductName", "ivIu8YTDnBSrYv/SN4G8Ag" },
    { "IsSimulator", "ulMliLomP737aAOJ/w/evA" },
    { "InternalBuild", "LBJfwOEzExRxzlAnSuI7eg" },
    { "CPUArchitecture", "k7QIBwZJJOVw+Sej/8h8VA" },
    { "SupportedDeviceFamilies", "9MZ5AdH43csAUajl/dU+IQ" },
    { "RegionCode", "h63QSdBCiT/z0WU6rdQv6Q" },
    { "RegionInfo", "zHeENZu+wbg7PUprwNwBWg" },
    { "RegulatoryModelNumber", "97JDvERpVwO+GHtthIh7hA" },
    { "ModelNumber", "D0cJ8r7U5zve6uA6QbOiLA" },
    { "DeviceColorMapPolicy", "87sSAh2rboMI2TDvFBimkg" },
    { "FirmwareVersion", "LeSRsiLoJCMhjn6nd6GWbQ" },
    { "HasBaseband", "AJFQheZDyUbvI6RmBMT9Cg" },
    { "contains-cellular-radio", "yRZv0s7Dpj8ZBk0S+0+nMA" },
    { "CellularTelephonyCapability", "ebyBs0j3KAquBsgcfrNZIg" },
    { "arm64", "kKgJsWN/rBUAkimOtm/wbA" },
    { "DeviceSupports64Bit", "dvaktw5A/h0zlsWpMg/COQ" },
    { "DeviceSupportsARKit", "iAu0GZogf4TG69GSO5rHcg" },
    { "HasExtendedColorDisplay", "Aixt/MEN2O2B7f+8m4TxUA" },
    { "DeviceCornerRadius", "y5dppxx/LzxoNuW+iIKR3g" },
    { "SupportsForceTouch", "eQd5mlz0BN0amTp/2ccMoA" },
    { "MainDisplayRotation", "IIYjwu1lbAyGz5t0c4ECoA" },
    { "HasSpringBoard", "OBqqs000I0SR+EbJ7VO8UQ" },
    { "DeviceEnclosureColor", "JhEU414EIaDvAz8ki5DSqw" },
    { "DeviceColor", "mumHZHMLEfAuTkkd28fHlQ" },
    { "MainScreenStaticInfo", "oBbtJ8x+s1q0OkaiocPuog" },
    { "MainScreenOrientation", "1h6a0FG2nwMsUDVnYfaxxA" },
    { "UniqueDeviceID", "re6Zb+zwFKJNlkQTUeT+/w" },
    { "UniqueDeviceIDData", "nFRqKto/RuQAV1P+0/qkBA" },
    { "SerialNumber", "VasUgeSzVyHdB27g2XpN0g" },
    { "MLBSerialNumber", "Q1Ty5w8gxMWHx3p4lQ1fhA" },
    { "InverseDeviceID", "frZQaeyWLUvLjeuEK43hmg" },
    /* Computed for this delivery with the same formula. */
    { "MetalCapability", "/8aKho3RQEvq1PxPnN2ziQ" },
    { "BluetoothAddress", "k5lVWbXuiZHLA17KGiVUAA" },
};

#define OBF_PAIR_COUNT (sizeof(g_obf_pairs) / sizeof(g_obf_pairs[0]))

static void scen_1_obfuscation(void) {
    size_t i;
    char out[32];
    uint8_t digest[16];
    char hex[33];
    static const char hexd[] = "0123456789abcdef";
    int k;
    SCEN_BEGIN(1, "obfuscation formula (51 pairs + anomaly)");
    /* MD5 self-check against the RFC 1321 test vectors. */
    test_md5((const uint8_t *)"", 0, digest);
    for (k = 0; k < 16; k++) {
        hex[k * 2] = hexd[(digest[k] >> 4) & 0xf];
        hex[k * 2 + 1] = hexd[digest[k] & 0xf];
    }
    hex[32] = '\0';
    CHECK(strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e") == 0);
    test_md5((const uint8_t *)"abc", 3, digest);
    for (k = 0; k < 16; k++) {
        hex[k * 2] = hexd[(digest[k] >> 4) & 0xf];
        hex[k * 2 + 1] = hexd[digest[k] & 0xf];
    }
    hex[32] = '\0';
    CHECK(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);
    /* Every known pair reproduces exactly. */
    for (i = 0; i < OBF_PAIR_COUNT; i++) {
        test_obfuscate(g_obf_pairs[i].question, out);
        if (strcmp(out, g_obf_pairs[i].key) != 0) {
            printf("  obf mismatch: %s -> %s (expected %s)\n",
                   g_obf_pairs[i].question, out, g_obf_pairs[i].key);
        }
        CHECK(strcmp(out, g_obf_pairs[i].key) == 0);
    }
    /* The anomaly: the formula does NOT reproduce the marketing-name
     * key, so the service must copy it verbatim (it does; scenario
     * 2 checks the cache value). */
    test_obfuscate("MarketingNameString", out);
    CHECK(strcmp(out, "Z/dqyWS6OZTRy10UcmUAhw") != 0);
    test_obfuscate("MarketingName", out);
    CHECK(strcmp(out, "Z/dqyWS6OZTRy10UcmUAhw") != 0);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: cache build + parse round-trip.                         */
/* ------------------------------------------------------------------ */

#define EXP_STR 0
#define EXP_INT 1
#define EXP_BOOL 2
#define EXP_REAL 3

typedef struct {
    const char *key;
    int kind;
    const char *s;
    int64_t i;
    double d;
    int b;
} exp_scalar_t;

static const exp_scalar_t g_exp_scalars[] = {
    { "+3Uf0Pm5F8Xy7Onyvko0vA", EXP_STR, "iPhone", 0, 0.0, 0 },
    { "mtrAoWJ3gsq+I90ZnQ0vQw", EXP_INT, NULL, 1, 0.0, 0 },
    { "rkqlwPcRHwixY4gapPjanw", EXP_STR, "iPhone", 0, 0.0, 0 },
    { "9s45ldrCC1WF+7b6C4H2BA", EXP_STR, "iPhone", 0, 0.0, 0 },
    { "h9jDsbgj7xIVeIQ8S3/X3Q", EXP_STR, "iPhone17,3", 0, 0.0, 0 },
    { "qNNddlUK+B/YlooNoymwgA", EXP_STR, "26.6.2", 0, 0.0, 0 },
    { "mZfUC7qo4pURNhyMHZ62RQ", EXP_STR, "23G90", 0, 0.0, 0 },
    { "ivIu8YTDnBSrYv/SN4G8Ag", EXP_STR, "iPhone OS", 0, 0.0, 0 },
    { "/YYygAofPDbhrwToVsXdeA", EXP_STR, "D47AP", 0, 0.0, 0 },
    { "ulMliLomP737aAOJ/w/evA", EXP_BOOL, NULL, 0, 0.0, 0 },
    { "LBJfwOEzExRxzlAnSuI7eg", EXP_BOOL, NULL, 0, 0.0, 0 },
    { "9ZQAcDmoSgmTrmVL06N7hA", EXP_BOOL, NULL, 0, 0.0, 0 },
    { "k7QIBwZJJOVw+Sej/8h8VA", EXP_STR, "arm64e", 0, 0.0, 0 },
    { "h63QSdBCiT/z0WU6rdQv6Q", EXP_STR, "LL", 0, 0.0, 0 },
    { "zHeENZu+wbg7PUprwNwBWg", EXP_STR, "LL/A", 0, 0.0, 0 },
    { "97JDvERpVwO+GHtthIh7hA", EXP_STR, "A3287", 0, 0.0, 0 },
    { "D0cJ8r7U5zve6uA6QbOiLA", EXP_STR, "MYEC3", 0, 0.0, 0 },
    { "Z/dqyWS6OZTRy10UcmUAhw", EXP_STR, "iPhone 16", 0, 0.0, 0 },
    { "87sSAh2rboMI2TDvFBimkg", EXP_INT, NULL, 1, 0.0, 0 },
    { "AJFQheZDyUbvI6RmBMT9Cg", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "yRZv0s7Dpj8ZBk0S+0+nMA", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "ebyBs0j3KAquBsgcfrNZIg", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "uKc7FPnEO++lVhHWHFlGbQ", EXP_BOOL, NULL, 0, 0.0, 0 },
    { "kKgJsWN/rBUAkimOtm/wbA", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "dvaktw5A/h0zlsWpMg/COQ", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "iAu0GZogf4TG69GSO5rHcg", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "/8aKho3RQEvq1PxPnN2ziQ", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "Aixt/MEN2O2B7f+8m4TxUA", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "y5dppxx/LzxoNuW+iIKR3g", EXP_REAL, NULL, 0, 0.0, 0 },
    { "eQd5mlz0BN0amTp/2ccMoA", EXP_BOOL, NULL, 0, 0.0, 0 },
    { "IIYjwu1lbAyGz5t0c4ECoA", EXP_INT, NULL, 0, 0.0, 0 },
    { "OBqqs000I0SR+EbJ7VO8UQ", EXP_BOOL, NULL, 0, 0.0, 1 },
    { "JhEU414EIaDvAz8ki5DSqw", EXP_STR, "#e1ccb5", 0, 0.0, 0 },
    { "mumHZHMLEfAuTkkd28fHlQ", EXP_STR, "#e1e4e3", 0, 0.0, 0 },
    { "g7YQ1Djxh4YiKlEeaoGhzg", EXP_INT, NULL, 1440, 0.0, 0 },
    { "OjzOua0LkOegX7pQdgMksw", EXP_INT, NULL, 3200, 0.0, 0 },
    { "SNfDJgQFV2Xj7+WnozcJPw", EXP_REAL, NULL, 0, 4.0, 0 },
    { "0l4wqBtWEAK1tOkeBHkU6Q", EXP_INT, NULL, 521, 0.0, 0 },
    { "fdh+s6j3VijuyrK7xLjd7g", EXP_INT, NULL, 8, 0.0, 0 },
    { "1h6a0FG2nwMsUDVnYfaxxA", EXP_REAL, NULL, 0, 0.0, 0 },
};

#define EXP_SCALAR_COUNT (sizeof(g_exp_scalars) / sizeof(g_exp_scalars[0]))

typedef struct {
    const char *key;
    const char *value;
} exp_dup_t;

static const exp_dup_t g_exp_dups[] = {
    { "xNN67KktpWp7syTT3S1BFA", "iPhone17,3" },
    { "+1TeoctsaQC55zwHZ6MESg", "iPhone17,3" },
    { "myx96YOqBSDzLwljSYWBiQ", "iPhone17,3" },
    { "0+nc/Udy4WNG8S+Q7a/s1A", "iPhone17,3" },
    { "GEsznZwAYGOa1a67QU1Uew", "iPhone17,3" },
    { "GqAdWRLnC7oYQrNYF48VYA", "iPhone17,3" },
    { "MKE8hwsOxxRCtwBk2aDBZA", "iPhone17,3" },
    { "G91h5IuJvXISeyngNFqEpg", "iPhone17,3" },
    { "vme9Buk6XiWFCXoHApxNFA", "iPhone" },
    { "JUWcn+5Ss0nvr5w/jk4WEg", "iPhone" },
    { "oQNDePXjSD1z7W0ddqt9tg", "D47AP" },
    { "dW5fpt/6HhaTbnK/UqL6cA", "D47AP" },
    { "GGIIDN/ANr8X2WrgS6nBYQ", "D47AP" },
    { "ZGraRMW0TsxCvONeeJ5C2w", "D47AP" },
    { "uCIk6n9Am5fsV2cTjhqFQw", "D47AP" },
    { "b4e7mEbjqfewD6oXmo9U5g", "D47AP" },
    { "oYicEKzVTz4/CxxE05pEgQ", "D47AP" },
    { "yAfB6E2v0++rHtdW7SDg8w", "D47AP" },
    { "bbtR9jQx50Fv5Af/affNtA", "iPhone 16" },
};

#define EXP_DUP_COUNT (sizeof(g_exp_dups) / sizeof(g_exp_dups[0]))

/* The built cache bytes, kept for the bp_parse fuzz (scenario 10). */
static uint8_t g_cache_bytes[MG_CACHE_BUILD_CAP];
static uint32_t g_cache_len = 0;

static void scen_2_cache_roundtrip(void) {
    static uint8_t arena_buf[MG_CACHE_ARENA_CAP];
    bp_arena arena;
    bp_value *root = NULL;
    bp_value *extra = NULL;
    bp_value *v = NULL;
    uint32_t out_len = 0;
    size_t i;
    SCEN_BEGIN(2, "cache build + parse round-trip");
    CHECK_RC(mg_build_cache(g_cache_bytes, sizeof(g_cache_bytes),
                            &out_len));
    g_cache_len = out_len;
    if (out_len == 0) {
        /* Build failed; the checks below would only repeat it. */
        SCEN_END(2);
        return;
    }
    CHECK(out_len > MG_CACHEDATA_LEN);
    CHECK(out_len < MG_CACHE_BUILD_CAP);
    CHECK(memcmp(g_cache_bytes, "bplist00", 8) == 0);
    bp_arena_init(&arena, arena_buf, sizeof(arena_buf));
    CHECK_RC(bp_parse(g_cache_bytes, out_len, &arena, &root));
    CHECK(root != NULL && root->type == BP_DICT);
    /* All four top-level keys, with the right types. */
    v = bp_dict_get(root, "CacheData");
    CHECK(v != NULL && v->type == BP_DATA);
    CHECK(v != NULL && v->len == MG_CACHEDATA_LEN);
    if (v != NULL && v->type == BP_DATA && v->len == MG_CACHEDATA_LEN) {
        uint32_t z;
        int all_zero = 1;
        for (z = 0; z < v->len; z++) {
            if (v->s[z] != 0) {
                all_zero = 0;
                break;
            }
        }
        CHECK(all_zero);
    }
    v = bp_dict_get(root, "CacheExtra");
    CHECK(v != NULL && v->type == BP_DICT);
    extra = v;
    v = bp_dict_get(root, "CacheUUID");
    CHECK(v != NULL && v->type == BP_STRING &&
          strcmp((const char *)v->s, "CEEA7316-5800-454A-AA6A-834513AAC59B") == 0);
    v = bp_dict_get(root, "CacheVersion");
    CHECK(v != NULL && v->type == BP_STRING &&
          strcmp((const char *)v->s, "23G90") == 0);
    CHECK(extra != NULL);
    if (extra == NULL) {
        SCEN_END(2);
        return;
    }
    /* Every scalar launch-time answer. */
    for (i = 0; i < EXP_SCALAR_COUNT; i++) {
        const exp_scalar_t *e = &g_exp_scalars[i];
        v = bp_dict_get(extra, e->key);
        if (v == NULL) {
            printf("  cache missing key %s\n", e->key);
            CHECK(0);
            continue;
        }
        switch (e->kind) {
        case EXP_STR:
            CHECK(v->type == BP_STRING && v->s != NULL &&
                  strcmp((const char *)v->s, e->s) == 0);
            break;
        case EXP_INT:
            CHECK(v->type == BP_INT && v->i == e->i);
            break;
        case EXP_BOOL:
            CHECK(v->type == BP_BOOL && v->b == e->b);
            break;
        case EXP_REAL:
            CHECK(v->type == BP_REAL && v->d == e->d);
            break;
        default:
            CHECK(0);
            break;
        }
    }
    /* Every duplicate key is filled with the same value. */
    for (i = 0; i < EXP_DUP_COUNT; i++) {
        v = bp_dict_get(extra, g_exp_dups[i].key);
        CHECK(v != NULL && v->type == BP_STRING && v->s != NULL &&
              strcmp((const char *)v->s, g_exp_dups[i].value) == 0);
    }
    /* MainScreenStaticInfo: 24 bytes, pinned by the doc's base64. */
    {
        uint8_t expect[24];
        int n = test_b64_decode("oAUAAIAMAAAJAgAAAACAQAAAAAAIAAAA",
                                expect, sizeof(expect));
        CHECK(n == 24);
        v = bp_dict_get(extra, "oBbtJ8x+s1q0OkaiocPuog");
        CHECK(v != NULL && v->type == BP_DATA && v->len == 24);
        if (v != NULL && v->type == BP_DATA && v->len == 24 && n == 24)
            CHECK(memcmp(v->s, expect, 24) == 0);
    }
    /* MainScreenCanvasSizes: one CGSize {1440.0, 3200.0}, float64 LE. */
    {
        uint8_t expect[16];
        double w = 1440.0, h = 3200.0;
        memcpy(expect, &w, 8);
        memcpy(expect + 8, &h, 8);
        v = bp_dict_get(extra, "ybGkijAwLTwevankfVzsDQ");
        CHECK(v != NULL && v->type == BP_DATA && v->len == 16);
        if (v != NULL && v->type == BP_DATA && v->len == 16)
            CHECK(memcmp(v->s, expect, 16) == 0);
    }
    /* CompatibleAppVariants: ["iPhone17,3", "iPhone16,1"]. */
    v = bp_dict_get(extra, "VuGdqp8UBpi9vPWHlPluVQ");
    CHECK(v != NULL && v->type == BP_ARRAY);
    if (v != NULL && v->type == BP_ARRAY) {
        bp_value *c0 = v->child;
        bp_value *c1 = c0 != NULL ? c0->next : NULL;
        CHECK(c0 != NULL && c0->type == BP_STRING &&
              strcmp((const char *)c0->s, "iPhone17,3") == 0);
        CHECK(c1 != NULL && c1->type == BP_STRING &&
              strcmp((const char *)c1->s, "iPhone16,1") == 0);
        CHECK(c1 == NULL || c1->next == NULL);
    }
    /* SupportedDeviceFamilies: [1]. */
    v = bp_dict_get(extra, "9MZ5AdH43csAUajl/dU+IQ");
    CHECK(v != NULL && v->type == BP_ARRAY);
    if (v != NULL && v->type == BP_ARRAY) {
        bp_value *c0 = v->child;
        CHECK(c0 != NULL && c0->type == BP_INT && c0->i == 1);
        CHECK(c0 == NULL || c0->next == NULL);
    }
    /* ArtworkTraits sub-dict. */
    v = bp_dict_get(extra, "oPeik/9e8lQWMszEjbPzng");
    CHECK(v != NULL && v->type == BP_DICT);
    if (v != NULL && v->type == BP_DICT) {
        bp_value *a;
        a = bp_dict_get(v, "ArtworkDeviceIdiom");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "phone") == 0);
        a = bp_dict_get(v, "ArtworkDeviceSubType");
        CHECK(a != NULL && a->type == BP_INT && a->i == 2778);
        a = bp_dict_get(v, "ArtworkDeviceScaleFactor");
        CHECK(a != NULL && a->type == BP_INT && a->i == 3);
        a = bp_dict_get(v, "ArtworkDisplayGamut");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "P3") == 0);
        a = bp_dict_get(v, "ArtworkDynamicDisplayMode");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "0") == 0);
        a = bp_dict_get(v, "ArtworkDeviceProductDescription");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "iPhone 16") == 0);
        a = bp_dict_get(v, "GraphicsFeatureSetClass");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "APPLE8") == 0);
        a = bp_dict_get(v, "GraphicsFeatureSetFallbacks");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s,
                     "APPLE7:APPLE6:APPLE5:APPLE4:APPLE3:APPLE2:APPLE1:GLES2,0") == 0);
        a = bp_dict_get(v, "CompatibleDeviceFallback");
        CHECK(a != NULL && a->type == BP_STRING &&
              strcmp((const char *)a->s, "iPhone16,1") == 0);
    }
    /* GPU buffer-compression sub-dict. */
    v = bp_dict_get(extra, "AoKnINTLPoKML3ctoP0AZg");
    CHECK(v != NULL && v->type == BP_DICT);
    if (v != NULL && v->type == BP_DICT) {
        bp_value *a;
        a = bp_dict_get(v, "buffer-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
        a = bp_dict_get(v, "media-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
        a = bp_dict_get(v, "universal-buffer-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 1);
        a = bp_dict_get(v, "universal-buffer-compression-requires-plane-alignment-2048");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
        a = bp_dict_get(v, "universal-lossy-buffer-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
        a = bp_dict_get(v, "universal-multislice-buffer-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
        a = bp_dict_get(v, "universal-pARGB10101010-buffer-compression");
        CHECK(a != NULL && a->type == BP_BOOL && a->b == 0);
    }
    /* All-zero 52-byte opaque field. */
    v = bp_dict_get(extra, "91LyMcx4z1w3SGVeqteMnA");
    CHECK(v != NULL && v->type == BP_DATA && v->len == 52);
    if (v != NULL && v->type == BP_DATA && v->len == 52) {
        uint32_t z;
        int all_zero = 1;
        for (z = 0; z < v->len; z++) {
            if (v->s[z] != 0) {
                all_zero = 0;
                break;
            }
        }
        CHECK(all_zero);
    }
    /* Protected and dynamic questions must NOT be in the cache. */
    CHECK(bp_dict_get(extra, "re6Zb+zwFKJNlkQTUeT+/w") == NULL);
    CHECK(bp_dict_get(extra, "VasUgeSzVyHdB27g2XpN0g") == NULL);
    CHECK(bp_dict_get(extra, "k5lVWbXuiZHLA17KGiVUAA") == NULL);
    CHECK(bp_dict_get(extra, "l/n0Z7pMB8k0GyTKz3v3Tw") == NULL);
    CHECK(bp_dict_get(extra, "f2DlVMUVcV+MeWs/g2ku+g") == NULL);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: cache file on disk.                                     */
/* ------------------------------------------------------------------ */

static char g_tmpdir[108];
static char g_cache_path[MG_CACHE_PATH_CAP];

/* Write len bytes to path with libc. Returns 0 or -1. */
static int write_file_bytes(const char *path, const uint8_t *buf,
                            uint32_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint32_t off = 0;
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (uint32_t)w;
    }
    if (close(fd) != 0) return -1;
    return 0;
}

static void scen_3_cache_on_disk(void) {
    char expect[MG_CACHE_PATH_CAP];
    uint8_t buf[MG_CACHE_BUILD_CAP];
    uint32_t len = 0;
    SCEN_BEGIN(3, "cache file on disk (valid / stale / missing / corrupt)");
    /* mg_init already wrote the file; the path is the container path. */
    CHECK_RC(mg_cache_path(g_cache_path, sizeof(g_cache_path)));
    snprintf(expect, sizeof(expect),
             "%s/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache"
             "/Library/Caches/com.apple.MobileGestalt.plist", g_tmpdir);
    CHECK(strcmp(g_cache_path, expect) == 0);
    CHECK(mg_cache_valid() == 1);
    /* The file on disk starts with the bplist00 magic. */
    {
        int fd = open(g_cache_path, O_RDONLY);
        uint8_t magic[8];
        ssize_t n = -1;
        CHECK(fd >= 0);
        if (fd >= 0) {
            n = read(fd, magic, sizeof(magic));
            close(fd);
        }
        CHECK(n == 8 && memcmp(magic, "bplist00", 8) == 0);
    }
    /* mg_init is idempotent. */
    CHECK(mg_init() == KERN_SUCCESS);
    CHECK(mg_init() == KERN_SUCCESS);
    /* Stale: CacheVersion 23G89 must be rejected, then restored. */
    {
        static uint8_t arena_buf[MG_CACHE_ARENA_CAP];
        bp_arena arena;
        bp_value *root = NULL;
        bp_value *v = NULL;
        CHECK_RC(mg_build_cache(buf, sizeof(buf), &len));
        bp_arena_init(&arena, arena_buf, sizeof(arena_buf));
        CHECK_RC(bp_parse(buf, len, &arena, &root));
        v = bp_new_string(&arena, "23G89");
        CHECK(v != NULL);
        if (v != NULL)
            CHECK_RC(bp_dict_set(&arena, root, "CacheVersion", v));
        CHECK_RC(bp_write(root, buf, sizeof(buf), &len));
        CHECK(write_file_bytes(g_cache_path, buf, len) == 0);
        CHECK(mg_cache_valid() == 0);
        CHECK_RC(mg_ensure_cache());
        CHECK(mg_cache_valid() == 1);
    }
    /* Missing: valid() is false, ensure_cache() restores it. */
    CHECK(unlink(g_cache_path) == 0);
    CHECK(mg_cache_valid() == 0);
    CHECK_RC(mg_ensure_cache());
    CHECK(mg_cache_valid() == 1);
    /* Corrupt: garbage bytes are rejected, then restored. */
    {
        uint8_t garbage[64];
        uint32_t z;
        for (z = 0; z < sizeof(garbage); z++) garbage[z] = 0xaa;
        CHECK(write_file_bytes(g_cache_path, garbage,
                               sizeof(garbage)) == 0);
        CHECK(mg_cache_valid() == 0);
        CHECK_RC(mg_ensure_cache());
        CHECK(mg_cache_valid() == 1);
    }
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: helper plain-dict queries.                              */
/* ------------------------------------------------------------------ */

/* Reply answer readers. Each returns XW_OK on a matching answer. */

static int reply_string(xw_reader *rd, const char **out) {
    xw_value v;
    uint32_t len = 0;
    if (xw_dict_find(rd, TEST_ANSWER_KEY, &v) != XW_OK) return -1;
    if (xw_as_string(&v, out, &len) != XW_OK) return -1;
    return XW_OK;
}

static int reply_int64(xw_reader *rd, int64_t *out) {
    xw_value v;
    if (xw_dict_find(rd, TEST_ANSWER_KEY, &v) != XW_OK) return -1;
    return xw_as_int64(&v, out);
}

static int reply_bool(xw_reader *rd, int *out) {
    xw_value v;
    if (xw_dict_find(rd, TEST_ANSWER_KEY, &v) != XW_OK) return -1;
    return xw_as_bool(&v, out);
}

static int reply_double(xw_reader *rd, double *out) {
    xw_value v;
    if (xw_dict_find(rd, TEST_ANSWER_KEY, &v) != XW_OK) return -1;
    return xw_as_double(&v, out);
}

static int reply_error(xw_reader *rd, int64_t *out) {
    xw_value v;
    if (xw_dict_find(rd, TEST_ERROR_KEY, &v) != XW_OK) return -1;
    return xw_as_int64(&v, out);
}

static void check_answer_string(const char *question,
                                const char *expect) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    const char *s = NULL;
    int64_t err = -1;
    CHECK(ask(question, reply, sizeof(reply), &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
    CHECK(reply_string(&rd, &s) == XW_OK && s != NULL &&
          strcmp(s, expect) == 0);
}

static void check_answer_int(const char *question, int64_t expect) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    int64_t v = 0;
    int64_t err = -1;
    CHECK(ask(question, reply, sizeof(reply), &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
    CHECK(reply_int64(&rd, &v) == XW_OK && v == expect);
}

static void check_answer_bool(const char *question, int expect) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    int v = -1;
    int64_t err = -1;
    CHECK(ask(question, reply, sizeof(reply), &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
    CHECK(reply_bool(&rd, &v) == XW_OK && v == expect);
}

static void check_answer_double(const char *question, double expect) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    double v = -1.0;
    int64_t err = -1;
    CHECK(ask(question, reply, sizeof(reply), &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
    CHECK(reply_double(&rd, &v) == XW_OK && v == expect);
}

static void scen_4_helper_queries(void) {
    SCEN_BEGIN(4, "helper plain-dict queries");
    check_answer_string("ProductType", "iPhone17,3");
    check_answer_string("ProductVersion", "26.6.2");
    check_answer_string("BuildVersion", "23G90");
    check_answer_string("DeviceClass", "iPhone");
    check_answer_string("HWModelStr", "D47AP");
    check_answer_string("CPUArchitecture", "arm64e");
    check_answer_string("RegulatoryModelNumber", "A3287");
    check_answer_string("RegionCode", "LL");
    check_answer_string("MarketingNameString", "iPhone 16");
    check_answer_int("DeviceClassNumber", 1);
    check_answer_int("main-screen-width", 1440);
    check_answer_int("main-screen-height", 3200);
    check_answer_int("main-screen-pitch", 521);
    check_answer_int("main-screen-class", 8);
    check_answer_int("MainDisplayRotation", 0);
    check_answer_bool("IsSimulator", 0);
    check_answer_bool("ipad", 0);
    check_answer_bool("MetalCapability", 1);
    check_answer_bool("AppleInternalInstallCapability", 0);
    check_answer_bool("HasExtendedColorDisplay", 1);
    check_answer_bool("SupportsForceTouch", 0);
    check_answer_double("main-screen-scale", 4.0);
    check_answer_double("DeviceCornerRadius", 0.0);
    check_answer_double("MainScreenOrientation", 0.0);
    /* Data answer: MainScreenStaticInfo, 24 bytes. */
    {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        const uint8_t *ptr = NULL;
        uint32_t len = 0;
        uint32_t desc = 0;
        uint8_t expect[24];
        int n;
        int64_t err = -1;
        CHECK(ask("MainScreenStaticInfo", reply, sizeof(reply),
                  &reply_size, &rd) == XW_OK);
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(xw_as_data(&v, &ptr, &len, &desc) == XW_OK);
        CHECK(ptr != NULL && len == 24 && desc == XW_NO_DESC);
        n = test_b64_decode("oAUAAIAMAAAJAgAAAACAQAAAAAAIAAAA",
                            expect, sizeof(expect));
        CHECK(n == 24);
        if (ptr != NULL && len == 24 && n == 24)
            CHECK(memcmp(ptr, expect, 24) == 0);
    }
    /* Data answer: MainScreenCanvasSizes, one CGSize. */
    {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        const uint8_t *ptr = NULL;
        uint32_t len = 0;
        uint32_t desc = 0;
        uint8_t expect[16];
        double w = 1440.0, h = 3200.0;
        int64_t err = -1;
        memcpy(expect, &w, 8);
        memcpy(expect + 8, &h, 8);
        CHECK(ask("MainScreenCanvasSizes", reply, sizeof(reply),
                  &reply_size, &rd) == XW_OK);
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(xw_as_data(&v, &ptr, &len, &desc) == XW_OK);
        CHECK(ptr != NULL && len == 16 && desc == XW_NO_DESC);
        if (ptr != NULL && len == 16)
            CHECK(memcmp(ptr, expect, 16) == 0);
    }
    /* Array answer: CompatibleAppVariants. */
    {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        uint32_t n = 0;
        xw_value e0, e1;
        const char *s0 = NULL, *s1 = NULL;
        uint32_t l0 = 0, l1 = 0;
        int64_t err = -1;
        CHECK(ask("CompatibleAppVariants", reply, sizeof(reply),
                  &reply_size, &rd) == XW_OK);
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(xw_array_len_v(&v, &n) == XW_OK && n == 2);
        CHECK(xw_array_at_v(&v, 0, &e0) == XW_OK);
        CHECK(xw_array_at_v(&v, 1, &e1) == XW_OK);
        CHECK(xw_as_string(&e0, &s0, &l0) == XW_OK &&
              strcmp(s0, "iPhone17,3") == 0);
        CHECK(xw_as_string(&e1, &s1, &l1) == XW_OK &&
              strcmp(s1, "iPhone16,1") == 0);
    }
    /* Array answer: SupportedDeviceFamilies. */
    {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        uint32_t n = 0;
        xw_value e0;
        int64_t i0 = -1;
        int64_t err = -1;
        CHECK(ask("SupportedDeviceFamilies", reply, sizeof(reply),
                  &reply_size, &rd) == XW_OK);
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(xw_array_len_v(&v, &n) == XW_OK && n == 1);
        CHECK(xw_array_at_v(&v, 0, &e0) == XW_OK);
        CHECK(xw_as_int64(&e0, &i0) == XW_OK && i0 == 1);
    }
    /* Dict answer: ArtworkTraits. */
    {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        xw_value a;
        const char *s = NULL;
        uint32_t sl = 0;
        int64_t iv = -1;
        int64_t err = -1;
        CHECK(ask("ArtworkTraits", reply, sizeof(reply),
                  &reply_size, &rd) == XW_OK);
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(v.type == XW_TYPE_DICTIONARY);
        CHECK(xw_dict_find_v(&v, "ArtworkDeviceIdiom", &a) == XW_OK);
        CHECK(xw_as_string(&a, &s, &sl) == XW_OK &&
              strcmp(s, "phone") == 0);
        CHECK(xw_dict_find_v(&v, "ArtworkDeviceScaleFactor", &a) == XW_OK);
        CHECK(xw_as_int64(&a, &iv) == XW_OK && iv == 3);
        CHECK(xw_dict_find_v(&v, "GraphicsFeatureSetClass", &a) == XW_OK);
        CHECK(xw_as_string(&a, &s, &sl) == XW_OK &&
              strcmp(s, "APPLE8") == 0);
    }
    /* Fallback scan: the question under a different key still
     * answers (this is what lets the handler cope with the real
     * NSXPC shape until the trace lands). */
    {
        uint8_t body[512];
        uint32_t body_len = 0;
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_builder b;
        xw_reader rd;
        const char *s = NULL;
        int64_t err = -1;
        int r = xw_builder_init(&b, body, sizeof(body));
        if (r == XW_OK) r = xw_dict_begin(&b);
        if (r == XW_OK) r = xw_dict_add_string(&b, "request",
                                              "ProductType");
        if (r == XW_OK) r = xw_dict_end(&b);
        CHECK(r == XW_OK);
        body_len = xw_builder_len(&b);
        CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                      reply, sizeof(reply), &reply_size) == 1);
        {
            xw_envelope env;
            CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        }
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(reply_string(&rd, &s) == XW_OK &&
              strcmp(s, "iPhone17,3") == 0);
    }
    /* Verbatim trace smoke test: tracing on must not change the
     * answer (the log goes to fd 2). */
    bsxpc_set_trace(1);
    check_answer_string("ProductType", "iPhone17,3");
    bsxpc_set_trace(-1);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: unknown question.                                       */
/* ------------------------------------------------------------------ */

static void scen_5_unknown(void) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    xw_value v;
    int64_t err = -1;
    SCEN_BEGIN(5, "unknown question");
    CHECK(ask("NoSuchQuestion", reply, sizeof(reply), &reply_size,
              &rd) == XW_OK);
    CHECK(reply_error(&rd, &err) == XW_OK && err == 1);
    /* An error reply carries no answer key. */
    CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) != XW_OK);
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: protected and dynamic questions.                        */
/* ------------------------------------------------------------------ */

static const char *g_protected_q[] = {
    "UniqueDeviceID", "UniqueDeviceIDData", "SerialNumber",
    "MLBSerialNumber", "BluetoothAddress", "InverseDeviceID",
    "UniqueChipID", "MobileEquipmentIdentifier",
};

static const char *g_dynamic_q[] = {
    "BatteryCurrentCapacity", "BatteryIsCharging", "AirplaneMode",
    "SIMStatus", "ActiveWirelessTechnology",
};

static void scen_6_protected_dynamic(void) {
    size_t i;
    SCEN_BEGIN(6, "protected denial + dynamic NULL");
    for (i = 0; i < sizeof(g_protected_q) / sizeof(g_protected_q[0]);
         i++) {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        int64_t err = -1;
        CHECK(ask(g_protected_q[i], reply, sizeof(reply), &reply_size,
                  &rd) == XW_OK);
        /* Denied: NULL answer plus an error, never a value. */
        CHECK(reply_error(&rd, &err) == XW_OK && err == 1);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(v.type == XW_TYPE_NULL);
    }
    for (i = 0; i < sizeof(g_dynamic_q) / sizeof(g_dynamic_q[0]); i++) {
        uint8_t reply[SVCHOST_MAX_REPLY];
        uint32_t reply_size = 0;
        xw_reader rd;
        xw_value v;
        int64_t err = -1;
        CHECK(ask(g_dynamic_q[i], reply, sizeof(reply), &reply_size,
                  &rd) == XW_OK);
        /* Dynamic: NULL answer, no error. */
        CHECK(reply_error(&rd, &err) == XW_OK && err == 0);
        CHECK(xw_dict_find(&rd, TEST_ANSWER_KEY, &v) == XW_OK);
        CHECK(v.type == XW_TYPE_NULL);
    }
    /* Direct table checks. */
    CHECK(mg_is_protected("UniqueDeviceID") == 1);
    CHECK(mg_is_protected("SerialNumber") == 1);
    CHECK(mg_is_protected("ProductType") == 0);
    CHECK(mg_is_dynamic("AirplaneMode") == 1);
    CHECK(mg_is_dynamic("ProductType") == 0);
    {
        mg_answer_t ans;
        CHECK(mg_answer_for("ProductType", &ans) == 0);
        CHECK(ans.kind == MG_ANS_STRING &&
              strcmp(ans.s, "iPhone17,3") == 0);
        CHECK(mg_answer_for("NoSuchQuestion", &ans) != 0);
        CHECK(mg_answer_for("UniqueDeviceID", &ans) != 0);
        CHECK(mg_answer_for(NULL, &ans) != 0);
    }
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: malformed input.                                        */
/* ------------------------------------------------------------------ */

static void check_malformed_body(const uint8_t *body, uint32_t body_len,
                                 const char *what) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    int64_t err = -1;
    int ret = deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                      reply, sizeof(reply), &reply_size);
    if (ret != 1) {
        printf("  malformed %s: bad return %d\n", what, ret);
        CHECK(0);
        return;
    }
    if (reply_error_code(reply, reply_size, &err) != XW_OK || err != 22) {
        printf("  malformed %s: error %lld (expected 22)\n", what,
               (long long)err);
        CHECK(0);
        return;
    }
    CHECK(1);
}

static void scen_7_malformed(void) {
    uint8_t body[512];
    uint32_t body_len = 0;
    xw_builder b;
    int r;
    SCEN_BEGIN(7, "malformed input");
    /* Truncated XPC body (valid dict cut in half). */
    CHECK_RC(build_question_body(body, sizeof(body), "ProductType",
                                 &body_len));
    CHECK(body_len > 8);
    check_malformed_body(body, body_len / 2, "truncated body");
    /* Non-string question. */
    r = xw_builder_init(&b, body, sizeof(body));
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_int64(&b, TEST_QUESTION_KEY, 42);
    if (r == XW_OK) r = xw_dict_end(&b);
    CHECK(r == XW_OK);
    check_malformed_body(body, xw_builder_len(&b), "non-string question");
    /* Empty question string. */
    CHECK_RC(build_question_body(body, sizeof(body), "", &body_len));
    check_malformed_body(body, body_len, "empty question");
    /* Empty dict. */
    r = xw_builder_init(&b, body, sizeof(body));
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    CHECK(r == XW_OK);
    check_malformed_body(body, xw_builder_len(&b), "empty dict");
    /* Array root instead of a dict. */
    r = xw_builder_init(&b, body, sizeof(body));
    if (r == XW_OK) r = xw_array_begin(&b);
    if (r == XW_OK) r = xw_add_int64(&b, 1);
    if (r == XW_OK) r = xw_add_int64(&b, 2);
    if (r == XW_OK) r = xw_array_end(&b);
    CHECK(r == XW_OK);
    check_malformed_body(body, xw_builder_len(&b), "array root");
    /* Garbage bytes (not an XPC body at all). */
    {
        uint8_t garbage[16];
        uint32_t z;
        for (z = 0; z < sizeof(garbage); z++) garbage[z] = (uint8_t)(0x41 + z);
        check_malformed_body(garbage, sizeof(garbage), "garbage body");
    }
    /* Zero-length body. */
    check_malformed_body(body, 0, "zero-length body");
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: NSXPC wire invocation replay.                           */
/* ------------------------------------------------------------------ */

static int build_nsxpc_request(uint8_t *body, uint32_t cap,
                               uint64_t seq, const char *replysig,
                               const char *selector, const char *question,
                               uint32_t *out_len) {
    uint8_t bplist[512];
    uint32_t off = 0;

    /* 1. bplist17 magic */
    memcpy(bplist + off, "bplist17", 8);
    off += 8;

    /* 2. Container 0xd0 */
    bplist[off++] = 0xd0;
    uint32_t cont_end_pos = off;
    off += 8; /* placeholder */

    /* 3. Selector */
    size_t slen = strlen(selector);
    if (slen < 15) {
        bplist[off++] = (uint8_t)(0x70 | slen);
    } else {
        bplist[off++] = 0x7f;
        bplist[off++] = 0x11;
        bplist[off++] = (uint8_t)slen;
    }
    memcpy(bplist + off, selector, slen);
    off += (uint32_t)slen;

    /* 4. Arguments array */
    if (question != NULL) {
        bplist[off++] = 0xa1; /* 1 element */
        uint32_t arr_end_pos = off;
        off += 8; /* placeholder */

        size_t qlen = strlen(question);
        if (qlen < 15) {
            bplist[off++] = (uint8_t)(0x50 | qlen);
        } else {
            bplist[off++] = 0x5f;
            bplist[off++] = 0x11;
            bplist[off++] = (uint8_t)qlen;
        }
        memcpy(bplist + off, question, qlen);
        off += (uint32_t)qlen;

        /* write array end offset */
        uint64_t a_end = off - 1;
        memcpy(bplist + arr_end_pos, &a_end, 8);
    } else {
        bplist[off++] = 0xa0; /* 0 elements */
        uint32_t arr_end_pos = off;
        off += 8; /* placeholder */
        uint64_t a_end = off - 1;
        memcpy(bplist + arr_end_pos, &a_end, 8);
    }

    /* write container end offset */
    uint64_t c_end = off - 1;
    memcpy(bplist + cont_end_pos, &c_end, 8);

    /* Build outer XPC dictionary */
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "proxynum", 1);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "sequence", seq);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "f", 0x41);
    if (r == XW_OK && replysig) r = xw_dict_add_string(&b, "replysig", replysig);
    if (r == XW_OK) r = xw_dict_add_data(&b, "root", bplist, off);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

static void check_nsxpc_reply(const uint8_t *reply_buf, uint32_t reply_size,
                              const char *expected_selector_or_sig,
                              uint8_t expected_arg_tag,
                              const char *expected_str) {
    xw_envelope env;
    xw_reader rd;
    xw_value root_v;
    const uint8_t *p = NULL;
    uint32_t len = 0;
    uint32_t desc = 0;

    CHECK_RC(parse_reply(reply_buf, reply_size, &env, &rd));
    CHECK_RC(xw_dict_find(&rd, "root", &root_v));
    CHECK(root_v.type == XW_TYPE_DATA);
    CHECK_RC(xw_as_data(&root_v, &p, &len, &desc));
    CHECK(p != NULL);
    CHECK(len >= 18);
    CHECK(memcmp(p, "bplist17", 8) == 0);
    CHECK(p[8] == 0xd0);
    CHECK(p[17] == 0xe0); /* reply marker */

    /* Reply Method Signature (encoded with tag 0x70) */
    const char *expected_sig = nsxpc_reply_signature(expected_selector_or_sig);
    uint32_t off = 18;
    uint8_t sig_tag = p[off++];
    CHECK((sig_tag & 0xf0) == 0x70);
    uint32_t sig_len = sig_tag & 0x0f;
    if (sig_len == 0x0f) {
        CHECK(p[off++] == 0x11);
        sig_len = p[off++];
    }
    CHECK(sig_len == strlen(expected_sig));
    CHECK(memcmp(p + off, expected_sig, sig_len) == 0);
    off += sig_len;

    /* Arguments array */
    uint8_t arr_tag = p[off++];
    CHECK(arr_tag == 0xa1); /* 1 return argument */
    off += 8; /* skip array end_offset */

    /* Check argument tag */
    uint8_t arg_tag = p[off++];
    CHECK(arg_tag == expected_arg_tag);

    if (expected_str != NULL) {
        uint32_t str_len = arg_tag & 0x0f;
        if (str_len == 0x0f) {
            CHECK(p[off++] == 0x11);
            str_len = p[off++];
        }
        CHECK(str_len == strlen(expected_str));
        CHECK(memcmp(p + off, expected_str, str_len) == 0);
    }
}

static void scen_8_nsxpc_replay(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0;
    uint32_t reply_size = 0;

    SCEN_BEGIN(8, "NSXPC invocation replay (bplist17 wire protocol)");

    /* 0. Unit tests for nsxpc_reply_signature resolution */
    CHECK(strcmp(nsxpc_reply_signature("v@?@"), "v@?@") == 0);
    CHECK(strcmp(nsxpc_reply_signature("v@?B"), "v@?B") == 0);
    CHECK(strcmp(nsxpc_reply_signature("getServerAnswerForQuestion:reply:"), "v@?@") == 0);
    CHECK(strcmp(nsxpc_reply_signature("rebuildCache:"), "v@?B") == 0);
    CHECK(strcmp(nsxpc_reply_signature("setCacheSentinel:"), "v@?B") == 0);
    CHECK(strcmp(nsxpc_reply_signature("getSpringboardRegionOverride:reply:"), "v@?@") == 0);
    CHECK(strcmp(nsxpc_reply_signature("unknownCustomMethod:reply:"), "v@?@") == 0);
    CHECK(strcmp(nsxpc_reply_signature(NULL), "v@?@") == 0);

    /* 1. getServerAnswerForQuestion:reply: for ProductType -> iPhone17,3 */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 1, "v@?@",
                                 "getServerAnswerForQuestion:reply:",
                                 "ProductType", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    CHECK(reply_size > sizeof(mach_msg_header_t));
    check_nsxpc_reply(reply, reply_size, "getServerAnswerForQuestion:reply:",
                      0x5a, "iPhone17,3");
    check_nsxpc_reply(reply, reply_size, "v@?@", 0x5a, "iPhone17,3");

    /* 2. getServerAnswerForQuestion:reply: for BuildVersion -> 23G90 */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 2, "v@?@",
                                 "getServerAnswerForQuestion:reply:",
                                 "BuildVersion", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "getServerAnswerForQuestion:reply:",
                      0x55, "23G90");
    check_nsxpc_reply(reply, reply_size, "v@?@", 0x55, "23G90");

    /* 2b. Request without replysig in dict (fallback path in svchost handler) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 20, NULL,
                                 "getServerAnswerForQuestion:reply:",
                                 "ProductType", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0x5a, "iPhone17,3");

    /* 3. rebuildCache: -> BOOL true (0xb0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 3, "v@?B",
                                 "rebuildCache:", NULL, &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "rebuildCache:", 0xb0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?B", 0xb0, NULL);

    /* 3b. rebuildCache: without replysig in dict (fallback path) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 30, NULL,
                                 "rebuildCache:", NULL, &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "v@?B", 0xb0, NULL);

    /* 4. setCacheSentinel: -> BOOL true (0xb0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 4, "v@?B",
                                 "setCacheSentinel:", NULL, &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "setCacheSentinel:", 0xb0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?B", 0xb0, NULL);

    /* 5. getSpringboardRegionOverride:reply: -> nil (0xe0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 5, "v@?@",
                                 "getSpringboardRegionOverride:reply:",
                                 NULL, &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size,
                      "getSpringboardRegionOverride:reply:", 0xe0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0xe0, NULL);

    /* 6. Protected question denial -> nil (0xe0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 6, "v@?@",
                                 "getServerAnswerForQuestion:reply:",
                                 "UniqueDeviceID", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "getServerAnswerForQuestion:reply:",
                      0xe0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0xe0, NULL);

    /* 7. Dynamic question -> nil (0xe0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 7, "v@?@",
                                 "getServerAnswerForQuestion:reply:",
                                 "BatteryCurrentCapacity", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "getServerAnswerForQuestion:reply:",
                      0xe0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0xe0, NULL);

    /* 8. Unknown question -> nil (0xe0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 8, "v@?@",
                                 "getServerAnswerForQuestion:reply:",
                                 "NoSuchQuestionName", &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "getServerAnswerForQuestion:reply:",
                      0xe0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0xe0, NULL);

    /* 9. Unknown selector -> nil (0xe0) */
    CHECK_RC(build_nsxpc_request(body, sizeof(body), 9, "v@?@",
                                 "unknownCustomMethod:reply:",
                                 NULL, &body_len));
    CHECK(deliver(g_mg_port, SIM_REPLY_PORT, body, body_len,
                  reply, sizeof(reply), &reply_size) == 1);
    check_nsxpc_reply(reply, reply_size, "unknownCustomMethod:reply:",
                      0xe0, NULL);
    check_nsxpc_reply(reply, reply_size, "v@?@", 0xe0, NULL);

    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: fuzz mg_process_msg.                                    */
/* ------------------------------------------------------------------ */

static void scen_9_fuzz_ipc(void) {
    uint8_t valid[2048];
    uint8_t scratch[4096];
    uint8_t reply[2048];
    uint32_t valid_size = 0;
    uint32_t body_len = 0;
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(9, "fuzz mg_process_msg (10000 iterations)");
    CHECK_RC(build_question_body(valid + 24, sizeof(valid) - 24,
                                 "ProductType", &body_len));
    valid_size = build_mach_msg(valid, sizeof(valid), g_mg_port,
                                SIM_REPLY_PORT, valid + 24, body_len);
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
        if ((rng_next() % 8u) == 0)
            send_size = rng_next() % (valid_size + 1);
        /* Sometimes drop the reply port (one-way message). */
        if ((rng_next() % 16u) == 0)
            put_u32le(scratch + 12, 0);
        ret = mg_process_msg((const mach_msg_header_t *)scratch,
                             send_size, reply, sizeof(reply),
                             &reply_size);
        if (ret != 0 && ret != 1) {
            if (violations < 5)
                printf("\n  fuzz iter %d: bad return %d\n", i, ret);
            violations++;
            continue;
        }
        if (ret == 1 && reply_size > 0) {
            if (reply_size < (uint32_t)sizeof(mach_msg_header_t) + 40) {
                if (violations < 5)
                    printf("\n  fuzz iter %d: short reply %u\n", i,
                           reply_size);
                violations++;
                continue;
            }
            {
                xw_envelope env;
                xw_reader rd;
                if (parse_reply(reply, reply_size, &env, &rd) != XW_OK) {
                    if (violations < 5)
                        printf("\n  fuzz iter %d: reply does not parse\n",
                               i);
                    violations++;
                }
            }
        }
    }
    printf("  fuzz: 10000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* Scenario 10: fuzz bp_parse over mutated cache bytes.                */
/* ------------------------------------------------------------------ */

static void scen_10_fuzz_bplist(void) {
    static uint8_t arena_buf[MG_CACHE_ARENA_CAP];
    uint8_t scratch[MG_CACHE_BUILD_CAP];
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(10, "fuzz bp_parse over cache bytes (5000 iterations)");
    CHECK(g_cache_len > 40u);
    if (g_cache_len <= 40u) {
        /* No cache bytes to fuzz (scenario 2 failed); do not divide
         * by the zero length below. */
        SKIP("no cache bytes to fuzz (scenario 2 failed)");
        SCEN_END(10);
        return;
    }
    for (i = 0; i < 5000; i++) {
        bp_arena arena;
        bp_value *root = NULL;
        int rc;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        uint32_t len = g_cache_len;
        memcpy(scratch, g_cache_bytes, g_cache_len);
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % g_cache_len;
            scratch[off] = (uint8_t)rng_next();
        }
        if ((rng_next() % 8u) == 0)
            len = rng_next() % (g_cache_len + 1);
        bp_arena_init(&arena, arena_buf, sizeof(arena_buf));
        rc = bp_parse(scratch, len, &arena, &root);
        if (rc != 0 && rc >= 0) {
            if (violations < 5)
                printf("\n  fuzz iter %d: bad bp_parse rc %d\n", i, rc);
            violations++;
            continue;
        }
        if (rc == 0 && root == NULL) {
            if (violations < 5)
                printf("\n  fuzz iter %d: parse ok but root NULL\n", i);
            violations++;
        }
    }
    printf("  fuzz: 5000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(10);
}

/* ------------------------------------------------------------------ */
/* Scenario 11: reply size.                                            */
/* ------------------------------------------------------------------ */

static void scen_11_reply_size(void) {
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t reply_size = 0;
    xw_reader rd;
    SCEN_BEGIN(11, "reply size");
    CHECK(ask("CompatibleAppVariants", reply, sizeof(reply),
              &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_size - (uint32_t)sizeof(mach_msg_header_t) <=
          SVCHOST_MAX_REPLY_BODY);
    CHECK(ask("MainScreenStaticInfo", reply, sizeof(reply),
              &reply_size, &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_size - (uint32_t)sizeof(mach_msg_header_t) <=
          SVCHOST_MAX_REPLY_BODY);
    CHECK(ask("ArtworkTraits", reply, sizeof(reply), &reply_size,
              &rd) == XW_OK);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    CHECK(reply_size - (uint32_t)sizeof(mach_msg_header_t) <=
          SVCHOST_MAX_REPLY_BODY);
    SCEN_END(11);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    char tmpl[] = "/tmp/mg_test_XXXXXX";
    char *tmp = NULL;
    char rm_cmd[256];
    int rc = 0;
    printf("mg_test: MobileGestalt service host test\n");
    tmp = mkdtemp(tmpl);
    if (tmp == NULL) {
        printf("FAIL: mkdtemp failed\n");
        return 1;
    }
    strncpy(g_tmpdir, tmp, sizeof(g_tmpdir) - 1);
    g_tmpdir[sizeof(g_tmpdir) - 1] = '\0';
    CHECK_RC(container_set_home(g_tmpdir));
    /* Play the launcher: pre-register the MobileGestalt system
     * group under the temp home (see mg.h for the real-launcher
     * requirement). Without this, container_group() returns
     * NOT_FOUND and no cache file is ever written. */
    {
        char group_path[MG_CACHE_PATH_CAP];
        snprintf(group_path, sizeof(group_path),
                 "%s/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache",
                 g_tmpdir);
        CHECK_RC(container_set_group(
            "systemgroup.com.apple.mobilegestaltcache", group_path));
    }

    scen_1_obfuscation();
    scen_2_cache_roundtrip();

    /* The service needs its port before the XPC scenarios. */
    CHECK(mg_init() == KERN_SUCCESS);
    g_mg_port = svchost_lookup_port("com.apple.mobilegestalt.xpc");
    CHECK(g_mg_port != MACH_PORT_NULL);

    scen_3_cache_on_disk();
    scen_4_helper_queries();
    scen_5_unknown();
    scen_6_protected_dynamic();
    scen_7_malformed();
    scen_8_nsxpc_replay();
    scen_9_fuzz_ipc();
    scen_10_fuzz_bplist();
    scen_11_reply_size();

    /* Clean up the temp dir. */
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", g_tmpdir);
    rc = system(rm_cmd);
    (void)rc;

    printf("TOTAL: %d checks, %d failures, %d skipped\n", g_checks,
           g_failures, g_skipped);
    return g_failures != 0 ? 1 : 0;
}
