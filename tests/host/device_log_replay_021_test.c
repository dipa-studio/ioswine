/*
 * device_log_replay_021_test.c - Replay device-round logs.
 *
 * Part 1 pins the exact failure signatures from the two device logs
 * (verbatim excerpts).
 * Part 2 replays the logged tap inputs through the REAL touch.c
 * pipeline, route C (IOSWINE_TOUCH_DIRECT=1, the route run on
 * device), with the device-faithful async mock (mock_uikit_async.h).
 * It asserts the shared invariants (normalized coords within the
 * presenter wobble, context id, sendEvent: + window sendEvent: reached,
 * no drops) and branches on whether the interim TouchUpInside hack fired:
 * on the unpatched tree it asserts the device outcome (hack fires
 * for tab/button taps, never for SwiftUI cell taps); on a
 * fixed tree it asserts the same deliveries with no hack.
 * Part 3 is the gate: a logged tab tap must deliver through
 * sendEvent: + window sendEvent: with NO TouchUpInside hack
 * firing. It FAILS on the unpatched tree (hack fires) and PASSES
 * once the hack is removed and UIControl tracking completes.
 * Part 4 is crash-sequence liveness: after replaying the exact tap
 * sequence that crashed the device (crash B), the cadisplay vsync
 * clock (cadisplay_on_vsync, the feed that drives CADisplayLink)
 * still advances tick by tick and the touch pipeline is clean
 * (no drops, queues drained). It pins "no fault in our pipeline
 * for the exact crash inputs"; only the device run can prove
 * UIKit's displayLinkWithDisplay: itself no longer faults, because
 * the SIGSEGV is inside UIKit (nil CADisplay), not in our code.
 *
 * Dependency: UIControl tracking. If the "[touch] direct fire TouchUpInside"
 * log line is kept for diagnostics while removing the sendActions call,
 * part 3 stays red: retarget the gate at g_touchupinside_count, not at
 * the log line.
 *
 * The mock cannot know the real view class names
 * (object_getClassName is not modelled), so the "[touch] direct
 * kif sendEvent view=" log line prints an empty view name on host;
 * the test asserts the prefix and phase only, never the class.
 * The mock models only the Browse tab rect (x 72..144, y 752..800);
 * the logged tab taps are on other tabs (x 236..327), so no mock
 * tab flip is expected for them; tab switching for those tabs is
 * proven on device (live-taps shows the tabs switching) and by the
 * selftest gate in device_log_replay_test.
 *
 * Plain cc + ASan/UBSan on Linux host. No Apple code, no device.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_uikit_async.h"
#include "cadisplay.h"

/* iosurface stubs: cadisplay.c links against the IOSurface table,
 * which has no host instance here. The vsync path under test never
 * touches a surface; the stubs only satisfy the linker. */
int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    (void)surface_id;
    (void)out;
    return 15; /* KERN_INVALID_NAME: no surfaces on host. */
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    (void)surface_id;
    (void)fd_out;
    return 15; /* KERN_INVALID_NAME. */
}

uint32_t iosurface_surface_count(void) {
    return 0;
}

/* ------------------------------------------------------------------ */
/* Harness                                                            */
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

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Part 1: verbatim device-log excerpts + failure signatures          */
/* ------------------------------------------------------------------ */

/* Verbatim excerpts from crash log: 36 pump frames at 720x1600,
 * then two taps, then crash (B). */
