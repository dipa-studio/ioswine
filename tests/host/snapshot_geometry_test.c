/*
 * Host unit test for the 019 snapshot geometry fixes (snapshot.c).
 * Plain `cc`, no NDK, no Android headers:
 *   cc -std=c11 -Wall -Wextra -DSNAPSHOT_HOST_TEST=1 \
 *      -I <tree>/libsystem_kernel/src -I <tree>/android/surface/src/common \
 *      snapshot_geometry_test.c <tree>/libsystem_kernel/src/snapshot.c \
 *      -o snapshot_geometry_test && ./snapshot_geometry_test
 *
 * Mock Objective-C / CoreGraphics world, adapted from
 * tests/host/snapshot_test.c. Scenarios:
 *   1. UIScreen scale 1.0 but presenter INFO says 1440x3200: the
 *      frame is 1440x3200 (INFO scale wins) and the log says so.
 *   2. No INFO and UIScreen scale 1.0: the frame stays 360x800.
 *   3. UIScreen scale 4.0, no INFO: the frame is 1440x3200.
 *   4. The canvas is opaque white before renderInContext paints it:
 *      a mock layer scans the whole buffer at render time and counts
 *      non-white words; the count must be 0.
 *   5. Frame size change (INFO arrives late): present_shutdown is
 *      called once and the next present is at the new size.
 *   6. A failed present does not record a size, so no shutdown is
 *      issued for the size change that follows it.
 *   7. Insane INFO scales are ignored (too small and too large).
 *   8. Landscape INFO (rotation 90, 3200x1440 window): the frame
 *      follows the window (aspect-fit scale from INFO).
 *   9. Stale scale source (020): UIScreen scale 3.0 with INFO
 *      1440x3200: the frame is 1440x3200, and the INFO log names
 *      the bounds and the INFO screen size.
 */
#define SNAPSHOT_HOST_TEST 1

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"
#include "protocol.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Test environment / presenter stubs                                 */
/* ------------------------------------------------------------------ */

static const char *g_env_snapshot;
static const char *g_env_png;
static uint64_t g_now_ns;
static char g_log_buf[8192];
static size_t g_log_len;

static int g_present_calls;
static int g_present_rc;
static uint32_t g_present_w, g_present_h, g_present_stride;
static uint32_t g_present_first_pixel;
static int g_shutdown_calls;
static isws_info_t g_info;

int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    g_present_calls++;
    g_present_w = w;
    g_present_h = h;
    g_present_stride = stride;
    if (bgra && w > 0 && h > 0) {
        memcpy(&g_present_first_pixel, bgra, sizeof(uint32_t));
    }
    return g_present_rc;
}

isws_info_t present_last_info(void) {
    return g_info;
}

void present_shutdown(void) {
    g_shutdown_calls++;
}

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
    if (strcmp(name, "IOSWINE_SNAPSHOT_PNG") == 0) return g_env_png;
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

/* ------------------------------------------------------------------ */
/* Mock Objective-C & CoreGraphics objects                            */
/* ------------------------------------------------------------------ */

typedef struct {
    double x, y;
} test_point_t;

typedef struct {
    double width, height;
} test_size_t;

typedef struct {
    test_point_t origin;
    test_size_t size;
} test_rect_t;

enum {
    KIND_NONE = 0,
    KIND_CLASS_UIAPP,
    KIND_CLASS_UISCREEN,
    KIND_APP,
    KIND_SET,
    KIND_ARRAY,
    KIND_SCENE,
    KIND_WINDOW,
    KIND_SCREEN,
    KIND_LAYER,
};

typedef struct mock_obj {
    int kind;
    void *items[16];
    uint64_t count;
    uint8_t hidden;
    double alpha;
    test_rect_t bounds;
    test_rect_t frame;
    double level;
    struct mock_obj *screen;
    struct mock_obj *layer;
    double content_scale;
    double scale;
    struct mock_obj *scenes_set;
    struct mock_obj *legacy_windows;
    int tag;
    uint32_t fill_pixel;
} mock_obj_t;

typedef struct {
    void *data;
    uint32_t w, h, stride;
    uint32_t bitmap_info;
    double tx, ty;
    double sx, sy;
    int save_depth;
} mock_cg_ctx_t;

