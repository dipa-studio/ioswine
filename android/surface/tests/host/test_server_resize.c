/*
 * Host unit test for the presenter server resize wiring
 * (android/surface/src/server/server.c).
 *
 * server.c is a NativeActivity app, so the test compiles it on host
 * against the stub Android headers in stubs/ (fake ANativeWindow,
 * fake AConfiguration, log capture) and drives its static on_cmd
 * directly by including server.c. The stubs are test-only and never
 * ship in the APK.
 *
 *   cc -std=c11 -Wall -Wextra -I stubs -I ../../src/common \
 *      -I ../../src/server test_server_resize.c \
 *      ../../src/server/ui.c ../../src/server/sysinfo.c \
 *      ../../src/common/protocol.c ../../src/common/touch_input.c \
 *      stubs/test_server_stubs.c -o test_server_resize -lpthread
 *
 * Covers: CONFIG_CHANGED and APP_CMD_WINDOW_RESIZED both route
 * through on_window_size_changed (previously, CONFIG_CHANGED was an inline
 * duplicate of the helper and WINDOW_RESIZED was unhandled, so a
 * resize arriving as WINDOW_RESIZED never re-validated the mapping
 * and the helper was dead code), the regression case
 * (a 1080x2400 mapping applied to a 1440x3200 buffer), and the
 * launcher-mode resize (no client yet: launcher screen repaints,
 * mapping stays invalid).
 *
 * RED check: on the unpatched tree the test reports 33 checks,
 * 10 failures. The functional failures are the WINDOW_RESIZED
 * ones: test_window_resized_revalidates (mapping stays 1440x3200
 * while the window is 720x1600, plus the shared-path memcmp) and
 * test_resize_launcher_no_hello (window size never moves to
 * 720x1600). The other two failures are the new "geometry
 * config-changed:" / "config-changed:" log tags: the unpatched
 * CONFIG_CHANGED body logs "config changed:" and never emits a
 * geometry line, so the tag assertions fail while the mapping
 * assertions in that case still pass (the launcher's inline body
 * did refresh the mapping; the wiring preserves it).
 */
/* Must precede every system include: server.c needs clock_gettime,
 * struct ucred and SO_PEERCRED (all _GNU_SOURCE-gated), and it
 * defines _GNU_SOURCE itself only at its own top, after this file's
 * includes have already pulled the headers in. Defining it here
 * makes the identical define in server.c a silent no-op. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include "android/log.h"
#include "android/native_window.h"
#include "android/configuration.h"
#include "android_native_app_glue.h"

/* White-box: on_cmd, on_window_size_changed, g_s. */
#include "server.c"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static struct android_app g_app;
static ANativeWindow *g_win;
static AConfiguration *g_cfg;

/* Mirror android_main's state init, minus the threads and looper:
 * no probe/socket thread runs in this test, so nothing else touches
 * g_s and the test is single-threaded. */
static void test_state_init(int32_t win_w, int32_t win_h, bool have_hello,
                            uint32_t client_w, uint32_t client_h) {
    memset(&g_s, 0, sizeof(g_s));
    pthread_mutex_init(&g_s.draw_lock, NULL);
    pthread_mutex_init(&g_s.lock, NULL);
    pthread_mutex_init(&g_s.send_lock, NULL);
    pthread_mutex_init(&g_s.probe_lock, NULL);
    pthread_cond_init(&g_s.probe_cond, NULL);
    isws_sysinfo_init(&g_s.sysinfo);
    g_win = test_window_create(win_w, win_h);
    g_cfg = test_config_create(ACONFIGURATION_ORIENTATION_PORT);
    memset(&g_app, 0, sizeof(g_app));
    g_app.window = g_win;
    g_app.config = g_cfg;
    g_app.activity = NULL;
    g_app.destroyRequested = 0;
    g_s.app = &g_app;
    g_s.client_fd = -1;
    g_s.memfd = -1;
    g_s.stats_last_log_ns = now_ns();
    atomic_store(&g_s.running, true);
    g_s.have_hello = have_hello;
    if (have_hello) {
        g_s.hello.version = ISWS_VERSION;
        g_s.hello.width = client_w;
        g_s.hello.height = client_h;
        g_s.hello.stride = client_w * 4u;
        g_s.hello.format = ISWS_FORMAT_BGRA8888_PREMUL;
        g_s.hello.flags = 0;
    }
    test_log_reset();
}

static void test_state_fini(void) {
    test_window_destroy(g_win);
    g_win = NULL;
    free(g_cfg);
    g_cfg = NULL;
}

/* Assert the stored mapping covers the given window exactly. */
static void check_mapping_covers(int32_t win_w, int32_t win_h) {
    int32_t x0, y0, x1, y1;
    CHECK(g_s.mapping_valid);
    CHECK(g_s.mapping.screen_w == (uint32_t)win_w);
    CHECK(g_s.mapping.screen_h == (uint32_t)win_h);
    CHECK(g_s.window_w == win_w && g_s.window_h == win_h);
    isws_dest_rect(&g_s.mapping, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 0 && x1 == win_w && y1 == win_h);
}

