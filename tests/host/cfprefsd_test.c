/* cfprefsd_test.c: host test for the fake cfprefsd.
 *
 * Drives cfprefsd_process_msg directly with hand-built Mach
 * messages (header + XPC body from xw_builder) and checks every
 * reply with xw_reader, per the harness rules. The
 * domain plists live in a temp dir under /tmp (mkdtemp), used as
 * HOME, and are removed at the end.
 *
 * Scenarios:
 *   1. init: both service names registered, idempotent second call.
 *   2. T1 op 1 set, verbatim DEF CON sample (pre-created empty
 *      plist, O_RDWR access-token fd).
 *   3. T1b first write: no domain file yet, no token (first-write
 *      rule, see cfprefsd.h).
 *   4. T2 op 5 multi: two op 1 sub-messages, one reply, both keys set.
 *   5. T3 op 8 set-multiple [guess]: three inline string pairs.
 *   6. T4 op 1 without an access token on an existing file: error 22,
 *      file unchanged.
 *   7. T5 op 1 with an O_RDONLY access-token fd: error 22, unchanged.
 *   8. T7/T8 malformed (no Operation key) and unknown op (99, and 2,
 *      the hypothesized get number): 22 and 1.
 *   9. T9 reply size: a multi setting 40 keys with 40-byte values
 *      still answers a reply under 1024 bytes, no error 12.
 *  10. T11 ByHost set: file lands under Preferences/ByHost/ with
 *      the placeholder host id.
 *  11. T12 NSGlobalDomain set: file is .GlobalPreferences.plist.
 *  12. T13 domain traversal ("foo/bar", "..", ""): error 22, nothing
 *      created outside Preferences.
 *  13. T14 missing Preferences directory: error 22 (never created).
 *  14. T15 verbatim trace behind IOSWINE_SVC_TRACE / bsxpc_set_trace.
 *  15. T10 fuzz: 10000 mutated/truncated messages, no crash, every
 *      reply parses.
 * T6 (get) is RESERVED: the operation number is unknown, so there is
 * no test for it; scenario 8 checks that op 2 (the hypothesized get
 * number) is rejected with error 1 rather than silently accepted.
 *
 * How the service is driven on host: there is no real ipc.c here, so
 * this file stubs the ipc_* symbols the .c files reference (found
 * with `nm -u` on the compiled objects: g_ipc_trace, lsk_getenv,
 * ipc_task_self, ipc_mach_reply_port, ipc_svchost_port_create,
 * ipc_port_from_name, ipc_port_deallocate, ipc_port_destroy,
 * ipc_msg). ipc_msg emulates ONLY the bootstrap_register2 path
 * (msgh_id 0x100000cc) that svchost_register's self-registration
 * uses: it records name -> port in a small static table and builds
 * a reply {"error": 0} with msgh_id 0x20000000 in the same buffer.
 * Any other msgh_id returns MACH_SEND_INVALID_DEST.
 *
 * container: the test links the real container.c and sets the
 * home with container_set_home(). container.c needs lsk_getuid
 * (asm symbol _getuid on target); the host stub below returns 501.
 *
 * File I/O: cfprefsd.c calls the plain Darwin names (openat, read,
 * write, close, renameat, fcntl) with Darwin flag values. On host
 * those calls are redirected to the cfp_host_* translators below,
 * which translate the Darwin flags to Linux flags and call libc.
 * The redirection MUST apply to cfprefsd.c only: the test file
 * itself calls libc open/fcntl, so cfprefsd.c is compiled to an
 * object file on its own first, with the -D flags, and linked in.
 *
 * Build: The target linux.h
 * is aarch64-only, so the build needs the host shim first on the
 * -I path (tests/host/host-shim/linux.h).
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -DCFP_OPENAT=cfp_host_openat -DCFP_READ=cfp_host_read \
 *        -DCFP_WRITE=cfp_host_write -DCFP_CLOSE=cfp_host_close \
 *        -DCFP_RENAMEAT=cfp_host_renameat -DCFP_FCNTL=cfp_host_fcntl \
 *        -c src/cfprefsd.c -o /tmp/cfprefsd.o
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/cfprefsd_test tests/host/cfprefsd_test.c \
 *        /tmp/cfprefsd.o src/svchost.c src/bplist.c src/bsxpc.c \
 *        src/container.c src/xpcwire.c
 *     /tmp/cfprefsd_test
 *   ASan+UBSan (same commands with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src \
 *        -DCFP_OPENAT=cfp_host_openat -DCFP_READ=cfp_host_read \
 *        -DCFP_WRITE=cfp_host_write -DCFP_CLOSE=cfp_host_close \
 *        -DCFP_RENAMEAT=cfp_host_renameat -DCFP_FCNTL=cfp_host_fcntl \
 *        -c src/cfprefsd.c -o /tmp/cfprefsd_asan.o
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src \
 *        -o /tmp/cfprefsd_test_asan tests/host/cfprefsd_test.c \
 *        /tmp/cfprefsd_asan.o src/svchost.c src/bplist.c \
 *        src/bsxpc.c src/container.c src/xpcwire.c
 *     /tmp/cfprefsd_test_asan
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include "xpcwire.h"
#include "bsxpc.h"
#include "svchost.h"
#include "bplist.h"
#include "cfprefsd.h"
#include "cfprefs_names.h"
#include "container.h"
#include "launchd.h"

/* ------------------------------------------------------------------ */
/* Test constants.                                                     */
/* ------------------------------------------------------------------ */

#define TEST_DOMAIN      "com.example.test"
#define TEST_MULTI_DOMAIN "com.example.multi"
#define TEST_SETM_DOMAIN "com.example.setmulti"
#define TEST_BIG_DOMAIN  "com.example.big"
#define TEST_FRESH_DOMAIN "com.example.fresh"
#define TEST_FUZZ_DOMAIN "com.example.fuzz"
#define TEST_REPLY_PORT  ((mach_port_name_t)0x3000)
#define TEST_SVC_PORT    ((mach_port_name_t)0x3001)

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
    if (xw_dict_add_int64(&b, CFP_KEY_ERROR, 0) != XW_OK)
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
/* container: the test links the real container.c. It needs
 * lsk_getuid (bsd.c provides it as asm symbol _getuid on target);
 * the host stub below returns the mobile uid. The test sets the
 * home with container_set_home() in main. */
uint32_t _getuid(void) {
    return 501;
}

static char g_test_home[PATH_MAX] = "";

/* Bounded joins (snprintf trips -Wformat-truncation under -Werror
 * here). Returns 0 or -1 when the result does not fit. */
static int join2(char *out, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 1 > cap) return -1;
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return 0;
}

static int join3(char *out, size_t cap, const char *a, const char *b,
                 const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > cap) return -1;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc + 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cfp_host_* file translators (Darwin flags in, libc out).             */
/* See the file header: these are the -D targets for cfprefsd.c.       */
/* ------------------------------------------------------------------ */

#define DARWIN_O_RDONLY    0
#define DARWIN_O_WRONLY    1
#define DARWIN_O_RDWR      2
#define DARWIN_O_CREAT     0x0200
#define DARWIN_O_TRUNC     0x0400
#define DARWIN_O_EXCL      0x0800
#define DARWIN_O_APPEND    0x0008
#define DARWIN_O_NONBLOCK  0x0004
#define DARWIN_O_DIRECTORY 0x100000
#define DARWIN_O_CLOEXEC   0x1000000
#define DARWIN_F_GETFL     3
#define DARWIN_F_GETPATH   50
#define DARWIN_AT_FDCWD    (-2)

static int darwin_open_flags_to_linux(int flags) {
    int lflags = flags & 3; /* access mode: same values */
    if (flags & DARWIN_O_CREAT) lflags |= O_CREAT;
    if (flags & DARWIN_O_TRUNC) lflags |= O_TRUNC;
    if (flags & DARWIN_O_EXCL) lflags |= O_EXCL;
    if (flags & DARWIN_O_APPEND) lflags |= O_APPEND;
    if (flags & DARWIN_O_NONBLOCK) lflags |= O_NONBLOCK;
    if (flags & DARWIN_O_DIRECTORY) lflags |= O_DIRECTORY;
    if (flags & DARWIN_O_CLOEXEC) lflags |= O_CLOEXEC;
    return lflags;
}

