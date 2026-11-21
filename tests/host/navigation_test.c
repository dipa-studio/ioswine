#define SNAPSHOT_HOST_TEST 1

/* navigation_test.c
 *
 * Drives the snapshot pump through push / pop / menu cycles with a
 * mock UIKit that models the view-controller chain (UITabBarController
 * -> UINavigationController -> top view controller) and the commit rule:
 * a layer's content paints only once a [CATransaction flush] has run
 * AFTER the view was laid out. The pump flushes before it lays out, so
 * a view laid out by the window pass is uncommitted at render time unless
 * the navigation settle (snapshot.c) lays the new top view controller's
 * view out and flushes a second time.
 *
 * Mock faithfulness notes (where the mock simplifies real UIKit):
 * - Window layoutIfNeeded lays out the whole VC chain under the
 *   window, as real UIKit does (layout cascades to descendants).
 *   The settle's VC-view layout call is therefore belt-and-braces
 *   for layout; the settle's SECOND FLUSH is the part that makes
 *   transition-tick content appear, and that is what the push
 *   assertions measure.
 * - The window's layer paints the current top VC view's content
 *   (bg always, content rect only once committed), standing in for
 *   the real layer tree, which the pump renders via renderInContext.
 * - SwiftUI graph updates are NOT modelled: a UIHostingController
 *   view whose AttributeGraph never ran has no layers at all, which
 *   no pump-side flush can fix. This test covers the UIKit half only.
 *
 * RED check: the same test file built against the unpatched tree
 * fails the push assertions (no "[snapshot] nav top=" line, no
 * child content in the transition tick, no VC-view layout call),
 * because the unpatched pump has no settle pass.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"

/* ------------------------------------------------------------------ */
/* Test environment / logging / clock state                           */
/* ------------------------------------------------------------------ */

static const char *g_env_snapshot;
static uint64_t g_now_ns;
static char g_log_buf[16384];
static size_t g_log_len;
static int g_dlsym_calls;
static int g_checks;

#define CHECK(cond) do { g_checks++; assert(cond); } while (0)

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

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
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
    KIND_CLASS_CATXN,
    KIND_APP,
    KIND_SET,
    KIND_ARRAY,
    KIND_SCENE,
    KIND_WINDOW,
    KIND_SCREEN,
    KIND_LAYER,
    KIND_VC,
    KIND_VIEW,
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
    /* navigation */
    const char *vc_name;
    struct mock_obj *view;       /* VC -> its view; layer -> its view */
    struct mock_obj *top;        /* nav VC -> top VC */
    struct mock_obj *presented;  /* VC -> presented VC */
    struct mock_obj *selected;   /* tab VC -> selected VC */
    struct mock_obj *root_vc;    /* window -> root VC */
    struct mock_obj *win;        /* window layer -> its window */
    int laid_out;
    int committed;
    int layout_calls;
    uint32_t content_px;
    uint32_t bg_px;
    test_rect_t content_rect;    /* point rect the view paints when committed */
    int is_window_layer;
    uint32_t menu_px;
    test_rect_t menu_rect;
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
static mock_obj_t g_cls_catxn = { .kind = KIND_CLASS_CATXN };
static mock_obj_t *g_shared_app = NULL;
static mock_obj_t *g_main_screen = NULL;

static int g_dispatch_main_q_dummy;
static int g_dispatch_after_calls;
static uint64_t g_last_dispatch_when;
static void (*g_last_dispatch_cb)(void *);
static void *g_last_dispatch_ctx;

static int g_render_order[32];
static int g_render_count;

/* Call-order log. Codes: 1 = [CATransaction flush], 2 = [window
 * layoutIfNeeded], 3 = -[CALayer renderInContext:], 4 = [top VC
 * view layoutIfNeeded] (the settle's direct call). The fix
 * requires 1, then 2, then 4, then 1, then 3 on a transition tick. */
static int g_call_seq[64];
static int g_call_seq_n;
static int g_flush_calls;
static int g_layout_calls;
static int g_vc_layout_calls;

static void seq_push(int code) {
    if (g_call_seq_n < 64) g_call_seq[g_call_seq_n++] = code;
}

static int g_present_calls;
static int g_present_rc = 0;
static uint32_t g_present_w, g_present_h, g_present_stride;
/* Full copy of the last presented buffer, so tests can scan it for
 * painted pixels. */
static uint32_t *g_present_copy;
static size_t g_present_copy_bytes;

