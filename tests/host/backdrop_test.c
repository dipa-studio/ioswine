/*
 * Host unit test for the 021 backdrop fix (snapshot.c).
 * Plain `cc`, no NDK, no Android headers:
 *   cc -std=c11 -Wall -Wextra -DSNAPSHOT_HOST_TEST=1 \
 *      -I <tree>/libsystem_kernel/src -I <tree>/android/surface/src/common \
 *      backdrop_test.c <tree>/libsystem_kernel/src/snapshot.c \
 *      -o backdrop_test && ./backdrop_test
 *
 * Mock Objective-C / CoreGraphics world, adapted from
 * tests/host/snapshot_test.c, extended with a layer tree: each mock
 * layer has sublayers, a frame, an is_backdrop flag (CABackdropLayer),
 * and a delegate/superview/effect/trait chain for the style lookup.
 * The mock backdrop layer simulates the device failure: when it
 * renders it paints deterministic grey streaks into its rect.
 * Scenarios (360x800 pt window @ 2.0 -> 720x1600 buffer):
 *   1. Plain mode, light style: tab-bar backdrop rect is the light
 *      fill, no streak pixels anywhere, glyph bars over it render
 *      with exact interiors and edges blended over the fill, the
 *      backdrop layer is unhidden after the tick.
 *   2. Plain mode, dark style (UIBlurEffectStyleDark): the rect is
 *      the dark fill.
 *   3. Plain mode, adaptive style + dark trait: the rect is the
 *      dark fill; with a light trait it is the light fill.
 *   4. Plain mode, IOSWINE_BACKDROP_FILL override: dark wins over a
 *      light style; a 6-hex override paints that exact colour.
 *   5. Sample mode: the blue/white edge above the backdrop rect is
 *      blurred inside the rect (intermediate pixels at the edge),
 *      stays sharp outside it, glyph interiors stay exact, no
 *      streaks, and the render sequence proves the two passes
 *      (beneath once, text once, backdrop never, root twice).
 *   6. No backdrop layer: the tick behaves exactly as before (no
 *      backdrop log line, present called, regression guard for the
 *      render-loop refactor).
 *   7. Already-hidden backdrop: not collected, no streaks, no
 *      backdrop log line.
 *   8. Two windows, each with a backdrop: both rects clean.
 * RED: scenarios 1-5 and 8 fail on the unpatched tree, where the
 * backdrop layer renders its streaks (streak pixels present, fill
 * absent, glyph edges blended over streaks, backdrop tag in the
 * render sequence). Scenario 5 additionally fails because no blur
 * is applied. Run the RED check by building this file against a
 * pristine snapshot.c.
 */
#define SNAPSHOT_HOST_TEST 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"

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
/* Test environment / logging / clock state                           */
/* ------------------------------------------------------------------ */

static const char *g_env_snapshot;
static const char *g_env_png;
static const char *g_env_backdrop_mode;
static const char *g_env_backdrop_fill;
static uint64_t g_now_ns;
static char g_log_buf[8192];
static size_t g_log_len;
static int g_dlsym_calls;

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
    if (strcmp(name, "IOSWINE_BACKDROP_MODE") == 0) return g_env_backdrop_mode;
    if (strcmp(name, "IOSWINE_BACKDROP_FILL") == 0) return g_env_backdrop_fill;
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
    KIND_CLASS_CATXN,
    KIND_APP,
    KIND_SET,
    KIND_ARRAY,
    KIND_SCENE,
    KIND_SCENE_NOWIN,
    KIND_WINDOW,
    KIND_SCREEN,
    KIND_LAYER,
    KIND_VIEW,
    KIND_EFFECT,
    KIND_TRAIT,
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
    /* Backdrop extensions. */
    struct mock_obj *sublayers_arr;
    int is_backdrop;
    /* Absolute point rect this layer paints, and how. */
    test_rect_t paint_rect;
    int has_paint_rect;
    uint32_t paint_px;
    int paint_streaks;
    int paint_edge; /* left half black, right half white, sharp edge */
    /* Glyph bars: 3 bars painted with a 1 pt blended edge. */
    int paint_glyphs;
    double glyph_bars[3][4];
    uint32_t glyph_px;
    /* Style chain: layer.delegate -> _UIBackdropView -> superviews
     * -> UIVisualEffectView (.effect, .traitCollection). */
    struct mock_obj *delegate;
    struct mock_obj *superview;
    struct mock_obj *effect;
    struct mock_obj *trait;
    int64_t style_val;
    int64_t ui_style_val;
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
static int g_cls_backdrop_dummy;
static mock_obj_t *g_shared_app = NULL;
static mock_obj_t *g_main_screen = NULL;