int cfp_host_openat(int dirfd, const char *path, int flags, int mode) {
    int ldir = (dirfd == DARWIN_AT_FDCWD) ? AT_FDCWD : dirfd;
    return openat(ldir, path, darwin_open_flags_to_linux(flags), mode);
}

long cfp_host_read(int fd, void *buf, unsigned long count) {
    return (long)read(fd, buf, (size_t)count);
}

long cfp_host_write(int fd, const void *buf, unsigned long count) {
    return (long)write(fd, buf, (size_t)count);
}

int cfp_host_close(int fd) {
    return close(fd);
}

int cfp_host_renameat(int oldfd, const char *oldpath,
                      int newfd, const char *newpath) {
    int lo = (oldfd == DARWIN_AT_FDCWD) ? AT_FDCWD : oldfd;
    int ln = (newfd == DARWIN_AT_FDCWD) ? AT_FDCWD : newfd;
    return renameat(lo, oldpath, ln, newpath);
}

int cfp_host_fcntl(int fd, int cmd, void *arg) {
    if (cmd == DARWIN_F_GETPATH) {
        /* Linux has no F_GETPATH: readlink /proc/self/fd/N. */
        char procfd[64];
        ssize_t n;
        if (arg == NULL) return -1;
        snprintf(procfd, sizeof(procfd), "/proc/self/fd/%d", fd);
        n = readlink(procfd, (char *)arg, 1023);
        if (n < 0) return -1;
        ((char *)arg)[n] = '\0';
        return 0;
    }
    if (cmd == DARWIN_F_GETFL) {
        return fcntl(fd, F_GETFL);
    }
    return fcntl(fd, cmd, arg);
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
/* Temp-dir setup.                                                     */
/* ------------------------------------------------------------------ */

static char g_prefs_dir[PATH_MAX] = "";

static void prefs_path(char *out, size_t cap, const char *file) {
    /* Test paths are short by construction; a failed join leaves an
     * empty path so the downstream open fails loudly. */
    if (join3(out, cap, g_prefs_dir, "/", file) != 0) out[0] = '\0';
}

static void byhost_path(char *out, size_t cap, const char *file) {
    if (join3(out, cap, g_prefs_dir, "/ByHost/", file) != 0)
        out[0] = '\0';
}

/* Create an empty file at path (truncate if it exists). Returns 0. */
static int make_empty_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (close(fd) != 0) return -1;
    return 0;
}

/* Read a whole file into a malloc'd buffer. *len_out gets the size.
 * Returns NULL on failure. */
static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    size_t cap = 0, len = 0;
    if (f == NULL) return NULL;
    cap = 4096;
    buf = (uint8_t *)malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        size_t n = 0;
        if (len == cap) {
            uint8_t *nb = NULL;
            cap *= 2;
            nb = (uint8_t *)realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    fclose(f);
    *len_out = len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* XPC body builders (the fake client's messages).                     */
/* ------------------------------------------------------------------ */

/* Op 1 set dict. with_token includes the AccessToken fd value. */
static int build_set_msg(uint8_t *body, uint32_t cap,
                         const char *domain, const char *key,
                         const uint8_t *val, uint32_t val_len,
                         int by_host, int with_token, int token_fd,
                         uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_SET);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_KEY, key);
    if (r == XW_OK)
        r = xw_dict_add_data(&b, CFP_KEY_VALUE, val, val_len);
    if (r == XW_OK)
        r = xw_dict_add_bool(&b, CFP_KEY_CURRENT_APP_DOMAIN, 1);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK && by_host)
        r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK && with_token)
        r = xw_dict_add_fd(&b, CFP_KEY_ACCESS_TOKEN,
                           (uint64_t)token_fd, MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Op 5 multi: outer dict with a Messages array of two op 1
 * sub-dicts. The access token rides on the outer dict. */
static int build_multi_msg(uint8_t *body, uint32_t cap,
                           const char *domain,
                           const char *key1, const uint8_t *val1,
                           uint32_t val1_len,
                           const char *key2, const uint8_t *val2,
                           uint32_t val2_len,
                           int with_token, int token_fd,
                           uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_MULTI);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK && with_token)
        r = xw_dict_add_fd(&b, CFP_KEY_ACCESS_TOKEN,
                           (uint64_t)token_fd, MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK) r = xw_dict_add_array_begin(&b, CFP_KEY_MESSAGES);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_SET);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_KEY, key1);
    if (r == XW_OK)
        r = xw_dict_add_data(&b, CFP_KEY_VALUE, val1, val1_len);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_SET);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_KEY, key2);
    if (r == XW_OK)
        r = xw_dict_add_data(&b, CFP_KEY_VALUE, val2, val2_len);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r == XW_OK) r = xw_array_end(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Op 8 set-multiple [guess]: three inline string pairs. */
static int build_set_multi_msg(uint8_t *body, uint32_t cap,
                               const char *domain,
                               const char *k1, const char *v1,
                               const char *k2, const char *v2,
                               const char *k3, const char *v3,
                               int with_token, int token_fd,
                               uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION,
                              CFP_OP_SET_MULTIPLE);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK) r = xw_dict_add_string(&b, k1, v1);
    if (r == XW_OK) r = xw_dict_add_string(&b, k2, v2);
    if (r == XW_OK) r = xw_dict_add_string(&b, k3, v3);
    if (r == XW_OK && with_token)
        r = xw_dict_add_fd(&b, CFP_KEY_ACCESS_TOKEN,
                           (uint64_t)token_fd, MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Dict with only an Operation key (unknown-op and no-key cases use
 * their own builders below). */
static int build_op_only_msg(uint8_t *body, uint32_t cap, int64_t op,
                             uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, op);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Dict with Domain/Key/Value but no Operation key (T7). */
static int build_no_op_msg(uint8_t *body, uint32_t cap,
                           const char *domain, const char *key,
                           const uint8_t *val, uint32_t val_len,
                           uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_KEY, key);
    if (r == XW_OK)
        r = xw_dict_add_data(&b, CFP_KEY_VALUE, val, val_len);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Build the exact 11-message batch from the Aidoku device log
 * (measured 2026-11-15). */
static int build_aidoku_batch_msg(uint8_t *body, uint32_t cap, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_MULTI);
    if (r == XW_OK) r = xw_dict_add_array_begin(&b, CFP_KEY_MESSAGES);
    /* [0] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_IS_MANAGED, 1);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, "com.apple.UIKit");
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [1] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_IS_MANAGED, 1);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, "com.apple.UIKit");
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [2] null */
    if (r == XW_OK) r = xw_add_null(&b);
    /* [3] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, "com.apple.UIKit");
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [4] null */
    if (r == XW_OK) r = xw_add_null(&b);
    /* [5] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, CFP_DOMAIN_ANY_APP);
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [6] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, CFP_DOMAIN_ANY_APP);
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [7] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, "com.apple.UIKit");
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [8] null */
    if (r == XW_OK) r = xw_add_null(&b);
    /* [9] */
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, "app.aidoku.Aidoku");
    if (r == XW_OK) r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, CFP_DOMAIN_ANY_APP);
    if (r == XW_OK) r = xw_dict_end(&b);
    /* [10] null */
    if (r == XW_OK) r = xw_add_null(&b);

    if (r == XW_OK) r = xw_array_end(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Standalone Op 1 read builder matching Aidoku launch log. */
static int build_read_msg(uint8_t *body, uint32_t cap,
                          const char *domain, int current_app,
                          int by_host, int is_managed,
                          const char *host_bundle, uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_READ);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, domain);
    if (r == XW_OK && current_app)
        r = xw_dict_add_bool(&b, CFP_KEY_CURRENT_APP_DOMAIN, 1);
    if (r == XW_OK && by_host)
        r = xw_dict_add_bool(&b, CFP_KEY_BY_HOST, 1);
    if (r == XW_OK && is_managed)
        r = xw_dict_add_bool(&b, CFP_KEY_IS_MANAGED, 1);
    if (r == XW_OK && host_bundle != NULL)
        r = xw_dict_add_string(&b, CFP_KEY_HOST_BUNDLE_ID, host_bundle);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Mach message delivery.                                              */
