/* webkit_test.c: host test for the WebKit launch-failure diagnostic
 *
 *
 * Background: WKWebView needs a
 * separate WebContent process, launched on iOS 26 through
 * BrowserEngineKit/ExtensionKit (BEWebContentProcess, bundle id
 * com.apple.WebKit.WebContent) or, failing that, through
 * xpc_connection_create("com.apple.WebKit.WebContent"). ioswine has
 * no process spawning and no ExtensionKit, and the fake launchd
 * answers every lookup for an unregistered service with error 124,
 * which libxpc maps to BOOTSTRAP_UNKNOWN_SERVICE (1102). The
 * WebProcessProxy then gets XPC_ERROR_CONNECTION_INVALID and the web
 * view stays white. There is no single-process mode in current
 * WebKit to select instead, so this test does
 * not fake a fix: it makes the failure diagnosed. launchd.c prints
 * one always-on [webkit] line per WebKit service lookup that fails,
 * so the white view is explained on device even though g_ipc_trace
 * is off there (the device logs carry no [launchd] lines at
 * all).
 *
 * What it covers:
 *   1. Lookup of com.apple.WebKit.WebContent (routine 0xcf):
 *      reply built, error 124, [webkit] line names the service.
 *   2. Same for Networking, GPU, Model and the .Development
 *      variant of WebContent, and through the 0x324 XPC_LOOKUP
 *      routine.
 *   3. A non-WebKit unknown service (com.apple.foo.bar) still
 *      answers error 124 but prints no [webkit] line.
 *   4. The diagnostic fires with g_ipc_trace off (the device
 *      configuration) and does not duplicate the [launchd] trace
 *      line when the trace is on.
 *   5. Malformed input: an empty dict (no "name") answers error
 *      124, prints no [webkit] line, and never crashes.
 *
 * How the service is driven on host: there is no real ipc.c here,
 * so this file stubs the ipc_* symbols launchd.c references
 * (g_ipc_trace, ipc_get_bks_display_port, ipc_get_hidsys_port,
 * ipc_port_from_name, ipc_port_retain_send, ipc_desc_store_port).
 * Messages are built with the real xpcwire builder: a 24-byte Mach
 * header (msgh_id 0x10000000 | routine) plus an XPC body carrying a
 * {"name": ...} dict. fd 2 is redirected to a temp file while the
 * lookup runs, then restored, and the file is searched for the
 * diagnostic line (same pattern as lsd_test.c).
 *
 * RED check: on the unpatched tree launchd.c prints no [webkit]
 * line, so cases 1, 2 and 4 fail there (the error-124 replies
 * still pass). Run against a pristine base copy to confirm.
 *
 * Build (run from the tree root; the target linux.h is aarch64-only,
 * so the build needs the host shim first on the -I path):
 *
 *   Plain:
 *     cc -std=gnu11 -O1 -g -w -iquote build/hosttests-plain/src \
 *        -iquote tests/host -o /tmp/webkit_test \
 *        tests/host/webkit_test.c build/hosttests-plain/src/launchd.c \
 *        build/hosttests-plain/src/xpcwire.c
 *     /tmp/webkit_test
 *   ASan+UBSan: same command with -fsanitize=address,undefined
 *     -fno-sanitize-recover=all, or run tools/host_tests.sh.
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
#include "ipc.h"
#include "launchd.h"

/* ------------------------------------------------------------------ */
/* Stubs for the ipc_* symbols launchd.c references. There is no real */
/* ipc.c on host; the unknown-service path never touches a real port.  */
/* ------------------------------------------------------------------ */

int g_ipc_trace = 0;

struct ipc_port *ipc_get_bks_display_port(void) { return NULL; }
struct ipc_port *ipc_get_hidsys_port(void) { return NULL; }
struct ipc_port *ipc_port_from_name(mach_port_name_t name) {
    (void)name;
    return NULL;
}
void ipc_port_retain_send(struct ipc_port *port) { (void)port; }
void ipc_desc_store_port(uint8_t *desc_ptr, struct ipc_port *port) {
    (void)desc_ptr;
    (void)port;
}