static int g_pool_push_calls;
static int g_pool_pop_calls;

/* Views the mock CATransaction flush commits. */
static mock_obj_t *g_views[16];
static int g_views_n;

static void reg_view(mock_obj_t *v) {
    if (g_views_n < 16) g_views[g_views_n++] = v;
}

int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    g_present_calls++;
    g_present_w = w;
    g_present_h = h;
    g_present_stride = stride;
    if (bgra && w > 0 && h > 0) {
        size_t bytes = (size_t)stride * h;
        free(g_present_copy);
        g_present_copy = malloc(bytes ? bytes : 1);
        g_present_copy_bytes = bytes;
        if (g_present_copy) memcpy(g_present_copy, bgra, bytes);
    }
    return g_present_rc;
}

static uint64_t mock_dispatch_time(uint64_t when, int64_t delta) {
    return when + (uint64_t)delta;
}

static void mock_dispatch_after_f(uint64_t when, void *q, void *ctx, void (*cb)(void *)) {
    assert(q == &g_dispatch_main_q_dummy);
    g_dispatch_after_calls++;
    g_last_dispatch_when = when;
    g_last_dispatch_ctx = ctx;
    g_last_dispatch_cb = cb;
}

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "UIApplication") == 0) return &g_cls_uiapp;
    if (strcmp(name, "UIScreen") == 0) return &g_cls_uiscreen;
    if (strcmp(name, "CATransaction") == 0) return &g_cls_catxn;
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    return (void *)name;
}

static int sel_eq(void *sel, const char *name) {
    return sel && strcmp((const char *)sel, name) == 0;
}

/* Walk a VC chain the way snapshot.c's snap_nav_top_vc does:
 * presented, then selected, then top, capped at 8 steps. */
static mock_obj_t *mock_top_vc(mock_obj_t *vc) {
    int steps;
    for (steps = 0; steps < 8 && vc; steps++) {
        mock_obj_t *next = NULL;
        if (vc->presented) next = vc->presented;
        else if (vc->selected) next = vc->selected;
        else if (vc->top) next = vc->top;
        if (!next) break;
        vc = next;
    }
    return vc;
}

static void mock_layout_chain(mock_obj_t *vc) {
    int steps;
    for (steps = 0; steps < 8 && vc; steps++) {
        if (vc->view) vc->view->laid_out = 1;
        {
            mock_obj_t *next = NULL;
            if (vc->presented) next = vc->presented;
            else if (vc->selected) next = vc->selected;
            else if (vc->top) next = vc->top;
            if (!next) break;
            vc = next;
        }
    }
}

static int mock_responds(void *obj, void *sel) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return 0;
    switch (o->kind) {
    case KIND_CLASS_UIAPP:
        return sel_eq(sel, "sharedApplication");
    case KIND_CLASS_UISCREEN:
        return sel_eq(sel, "mainScreen");
    case KIND_CLASS_CATXN:
        return sel_eq(sel, "flush");
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
               sel_eq(sel, "contentScaleFactor") || sel_eq(sel, "layer") ||
               sel_eq(sel, "layoutIfNeeded") || sel_eq(sel, "rootViewController");
    case KIND_SCREEN:
        return sel_eq(sel, "scale");
    case KIND_LAYER:
        return sel_eq(sel, "renderInContext:");
    case KIND_VC:
        return sel_eq(sel, "topViewController") ||
               sel_eq(sel, "presentedViewController") ||
               sel_eq(sel, "selectedViewController") || sel_eq(sel, "view");
    case KIND_VIEW:
        return sel_eq(sel, "layoutIfNeeded") || sel_eq(sel, "layer");
    default:
        return 0;
    }
}

static const char *mock_object_getClassName(void *obj) {
    mock_obj_t *o = (mock_obj_t *)obj;
    if (!o) return "(nil)";
    if (o->kind == KIND_VC) return o->vc_name ? o->vc_name : "(unknown)";
    if (o->kind == KIND_WINDOW) return "UIWindow";
    return "NSObject";
}

/* Paint one point rect into the mock bitmap context, mapping points
 * to pixels through the context CTM exactly as CoreGraphics would
 * (px = x*sx + tx, py = y*sy + ty). */