static mock_obj_t g_cls_uiapp = { .kind = KIND_CLASS_UIAPP };
static mock_obj_t g_cls_uiscreen = { .kind = KIND_CLASS_UISCREEN };
static mock_obj_t *g_shared_app = NULL;
static mock_obj_t *g_main_screen = NULL;

static int g_dispatch_main_q_dummy;
static uint64_t g_nonwhite_before_render;

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "UIApplication") == 0) return &g_cls_uiapp;
    if (strcmp(name, "UIScreen") == 0) return &g_cls_uiscreen;
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    return (void *)name;
}

static int sel_eq(void *sel, const char *name) {
    return sel && strcmp((const char *)sel, name) == 0;
}

static uint8_t mock_responds(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return 0;
    switch (o->kind) {
    case KIND_CLASS_UIAPP:
        return sel_eq(sel, "sharedApplication");
    case KIND_CLASS_UISCREEN:
        return sel_eq(sel, "mainScreen");
    case KIND_APP:
        return sel_eq(sel, "connectedScenes") || sel_eq(sel, "windows");
    case KIND_SET:
        return sel_eq(sel, "allObjects");
    case KIND_ARRAY:
        return sel_eq(sel, "count") || sel_eq(sel, "objectAtIndex:");
    case KIND_SCENE:
        return sel_eq(sel, "windows");
    case KIND_WINDOW:
        return sel_eq(sel, "isHidden") || sel_eq(sel, "alpha") ||
               sel_eq(sel, "bounds") || sel_eq(sel, "frame") ||
               sel_eq(sel, "windowLevel") || sel_eq(sel, "screen") ||
               sel_eq(sel, "contentScaleFactor") || sel_eq(sel, "layer");
    case KIND_SCREEN:
        return sel_eq(sel, "scale");
    case KIND_LAYER:
        return sel_eq(sel, "renderInContext:");
    default:
        return 0;
    }
}

static void *mock_objc_msgSend_id(void *obj, void *sel, ...) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return NULL;
    if (sel_eq(sel, "respondsToSelector:")) {
        __builtin_va_list ap;
        __builtin_va_start(ap, sel);
        void *qsel = __builtin_va_arg(ap, void *);
        __builtin_va_end(ap);
        return (void *)(uintptr_t)mock_responds(obj, qsel);
    }
    if (o->kind == KIND_CLASS_UIAPP && sel_eq(sel, "sharedApplication")) {
        return g_shared_app;
    }
    if (o->kind == KIND_CLASS_UISCREEN && sel_eq(sel, "mainScreen")) {
        return g_main_screen;
    }
    if (o->kind == KIND_APP) {
        if (sel_eq(sel, "connectedScenes")) return o->scenes_set;
        if (sel_eq(sel, "windows")) return o->legacy_windows;
    }
    if (o->kind == KIND_SET && sel_eq(sel, "allObjects")) {
        return o->items[0];
    }
    if (o->kind == KIND_ARRAY) {
        if (sel_eq(sel, "count")) return (void *)(uintptr_t)o->count;
        if (sel_eq(sel, "objectAtIndex:")) {
            __builtin_va_list ap;
            __builtin_va_start(ap, sel);
            uint64_t idx = __builtin_va_arg(ap, uint64_t);
            __builtin_va_end(ap);
            return idx < o->count ? o->items[idx] : NULL;
        }
    }
    if (o->kind == KIND_SCENE && sel_eq(sel, "windows")) {
        return o->items[0];
    }
    if (o->kind == KIND_WINDOW) {
        if (sel_eq(sel, "isHidden")) return (void *)(uintptr_t)o->hidden;
        if (sel_eq(sel, "screen")) return o->screen;
        if (sel_eq(sel, "layer")) return o->layer;
    }
    if (o->kind == KIND_LAYER && sel_eq(sel, "renderInContext:")) {
        __builtin_va_list ap;
        __builtin_va_start(ap, sel);
        mock_cg_ctx_t *ctx = __builtin_va_arg(ap, mock_cg_ctx_t *);
        __builtin_va_end(ap);
        /* The canvas must already be opaque white when the layer
         * starts painting: count words that are not 0xFFFFFFFF. */
        if (ctx && ctx->data) {
            uint32_t *px = (uint32_t *)ctx->data;
            uint64_t n = (uint64_t)ctx->w * ctx->h;
            uint64_t bad = 0;
            for (uint64_t i = 0; i < n; i++) {
                if (px[i] != 0xffffffffu) bad++;
            }
            g_nonwhite_before_render = bad;
            if (o->fill_pixel != 0) {
                px[0] = o->fill_pixel;
            }
        }
        return NULL;
    }
    return NULL;
}

