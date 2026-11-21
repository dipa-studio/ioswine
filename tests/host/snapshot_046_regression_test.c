/*
 * snapshot_046_regression_test.c - Regression test for debug hooks.
 *
 * Debug hooks in libsystem_kernel/src/snapshot.c (NSError
 * hook with backtrace, NSException hook, IOSWINE_OBJC_TRACE method
 * tracer, IOSWINE_DUMP_GOT). Afterwards tests/host/snapshot_test.c
 * failed because the hooks logged during a plain run. The fix: the new
 * hooks must stay silent and side-effect free unless their env vars
 * are set.
 *
 * This test drives the REAL snapshot.c (SNAPSHOT_HOST_TEST) through
 * snapshot_init() only, with a small mock, and asserts the silence
 * property directly:
 *   A. With every debug env var unset, snapshot_init() logs nothing.
 *   B. With IOSWINE_NSERROR_TRACE=1 the hook install path runs and
 *      reports (here: "[nserror] hook failed", because the mock has no
 *      NSError class; on device it would install).
 *   C. With IOSWINE_OBJC_TRACE set, the tracer installs and logs
 *      "[objc] hooked ...".
 *   D. With IOSWINE_RENDERS_LOCALLY=1 the rendersLocally hook installs
 *      and logs "[rl] +[UIApplication rendersLocally] -> YES".
 *   E. With IOSWINE_OBJC_NIL set, the nil-ing pass runs and logs one
 *      line per entry (here: "[objcnil] no class: ...").
 *   F. IOSWINE_DUMP_GOT is device-only: with it unset no "[got]" line
 *      appears. Setting it on host is NOT tested: snapshot_init()
 *      dereferences the fixed Aidoku GOT address 0x10110b448, which
 *      would crash the host process.
 *
 * This test does not duplicate snapshot_test.c (which covers the pump,
 * window selection and PNG logic with its full mock). It covers only
 * the silence property, which snapshot_test.c does not assert.
 *
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static const char *g_env_snapshot = NULL;
static const char *g_env_nserror = NULL;
static const char *g_env_objc_trace = NULL;
static const char *g_env_dump_got = NULL;
static const char *g_env_renders_locally = NULL;
static const char *g_env_objc_nil = NULL;
static char g_log_buf[8192];
static size_t g_log_len = 0;
static uint64_t g_now_ns = 1000000000ULL;

static void reset_log(void) {
    g_log_len = 0;
    g_log_buf[0] = '\0';
}

static void test_log(const char *msg, size_t len) {
    if (g_log_len + len < sizeof(g_log_buf)) {
        memcpy(g_log_buf + g_log_len, msg, len);
        g_log_len += len;
        g_log_buf[g_log_len] = '\0';
    }
}

static const char *test_getenv(const char *name) {
    if (strcmp(name, "IOSWINE_SNAPSHOT") == 0) return g_env_snapshot;
    if (strcmp(name, "IOSWINE_NSERROR_TRACE") == 0) return g_env_nserror;
    if (strcmp(name, "IOSWINE_OBJC_TRACE") == 0) return g_env_objc_trace;
    if (strcmp(name, "IOSWINE_DUMP_GOT") == 0) return g_env_dump_got;
    if (strcmp(name, "IOSWINE_RENDERS_LOCALLY") == 0) return g_env_renders_locally;
    if (strcmp(name, "IOSWINE_OBJC_NIL") == 0) return g_env_objc_nil;
    return NULL;
}

static uint64_t test_now_ns(void) {
    return g_now_ns;
}

static void *test_alloc_buf(size_t size) {
    return calloc(1, size);
}

static void test_free_buf(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}

static int g_fake_uiapp_cls;
static int g_fake_method;
static int g_fake_sel;
static int g_fake_space;
static int g_fake_ctx;

static void *test_objc_getClass(const char *name) {
    if (name && strcmp(name, "UIApplication") == 0) return &g_fake_uiapp_cls;
    return NULL;
}

static void *test_sel_registerName(const char *name) {
    (void)name;
    return &g_fake_sel;
}

static void *test_msgSend(void *obj, void *sel) {
    (void)obj; (void)sel;
    return NULL;
}

static void *test_cg_create(void) { return &g_fake_space; }
static void test_cg_release(void *p) { (void)p; }
static void *test_cg_ctx_create(void *d, size_t w, size_t h, size_t bpc,
                                size_t bpr, void *s, uint32_t bi) {
    (void)d; (void)w; (void)h; (void)bpc; (void)bpr; (void)s; (void)bi;
    return &g_fake_ctx;
}
static void test_ctx_release(void *p) { (void)p; }
static void test_ctx_translate(void *p, double x, double y) { (void)p; (void)x; (void)y; }
static void test_ctx_scale(void *p, double x, double y) { (void)p; (void)x; (void)y; }

static void *test_gim(void *cls, void *sel) {
    (void)cls; (void)sel;
    return &g_fake_method;
}

static void *test_gcm(void *cls, void *sel) {
    (void)cls; (void)sel;
    return &g_fake_method;
}

static void *test_msi(void *m, void *imp) {
    (void)m; (void)imp;
    return NULL;
}

static void *test_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "objc_getClass") == 0) return (void *)test_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)test_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)test_msgSend;
    if (strcmp(name, "CGColorSpaceCreateDeviceRGB") == 0) return (void *)test_cg_create;
    if (strcmp(name, "CGColorSpaceRelease") == 0) return (void *)test_cg_release;
    if (strcmp(name, "CGBitmapContextCreate") == 0) return (void *)test_cg_ctx_create;
    if (strcmp(name, "CGContextRelease") == 0) return (void *)test_ctx_release;
    if (strcmp(name, "CGContextTranslateCTM") == 0) return (void *)test_ctx_translate;
    if (strcmp(name, "CGContextScaleCTM") == 0) return (void *)test_ctx_scale;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)test_gim;
    if (strcmp(name, "class_getClassMethod") == 0) return (void *)test_gcm;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)test_msi;
    return NULL;
}

static const snapshot_test_ops_t g_ops = {
    .dlsym_default = test_dlsym,
    .getenv_fn = test_getenv,
    .now_ns = test_now_ns,
    .log_fn = test_log,
    .alloc_buf = test_alloc_buf,
    .free_buf = test_free_buf,
};

static void reset_env(void) {
    g_env_snapshot = NULL;
    g_env_nserror = NULL;
    g_env_objc_trace = NULL;
    g_env_dump_got = NULL;
    g_env_renders_locally = NULL;
    g_env_objc_nil = NULL;
    reset_log();
    snapshot_test_reset();
}

int main(void) {
    /* Unbuffered stdout: CHECK failures must survive a sanitizer abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    snapshot_test_configure(&g_ops);

    /* A. All debug env vars unset: snapshot_init() must be silent. */
    reset_env();
    snapshot_init();
    CHECK(g_log_len == 0);

    /* A2. Same, but with the pump enabled and no debug vars: the only
     * log line is the pump arming line, no debug hook lines. */
    reset_env();
    g_env_snapshot = "1";
    /* dispatch symbols are absent from this mock, so init logs the
     * missing-symbols line and returns; assert no debug hook output. */
    snapshot_init();
    CHECK(strstr(g_log_buf, "[nserror]") == NULL);
    CHECK(strstr(g_log_buf, "[nsexception]") == NULL);
    CHECK(strstr(g_log_buf, "[objc]") == NULL);
    CHECK(strstr(g_log_buf, "[got]") == NULL);
    CHECK(strstr(g_log_buf, "[rl]") == NULL);
    CHECK(strstr(g_log_buf, "[objcnil]") == NULL);

    /* B. NSERROR_TRACE=1: the hook install path runs and reports. */
    reset_env();
    g_env_nserror = "1";
    snapshot_init();
    CHECK(strstr(g_log_buf, "[nserror] hook failed") != NULL);

    /* C. OBJC_TRACE set: the tracer installs and logs the hook. */
    reset_env();
    g_env_objc_trace = "UIApplication -sendEvent:";
    snapshot_init();
    CHECK(strstr(g_log_buf, "[objc] hooked UIApplication sendEvent:") != NULL);

    /* D. RENDERS_LOCALLY=1: the rendersLocally hook installs. */
    reset_env();
    g_env_renders_locally = "1";
    snapshot_init();
    CHECK(strstr(g_log_buf, "[rl] +[UIApplication rendersLocally] -> YES") != NULL);

    /* E. OBJC_NIL set: the nil-ing pass runs and logs one line per entry. */
    reset_env();
    g_env_objc_nil = "NSAssertionHandler -handleFailureInMethod:object:file:lineNumber:description:";
    snapshot_init();
    CHECK(strstr(g_log_buf, "[objcnil] no class: NSAssertionHandler -handleFailureInMethod:") != NULL);

    /* F. DUMP_GOT unset: no "[got]" line. Setting DUMP_GOT=1 on host
     * would dereference 0x10110b448 and crash; it is device-only and
     * deliberately not exercised here. */
    reset_env();
    snapshot_init();
    CHECK(strstr(g_log_buf, "[got]") == NULL);

    /* G. Reset restores silence: after a noisy run, a clean run logs nothing. */
    reset_env();
    g_env_nserror = "1";
    snapshot_init();
    CHECK(g_log_len > 0);
    reset_env();
    snapshot_init();
    CHECK(g_log_len == 0);

    printf("snapshot_046_regression_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