static int g_dispatch_main_q_dummy;
static int g_dispatch_after_calls;
static uint64_t g_last_dispatch_when;
static void (*g_last_dispatch_cb)(void *);
static void *g_last_dispatch_ctx;

/* Render sequence: tags of layers rendered, in order. */
static int g_render_seq[64];
static int g_render_seq_n;

static int g_present_calls;
static int g_present_rc = 0;
static uint32_t g_present_w, g_present_h, g_present_stride;
static uint32_t g_present_first_pixel;
static uint32_t *g_present_copy;
static size_t g_present_copy_bytes;

static int g_png_calls;
static char g_png_last_path[256];
static uint32_t g_last_bitmap_info;
static double g_last_tx, g_last_ty, g_last_sx, g_last_sy;
static int g_pool_push_calls;
static int g_pool_pop_calls;

int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    g_present_calls++;
    g_present_w = w;
    g_present_h = h;
    g_present_stride = stride;
    if (bgra && w > 0 && h > 0) {
        memcpy(&g_present_first_pixel, bgra, sizeof(uint32_t));
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
    (void)q;
    g_dispatch_after_calls++;
    g_last_dispatch_when = when;
    g_last_dispatch_ctx = ctx;
    g_last_dispatch_cb = cb;
}

static void *mock_objc_getClass(const char *name) {
    if (strcmp(name, "UIApplication") == 0) return &g_cls_uiapp;
    if (strcmp(name, "UIScreen") == 0) return &g_cls_uiscreen;
    if (strcmp(name, "CATransaction") == 0) return &g_cls_catxn;
    if (strcmp(name, "CABackdropLayer") == 0) return &g_cls_backdrop_dummy;
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
    case KIND_SCENE_NOWIN:
        return 0;
    case KIND_WINDOW:
        return sel_eq(sel, "isHidden") || sel_eq(sel, "alpha") ||
               sel_eq(sel, "bounds") || sel_eq(sel, "frame") ||
               sel_eq(sel, "windowLevel") || sel_eq(sel, "screen") ||
               sel_eq(sel, "contentScaleFactor") || sel_eq(sel, "layer") ||
               sel_eq(sel, "layoutIfNeeded");
    case KIND_SCREEN:
        return sel_eq(sel, "scale");
    case KIND_LAYER:
        if (sel_eq(sel, "sublayers")) return o->sublayers_arr != NULL;
        if (sel_eq(sel, "delegate")) return o->delegate != NULL;
        return sel_eq(sel, "renderInContext:") || sel_eq(sel, "frame") ||
               sel_eq(sel, "isKindOfClass:") || sel_eq(sel, "setHidden:") ||
               sel_eq(sel, "isHidden");
    case KIND_VIEW:
        if (sel_eq(sel, "superview")) return o->superview != NULL;
        if (sel_eq(sel, "effect")) return o->effect != NULL;
        if (sel_eq(sel, "traitCollection")) return o->trait != NULL;
        return 0;
    case KIND_EFFECT:
        return sel_eq(sel, "style");
    case KIND_TRAIT:
        return sel_eq(sel, "userInterfaceStyle");
    default:
        return 0;
    }
}

/* Paint one point rect into the mock bitmap context, mapping points
 * to pixels through the context CTM (px = x*sx + tx, py = y*sy +
 * ty). Colors are BGRA uint32 LE with alpha 255. */
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

/* Device-failure simulation: the backdrop layer paints deterministic
 * grey streaks (vertical bands alternating 0xFF808080 / 0xFF3A3A3A
 * every 2 px) into its rect. */
static void paint_streaks_pt(mock_cg_ctx_t *ctx, double x0, double y0,
                             double x1, double y1) {
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
            d[(size_t)y * row_px + (size_t)x] =
                ((x / 2) % 2 == 0) ? 0xFF808080u : 0xFF3A3A3Au;
        }
    }
}

/* Blend one point rect 50/50 with a colour over the pixels already
 * in the buffer: models antialiased text edges picking up whatever
 * is beneath them. Each pixel is blended exactly once. */
static void paint_blend_pt(mock_cg_ctx_t *ctx, double x0, double y0,
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
            uint32_t cur = d[(size_t)y * row_px + (size_t)x];
            uint32_t out = 0xFF000000u |
                ((((pxv >> 16) & 0xFF) + ((cur >> 16) & 0xFF)) / 2 << 16) |
                ((((pxv >> 8) & 0xFF) + ((cur >> 8) & 0xFF)) / 2 << 8) |
                (((pxv & 0xFF) + (cur & 0xFF)) / 2);
            d[(size_t)y * row_px + (size_t)x] = out;
        }
    }
}

