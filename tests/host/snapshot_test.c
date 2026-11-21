#define SNAPSHOT_HOST_TEST 1

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
static const char *g_env_png;
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
/* Mock Objective-C & CoreGraphics & ImageIO objects                  */
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
    int no_layer_render;
    /* Paint the search-field layout (Library title,
     * field background, placeholder glyph bars) in point coordinates
     * via the context CTM. paint_glyphs=0 paints the background but
     * no glyphs: the device symptom (bg present, text missing). */
    int paint_field;
    int paint_glyphs;
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

static int g_render_order[16];
static int g_render_count;

/* Call-order log for the commit-before-snapshot fix.
 * Codes: 1 = [CATransaction flush], 2 = [window layoutIfNeeded],
 * 3 = -[CALayer renderInContext:]. The fix requires 1, then 2s,
 * then 3s. */
static int g_call_seq[64];
static int g_call_seq_n;
static int g_flush_calls;
static int g_layout_calls;

static void seq_push(int code) {
    if (g_call_seq_n < 64) g_call_seq[g_call_seq_n++] = code;
}

static int g_present_calls;
static int g_present_rc = 0;
static uint32_t g_present_w, g_present_h, g_present_stride;
static uint32_t g_present_first_pixel;
/* Full copy of the last presented buffer, so tests can
 * scan it for painted pixels (title / field bg / glyphs). */
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
        if (o->no_layer_render) return 0;
        return sel_eq(sel, "renderInContext:");
    default:
        return 0;
    }
}

/* Paint one point rect into the mock bitmap context,
 * mapping points to pixels through the context CTM exactly as
 * CoreGraphics would (px = x*sx + tx, py = y*sy + ty). Colors are
 * BGRA uint32 LE with alpha 255. */
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

/* The reference PNG layout, in UIKit points
 * (360x800): Library title x 19..136, y 53..85 (black); search
 * field background x 16..343, y 97..132 (light gray 238,238,239);
 * placeholder glyph bars inside the field (gray 131,131,136).
 * The magnifier icon sits at x 24..45; the glyph bars stand in
 * for the "Search" text. */