static double mock_objc_msgSend_f64(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return 0.0;
    if (o->kind == KIND_WINDOW) {
        if (sel_eq(sel, "alpha")) return o->alpha;
        if (sel_eq(sel, "windowLevel")) return o->level;
        if (sel_eq(sel, "contentScaleFactor")) return o->content_scale;
    }
    if (o->kind == KIND_SCREEN && sel_eq(sel, "scale")) {
        return o->scale;
    }
    return 0.0;
}

static test_rect_t mock_objc_msgSend_rect(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    test_rect_t z = { {0, 0}, {0, 0} };
    if (!o || o->kind != KIND_WINDOW) return z;
    if (sel_eq(sel, "bounds")) return o->bounds;
    if (sel_eq(sel, "frame")) return o->frame;
    return z;
}

static void *mock_pool_push(void) {
    return (void *)(uintptr_t)0x1234;
}

static void mock_pool_pop(void *p) {
    (void)p;
}

static void *mock_cg_cs_create(void) {
    static int cs_dummy;
    return &cs_dummy;
}

static void mock_cg_cs_release(void *cs) { (void)cs; }

static void *mock_cg_ctx_create(void *data, size_t w, size_t h, size_t bpc,
                                size_t bpr, void *cs, uint32_t bitmapInfo) {
    (void)bpc;
    (void)cs;
    mock_cg_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->data = data;
    ctx->w = (uint32_t)w;
    ctx->h = (uint32_t)h;
    ctx->stride = (uint32_t)bpr;
    ctx->bitmap_info = bitmapInfo;
    return ctx;
}

static void mock_cg_ctx_release(void *c) { free(c); }

static void mock_cg_translate(void *c, double tx, double ty) {
    mock_cg_ctx_t *ctx = (mock_cg_ctx_t *)c;
    if (ctx->save_depth == 0) {
        ctx->tx = tx;
        ctx->ty = ty;
    }
}

static void mock_cg_scale(void *c, double sx, double sy) {
    mock_cg_ctx_t *ctx = (mock_cg_ctx_t *)c;
    ctx->sx = sx;
    ctx->sy = sy;
}

static void mock_cg_save(void *c) { ((mock_cg_ctx_t *)c)->save_depth++; }
static void mock_cg_restore(void *c) { ((mock_cg_ctx_t *)c)->save_depth--; }

static void *mock_cg_create_image(void *c) {
    (void)c;
    static int img_dummy;
    return &img_dummy;
}

static void mock_cg_image_release(void *i) { (void)i; }

static void *mock_dlsym(const char *name) {
    if (strcmp(name, "_dispatch_main_q") == 0) return &g_dispatch_main_q_dummy;
    if (strcmp(name, "dispatch_after_f") == 0) return NULL;
    if (strcmp(name, "dispatch_time") == 0) return NULL;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend_id;
    if (strcmp(name, "objc_msgSend_fpret") == 0) return (void *)mock_objc_msgSend_f64;
    if (strcmp(name, "objc_msgSend_stret") == 0) return (void *)mock_objc_msgSend_rect;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "CGColorSpaceCreateDeviceRGB") == 0) return (void *)mock_cg_cs_create;
    if (strcmp(name, "CGColorSpaceRelease") == 0) return (void *)mock_cg_cs_release;
    if (strcmp(name, "CGBitmapContextCreate") == 0) return (void *)mock_cg_ctx_create;
    if (strcmp(name, "CGContextRelease") == 0) return (void *)mock_cg_ctx_release;
    if (strcmp(name, "CGContextTranslateCTM") == 0) return (void *)mock_cg_translate;
    if (strcmp(name, "CGContextScaleCTM") == 0) return (void *)mock_cg_scale;
    if (strcmp(name, "CGContextSaveGState") == 0) return (void *)mock_cg_save;
    if (strcmp(name, "CGContextRestoreGState") == 0) return (void *)mock_cg_restore;
    if (strcmp(name, "CGBitmapContextCreateImage") == 0) return (void *)mock_cg_create_image;
    if (strcmp(name, "CGImageRelease") == 0) return (void *)mock_cg_image_release;
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
/* Fixture: one 360x800 pt window on a screen with a given scale      */
/* ------------------------------------------------------------------ */