/* Glyph bars with a 1 pt blended edge: interior opaque, border ring
 * blended 50/50 over the current pixels. The four border strips do
 * not overlap, so no pixel is blended twice. */
static void paint_glyph_bars(mock_cg_ctx_t *ctx, mock_obj_t *o) {
    int i;
    for (i = 0; i < 3; i++) {
        double x0 = o->glyph_bars[i][0], y0 = o->glyph_bars[i][1];
        double x1 = o->glyph_bars[i][2], y1 = o->glyph_bars[i][3];
        paint_rect_pt(ctx, x0 + 1.0, y0 + 1.0, x1 - 1.0, y1 - 1.0, o->glyph_px);
        paint_blend_pt(ctx, x0, y0, x1, y0 + 1.0, o->glyph_px);
        paint_blend_pt(ctx, x0, y1 - 1.0, x1, y1, o->glyph_px);
        paint_blend_pt(ctx, x0, y0 + 1.0, x0 + 1.0, y1 - 1.0, o->glyph_px);
        paint_blend_pt(ctx, x1 - 1.0, y0 + 1.0, x1, y1 - 1.0, o->glyph_px);
    }
}

static void mock_render_layer(mock_obj_t *o, mock_cg_ctx_t *ctx) {
    uint64_t i;
    if (!o || o->hidden) return;
    if (g_render_seq_n < 64) g_render_seq[g_render_seq_n++] = o->tag;
    if (o->has_paint_rect) {
        if (o->paint_streaks) {
            paint_streaks_pt(ctx, o->paint_rect.origin.x, o->paint_rect.origin.y,
                             o->paint_rect.origin.x + o->paint_rect.size.width,
                             o->paint_rect.origin.y + o->paint_rect.size.height);
        } else if (o->paint_edge) {
            double mx = o->paint_rect.origin.x + o->paint_rect.size.width / 2.0;
            paint_rect_pt(ctx, o->paint_rect.origin.x, o->paint_rect.origin.y,
                          mx, o->paint_rect.origin.y + o->paint_rect.size.height,
                          0xFF000000u);
            paint_rect_pt(ctx, mx, o->paint_rect.origin.y,
                          o->paint_rect.origin.x + o->paint_rect.size.width,
                          o->paint_rect.origin.y + o->paint_rect.size.height,
                          0xFFFFFFFFu);
        } else {
            paint_rect_pt(ctx, o->paint_rect.origin.x, o->paint_rect.origin.y,
                          o->paint_rect.origin.x + o->paint_rect.size.width,
                          o->paint_rect.origin.y + o->paint_rect.size.height,
                          o->paint_px);
        }
    }
    if (o->paint_glyphs) paint_glyph_bars(ctx, o);
    if (o->sublayers_arr) {
        for (i = 0; i < o->sublayers_arr->count; i++) {
            mock_render_layer((mock_obj_t *)o->sublayers_arr->items[i], ctx);
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
        if (sel_eq(sel, "layoutIfNeeded")) return NULL;
    }
    if (o->kind == KIND_LAYER) {
        if (sel_eq(sel, "isHidden")) return (void *)(uintptr_t)o->hidden;
        if (sel_eq(sel, "sublayers")) return o->sublayers_arr;
        if (sel_eq(sel, "delegate")) return o->delegate;
        if (sel_eq(sel, "isKindOfClass:")) {
            __builtin_va_list ap;
            __builtin_va_start(ap, sel);
            void *cls = __builtin_va_arg(ap, void *);
            __builtin_va_end(ap);
            return (void *)(uintptr_t)(cls == &g_cls_backdrop_dummy && o->is_backdrop);
        }
        if (sel_eq(sel, "setHidden:")) {
            __builtin_va_list ap;
            __builtin_va_start(ap, sel);
            int v = __builtin_va_arg(ap, int);
            __builtin_va_end(ap);
            o->hidden = v ? 1 : 0;
            return NULL;
        }
        if (sel_eq(sel, "renderInContext:")) {
            __builtin_va_list ap;
            __builtin_va_start(ap, sel);
            mock_cg_ctx_t *ctx = __builtin_va_arg(ap, mock_cg_ctx_t *);
            __builtin_va_end(ap);
            mock_render_layer(o, ctx);
            return NULL;
        }
    }
    if (o->kind == KIND_VIEW) {
        if (sel_eq(sel, "superview")) return o->superview;
        if (sel_eq(sel, "effect")) return o->effect;
        if (sel_eq(sel, "traitCollection")) return o->trait;
    }
    if (o->kind == KIND_EFFECT && sel_eq(sel, "style")) {
        return (void *)(uintptr_t)o->style_val;
    }
    if (o->kind == KIND_TRAIT && sel_eq(sel, "userInterfaceStyle")) {
        return (void *)(uintptr_t)o->ui_style_val;
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
    if (!o) return z;
    if (o->kind == KIND_WINDOW) {
        if (sel_eq(sel, "bounds")) return o->bounds;
        if (sel_eq(sel, "frame")) return o->frame;
    }
    if (o->kind == KIND_LAYER) {
        if (sel_eq(sel, "frame")) return o->frame;
    }
    return z;
}

static void *mock_pool_push(void) {
    g_pool_push_calls++;
    return (void *)(uintptr_t)0x1234;
}

static void mock_pool_pop(void *p) {
    (void)p;
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
    g_last_bitmap_info = bitmapInfo;
    return ctx;
}

static void mock_cg_ctx_release(void *c) { free(c); }

static void mock_cg_translate(void *c, double tx, double ty) {
    mock_cg_ctx_t *ctx = (mock_cg_ctx_t *)c;
    if (ctx->save_depth == 0) {
        ctx->tx = tx;
        ctx->ty = ty;
        g_last_tx = tx;
        g_last_ty = ty;
    }
}

static void mock_cg_scale(void *c, double sx, double sy) {
    mock_cg_ctx_t *ctx = (mock_cg_ctx_t *)c;
    ctx->sx = sx;
    ctx->sy = sy;
    g_last_sx = sx;
    g_last_sy = sy;
}

static void mock_cg_save(void *c) { ((mock_cg_ctx_t *)c)->save_depth++; }
static void mock_cg_restore(void *c) { ((mock_cg_ctx_t *)c)->save_depth--; }

static void *mock_cg_create_image(void *c) {
    (void)c;
    static int img_dummy;
    return &img_dummy;
}

static void mock_cg_image_release(void *i) { (void)i; }

static void *mock_cf_url_create(void *alloc, const uint8_t *buf, long len, uint8_t isDir) {
    (void)alloc;
    (void)isDir;
    char *s = malloc((size_t)len + 1);
    memcpy(s, buf, (size_t)len);
    s[len] = '\0';
    return s;
}

static void *mock_cf_str_create(void *alloc, const char *cstr, uint32_t enc) {
    (void)alloc;
    (void)enc;
    char *s = malloc(strlen(cstr) + 1);
    strcpy(s, cstr);
    return s;
}

static void mock_cf_release(void *p) { free(p); }

static void *mock_img_dest_create(void *url, void *type, size_t count, void *opts) {
    (void)count;
    (void)opts;
    snprintf(g_png_last_path, sizeof(g_png_last_path), "%s", (const char *)url);
    return malloc(8);
}

static void mock_img_dest_add(void *idst, void *img, void *props) {
    (void)idst;
    (void)img;
    (void)props;
}

static uint8_t mock_img_dest_finalize(void *idst) {
    (void)idst;
    g_png_calls++;
    return 1;
}

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
    if (strcmp(name, "CFURLCreateFromFileSystemRepresentation") == 0) return (void *)mock_cf_url_create;
    if (strcmp(name, "CFStringCreateWithCString") == 0) return (void *)mock_cf_str_create;
    if (strcmp(name, "CFRelease") == 0) return (void *)mock_cf_release;
    if (strcmp(name, "CGImageDestinationCreateWithURL") == 0) return (void *)mock_img_dest_create;
    if (strcmp(name, "CGImageDestinationAddImage") == 0) return (void *)mock_img_dest_add;
    if (strcmp(name, "CGImageDestinationFinalize") == 0) return (void *)mock_img_dest_finalize;
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
    g_env_png = NULL;
    g_env_backdrop_mode = NULL;
    g_env_backdrop_fill = NULL;
    g_now_ns = 0;
    g_dlsym_calls = 0;
    g_dispatch_after_calls = 0;
    g_last_dispatch_when = 0;
    g_last_dispatch_cb = NULL;
    g_last_dispatch_ctx = NULL;
    g_shared_app = NULL;
    g_main_screen = NULL;
    g_render_seq_n = 0;
    g_present_calls = 0;
    g_present_rc = 0;
    g_present_w = g_present_h = g_present_stride = 0;
    g_present_first_pixel = 0;
    free(g_present_copy);
    g_present_copy = NULL;
    g_present_copy_bytes = 0;
    g_png_calls = 0;
    g_png_last_path[0] = '\0';
    g_last_bitmap_info = 0;
    g_last_tx = g_last_ty = g_last_sx = g_last_sy = 0.0;
    g_pool_push_calls = 0;
    g_pool_pop_calls = 0;
    reset_log();
}

/* Count exact-colour pixels in a buffer rect. Buffer rows are
 * bottom-origin (row 0 = canvas y 0); the presented copy is a
 * verbatim copy of the snapshot canvas. */
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

/* Same, for a UIKit point rect at the given scale and canvas height:
 * buffer col = pt.x * scale, buffer row = h - pt.y * scale. */
static uint32_t count_color_in_pt_rect(uint32_t color, double px0,
                                       double py0, double px1, double py1,
                                       double scale, uint32_t h) {
    int x0 = (int)(px0 * scale);
    int x1 = (int)(px1 * scale);
    int y0 = (int)((double)h - py1 * scale);
    int y1 = (int)((double)h - py0 * scale);
    return count_color_in_rect(color, x0, y0, x1, y1);
}

/* Count pixels whose red channel is strictly between lo and hi in a
 * point rect: the blur-gradient probe. Streak colours are excluded:
 * the mock backdrop's grey streaks are themselves mid-red, so
 * counting them would make the probe pass on the unpatched tree. */
static uint32_t count_mid_red_in_pt_rect(int lo, int hi, double px0,
                                         double py0, double px1, double py1,
                                         double scale, uint32_t h) {
    int x0 = (int)(px0 * scale);
    int x1 = (int)(px1 * scale);
    int y0 = (int)((double)h - py1 * scale);
    int y1 = (int)((double)h - py0 * scale);
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
            uint32_t px = g_present_copy[(size_t)y * row_px + (size_t)x];
            int r = (int)((px >> 16) & 0xFF);
            if (px == 0xFF808080u || px == 0xFF3A3A3Au) continue;
            if (r > lo && r < hi) n++;
        }
    }
    return n;
}