static const char kCrashLog[] =
    "[lsk] ioswine-019 2026-11-17\n"
    "[snapshot] pump armed (thread, 1000 ms)\n"
    "[present] geometry client=720x1600 scale=1.93 offset=24,0\n"
    "[snapshot] frame 1 720x1600 windows=2 rc=0\n"
    "2026-11-17 20:06:55.271 Aidoku[18958:0] could not create directory \"/local/com.apple.WebKit.GPU\" for future sandbox extension, error Error Domain=NSCocoaErrorDomain Code=642 ...\n"
    "[snapshot] frame 36 720x1600 windows=2 rc=0\n"
    "[touch] down x=271 y=40 id=0\n"
    "[touch] hid-built down nx=754 ny=51 id=0 wire_ts=2917711953990\n"
    "[touch] direct kif sendEvent view=_UIButtonBarButton phase=began\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] up x=271 y=40 id=0\n"
    "[touch] hid-built up nx=754 ny=51 id=0 wire_ts=2917829177948\n"
    "[touch] direct kif sendEvent view=_UIButtonBarButton phase=ended\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] direct fire TouchUpInside\n"
    "[touch] down x=266 y=24 id=0\n"
    "[touch] hid-built down nx=740 ny=30 id=0 wire_ts=2918081539875\n"
    "[touch] direct kif sendEvent view=SwiftUI.UpdateCoalescingCollectionView phase=began\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] up x=266 y=24 id=0\n"
    "[touch] hid-built up nx=740 ny=30 id=0 wire_ts=2918177509354\n"
    "[touch] direct kif sendEvent view=SwiftUI.UpdateCoalescingCollectionView phase=ended\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[lsk CRASH] sig=0x000000000000000b pc=0x0000000183f96b9c lr=0x0000000183f96b98 sp=0x0000007e438a79d0 fp=0x0000007e438a7a10 x0=0x0000000501651500 x8=0x0000000000000000 x9=0x0000000000000000 x16=0x952d000501651500\n"
    "=== ioswine launch exit=139 duration=10s ===\n";

/* live-taps-20261117.txt, verbatim excerpts: the first tab tap, the
 * last tab tap, the SwiftUI cell tap, then crash (C). The full log
 * has 13 tab taps (UITabBarButton, each ending in the hack line)
 * and no banner and no frame lines (filtered [touch] extract). */
static const char kLiveTaps[] =
    "[touch] reader started\n"
    "[touch] down x=321 y=774 id=0\n"
    "[touch] hid-built down nx=891 ny=967 id=0 wire_ts=1068868323080\n"
    "[touch] direct kif sendEvent view=UITabBarButton phase=began\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] up x=321 y=774 id=0\n"
    "[touch] hid-built up nx=891 ny=967 id=0 wire_ts=1068921173810\n"
    "[touch] direct kif sendEvent view=UITabBarButton phase=ended\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] direct fire TouchUpInside\n"
    "[touch] down x=238 y=779 id=0\n"
    "[touch] hid-built down nx=661 ny=973 id=0 wire_ts=1072152533965\n"
    "[touch] direct kif sendEvent view=UITabBarButton phase=began\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] up x=238 y=779 id=0\n"
    "[touch] hid-built up nx=661 ny=973 id=0 wire_ts=1072194525683\n"
    "[touch] direct kif sendEvent view=UITabBarButton phase=ended\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] direct fire TouchUpInside\n"
    "[touch] down x=174 y=534 id=0\n"
    "[touch] hid-built down nx=484 ny=667 id=0 wire_ts=1075412897557\n"
    "[touch] direct kif sendEvent view=_TtGC7SwiftUI15CellHostingViewGVS_15ModifiedContentVS_14_ViewList_ViewVS_26CollectionViewCellModifier__ phase=began\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[touch] up x=174 y=534 id=0\n"
    "[touch] hid-built up nx=484 ny=667 id=0 wire_ts=1075488407609\n"
    "[touch] direct kif sendEvent view=_TtGC7SwiftUI15CellHostingViewGVS_15ModifiedContentVS_14_ViewList_ViewVS_26CollectionViewCellModifier__ phase=ended\n"
    "[touch] sendEvent: reached with touch count 1\n"
    "[touch] window sendEvent:\n"
    "[lsk CRASH] sig=0x000000000000000b pc=0x00000001b6cecb70 lr=0x00000001b6cec914 sp=0x0000007293ade760 fp=0x0000007293ade7f0 x0=0x0000000000000001 x8=0x0000000000000000 x9=0x0000000000000020 x16=0x04f80001ef768bd0\n";


/* Parse the FIRST "[touch] hid-built <phase> nx=N ny=M ..." line
 * out of the mock log. Returns 0 on success. FIRST, not last: the
 * mock log buffer is 8192 bytes and keeps the earliest lines, so on
 * a long replay the tail is truncated away. The device nx/ny wobble
 * by up to 2 from the pure x/360*1000 computation (presenter float
 * mapping, e.g. (271,40) logs nx=754 not 753, (266,24) logs nx=740
 * not 739), so callers compare with eps 0.005, not exactly. */