static mock_obj_t g_scr;
static mock_obj_t g_layer;
static mock_obj_t g_win;
static mock_obj_t g_scene_wins;
static mock_obj_t g_legacy_wins;
static mock_obj_t g_scene;
static mock_obj_t g_scenes_arr;
static mock_obj_t g_scenes_set;
static mock_obj_t g_app;

static void build_fixture(double screen_scale) {
    memset(&g_scr, 0, sizeof(g_scr));
    g_scr.kind = KIND_SCREEN;
    g_scr.scale = screen_scale;
    memset(&g_layer, 0, sizeof(g_layer));
    g_layer.kind = KIND_LAYER;
    g_layer.tag = 1;
    g_layer.fill_pixel = 0xff112233u;
    memset(&g_win, 0, sizeof(g_win));
    g_win.kind = KIND_WINDOW;
    g_win.hidden = 0;
    g_win.alpha = 1.0;
    g_win.bounds.origin.x = 0;
    g_win.bounds.origin.y = 0;
    g_win.bounds.size.width = 360;
    g_win.bounds.size.height = 800;
    g_win.frame.origin.x = 0;
    g_win.frame.origin.y = 0;
    g_win.frame.size.width = 360;
    g_win.frame.size.height = 800;
    g_win.level = 0.0;
    g_win.screen = &g_scr;
    g_win.layer = &g_layer;
    g_win.content_scale = 0.0;
    memset(&g_scene_wins, 0, sizeof(g_scene_wins));
    g_scene_wins.kind = KIND_ARRAY;
    g_scene_wins.count = 1;
    g_scene_wins.items[0] = &g_win;
    memset(&g_legacy_wins, 0, sizeof(g_legacy_wins));
    g_legacy_wins.kind = KIND_ARRAY;
    g_legacy_wins.count = 0;
    memset(&g_scene, 0, sizeof(g_scene));
    g_scene.kind = KIND_SCENE;
    g_scene.items[0] = &g_scene_wins;
    memset(&g_scenes_arr, 0, sizeof(g_scenes_arr));
    g_scenes_arr.kind = KIND_ARRAY;
    g_scenes_arr.count = 1;
    g_scenes_arr.items[0] = &g_scene;
    memset(&g_scenes_set, 0, sizeof(g_scenes_set));
    g_scenes_set.kind = KIND_SET;
    g_scenes_set.items[0] = &g_scenes_arr;
    memset(&g_app, 0, sizeof(g_app));
    g_app.kind = KIND_APP;
    g_app.scenes_set = &g_scenes_set;
    g_app.legacy_windows = &g_legacy_wins;
    g_shared_app = &g_app;
    g_main_screen = &g_scr;
}

static void reset_all(void) {
    snapshot_test_reset();
    snapshot_test_configure(&g_ops);
    g_env_snapshot = "1";
    g_env_png = NULL;
    g_now_ns = 0;
    g_present_calls = 0;
    g_present_rc = 0;
    g_present_w = g_present_h = g_present_stride = 0;
    g_present_first_pixel = 0;
    g_shutdown_calls = 0;
    g_nonwhite_before_render = 0;
    memset(&g_info, 0, sizeof(g_info));
    reset_log();
}

static void set_info(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch,
                     float scale, uint32_t rotation) {
    g_info.screen_w = sw;
    g_info.screen_h = sh;
    g_info.client_w = cw;
    g_info.client_h = ch;
    g_info.scale = scale;
    g_info.offset_x = 0.0f;
    g_info.offset_y = 0.0f;
    g_info.rotation = rotation;
}