static int count_tag_in_seq(int tag) {
    int n = 0;
    int i;
    for (i = 0; i < g_render_seq_n; i++) {
        if (g_render_seq[i] == tag) n++;
    }
    return n;
}

/* 50/50 blend of two BGRA colours, channel-wise (alpha forced 255):
 * the expected colour of an antialiased glyph edge over a fill. */
static uint32_t blend50(uint32_t a, uint32_t b) {
    return 0xFF000000u |
        ((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 2 << 16) |
        ((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) / 2 << 8) |
        (((a & 0xFF) + (b & 0xFF)) / 2);
}

/* Colours (BGRA uint32 LE). */
#define PX_WHITE     0xFFFFFFFFu
#define PX_BLUE      0xFF112233u
#define PX_FILL_L    0xFFF2F2F7u
#define PX_FILL_D    0xFF1C1C1Eu
#define PX_STREAK_A  0xFF808080u
#define PX_STREAK_B  0xFF3A3A3Au
#define PX_GLYPH     0xFF000000u

/* Glyph bars inside the tab-bar rect (pt): 3 bars, 20x16 pt. */
static const double g_bars[3][4] = {
    { 40.0, 740.0, 60.0, 756.0 },
    { 80.0, 740.0, 100.0, 756.0 },
    { 120.0, 740.0, 140.0, 756.0 },
};

/* ------------------------------------------------------------------ */
/* Scenarios                                                          */
/* ------------------------------------------------------------------ */

/* Build the standard one-window scene: root layer with sublayers
 * [beneath, backdrop, text]. The beneath layer paints solid blue
 * y 0..720 only, so the tab-bar rect y 720..800 is transparent and
 * the fill/blur shows there. The backdrop layer is a CABackdropLayer
 * over the tab bar rect; the text layer paints 3 glyph bars in it. */
#define BUILD_SCENE(style_val_, ui_style_val_) \
    mock_obj_t scr = { .kind = KIND_SCREEN, .scale = 2.0 }; \
    mock_obj_t fx = { .kind = KIND_EFFECT, .style_val = (style_val_) }; \
    mock_obj_t trait = { .kind = KIND_TRAIT, .ui_style_val = (ui_style_val_) }; \
    mock_obj_t fxview = { .kind = KIND_VIEW, .effect = &fx, .trait = &trait }; \
    mock_obj_t container = { .kind = KIND_VIEW, .superview = &fxview }; \
    mock_obj_t bdview = { .kind = KIND_VIEW, .superview = &container }; \
    mock_obj_t l_beneath = { .kind = KIND_LAYER, .tag = 10, \
        .frame = { {0, 0}, {360, 800} }, \
        .has_paint_rect = 1, \
        .paint_rect = { {0, 0}, {360, 720} }, \
        .paint_px = PX_BLUE }; \
    mock_obj_t l_backdrop = { .kind = KIND_LAYER, .tag = 20, .is_backdrop = 1, \
        .frame = { {0, 720}, {360, 80} }, \
        .has_paint_rect = 1, \
        .paint_rect = { {0, 720}, {360, 80} }, \
        .paint_streaks = 1, .delegate = &bdview }; \
    mock_obj_t l_text = { .kind = KIND_LAYER, .tag = 30, \
        .frame = { {0, 720}, {360, 80} }, \
        .paint_glyphs = 1, .glyph_px = PX_GLYPH }; \
    mock_obj_t arr_subs = { .kind = KIND_ARRAY, .count = 3, \
        .items = { &l_beneath, &l_backdrop, &l_text } }; \
    mock_obj_t l_root = { .kind = KIND_LAYER, .tag = 1, \
        .frame = { {0, 0}, {360, 800} }, .sublayers_arr = &arr_subs }; \
    mock_obj_t w_main = { \
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0, \
        .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} }, \
        .level = 0.0, .screen = &scr, .layer = &l_root, \
    }; \
    mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } }; \
    mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } }; \
    mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } }; \
    mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } }; \
    mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set, \
                       .legacy_windows = NULL }; \
    memcpy(l_text.glyph_bars, g_bars, sizeof(g_bars)); \
    g_shared_app = &app