/* ------------------------------------------------------------------ */

/* Build a Mach message in buf: 24-byte header, optional one COPY_SEND
 * port descriptor, then the XPC body. Returns the total size, or 0
 * when the buffer is too small. */
static uint32_t build_mach_msg(uint8_t *buf, uint32_t cap,
                               mach_port_name_t dest,
                               mach_port_name_t reply_port,
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
    put_u32le(p + 20, XW_MSGH_ID_XPC);
    p += sizeof(mach_msg_header_t);
    if (with_desc) {
        put_u32le(p + 0, 1); /* descriptor count */
        put_u32le(p + 4, desc_name);
        put_u32le(p + 8, 0); /* pad1 */
        put_u32le(p + 12, 0x00130000u); /* pad2 0, disp 0x13, type 0 */
        p += 4 + 12;
    }
    /* memmove, not memcpy: the fuzzer builds its seed bodies in
     * place at buf + 24 and passes the same address as body, so
     * source and destination may be identical. */
    memmove(p, body, body_len);
    return total;
}

/* Deliver a built message to cfprefsd_process_msg. Returns the
 * entry's return value (0 or 1), or -1 when the message did not fit. */
static int deliver(mach_port_name_t dest, mach_port_name_t reply_port,
                   const uint8_t *body, uint32_t body_len,
                   int with_desc, mach_port_name_t desc_name,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    /* 64 KiB: the kernel's default maximum for one Mach message.
     * The harness buffer must not be the bottleneck for large
     * multis: T9 sends a 9 KiB request. */
    uint8_t msg[65536];
    uint32_t size = build_mach_msg(msg, sizeof(msg), dest, reply_port,
                                   body, body_len, with_desc, desc_name);
    if (size == 0) return -1;
    return cfprefsd_process_msg((const mach_msg_header_t *)msg, size,
                                reply_buf, reply_cap, out_reply_size);
}

/* ------------------------------------------------------------------ */
/* Reply parsing and plist verification.                               */
/* ------------------------------------------------------------------ */

/* Parse a full Mach message (reply) into a reader. Returns XW_OK or
 * a negative XW_ERR_*. Also checks the reply id is 0x20000000. */
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
    rc = xw_dict_find(&rd, CFP_KEY_ERROR, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, err_out);
}

/* True when the reply is a dictionary with no "error" key and no
 * other entries (the empty-dict set reply). */
static int reply_is_empty_dict(const uint8_t *reply_buf,
                               uint32_t reply_size) {
    xw_envelope env;
    xw_reader rd;
    xw_dict_iter it;
    const char *key = NULL;
    xw_value v;
    int rc = parse_reply(reply_buf, reply_size, &env, &rd);
    int count = 0;
    if (rc != XW_OK) return 0;
    if (xw_dict_iter_init(&rd, &it) != XW_OK) return 0;
    for (;;) {
        int next = xw_dict_next(&it, &key, &v);
        if (next == 0) break;
        if (next < 0) return 0;
        count++;
    }
    return count == 0;
}

/* Parse a plist file into the caller's arena. Returns 0 or -1. */
static uint8_t g_plist_arena_buf[0x110000];
static int parse_plist_file(const char *path, bp_value **root_out) {
    static bp_arena arena;
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    bp_value *root = NULL;
    if (buf == NULL) return -1;
    bp_arena_init(&arena, g_plist_arena_buf, sizeof(g_plist_arena_buf));
    if (bp_parse(buf, (uint32_t)len, &arena, &root) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    if (root == NULL || root->type != BP_DICT) return -1;
    *root_out = root;
    return 0;
}

/* Check a DATA value in a parsed domain dict. */
static void check_data_value(const bp_value *root, const char *key,
                             const uint8_t *want, uint32_t want_len) {
    const bp_value *v = bp_dict_get(root, key);
    CHECK(v != NULL);
    if (v == NULL) return;
    CHECK(v->type == BP_DATA);
    CHECK(v->len == want_len);
    if (v->len == want_len && v->s != NULL)
        CHECK(memcmp(v->s, want, want_len) == 0);
}

/* Check a STRING value in a parsed domain dict. */
static void check_string_value(const bp_value *root, const char *key,
                               const char *want) {
    const bp_value *v = bp_dict_get(root, key);
    CHECK(v != NULL);
    if (v == NULL) return;
    CHECK(v->type == BP_STRING);
    if (v->s != NULL) CHECK(strcmp((const char *)v->s, want) == 0);
}

/* ------------------------------------------------------------------ */
/* Scenario 1: init + registration.                                    */
/* ------------------------------------------------------------------ */

static mach_port_name_t g_agent_port = MACH_PORT_NULL;
static mach_port_name_t g_daemon_port = MACH_PORT_NULL;

static void scen_1_init(void) {
    int rc = 0;
    SCEN_BEGIN(1, "init + registration");
    rc = cfprefsd_init();
    CHECK(rc == KERN_SUCCESS);
    g_agent_port = svchost_lookup_port(CFP_SVC_AGENT);
    g_daemon_port = svchost_lookup_port(CFP_SVC_DAEMON);
    CHECK(g_agent_port != MACH_PORT_NULL);
    CHECK(g_daemon_port != MACH_PORT_NULL);
    CHECK(g_agent_port != g_daemon_port);
    rc = cfprefsd_init();
    CHECK(rc == KERN_SUCCESS);
    CHECK(svchost_lookup_port(CFP_SVC_AGENT) == g_agent_port);
    CHECK(svchost_lookup_port(CFP_SVC_DAEMON) == g_daemon_port);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: T1 op 1 set, verbatim DEF CON sample.                   */
/* ------------------------------------------------------------------ */

static void scen_2_set(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    int fd = -1;
    bp_value *root = NULL;
    SCEN_BEGIN(2, "T1 op 1 set (verbatim sample)");
    prefs_path(path, sizeof(path), TEST_DOMAIN ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(2);
        return;
    }
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "hello",
                           (const uint8_t *)"world", 5, 0, 1, fd,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > (uint32_t)sizeof(mach_msg_header_t));
    CHECK(reply_is_empty_dict(reply, reply_size) == 1);
    close(fd);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL)
        check_data_value(root, "hello", (const uint8_t *)"world", 5);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: T1b first write, no file, no token.                     */
/* ------------------------------------------------------------------ */

static void scen_3_first_write(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    bp_value *root = NULL;
    SCEN_BEGIN(3, "T1b first write (no file, no token)");
    prefs_path(path, sizeof(path), TEST_FRESH_DOMAIN ".plist");
    CHECK(access(path, F_OK) != 0);
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_FRESH_DOMAIN,
                           "first", (const uint8_t *)"value", 5, 0, 0,
                           0, &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_is_empty_dict(reply, reply_size) == 1);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL)
        check_data_value(root, "first", (const uint8_t *)"value", 5);
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: T2 op 5 multi.                                          */
/* ------------------------------------------------------------------ */