static int first_hid_built(const char *phase, int *nx_out, int *ny_out) {
    char needle[64];
    const char *p;
    snprintf(needle, sizeof(needle), "[touch] hid-built %s nx=", phase);
    p = strstr(g_log_buf, needle);
    if (!p) return -1;
    return sscanf(p + strlen(needle), "%d ny=%d", nx_out, ny_out) == 2 ? 0 : -1;
}

static void test_log_signatures(void) {
    /* Crash log: banner check. */
    CHECK(strstr(kCrashLog, "[lsk] ioswine-019 2026-11-17\n") != NULL);
    CHECK(strstr(kCrashLog, "[lsk] ioswine-020") == NULL);
    CHECK(strstr(kCrashLog, "[snapshot] pump armed (thread, 1000 ms)\n") != NULL);
    CHECK(strstr(kCrashLog, "[present] geometry client=720x1600 scale=1.93 offset=24,0\n") != NULL);
    CHECK(strstr(kCrashLog, "[snapshot] frame 1 720x1600 windows=2 rc=0\n") != NULL);
    CHECK(strstr(kCrashLog, "[snapshot] frame 36 720x1600 windows=2 rc=0\n") != NULL);
    /* Crash (B): SIGSEGV (sig=0xb) at
     * +[CADisplayLink displayLinkWithDisplay:target:selector:]+84,
     * pc 0x183f96b9c, x8=0 (no CADisplay). */
    CHECK(strstr(kCrashLog, "[lsk CRASH] sig=0x000000000000000b pc=0x0000000183f96b9c") != NULL);
    CHECK(strstr(kCrashLog, "x8=0x0000000000000000") != NULL);
    CHECK(strstr(kCrashLog, "=== ioswine launch exit=139 duration=10s ===\n") != NULL);
    /* Button tap fired the hack; the SwiftUI cell tap did not. */
    CHECK(strstr(kCrashLog, "[touch] direct kif sendEvent view=_UIButtonBarButton phase=began\n") != NULL);
    CHECK(strstr(kCrashLog, "[touch] direct kif sendEvent view=SwiftUI.UpdateCoalescingCollectionView phase=began\n") != NULL);
    CHECK(count_occurrences(kCrashLog, "[touch] direct fire TouchUpInside\n") == 1);
    CHECK(count_occurrences(kCrashLog, "[touch] sendEvent: reached with touch count 1\n") == 4);
    CHECK(count_occurrences(kCrashLog, "[touch] window sendEvent:\n") == 4);
    /* Live taps: 13 tab taps, each ending in the hack line, then the
     * cell tap with no hack line, then crash (C). */
    CHECK(strstr(kLiveTaps, "[touch] reader started\n") != NULL);
    CHECK(strstr(kLiveTaps, "[touch] hid-built down nx=891 ny=967 id=0 wire_ts=1068868323080\n") != NULL);
    CHECK(strstr(kLiveTaps, "[touch] hid-built down nx=484 ny=667 id=0 wire_ts=1075412897557\n") != NULL);
    CHECK(strstr(kLiveTaps, "[touch] direct kif sendEvent view=_TtGC7SwiftUI15CellHostingViewGVS_15ModifiedContentVS_14_ViewList_ViewVS_26CollectionViewCellModifier__ phase=began\n") != NULL);
    /* Crash (C): SIGSEGV (sig=0xb) at AG::Graph::UpdateStack::update,
     * pc 0x1b6cecb70, x8=0. */
    CHECK(strstr(kLiveTaps, "[lsk CRASH] sig=0x000000000000000b pc=0x00000001b6cecb70") != NULL);
    /* WebKit evidence in the crash log: the
     * WebContent GPU sandbox dir could not be created (read-only
     * volume), the first hint the out-of-process WebContent path
     * cannot work as-is. */
    CHECK(strstr(kCrashLog, "could not create directory \"/local/com.apple.WebKit.GPU\" for future sandbox extension") != NULL);
}

/* ------------------------------------------------------------------ */
/* Part 2: replay the logged inputs through real touch.c, route C    */
/* ------------------------------------------------------------------ */