/* Probe strip inside the tab-bar rect with no glyphs. */
#define PROBE_X0 200.0
#define PROBE_X1 360.0
#define PROBE_Y0 720.0
#define PROBE_Y1 800.0
/* Probe strip area in pixels @ 2.0: 320 x 160 = 51200. */
#define PROBE_AREA 51200u

static uint32_t streaks_in_probe(void) {
    return count_color_in_pt_rect(PX_STREAK_A, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) +
           count_color_in_pt_rect(PX_STREAK_B, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600);
}

static void check_glyphs_exact(void) {
    int i;
    uint32_t n_interior = 0, n_edge = 0;
    for (i = 0; i < 3; i++) {
        n_interior += count_color_in_pt_rect(PX_GLYPH, g_bars[i][0] + 1.0, g_bars[i][1] + 1.0,
                                             g_bars[i][2] - 1.0, g_bars[i][3] - 1.0, 2.0, 1600);
    }
    CHECK(n_interior == 3024u);
    (void)n_edge;
}

static void check_glyph_edges(uint32_t fill) {
    uint32_t edge = blend50(PX_GLYPH, fill);
    uint32_t n_edge = 0;
    int i;
    for (i = 0; i < 3; i++) {
        n_edge += count_color_in_pt_rect(edge, g_bars[i][0], g_bars[i][1],
                                         g_bars[i][2], g_bars[i][3], 2.0, 1600);
    }
    /* The ring is 272 px per bar (40x32 minus 36x28 interior). */
    CHECK(n_edge == 816u);
}