/* ------------------------------------------------------------------ */
/* Check harness.                                                      */
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
/* Message builders.                                                   */
/* ------------------------------------------------------------------ */

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Build a {"name": service} XPC body. Returns the body length, or 0. */
static uint32_t build_name_body(uint8_t *buf, uint32_t cap,
                                const char *service) {
    xw_builder b;
    if (xw_builder_init(&b, buf, cap) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_add_string(&b, "name", service) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    if (xw_builder_error(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* Build an empty-dict XPC body (no "name" key). Returns the length. */
static uint32_t build_empty_body(uint8_t *buf, uint32_t cap) {
    xw_builder b;
    if (xw_builder_init(&b, buf, cap) != XW_OK) return 0;
    if (xw_dict_begin(&b) != XW_OK) return 0;
    if (xw_dict_end(&b) != XW_OK) return 0;
    if (xw_builder_error(&b) != XW_OK) return 0;
    return xw_builder_len(&b);
}

/* Wrap a body in a 24-byte Mach header with the given routine in the
 * low 24 bits of msgh_id. Returns the total message size, or 0. */
static uint32_t build_mach_msg(uint8_t *buf, uint32_t cap, uint32_t routine,
                               const uint8_t *body, uint32_t body_len) {
    uint32_t total = (uint32_t)sizeof(mach_msg_header_t) + body_len;
    uint8_t *p = buf;
    if (cap < total) return 0;
    put_u32le(p + 0, 0x1513u); /* COPY_SEND | MAKE_SEND_ONCE<<8 */
    put_u32le(p + 4, total);
    put_u32le(p + 8, 0); /* dest: launchd kobject port, unused here */
    put_u32le(p + 12, 0); /* reply port */
    put_u32le(p + 16, 0);
    put_u32le(p + 20, 0x10000000u | (routine & 0xffffffu));
    p += sizeof(mach_msg_header_t);
    memcpy(p, body, body_len);
    return total;
}

/* Deliver a built message to launchd_process_msg. Returns its return
 * value (0 or 1), or -1 when the message does not fit. */
static int deliver(uint32_t routine, const uint8_t *body, uint32_t body_len,
                   uint8_t *reply_buf, uint32_t reply_cap,
                   uint32_t *out_reply_size) {
    uint8_t msg[4096];
    uint32_t size = build_mach_msg(msg, sizeof(msg), routine,
                                   body, body_len);
    if (size == 0) return -1;
    return launchd_process_msg((const mach_msg_header_t *)msg, size,
                               reply_buf, reply_cap, out_reply_size);
}

/* Read the "error" int64 out of a reply. The reply is a Mach header
 * plus an XPC body. Returns 1 on success, 0 otherwise. */
static int reply_error(const uint8_t *reply, uint32_t reply_size,
                       int64_t *out_err) {
    xw_reader r;
    xw_value v;
    int64_t err = 0;
    if (reply_size <= sizeof(mach_msg_header_t)) return 0;
    if (xw_reader_init(&r, reply + sizeof(mach_msg_header_t),
                       reply_size - (uint32_t)sizeof(mach_msg_header_t),
                       NULL, 0) != XW_OK)
        return 0;
    if (xw_dict_find(&r, "error", &v) != XW_OK) return 0;
    if (xw_as_int64(&v, &err) != XW_OK) return 0;
    *out_err = err;
    return 1;
}

/* ------------------------------------------------------------------ */
/* fd 2 capture (same pattern as lsd_test.c).                          */
/* ------------------------------------------------------------------ */

static char g_log_path[256];

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    char buf[16384];
    size_t n = 0;
    int found = 0;
    if (f == NULL) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (strstr(buf, needle) != NULL) found = 1;
    return found;
}

/* Redirect fd 2 to the log file. Returns the saved fd, or -1. */
static int capture_start(void) {
    int saved_fd = dup(2);
    int log_fd;
    if (saved_fd < 0) return -1;
    log_fd = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0) {
        close(saved_fd);
        return -1;
    }
    if (dup2(log_fd, 2) != 2) {
        close(log_fd);
        close(saved_fd);
        return -1;
    }
    close(log_fd);
    return saved_fd;
}

static void capture_stop(int saved_fd) {
    if (saved_fd >= 0) {
        if (dup2(saved_fd, 2) == 2) close(saved_fd);
    }
}

/* One lookup with fd 2 captured. Fills reply_size and returns the
 * launchd_process_msg return value. */
static int lookup_captured(uint32_t routine, const char *service,
                           uint8_t *reply, uint32_t reply_cap,
                           uint32_t *out_reply_size) {
    uint8_t body[512];
    uint32_t body_len = build_name_body(body, sizeof(body), service);
    int saved_fd;
    int ret;
    CHECK(body_len > 0);
    saved_fd = capture_start();
    CHECK(saved_fd >= 0);
    ret = deliver(routine, body, body_len, reply, reply_cap,
                  out_reply_size);
    capture_stop(saved_fd);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Cases.                                                              */
/* ------------------------------------------------------------------ */

static void test_webkit_services(void) {
    static const char *services[] = {
        "com.apple.WebKit.WebContent",
        "com.apple.WebKit.Networking",
        "com.apple.WebKit.GPU",
        "com.apple.WebKit.Model",
        "com.apple.WebKit.WebContent.Development",
        "com.apple.WebKit.WebContent.CaptivePortal",
    };
    uint8_t reply[512];
    uint32_t reply_size = 0;
    int64_t err = 0;
    size_t i;

    g_ipc_trace = 0; /* device configuration: trace off */
    for (i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        char needle[320];
        int ret;
        reply_size = 0;
        ret = lookup_captured(LAUNCHD_ROUTINE_LOOKUP, services[i],
                              reply, sizeof(reply), &reply_size);
        CHECK(ret == 1);
        CHECK(reply_error(reply, reply_size, &err) == 1);
        CHECK(err == 124); /* BOOTSTRAP_UNKNOWN_SERVICE */
        snprintf(needle, sizeof(needle),
                 "[webkit] service \"%s\" not launchable", services[i]);
        CHECK(file_contains(g_log_path, needle) == 1);
    }
}

static void test_xpc_lookup_routine(void) {
    uint8_t reply[512];
    uint32_t reply_size = 0;
    int64_t err = 0;
    int ret;

    g_ipc_trace = 0;
    ret = lookup_captured(LAUNCHD_ROUTINE_XPC_LOOKUP,
                          "com.apple.WebKit.WebContent",
                          reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_error(reply, reply_size, &err) == 1);
    CHECK(err == 124);
    CHECK(file_contains(g_log_path,
                        "[webkit] service \"com.apple.WebKit.WebContent\" "
                        "not launchable") == 1);
}

static void test_non_webkit_unknown(void) {
    uint8_t reply[512];
    uint32_t reply_size = 0;
    int64_t err = 0;
    int ret;

    g_ipc_trace = 0;
    ret = lookup_captured(LAUNCHD_ROUTINE_LOOKUP, "com.apple.foo.bar",
                          reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_error(reply, reply_size, &err) == 1);
    CHECK(err == 124);
    CHECK(file_contains(g_log_path, "[webkit]") == 0);
}

static void test_trace_on_no_duplicate(void) {
    uint8_t reply[512];
    uint32_t reply_size = 0;
    int64_t err = 0;
    int ret;

    /* Trace on: the [launchd] trace line appears, and the [webkit]
     * line still appears once per lookup (different prefix, no
     * duplication of the diagnostic itself). Each lookup truncates
     * the capture file, so the second lookup must show only its own
     * service line, proving the diagnostic is per lookup. */
    g_ipc_trace = 1;
    ret = lookup_captured(LAUNCHD_ROUTINE_LOOKUP,
                          "com.apple.WebKit.WebContent",
                          reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_error(reply, reply_size, &err) == 1);
    CHECK(err == 124);
    CHECK(file_contains(g_log_path, "[launchd]") == 1);
    CHECK(file_contains(g_log_path,
                        "[webkit] service \"com.apple.WebKit.WebContent\" "
                        "not launchable") == 1);
    ret = lookup_captured(LAUNCHD_ROUTINE_LOOKUP,
                          "com.apple.WebKit.GPU",
                          reply, sizeof(reply), &reply_size);
    CHECK(ret == 1);
    CHECK(reply_error(reply, reply_size, &err) == 1);
    CHECK(err == 124);
    CHECK(file_contains(g_log_path,
                        "[webkit] service \"com.apple.WebKit.GPU\" "
                        "not launchable") == 1);
    CHECK(file_contains(g_log_path, "WebContent") == 0);
    g_ipc_trace = 0;
}

static void test_malformed_no_name(void) {
    uint8_t body[256];
    uint8_t reply[512];
    uint32_t body_len = build_empty_body(body, sizeof(body));
    uint32_t reply_size = 0;
    int64_t err = 0;
    int saved_fd;
    int ret;

    CHECK(body_len > 0);
    g_ipc_trace = 0;
    saved_fd = capture_start();
    CHECK(saved_fd >= 0);
    ret = deliver(LAUNCHD_ROUTINE_LOOKUP, body, body_len,
                  reply, sizeof(reply), &reply_size);
    capture_stop(saved_fd);
    CHECK(ret == 1);
    CHECK(reply_error(reply, reply_size, &err) == 1);
    CHECK(err == 124);
    CHECK(file_contains(g_log_path, "[webkit]") == 0);
}

int main(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') tmpdir = "/tmp";
    snprintf(g_log_path, sizeof(g_log_path), "%s/webkit_test_%d.log",
             tmpdir, (int)getpid());

    test_webkit_services();
    test_xpc_lookup_routine();
    test_non_webkit_unknown();
    test_trace_on_no_duplicate();
    test_malformed_no_name();

    unlink(g_log_path);
    if (g_failures == 0) {
        printf("webkit_test: %d checks, 0 failures\n", g_checks);
        return 0;
    }
    printf("webkit_test: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
}