static void scen_4_multi(void) {
    uint8_t body[2048];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    int fd = -1;
    bp_value *root = NULL;
    SCEN_BEGIN(4, "T2 op 5 multi");
    prefs_path(path, sizeof(path), TEST_MULTI_DOMAIN ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(4);
        return;
    }
    CHECK_RC(build_multi_msg(body, sizeof(body), TEST_MULTI_DOMAIN,
                             "a", (const uint8_t *)"1", 1,
                             "b", (const uint8_t *)"22", 2,
                             1, fd, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value arr, elem;
        uint32_t count = 0;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_dict_find(&rd, CFP_KEY_MESSAGES, &arr) == XW_OK);
        CHECK(arr.type == XW_TYPE_ARRAY);
        CHECK(xw_array_len_v(&arr, &count) == XW_OK);
        CHECK(count == 2);
        CHECK(xw_array_at_v(&arr, 0, &elem) == XW_OK);
        CHECK(elem.type == XW_TYPE_DICTIONARY);
        CHECK(xw_array_at_v(&arr, 1, &elem) == XW_OK);
        CHECK(elem.type == XW_TYPE_DICTIONARY);
    }
    close(fd);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL) {
        check_data_value(root, "a", (const uint8_t *)"1", 1);
        check_data_value(root, "b", (const uint8_t *)"22", 2);
    }
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: T3 op 8 set-multiple [guess].                           */
/* ------------------------------------------------------------------ */

