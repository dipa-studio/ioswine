/* hacks_removal_test.c - prove the BKS nil hook is gone.
 *
 * With IOSWINE_RENDERS_LOCALLY=1, snapshot_init() must still install
 * +[UIApplication rendersLocally] -> YES, but must NOT touch
 * +[BKSHIDEventDeliveryManager sharedInstance]. An earlier workaround
 * hooked sharedInstance to nil (snapshot.c renders_locally_install);
 * the bkshid.c in-process fake replaces that hook, and this test
 * pins the removal: it records every method_setImplementation call
 * through a mock ObjC runtime and asserts the BKS method is never
 * among them.
 *
 * Failure mode: building this test against snapshot.c with the nil hook
 * must FAIL (method_setImplementation is called twice, once for the
 * BKS sharedInstance method, and the log contains the nil-hook line).
 *
 * Host test only: plain cc + ASan/UBSan. No Apple code, no device.
 */
#define SNAPSHOT_HOST_TEST 1

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"

/* ------------------------------------------------------------------ */
/* Mock ObjC runtime: records method_setImplementation calls.        */
/* ------------------------------------------------------------------ */

static int g_cls_uiapp_dummy;
static int g_cls_bks_dummy;
static int g_meth_rl_dummy;
static int g_meth_bks_dummy;

static void *g_msi_methods[16];
static void *g_msi_imps[16];
static int g_msi_calls;

static char g_log_buf[8192];
static size_t g_log_len;

static void test_log(const char *msg, size_t len) {
    if (g_log_len + len < sizeof(g_log_buf)) {
        memcpy(g_log_buf + g_log_len, msg, len);
        g_log_len += len;
        g_log_buf[g_log_len] = '\0';
    }
}

static const char *test_getenv(const char *name) {
    if (strcmp(name, "IOSWINE_RENDERS_LOCALLY") == 0) return "1";
    return NULL;
}

static uint64_t test_now_ns(void) { return 0; }

static void *test_alloc_buf(size_t size) { return calloc(1, size); }

static void test_free_buf(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "UIApplication") == 0) return &g_cls_uiapp_dummy;
    if (strcmp(name, "BKSHIDEventDeliveryManager") == 0) return &g_cls_bks_dummy;
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    return (void *)name;
}

static void *mock_dummy_fn(void) { return NULL; }

static void *mock_class_getClassMethod(void *cls, void *sel) {
    if (cls == &g_cls_uiapp_dummy && sel && strcmp((const char *)sel, "rendersLocally") == 0)
        return &g_meth_rl_dummy;
    if (cls == &g_cls_bks_dummy && sel && strcmp((const char *)sel, "sharedInstance") == 0)
        return &g_meth_bks_dummy;
    return NULL;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    (void)cls;
    (void)sel;
    return NULL;
}

static void *mock_method_setImplementation(void *m, void *imp) {
    if (g_msi_calls < 16) {
        g_msi_methods[g_msi_calls] = m;
        g_msi_imps[g_msi_calls] = imp;
    }
    g_msi_calls++;
    return NULL;
}

static void *mock_dlsym(const char *name) {
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "class_getClassMethod") == 0) return (void *)mock_class_getClassMethod;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    /* Core syms ensure_core_syms() requires; any non-NULL value works. */
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGColorSpaceCreateDeviceRGB") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGColorSpaceRelease") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGBitmapContextCreate") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGContextRelease") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGContextTranslateCTM") == 0) return (void *)mock_dummy_fn;
    if (strcmp(name, "CGContextScaleCTM") == 0) return (void *)mock_dummy_fn;
    return NULL;
}

static const snapshot_test_ops_t g_ops = {
    .dlsym_default = mock_dlsym,
    .getenv_fn = test_getenv,
    .now_ns = test_now_ns,
    .log_fn = test_log,
    .alloc_buf = test_alloc_buf,
    .free_buf = test_free_buf,
};

/* ------------------------------------------------------------------ */
/* Checks                                                             */
/* ------------------------------------------------------------------ */

static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        printf("FAIL check %d: %s (line %d)\n", g_checks, #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    snapshot_test_reset();
    snapshot_test_configure(&g_ops);
    g_log_len = 0;
    g_log_buf[0] = '\0';
    g_msi_calls = 0;

    /* IOSWINE_SNAPSHOT is unset in test_getenv, so snapshot_init()
     * runs the hook installs and returns before arming the pump. */
    snapshot_init();

    /* 1-2: exactly one IMP swap happened (rendersLocally only). */
    CHECK(g_msi_calls >= 1);
    CHECK(g_msi_calls == 1);

    /* 3-4: the swapped method is rendersLocally, with a real IMP. */
    CHECK(g_msi_methods[0] == &g_meth_rl_dummy);
    CHECK(g_msi_imps[0] != NULL);

    /* 5: the BKS sharedInstance method was never touched. */
    {
        int i, touched = 0;
        for (i = 0; i < g_msi_calls && i < 16; i++) {
            if (g_msi_methods[i] == &g_meth_bks_dummy) touched = 1;
        }
        CHECK(!touched);
    }

    /* 6-8: the log says rendersLocally went in, and says nothing
     * about a BKS nil hook. */
    CHECK(strstr(g_log_buf, "[rl] +[UIApplication rendersLocally] -> YES") != NULL);
    CHECK(strstr(g_log_buf, "sharedInstance] -> nil") == NULL);
    CHECK(strstr(g_log_buf, "BKSHIDEventDeliveryManager") == NULL);

    printf("PASS hacks_removal_test: %d checks\n", g_checks);
    return 0;
}