static void paint_search_field(mock_cg_ctx_t *ctx, int glyphs) {
    static const double bars[5][2] = {
        { 48.0, 58.0 }, { 64.0, 74.0 }, { 80.0, 90.0 },
        { 96.0, 106.0 }, { 112.0, 122.0 },
    };
    uint32_t title_px = 0xFF000000u;
    uint32_t field_px = (0xFFu << 24) | (238u << 16) | (238u << 8) | 239u;
    uint32_t glyph_px = (0xFFu << 24) | (131u << 16) | (131u << 8) | 136u;
    int i;
    paint_rect_pt(ctx, 19.0, 53.0, 136.0, 85.0, title_px);
    paint_rect_pt(ctx, 16.0, 97.0, 343.0, 132.0, field_px);
    if (!glyphs) return;
    for (i = 0; i < 5; i++) {
        paint_rect_pt(ctx, bars[i][0], 108.0, bars[i][1], 122.0, glyph_px);
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
        g_flush_calls++;
        seq_push(1);
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
        if (sel_eq(sel, "layoutIfNeeded")) {
            g_layout_calls++;
            seq_push(2);
            return NULL;
        }
    }
    if (o->kind == KIND_LAYER && sel_eq(sel, "renderInContext:")) {
        __builtin_va_list ap;
        __builtin_va_start(ap, sel);
        mock_cg_ctx_t *ctx = __builtin_va_arg(ap, mock_cg_ctx_t *);
        __builtin_va_end(ap);
        seq_push(3);
        if (g_render_count < 16) {
            g_render_order[g_render_count++] = o->tag;
        }
        if (ctx && ctx->data && o->fill_pixel != 0) {
            uint32_t *px = (uint32_t *)ctx->data;
            px[0] = o->fill_pixel;
        }
        if (ctx && o->paint_field) {
            paint_search_field(ctx, o->paint_glyphs);
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
    assert(strcmp((const char *)type, "public.png") == 0);
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

/* Count exact-color pixels in a buffer rect. Buffer
 * rows are bottom-origin (row 0 = canvas y 0); the presented copy
 * is a verbatim copy of the snapshot canvas. */
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
 * buffer col = pt.x * scale, buffer row = h - pt.y * scale (the
 * CTM flip; the PNG writer flips it back, so PNG row = h - buffer
 * row, matching the reference PNG rows). */
static uint32_t count_color_in_pt_rect(uint32_t color, double px0,
                                       double py0, double px1, double py1,
                                       double scale, uint32_t h) {
    int x0 = (int)(px0 * scale);
    int x1 = (int)(px1 * scale);
    int y0 = (int)((double)h - py1 * scale);
    int y1 = (int)((double)h - py0 * scale);
    return count_color_in_rect(color, x0, y0, x1, y1);
}

int main(void) {
    /* 1. Disabled by default: strict no-op */
    reset_all();
    snapshot_init();
    assert(g_dlsym_calls == 0);
    assert(g_dispatch_after_calls == 0);
    assert(g_log_len == 0);

    g_env_snapshot = "0";
    snapshot_init();
    assert(g_dlsym_calls == 0);
    assert(g_dispatch_after_calls == 0);

    /* 2. Enabled: arms dispatch_after_f(1000ms) on _dispatch_main_q */
    reset_all();
    g_env_snapshot = "1";
    snapshot_init();
    assert(g_dispatch_after_calls == 1);
    assert(g_last_dispatch_when == 1000000000ULL);
    assert(g_last_dispatch_cb != NULL);
    assert(strstr(g_log_buf, "[snapshot] pump armed") != NULL);

    /* 3. No windows yet -> logs once per 10s */
    reset_log();
    g_now_ns = 1000000000ULL;
    g_last_dispatch_cb(g_last_dispatch_ctx);
    assert(g_dispatch_after_calls == 2); /* rescheduled */
    assert(g_present_calls == 0);
    assert(strstr(g_log_buf, "[snapshot] no windows yet") != NULL);

    reset_log();
    g_now_ns = 5000000000ULL; /* +4s (< 10s): suppressed */
    snapshot_test_tick();
    assert(g_log_len == 0);

    reset_log();
    g_now_ns = 11000000000ULL; /* +10s: logs again */
    snapshot_test_tick();
    assert(strstr(g_log_buf, "[snapshot] no windows yet") != NULL);

    /* 4. Window collection: connectedScenes + legacy, dedup, filter, back-to-front sort, CTM, scale, PNG */
    reset_all();
    g_env_snapshot = "1";
    g_env_png = "/data/local/tmp/ioswine/snap.png";

    mock_obj_t scr = { .kind = KIND_SCREEN, .scale = 2.0 };
    mock_obj_t l_back = { .kind = KIND_LAYER, .tag = 10, .fill_pixel = 0 }; /* empty first frame */
    mock_obj_t l_front = { .kind = KIND_LAYER, .tag = 20, .fill_pixel = 0 };
    mock_obj_t l_bad = { .kind = KIND_LAYER, .tag = 99, .no_layer_render = 1 };

    mock_obj_t w_front = {
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
        .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
        .level = 10.0, .screen = &scr, .layer = &l_front,
    };
    mock_obj_t w_back = {
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
        .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
        .level = 0.0, .screen = &scr, .layer = &l_back,
    };
    mock_obj_t w_hidden = {
        .kind = KIND_WINDOW, .hidden = 1, .alpha = 1.0,
        .bounds = { {0, 0}, {360, 800} }, .level = 1.0, .screen = &scr, .layer = &l_back,
    };
    mock_obj_t w_zero_alpha = {
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 0.0,
        .bounds = { {0, 0}, {360, 800} }, .level = 1.0, .screen = &scr, .layer = &l_back,
    };
    mock_obj_t w_nolayer = {
        .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
        .bounds = { {0, 0}, {360, 800} }, .level = 1.0, .screen = &scr, .layer = &l_bad,
    };

    mock_obj_t scene_wins = {
        .kind = KIND_ARRAY, .count = 4,
        .items = { &w_front, &w_hidden, &w_back, &w_nolayer },
    };
    mock_obj_t legacy_wins = {
        .kind = KIND_ARRAY, .count = 2,
        .items = { &w_back, &w_zero_alpha }, /* w_back duplicated! */
    };
    mock_obj_t scene_ok = { .kind = KIND_SCENE, .items = { &scene_wins } };
    mock_obj_t scene_bad = { .kind = KIND_SCENE_NOWIN };
    mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 2, .items = { &scene_bad, &scene_ok } };
    mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
    mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set, .legacy_windows = &legacy_wins };
    g_shared_app = &app;

    /* Frame 1: empty buffer -> present called, PNG NOT written */
    g_present_rc = -1;
    snapshot_test_tick();
    assert(g_present_calls == 1);
    assert(g_present_w == 720 && g_present_h == 1600 && g_present_stride == 720 * 4);
    assert(g_last_bitmap_info == (2u | (2u << 12)));
    assert(g_last_tx == 0.0 && g_last_ty == 1600.0);
    assert(g_last_sx == 2.0 && g_last_sy == -2.0);
    assert(g_render_count == 2);
    assert(g_render_order[0] == 10); /* w_back (level 0) before w_front (level 10) */
    assert(g_render_order[1] == 20);
    assert(g_png_calls == 0);
    assert(g_pool_push_calls == 1 && g_pool_pop_calls == 1);
    assert(strstr(g_log_buf, "[snapshot] frame 1 720x1600 windows=2 rc=-1") != NULL);

    /* Frame 2: non-empty pixels -> PNG NOT written yet (* the one-shot PNG waits for the second consecutive non-empty
     * tick at the same size); fallback scale 3.0 when screen=NULL */
    reset_log();
    g_render_count = 0;
    w_back.screen = NULL;
    w_front.screen = NULL;
    l_back.fill_pixel = 0xff112233u;
    g_present_rc = 0;
    snapshot_test_tick();
    assert(g_present_calls == 2);
    assert(g_present_w == 1080 && g_present_h == 2400 && g_present_stride == 1080 * 4);
    assert(g_present_first_pixel == 0xff112233u);
    assert(g_png_calls == 0);
    assert(strstr(g_log_buf, "[snapshot] frame 2 1080x2400 windows=2 rc=0") != NULL);

    /* Frame 3: second consecutive non-empty tick at the same size ->
     * PNG written once, and never again */
    snapshot_test_tick();
    assert(g_present_calls == 3);
    assert(g_png_calls == 1);
    assert(strcmp(g_png_last_path, "/data/local/tmp/ioswine/snap.png") == 0);

    /* Frame 4: PNG is NOT written a second time */
    snapshot_test_tick();
    assert(g_present_calls == 4);
    assert(g_png_calls == 1);

    /* 5. Search-field placeholder: the reference PNG layout
     * at device geometry (360x800 pt @ 4.0 -> 1440x3200).
     * The presented frame must carry the Library title pixels AND
     * the placeholder glyph pixels inside the field, and the commit
     * calls must run before the render. */
    reset_all();
    g_env_snapshot = "1";
    g_env_png = "/data/local/tmp/ioswine/snap.png";
    {
        mock_obj_t scr4 = { .kind = KIND_SCREEN, .scale = 4.0 };
        mock_obj_t l_field = { .kind = KIND_LAYER, .tag = 30,
                               .paint_field = 1, .paint_glyphs = 1 };
        mock_obj_t w_main = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 0.0, .screen = &scr4, .layer = &l_field,
        };
        mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } };
        mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
        mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
        mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
        mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                           .legacy_windows = NULL };
        uint32_t title_px = 0xFF000000u;
        uint32_t field_px = (0xFFu << 24) | (238u << 16) | (238u << 8) | 239u;
        uint32_t glyph_px = (0xFFu << 24) | (131u << 16) | (131u << 8) | 136u;
        uint32_t n_title, n_desc, n_field, n_glyph;
        g_shared_app = &app;
        g_present_rc = 0;

        /* Tick 1 @4x: frame 1440x3200, commit before render, PNG not yet. */
        snapshot_test_tick();
        assert(g_present_calls == 1);
        assert(g_present_w == 1440 && g_present_h == 3200);
        assert(g_flush_calls == 1 && g_layout_calls == 1);
        assert(g_call_seq_n == 3);
        assert(g_call_seq[0] == 1 && g_call_seq[1] == 2 && g_call_seq[2] == 3);
        assert(g_png_calls == 0);
        n_title = count_color_in_pt_rect(title_px, 19.0, 53.0, 136.0, 85.0, 4.0, 3200);
        n_desc = count_color_in_pt_rect(title_px, 19.0, 78.0, 136.0, 85.0, 4.0, 3200);
        n_field = count_color_in_pt_rect(field_px, 16.0, 97.0, 343.0, 132.0, 4.0, 3200);
        n_glyph = count_color_in_pt_rect(glyph_px, 24.0, 107.0, 154.0, 123.0, 4.0, 3200);
        assert(n_title > 1000);  /* Library title paints */
        assert(n_desc > 1000);   /* "y" descender band paints */
        assert(n_field > 100000);/* field background paints */
        assert(n_glyph > 100);   /* placeholder glyphs survive the pipeline */

        /* Tick 2 @4x: second consecutive non-empty tick -> PNG written. */
        snapshot_test_tick();
        assert(g_present_calls == 2);
        assert(g_png_calls == 1);
    }

    /* 5b. Discriminator: background paints but glyphs do not (the
     * device symptom). The counts must separate the two: field bg
     * present, glyphs zero, title still present. */
    reset_all();
    g_env_snapshot = "1";
    {
        mock_obj_t scr4 = { .kind = KIND_SCREEN, .scale = 4.0 };
        mock_obj_t l_field = { .kind = KIND_LAYER, .tag = 30,
                               .paint_field = 1, .paint_glyphs = 0 };
        mock_obj_t w_main = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 0.0, .screen = &scr4, .layer = &l_field,
        };
        mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } };
        mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
        mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
        mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
        mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                           .legacy_windows = NULL };
        uint32_t title_px = 0xFF000000u;
        uint32_t field_px = (0xFFu << 24) | (238u << 16) | (238u << 8) | 239u;
        uint32_t glyph_px = (0xFFu << 24) | (131u << 16) | (131u << 8) | 136u;
        uint32_t n_title, n_field, n_glyph;
        g_shared_app = &app;
        g_present_rc = 0;
        snapshot_test_tick();
        assert(g_present_calls == 1);
        n_title = count_color_in_pt_rect(title_px, 19.0, 53.0, 136.0, 85.0, 4.0, 3200);
        n_field = count_color_in_pt_rect(field_px, 16.0, 97.0, 343.0, 132.0, 4.0, 3200);
        n_glyph = count_color_in_pt_rect(glyph_px, 24.0, 107.0, 154.0, 123.0, 4.0, 3200);
        assert(n_title > 1000);
        assert(n_field > 100000);
        assert(n_glyph == 0); /* the detector sees the empty field */
    }

    /* 5c. 1x ground truth: the reference PNG rows. At scale 1.0
     * the glyph band pt y 107..123 lands at buffer rows 677..693
     * (PNG row = 800 - buffer row), cols 24..154. */
    reset_all();
    g_env_snapshot = "1";
    {
        mock_obj_t scr1 = { .kind = KIND_SCREEN, .scale = 1.0 };
        mock_obj_t l_field = { .kind = KIND_LAYER, .tag = 30,
                               .paint_field = 1, .paint_glyphs = 1 };
        mock_obj_t w_main = {
            .kind = KIND_WINDOW, .hidden = 0, .alpha = 1.0,
            .bounds = { {0, 0}, {360, 800} }, .frame = { {0, 0}, {360, 800} },
            .level = 0.0, .screen = &scr1, .layer = &l_field,
        };
        mock_obj_t wins = { .kind = KIND_ARRAY, .count = 1, .items = { &w_main } };
        mock_obj_t scene = { .kind = KIND_SCENE, .items = { &wins } };
        mock_obj_t scenes_arr = { .kind = KIND_ARRAY, .count = 1, .items = { &scene } };
        mock_obj_t scenes_set = { .kind = KIND_SET, .items = { &scenes_arr } };
        mock_obj_t app = { .kind = KIND_APP, .scenes_set = &scenes_set,
                           .legacy_windows = NULL };
        uint32_t glyph_px = (0xFFu << 24) | (131u << 16) | (131u << 8) | 136u;
        uint32_t n_glyph;
        g_shared_app = &app;
        g_present_rc = 0;
        snapshot_test_tick();
        assert(g_present_calls == 1);
        assert(g_present_w == 360 && g_present_h == 800);
        n_glyph = count_color_in_pt_rect(glyph_px, 24.0, 107.0, 154.0, 123.0, 1.0, 800);
        assert(n_glyph > 100);
        /* Exact band check against the PNG pixel analysis. */
        assert(count_color_in_rect(glyph_px, 24, 677, 154, 693) > 100);
    }

    snapshot_test_reset();
    printf("snapshot_test: all assertions passed\n");
    return 0;
}