static void scen_5_set_multiple(void) {
    uint8_t body[2048];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    int fd = -1;
    bp_value *root = NULL;
    SCEN_BEGIN(5, "T3 op 8 set-multiple [guess]");
    prefs_path(path, sizeof(path), TEST_SETM_DOMAIN ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(5);
        return;
    }
    CHECK_RC(build_set_multi_msg(body, sizeof(body), TEST_SETM_DOMAIN,
                                 "x", "one", "y", "two", "z", "three",
                                 1, fd, &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    CHECK(reply_is_empty_dict(reply, reply_size) == 1);
    close(fd);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL) {
        check_string_value(root, "x", "one");
        check_string_value(root, "y", "two");
        check_string_value(root, "z", "three");
    }
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: T4 op 1 without a token on an existing file.            */
/* ------------------------------------------------------------------ */

static void scen_6_no_token(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    char path[PATH_MAX];
    size_t before_len = 0, after_len = 0;
    uint8_t *before = NULL, *after = NULL;
    SCEN_BEGIN(6, "T4 op 1 without token (existing file)");
    prefs_path(path, sizeof(path), TEST_DOMAIN ".plist");
    before = read_file(path, &before_len);
    CHECK(before != NULL);
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "hello",
                           (const uint8_t *)"pwned", 5, 0, 0, 0,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    after = read_file(path, &after_len);
    CHECK(after != NULL);
    CHECK(after_len == before_len);
    if (before != NULL && after != NULL && after_len == before_len)
        CHECK(memcmp(before, after, before_len) == 0);
    free(before);
    free(after);
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: T5 op 1 with an O_RDONLY token fd.                      */
/* ------------------------------------------------------------------ */

static void scen_7_readonly_token(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    char path[PATH_MAX];
    size_t before_len = 0, after_len = 0;
    uint8_t *before = NULL, *after = NULL;
    int fd = -1;
    SCEN_BEGIN(7, "T5 op 1 with O_RDONLY token");
    prefs_path(path, sizeof(path), TEST_DOMAIN ".plist");
    before = read_file(path, &before_len);
    CHECK(before != NULL);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) {
        free(before);
        SCEN_END(7);
        return;
    }
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "hello",
                           (const uint8_t *)"pwned", 5, 0, 1, fd,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    close(fd);
    after = read_file(path, &after_len);
    CHECK(after != NULL);
    CHECK(after_len == before_len);
    if (before != NULL && after != NULL && after_len == before_len)
        CHECK(memcmp(before, after, before_len) == 0);
    free(before);
    free(after);
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: T7/T8 malformed + unknown op.                           */
/* ------------------------------------------------------------------ */

static void scen_8_errors(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    SCEN_BEGIN(8, "T7/T8 malformed + unknown op");
    /* No Operation key. */
    CHECK_RC(build_no_op_msg(body, sizeof(body), TEST_DOMAIN, "hello",
                             (const uint8_t *)"world", 5, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* Unknown op 99. */
    CHECK_RC(build_op_only_msg(body, sizeof(body), 99, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    /* Op 2, the hypothesized get number: rejected, not silently
     * accepted (T6 is reserved until the trace supplies the number). */
    CHECK_RC(build_op_only_msg(body, sizeof(body), 2, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_UNKNOWN_MESSAGE);
    /* Truncated XPC body: header plus 10 body bytes (magic +
     * version + 2 bytes of the root type word). */
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "hello",
                           (const uint8_t *)"world", 5, 0, 0, 0,
                           &body_len));
    {
        uint8_t msg[8192];
        uint32_t size = build_mach_msg(msg, sizeof(msg), g_agent_port,
                                       TEST_REPLY_PORT, body, body_len,
                                       0, 0);
        CHECK(size > 24 + 10);
        ret = cfprefsd_process_msg((const mach_msg_header_t *)msg,
                                   24 + 10, reply, sizeof(reply),
                                   &reply_size);
        CHECK(ret == 1);
        CHECK_RC(reply_error_code(reply, reply_size, &err));
        CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    }
    /* One-way message (no reply port): side effects still happen,
     * no reply is built. Uses a fresh domain so the first-write
     * rule applies (an existing file would need a token). */
    CHECK_RC(build_set_msg(body, sizeof(body), "com.example.oneway",
                           "oneway", (const uint8_t *)"yes", 3, 0, 0,
                           0, &body_len));
    ret = deliver(g_agent_port, MACH_PORT_NULL, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size == 0);
    {
        char path[PATH_MAX];
        bp_value *root = NULL;
        prefs_path(path, sizeof(path), "com.example.oneway.plist");
        CHECK(parse_plist_file(path, &root) == 0);
        if (root != NULL)
            check_data_value(root, "oneway", (const uint8_t *)"yes", 3);
    }
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: T9 reply size (40 keys, 40-byte values).                */
/* ------------------------------------------------------------------ */

static void scen_9_reply_size(void) {
    /* One multi with 40 op 1 sub-messages, each a 40-byte value. */
    static uint8_t body[32768];
    uint8_t reply[2048];
    uint32_t reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    int fd = -1;
    xw_builder b;
    int r = XW_OK;
    int i = 0;
    uint8_t val[40];
    SCEN_BEGIN(9, "T9 reply size (40 keys, 40-byte values)");
    memset(val, 'v', sizeof(val));
    prefs_path(path, sizeof(path), TEST_BIG_DOMAIN ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(9);
        return;
    }
    r = xw_builder_init(&b, body, sizeof(body));
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_USER, CFP_USER_CURRENT);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_MULTI);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, CFP_KEY_DOMAIN, TEST_BIG_DOMAIN);
    if (r == XW_OK)
        r = xw_dict_add_fd(&b, CFP_KEY_ACCESS_TOKEN, (uint64_t)fd,
                           MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK) r = xw_dict_add_array_begin(&b, CFP_KEY_MESSAGES);
    for (i = 0; i < 40 && r == XW_OK; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%02d", i);
        r = xw_dict_begin(&b);
        if (r == XW_OK)
            r = xw_dict_add_int64(&b, CFP_KEY_OPERATION, CFP_OP_SET);
        if (r == XW_OK)
            r = xw_dict_add_string(&b, CFP_KEY_DOMAIN,
                                   TEST_BIG_DOMAIN);
        if (r == XW_OK)
            r = xw_dict_add_string(&b, CFP_KEY_USER,
                                   CFP_USER_CURRENT);
        if (r == XW_OK) r = xw_dict_add_string(&b, CFP_KEY_KEY, key);
        if (r == XW_OK)
            r = xw_dict_add_data(&b, CFP_KEY_VALUE, val, sizeof(val));
        if (r == XW_OK) r = xw_dict_end(&b);
    }
    if (r == XW_OK) r = xw_array_end(&b);
    if (r == XW_OK) r = xw_dict_end(&b);
    CHECK(r == XW_OK);
    if (r == XW_OK) {
        uint32_t body_len = xw_builder_len(&b);
        ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                      1, (mach_port_name_t)fd, reply, sizeof(reply),
                      &reply_size);
        CHECK(ret == 1);
        /* The reply is a multi-message batch reply: well under SVCHOST_MAX_REPLY. */
        CHECK(reply_size < SVCHOST_MAX_REPLY);
        {
            xw_envelope env;
            xw_reader rd;
            xw_value arr;
            uint32_t count = 0;
            CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
            CHECK(xw_dict_find(&rd, CFP_KEY_MESSAGES, &arr) == XW_OK);
            CHECK(arr.type == XW_TYPE_ARRAY);
            CHECK(xw_array_len_v(&arr, &count) == XW_OK);
            CHECK(count == 40);
        }
    }
    close(fd);
    {
        bp_value *root = NULL;
        CHECK(parse_plist_file(path, &root) == 0);
        if (root != NULL) {
            check_data_value(root, "k00", val, sizeof(val));
            check_data_value(root, "k39", val, sizeof(val));
        }
    }
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* Scenario 10: T11 ByHost set.                                        */
/* ------------------------------------------------------------------ */

static void scen_10_byhost(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    char plain[PATH_MAX];
    int fd = -1;
    bp_value *root = NULL;
    SCEN_BEGIN(10, "T11 ByHost set");
    byhost_path(path, sizeof(path),
                TEST_DOMAIN "." CFP_HOST_ID ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(10);
        return;
    }
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "hostkey",
                           (const uint8_t *)"hostval", 7, 1, 1, fd,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    CHECK(reply_is_empty_dict(reply, reply_size) == 1);
    close(fd);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL)
        check_data_value(root, "hostkey", (const uint8_t *)"hostval", 7);
    /* The plain (non-ByHost) file must not have gained the key. */
    prefs_path(plain, sizeof(plain), TEST_DOMAIN ".plist");
    CHECK(parse_plist_file(plain, &root) == 0);
    if (root != NULL)
        CHECK(bp_dict_get(root, "hostkey") == NULL);
    SCEN_END(10);
}

/* ------------------------------------------------------------------ */
/* Scenario 11: T12 NSGlobalDomain set.                                */
/* ------------------------------------------------------------------ */

static void scen_11_global(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    int fd = -1;
    bp_value *root = NULL;
    SCEN_BEGIN(11, "T12 NSGlobalDomain set");
    prefs_path(path, sizeof(path), ".GlobalPreferences.plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(11);
        return;
    }
    CHECK_RC(build_set_msg(body, sizeof(body), CFP_DOMAIN_GLOBAL,
                           "gkey", (const uint8_t *)"gval", 4, 0, 1,
                           fd, &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  1, (mach_port_name_t)fd, reply, sizeof(reply),
                  &reply_size);
    CHECK(ret == 1);
    CHECK(reply_is_empty_dict(reply, reply_size) == 1);
    close(fd);
    CHECK(parse_plist_file(path, &root) == 0);
    if (root != NULL)
        check_data_value(root, "gkey", (const uint8_t *)"gval", 4);
    SCEN_END(11);
}

/* ------------------------------------------------------------------ */
/* Scenario 12: T13 domain traversal.                                  */
/* ------------------------------------------------------------------ */

static void scen_12_traversal(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    char outside[PATH_MAX];
    SCEN_BEGIN(12, "T13 domain traversal");
    CHECK_RC(build_set_msg(body, sizeof(body), "foo/bar", "k",
                           (const uint8_t *)"v", 1, 0, 0, 0,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    CHECK(join2(outside, sizeof(outside), g_test_home, "/foo") == 0);
    CHECK(access(outside, F_OK) != 0);
    CHECK_RC(build_set_msg(body, sizeof(body), "..", "k",
                           (const uint8_t *)"v", 1, 0, 0, 0,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    CHECK_RC(build_set_msg(body, sizeof(body), "", "k",
                           (const uint8_t *)"v", 1, 0, 0, 0,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    SCEN_END(12);
}

/* ------------------------------------------------------------------ */
/* Scenario 13: T14 missing Preferences directory.                     */
/* ------------------------------------------------------------------ */

static void scen_13_missing_dir(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int64_t err = -1;
    char home2_template[PATH_MAX];
    char home2[PATH_MAX];
    char saved_home[PATH_MAX];
    SCEN_BEGIN(13, "T14 missing Preferences directory");
    snprintf(home2_template, sizeof(home2_template),
             "/tmp/cfprefsd_test2_XXXXXX");
    if (mkdtemp(home2_template) == NULL) {
        printf("  (mkdtemp failed)\n");
        CHECK(0);
        SCEN_END(13);
        return;
    }
    if (realpath(home2_template, home2) == NULL) {
        printf("  (realpath failed)\n");
        CHECK(0);
        rmdir(home2_template);
        SCEN_END(13);
        return;
    }
    snprintf(saved_home, sizeof(saved_home), "%s", g_test_home);
    snprintf(g_test_home, sizeof(g_test_home), "%s", home2);
    CHECK(container_set_home(g_test_home) == 0);
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_DOMAIN, "k",
                           (const uint8_t *)"v", 1, 0, 0, 0,
                           &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK_RC(reply_error_code(reply, reply_size, &err));
    CHECK(err == SVCHOST_ERR_BAD_MESSAGE);
    /* The directory was not created for us. */
    {
        char prefs2[PATH_MAX];
        CHECK(join2(prefs2, sizeof(prefs2), home2,
                    "/Library/Preferences") == 0);
        CHECK(access(prefs2, F_OK) != 0);
    }
    snprintf(g_test_home, sizeof(g_test_home), "%s", saved_home);
    CHECK(container_set_home(g_test_home) == 0);
    rmdir(home2);
    SCEN_END(13);
}

/* ------------------------------------------------------------------ */
/* Scenario 14: T15 verbatim trace.                                    */
/* ------------------------------------------------------------------ */

static long test_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);
    return size;
}

static int test_file_contains(const char *path, const char *needle) {
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

static void scen_14_trace(void) {
    const char *log_path = "/tmp/cfprefsd_trace.log";
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    int saved_fd = -1;
    int log_fd = -1;
    long size_before = -1, size_after = -1;
    SCEN_BEGIN(14, "T15 verbatim trace");
    bsxpc_set_trace(1);
    saved_fd = dup(2);
    CHECK(saved_fd >= 0);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(log_fd >= 0);
    if (log_fd >= 0) {
        CHECK(dup2(log_fd, 2) == 2);
        close(log_fd);
    }
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_FRESH_DOMAIN,
                           "tracekey", (const uint8_t *)"t", 1, 0, 0,
                           0, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    if (saved_fd >= 0) {
        CHECK(dup2(saved_fd, 2) == 2);
        close(saved_fd);
    }
    CHECK(test_file_contains(log_path, CFP_TRACE_PREFIX) == 1);
    CHECK(test_file_contains(log_path, CFP_KEY_OPERATION) == 1);
    CHECK(test_file_contains(log_path, "tracekey") == 1);
    /* Trace off: a second message must not grow the log. */
    bsxpc_set_trace(0);
    size_before = test_file_size(log_path);
    CHECK(size_before > 0);
    CHECK_RC(build_set_msg(body, sizeof(body), TEST_FRESH_DOMAIN,
                           "tracekey2", (const uint8_t *)"t", 1, 0, 0,
                           0, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    size_after = test_file_size(log_path);
    CHECK(size_after == size_before);
    bsxpc_set_trace(-1);
    unlink(log_path);
    SCEN_END(14);
}

/* ------------------------------------------------------------------ */
/* Scenario 15: T10 fuzz.                                              */
/* ------------------------------------------------------------------ */

static void scen_15_fuzz(void) {
    uint8_t seed_a[8192];
    uint8_t seed_b[8192];
    uint8_t scratch[8192];
    uint8_t reply[2048];
    uint32_t seed_a_size = 0, seed_b_size = 0;
    uint32_t body_len = 0;
    int violations = 0;
    int i = 0;
    char path[PATH_MAX];
    int fd = -1;
    SCEN_BEGIN(15, "T10 fuzz (10000 iterations)");
    prefs_path(path, sizeof(path), TEST_FUZZ_DOMAIN ".plist");
    CHECK(make_empty_file(path) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0) {
        SCEN_END(15);
        return;
    }
    /* Seed A: a valid op 1 set with a token descriptor. */
    CHECK_RC(build_set_msg(seed_a + 24, sizeof(seed_a) - 24,
                           TEST_FUZZ_DOMAIN, "fuzzkey",
                           (const uint8_t *)"fuzzval", 7, 0, 1, fd,
                           &body_len));
    seed_a_size = build_mach_msg(seed_a, sizeof(seed_a), g_agent_port,
                                 TEST_REPLY_PORT, seed_a + 24, body_len,
                                 1, (mach_port_name_t)fd);
    CHECK(seed_a_size > 24);
    /* Seed B: a valid op 5 multi with a token descriptor. */
    CHECK_RC(build_multi_msg(seed_b + 24, sizeof(seed_b) - 24,
                             TEST_FUZZ_DOMAIN,
                             "fa", (const uint8_t *)"1", 1,
                             "fb", (const uint8_t *)"2", 1,
                             1, fd, &body_len));
    seed_b_size = build_mach_msg(seed_b, sizeof(seed_b), g_agent_port,
                                 TEST_REPLY_PORT, seed_b + 24, body_len,
                                 1, (mach_port_name_t)fd);
    CHECK(seed_b_size > 24);
    for (i = 0; i < 10000; i++) {
        int use_b = (i % 2) == 1;
        uint32_t orig_size = use_b ? seed_b_size : seed_a_size;
        uint32_t send_size = orig_size;
        uint32_t reply_size = 0;
        int ret = 0;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        mach_msg_header_t *hdr = NULL;
        if (i > 0 && (i % 1000) == 0) {
            /* Bound the fuzz domain's growth: reset the file. */
            close(fd);
            unlink(path);
            CHECK(make_empty_file(path) == 0);
            fd = open(path, O_RDWR);
            if (fd < 0) {
                printf("\n  fuzz iter %d: cannot reopen fuzz file\n",
                       i);
                violations++;
                break;
            }
        }
        memcpy(scratch, use_b ? seed_b : seed_a, orig_size);
        /* 1 to 8 random byte mutations. */
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % orig_size;
            scratch[off] = (uint8_t)rng_next();
        }
        /* Sometimes truncate the message. */
        if ((rng_next() % 8u) == 0) {
            send_size = rng_next() % (orig_size + 1);
        }
        /* Sometimes drop the reply port (one-way message). */
        if ((rng_next() % 16u) == 0) {
            put_u32le(scratch + 12, 0);
        }
        hdr = (mach_msg_header_t *)scratch;
        ret = cfprefsd_process_msg(hdr, send_size, reply,
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
    close(fd);
    printf("  fuzz: 10000 iterations, %d violations\n", violations);
    CHECK(violations == 0);
    SCEN_END(15);
}

/* ------------------------------------------------------------------ */
/* Scenario 16: Aidoku 11-message Op 5 batch (no files yet).          */
/* ------------------------------------------------------------------ */

static void scen_16_aidoku_batch_empty(void) {
    uint8_t body[4096];
    uint8_t reply[4096];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char gpath[PATH_MAX];
    SCEN_BEGIN(16, "Aidoku 11-message Op 5 batch (no files yet)");
    prefs_path(gpath, sizeof(gpath), ".GlobalPreferences.plist");
    unlink(gpath);
    CHECK_RC(build_aidoku_batch_msg(body, sizeof(body), &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > sizeof(mach_msg_header_t));
    {
        int64_t err = 0;
        /* Must not be an error reply (error=22 was the bug). */
        CHECK(reply_error_code(reply, reply_size, &err) != XW_OK);
    }
    {
        xw_envelope env;
        xw_reader rd;
        xw_value arr;
        uint32_t count = 0;
        uint32_t i = 0;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_dict_find(&rd, CFP_KEY_MESSAGES, &arr) == XW_OK);
        CHECK(arr.type == XW_TYPE_ARRAY);
        CHECK(xw_array_len_v(&arr, &count) == XW_OK);
        CHECK(count == 11);
        for (i = 0; i < count; i++) {
            xw_value elem;
            CHECK(xw_array_at_v(&arr, i, &elem) == XW_OK);
            if (i == 2 || i == 4 || i == 8 || i == 10) {
                CHECK(elem.type == XW_TYPE_NULL);
            } else {
                xw_value plist_val;
                CHECK(elem.type == XW_TYPE_DICTIONARY);
                /* No domain files exist, so sub-replies are empty dicts. */
                CHECK(xw_dict_find_v(&elem, CFP_KEY_PLIST_DATA, &plist_val) != XW_OK);
            }
        }
    }
    SCEN_END(16);
}

/* ------------------------------------------------------------------ */
/* Scenario 17: Aidoku Op 5 batch with existing plist data.           */
/* ------------------------------------------------------------------ */

static void scen_17_aidoku_batch_with_data(void) {
    uint8_t body[4096];
    uint8_t reply[4096];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char gpath[PATH_MAX];
    uint8_t bp_buf[512];
    uint32_t bp_len = 0;
    SCEN_BEGIN(17, "Aidoku Op 5 batch with existing plist data");
    prefs_path(gpath, sizeof(gpath), ".GlobalPreferences.plist");
    {
        bp_arena arena;
        bp_value *d = NULL;
        bp_arena_init(&arena, g_plist_arena_buf, sizeof(g_plist_arena_buf));
        d = bp_new(&arena, BP_DICT);
        CHECK(d != NULL);
        CHECK(bp_dict_set(&arena, d, "AppleLanguages",
                          bp_new_string(&arena, "en")) == 0);
        CHECK(bp_write(d, bp_buf, sizeof(bp_buf), &bp_len) == 0);
        FILE *f = fopen(gpath, "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            CHECK(fwrite(bp_buf, 1, bp_len, f) == bp_len);
            fclose(f);
        }
    }
    CHECK_RC(build_aidoku_batch_msg(body, sizeof(body), &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value arr;
        uint32_t count = 0;
        uint32_t i = 0;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_dict_find(&rd, CFP_KEY_MESSAGES, &arr) == XW_OK);
        CHECK(arr.type == XW_TYPE_ARRAY);
        CHECK(xw_array_len_v(&arr, &count) == XW_OK);
        CHECK(count == 11);
        for (i = 0; i < count; i++) {
            xw_value elem;
            CHECK(xw_array_at_v(&arr, i, &elem) == XW_OK);
            if (i == 2 || i == 4 || i == 8 || i == 10) {
                CHECK(elem.type == XW_TYPE_NULL);
            } else if (i == 5 || i == 6) {
                /* kCFPreferencesAnyApplication without ByHost maps to .GlobalPreferences */
                xw_value plist_val;
                const uint8_t *pdata = NULL;
                uint32_t plen = 0;
                uint32_t pdesc = XW_NO_DESC;
                CHECK(elem.type == XW_TYPE_DICTIONARY);
                CHECK(xw_dict_find_v(&elem, CFP_KEY_PLIST_DATA, &plist_val) == XW_OK);
                CHECK(xw_as_data(&plist_val, &pdata, &plen, &pdesc) == XW_OK);
                CHECK(plen == bp_len);
                if (pdata != NULL && plen == bp_len) {
                    CHECK(memcmp(pdata, bp_buf, bp_len) == 0);
                }
            } else {
                xw_value plist_val;
                CHECK(elem.type == XW_TYPE_DICTIONARY);
                CHECK(xw_dict_find_v(&elem, CFP_KEY_PLIST_DATA, &plist_val) != XW_OK);
            }
        }
    }
    unlink(gpath);
    SCEN_END(17);
}

/* ------------------------------------------------------------------ */
/* Scenario 18: Standalone Op 1 read (empty domain, no error 22).      */
/* ------------------------------------------------------------------ */

/* Helper: extract and validate the three shmem cache-validation fields
 * from a read reply dict. Returns XW_OK when all three are present and
 * valid. */
static int check_read_shmem_fields(const xw_value *dict,
                                   const char **name_out,
                                   int64_t *idx_out,
                                   uint64_t *state_out) {
    xw_value nv, iv, sv;
    const char *name = NULL;
    uint32_t nlen = 0;
    int64_t idx = -1;
    uint64_t state = 0;
    if (xw_dict_find_v(dict, CFP_KEY_SHMEM_NAME, &nv) != XW_OK)
        return XW_ERR_NOT_FOUND;
    if (xw_as_string(&nv, &name, &nlen) != XW_OK || name == NULL || nlen < 2)
        return XW_ERR_BAD_TYPE;
    if (xw_dict_find_v(dict, CFP_KEY_SHMEM_INDEX, &iv) != XW_OK)
        return XW_ERR_NOT_FOUND;
    if (xw_as_int64(&iv, &idx) != XW_OK || idx <= 0 ||
        idx >= (int64_t)CFP_SHMEM_SLOTS)
        return XW_ERR_BAD_TYPE;
    if (xw_dict_find_v(dict, CFP_KEY_SHMEM_STATE, &sv) != XW_OK)
        return XW_ERR_NOT_FOUND;
    if (xw_as_uint64(&sv, &state) != XW_OK || state == 0 ||
        state == 0xffffffffu)
        return XW_ERR_BAD_TYPE;
    if (name_out) *name_out = name;
    if (idx_out) *idx_out = idx;
    if (state_out) *state_out = state;
    return XW_OK;
}

static void scen_18_standalone_read_empty(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    SCEN_BEGIN(18, "Standalone Op 1 read (empty domain, no error 22)");
    /* Read for app.aidoku.Aidoku (with ByHost, IsManaged, etc.). */
    CHECK_RC(build_read_msg(body, sizeof(body), "app.aidoku.Aidoku",
                            1, 1, 1, "app.aidoku.Aidoku", &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > sizeof(mach_msg_header_t));
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root, v;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(xw_dict_find_v(&root, CFP_KEY_ERROR, &v) != XW_OK);
        CHECK(xw_dict_find_v(&root, CFP_KEY_PLIST_DATA, &v) != XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, NULL, NULL) == XW_OK);
    }

    /* Read for com.apple.UIKit. */
    CHECK_RC(build_read_msg(body, sizeof(body), "com.apple.UIKit",
                            0, 0, 0, NULL, &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size > sizeof(mach_msg_header_t));
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root, v;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(xw_dict_find_v(&root, CFP_KEY_ERROR, &v) != XW_OK);
        CHECK(xw_dict_find_v(&root, CFP_KEY_PLIST_DATA, &v) != XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, NULL, NULL) == XW_OK);
    }
    SCEN_END(18);
}

/* ------------------------------------------------------------------ */
/* Scenario 19: Standalone Op 1 read with domain plist data.          */
/* ------------------------------------------------------------------ */

static void scen_19_standalone_read_data(void) {
    uint8_t body[1024];
    uint8_t reply[2048];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    char path[PATH_MAX];
    uint8_t bp_buf[512];
    uint32_t bp_len = 0;
    SCEN_BEGIN(19, "Standalone Op 1 read with domain plist data");
    prefs_path(path, sizeof(path), "app.aidoku.Aidoku.plist");
    {
        bp_arena arena;
        bp_value *d = NULL;
        bp_arena_init(&arena, g_plist_arena_buf, sizeof(g_plist_arena_buf));
        d = bp_new(&arena, BP_DICT);
        CHECK(d != NULL);
        CHECK(bp_dict_set(&arena, d, "Theme",
                          bp_new_string(&arena, "Dark")) == 0);
        CHECK(bp_write(d, bp_buf, sizeof(bp_buf), &bp_len) == 0);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            CHECK(fwrite(bp_buf, 1, bp_len, f) == bp_len);
            fclose(f);
        }
    }
    CHECK_RC(build_read_msg(body, sizeof(body), "app.aidoku.Aidoku",
                            1, 0, 0, "app.aidoku.Aidoku", &body_len));
    ret = deliver(g_daemon_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value plist_val;
        const uint8_t *pdata = NULL;
        uint32_t plen = 0;
        uint32_t pdesc = XW_NO_DESC;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_dict_find(&rd, CFP_KEY_PLIST_DATA, &plist_val) == XW_OK);
        CHECK(xw_as_data(&plist_val, &pdata, &plen, &pdesc) == XW_OK);
        CHECK(plen == bp_len);
        if (pdata != NULL && plen == bp_len) {
            CHECK(memcmp(pdata, bp_buf, bp_len) == 0);
        }
    }
    unlink(path);
    SCEN_END(19);
}

/* ------------------------------------------------------------------ */
/* Scenario 20: Shmem cache validation, stability, and bump on write.  */
/* ------------------------------------------------------------------ */

static void scen_20_shmem_cache_and_bump(void) {
    uint8_t body[4096];
    uint8_t reply[SVCHOST_MAX_REPLY];
    uint32_t body_len = 0, reply_size = 0;
    int ret = 0;
    const char *shm_name = NULL;
    int64_t idx1 = -1, idx2 = -1, idx_global = -1, idx_any = -1, idx_bh = -1;
    uint64_t state1 = 0, state2 = 0;
    char shm_path[128];
    int shm_fd = -1;
    const volatile uint32_t *shm_map = NULL;
    char path[PATH_MAX];
    int tok_fd = -1;

    SCEN_BEGIN(20, "Shmem cache fields, /dev/shm page, and bump on write");
    prefs_path(path, sizeof(path), "com.example.cache.plist");
    unlink(path);

    /* 1. Initial read of missing domain returns shmem metadata. */
    CHECK_RC(build_read_msg(body, sizeof(body), "com.example.cache",
                            1, 0, 0, "com.example.cache", &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(check_read_shmem_fields(&root, &shm_name, &idx1, &state1) == XW_OK);
        CHECK(shm_name != NULL && strcmp(shm_name, cfprefsd_shm_name()) == 0);
    }

    /* 2. Open and mmap the same /dev/shm page as CoreFoundation's shm_open(). */
    snprintf(shm_path, sizeof(shm_path), "%s%s", CFP_DEV_SHM_DIR,
             cfprefsd_shm_name());
    shm_fd = open(shm_path, O_RDONLY);
    CHECK(shm_fd >= 0);
    if (shm_fd >= 0) {
        void *p = mmap(NULL, CFP_SHMEM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
        CHECK(p != MAP_FAILED);
        close(shm_fd);
        if (p != MAP_FAILED) {
            shm_map = (const volatile uint32_t *)p;
            CHECK(shm_map[idx1] == (uint32_t)state1);
        }
    }

    /* 3. Repeated read returns identical index and generation state. */
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, &idx2, &state2) == XW_OK);
        CHECK(idx2 == idx1);
        CHECK(state2 == state1);
    }

    /* 4. NSGlobalDomain and kCFPreferencesAnyApplication alias the same
     * slot, while ByHost gets a distinct slot. */
    CHECK_RC(build_read_msg(body, sizeof(body), CFP_DOMAIN_GLOBAL,
                            0, 0, 0, NULL, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, &idx_global, NULL) == XW_OK);
    }
    CHECK_RC(build_read_msg(body, sizeof(body), CFP_DOMAIN_ANY_APP,
                            0, 0, 0, NULL, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, &idx_any, NULL) == XW_OK);
        CHECK(idx_any == idx_global);
    }
    CHECK_RC(build_read_msg(body, sizeof(body), "com.example.cache",
                            0, 1, 0, NULL, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, &idx_bh, NULL) == XW_OK);
        CHECK(idx_bh != idx1);
    }

    /* 5. Op 1 set (first write) bumps the shmem slot on the shared page. */
    CHECK_RC(build_set_msg(body, sizeof(body), "com.example.cache",
                           "k1", (const uint8_t *)"v1", 2, 0, 0, 0, &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    if (shm_map != NULL) {
        CHECK(shm_map[idx1] == (uint32_t)(state1 + 1u));
    }

    /* 6. Next read sees the bumped CFPreferencesShmemState + plist data. */
    CHECK_RC(build_read_msg(body, sizeof(body), "com.example.cache",
                            1, 0, 0, "com.example.cache", &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value root, pv;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_reader_root(&rd, &root) == XW_OK);
        CHECK(xw_dict_find_v(&root, CFP_KEY_PLIST_DATA, &pv) == XW_OK);
        CHECK(check_read_shmem_fields(&root, NULL, &idx2, &state2) == XW_OK);
        CHECK(idx2 == idx1);
        CHECK(state2 == state1 + 1u);
    }

    /* 7. Op 8 set-multiple and Op 5 multi each bump the shmem slot. */
    tok_fd = open(path, O_RDWR);
    CHECK(tok_fd >= 0);
    if (tok_fd >= 0) {
        CHECK_RC(build_set_multi_msg(body, sizeof(body), "com.example.cache",
                                     "a", "1", "b", "2", "c", "3",
                                     1, tok_fd, &body_len));
        ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                      1, (mach_port_name_t)tok_fd, reply, sizeof(reply),
                      &reply_size);
        CHECK(ret == 1);
        close(tok_fd);
        if (shm_map != NULL) {
            CHECK(shm_map[idx1] == (uint32_t)(state1 + 2u));
        }
    }
    tok_fd = open(path, O_RDWR);
    CHECK(tok_fd >= 0);
    if (tok_fd >= 0) {
        CHECK_RC(build_multi_msg(body, sizeof(body), "com.example.cache",
                                 "m1", (const uint8_t *)"x", 1,
                                 "m2", (const uint8_t *)"y", 1,
                                 1, tok_fd, &body_len));
        ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                      1, (mach_port_name_t)tok_fd, reply, sizeof(reply),
                      &reply_size);
        CHECK(ret == 1);
        close(tok_fd);
        if (shm_map != NULL) {
            CHECK(shm_map[idx1] == (uint32_t)(state1 + 4u));
        }
    }

    /* 8. Aidoku 11-message Op 5 batch fits inside SVCHOST_MAX_REPLY (1024 bytes)
     * with shmem fields populated on all 7 non-null sub-replies. */
    CHECK_RC(build_aidoku_batch_msg(body, sizeof(body), &body_len));
    ret = deliver(g_agent_port, TEST_REPLY_PORT, body, body_len,
                  0, 0, reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_size <= SVCHOST_MAX_REPLY);
    {
        xw_envelope env;
        xw_reader rd;
        xw_value arr;
        uint32_t count = 0;
        CHECK(parse_reply(reply, reply_size, &env, &rd) == XW_OK);
        CHECK(xw_dict_find(&rd, CFP_KEY_MESSAGES, &arr) == XW_OK);
        CHECK(xw_array_len_v(&arr, &count) == XW_OK);
        CHECK(count == 11);
        for (uint32_t i = 0; i < count; i++) {
            xw_value elem;
            CHECK(xw_array_at_v(&arr, i, &elem) == XW_OK);
            if (i == 2 || i == 4 || i == 8 || i == 10) {
                CHECK(elem.type == XW_TYPE_NULL);
            } else {
                CHECK(elem.type == XW_TYPE_DICTIONARY);
                CHECK(check_read_shmem_fields(&elem, NULL, NULL, NULL) == XW_OK);
            }
        }
    }

    if (shm_map != NULL) munmap((void *)(uintptr_t)shm_map, CFP_SHMEM_SIZE);
    unlink(path);
    SCEN_END(20);
}

/* ------------------------------------------------------------------ */
/* Cleanup and main.                                                   */
/* ------------------------------------------------------------------ */

static void test_cleanup(void) {
    char path[PATH_MAX];
    const char *files[] = {
        TEST_DOMAIN ".plist",
        TEST_MULTI_DOMAIN ".plist",
        TEST_SETM_DOMAIN ".plist",
        TEST_BIG_DOMAIN ".plist",
        TEST_FRESH_DOMAIN ".plist",
        TEST_FUZZ_DOMAIN ".plist",
        "com.example.oneway.plist",
        ".GlobalPreferences.plist",
        "app.aidoku.Aidoku.plist",
        "com.apple.UIKit.plist",
        "com.example.cache.plist",
        NULL
    };
    const char *tmpfiles[] = {
        TEST_DOMAIN ".plist.cfptmp",
        TEST_MULTI_DOMAIN ".plist.cfptmp",
        TEST_SETM_DOMAIN ".plist.cfptmp",
        TEST_BIG_DOMAIN ".plist.cfptmp",
        TEST_FRESH_DOMAIN ".plist.cfptmp",
        TEST_FUZZ_DOMAIN ".plist.cfptmp",
        "com.example.oneway.plist.cfptmp",
        ".GlobalPreferences.plist.cfptmp",
        "app.aidoku.Aidoku.plist.cfptmp",
        "com.apple.UIKit.plist.cfptmp",
        "com.example.cache.plist.cfptmp",
        NULL
    };
    int i = 0;
    for (i = 0; files[i] != NULL; i++) {
        prefs_path(path, sizeof(path), files[i]);
        unlink(path);
    }
    for (i = 0; tmpfiles[i] != NULL; i++) {
        prefs_path(path, sizeof(path), tmpfiles[i]);
        unlink(path);
    }
    byhost_path(path, sizeof(path),
                TEST_DOMAIN "." CFP_HOST_ID ".plist");
    unlink(path);
    byhost_path(path, sizeof(path),
                TEST_DOMAIN "." CFP_HOST_ID ".plist.cfptmp");
    unlink(path);
    (void)join2(path, sizeof(path), g_prefs_dir, "/ByHost");
    rmdir(path);
    rmdir(g_prefs_dir);
    (void)join2(path, sizeof(path), g_test_home, "/Library");
    rmdir(path);
    rmdir(g_test_home);
    if (cfprefsd_shm_name()[0] != '\0') {
        char shm_path[128];
        if (join2(shm_path, sizeof(shm_path), CFP_DEV_SHM_DIR,
                  cfprefsd_shm_name()) == 0) {
            unlink(shm_path);
        }
    }
}

int main(void) {
    char tmpl[PATH_MAX];
    char home[PATH_MAX];
    char lib[PATH_MAX];
    char byhost[PATH_MAX];
    printf("cfprefsd_test: fake CFPreferences daemon host test\n");
    snprintf(tmpl, sizeof(tmpl), "/tmp/cfprefsd_test_XXXXXX");
    if (mkdtemp(tmpl) == NULL) {
        printf("mkdtemp failed\n");
        return 1;
    }
    if (realpath(tmpl, home) == NULL) {
        printf("realpath failed\n");
        return 1;
    }
    snprintf(g_test_home, sizeof(g_test_home), "%s", home);
    if (container_set_home(g_test_home) != 0) {
        printf("container_set_home failed\n");
        return 1;
    }
    if (join3(g_prefs_dir, sizeof(g_prefs_dir), g_test_home,
              "/Library/Preferences", "") != 0) {
        printf("prefs dir path too long\n");
        test_cleanup();
        return 1;
    }
    if (join2(lib, sizeof(lib), g_test_home, "/Library") != 0 ||
        join2(byhost, sizeof(byhost), g_prefs_dir, "/ByHost") != 0) {
        printf("setup path too long\n");
        test_cleanup();
        return 1;
    }
    if (mkdir(lib, 0755) != 0 || mkdir(g_prefs_dir, 0755) != 0 ||
        mkdir(byhost, 0755) != 0) {
        printf("mkdir setup failed\n");
        test_cleanup();
        return 1;
    }
    scen_1_init();
    scen_2_set();
    scen_3_first_write();
    scen_4_multi();
    scen_5_set_multiple();
    scen_6_no_token();
    scen_7_readonly_token();
    scen_8_errors();
    scen_9_reply_size();
    scen_10_byhost();
    scen_11_global();
    scen_12_traversal();
    scen_13_missing_dir();
    scen_14_trace();
    scen_15_fuzz();
    scen_16_aidoku_batch_empty();
    scen_17_aidoku_batch_with_data();
    scen_18_standalone_read_empty();
    scen_19_standalone_read_data();
    scen_20_shmem_cache_and_bump();
    test_cleanup();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