int main(void) {
    /* 1. UIScreen scale 1.0, INFO 1440x3200: INFO wins, 1440x3200. */
    reset_all();
    build_fixture(1.0);
    set_info(1440, 3200, 360, 800, 4.0f, 0);
    snapshot_test_tick();
    CHECK(g_present_calls == 1);
    CHECK(g_present_w == 1440 && g_present_h == 3200);
    CHECK(g_present_stride == 1440 * 4);
    CHECK(g_present_first_pixel == 0xff112233u);
    CHECK(g_nonwhite_before_render == 0);
    CHECK(strstr(g_log_buf, "[snapshot] scale from presenter INFO: 4000/1000") != NULL);
    CHECK(strstr(g_log_buf, "[snapshot] frame 1 1440x3200 windows=1 rc=0") != NULL);

    /* 2. No INFO, UIScreen scale 1.0: the frame stays 360x800. */
    reset_all();
    build_fixture(1.0);
    snapshot_test_tick();
    CHECK(g_present_calls == 1);
    CHECK(g_present_w == 360 && g_present_h == 800);
    CHECK(g_nonwhite_before_render == 0);
    CHECK(strstr(g_log_buf, "presenter INFO") == NULL);

    /* 3. UIScreen scale 4.0, no INFO: 1440x3200 with no INFO log. */
    reset_all();
    build_fixture(4.0);
    snapshot_test_tick();
    CHECK(g_present_calls == 1);
    CHECK(g_present_w == 1440 && g_present_h == 3200);
    CHECK(g_nonwhite_before_render == 0);
    CHECK(strstr(g_log_buf, "presenter INFO") == NULL);

    /* 4. Late INFO: first tick 360x800, second tick 1440x3200 with
     * exactly one session reset between them. */
    reset_all();
    build_fixture(1.0);
    snapshot_test_tick();
    CHECK(g_present_w == 360 && g_present_h == 800);
    CHECK(g_shutdown_calls == 0);
    set_info(1440, 3200, 360, 800, 4.0f, 0);
    reset_log();
    snapshot_test_tick();
    CHECK(g_present_calls == 2);
    CHECK(g_present_w == 1440 && g_present_h == 3200);
    CHECK(g_shutdown_calls == 1);
    CHECK(strstr(g_log_buf, "[snapshot] frame size changed, presenter session reset") != NULL);
    /* Third tick at the same size: no further reset. */
    snapshot_test_tick();
    CHECK(g_present_calls == 3);
    CHECK(g_shutdown_calls == 1);

    /* 5. A failed present records no size, so the size change after
     * it issues no shutdown. */
    reset_all();
    build_fixture(1.0);
    g_present_rc = -1;
    snapshot_test_tick();
    CHECK(g_present_w == 360 && g_present_h == 800);
    CHECK(g_shutdown_calls == 0);
    set_info(1440, 3200, 360, 800, 4.0f, 0);
    g_present_rc = 0;
    snapshot_test_tick();
    CHECK(g_present_w == 1440 && g_present_h == 3200);
    CHECK(g_shutdown_calls == 0);

    /* 6. Insane INFO scales are ignored, both directions. */
    reset_all();
    build_fixture(1.0);
    set_info(100000, 100000, 360, 800, 277.0f, 0);
    snapshot_test_tick();
    CHECK(g_present_w == 360 && g_present_h == 800);
    reset_all();
    build_fixture(1.0);
    set_info(72, 160, 360, 800, 0.2f, 0);
    snapshot_test_tick();
    CHECK(g_present_w == 360 && g_present_h == 800);

    /* 7. INFO at the low end of the sane range is honoured. */
    reset_all();
    build_fixture(1.0);
    set_info(90, 200, 360, 800, 0.25f, 0);
    snapshot_test_tick();
    CHECK(g_present_w == 90 && g_present_h == 200);

    /* 8. Landscape INFO (rotation 90, 3200x1440 window): aspect-fit
     * scale from INFO, frame follows the window. */
    reset_all();
    build_fixture(1.0);
    set_info(3200, 1440, 360, 800, 1.8f, 90);
    snapshot_test_tick();
    CHECK(g_present_w == 648 && g_present_h == 1440);
    CHECK(g_present_stride == 648 * 4);

    /* 9. Stale scale source (020): UIScreen scale 3.0, the value
     * MobileGestalt used to report before it was aligned to the BKS
     * fake, with INFO 1440x3200. The frame is 1440x3200, not
     * 1080x2400: INFO wins, and the log names the bounds and the
     * INFO screen size so the next device run shows which link
     * supplied the scale. */
    reset_all();
    build_fixture(3.0);
    set_info(1440, 3200, 360, 800, 4.0f, 0);
    snapshot_test_tick();
    CHECK(g_present_calls == 1);
    CHECK(g_present_w == 1440 && g_present_h == 3200);
    CHECK(strstr(g_log_buf, "[snapshot] scale from presenter INFO: 4000/1000 bounds=360x800 info=1440x3200 (UIScreen gave 3000/1000)") != NULL);

    snapshot_test_reset();
    printf("snapshot_geometry_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