static void test_config_changed_revalidates(void) {
    /* Regression setup: Aidoku client 1440x3200, window
     * still 1080x2400, so INIT_WINDOW computes a 1080x2400 mapping. */
    test_state_init(1080, 2400, true, 1440, 3200);
    on_cmd(&g_app, APP_CMD_INIT_WINDOW);
    CHECK(g_s.mapping_valid);
    CHECK(g_s.mapping.screen_w == 1080u && g_s.mapping.screen_h == 2400u);

    /* Window grows to the full 1440x3200; CONFIG_CHANGED must
     * re-validate the mapping instead of painting 1:1 into the
     * bigger buffer (180 px left margin, 400 px top bar). */
    test_window_resize(g_win, 1440, 3200);
    test_log_reset();
    on_cmd(&g_app, APP_CMD_CONFIG_CHANGED);
    check_mapping_covers(1440, 3200);
    CHECK(test_log_contains("geometry config-changed: window=1440x3200"));
    CHECK(test_log_contains("config-changed: window 1440x3200 rotation=0"));
    test_state_fini();
}

static void test_window_resized_revalidates(void) {
    isws_info_t before;
    test_state_init(1080, 2400, true, 1440, 3200);
    on_cmd(&g_app, APP_CMD_INIT_WINDOW);
    test_window_resize(g_win, 1440, 3200);
    on_cmd(&g_app, APP_CMD_CONFIG_CHANGED);
    check_mapping_covers(1440, 3200);

    /* Same resize, now delivered as WINDOW_RESIZED: the 021 gate.
     * On the unpatched tree this command is unhandled and the
     * mapping stays 1440x3200 while the window is 720x1600. */
    test_window_resize(g_win, 720, 1600);
    test_log_reset();
    on_cmd(&g_app, APP_CMD_WINDOW_RESIZED);
    check_mapping_covers(720, 1600);
    CHECK(test_log_contains("geometry window-resized: window=720x1600"));
    CHECK(test_log_contains("window-resized: window 720x1600 rotation=0"));

    /* Shared-path pin: back to 1440x3200 via WINDOW_RESIZED must
     * produce byte-identical mapping state to the CONFIG_CHANGED
     * path for the same window size. */
    test_window_resize(g_win, 1440, 3200);
    on_cmd(&g_app, APP_CMD_WINDOW_RESIZED);
    before = g_s.mapping;
    test_window_resize(g_win, 720, 1600);
    on_cmd(&g_app, APP_CMD_CONFIG_CHANGED);
    test_window_resize(g_win, 1440, 3200);
    on_cmd(&g_app, APP_CMD_WINDOW_RESIZED);
    CHECK(memcmp(&before, &g_s.mapping, sizeof(before)) == 0);
    test_state_fini();
}

static void test_resize_launcher_no_hello(void) {
    /* No client connected yet: a resize must repaint the launcher
     * screen (which lays text out for the window size) and must not
     * invent a mapping without a hello. */
    test_state_init(1080, 2400, false, 0, 0);
    on_cmd(&g_app, APP_CMD_INIT_WINDOW);
    CHECK(!g_s.mapping_valid);
    CHECK(g_s.ui_layout.btn_primary.w > 0);
    test_window_resize(g_win, 1440, 3200);
    test_log_reset();
    on_cmd(&g_app, APP_CMD_CONFIG_CHANGED);
    CHECK(!g_s.mapping_valid);
    CHECK(g_s.window_w == 1440 && g_s.window_h == 3200);
    CHECK(g_s.ui_layout.btn_primary.w > 0);
    test_window_resize(g_win, 720, 1600);
    on_cmd(&g_app, APP_CMD_WINDOW_RESIZED);
    CHECK(!g_s.mapping_valid);
    CHECK(g_s.window_w == 720 && g_s.window_h == 1600);
    test_state_fini();
}

static void test_resize_no_window(void) {
    /* TERM_WINDOW then a resize: no window, no crash, no mapping. */
    test_state_init(1080, 2400, true, 1440, 3200);
    on_cmd(&g_app, APP_CMD_INIT_WINDOW);
    on_cmd(&g_app, APP_CMD_TERM_WINDOW);
    CHECK(!g_s.mapping_valid);
    CHECK(g_s.window == NULL);
    on_cmd(&g_app, APP_CMD_WINDOW_RESIZED);
    CHECK(!g_s.mapping_valid);
    on_cmd(&g_app, APP_CMD_CONFIG_CHANGED);
    CHECK(!g_s.mapping_valid);
    test_state_fini();
}

int main(void) {
    test_config_changed_revalidates();
    test_window_resized_revalidates();
    test_resize_launcher_no_hello();
    test_resize_no_window();
    printf("test_server_resize: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