/* Crash-log taps: button (control ancestor) then SwiftUI cell (no
 * control ancestor). Wire timestamps verbatim from the log. */
static const struct {
    uint32_t action;
    float x;
    float y;
    int64_t ts;
    int is_control;
} kCrashTaps[] = {
    { ISWS_TOUCH_DOWN, 271.0f, 40.0f, 2917711953990LL, 1 },
    { ISWS_TOUCH_UP, 271.0f, 40.0f, 2917829177948LL, 1 },
    { ISWS_TOUCH_DOWN, 266.0f, 24.0f, 2918081539875LL, 0 },
    { ISWS_TOUCH_UP, 266.0f, 24.0f, 2918177509354LL, 0 },
};

/* Live-taps: 13 tab taps (control ancestor) then the SwiftUI cell
 * tap (no control ancestor). Wire timestamps verbatim from the log. */
static const struct {
    uint32_t action;
    float x;
    float y;
    int64_t ts;
    int is_control;
} kLiveTapsReplay[] = {
    { ISWS_TOUCH_DOWN, 321.0f, 774.0f, 1068868323080LL, 1 },
    { ISWS_TOUCH_UP, 321.0f, 774.0f, 1068921173810LL, 1 },
    { ISWS_TOUCH_DOWN, 311.0f, 778.0f, 1069033647455LL, 1 },
    { ISWS_TOUCH_UP, 311.0f, 778.0f, 1069075155528LL, 1 },
    { ISWS_TOUCH_DOWN, 306.0f, 783.0f, 1069169925216LL, 1 },
    { ISWS_TOUCH_UP, 306.0f, 783.0f, 1069227513237LL, 1 },
    { ISWS_TOUCH_DOWN, 327.0f, 782.0f, 1069333494747LL, 1 },
    { ISWS_TOUCH_UP, 327.0f, 782.0f, 1069380827872LL, 1 },
    { ISWS_TOUCH_DOWN, 310.0f, 777.0f, 1069492288705LL, 1 },
    { ISWS_TOUCH_UP, 310.0f, 777.0f, 1069549846830LL, 1 },
    { ISWS_TOUCH_DOWN, 316.0f, 772.0f, 1069649422403LL, 1 },
    { ISWS_TOUCH_UP, 316.0f, 772.0f, 1069701683966LL, 1 },
    { ISWS_TOUCH_DOWN, 310.0f, 770.0f, 1069931694278LL, 1 },
    { ISWS_TOUCH_UP, 310.0f, 770.0f, 1069992905424LL, 1 },
    { ISWS_TOUCH_DOWN, 258.0f, 777.0f, 1071350887559LL, 1 },
    { ISWS_TOUCH_UP, 258.0f, 777.0f, 1071398730632LL, 1 },
    { ISWS_TOUCH_DOWN, 239.0f, 781.0f, 1071511962767LL, 1 },
    { ISWS_TOUCH_UP, 239.0f, 781.0f, 1071539551621LL, 1 },
    { ISWS_TOUCH_DOWN, 236.0f, 778.0f, 1071647012038LL, 1 },
    { ISWS_TOUCH_UP, 236.0f, 778.0f, 1071697106465LL, 1 },
    { ISWS_TOUCH_DOWN, 239.0f, 778.0f, 1071801207767LL, 1 },
    { ISWS_TOUCH_UP, 239.0f, 778.0f, 1071858197246LL, 1 },
    { ISWS_TOUCH_DOWN, 242.0f, 777.0f, 1071977652454LL, 1 },
    { ISWS_TOUCH_UP, 242.0f, 777.0f, 1072027185996LL, 1 },
    { ISWS_TOUCH_DOWN, 238.0f, 779.0f, 1072152533965LL, 1 },
    { ISWS_TOUCH_UP, 238.0f, 779.0f, 1072194525683LL, 1 },
    { ISWS_TOUCH_DOWN, 174.0f, 534.0f, 1075412897557LL, 0 },
    { ISWS_TOUCH_UP, 174.0f, 534.0f, 1075488407609LL, 0 },
};

