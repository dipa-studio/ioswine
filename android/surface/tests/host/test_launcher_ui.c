/*
 * Host unit test for android/surface/src/server/sysinfo.c and ui.c.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8x16.h"
#include "sysinfo.h"
#include "ui.h"

static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_formatters(void) {
    char buf[64];
    isws_fmt_kb(-1, buf, sizeof(buf));
    CHECK(strcmp(buf, "-") == 0);
    isws_fmt_kb(512, buf, sizeof(buf));
    CHECK(strcmp(buf, "512 KB") == 0);
    isws_fmt_kb(2048, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.0 MB") == 0);
    isws_fmt_kb(2LL * 1024 * 1024, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.00 GB") == 0);

    isws_fmt_bytes(-1, buf, sizeof(buf));
    CHECK(strcmp(buf, "-") == 0);
    isws_fmt_bytes(500, buf, sizeof(buf));
    CHECK(strcmp(buf, "500 B") == 0);
    isws_fmt_bytes(4096, buf, sizeof(buf));
    CHECK(strcmp(buf, "4.0 KB") == 0);

    isws_fmt_duration(-1, buf, sizeof(buf));
    CHECK(strcmp(buf, "-") == 0);
    isws_fmt_duration(42, buf, sizeof(buf));
    CHECK(strcmp(buf, "42s") == 0);
    isws_fmt_duration(125, buf, sizeof(buf));
    CHECK(strcmp(buf, "2m 05s") == 0);
    isws_fmt_duration(3720, buf, sizeof(buf));
    CHECK(strcmp(buf, "1h 02m") == 0);

    isws_fmt_epoch(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "-") == 0);
    isws_fmt_epoch(1700000000, buf, sizeof(buf));
    CHECK(strlen(buf) == 16 && buf[4] == '-' && buf[10] == ' ');
}

static void test_sysinfo_parse(void) {
    isws_sysinfo_t sys;
    isws_sysinfo_parse_probe(&sys, NULL);
    CHECK(sys.probed);
    CHECK(!sys.root_available);
    CHECK(strcmp(sys.selinux, "unknown") == 0);
    CHECK(sys.du_total_kb == -1);

    const char *sample =
        "ROOT=1\n"
        "SELINUX=Enforcing\n"
        "DU_TOTAL=6400000\n"
        "DU_root=12000\n"
        "DU_dsc=6200000\n"
        "DU_Aidoku.app=45000\n"
        "DU_home=1800\n"
        "DF_DATA=/dev/block/dm-5 234567890 120000000 114567890 52% /data\n"
        "FILE_ioswine-loader=245760 1700000000 abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n"
        "FILE_libsystem_kernel.dylib=512000 1700000100 1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff\n"
        "PID=\n"
        "===RELEASE_BEGIN===\n"
        "version: v0.1-42-g1234abc\n"
        "date: 2026-11-17T20:00:00+01:00\n"
        "===RELEASE_END===\n"
        "===LOG_META_BEGIN===\n"
        "=== ioswine launch start=1700000200 ===\n"
        "stats: presented=321 dropped=0\n"
        "FATAL: SIGSEGV at 0xdeadbeef\n"
        "=== ioswine launch exit=139 duration=17s ===\n"
        "===LOG_META_END===\n"
        "===LOG_TAIL_BEGIN===\n"
        "boot ok\n"
        "FATAL: SIGSEGV at 0xdeadbeef\n"
        "===LOG_TAIL_END===\n";

    isws_sysinfo_parse_probe(&sys, sample);
    CHECK(sys.root_available);
    CHECK(strcmp(sys.selinux, "Enforcing") == 0);
    CHECK(sys.du_total_kb == 6400000);
    CHECK(sys.du_root_kb == 12000);
    CHECK(sys.du_dsc_kb == 6200000);
    CHECK(sys.du_app_kb == 45000);
    CHECK(sys.du_home_kb == 1800);
    CHECK(sys.data_total_kb == 234567890);
    CHECK(sys.data_avail_kb == 114567890);
    CHECK(sys.loader_info.exists && sys.loader_info.size_bytes == 245760);
    CHECK(strncmp(sys.loader_info.sha256, "abcdef012345", 12) == 0);
    CHECK(sys.dylib_info.exists && sys.dylib_info.size_bytes == 512000);
    CHECK(!sys.loader_running);
    CHECK(strstr(sys.release_txt, "v0.1-42-g1234abc") != NULL);
    CHECK(sys.has_last_run);
    CHECK(sys.last_exit_code == 139);
    CHECK(sys.last_duration_s == 17);
    CHECK(sys.last_frames == 321);
    CHECK(strstr(sys.last_crash, "SIGSEGV") != NULL);
    CHECK(strstr(sys.log_tail, "boot ok") != NULL);
}

static void test_launcher_draw_and_hit(int32_t w, int32_t h, int32_t stride) {
    uint32_t *buf = calloc((size_t)stride * (size_t)h, sizeof(uint32_t));
    CHECK(buf != NULL);

    /* Put sentinel in stride padding beyond w. */
    if (stride > w) {
        for (int32_t y = 0; y < h; y++) {
            buf[(size_t)y * (size_t)stride + (size_t)w] = 0xCAFEBABEu;
        }
    }

    isws_sysinfo_t sys;
    isws_sysinfo_init(&sys);
    sys.probed = true;
    sys.root_available = true;
    snprintf(sys.selinux, sizeof(sys.selinux), "Enforcing");
    sys.du_total_kb = 6000000;
    sys.data_avail_kb = 100000000;
    sys.data_total_kb = 200000000;

    ui_launcher_state_t st = {
        .sys = &sys,
        .view = UI_VIEW_LAUNCHER,
        .launching = false,
        .stopping = false,
        .status_msg = "Ready",
        .have_session_stats = true,
        .last_session_frames = 120,
        .last_session_dropped = 1,
        .last_session_duration_s = 12,
    };
    ui_layout_t lay;
    ui_draw_launcher(buf, w, h, stride, &st, &lay);

    /* Stride sentinel untouched. */
    if (stride > w) {
        for (int32_t y = 0; y < h; y++) {
            CHECK(buf[(size_t)y * (size_t)stride + (size_t)w] == 0xCAFEBABEu);
        }
    }

    /* Buttons inside screen bounds and hittable. */
    CHECK(!lay.primary_is_stop);
    CHECK(lay.btn_primary.w > 0 && lay.btn_primary.h > 0);
    CHECK(lay.btn_primary.x >= 0 && lay.btn_primary.x + lay.btn_primary.w <= w);
    CHECK(lay.btn_primary.y >= 0 && lay.btn_primary.y + lay.btn_primary.h <= h);

    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LAUNCHER,
                               (float)(lay.btn_primary.x + lay.btn_primary.w / 2),
                               (float)(lay.btn_primary.y + lay.btn_primary.h / 2)) == UI_ACT_LAUNCH);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LAUNCHER,
                               (float)(lay.btn_refresh.x + lay.btn_refresh.w / 2),
                               (float)(lay.btn_refresh.y + lay.btn_refresh.h / 2)) == UI_ACT_REFRESH);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LAUNCHER,
                               (float)(lay.btn_log.x + lay.btn_log.w / 2),
                               (float)(lay.btn_log.y + lay.btn_log.h / 2)) == UI_ACT_VIEW_LOG);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LAUNCHER, 0.0f, 0.0f) == UI_ACT_NONE);

    /* Running state flips primary button to STOP. */
    st.launching = true;
    ui_draw_launcher(buf, w, h, stride, &st, &lay);
    CHECK(lay.primary_is_stop);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LAUNCHER,
                               (float)(lay.btn_primary.x + lay.btn_primary.w / 2),
                               (float)(lay.btn_primary.y + lay.btn_primary.h / 2)) == UI_ACT_STOP);

    /* Log view buttons. */
    st.launching = false;
    st.view = UI_VIEW_LOG;
    ui_draw_launcher(buf, w, h, stride, &st, &lay);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LOG,
                               (float)(lay.btn_log_back.x + lay.btn_log_back.w / 2),
                               (float)(lay.btn_log_back.y + lay.btn_log_back.h / 2)) == UI_ACT_LOG_BACK);
    CHECK(ui_hit_test_launcher(&lay, UI_VIEW_LOG,
                               (float)(lay.btn_refresh.x + lay.btn_refresh.w / 2),
                               (float)(lay.btn_refresh.y + lay.btn_refresh.h / 2)) == UI_ACT_REFRESH);

    free(buf);
}