int main(void) {
    /* 1. Plain mode, light style (UIBlurEffectStyleLight = 1). */
    reset_all();
    g_env_snapshot = "1";
    {
        BUILD_SCENE(1, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(g_present_w == 720 && g_present_h == 1600);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop 1 mode=plain") != NULL);
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_L, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
        check_glyphs_exact();
        check_glyph_edges(PX_FILL_L);
        CHECK(l_backdrop.hidden == 0);
        CHECK(count_tag_in_seq(20) == 0);
        CHECK(g_render_seq_n == 3);
        CHECK(g_render_seq[0] == 1 && g_render_seq[1] == 10 && g_render_seq[2] == 30);
    }

    /* 2. Plain mode, dark style (UIBlurEffectStyleDark = 2). */
    reset_all();
    g_env_snapshot = "1";
    {
        BUILD_SCENE(2, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop 1 mode=plain") != NULL);
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_D, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
        CHECK(count_color_in_pt_rect(PX_FILL_L, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == 0);
        check_glyphs_exact();
        check_glyph_edges(PX_FILL_D);
        CHECK(l_backdrop.hidden == 0);
    }

    /* 3a. Adaptive style (regular = 4) + dark trait -> dark fill. */
    reset_all();
    g_env_snapshot = "1";
    {
        BUILD_SCENE(4, 2);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_D, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
        check_glyphs_exact();
    }

    /* 3b. Adaptive style (regular = 4) + light trait -> light fill. */
    reset_all();
    g_env_snapshot = "1";
    {
        BUILD_SCENE(4, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_L, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
    }

    /* 4a. IOSWINE_BACKDROP_FILL=dark wins over a light style. */
    reset_all();
    g_env_snapshot = "1";
    g_env_backdrop_fill = "dark";
    {
        BUILD_SCENE(1, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_D, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
    }

    /* 4b. IOSWINE_BACKDROP_FILL=FF0000 paints that exact colour. */
    reset_all();
    g_env_snapshot = "1";
    g_env_backdrop_fill = "FF0000";
    {
        BUILD_SCENE(1, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(0xFFFF0000u, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
    }

    /* 5. Sample mode: blur the blue/white edge inside the rect,
     * keep it sharp outside, glyphs exact, no streaks, two passes. */
    reset_all();
    g_env_snapshot = "1";
    g_env_backdrop_mode = "sample";
    {
        BUILD_SCENE(1, 1);
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop 1 mode=sample") != NULL);
        CHECK(streaks_in_probe() == 0);
        CHECK(count_tag_in_seq(20) == 0);
        CHECK(count_tag_in_seq(10) == 1);
        CHECK(count_tag_in_seq(30) == 1);
        CHECK(count_tag_in_seq(1) == 2);
        CHECK(g_render_seq_n == 4);
        CHECK(g_render_seq[0] == 1 && g_render_seq[1] == 10 &&
              g_render_seq[2] == 1 && g_render_seq[3] == 30);
        check_glyphs_exact();
        /* Gradient at the edge inside the rect (rows y 718..726). */
        CHECK(count_mid_red_in_pt_rect(0x11, 0xFF, 90.0, 718.0, 110.0, 726.0, 2.0, 1600) > 100u);
        /* Sharp above the rect (y 700..710): all blue. */
        CHECK(count_color_in_pt_rect(PX_BLUE, 90.0, 700.0, 110.0, 710.0, 2.0, 1600) == 800u);
        /* Pure white deep in the rect (y 760..770). */
        CHECK(count_color_in_pt_rect(PX_WHITE, 90.0, 760.0, 110.0, 770.0, 2.0, 1600) == 800u);
        CHECK(l_backdrop.hidden == 0);
    }

    /* 6. No backdrop layer: tick behaves as before. */
    reset_all();
    g_env_snapshot = "1";
    {
        mock_obj_t scr = { .kind = KIND_SCREEN, .scale = 2.0 };
        mock_obj_t l_beneath = { .kind = KIND_LAYER, .tag = 10,
            .frame = { {0, 0}, {360, 800} },
            .has_paint_rect = 1,
            .paint_rect = { {0, 0}, {360, 720} },
            .paint_px = PX_BLUE };
        mock_obj_t arr_subs = { .kind = KIND_ARRAY, .count = 1,
            .items = { &l_beneath } };
        mock_obj_t l_root = { .kind = KIND_LAYER, .tag = 1,
            .frame = { {0, 0}, {360, 800} }, .sublayers_arr = &arr_subs };
        mock_obj_t w_main = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 0.0, .screen = &scr, .layer = &l_root,
        };
        mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } };
        mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
        mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
        mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
        mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                           .legacy_windows = NULL };
        g_shared_app = &app;
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(g_present_w == 720 && g_present_h == 1600);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop") == NULL);
        CHECK(count_color_in_pt_rect(PX_BLUE, 90.0, 700.0, 110.0, 710.0, 2.0, 1600) == 800u);
    }

    /* 7. Already-hidden backdrop: not collected, no streaks. */
    reset_all();
    g_env_snapshot = "1";
    {
        BUILD_SCENE(1, 1);
        l_backdrop.hidden = 1;
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop") == NULL);
        CHECK(streaks_in_probe() == 0);
        CHECK(l_backdrop.hidden == 1);
    }

    /* 8. Two windows, each with a backdrop: both rects clean. */
    reset_all();
    g_env_snapshot = "1";
    {
        mock_obj_t scr = { .kind = KIND_SCREEN, .scale = 2.0 };
        mock_obj_t fx = { .kind = KIND_EFFECT, .style_val = 1 };
        mock_obj_t trait = { .kind = KIND_TRAIT, .ui_style_val = 1 };
        mock_obj_t fxview = { .kind = KIND_VIEW, .effect = &fx, .trait = &trait };
        mock_obj_t container = { .kind = KIND_VIEW, .superview = &fxview };
        mock_obj_t bdview = { .kind = KIND_VIEW, .superview = &container };
        mock_obj_t l_beneath = { .kind = KIND_LAYER, .tag = 10,
            .frame = { {0, 0}, {360, 800} },
            .has_paint_rect = 1,
            /* Paints y 180..720 only: the second window's backdrop
             * rect y 100..180 must stay transparent in the lower
             * window, or its fill would be covered by opaque blue. */
            .paint_rect = { {0, 180}, {360, 540} },
            .paint_px = PX_BLUE };
        mock_obj_t l_backdrop = { .kind = KIND_LAYER, .tag = 20, .is_backdrop = 1,
            .frame = { {0, 720}, {360, 80} },
            .has_paint_rect = 1,
            .paint_rect = { {0, 720}, {360, 80} },
            .paint_streaks = 1, .delegate = &bdview };
        mock_obj_t l_text = { .kind = KIND_LAYER, .tag = 30,
            .frame = { {0, 720}, {360, 80} },
            .paint_glyphs = 1, .glyph_px = PX_GLYPH };
        mock_obj_t arr_subs = { .kind = KIND_ARRAY, .count = 3,
            .items = { &l_beneath, &l_backdrop, &l_text } };
        mock_obj_t l_root = { .kind = KIND_LAYER, .tag = 1,
            .frame = { {0, 0}, {360, 800} }, .sublayers_arr = &arr_subs };
        mock_obj_t w_main = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 0.0, .screen = &scr, .layer = &l_root,
        };
        /* Second window (level 10): green content y 100..140, a
         * top backdrop rect y 100..180, glyphs y 120..136. */
        mock_obj_t l_beneath2 = { .kind = KIND_LAYER, .tag = 11,
            .frame = { {0, 0}, {360, 800} },
            .has_paint_rect = 1,
            .paint_rect = { {0, 100}, {360, 40} },
            .paint_px = 0xFF00FF00u };
        mock_obj_t l_backdrop2 = { .kind = KIND_LAYER, .tag = 21, .is_backdrop = 1,
            .frame = { {0, 100}, {360, 80} },
            .has_paint_rect = 1,
            .paint_rect = { {0, 100}, {360, 80} },
            .paint_streaks = 1, .delegate = &bdview };
        mock_obj_t l_text2 = { .kind = KIND_LAYER, .tag = 31,
            .frame = { {0, 100}, {360, 80} },
            .paint_glyphs = 1, .glyph_px = PX_GLYPH };
        mock_obj_t arr_subs2 = { .kind = KIND_ARRAY, .count = 3,
            .items = { &l_beneath2, &l_backdrop2, &l_text2 } };
        mock_obj_t l_root2 = { .kind = KIND_LAYER, .tag = 2,
            .frame = { {0, 0}, {360, 800} }, .sublayers_arr = &arr_subs2 };
        mock_obj_t w_top = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 10.0, .screen = &scr, .layer = &l_root2,
        };
        mock_obj_t wins = { .kind = KIND_ARRAY, .count = 2, .items = { &w_main, &w_top } };
        mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
        mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
        mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
        mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                           .legacy_windows = NULL };
        static const double bars2[3][4] = {
            { 40.0, 120.0, 60.0, 136.0 },
            { 80.0, 120.0, 100.0, 136.0 },
            { 120.0, 120.0, 140.0, 136.0 },
        };
        memcpy(l_text.glyph_bars, g_bars, sizeof(g_bars));
        memcpy(l_text2.glyph_bars, bars2, sizeof(bars2));
        g_shared_app = &app;
        g_present_rc = 0;
        snapshot_init();
        snapshot_test_tick();
        CHECK(g_present_calls == 1);
        CHECK(strstr(g_log_buf, "[snapshot] backdrop 2 mode=plain") != NULL);
        CHECK(streaks_in_probe() == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_L, PROBE_X0, PROBE_Y0, PROBE_X1, PROBE_Y1, 2.0, 1600) == PROBE_AREA);
        /* Second backdrop: probe strip x 200..360, y 150..170 is
         * fill (beneath2 only paints y 100..140 there). */
        CHECK(count_color_in_pt_rect(PX_STREAK_A, 200.0, 150.0, 360.0, 170.0, 2.0, 1600) == 0);
        CHECK(count_color_in_pt_rect(PX_STREAK_B, 200.0, 150.0, 360.0, 170.0, 2.0, 1600) == 0);
        CHECK(count_color_in_pt_rect(PX_FILL_L, 200.0, 150.0, 360.0, 170.0, 2.0, 1600) == 12800u);
        check_glyphs_exact();
        CHECK(l_backdrop.hidden == 0);
        CHECK(l_backdrop2.hidden == 0);
    }

    snapshot_test_reset();
    printf("backdrop_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