static void replay_setup(void) {
    touch_test_reset();
    reset_mocks();
    mock_async_setup_fixture();

    g_fake_context_id = 0x54321u;
    g_tab_selected_index = 0;

    touch_test_configure(&mock_async_ops_base);
    g_env_touch = "1";
    g_env_trace = "1";
    g_env_direct = "1"; /* route C, the route used on device */
    g_env_selftest = NULL;
    touch_init();
    touch_set_window_size(360.0, 800.0);
}

static void inject_one(uint32_t action, float x, float y, int64_t ts, int is_control) {
    isws_touch_t t;
    g_kif_hit_is_control = is_control;
    t.action = action;
    t.pointer_id = 0;
    t.pointer_count = 1;
    t.x = x;
    t.y = y;
    t.timestamp_ns = ts;
    CHECK(touch_test_process_touch(&t, 360, 800) == 0);
}

static void test_replay_crash_sequence(void) {
    replay_setup();

    for (size_t i = 0; i < sizeof(kCrashTaps) / sizeof(kCrashTaps[0]); i++) {
        inject_one(kCrashTaps[i].action, kCrashTaps[i].x, kCrashTaps[i].y,
                   kCrashTaps[i].ts, kCrashTaps[i].is_control);
    }

    /* Shared invariants, hack or no hack: every phase reached
     * sendEvent: and the window, coords land on the logged points,
     * no drop counters fired. The hid-built nx/ny are checked
     * numerically (last_hid_built) with eps 0.005, not exactly:
     * the device values wobble by up to 2 from the pure
     * x/360*1000 computation (presenter float mapping). */
    CHECK(g_direct_send_event_count == 4);
    CHECK(touch_test_sendevent_count() >= 4);
    CHECK(g_window_send_event_count == 4);
    CHECK(strstr(g_log_buf, "[touch] direct kif sendEvent view= phase=began\n") != NULL);
    CHECK(strstr(g_log_buf, "[touch] direct kif sendEvent view= phase=ended\n") != NULL);
    {
        int nx = 0, ny = 0;
        CHECK(first_hid_built("down", &nx, &ny) == 0);
        CHECK_NEAR(nx / 1000.0, 271.0 / 360.0, 0.005);
        CHECK_NEAR(ny / 1000.0, 40.0 / 800.0, 0.005);
    }
    CHECK_NEAR(g_last_direct_touch->loc_x, 266.0, 0.001);
    CHECK_NEAR(g_last_direct_touch->loc_y, 24.0, 0.001);
    CHECK(g_last_direct_phase == 3); /* UP phase is Ended */
    {
        uint64_t rf = 0, iv = 0, st = 0, ca = 0;
        touch_test_drop_counts(&rf, &iv, &st, &ca);
        CHECK(rf == 0 && iv == 0 && st == 0 && ca == 0);
    }

    if (g_touchupinside_count > 0) {
        /* Unpatched tree: the device outcome. The button tap fired
         * the hack once (on UP); the SwiftUI cell tap did not. */
        CHECK(g_touchupinside_count == 1);
        CHECK(strstr(g_log_buf, "[touch] direct fire TouchUpInside\n") != NULL);
        CHECK(count_occurrences(g_log_buf, "[touch] direct fire TouchUpInside\n") == 1);
    } else {
        /* Fixed tree: same deliveries, no hack. */
        CHECK(strstr(g_log_buf, "[touch] direct fire TouchUpInside\n") == NULL);
    }
}