static void paint_rect_pt(mock_cg_ctx_t *ctx, double x0, double y0,
                          double x1, double y1, uint32_t pxv) {
    int ix0 = (int)(x0 * ctx->sx + ctx->tx);
    int iy0 = (int)(y0 * ctx->sy + ctx->ty);
    int ix1 = (int)(x1 * ctx->sx + ctx->tx);
    int iy1 = (int)(y1 * ctx->sy + ctx->ty);
    int xmin = ix0 < ix1 ? ix0 : ix1;
    int xmax = ix0 < ix1 ? ix1 : ix0;
    int ymin = iy0 < iy1 ? iy0 : iy1;
    int ymax = iy0 < iy1 ? iy1 : iy0;
    uint32_t *d;
    uint32_t row_px;
    int x, y;
    if (!ctx || !ctx->data) return;
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax > (int)ctx->w) xmax = (int)ctx->w;
    if (ymax > (int)ctx->h) ymax = (int)ctx->h;
    d = (uint32_t *)ctx->data;
    row_px = ctx->stride / 4;
    for (y = ymin; y < ymax; y++) {
        for (x = xmin; x < xmax; x++) {
            d[(size_t)y * row_px + (size_t)x] = pxv;
        }
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
    if (o->kind == KIND_CLASS_CATXN && sel_eq(sel, "flush")) {
        int i;
        g_flush_calls++;
        seq_push(1);
        for (i = 0; i < g_views_n; i++) {
            if (g_views[i]->laid_out) g_views[i]->committed = 1;
        }
        return NULL;
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
        if (sel_eq(sel, "rootViewController")) return o->root_vc;
        if (sel_eq(sel, "layoutIfNeeded")) {
            g_layout_calls++;
            seq_push(2);
            if (o->root_vc) mock_layout_chain(o->root_vc);
            return NULL;
        }
    }
    if (o->kind == KIND_VC) {
        if (sel_eq(sel, "topViewController")) return o->top;
        if (sel_eq(sel, "presentedViewController")) return o->presented;
        if (sel_eq(sel, "selectedViewController")) return o->selected;
        if (sel_eq(sel, "view")) return o->view;
    }
    if (o->kind == KIND_VIEW) {
        if (sel_eq(sel, "layoutIfNeeded")) {
            o->laid_out = 1;
            o->layout_calls++;
            g_vc_layout_calls++;
            seq_push(4);
            return NULL;
        }
        if (sel_eq(sel, "layer")) return o->layer;
    }
    if (o->kind == KIND_LAYER && sel_eq(sel, "renderInContext:")) {
        __builtin_va_list ap;
        __builtin_va_start(ap, sel);
        mock_cg_ctx_t *ctx = __builtin_va_arg(ap, mock_cg_ctx_t *);
        __builtin_va_end(ap);
        seq_push(3);
        if (g_render_count < 32) {
            g_render_order[g_render_count++] = o->tag;
        }
        if (!ctx || !ctx->data) return NULL;
        if (o->is_window_layer && o->win && o->win->root_vc) {
            /* Paint the current top VC view's content: bg always,
             * content rect only once the view is committed. */
            mock_obj_t *top = mock_top_vc(o->win->root_vc);
            mock_obj_t *view = top ? top->view : NULL;
            if (view) {
                paint_rect_pt(ctx, 0.0, 0.0, 360.0, 800.0, view->bg_px);
                if (view->committed) {
                    paint_rect_pt(ctx, view->content_rect.origin.x,
                                  view->content_rect.origin.y,
                                  view->content_rect.origin.x + view->content_rect.size.width,
                                  view->content_rect.origin.y + view->content_rect.size.height,
                                  view->content_px);
                }
            }
        } else if (o->menu_px) {
            /* Plain menu-window layer: paints its menu rect. */
            paint_rect_pt(ctx, o->menu_rect.origin.x, o->menu_rect.origin.y,
                          o->menu_rect.origin.x + o->menu_rect.size.width,
                          o->menu_rect.origin.y + o->menu_rect.size.height,
                          o->menu_px);
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
    g_pool_push_calls++;
    return (void *)(uintptr_t)0x1234;
}

static void mock_pool_pop(void *p) {
    assert(p == (void *)(uintptr_t)0x1234);
    g_pool_pop_calls++;
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
    g_dlsym_calls++;
    if (strcmp(name, "_dispatch_main_q") == 0) return &g_dispatch_main_q_dummy;
    if (strcmp(name, "dispatch_after_f") == 0) return (void *)mock_dispatch_after_f;
    if (strcmp(name, "dispatch_time") == 0) return (void *)mock_dispatch_time;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend_id;
    if (strcmp(name, "objc_msgSend_fpret") == 0) return (void *)mock_objc_msgSend_f64;
    if (strcmp(name, "objc_msgSend_stret") == 0) return (void *)mock_objc_msgSend_rect;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "object_getClassName") == 0) return (void *)mock_object_getClassName;
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

static void reset_all(void) {
    snapshot_test_reset();
    snapshot_test_configure(&g_ops);
    g_env_snapshot = NULL;
    g_now_ns = 0;
    g_dlsym_calls = 0;
    g_dispatch_after_calls = 0;
    g_last_dispatch_when = 0;
    g_last_dispatch_cb = NULL;
    g_last_dispatch_ctx = NULL;
    g_shared_app = NULL;
    g_main_screen = NULL;
    g_render_count = 0;
    g_call_seq_n = 0;
    g_flush_calls = 0;
    g_layout_calls = 0;
    g_vc_layout_calls = 0;
    g_present_calls = 0;
    g_present_rc = 0;
    g_present_w = g_present_h = g_present_stride = 0;
    free(g_present_copy);
    g_present_copy = NULL;
    g_present_copy_bytes = 0;
    g_pool_push_calls = 0;
    g_pool_pop_calls = 0;
    g_views_n = 0;
    reset_log();
}

/* Count exact-color pixels in a buffer rect. Buffer rows are
 * bottom-origin (row 0 = canvas y 0). */
static uint32_t count_color_in_rect(uint32_t color, int x0, int y0,
                                    int x1, int y1) {
    uint32_t n = 0;
    uint32_t row_px;
    int x, y;
    if (!g_present_copy || g_present_w == 0 || g_present_h == 0) return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_present_w) x1 = (int)g_present_w;
    if (y1 > (int)g_present_h) y1 = (int)g_present_h;
    row_px = g_present_stride / 4;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (g_present_copy[(size_t)y * row_px + (size_t)x] == color) n++;
        }
    }
    return n;
}