static void test_overlay_draw_and_hit(void) {
    int32_t w = 1080, h = 2400, stride = 1088;
    uint32_t *buf = malloc((size_t)stride * (size_t)h * sizeof(uint32_t));
    CHECK(buf != NULL);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < stride; x++) {
            buf[(size_t)y * (size_t)stride + (size_t)x] = 0xFF102030u;
        }
    }

    ui_overlay_state_t ov = {
        .fps = 59.8,
        .frames_presented = 1234,
        .frames_dropped = 2,
        .touches_sent = 55,
        .uptime_s = 90,
        .client_pid = 4321,
    };
    ui_layout_t lay;
    ui_draw_overlay(buf, w, h, stride, &ov, &lay);

    CHECK(lay.overlay_box.w > 100 && lay.overlay_box.h > 80);
    /* Bottom-right outside overlay must remain untouched. */
    CHECK(buf[(size_t)(h - 1) * (size_t)stride + (size_t)(w - 1)] == 0xFF102030u);
    /* Inside overlay must have changed. */
    CHECK(buf[(size_t)(lay.overlay_box.y + 4) * (size_t)stride + (size_t)(lay.overlay_box.x + 4)] != 0xFF102030u);

    CHECK(ui_hit_test_overlay(&lay,
                              (float)(lay.btn_overlay_stop.x + lay.btn_overlay_stop.w / 2),
                              (float)(lay.btn_overlay_stop.y + lay.btn_overlay_stop.h / 2)) == UI_ACT_OVERLAY_STOP);
    CHECK(ui_hit_test_overlay(&lay,
                              (float)(lay.btn_overlay_hide.x + lay.btn_overlay_hide.w / 2),
                              (float)(lay.btn_overlay_hide.y + lay.btn_overlay_hide.h / 2)) == UI_ACT_OVERLAY_HIDE);
    CHECK(ui_hit_test_overlay(&lay, 1000.0f, 2000.0f) == UI_ACT_NONE);

    free(buf);
}

static void test_font(void) {
    CHECK(font8x16_glyph('A') != font8x16_glyph('B'));
    CHECK(font8x16_glyph('\x01') == font8x16_glyph('?'));
    CHECK(font8x16_glyph('\xff') == font8x16_glyph('?'));
    const uint8_t *sp = font8x16_glyph(' ');
    for (int i = 0; i < FONT8X16_H; i++) CHECK(sp[i] == 0);
}

int main(void) {
    test_font();
    test_formatters();
    test_sysinfo_parse();
    test_launcher_draw_and_hit(1080, 2400, 1088);
    test_launcher_draw_and_hit(1440, 3200, 1440);
    test_launcher_draw_and_hit(2400, 1080, 2400);
    test_launcher_draw_and_hit(400, 600, 400);
    test_overlay_draw_and_hit();
    printf("test_launcher_ui: %d checks, 0 failures\n", g_checks);
    return 0;
}