static void test_replay_live_taps(void) {
    replay_setup();

    for (size_t i = 0; i < sizeof(kLiveTapsReplay) / sizeof(kLiveTapsReplay[0]); i++) {
        inject_one(kLiveTapsReplay[i].action, kLiveTapsReplay[i].x, kLiveTapsReplay[i].y,
                   kLiveTapsReplay[i].ts, kLiveTapsReplay[i].is_control);
    }

    /* 28 phases, 28 sendEvent: deliveries, 28 window deliveries,
     * no drops. */
    CHECK(g_direct_send_event_count == 28);
    CHECK(touch_test_sendevent_count() >= 28);
    CHECK(g_window_send_event_count == 28);
    {
        int nx = 0, ny = 0;
        CHECK(first_hid_built("down", &nx, &ny) == 0);
        CHECK_NEAR(nx / 1000.0, 321.0 / 360.0, 0.005);
        CHECK_NEAR(ny / 1000.0, 774.0 / 800.0, 0.005);
    }
    CHECK_NEAR(g_last_direct_touch->loc_x, 174.0, 0.001);
    CHECK_NEAR(g_last_direct_touch->loc_y, 534.0, 0.001);
    {
        uint64_t rf = 0, iv = 0, st = 0, ca = 0;
        touch_test_drop_counts(&rf, &iv, &st, &ca);
        CHECK(rf == 0 && iv == 0 && st == 0 && ca == 0);
    }

    if (g_touchupinside_count > 0) {
        /* Unpatched tree: the device outcome. 13 tab taps fired
         * the hack (one per UP); the cell tap did not. */
        /* The counter is authoritative here: the 8192-byte mock
         * log truncates on a 28-phase replay, so counting the hack
         * line in the log undercounts (12 of 13 survive). */
        CHECK(g_touchupinside_count == 13);
    } else {
        /* Fixed tree: same deliveries, no hack. */
        CHECK(strstr(g_log_buf, "[touch] direct fire TouchUpInside\n") == NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Part 3: the gate. Tab tap delivers with NO TouchUpInside hack.    */
/* ------------------------------------------------------------------ */

static void test_tracking_gate(void) {
    replay_setup();

    /* Logged tab tap (321, 774), the first tap in live-taps. */
    inject_one(ISWS_TOUCH_DOWN, 321.0f, 774.0f, 1068868323080LL, 1);
    inject_one(ISWS_TOUCH_UP, 321.0f, 774.0f, 1068921173810LL, 1);

    CHECK(g_direct_send_event_count == 2);
    CHECK(touch_test_sendevent_count() >= 2);
    CHECK(g_window_send_event_count == 2);
    /* GATE: fails on the unpatched tree (hack fires, count 1),
     * passes once the hack is removed. */
    CHECK(g_touchupinside_count == 0);
    CHECK(strstr(g_log_buf, "[touch] direct fire TouchUpInside\n") == NULL);
}

/* ------------------------------------------------------------------ */
/* Part 4: crash-sequence liveness + vsync clock advances.           */
/* ------------------------------------------------------------------ */

static void test_crash_liveness_vsync(void) {
    replay_setup();

    for (size_t i = 0; i < sizeof(kCrashTaps) / sizeof(kCrashTaps[0]); i++) {
        inject_one(kCrashTaps[i].action, kCrashTaps[i].x, kCrashTaps[i].y,
                   kCrashTaps[i].ts, kCrashTaps[i].is_control);
    }

    /* The exact tap sequence that crashed the device leaves our
     * pipeline clean: no drops, nothing queued, deliveries exact. */
    {
        uint64_t rf = 0, iv = 0, st = 0, ca = 0;
        touch_test_drop_counts(&rf, &iv, &st, &ca);
        CHECK(rf == 0 && iv == 0 && st == 0 && ca == 0);
    }
    CHECK(g_direct_send_event_count == 4);
    CHECK(g_window_send_event_count == 4);

    /* The vsync clock that feeds CADisplayLink still advances tick
     * by tick after the crash sequence: 10 ticks at 60 Hz from a
     * 1 s base, each next_vsync exactly one 16.666 ms period on. */
    CHECK(cadisplay_init(1440, 3200, 4) == 0);
    {
        int64_t t = 1000000000LL;
        int64_t prev_next = -1;
        for (int i = 0; i < 10; i++) {
            int64_t want_next;
            CHECK(cadisplay_on_vsync(t) == 0);
            want_next = t + 1000000000LL / 60;
            CHECK(cadisplay_next_vsync_ns() == want_next);
            CHECK(prev_next < 0 || cadisplay_next_vsync_ns() > prev_next);
            prev_next = cadisplay_next_vsync_ns();
            t += 1000000000LL / 60;
        }
    }
    cadisplay_shutdown();
}

int main(void) {
    test_log_signatures();
    test_replay_crash_sequence();
    test_replay_live_taps();
    test_tracking_gate();
    test_crash_liveness_vsync();
    printf("device_log_replay_021_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