/* Same, for a UIKit point rect at the given scale and canvas height. */
static uint32_t count_color_in_pt_rect(uint32_t color, double px0,
                                       double py0, double px1, double py1,
                                       double scale, uint32_t h) {
    int x0 = (int)(px0 * scale);
    int x1 = (int)(px1 * scale);
    int y0 = (int)((double)h - py1 * scale);
    int y1 = (int)((double)h - py0 * scale);
    return count_color_in_rect(color, x0, y0, x1, y1);
}

#define PARENT_PX 0xFF0000FFu
#define CHILD_PX  0xFF00FF00u
#define MENU_PX   0xFFFF0000u
#define BG_PX     0xFFDDDDDDu

int main(void) {
    /* Shared scene: tab bar -> nav controller -> Library (parent) /
     * Settings (child). Main window 360x800 pt @ 2.0 -> 720x1600. */
    mock_obj_t scr = { .kind = KIND_SCREEN, .scale = 2.0 };
    mock_obj_t l_parent = { .kind = KIND_LAYER, .tag = 11 };
    mock_obj_t l_child = { .kind = KIND_LAYER, .tag = 12 };
    mock_obj_t v_parent = {
        .kind = KIND_VIEW, .layer = &l_parent, .bg_px = BG_PX,
        .content_px = PARENT_PX,
        .content_rect = { {19.0, 53.0}, {181.0, 32.0} },
    };
    mock_obj_t v_child = {
        .kind = KIND_VIEW, .layer = &l_child, .bg_px = BG_PX,
        .content_px = CHILD_PX,
        .content_rect = { {19.0, 53.0}, {201.0, 32.0} },
    };
    mock_obj_t vc_parent = { .kind = KIND_VC, .vc_name = "LibraryVC", .view = &v_parent };
    mock_obj_t vc_child = { .kind = KIND_VC, .vc_name = "SettingsVC", .view = &v_child };
    mock_obj_t vc_nav = { .kind = KIND_VC, .vc_name = "NavVC", .top = &vc_parent };
    mock_obj_t vc_tab = { .kind = KIND_VC, .vc_name = "TabBarVC", .selected = &vc_nav };
    mock_obj_t l_main = { .kind = KIND_LAYER, .tag = 10, .is_window_layer = 1 };
    mock_obj_t w_main = {
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
        .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
        .level = 0.0, .screen = &scr, .layer = &l_main, .root_vc = &vc_tab,
    };
    mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } };
    mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
    mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
    mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
    mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                       .legacy_windows = NULL };
    uint32_t n_parent, n_child;
    int vc_layout_before;
    int i;

    l_main.win = &w_main;
    l_parent.view = &v_parent;
    l_child.view = &v_child;

    /* 1. Push: the transition tick itself carries the new content. */
    reset_all();
    g_env_snapshot = "1";
    reg_view(&v_parent);
    reg_view(&v_child);
    g_shared_app = &app;
    g_present_rc = 0;

    /* Tick 1: first sight of the top VC logs it and settles it. */
    snapshot_init();
    snapshot_test_tick();
    CHECK(g_present_calls == 1);
    CHECK(g_present_w == 720 && g_present_h == 1600);
    CHECK(strstr(g_log_buf, "[snapshot] frame 1 720x1600 windows=1 rc=0") != NULL);
    CHECK(strstr(g_log_buf, "[snapshot] nav top=LibraryVC prev=(none)") != NULL);
    CHECK(g_vc_layout_calls >= 1);
    CHECK(g_flush_calls >= 2);
    n_parent = count_color_in_pt_rect(PARENT_PX, 19.0, 53.0, 200.0, 85.0, 2.0, 1600);
    CHECK(n_parent > 1000);
    /* Commit order on a transition tick: flush, window layout,
     * VC-view layout, settle flush, render. */
    CHECK(g_call_seq_n == 5);
    CHECK(g_call_seq[0] == 1 && g_call_seq[1] == 2 && g_call_seq[2] == 4 &&
          g_call_seq[3] == 1 && g_call_seq[4] == 3);

    /* Push the Settings child; the transition tick must already
     * show its content (no one-tick white flash). */
    reset_log();
    g_call_seq_n = 0;
    vc_nav.top = &vc_child;
    snapshot_test_tick();
    CHECK(g_present_calls == 2);
    CHECK(strstr(g_log_buf, "[snapshot] nav top=SettingsVC prev=LibraryVC") != NULL);
    CHECK(g_vc_layout_calls >= 2);
    n_child = count_color_in_pt_rect(CHILD_PX, 19.0, 53.0, 220.0, 85.0, 2.0, 1600);
    n_parent = count_color_in_pt_rect(PARENT_PX, 19.0, 53.0, 200.0, 85.0, 2.0, 1600);
    CHECK(n_child > 1000);
    CHECK(n_parent == 0);
    CHECK(g_call_seq_n == 5);
    CHECK(g_call_seq[0] == 1 && g_call_seq[1] == 2 && g_call_seq[2] == 4 &&
          g_call_seq[3] == 1 && g_call_seq[4] == 3);

    /* Tick 3: no top change, but the armed settle tick still lays
     * the top view out and flushes once more. */
    vc_layout_before = g_vc_layout_calls;
    snapshot_test_tick();
    CHECK(g_present_calls == 3);
    CHECK(g_vc_layout_calls > vc_layout_before);
    n_child = count_color_in_pt_rect(CHILD_PX, 19.0, 53.0, 220.0, 85.0, 2.0, 1600);
    CHECK(n_child > 1000);

    /* 2. Pop: the parent returns, the child layers are gone. */
    reset_log();
    vc_nav.top = &vc_parent;
    snapshot_test_tick();
    CHECK(g_present_calls == 4);
    CHECK(strstr(g_log_buf, "[snapshot] nav top=LibraryVC prev=SettingsVC") != NULL);
    n_parent = count_color_in_pt_rect(PARENT_PX, 19.0, 53.0, 200.0, 85.0, 2.0, 1600);
    n_child = count_color_in_pt_rect(CHILD_PX, 19.0, 53.0, 220.0, 85.0, 2.0, 1600);
    CHECK(n_parent > 1000);
    CHECK(n_child == 0);

    /* 3. Ten push/pop cycles: no stuck state, frames keep coming,
     * content follows the top every time. */
    reset_log();
    for (i = 0; i < 10; i++) {
        vc_nav.top = (i % 2 == 0) ? &vc_child : &vc_parent;
        snapshot_test_tick();
        CHECK(g_present_calls == 5 + i);
        n_child = count_color_in_pt_rect(CHILD_PX, 19.0, 53.0, 220.0, 85.0, 2.0, 1600);
        n_parent = count_color_in_pt_rect(PARENT_PX, 19.0, 53.0, 200.0, 85.0, 2.0, 1600);
        if (i % 2 == 0) {
            CHECK(n_child > 1000);
            CHECK(n_parent == 0);
        } else {
            CHECK(n_parent > 1000);
            CHECK(n_child == 0);
        }
    }
    CHECK(count_substr(g_log_buf, "[snapshot] nav top=") == 10);

    /* 4. Menu present/dismiss: the menu renders once (dedup across
     * scene.windows and app.windows), then is gone; a hidden or
     * zero-alpha menu window is skipped by the pump. */
    reset_all();
    g_env_snapshot = "1";
    reg_view(&v_parent);
    reg_view(&v_child);
    {
        mock_obj_t l_menu = { .kind = KIND_LAYER, .tag = 20,
                              .menu_px = MENU_PX,
                              .menu_rect = { {100.0, 300.0}, {200.0, 200.0} } };
        mock_obj_t w_menu = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 2000.0, .screen = &scr, .layer = &l_menu,
        };
        mock_obj_t wins2 = { .kind = KIND_ARRAY, .count = 2,
                             .items = { &w_main, &w_menu } };
        mock_obj_t legacy2 = { .kind = KIND_ARRAY, .count = 1,
                               .items = { &w_menu } }; /* dup: dedup check */
        mock_obj_t scene2 = { .kind = KIND_SCENE, .items = { &wins2 } };
        mock_obj_t scenes_arr2 = { .kind = KIND_ARRAY, .count = 1, .items = { &scene2 } };
        mock_obj_t scenes_set2 = { .kind = KIND_SET, .items = { &scenes_arr2 } };
        mock_obj_t app2 = { .kind = KIND_APP, .scenes_set = &scenes_set2,
                            .legacy_windows = &legacy2 };
        uint32_t n_menu;
        int menu_renders;
        int r;
        g_shared_app = &app2;
        g_present_rc = 0;
        vc_nav.top = &vc_parent;
        v_parent.laid_out = 0;
        v_parent.committed = 0;

        /* Present: menu renders exactly once, on top of the parent. */
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(strstr(g_log_buf, "[snapshot] frame 1 720x1600 windows=2 rc=0") != NULL);
        n_menu = count_color_in_pt_rect(MENU_PX, 100.0, 300.0, 300.0, 500.0, 2.0, 1600);
        CHECK(n_menu > 10000);
        menu_renders = 0;
        for (r = 0; r < g_render_count; r++) {
            if (g_render_order[r] == 20) menu_renders++;
        }
        CHECK(menu_renders == 1);
        n_parent = count_color_in_pt_rect(PARENT_PX, 19.0, 53.0, 200.0, 85.0, 2.0, 1600);
        CHECK(n_parent > 1000);

        /* Dismiss: the window leaves both arrays, the menu is gone. */
        reset_log();
        wins2.count = 1;
        legacy2.count = 0;
        snapshot_test_tick();
        CHECK(g_present_calls == 2);
        CHECK(strstr(g_log_buf, "[snapshot] frame 2 720x1600 windows=1 rc=0") != NULL);
        n_menu = count_color_in_pt_rect(MENU_PX, 100.0, 300.0, 300.0, 500.0, 2.0, 1600);
        CHECK(n_menu == 0);

        /* Hidden menu left in the array is skipped by the pump. */
        reset_log();
        w_menu.hidden = 1;
        wins2.count = 2;
        snapshot_test_tick();
        CHECK(g_present_calls == 3);
        CHECK(strstr(g_log_buf, "[snapshot] frame 3 720x1600 windows=1 rc=0") != NULL);
        n_menu = count_color_in_pt_rect(MENU_PX, 100.0, 300.0, 300.0, 500.0, 2.0, 1600);
        CHECK(n_menu == 0);

        /* Zero-alpha menu is skipped the same way. */
        reset_log();
        w_menu.hidden = 0;
        w_menu.alpha = 0.0;
        snapshot_test_tick();
        CHECK(g_present_calls == 4);
        CHECK(strstr(g_log_buf, "[snapshot] frame 4 720x1600 windows=1 rc=0") != NULL);
        n_menu = count_color_in_pt_rect(MENU_PX, 100.0, 300.0, 300.0, 500.0, 2.0, 1600);
        CHECK(n_menu == 0);
    }

    snapshot_test_reset();
    printf("navigation_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
