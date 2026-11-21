/*
 * snapshot.c - Periodic UIWindow -> CALayer snapshot pump (fallback display path).
 *
 * Activated only when IOSWINE_SNAPSHOT=1.
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc, no malloc,
 * no __thread, no static function-pointer tables).
 */
#include "snapshot.h"
#include "protocol.h"

#ifndef SNAPSHOT_HOST_TEST
#include "linux.h"
#include "touch.h"
extern const char *lsk_getenv(const char *name);
#endif

/* ------------------------------------------------------------------ */
/* Types and constants                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    double x;
    double y;
} snap_point_t;

typedef struct {
    double width;
    double height;
} snap_size_t;

typedef struct {
    snap_point_t origin;
    snap_size_t size;
} snap_rect_t;

typedef struct {
    void *win;
    double level;
    uint32_t order;
} snap_win_entry_t;

#define SNAP_MAX_WINDOWS        64u
#define SNAP_INTERVAL_NS        1000000000LL
#define SNAP_NO_WIN_LOG_NS      10000000000ULL
#define SNAP_MAX_DIM            8192u
#define SNAP_FALLBACK_SCALE     3.0
/* kCGImageAlphaPremultipliedFirst (2) | kCGBitmapByteOrder32Little (2 << 12) */
#define SNAP_CG_BITMAP_BGRA     (2u | (2u << 12))
#define SNAP_CF_STRING_UTF8     0x08000100u

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache */
#define SNAP_GAPIS_ADDR         0x1ec954040ULL
#define SNAP_GAPIS_OBJ_DISC     0x63faULL
#define SNAP_SLOT_DLOPEN        13ULL
#define SNAP_DIV_DLOPEN         0xbf31ULL
#define SNAP_SLOT_DLSYM         16ULL
#define SNAP_DIV_DLSYM          0x839dULL

/* ------------------------------------------------------------------ */
/* Test hooks (host test only)                                        */
/* ------------------------------------------------------------------ */

#ifdef SNAPSHOT_HOST_TEST
static const snapshot_test_ops_t *g_test_ops = NULL;
#endif

/* ------------------------------------------------------------------ */
/* Runtime symbol cache (all zero-initialized in .bss)                */
/* ------------------------------------------------------------------ */

typedef struct {
    int dispatch_resolved;
    void *dispatch_main_q;
    void (*dispatch_after_f)(uint64_t when, void *queue, void *ctx, void (*work)(void *));
    uint64_t (*dispatch_time)(uint64_t when, int64_t delta);

    int core_resolved;
    void *(*objc_getClass)(const char *name);
    void *(*sel_registerName)(const char *name);
    void *objc_msgSend;
#ifdef SNAPSHOT_HOST_TEST
    void *objc_msgSend_f64;
    void *objc_msgSend_rect;
#endif
    void *(*objc_autoreleasePoolPush)(void);
    void (*objc_autoreleasePoolPop)(void *pool);

    void *(*CGColorSpaceCreateDeviceRGB)(void);
    void (*CGColorSpaceRelease)(void *space);
    void *(*CGBitmapContextCreate)(void *data, size_t w, size_t h, size_t bpc,
                                   size_t bpr, void *space, uint32_t bitmapInfo);
    void (*CGContextRelease)(void *ctx);
    void (*CGContextTranslateCTM)(void *ctx, double tx, double ty);
    void (*CGContextScaleCTM)(void *ctx, double sx, double sy);
    void (*CGContextSaveGState)(void *ctx);
    void (*CGContextRestoreGState)(void *ctx);
    void *(*CGBitmapContextCreateImage)(void *ctx);
    void (*CGImageRelease)(void *img);

    int imageio_resolved;
    void *(*CFURLCreateFromFileSystemRepresentation)(void *alloc, const uint8_t *buf,
                                                     long len, uint8_t isDir);
    void *(*CFStringCreateWithCString)(void *alloc, const char *cStr, uint32_t encoding);
    void (*CFRelease)(void *cf);
    void *(*CGImageDestinationCreateWithURL)(void *url, void *type, size_t count, void *opts);
    void (*CGImageDestinationAddImage)(void *idst, void *image, void *props);
    uint8_t (*CGImageDestinationFinalize)(void *idst);

    void *sel_respondsToSelector;
    void *sel_sharedApplication;
    void *sel_connectedScenes;
    void *sel_allObjects;
    void *sel_windows;
    void *sel_count;
    void *sel_objectAtIndex;
    void *sel_isHidden;
    void *sel_alpha;
    void *sel_bounds;
    void *sel_frame;
    void *sel_windowLevel;
    void *sel_screen;
    void *sel_mainScreen;
    void *sel_scale;
    void *sel_contentScaleFactor;
    void *sel_layer;
    void *sel_renderInContext;
    void *sel_layoutIfNeeded;
    void *sel_flush;
    /* Navigation settle: view-controller chain walk. */
    void *sel_rootViewController;
    void *sel_topViewController;
    void *sel_presentedViewController;
    void *sel_selectedViewController;
    void *sel_view;
    const char *(*object_getClassName)(void *obj);
    /* Backdrop layers: walking the layer tree to find
     * CABackdropLayers, hiding them, and reading the effect style.
     * Best-effort: every use checks for NULL first, so a missing
     * selector only disables the backdrop path. */
    void *sel_sublayers;
    void *sel_isKindOfClass;
    void *sel_setHidden;
    void *sel_delegate;
    void *sel_superview;
    void *sel_effect;
    void *sel_style;
    void *sel_traitCollection;
    void *sel_userInterfaceStyle;
    void *backdrop_cls;
    const char *(*class_getName)(void *cls);
} snap_syms_t;

static snap_syms_t g_syms;
static int g_started;
static uint64_t g_frame_no;
/* Frame source accounting: which producer a frame
 * came from. g_tick_src is the source of the most recent request
 * (0 = pump, 1 = carender); the tick that renders reads it when it
 * runs. Last-write-wins is exact in every real configuration: when
 * carender is armed the pump is deferred, so only carender
 * requests; otherwise only the pump requests. g_pump_frames /
 * g_ca_frames count rendered frames per source; their sum always
 * equals g_frame_no. */
#define SNAP_TICK_SRC_PUMP 0
#define SNAP_TICK_SRC_CA 1
static int g_tick_src;
static uint64_t g_pump_frames;
static uint64_t g_ca_frames;
static int g_logged_no_win;
static uint64_t g_last_no_win_ns;
static int g_png_written;
static uint8_t *g_buf;
static size_t g_buf_size;
/* Last frame size actually accepted by the presenter: the
 * presenter session is fixed at that size, so a size change needs a
 * session reset before the next present. 0 means no frame presented
 * yet. g_logged_info_scale suppresses repeat INFO-scale log lines. */
static uint32_t g_last_present_w;
static uint32_t g_last_present_h;
static double g_logged_info_scale = -1.0;
/* Settled-frame one-shot PNG: consecutive non-empty
 * ticks at one frame size, and the size of the last non-empty tick.
 * The first non-empty frame catches the UI mid-layout (a white
 * search field), so the PNG waits for the second consecutive
 * non-empty tick at the same size. A size change or an empty frame
 * restarts the count. */
static int g_png_settled_ticks;
static uint32_t g_png_last_w;
static uint32_t g_png_last_h;

/* ------------------------------------------------------------------ */
/* Weak fallback for ioswine_present_frame                 */
/* ------------------------------------------------------------------ */

__attribute__((weak))
int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    (void)bgra;
    (void)w;
    (void)h;
    (void)stride;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Weak fallbacks for the presenter session. present.c provides */
/* the strong definitions when it is linked in; without it these     */
/* no-ops keep snapshot.c linkable (same pattern as                   */
/* ioswine_present_frame above). present_last_info reports the       */
/* latest INFO mapping (0-filled when no INFO arrived);               */
/* present_shutdown tears the session down so the next present call */
/* re-inits at a new frame size.                                      */
/* ------------------------------------------------------------------ */

__attribute__((weak))
isws_info_t present_last_info(void) {
    isws_info_t z;
    z.screen_w = 0;
    z.screen_h = 0;
    z.client_w = 0;
    z.client_h = 0;
    z.scale = 0.0f;
    z.offset_x = 0.0f;
    z.offset_y = 0.0f;
    z.rotation = 0;
    return z;
}

__attribute__((weak))
void present_shutdown(void) {
}

/* ------------------------------------------------------------------ */
/* Freestanding string / memory / logging / syscall helpers           */
/* ------------------------------------------------------------------ */

static size_t snap_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void snap_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}

static const char *snap_getenv(const char *name) {
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->getenv_fn) return g_test_ops->getenv_fn(name);
    return NULL;
#else
    return lsk_getenv(name);
#endif
}

static void snap_log_write(const char *buf, size_t len) {
    if (!buf || len == 0) return;
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->log_fn) {
        g_test_ops->log_fn(buf, len);
    }
#else
    sys_write(2, buf, len);
#endif
}

static void snap_log_str(const char *s) {
    snap_log_write(s, snap_strlen(s));
}

static size_t fmt_u64(char *out, uint64_t val) {
    char tmp[24];
    size_t n = 0;
    if (val == 0) {
        out[0] = '0';
        return 1;
    }
    while (val > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

static size_t fmt_i64(char *out, int64_t val) {
    if (val < 0) {
        out[0] = '-';
        uint64_t mag = (uint64_t)(-(val + 1)) + 1ULL;
        return 1 + fmt_u64(out + 1, mag);
    }
    return fmt_u64(out, (uint64_t)val);
}

/* Frame log line: the src tag says which producer
 * rendered the frame, so a device log can tell carender frames
 * from pump fallback frames without guessing from the rate:
 *   [snapshot] frame N WxH windows=K rc=R src=pump
 *   [snapshot] frame N WxH windows=K rc=R src=ca
 * The tag is appended after rc=, so the old prefix still matches
 * a substring search. */
static void snap_log_frame(uint64_t frame_no, uint32_t w, uint32_t h,
                           uint32_t windows, int rc, int src) {
    char buf[128];
    size_t pos = 0;
    const char *p1 = "[snapshot] frame ";
    for (size_t i = 0; p1[i]; i++) buf[pos++] = p1[i];
    pos += fmt_u64(buf + pos, frame_no);
    buf[pos++] = ' ';
    pos += fmt_u64(buf + pos, w);
    buf[pos++] = 'x';
    pos += fmt_u64(buf + pos, h);
    const char *p2 = " windows=";
    for (size_t i = 0; p2[i]; i++) buf[pos++] = p2[i];
    pos += fmt_u64(buf + pos, windows);
    const char *p3 = " rc=";
    for (size_t i = 0; p3[i]; i++) buf[pos++] = p3[i];
    pos += fmt_i64(buf + pos, (int64_t)rc);
    const char *p4 = src == SNAP_TICK_SRC_CA ? " src=ca" : " src=pump";
    for (size_t i = 0; p4[i]; i++) buf[pos++] = p4[i];
    buf[pos++] = '\n';
    snap_log_write(buf, pos);
}

/* One summary line every 60 frames, so a device log shows the
 * per-source totals even when individual frame lines are lost in
 * interleaved output: "[snapshot] frames pump=N ca=M". */
static void snap_log_frame_counts(void) {
    char buf[96];
    size_t pos = 0;
    const char *p1 = "[snapshot] frames pump=";
    for (size_t i = 0; p1[i]; i++) buf[pos++] = p1[i];
    pos += fmt_u64(buf + pos, g_pump_frames);
    const char *p2 = " ca=";
    for (size_t i = 0; p2[i]; i++) buf[pos++] = p2[i];
    pos += fmt_u64(buf + pos, g_ca_frames);
    buf[pos++] = '\n';
    snap_log_write(buf, pos);
}

static uint64_t snap_now_ns(void) {
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->now_ns) return g_test_ops->now_ns();
    return 0;
#else
    int64_t ts[2] = { 0, 0 };
    if (sys2(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)ts) < 0) {
        return 0;
    }
    return (uint64_t)ts[0] * 1000000000ULL + (uint64_t)ts[1];
#endif
}

static void *snap_alloc_buf(size_t size) {
    if (size == 0) return NULL;
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->alloc_buf) return g_test_ops->alloc_buf(size);
    return NULL;
#else
    void *p = sys_mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uintptr_t)p >= (uintptr_t)-4095) return NULL;
    return p;
#endif
}

static void snap_free_buf(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->free_buf) g_test_ops->free_buf(ptr, size);
#else
    sys_munmap(ptr, size);
#endif
}

/* ------------------------------------------------------------------ */
/* DSC dlsym / dlopen + arm64e PAC signing                            */
/* ------------------------------------------------------------------ */

static void *lsk_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(SNAPSHOT_HOST_TEST)
    uint64_t v = (uint64_t)p;
    __asm__ volatile(
        "xpaci %0\n"
        "paciza %0\n"
        : "+r"(v)
    );
    return (void *)v;
#else
    return p;
#endif
}

#if defined(__aarch64__) && !defined(SNAPSHOT_HOST_TEST)
static __attribute__((noinline)) void *gapis_call2(uint64_t slot, uint64_t div,
                                                   uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)SNAP_GAPIS_ADDR;
    if (!obj) return NULL;
    uint64_t vt = *(volatile uint64_t *)obj;
    uint64_t obj_div = (obj & 0x0000ffffffffffffULL) | (SNAP_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    uint64_t slot_addr = vt + slot * 8ULL;
    uint64_t fn = *(volatile uint64_t *)slot_addr;
    uint64_t fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

    register uint64_t x0 __asm__("x0") = obj;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x16 __asm__("x16") = fn;
    register uint64_t x17 __asm__("x17") = fn_div;
    __asm__ volatile(
        "blraa x16, x17"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "+r"(x17)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "cc", "memory"
    );
    return (void *)x0;
}
#endif

static void *snap_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef SNAPSHOT_HOST_TEST
    if (g_test_ops && g_test_ops->dlsym_default) {
        return g_test_ops->dlsym_default(name);
    }
    return NULL;
#elif defined(__aarch64__)
    return gapis_call2(SNAP_SLOT_DLSYM, SNAP_DIV_DLSYM,
                       (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static void *snap_dlopen(const char *path) {
    if (!path) return NULL;
#if defined(__aarch64__) && !defined(SNAPSHOT_HOST_TEST)
    return gapis_call2(SNAP_SLOT_DLOPEN, SNAP_DIV_DLOPEN,
                       (uint64_t)path, 1 /* RTLD_LAZY */);
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Objective-C typed objc_msgSend wrappers                            */
/* ------------------------------------------------------------------ */

static inline uint8_t objc_responds(void *obj, void *sel) {
    if (!obj || !sel || !g_syms.objc_msgSend || !g_syms.sel_respondsToSelector) {
        return 0;
    }
    uint8_t (*fn)(void *, void *, void *) =
        (uint8_t (*)(void *, void *, void *))g_syms.objc_msgSend;
    return fn(obj, g_syms.sel_respondsToSelector, sel) ? 1 : 0;
}

static inline void *msg_id(void *obj, void *sel) {
    void *(*fn)(void *, void *) = (void *(*)(void *, void *))g_syms.objc_msgSend;
    return fn(obj, sel);
}

static inline void *msg_id_u64(void *obj, void *sel, uint64_t arg) {
    void *(*fn)(void *, void *, uint64_t) =
        (void *(*)(void *, void *, uint64_t))g_syms.objc_msgSend;
    return fn(obj, sel, arg);
}

static inline uint64_t msg_u64(void *obj, void *sel) {
    uint64_t (*fn)(void *, void *) = (uint64_t (*)(void *, void *))g_syms.objc_msgSend;
    return fn(obj, sel);
}

static inline uint8_t msg_bool(void *obj, void *sel) {
    uint8_t (*fn)(void *, void *) = (uint8_t (*)(void *, void *))g_syms.objc_msgSend;
    return fn(obj, sel) ? 1 : 0;
}

static inline double msg_f64(void *obj, void *sel) {
#ifdef SNAPSHOT_HOST_TEST
    void *target = g_syms.objc_msgSend_f64 ? g_syms.objc_msgSend_f64 : g_syms.objc_msgSend;
    double (*fn)(void *, void *) = (double (*)(void *, void *))target;
#else
    double (*fn)(void *, void *) = (double (*)(void *, void *))g_syms.objc_msgSend;
#endif
    return fn(obj, sel);
}

static inline snap_rect_t msg_rect(void *obj, void *sel) {
#ifdef SNAPSHOT_HOST_TEST
    void *target = g_syms.objc_msgSend_rect ? g_syms.objc_msgSend_rect : g_syms.objc_msgSend;
    snap_rect_t (*fn)(void *, void *) = (snap_rect_t (*)(void *, void *))target;
#else
    snap_rect_t (*fn)(void *, void *) = (snap_rect_t (*)(void *, void *))g_syms.objc_msgSend;
#endif
    return fn(obj, sel);
}

static inline void msg_void_id(void *obj, void *sel, void *arg) {
    void (*fn)(void *, void *, void *) = (void (*)(void *, void *, void *))g_syms.objc_msgSend;
    fn(obj, sel, arg);
}

/* No-argument void message (layoutIfNeeded, CATransaction flush). */
static inline void msg_void(void *obj, void *sel) {
    void (*fn)(void *, void *) = (void (*)(void *, void *))g_syms.objc_msgSend;
    fn(obj, sel);
}

/* BOOL message with one object argument (isKindOfClass:). */
static inline uint8_t msg_bool_id(void *obj, void *sel, void *arg) {
    uint8_t (*fn)(void *, void *, void *) =
        (uint8_t (*)(void *, void *, void *))g_syms.objc_msgSend;
    return fn(obj, sel, arg) ? 1 : 0;
}

/* Void message with one BOOL argument (setHidden:). */
static inline void msg_void_u8(void *obj, void *sel, uint8_t arg) {
    void (*fn)(void *, void *, uint8_t) =
        (void (*)(void *, void *, uint8_t))g_syms.objc_msgSend;
    fn(obj, sel, arg);
}

/* ------------------------------------------------------------------ */
/* Symbol resolution                                                  */
/* ------------------------------------------------------------------ */

static int ensure_dispatch_syms(void) {
    if (g_syms.dispatch_resolved) {
        return (g_syms.dispatch_main_q && g_syms.dispatch_after_f && g_syms.dispatch_time) ? 0 : -1;
    }
    g_syms.dispatch_resolved = 1;
    g_syms.dispatch_main_q = snap_dlsym("_dispatch_main_q");
    g_syms.dispatch_after_f =
        (void (*)(uint64_t, void *, void *, void (*)(void *)))lsk_sign_fn(snap_dlsym("dispatch_after_f"));
    g_syms.dispatch_time =
        (uint64_t (*)(uint64_t, int64_t))lsk_sign_fn(snap_dlsym("dispatch_time"));
    return (g_syms.dispatch_main_q && g_syms.dispatch_after_f && g_syms.dispatch_time) ? 0 : -1;
}

static int ensure_core_syms(void) {
    if (g_syms.core_resolved) {
        return (g_syms.objc_getClass && g_syms.sel_registerName && g_syms.objc_msgSend &&
                g_syms.CGColorSpaceCreateDeviceRGB && g_syms.CGColorSpaceRelease &&
                g_syms.CGBitmapContextCreate && g_syms.CGContextRelease &&
                g_syms.CGContextTranslateCTM && g_syms.CGContextScaleCTM) ? 0 : -1;
    }
    g_syms.core_resolved = 1;

    g_syms.objc_getClass = (void *(*)(const char *))lsk_sign_fn(snap_dlsym("objc_getClass"));
    g_syms.sel_registerName = (void *(*)(const char *))lsk_sign_fn(snap_dlsym("sel_registerName"));
    g_syms.objc_msgSend = lsk_sign_fn(snap_dlsym("objc_msgSend"));
#ifdef SNAPSHOT_HOST_TEST
    g_syms.objc_msgSend_f64 = snap_dlsym("objc_msgSend_fpret");
    g_syms.objc_msgSend_rect = snap_dlsym("objc_msgSend_stret");
#endif
    g_syms.objc_autoreleasePoolPush =
        (void *(*)(void))lsk_sign_fn(snap_dlsym("objc_autoreleasePoolPush"));
    g_syms.objc_autoreleasePoolPop =
        (void (*)(void *))lsk_sign_fn(snap_dlsym("objc_autoreleasePoolPop"));

    void *cs_create = snap_dlsym("CGColorSpaceCreateDeviceRGB");
    if (!cs_create) {
        snap_dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
        cs_create = snap_dlsym("CGColorSpaceCreateDeviceRGB");
    }
    g_syms.CGColorSpaceCreateDeviceRGB = (void *(*)(void))lsk_sign_fn(cs_create);
    g_syms.CGColorSpaceRelease = (void (*)(void *))lsk_sign_fn(snap_dlsym("CGColorSpaceRelease"));
    g_syms.CGBitmapContextCreate =
        (void *(*)(void *, size_t, size_t, size_t, size_t, void *, uint32_t))
        lsk_sign_fn(snap_dlsym("CGBitmapContextCreate"));
    g_syms.CGContextRelease = (void (*)(void *))lsk_sign_fn(snap_dlsym("CGContextRelease"));
    g_syms.CGContextTranslateCTM =
        (void (*)(void *, double, double))lsk_sign_fn(snap_dlsym("CGContextTranslateCTM"));
    g_syms.CGContextScaleCTM =
        (void (*)(void *, double, double))lsk_sign_fn(snap_dlsym("CGContextScaleCTM"));
    g_syms.CGContextSaveGState = (void (*)(void *))lsk_sign_fn(snap_dlsym("CGContextSaveGState"));
    g_syms.CGContextRestoreGState =
        (void (*)(void *))lsk_sign_fn(snap_dlsym("CGContextRestoreGState"));
    g_syms.CGBitmapContextCreateImage =
        (void *(*)(void *))lsk_sign_fn(snap_dlsym("CGBitmapContextCreateImage"));
    g_syms.CGImageRelease = (void (*)(void *))lsk_sign_fn(snap_dlsym("CGImageRelease"));

    if (g_syms.sel_registerName) {
        g_syms.sel_respondsToSelector = g_syms.sel_registerName("respondsToSelector:");
        g_syms.sel_sharedApplication = g_syms.sel_registerName("sharedApplication");
        g_syms.sel_connectedScenes = g_syms.sel_registerName("connectedScenes");
        g_syms.sel_allObjects = g_syms.sel_registerName("allObjects");
        g_syms.sel_windows = g_syms.sel_registerName("windows");
        g_syms.sel_count = g_syms.sel_registerName("count");
        g_syms.sel_objectAtIndex = g_syms.sel_registerName("objectAtIndex:");
        g_syms.sel_isHidden = g_syms.sel_registerName("isHidden");
        g_syms.sel_alpha = g_syms.sel_registerName("alpha");
        g_syms.sel_bounds = g_syms.sel_registerName("bounds");
        g_syms.sel_frame = g_syms.sel_registerName("frame");
        g_syms.sel_windowLevel = g_syms.sel_registerName("windowLevel");
        g_syms.sel_screen = g_syms.sel_registerName("screen");
        g_syms.sel_mainScreen = g_syms.sel_registerName("mainScreen");
        g_syms.sel_scale = g_syms.sel_registerName("scale");
        g_syms.sel_contentScaleFactor = g_syms.sel_registerName("contentScaleFactor");
        g_syms.sel_layer = g_syms.sel_registerName("layer");
        g_syms.sel_renderInContext = g_syms.sel_registerName("renderInContext:");
        g_syms.sel_layoutIfNeeded = g_syms.sel_registerName("layoutIfNeeded");
        g_syms.sel_flush = g_syms.sel_registerName("flush");
        /* Navigation settle. */
        g_syms.sel_rootViewController = g_syms.sel_registerName("rootViewController");
        g_syms.sel_topViewController = g_syms.sel_registerName("topViewController");
        g_syms.sel_presentedViewController = g_syms.sel_registerName("presentedViewController");
        g_syms.sel_selectedViewController = g_syms.sel_registerName("selectedViewController");
        g_syms.sel_view = g_syms.sel_registerName("view");
        g_syms.sel_sublayers = g_syms.sel_registerName("sublayers");
        g_syms.sel_isKindOfClass = g_syms.sel_registerName("isKindOfClass:");
        g_syms.sel_setHidden = g_syms.sel_registerName("setHidden:");
        g_syms.sel_delegate = g_syms.sel_registerName("delegate");
        g_syms.sel_superview = g_syms.sel_registerName("superview");
        g_syms.sel_effect = g_syms.sel_registerName("effect");
        g_syms.sel_style = g_syms.sel_registerName("style");
        g_syms.sel_traitCollection = g_syms.sel_registerName("traitCollection");
        g_syms.sel_userInterfaceStyle = g_syms.sel_registerName("userInterfaceStyle");
    }
    g_syms.object_getClassName =
        (const char *(*)(void *))lsk_sign_fn(snap_dlsym("object_getClassName"));

    return (g_syms.objc_getClass && g_syms.sel_registerName && g_syms.objc_msgSend &&
            g_syms.CGColorSpaceCreateDeviceRGB && g_syms.CGColorSpaceRelease &&
            g_syms.CGBitmapContextCreate && g_syms.CGContextRelease &&
            g_syms.CGContextTranslateCTM && g_syms.CGContextScaleCTM) ? 0 : -1;
}

static int ensure_imageio_syms(void) {
    if (g_syms.imageio_resolved) {
        return (g_syms.CGBitmapContextCreateImage && g_syms.CGImageRelease &&
                g_syms.CFURLCreateFromFileSystemRepresentation &&
                g_syms.CFStringCreateWithCString && g_syms.CFRelease &&
                g_syms.CGImageDestinationCreateWithURL &&
                g_syms.CGImageDestinationAddImage &&
                g_syms.CGImageDestinationFinalize) ? 0 : -1;
    }
    g_syms.imageio_resolved = 1;

    void *cf_url = snap_dlsym("CFURLCreateFromFileSystemRepresentation");
    if (!cf_url) {
        snap_dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
        cf_url = snap_dlsym("CFURLCreateFromFileSystemRepresentation");
    }
    g_syms.CFURLCreateFromFileSystemRepresentation =
        (void *(*)(void *, const uint8_t *, long, uint8_t))lsk_sign_fn(cf_url);
    g_syms.CFStringCreateWithCString =
        (void *(*)(void *, const char *, uint32_t))
        lsk_sign_fn(snap_dlsym("CFStringCreateWithCString"));
    g_syms.CFRelease = (void (*)(void *))lsk_sign_fn(snap_dlsym("CFRelease"));

    void *dest_create = snap_dlsym("CGImageDestinationCreateWithURL");
    if (!dest_create) {
        snap_dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO");
        dest_create = snap_dlsym("CGImageDestinationCreateWithURL");
    }
    g_syms.CGImageDestinationCreateWithURL =
        (void *(*)(void *, void *, size_t, void *))lsk_sign_fn(dest_create);
    g_syms.CGImageDestinationAddImage =
        (void (*)(void *, void *, void *))lsk_sign_fn(snap_dlsym("CGImageDestinationAddImage"));
    g_syms.CGImageDestinationFinalize =
        (uint8_t (*)(void *))lsk_sign_fn(snap_dlsym("CGImageDestinationFinalize"));

    return (g_syms.CGBitmapContextCreateImage && g_syms.CGImageRelease &&
            g_syms.CFURLCreateFromFileSystemRepresentation &&
            g_syms.CFStringCreateWithCString && g_syms.CFRelease &&
            g_syms.CGImageDestinationCreateWithURL &&
            g_syms.CGImageDestinationAddImage &&
            g_syms.CGImageDestinationFinalize) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Window collection & rendering                                      */
/* ------------------------------------------------------------------ */

static void collect_windows_from_array(void *arr, snap_win_entry_t *out,
                                       uint32_t *count, uint32_t max_count) {
    if (!arr || !count) return;
    if (!objc_responds(arr, g_syms.sel_count) ||
        !objc_responds(arr, g_syms.sel_objectAtIndex)) {
        return;
    }
    uint64_t n = msg_u64(arr, g_syms.sel_count);
    for (uint64_t i = 0; i < n && *count < max_count; i++) {
        void *w = msg_id_u64(arr, g_syms.sel_objectAtIndex, i);
        if (!w) continue;

        /* Deduplicate windows returned by both scene.windows and app.windows */
        int dup = 0;
        for (uint32_t j = 0; j < *count; j++) {
            if (out[j].win == w) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        if (objc_responds(w, g_syms.sel_isHidden) && msg_bool(w, g_syms.sel_isHidden)) {
            continue;
        }
        if (objc_responds(w, g_syms.sel_alpha) && msg_f64(w, g_syms.sel_alpha) <= 0.0) {
            continue;
        }
        if (!objc_responds(w, g_syms.sel_bounds)) continue;
        snap_rect_t b = msg_rect(w, g_syms.sel_bounds);
        if (b.size.width <= 0.0 || b.size.height <= 0.0) continue;

        if (!objc_responds(w, g_syms.sel_layer)) continue;
        void *layer = msg_id(w, g_syms.sel_layer);
        if (!layer || !objc_responds(layer, g_syms.sel_renderInContext)) continue;

        double level = 0.0;
        if (objc_responds(w, g_syms.sel_windowLevel)) {
            level = msg_f64(w, g_syms.sel_windowLevel);
        }

        uint32_t idx = *count;
        out[idx].win = w;
        out[idx].level = level;
        out[idx].order = idx;
        (*count)++;
    }
}

static void sort_windows_back_to_front(snap_win_entry_t *entries, uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        snap_win_entry_t key = entries[i];
        uint32_t j = i;
        while (j > 0 &&
               (entries[j - 1].level > key.level ||
                (entries[j - 1].level == key.level && entries[j - 1].order > key.order))) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static double resolve_scale(void *win) {
    if (win && objc_responds(win, g_syms.sel_screen)) {
        void *scr = msg_id(win, g_syms.sel_screen);
        if (scr && objc_responds(scr, g_syms.sel_scale)) {
            double s = msg_f64(scr, g_syms.sel_scale);
            if (s > 0.0) return s;
        }
    }
    if (win && objc_responds(win, g_syms.sel_contentScaleFactor)) {
        double s = msg_f64(win, g_syms.sel_contentScaleFactor);
        if (s > 0.0) return s;
    }
    void *scr_cls = g_syms.objc_getClass("UIScreen");
    if (scr_cls && objc_responds(scr_cls, g_syms.sel_mainScreen)) {
        void *main_scr = msg_id(scr_cls, g_syms.sel_mainScreen);
        if (main_scr && objc_responds(main_scr, g_syms.sel_scale)) {
            double s = msg_f64(main_scr, g_syms.sel_scale);
            if (s > 0.0) return s;
        }
    }
    return SNAP_FALLBACK_SCALE;
}

/* Presenter INFO scale limits: the snapshot must never render at a
 * scale outside this range, whatever the presenter reports. */
#define SNAP_INFO_SCALE_MIN     0.25
#define SNAP_INFO_SCALE_MAX     8.0

/* Frame scale for this tick. The UIScreen chain above was
 * observed returning 1.0 on device (the committed snapshot PNG is
 * 360x800, not 1080x2400): GraphicsServices falls back to scale 1.0
 * when it never got the BKS screen info, and a 1x frame is then
 * upscaled 4x by the presenter, which turns every 1 px artifact
 * into a visible band. The presenter knows the real window size in
 * physical pixels (INFO screen_w/screen_h), so when INFO has arrived
 * and implies a sane scale, it wins: scale = min(screen_w / bounds_w,
 * screen_h / bounds_h), the aspect-fit scale that makes the frame
 * exactly fill the window. With the BKS default at 1440x3200@4 the
 * two sources agree at 4.0; the INFO path is the belt and braces for
 * a lost or late BKS reply. */
static double resolve_frame_scale(void *win, double bounds_w, double bounds_h) {
    double ui_scale = resolve_scale(win);
#ifndef SNAPSHOT_HOST_TEST
    {   /* IOSWINE_SNAPSHOT_SCALE=1..4 forces the render scale (speed vs sharpness). */
        const char *sc = lsk_getenv("IOSWINE_SNAPSHOT_SCALE");
        if (sc && sc[0] >= '1' && sc[0] <= '4' && sc[1] == '\0') return (double)(sc[0] - '0');
    }
#endif
    isws_info_t info;
    double sw_scale, sh_scale, info_scale;
    if (bounds_w <= 0.0 || bounds_h <= 0.0) return ui_scale;
    info = present_last_info();
    if (info.screen_w == 0 || info.screen_h == 0) return ui_scale;
    sw_scale = (double)info.screen_w / bounds_w;
    sh_scale = (double)info.screen_h / bounds_h;
    info_scale = sw_scale < sh_scale ? sw_scale : sh_scale;
    if (info_scale < SNAP_INFO_SCALE_MIN || info_scale > SNAP_INFO_SCALE_MAX) {
        return ui_scale;
    }
    if (info_scale != ui_scale && info_scale != g_logged_info_scale) {
        char nb[24];
        size_t k;
        g_logged_info_scale = info_scale;
        snap_log_str("[snapshot] scale from presenter INFO: ");
        k = fmt_u64(nb, (uint64_t)(info_scale * 1000.0 + 0.5));
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str("/1000 bounds=");
        k = fmt_u64(nb, (uint64_t)(bounds_w + 0.5));
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str("x");
        k = fmt_u64(nb, (uint64_t)(bounds_h + 0.5));
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str(" info=");
        k = fmt_u64(nb, (uint64_t)info.screen_w);
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str("x");
        k = fmt_u64(nb, (uint64_t)info.screen_h);
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str(" (UIScreen gave ");
        k = fmt_u64(nb, (uint64_t)(ui_scale * 1000.0 + 0.5));
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str("/1000)\n");
    }
    return info_scale;
}

static int buf_is_nonempty(const uint8_t *buf, size_t len) {
    /* The canvas is cleared to opaque white (0xFF per byte), so
     * "nonempty" means any byte differs from the clear colour. A
     * frame no layer painted is uniformly white and must not count
     * as content for the one-shot PNG. */
    if (!buf || len == 0) return 0;
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v = 0;
        /* Unaligned-safe load */
        const uint8_t *p = buf + i;
        v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
            ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
            ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
        if (v != 0xFFFFFFFFFFFFFFFFULL) return 1;
        i += 8;
    }
    while (i < len) {
        if (buf[i] != 0xFF) return 1;
        i++;
    }
    return 0;
}

static void write_png_once(void *ctx, const char *path) {
    if (!ctx || !path || !path[0] || g_png_written) return;
    if (ensure_imageio_syms() != 0) {
        snap_log_str("[snapshot] png unavailable (missing ImageIO/CF symbols)\n");
        g_png_written = 1;
        return;
    }

    void *img = g_syms.CGBitmapContextCreateImage(ctx);
    if (!img) return;

    void *url = g_syms.CFURLCreateFromFileSystemRepresentation(
        NULL, (const uint8_t *)path, (long)snap_strlen(path), 0);
    void *uti = g_syms.CFStringCreateWithCString(NULL, "public.png", SNAP_CF_STRING_UTF8);
    void *dest = (url && uti) ? g_syms.CGImageDestinationCreateWithURL(url, uti, 1, NULL) : NULL;

    int ok = 0;
    if (dest) {
        g_syms.CGImageDestinationAddImage(dest, img, NULL);
        ok = g_syms.CGImageDestinationFinalize(dest) ? 1 : 0;
        g_syms.CFRelease(dest);
    }
    if (uti) g_syms.CFRelease(uti);
    if (url) g_syms.CFRelease(url);
    g_syms.CGImageRelease(img);

    if (ok) {
        g_png_written = 1;
        snap_log_str("[snapshot] png written: ");
        snap_log_str(path);
        snap_log_str("\n");
    } else {
        snap_log_str("[snapshot] png write failed: ");
        snap_log_str(path);
        snap_log_str("\n");
    }
}

static uint64_t g_dbg_nscenes = 0xffff;
static void log_no_windows(void) {
    uint64_t now = snap_now_ns();
    if (!g_logged_no_win || (now >= g_last_no_win_ns && now - g_last_no_win_ns >= SNAP_NO_WIN_LOG_NS)) {
        g_logged_no_win = 1;
        g_last_no_win_ns = now;
        { char nb[24]; size_t k = fmt_u64(nb, g_dbg_nscenes); nb[k] = 0;
          snap_log_str("[snapshot] no windows yet, connectedScenes=");
          snap_log_str(nb); snap_log_str("\n"); }
    }
}

/* Commit-before-snapshot: force the layer tree to lay
 * out and commit at the current scale before renderInContext
 * snapshots it. The pump used to render whatever state the tree
 * happened to be in: a text layer whose contents CoreAnimation
 * rasterizes on commit (the search field's placeholder label) can
 * sit uncommitted at the render scale, so renderInContext draws
 * the field's opaque background with no text, while views that
 * draw synchronously in drawRect: (the Library title) still paint.
 * Flushing the transaction and laying out each window first is
 * what drawViewHierarchyInRect:afterScreenUpdates:YES does
 * internally; at the 1 Hz pump rate the cost is one layout pass
 * plus one commit per tick. Both calls are best-effort: if the
 * symbols are missing the tick renders exactly as before. Runs on
 * the main thread (snapshot_do_tick's thread), the only thread
 * CATransaction flush and layoutIfNeeded are valid on. */
static void snap_commit_before_render(snap_win_entry_t *wins, uint32_t n_wins) {
    uint32_t i;
    if (!g_syms.objc_getClass || !g_syms.sel_registerName ||
        !g_syms.objc_msgSend) {
        return;
    }
    if (g_syms.sel_flush) {
        void *tx = g_syms.objc_getClass("CATransaction");
        if (tx && objc_responds(tx, g_syms.sel_flush)) {
            msg_void(tx, g_syms.sel_flush);
        }
    }
    if (g_syms.sel_layoutIfNeeded) {
        for (i = 0; i < n_wins; i++) {
            void *win = wins[i].win;
            if (win && objc_responds(win, g_syms.sel_layoutIfNeeded)) {
                msg_void(win, g_syms.sel_layoutIfNeeded);
            }
        }
    }
}

/* Navigation settle: make a push/pop/present visible
 * in the log and commit what its layout produced, in the same tick.
 *
 * Why this exists: snap_commit_before_render flushes the
 * transaction and then lays out each window. Layout can dirty
 * layers (a newly pushed view controller's view lays out text
 * layers whose contents CoreAnimation rasterizes on commit),
 * but nothing flushes again before renderInContext, so the pushed
 * content is uncommitted at render time and the transition tick
 * shows the pre-transition frame (a one-tick white flash on a
 * UIKit page). The settle below lays out the new top view
 * controller's view and flushes a second time, so the transition
 * tick itself carries the new content.
 *
 * What it does NOT do: it cannot create SwiftUI content. A
 * UIHostingController's view only gains layers when the
 * AttributeGraph runs, and the graph runs on display-link ticks.
 * Until the link fires, a pushed SwiftUI page stays
 * white past the settle; the "[snapshot] nav top=" line is then
 * the proof the push itself completed and the missing piece is
 * the graph, not the transition. Same for a menu dismissal whose
 * UIViewPropertyAnimator never advances without the link: the
 * window stays visible and the settle cannot hide it. Runs on the
 * main thread (snapshot_do_tick's thread). Best-effort throughout:
 * a window with no view controllers, or missing symbols, no-ops. */
static void *g_nav_top_vc;
static char g_nav_top_name[64];
static int g_nav_settle_ticks;

static void snap_nav_copy_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "(unknown)";
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int snap_nav_name_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a) a = "";
    if (!b) b = "";
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* Walk the base window's view-controller chain to the visible top:
 * rootViewController, then repeatedly presentedViewController (a
 * modal, including a modal nav controller), selectedViewController
 * (a UITabBarController's tab) or topViewController (a
 * UINavigationController's top). Capped at 8 steps; every step is
 * respondsToSelector-guarded and NULL-tolerant. */
static void *snap_nav_top_vc(void *base_win) {
    void *vc = NULL;
    int steps;
    if (!base_win || !g_syms.sel_rootViewController) return NULL;
    if (!objc_responds(base_win, g_syms.sel_rootViewController)) return NULL;
    vc = msg_id(base_win, g_syms.sel_rootViewController);
    for (steps = 0; steps < 8 && vc; steps++) {
        void *next = NULL;
        if (g_syms.sel_presentedViewController &&
            objc_responds(vc, g_syms.sel_presentedViewController)) {
            next = msg_id(vc, g_syms.sel_presentedViewController);
        }
        if (!next && g_syms.sel_selectedViewController &&
            objc_responds(vc, g_syms.sel_selectedViewController)) {
            next = msg_id(vc, g_syms.sel_selectedViewController);
        }
        if (!next && g_syms.sel_topViewController &&
            objc_responds(vc, g_syms.sel_topViewController)) {
            next = msg_id(vc, g_syms.sel_topViewController);
        }
        if (!next) break;
        vc = next;
    }
    return vc;
}

static void snap_nav_layout_top_view(void *vc) {
    void *view;
    if (!vc || !g_syms.sel_view || !g_syms.sel_layoutIfNeeded) return;
    if (!objc_responds(vc, g_syms.sel_view)) return;
    view = msg_id(vc, g_syms.sel_view);
    if (view && objc_responds(view, g_syms.sel_layoutIfNeeded)) {
        msg_void(view, g_syms.sel_layoutIfNeeded);
    }
}

static void snap_nav_flush(void) {
    void *tx;
    if (!g_syms.sel_flush) return;
    tx = g_syms.objc_getClass ? g_syms.objc_getClass("CATransaction") : NULL;
    if (tx && objc_responds(tx, g_syms.sel_flush)) {
        msg_void(tx, g_syms.sel_flush);
    }
}

static void snap_nav_settle(void *base_win) {
    void *top = snap_nav_top_vc(base_win);
    char cur_name[64];
    char prev_name[64];
    if (!top) {
        g_nav_settle_ticks = 0;
        return;
    }
    snap_nav_copy_name(cur_name, sizeof(cur_name),
                       g_syms.object_getClassName ?
                       g_syms.object_getClassName(top) : "(unknown)");
    /* Compare by pointer and by name: a popped controller can be
     * deallocated and a new one allocated at the same address, and
     * the pointer alone would then miss the change. */
    if (top != g_nav_top_vc || !snap_nav_name_eq(cur_name, g_nav_top_name)) {
        snap_nav_copy_name(prev_name, sizeof(prev_name),
                           g_nav_top_vc ? g_nav_top_name : "(none)");
        snap_nav_copy_name(g_nav_top_name, sizeof(g_nav_top_name), cur_name);
        g_nav_top_vc = top;
        snap_log_str("[snapshot] nav top=");
        snap_log_str(g_nav_top_name);
        snap_log_str(" prev=");
        snap_log_str(prev_name);
        snap_log_str("\n");
        snap_nav_layout_top_view(top);
        snap_nav_flush();
        g_nav_settle_ticks = 1;
    } else if (g_nav_settle_ticks > 0) {
        g_nav_settle_ticks--;
        snap_nav_layout_top_view(top);
        snap_nav_flush();
    }
}
/* ------------------------------------------------------------------ */
/* Backdrop layers                                      */
/*                                                                    */
/* Mechanism: a UIVisualEffectView's blur is a CABackdropLayer whose  */
/* filters sample the already-composited content beneath it. That     */
/* sampling is a render-server operation: the backdrop filter reads   */
/* the composited scene, not the layer tree. renderInContext: is an   */
/* in-process software rasterizer with no composited backing to       */
/* sample, so on device the backdrop layer emits uninitialised /      */
/* garbage pixels: the grey streaks, and the garbage stripes across   */
/* text drawn over a backdrop (glyph antialiasing blends against      */
/* the garbage beneath it). External confirmation that               */
/* renderInContext: cannot reproduce UIVisualEffectView blur: the     */
/* FBSnapshotTestCase docs say a live screen-based pass               */
/* (drawViewHierarchyInRect:afterScreenUpdates:) is required for      */
/* UIVisualEffectView, and that renderInContext: cannot reproduce    */
/* it. The exact garbage pattern on device is inference from the      */
/* symptom (grey streaks only where effect views are); the fix does   */
/* not depend on the pattern, only on the layer being a backdrop.     */
/*                                                                    */
/* Fix: find every CABackdropLayer in the window layer trees, hide    */
/* it for the render, and put sane pixels in its place:               */
/*   plain (default): a solid fill derived from the effect style      */
/*     (light/dark), painted into the canvas before renderInContext  */
/*     runs, so content below the backdrop still paints over it and   */
/*     content above still paints on top.                             */
/*   sample (IOSWINE_BACKDROP_MODE=sample): two-pass. Pass A renders  */
/*     everything below every backdrop into a scratch buffer, blurs   */
/*     the backdrop rects there, and copies it to the canvas; pass B  */
/*     renders only the content above the backdrops over it. The      */
/*     blur is a separable box blur over the raw BGRA buffer.         */
/* Plain is the default: it costs no extra buffer and no second       */
/* render (the allocator is already the prime suspect in crash A),    */
/* and a solid bar keeps tab-bar labels legible. Sample is the        */
/* opt-in faithful path. Neither mode ever leaves a backdrop          */
/* layer visible to renderInContext: the garbage source is always     */
/* hidden, even if the fill/blur step fails.                          */
/* ------------------------------------------------------------------ */

#define SNAP_BACKDROP_MAX       32u
#define SNAP_BACKDROP_HIDE_MAX  256u
#define SNAP_BACKDROP_DEPTH_MAX 64
#define SNAP_BACKDROP_MODE_PLAIN  0
#define SNAP_BACKDROP_MODE_SAMPLE 1
/* iOS systemGray6: light 242,242,247 / dark 28,28,30. BGRA8888 LE. */
#define SNAP_BACKDROP_FILL_LIGHT 0xFFF2F2F7u
#define SNAP_BACKDROP_FILL_DARK  0xFF1C1C1Eu

typedef struct {
    void *layer;
    snap_rect_t rect;   /* frame in base-window point coordinates */
    uint32_t fill;      /* BGRA plain fill for plain mode */
} snap_backdrop_t;

typedef struct {
    void *layer;
    uint8_t was_hidden;
} snap_hide_rec_t;

static uint8_t *g_scratch_buf;
static size_t g_scratch_size;
static uint8_t *g_blur_tmp;
static size_t g_blur_tmp_size;

static int snap_backdrop_mode(void) {
    const char *m = snap_getenv("IOSWINE_BACKDROP_MODE");
    if (m && m[0] == 's' && m[1] == 'a' && m[2] == 'm' && m[3] == 'p' &&
        m[4] == 'l' && m[5] == 'e' && m[6] == '\0') return SNAP_BACKDROP_MODE_SAMPLE;
    return SNAP_BACKDROP_MODE_PLAIN;
}

/* UIBlurEffectStyle (UIKit, public): 0 extraLight, 1 light, 2 dark,
 * 3 extraDark, 4 regular, 5 prominent, 6 systemUltraThinMaterial,
 * 7 systemThinMaterial, 8 systemMaterial, 9 systemThickMaterial,
 * 10 systemChromeMaterial. Styles 4-10 are adaptive: the trait
 * collection decides. UIUserInterfaceStyle: 1 light, 2 dark. */
static uint32_t snap_backdrop_fill_for_style(int64_t style, int64_t ui_style) {
    int dark = 0;
    if (style == 2 || style == 3) dark = 1;
    else if (style >= 4 && style <= 10) dark = (ui_style == 2);
    else dark = (ui_style == 2);
    return dark ? SNAP_BACKDROP_FILL_DARK : SNAP_BACKDROP_FILL_LIGHT;
}

static uint32_t snap_backdrop_fill(void *layer) {
    const char *ov = snap_getenv("IOSWINE_BACKDROP_FILL");
    void *view, *sup, *fx, *tc;
    int64_t style = -1, ui_style = 0;
    int hops;
    if (ov) {
        if (ov[0] == 'l' && ov[1] == 'i' && ov[2] == 'g' && ov[3] == 'h' &&
            ov[4] == 't' && ov[5] == '\0') return SNAP_BACKDROP_FILL_LIGHT;
        if (ov[0] == 'd' && ov[1] == 'a' && ov[2] == 'r' && ov[3] == 'k' &&
            ov[4] == '\0') return SNAP_BACKDROP_FILL_DARK;
        if (ov[0] && ov[1] && ov[2] && ov[3] && ov[4] && ov[5] && ov[6] == '\0') {
            uint32_t v = 0;
            int i, ok = 1;
            for (i = 0; i < 6; i++) {
                char c = ov[i];
                uint32_t d;
                if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                else { ok = 0; break; }
                v = (v << 4) | d;
            }
            if (ok) return 0xFF000000u | v;
        }
    }
    /* Style chain (best-effort): backdrop layer -> delegate view
     * (_UIBackdropView) -> superviews -> UIVisualEffectView ->
     * effect.style, traitCollection.userInterfaceStyle. Any missing
     * link falls through to the light default. The superview walk
     * checks each level for `effect` instead of assuming a fixed
     * hop count, so a changed internal hierarchy still finds it. */
    if (!layer || !g_syms.sel_delegate || !g_syms.sel_superview ||
        !g_syms.sel_effect || !g_syms.sel_style) return SNAP_BACKDROP_FILL_LIGHT;
    if (!objc_responds(layer, g_syms.sel_delegate)) return SNAP_BACKDROP_FILL_LIGHT;
    view = msg_id(layer, g_syms.sel_delegate);
    if (!view) return SNAP_BACKDROP_FILL_LIGHT;
    sup = view;
    fx = NULL;
    tc = NULL;
    for (hops = 0; hops < 3 && sup; hops++) {
        if (objc_responds(sup, g_syms.sel_effect)) {
            fx = msg_id(sup, g_syms.sel_effect);
            if (fx) {
                if (g_syms.sel_traitCollection &&
                    objc_responds(sup, g_syms.sel_traitCollection)) {
                    tc = msg_id(sup, g_syms.sel_traitCollection);
                }
                break;
            }
        }
        if (!objc_responds(sup, g_syms.sel_superview)) break;
        sup = msg_id(sup, g_syms.sel_superview);
    }
    if (!fx) return SNAP_BACKDROP_FILL_LIGHT;
    if (objc_responds(fx, g_syms.sel_style)) style = (int64_t)msg_u64(fx, g_syms.sel_style);
    if (tc && g_syms.sel_userInterfaceStyle &&
        objc_responds(tc, g_syms.sel_userInterfaceStyle)) {
        ui_style = (int64_t)msg_u64(tc, g_syms.sel_userInterfaceStyle);
    }
    return snap_backdrop_fill_for_style(style, ui_style);
}

static int snap_is_backdrop_layer(void *layer) {
    if (!layer) return 0;
    if (g_syms.sel_isKindOfClass) {
        if (!g_syms.backdrop_cls && g_syms.objc_getClass) {
            g_syms.backdrop_cls = g_syms.objc_getClass("CABackdropLayer");
        }
        if (g_syms.backdrop_cls &&
            objc_responds(layer, g_syms.sel_isKindOfClass)) {
            return msg_bool_id(layer, g_syms.sel_isKindOfClass,
                               g_syms.backdrop_cls) ? 1 : 0;
        }
    }
    /* Fallback: class name via libobjc class_getName (plain C). Only
     * reached when the CABackdropLayer class object is unavailable. */
    if (!g_syms.class_getName) {
        g_syms.class_getName =
            (const char *(*)(void *))lsk_sign_fn(snap_dlsym("class_getName"));
    }
    if (g_syms.class_getName) {
        void *(*ogc)(void *) =
            (void *(*)(void *))lsk_sign_fn(snap_dlsym("object_getClass"));
        if (ogc) {
            const char *nm = g_syms.class_getName(ogc(layer));
            const char *want = "CABackdropLayer";
            size_t i = 0;
            if (nm) {
                while (nm[i] && nm[i] == want[i]) i++;
                if (nm[i] == '\0' && want[i] == '\0') return 1;
            }
        }
    }
    return 0;
}

/* Walk one layer's sublayers in paint order, recording backdrop
 * layers with their frames translated into base-window point
 * coordinates (ox/oy is the accumulated superlayer offset). A
 * backdrop layer's own sublayers are not walked: it has no
 * paintable content of its own. Already-hidden layers paint
 * nothing, so a hidden backdrop produces no garbage and is skipped
 * (its subtree is skipped too). Depth-capped against a
 * pathological tree. */
static void snap_collect_backdrops_rec(void *layer, double ox, double oy,
                                       snap_backdrop_t *out, uint32_t *count,
                                       uint32_t max, int depth) {
    void *subs;
    uint64_t n, i;
    if (!layer || depth > SNAP_BACKDROP_DEPTH_MAX) return;
    if (!g_syms.sel_sublayers || !g_syms.sel_count ||
        !g_syms.sel_objectAtIndex) return;
    if (!objc_responds(layer, g_syms.sel_sublayers)) return;
    subs = msg_id(layer, g_syms.sel_sublayers);
    if (!subs || !objc_responds(subs, g_syms.sel_count) ||
        !objc_responds(subs, g_syms.sel_objectAtIndex)) return;
    n = msg_u64(subs, g_syms.sel_count);
    for (i = 0; i < n; i++) {
        void *sub = msg_id_u64(subs, g_syms.sel_objectAtIndex, i);
        snap_rect_t fr;
        double nx, ny;
        if (!sub) continue;
        if (!objc_responds(sub, g_syms.sel_frame)) continue;
        fr = msg_rect(sub, g_syms.sel_frame);
        if (fr.size.width <= 0.0 || fr.size.height <= 0.0) continue;
        nx = ox + fr.origin.x;
        ny = oy + fr.origin.y;
        if (objc_responds(sub, g_syms.sel_isHidden) &&
            msg_bool(sub, g_syms.sel_isHidden)) continue;
        if (snap_is_backdrop_layer(sub)) {
            uint32_t idx;
            if (*count >= max) continue;
            idx = *count;
            out[idx].layer = sub;
            out[idx].rect.origin.x = nx;
            out[idx].rect.origin.y = ny;
            out[idx].rect.size = fr.size;
            out[idx].fill = snap_backdrop_fill(sub);
            (*count)++;
            continue;
        }
        snap_collect_backdrops_rec(sub, nx, ny, out, count, max, depth + 1);
    }
}

/* Hide one layer, recording the prior state for restore. A layer
 * that is already hidden needs no change. Returns -1 when the
 * record array is full. */
static int snap_backdrop_hide_one(void *layer, snap_hide_rec_t *recs,
                                  uint32_t *nrecs, uint32_t maxrecs) {
    uint8_t was = 0;
    if (*nrecs >= maxrecs) return -1;
    if (objc_responds(layer, g_syms.sel_isHidden)) {
        was = msg_bool(layer, g_syms.sel_isHidden);
    }
    if (was) return 0;
    if (!g_syms.sel_setHidden) return 0;
    recs[*nrecs].layer = layer;
    recs[*nrecs].was_hidden = 0;
    (*nrecs)++;
    msg_void_u8(layer, g_syms.sel_setHidden, 1);
    return 0;
}

static void snap_backdrop_restore(snap_hide_rec_t *recs, uint32_t nrecs) {
    uint32_t i;
    if (!g_syms.sel_setHidden) return;
    for (i = 0; i < nrecs; i++) {
        msg_void_u8(recs[i].layer, g_syms.sel_setHidden, recs[i].was_hidden);
    }
}

/* Recursive pass over one parent's sublayers.
 * pass 0 (beneath): hide backdrop layers and every layer above one.
 * pass 1 (above): hide backdrop layers and every layer below one.
 * `above` is inherited: a descendant of an above layer is above.
 * Within one parent's list, `seen` flips after the first sublayer
 * whose subtree contains a backdrop, so later siblings are above.
 * Returns 1 if this subtree contains a backdrop. */
static int snap_backdrop_classify_rec(void *layer, int above, int pass,
                                      snap_hide_rec_t *recs, uint32_t *nrecs,
                                      uint32_t maxrecs, int *overflow,
                                      int depth) {
    void *subs;
    uint64_t n, i;
    int seen = 0;
    int has = 0;
    if (!layer || depth > SNAP_BACKDROP_DEPTH_MAX) return 0;
    if (!g_syms.sel_sublayers || !g_syms.sel_count ||
        !g_syms.sel_objectAtIndex) return 0;
    if (!objc_responds(layer, g_syms.sel_sublayers)) return 0;
    subs = msg_id(layer, g_syms.sel_sublayers);
    if (!subs || !objc_responds(subs, g_syms.sel_count) ||
        !objc_responds(subs, g_syms.sel_objectAtIndex)) return 0;
    n = msg_u64(subs, g_syms.sel_count);
    for (i = 0; i < n; i++) {
        void *sub = msg_id_u64(subs, g_syms.sel_objectAtIndex, i);
        int is_bd, child_above, child_has;
        if (!sub) continue;
        if (objc_responds(sub, g_syms.sel_isHidden) &&
            msg_bool(sub, g_syms.sel_isHidden)) continue;
        is_bd = snap_is_backdrop_layer(sub);
        child_above = above || seen;
        if (is_bd) {
            has = 1;
            seen = 1;
            if (snap_backdrop_hide_one(sub, recs, nrecs, maxrecs) != 0) {
                *overflow = 1;
            }
            continue;
        }
        child_has = snap_backdrop_classify_rec(sub, child_above, pass, recs,
                                               nrecs, maxrecs, overflow,
                                               depth + 1);
        if (child_has) {
            has = 1;
            seen = 1;
        }
        if (pass == 0) {
            if (child_above &&
                snap_backdrop_hide_one(sub, recs, nrecs, maxrecs) != 0) {
                *overflow = 1;
            }
        } else {
            if (!child_above &&
                snap_backdrop_hide_one(sub, recs, nrecs, maxrecs) != 0) {
                *overflow = 1;
            }
        }
    }
    return has;
}

/* Convert a point rect (base-window coordinates) to clamped buffer
 * pixel columns/rows. Buffer rows are bottom-origin (row 0 = CG
 * y 0), so UIKit y maps to row h - y*scale. */
static void snap_buf_rect(snap_rect_t r, double scale, uint32_t w, uint32_t h,
                          int *x0, int *y0, int *x1, int *y1) {
    int cx0 = (int)(r.origin.x * scale);
    int cy0 = (int)(r.origin.y * scale);
    int cx1 = (int)((r.origin.x + r.size.width) * scale + 0.5);
    int cy1 = (int)((r.origin.y + r.size.height) * scale + 0.5);
    int rx0 = cx0, ry0 = (int)h - cy1, rx1 = cx1, ry1 = (int)h - cy0;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > (int)w) rx1 = (int)w;
    if (ry1 > (int)h) ry1 = (int)h;
    *x0 = rx0;
    *y0 = ry0;
    *x1 = rx1;
    *y1 = ry1;
}

static void snap_fill_buf_rect(uint8_t *buf, uint32_t w, uint32_t h,
                               uint32_t stride, snap_rect_t r, double scale,
                               uint32_t px) {
    int x0, y0, x1, y1, x, y;
    uint32_t *rows;
    if (!buf) return;
    snap_buf_rect(r, scale, w, h, &x0, &y0, &x1, &y1);
    rows = (uint32_t *)buf;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            rows[(size_t)y * (stride / 4) + (size_t)x] = px;
        }
    }
}

static int snap_blur_tmp_ensure(size_t bytes) {
    if (bytes <= g_blur_tmp_size) return 0;
    if (g_blur_tmp) {
        snap_free_buf(g_blur_tmp, g_blur_tmp_size);
        g_blur_tmp = NULL;
        g_blur_tmp_size = 0;
    }
    g_blur_tmp = (uint8_t *)snap_alloc_buf(bytes);
    if (!g_blur_tmp) return -1;
    g_blur_tmp_size = bytes;
    return 0;
}

static int snap_backdrop_ensure_scratch(size_t size) {
    if (size == 0) return -1;
    if (size <= g_scratch_size) return 0;
    if (g_scratch_buf) {
        snap_free_buf(g_scratch_buf, g_scratch_size);
        g_scratch_buf = NULL;
        g_scratch_size = 0;
    }
    g_scratch_buf = (uint8_t *)snap_alloc_buf(size);
    if (!g_scratch_buf) return -1;
    g_scratch_size = size;
    return 0;
}

/* Separable box blur over one buffer rect, channel-wise, sliding
 * window (radius r, window 2r+1, edges clamped to the buffer). The
 * window samples the full buffer, not just the rect: a real
 * backdrop blurs the content around its edge too. Horizontal pass
 * blurs each row through a temp line buffer holding the full row,
 * vertical pass each column through a temp buffer holding the full
 * column. Only pixels inside the rect are written back. */
static void snap_box_blur_rect(uint8_t *buf, uint32_t w, uint32_t h,
                               uint32_t stride, snap_rect_t r, double scale,
                               int radius) {
    int x0, y0, x1, y1;
    uint32_t *px;
    uint32_t row_px;
    int x, y;
    size_t need;
    if (!buf || radius < 1) return;
    snap_buf_rect(r, scale, w, h, &x0, &y0, &x1, &y1);
    if (x1 - x0 < 2 || y1 - y0 < 2) return;
    need = (size_t)(w > h ? w : h) * sizeof(uint32_t);
    if (snap_blur_tmp_ensure(need) != 0) return;
    px = (uint32_t *)buf;
    row_px = stride / 4;
    for (y = y0; y < y1; y++) {
        uint32_t *tmp = (uint32_t *)g_blur_tmp;
        uint32_t *row = px + (size_t)y * row_px;
        int64_t sum[4] = { 0, 0, 0, 0 };
        int i;
        int win = 2 * radius + 1;
        for (i = 0; i < (int)w; i++) tmp[i] = row[i];
        for (i = -radius; i <= radius; i++) {
            int sx = x0 + i;
            uint32_t v;
            if (sx < 0) sx = 0;
            if (sx >= (int)w) sx = (int)w - 1;
            v = tmp[sx];
            sum[0] += (int64_t)((v >> 0) & 0xFF);
            sum[1] += (int64_t)((v >> 8) & 0xFF);
            sum[2] += (int64_t)((v >> 16) & 0xFF);
            sum[3] += (int64_t)((v >> 24) & 0xFF);
        }
        for (x = x0; x < x1; x++) {
            uint32_t out = ((uint32_t)(sum[3] / win) << 24) |
                           ((uint32_t)(sum[2] / win) << 16) |
                           ((uint32_t)(sum[1] / win) << 8) |
                           (uint32_t)(sum[0] / win);
            int xo = x - radius, xi = x + radius + 1, co, ci;
            uint32_t vo, vi;
            row[x] = out;
            co = xo < 0 ? 0 : (xo >= (int)w ? (int)w - 1 : xo);
            ci = xi < 0 ? 0 : (xi >= (int)w ? (int)w - 1 : xi);
            vo = tmp[co];
            vi = tmp[ci];
            sum[0] += (int64_t)((vi >> 0) & 0xFF) - (int64_t)((vo >> 0) & 0xFF);
            sum[1] += (int64_t)((vi >> 8) & 0xFF) - (int64_t)((vo >> 8) & 0xFF);
            sum[2] += (int64_t)((vi >> 16) & 0xFF) - (int64_t)((vo >> 16) & 0xFF);
            sum[3] += (int64_t)((vi >> 24) & 0xFF) - (int64_t)((vo >> 24) & 0xFF);
        }
    }
    for (x = x0; x < x1; x++) {
        uint32_t *tmp = (uint32_t *)g_blur_tmp;
        int64_t sum[4] = { 0, 0, 0, 0 };
        int i;
        int win = 2 * radius + 1;
        for (i = 0; i < (int)h; i++) tmp[i] = px[(size_t)i * row_px + x];
        for (i = -radius; i <= radius; i++) {
            int sy = y0 + i;
            uint32_t v;
            if (sy < 0) sy = 0;
            if (sy >= (int)h) sy = (int)h - 1;
            v = tmp[sy];
            sum[0] += (int64_t)((v >> 0) & 0xFF);
            sum[1] += (int64_t)((v >> 8) & 0xFF);
            sum[2] += (int64_t)((v >> 16) & 0xFF);
            sum[3] += (int64_t)((v >> 24) & 0xFF);
        }
        for (y = y0; y < y1; y++) {
            uint32_t out = ((uint32_t)(sum[3] / win) << 24) |
                           ((uint32_t)(sum[2] / win) << 16) |
                           ((uint32_t)(sum[1] / win) << 8) |
                           (uint32_t)(sum[0] / win);
            int yo = y - radius, yi = y + radius + 1, co, ci;
            uint32_t vo, vi;
            px[(size_t)y * row_px + x] = out;
            co = yo < 0 ? 0 : (yo >= (int)h ? (int)h - 1 : yo);
            ci = yi < 0 ? 0 : (yi >= (int)h ? (int)h - 1 : yi);
            vo = tmp[co];
            vi = tmp[ci];
            sum[0] += (int64_t)((vi >> 0) & 0xFF) - (int64_t)((vo >> 0) & 0xFF);
            sum[1] += (int64_t)((vi >> 8) & 0xFF) - (int64_t)((vo >> 8) & 0xFF);
            sum[2] += (int64_t)((vi >> 16) & 0xFF) - (int64_t)((vo >> 16) & 0xFF);
            sum[3] += (int64_t)((vi >> 24) & 0xFF) - (int64_t)((vo >> 24) & 0xFF);
        }
    }
}

#ifndef SNAPSHOT_HOST_TEST
/* Solarium (iOS 26 Liquid Glass) software-render support for renderInContext:.
 *
 * UIKit's Solarium bars (floating tab bar, glass nav bar buttons) render two
 * layers that CGContext renderInContext: ignores:
 *   1. CASDFLayer / CABackdropLayer ("glassBackground") overlays.
 *   2. _UIMultiLayer subtrees with a "vibrantColorMatrix" CAFilter whose
 *      foreground icons (CGImage + white contentsMultiplyColor) and labels
 *      (_UILabelLayer + CATintedImage) are emitted in white for GPU vibrancy
 *      compositing, rendering invisible against a light white canvas.
 *
 * We temporarily hide unrenderable SDF/destOut overlays, tint white vibrant
 * foregrounds to dark ink, and apply a subtle translucent pill backing to the
 * glass capsule layers, then restore every modified property after rendering. */
#define SNAP_GLASS_MAX 128u

enum {
    SNAP_GL_MUL = 1,
    SNAP_GL_TINT = 2,
    SNAP_GL_STYLE = 3
};

typedef struct {
    void *target;
    int kind;
    void *old_color;
    void *old_border_color;
    double old_cr;
    double old_bw;
} snap_glass_rec_t;

typedef struct {
    int ready;
    void *sel_filters;
    void *sel_name;
    void *sel_utf8;
    void *sel_cmp;
    void *sel_bg;
    void *sel_set_bg;
    void *sel_cr;
    void *sel_set_cr;
    void *sel_bw;
    void *sel_set_bw;
    void *sel_bc;
    void *sel_set_bc;
    void *sel_mtb;
    void *sel_cnt;
    void *sel_cmc;
    void *sel_set_cmc;
    void *sel_tint;
    void *sel_set_tint;
    void *(*cg_color_create)(void *space, const double *comp);
    void (*cg_color_release)(void *c);
    void *(*cf_retain)(void *cf);
    size_t (*cg_ncomp)(void *c);
    const double *(*cg_comp)(void *c);
} snap_glass_syms_t;

static snap_glass_syms_t g_gl;

static int snap_glass_ensure_syms(void) {
    if (g_gl.ready) return 0;
    g_gl.sel_filters = g_syms.sel_registerName("filters");
    g_gl.sel_name = g_syms.sel_registerName("name");
    g_gl.sel_utf8 = g_syms.sel_registerName("UTF8String");
    g_gl.sel_cmp = g_syms.sel_registerName("compositingFilter");
    g_gl.sel_bg = g_syms.sel_registerName("backgroundColor");
    g_gl.sel_set_bg = g_syms.sel_registerName("setBackgroundColor:");
    g_gl.sel_cr = g_syms.sel_registerName("cornerRadius");
    g_gl.sel_set_cr = g_syms.sel_registerName("setCornerRadius:");
    g_gl.sel_bw = g_syms.sel_registerName("borderWidth");
    g_gl.sel_set_bw = g_syms.sel_registerName("setBorderWidth:");
    g_gl.sel_bc = g_syms.sel_registerName("borderColor");
    g_gl.sel_set_bc = g_syms.sel_registerName("setBorderColor:");
    g_gl.sel_mtb = g_syms.sel_registerName("masksToBounds");
    g_gl.sel_cnt = g_syms.sel_registerName("contents");
    g_gl.sel_cmc = g_syms.sel_registerName("contentsMultiplyColor");
    g_gl.sel_set_cmc = g_syms.sel_registerName("setContentsMultiplyColor:");
    g_gl.sel_tint = g_syms.sel_registerName("tint");
    g_gl.sel_set_tint = g_syms.sel_registerName("setTint:");
    g_gl.cg_color_create = (void *(*)(void *, const double *))lsk_sign_fn(snap_dlsym("CGColorCreate"));
    g_gl.cg_color_release = (void (*)(void *))lsk_sign_fn(snap_dlsym("CGColorRelease"));
    g_gl.cf_retain = (void *(*)(void *))lsk_sign_fn(snap_dlsym("CFRetain"));
    g_gl.cg_ncomp = (size_t (*)(void *))lsk_sign_fn(snap_dlsym("CGColorGetNumberOfComponents"));
    g_gl.cg_comp = (const double *(*)(void *))lsk_sign_fn(snap_dlsym("CGColorGetComponents"));
    if (!g_gl.cg_color_create || !g_gl.cg_color_release || !g_gl.cf_retain) return -1;
    g_gl.ready = 1;
    return 0;
}

static int snap_cstr_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static const char *snap_nsstr_cstr(void *obj) {
    if (!obj || !g_gl.sel_utf8 || !objc_responds(obj, g_gl.sel_utf8)) return NULL;
    return ((const char *(*)(void *, void *))g_syms.objc_msgSend)(obj, g_gl.sel_utf8);
}

static const char *snap_cmp_name(void *layer) {
    if (!layer || !g_gl.sel_cmp || !objc_responds(layer, g_gl.sel_cmp)) return NULL;
    void *cmp = msg_id(layer, g_gl.sel_cmp);
    if (!cmp) return NULL;
    if (g_gl.sel_name && objc_responds(cmp, g_gl.sel_name)) {
        return snap_nsstr_cstr(msg_id(cmp, g_gl.sel_name));
    }
    return snap_nsstr_cstr(cmp);
}

static int snap_has_filter_name(void *layer, const char *want) {
    if (!layer || !g_gl.sel_filters || !objc_responds(layer, g_gl.sel_filters)) return 0;
    void *flt = msg_id(layer, g_gl.sel_filters);
    if (!flt || !objc_responds(flt, g_syms.sel_count) || !objc_responds(flt, g_syms.sel_objectAtIndex)) return 0;
    uint64_t n = msg_u64(flt, g_syms.sel_count);
    for (uint64_t i = 0; i < n; i++) {
        void *f = msg_id_u64(flt, g_syms.sel_objectAtIndex, i);
        if (f && g_gl.sel_name && objc_responds(f, g_gl.sel_name)) {
            const char *nm = snap_nsstr_cstr(msg_id(f, g_gl.sel_name));
            if (snap_cstr_eq(nm, want)) return 1;
        }
    }
    return 0;
}

static int snap_is_sdf_layer(void *layer) {
    const char *cls = (layer && g_syms.object_getClassName) ? g_syms.object_getClassName(layer) : NULL;
    return (cls && cls[0] == 'C' && cls[1] == 'A' && cls[2] == 'S' &&
            cls[3] == 'D' && cls[4] == 'F') ? 1 : 0;
}

static int snap_subtree_has_glass_bd(void *layer, int depth) {
    if (!layer || depth > 8) return 0;
    if (snap_is_backdrop_layer(layer) && snap_has_filter_name(layer, "glassBackground")) return 1;
    void *subs = objc_responds(layer, g_syms.sel_sublayers) ? msg_id(layer, g_syms.sel_sublayers) : NULL;
    uint64_t n = (subs && objc_responds(subs, g_syms.sel_count)) ? msg_u64(subs, g_syms.sel_count) : 0;
    if (!subs || !objc_responds(subs, g_syms.sel_objectAtIndex)) return 0;
    for (uint64_t i = 0; i < n; i++) {
        if (snap_subtree_has_glass_bd(msg_id_u64(subs, g_syms.sel_objectAtIndex, i), depth + 1)) return 1;
    }
    return 0;
}

static int snap_subtree_has_vibrant(void *layer, int depth) {
    if (!layer || depth > 10) return 0;
    if (snap_has_filter_name(layer, "vibrantColorMatrix")) return 1;
    void *subs = objc_responds(layer, g_syms.sel_sublayers) ? msg_id(layer, g_syms.sel_sublayers) : NULL;
    uint64_t n = (subs && objc_responds(subs, g_syms.sel_count)) ? msg_u64(subs, g_syms.sel_count) : 0;
    if (!subs || !objc_responds(subs, g_syms.sel_objectAtIndex)) return 0;
    for (uint64_t i = 0; i < n; i++) {
        if (snap_subtree_has_vibrant(msg_id_u64(subs, g_syms.sel_objectAtIndex, i), depth + 1)) return 1;
    }
    return 0;
}

static double snap_find_bd_cr(void *layer, int depth) {
    if (!layer || depth > 6) return 0.0;
    if (snap_is_backdrop_layer(layer) && objc_responds(layer, g_gl.sel_cr)) {
        double cr = msg_f64(layer, g_gl.sel_cr);
        if (cr > 0.5) return cr;
    }
    void *subs = objc_responds(layer, g_syms.sel_sublayers) ? msg_id(layer, g_syms.sel_sublayers) : NULL;
    uint64_t n = (subs && objc_responds(subs, g_syms.sel_count)) ? msg_u64(subs, g_syms.sel_count) : 0;
    if (!subs || !objc_responds(subs, g_syms.sel_objectAtIndex)) return 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double cr = snap_find_bd_cr(msg_id_u64(subs, g_syms.sel_objectAtIndex, i), depth + 1);
        if (cr > 0.5) return cr;
    }
    return 0.0;
}

static int snap_is_near_white(void *c) {
    if (!c || !g_gl.cg_ncomp || !g_gl.cg_comp) return 0;
    size_t nc = g_gl.cg_ncomp(c);
    const double *v = g_gl.cg_comp(c);
    if (!v) return 0;
    if (nc == 2) return (v[0] > 0.80 && v[1] > 0.40);
    if (nc >= 4) return (v[0] > 0.80 && v[1] > 0.80 && v[2] > 0.80 && v[3] > 0.40);
    return 0;
}

static void snap_glass_save_style(void *layer, snap_glass_rec_t *recs, uint32_t *nrecs,
                                  void *bg, double cr, double bw, void *bc) {
    if (*nrecs >= SNAP_GLASS_MAX) return;
    void *old_bg = objc_responds(layer, g_gl.sel_bg) ? msg_id(layer, g_gl.sel_bg) : NULL;
    void *old_bc = objc_responds(layer, g_gl.sel_bc) ? msg_id(layer, g_gl.sel_bc) : NULL;
    double old_cr = objc_responds(layer, g_gl.sel_cr) ? msg_f64(layer, g_gl.sel_cr) : 0.0;
    double old_bw = objc_responds(layer, g_gl.sel_bw) ? msg_f64(layer, g_gl.sel_bw) : 0.0;
    recs[*nrecs].target = layer;
    recs[*nrecs].kind = SNAP_GL_STYLE;
    recs[*nrecs].old_color = old_bg ? g_gl.cf_retain(old_bg) : NULL;
    recs[*nrecs].old_border_color = old_bc ? g_gl.cf_retain(old_bc) : NULL;
    recs[*nrecs].old_cr = old_cr;
    recs[*nrecs].old_bw = old_bw;
    (*nrecs)++;
    if (bg && objc_responds(layer, g_gl.sel_set_bg)) msg_void_id(layer, g_gl.sel_set_bg, bg);
    if (cr >= 0.0 && objc_responds(layer, g_gl.sel_set_cr)) {
        ((void (*)(void *, void *, double))g_syms.objc_msgSend)(layer, g_gl.sel_set_cr, cr);
    }
    if (bw >= 0.0 && objc_responds(layer, g_gl.sel_set_bw)) {
        ((void (*)(void *, void *, double))g_syms.objc_msgSend)(layer, g_gl.sel_set_bw, bw);
    }
    if (bc && objc_responds(layer, g_gl.sel_set_bc)) msg_void_id(layer, g_gl.sel_set_bc, bc);
}

static void snap_solarium_walk(void *layer, int in_glass, int in_vibrant, int pill_done,
                               snap_hide_rec_t *hrecs, uint32_t *nhrecs,
                               snap_glass_rec_t *grecs, uint32_t *ngrecs,
                               void *ink, void *pill_bg, void *sel_bg, void *border_bg,
                               int depth) {
    if (!layer || depth > 40) return;
    if (objc_responds(layer, g_syms.sel_isHidden) && msg_bool(layer, g_syms.sel_isHidden)) return;
    if (snap_is_sdf_layer(layer)) {
        snap_backdrop_hide_one(layer, hrecs, nhrecs, SNAP_BACKDROP_HIDE_MAX);
        return;
    }
    void *subs = objc_responds(layer, g_syms.sel_sublayers) ? msg_id(layer, g_syms.sel_sublayers) : NULL;
    uint64_t n = (subs && objc_responds(subs, g_syms.sel_count)) ? msg_u64(subs, g_syms.sel_count) : 0;
    const char *cmp_nm = snap_cmp_name(layer);
    if (cmp_nm) {
        if (snap_cstr_eq(cmp_nm, "destOut")) {
            snap_backdrop_hide_one(layer, hrecs, nhrecs, SNAP_BACKDROP_HIDE_MAX);
            return;
        }
        if (subs && objc_responds(subs, g_syms.sel_objectAtIndex)) {
            for (uint64_t i = 0; i < n; i++) {
                void *sub = msg_id_u64(subs, g_syms.sel_objectAtIndex, i);
                if (snap_is_backdrop_layer(sub) || snap_is_sdf_layer(sub)) {
                    snap_backdrop_hide_one(layer, hrecs, nhrecs, SNAP_BACKDROP_HIDE_MAX);
                    return;
                }
            }
        }
    }

    const char *cls = g_syms.object_getClassName ? g_syms.object_getClassName(layer) : NULL;
    int enter_glass = in_glass;
    if (!in_glass && snap_cstr_eq(cls, "_UIMultiLayer") && snap_subtree_has_glass_bd(layer, 0)) {
        enter_glass = 1;
    }
    int next_vibrant = in_vibrant || snap_has_filter_name(layer, "vibrantColorMatrix");

    if (in_vibrant) {
        if (objc_responds(layer, g_gl.sel_cmc) && objc_responds(layer, g_gl.sel_set_cmc)) {
            void *cmc = msg_id(layer, g_gl.sel_cmc);
            if (snap_is_near_white(cmc) && *ngrecs < SNAP_GLASS_MAX) {
                grecs[*ngrecs].target = layer;
                grecs[*ngrecs].kind = SNAP_GL_MUL;
                grecs[*ngrecs].old_color = g_gl.cf_retain(cmc);
                (*ngrecs)++;
                msg_void_id(layer, g_gl.sel_set_cmc, ink);
            }
        }
        if (snap_cstr_eq(cls, "_UILabelLayer") && objc_responds(layer, g_gl.sel_cnt)) {
            void *cnt = msg_id(layer, g_gl.sel_cnt);
            if (cnt && objc_responds(cnt, g_gl.sel_tint) && objc_responds(cnt, g_gl.sel_set_tint)) {
                void *old_t = msg_id(cnt, g_gl.sel_tint);
                if ((!old_t || snap_is_near_white(old_t)) && *ngrecs < SNAP_GLASS_MAX) {
                    grecs[*ngrecs].target = cnt;
                    grecs[*ngrecs].kind = SNAP_GL_TINT;
                    grecs[*ngrecs].old_color = old_t ? g_gl.cf_retain(old_t) : NULL;
                    (*ngrecs)++;
                    msg_void_id(cnt, g_gl.sel_set_tint, ink);
                }
            }
        }
    }

    int next_pill_done = pill_done;
    if (enter_glass) {
        if (snap_has_filter_name(layer, "opacityPair")) {
            double bcr = snap_find_bd_cr(layer, 0);
            if (bcr > 0.5) {
                snap_glass_save_style(layer, grecs, ngrecs, sel_bg, bcr, -1.0, NULL);
            }
        } else if (!pill_done) {
            void *cur_bg = objc_responds(layer, g_gl.sel_bg) ? msg_id(layer, g_gl.sel_bg) : NULL;
            double cur_cr = objc_responds(layer, g_gl.sel_cr) ? msg_f64(layer, g_gl.sel_cr) : 0.0;
            uint8_t cur_mtb = objc_responds(layer, g_gl.sel_mtb) ? msg_bool(layer, g_gl.sel_mtb) : 0;
            snap_rect_t fr = objc_responds(layer, g_syms.sel_frame) ? msg_rect(layer, g_syms.sel_frame)
                                                                     : (snap_rect_t){{0, 0}, {0, 0}};
            if (!cur_bg && cur_mtb && cur_cr >= 10.0 && fr.size.width >= 30.0 && fr.size.height >= 30.0) {
                snap_glass_save_style(layer, grecs, ngrecs, pill_bg, cur_cr, 0.75, border_bg);
                next_pill_done = 1;
            } else if (!cur_bg && fr.size.width >= 44.0 && fr.size.width < 350.0 &&
                       fr.size.height >= 44.0 && fr.size.height <= 80.0 &&
                       snap_subtree_has_vibrant(layer, 0)) {
                snap_glass_save_style(layer, grecs, ngrecs, pill_bg, fr.size.height * 0.5, 0.75, border_bg);
                next_pill_done = 1;
            }
        }
    }

    if (!subs || !objc_responds(subs, g_syms.sel_objectAtIndex)) return;
    /* Check if this parent has a destOut punchout sibling; if so, also hide the
     * preceding duplicate highlighted tab layer so labels do not double-draw. */
    void *dup_skip = NULL;
    if (enter_glass && n >= 4) {
        void *last = msg_id_u64(subs, g_syms.sel_objectAtIndex, n - 1);
        const char *lcmp = snap_cmp_name(last);
        if (snap_cstr_eq(lcmp, "destOut")) {
            dup_skip = msg_id_u64(subs, g_syms.sel_objectAtIndex, n - 2);
        }
    }
    for (uint64_t i = 0; i < n; i++) {
        void *sub = msg_id_u64(subs, g_syms.sel_objectAtIndex, i);
        if (sub == dup_skip) {
            snap_backdrop_hide_one(sub, hrecs, nhrecs, SNAP_BACKDROP_HIDE_MAX);
            continue;
        }
        snap_solarium_walk(sub, enter_glass, next_vibrant, next_pill_done,
                           hrecs, nhrecs, grecs, ngrecs,
                           ink, pill_bg, sel_bg, border_bg, depth + 1);
    }
}

static void snap_solarium_prepare(snap_win_entry_t *wins, uint32_t n_wins, void *cs,
                                  snap_hide_rec_t *hrecs, uint32_t *nhrecs,
                                  snap_glass_rec_t *grecs, uint32_t *ngrecs) {
    if (!cs || snap_glass_ensure_syms() != 0) return;
    const double ink_c[4] = { 0.11, 0.11, 0.12, 1.0 };
    const double pill_c[4] = { 0.95, 0.95, 0.97, 0.94 };
    const double sel_c[4] = { 0.47, 0.47, 0.50, 0.14 };
    const double brd_c[4] = { 0.0, 0.0, 0.0, 0.10 };
    void *ink = g_gl.cg_color_create(cs, ink_c);
    void *pill_bg = g_gl.cg_color_create(cs, pill_c);
    void *sel_bg = g_gl.cg_color_create(cs, sel_c);
    void *border_bg = g_gl.cg_color_create(cs, brd_c);
    if (ink && pill_bg && sel_bg && border_bg) {
        for (uint32_t i = 0; i < n_wins; i++) {
            void *layer = msg_id(wins[i].win, g_syms.sel_layer);
            if (layer) {
                snap_solarium_walk(layer, 0, 0, 0, hrecs, nhrecs, grecs, ngrecs,
                                   ink, pill_bg, sel_bg, border_bg, 0);
            }
        }
    }
    if (ink) g_gl.cg_color_release(ink);
    if (pill_bg) g_gl.cg_color_release(pill_bg);
    if (sel_bg) g_gl.cg_color_release(sel_bg);
    if (border_bg) g_gl.cg_color_release(border_bg);
}

static void snap_solarium_restore(snap_glass_rec_t *grecs, uint32_t ngrecs) {
    for (uint32_t i = ngrecs; i > 0; i--) {
        snap_glass_rec_t *r = &grecs[i - 1];
        if (r->kind == SNAP_GL_MUL) {
            msg_void_id(r->target, g_gl.sel_set_cmc, r->old_color);
            if (r->old_color) g_gl.cg_color_release(r->old_color);
        } else if (r->kind == SNAP_GL_TINT) {
            msg_void_id(r->target, g_gl.sel_set_tint, r->old_color);
            if (r->old_color) g_gl.cg_color_release(r->old_color);
        } else if (r->kind == SNAP_GL_STYLE) {
            if (objc_responds(r->target, g_gl.sel_set_bg))
                msg_void_id(r->target, g_gl.sel_set_bg, r->old_color);
            if (objc_responds(r->target, g_gl.sel_set_bc))
                msg_void_id(r->target, g_gl.sel_set_bc, r->old_border_color);
            if (objc_responds(r->target, g_gl.sel_set_cr))
                ((void (*)(void *, void *, double))g_syms.objc_msgSend)(r->target, g_gl.sel_set_cr, r->old_cr);
            if (objc_responds(r->target, g_gl.sel_set_bw))
                ((void (*)(void *, void *, double))g_syms.objc_msgSend)(r->target, g_gl.sel_set_bw, r->old_bw);
            if (r->old_color) g_gl.cg_color_release(r->old_color);
            if (r->old_border_color) g_gl.cg_color_release(r->old_border_color);
        }
    }
}
#endif

/* Render every collected window's layer into ctx. Factored out of
 * snapshot_do_tick_src so the sample-mode beneath pass can render
 * into the scratch context with the same code. */
static void snap_render_windows(void *ctx, snap_win_entry_t *wins,
                                uint32_t n_wins) {
    uint32_t i;
    for (i = 0; i < n_wins; i++) {
        void *win = wins[i].win;
        void *layer = msg_id(win, g_syms.sel_layer);
        if (!layer || !objc_responds(layer, g_syms.sel_renderInContext)) continue;

        if (g_syms.CGContextSaveGState) g_syms.CGContextSaveGState(ctx);
        if (i > 0 && objc_responds(win, g_syms.sel_frame)) {
            snap_rect_t fr = msg_rect(win, g_syms.sel_frame);
            if (fr.origin.x != 0.0 || fr.origin.y != 0.0) {
                g_syms.CGContextTranslateCTM(ctx, fr.origin.x, fr.origin.y);
            }
        }
        msg_void_id(layer, g_syms.sel_renderInContext, ctx);
        if (g_syms.CGContextRestoreGState) g_syms.CGContextRestoreGState(ctx);
    }
}

static void snapshot_do_tick_src(int src) {
    if (ensure_core_syms() != 0) {
        log_no_windows();
        return;
    }

    void *pool = g_syms.objc_autoreleasePoolPush ? g_syms.objc_autoreleasePoolPush() : NULL;

#ifndef SNAPSHOT_HOST_TEST
    {   /* IOSWINE_NO_ANIM=1: +[UIView setAnimationsEnabled:NO] (transitions complete instantly). */
        static int done;
        const char *na = lsk_getenv("IOSWINE_NO_ANIM");
        if (!done && na && na[0] == '1') {
            void *uiv = g_syms.objc_getClass("UIView");
            if (uiv) {
                ((void (*)(void *, void *, int))g_syms.objc_msgSend)(
                    uiv, g_syms.sel_registerName("setAnimationsEnabled:"), 0);
                snap_log_str("[snapshot] UIView animations disabled\n");
                done = 1;
            }
        }
    }
#endif

    void *app_cls = g_syms.objc_getClass("UIApplication");
    if (!app_cls || !objc_responds(app_cls, g_syms.sel_sharedApplication)) {
        log_no_windows();
        goto out_pool;
    }
    void *app = msg_id(app_cls, g_syms.sel_sharedApplication);
    if (!app) {
        log_no_windows();
        goto out_pool;
    }

    snap_win_entry_t wins[SNAP_MAX_WINDOWS];
    uint32_t n_wins = 0;

    if (objc_responds(app, g_syms.sel_connectedScenes)) {
        void *scenes = msg_id(app, g_syms.sel_connectedScenes);
        if (scenes && objc_responds(scenes, g_syms.sel_allObjects)) {
            void *scene_arr = msg_id(scenes, g_syms.sel_allObjects);
            if (scene_arr && objc_responds(scene_arr, g_syms.sel_count) &&
                objc_responds(scene_arr, g_syms.sel_objectAtIndex)) {
                uint64_t n_scenes = msg_u64(scene_arr, g_syms.sel_count);
                g_dbg_nscenes = n_scenes;
                for (uint64_t i = 0; i < n_scenes && n_wins < SNAP_MAX_WINDOWS; i++) {
                    void *scene = msg_id_u64(scene_arr, g_syms.sel_objectAtIndex, i);
                    if (scene && objc_responds(scene, g_syms.sel_windows)) {
                        void *sw = msg_id(scene, g_syms.sel_windows);
                        collect_windows_from_array(sw, wins, &n_wins, SNAP_MAX_WINDOWS);
                    }
                }
            }
        }
    }

    if (objc_responds(app, g_syms.sel_windows)) {
        void *legacy_wins = msg_id(app, g_syms.sel_windows);
        collect_windows_from_array(legacy_wins, wins, &n_wins, SNAP_MAX_WINDOWS);
    }

    if (n_wins == 0) {
        log_no_windows();
        goto out_pool;
    }

    sort_windows_back_to_front(wins, n_wins);

    /* Commit-before-snapshot: lay out and commit the
     * tree before reading bounds, so the canvas is sized from the
     * post-layout bounds and the text layers are committed at the
     * render scale when renderInContext runs below. */
    snap_commit_before_render(wins, n_wins);

    /* Navigation settle: if the top view controller
     * changed, lay its view out and flush again so the transition
     * tick carries the new content, and log the transition. */
    snap_nav_settle(wins[0].win);

    /* Size canvas from the base visible window's bounds and screen scale. */
    void *base_win = wins[0].win;
    snap_rect_t base_bounds = msg_rect(base_win, g_syms.sel_bounds);
    double scale = resolve_frame_scale(base_win, base_bounds.size.width,
                                       base_bounds.size.height);
    double dw = base_bounds.size.width * scale + 0.5;
    double dh = base_bounds.size.height * scale + 0.5;
    if (dw <= 0.0 || dh <= 0.0 || dw > (double)SNAP_MAX_DIM || dh > (double)SNAP_MAX_DIM) {
        log_no_windows();
        goto out_pool;
    }
#ifndef SNAPSHOT_HOST_TEST
    touch_set_window_size(base_bounds.size.width, base_bounds.size.height);
#endif
    uint32_t w = (uint32_t)dw;
    uint32_t h = (uint32_t)dh;
    uint32_t stride = w * 4u;
    size_t need_bytes = (size_t)stride * (size_t)h;

    if (need_bytes > g_buf_size) {
        if (g_buf) {
            snap_free_buf(g_buf, g_buf_size);
            g_buf = NULL;
            g_buf_size = 0;
        }
        g_buf = (uint8_t *)snap_alloc_buf(need_bytes);
        if (!g_buf) {
            snap_log_str("[snapshot] buffer alloc failed\n");
            goto out_pool;
        }
        g_buf_size = need_bytes;
    }
    /* Clear to opaque white, not transparent black: the canvas
     * is BGRA8888 premultiplied, so 0xFF per byte is white with alpha
     * 255. Any pixel no layer paints (a region between windows, a
     * layer mid-update) then shows as window background instead of a
     * black smudge; the presenter composites the same way. Every real
     * layer paints opaque content over this. */
    snap_memset(g_buf, 0xFF, need_bytes);

    void *cs = g_syms.CGColorSpaceCreateDeviceRGB();
    if (!cs) {
        snap_log_str("[snapshot] CGColorSpaceCreateDeviceRGB failed\n");
        goto out_pool;
    }

    void *ctx = g_syms.CGBitmapContextCreate(g_buf, w, h, 8, stride, cs, SNAP_CG_BITMAP_BGRA);
    if (!ctx) {
        g_syms.CGColorSpaceRelease(cs);
        snap_log_str("[snapshot] CGBitmapContextCreate failed\n");
        goto out_pool;
    }

    /* Flip CTM from bottom-left CoreGraphics to top-left UIKit point coordinates. */
    g_syms.CGContextTranslateCTM(ctx, 0.0, (double)h);
    g_syms.CGContextScaleCTM(ctx, scale, -scale);

    /* Backdrop layers: collect, hide, and degrade
     * before the render. The collect walk needs the window layers
     * and frames; the fill/sample steps need the canvas. */
    snap_backdrop_t backdrops[SNAP_BACKDROP_MAX];
    uint32_t n_backdrops = 0;
    snap_hide_rec_t hide_recs[SNAP_BACKDROP_HIDE_MAX];
    uint32_t n_hide_recs = 0;
    int backdrop_overflow = 0;
    int backdrop_mode = SNAP_BACKDROP_MODE_PLAIN;
    int sample_ok = 0;
    if (g_syms.sel_sublayers && g_syms.sel_frame) {
        for (uint32_t i = 0; i < n_wins; i++) {
            void *layer = msg_id(wins[i].win, g_syms.sel_layer);
            double ox = 0.0, oy = 0.0;
            if (!layer) continue;
            if (i > 0 && objc_responds(wins[i].win, g_syms.sel_frame)) {
                snap_rect_t fr = msg_rect(wins[i].win, g_syms.sel_frame);
                ox = fr.origin.x;
                oy = fr.origin.y;
            }
            snap_collect_backdrops_rec(layer, ox, oy, backdrops, &n_backdrops,
                                       SNAP_BACKDROP_MAX, 0);
        }
    }
    if (n_backdrops > 0) {
        uint32_t i;
        char nb[24];
        size_t k;
        backdrop_mode = snap_backdrop_mode();
        snap_log_str("[snapshot] backdrop ");
        k = fmt_u64(nb, n_backdrops);
        nb[k] = 0;
        snap_log_str(nb);
        snap_log_str(backdrop_mode == SNAP_BACKDROP_MODE_SAMPLE ?
                     " mode=sample\n" : " mode=plain\n");
        if (backdrop_mode == SNAP_BACKDROP_MODE_SAMPLE) {
            /* Beneath pass: hide backdrops + above-layers, render
             * into the scratch context, blur the backdrop rects,
             * copy the scratch buffer over the canvas. Any failure
             * (scratch alloc, context, overflow) falls back to
             * plain below: the backdrops stay hidden either way. */
            if (snap_backdrop_ensure_scratch(need_bytes) == 0) {
                /* The scratch buffer is the beneath pass's canvas:
                 * clear it to the same opaque white as the main
                 * canvas, or unpainted regions copy as transparent
                 * black over the frame. */
                snap_memset(g_scratch_buf, 0xFF, need_bytes);
                void *sctx = g_syms.CGBitmapContextCreate(g_scratch_buf, w, h, 8,
                                                          stride, cs, SNAP_CG_BITMAP_BGRA);
                if (sctx) {
                    int radius = (int)(scale * 4.0);
                    g_syms.CGContextTranslateCTM(sctx, 0.0, (double)h);
                    g_syms.CGContextScaleCTM(sctx, scale, -scale);
                    for (i = 0; i < n_wins; i++) {
                        void *layer = msg_id(wins[i].win, g_syms.sel_layer);
                        if (!layer) continue;
                        snap_backdrop_classify_rec(layer, 0, 0, hide_recs,
                                                   &n_hide_recs, SNAP_BACKDROP_HIDE_MAX,
                                                   &backdrop_overflow, 0);
                    }
                    if (!backdrop_overflow) {
                        size_t b;
                        snap_render_windows(sctx, wins, n_wins);
                        if (radius < 2) radius = 2;
                        if (radius > 24) radius = 24;
                        for (i = 0; i < n_backdrops; i++) {
                            snap_box_blur_rect(g_scratch_buf, w, h, stride,
                                               backdrops[i].rect, scale, radius);
                        }
                        for (b = 0; b < need_bytes; b++) {
                            g_buf[b] = g_scratch_buf[b];
                        }
                        sample_ok = 1;
                    }
                    g_syms.CGContextRelease(sctx);
                }
            }
            snap_backdrop_restore(hide_recs, n_hide_recs);
            n_hide_recs = 0;
            backdrop_overflow = 0;
            if (sample_ok) {
                /* Above pass: hide backdrops + below-layers. The
                 * main loop below renders only the above content
                 * over the blurred canvas. */
                for (i = 0; i < n_wins; i++) {
                    void *layer = msg_id(wins[i].win, g_syms.sel_layer);
                    if (!layer) continue;
                    snap_backdrop_classify_rec(layer, 0, 1, hide_recs,
                                               &n_hide_recs, SNAP_BACKDROP_HIDE_MAX,
                                               &backdrop_overflow, 0);
                }
                if (backdrop_overflow) {
                    /* Too many layers to classify: fall back to
                     * plain over the blurred canvas. */
                    snap_backdrop_restore(hide_recs, n_hide_recs);
                    n_hide_recs = 0;
                    sample_ok = 0;
                }
            }
        }
        if (!sample_ok) {
            /* Plain mode, or the sample fallback: hide the backdrop
             * layers and fill their rects with the style fill. */
            for (i = 0; i < n_backdrops; i++) {
                if (snap_backdrop_hide_one(backdrops[i].layer, hide_recs,
                                           &n_hide_recs, SNAP_BACKDROP_HIDE_MAX) != 0) {
                    backdrop_overflow = 1;
                }
                snap_fill_buf_rect(g_buf, w, h, stride, backdrops[i].rect,
                                   scale, backdrops[i].fill);
            }
            if (backdrop_overflow) {
                snap_log_str("[snapshot] backdrop hide overflow\n");
            }
        }
    }

#ifndef SNAPSHOT_HOST_TEST
    snap_glass_rec_t glass_recs[SNAP_GLASS_MAX];
    uint32_t n_glass_recs = 0;
    if (n_backdrops > 0) {
        snap_solarium_prepare(wins, n_wins, cs, hide_recs, &n_hide_recs,
                              glass_recs, &n_glass_recs);
    }
#endif
    snap_render_windows(ctx, wins, n_wins);

    /* Restore every hidden/glass state this tick changed. */
#ifndef SNAPSHOT_HOST_TEST
    if (n_glass_recs > 0) snap_solarium_restore(glass_recs, n_glass_recs);
#endif
    if (n_hide_recs > 0) snap_backdrop_restore(hide_recs, n_hide_recs);

    /* Frame size changed since the last accepted frame: the
     * presenter session is fixed at the old size and present_api
     * clips larger frames to it, so tear the session down first; the
     * present call below re-inits at the new size in the same tick.
     * This is how a late INFO (or a rotation/resize) grows the frame
     * from the first-tick size to the native window size. */
    if (g_last_present_w != 0 &&
        (w != g_last_present_w || h != g_last_present_h)) {
        present_shutdown();
        snap_log_str("[snapshot] frame size changed, presenter session reset\n");
    }

    int rc = ioswine_present_frame(g_buf, w, h, stride);
    if (rc == 0) {
        g_last_present_w = w;
        g_last_present_h = h;
    }
    g_frame_no++;
    if (src == SNAP_TICK_SRC_CA) {
        g_ca_frames++;
    } else {
        g_pump_frames++;
    }
    snap_log_frame(g_frame_no, w, h, n_wins, rc, src);
    if (g_frame_no % 60 == 0) snap_log_frame_counts();
#ifndef SNAPSHOT_HOST_TEST
    touch_on_frame(g_frame_no);
#endif

    const char *png_path = snap_getenv("IOSWINE_SNAPSHOT_PNG");
    if (png_path && png_path[0] != '\0' && !g_png_written) {
        if (buf_is_nonempty(g_buf, need_bytes)) {
            /* Settled-frame PNG: the first non-empty
             * frame catches the UI mid-layout, so wait for the
             * second consecutive non-empty tick at the same size
             * before writing the one-shot PNG. A size change or an
             * empty frame restarts the count; write_png_once still
             * writes at most once. */
            if (w == g_png_last_w && h == g_png_last_h) {
                g_png_settled_ticks++;
            } else {
                g_png_settled_ticks = 1;
                g_png_last_w = w;
                g_png_last_h = h;
            }
            if (g_png_settled_ticks >= 2) {
                write_png_once(ctx, png_path);
            }
        } else {
            g_png_settled_ticks = 0;
        }
    }

    g_syms.CGContextRelease(ctx);
    g_syms.CGColorSpaceRelease(cs);

out_pool:
    if (pool && g_syms.objc_autoreleasePoolPop) {
        g_syms.objc_autoreleasePoolPop(pool);
    }
}

/* Wrapper for callers that do not set a source: the tick renders
 * with the source of the most recent request (g_tick_src, PUMP
 * unless a carender request arrived since). */
static void snapshot_do_tick(void) {
    snapshot_do_tick_src(g_tick_src);
}
/* Ticks owed, 0..2, atomic. 1 = a tick is queued or running; 2 =
 * a tick is queued or running plus one more coalesced request.
 * snapshot_request_frame() adds one (capped at 2); the tick
 * subtracts one after rendering and re-dispatches while the count
 * stays above 0. fetch_add / fetch_sub serialize requesters with
 * the tick, so a request can neither be lost nor stack a second
 * tick. The render itself always runs on the main thread; this
 * counter is the only cross-thread state. */
static int g_ticks_owed;
static int g_pump_stop;
static int g_pump_enabled = 1;
static void snap_tick_once(void *ctx);
static void snap_dispatch_once(void);

#ifndef SNAPSHOT_HOST_TEST
static void (*g_async_f)(void *, void *, void (*)(void *));
static void *snap_pump_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct { long s, ns; } ts = { 1, 0 };
        {
            const char *ms_s = lsk_getenv("IOSWINE_SNAPSHOT_MS");
            long ms = 0;
            while (ms_s && *ms_s >= '0' && *ms_s <= '9') ms = ms * 10 + (*ms_s++ - '0');
            if (ms >= 10) { ts.s = ms / 1000; ts.ns = (ms % 1000) * 1000000L; }
        }
        sys2(SYS_nanosleep, (long)&ts, 0);
        if (g_pump_stop || !g_started) return 0;
        /* One tick per second, coalesced through the same entry
         * point display-link rendering uses, so the pump can never stack a tick
         * on top of an on-demand one. */
        snapshot_request_frame();
    }
    return 0;
}
#endif

/* One tick on the main thread: render, then drain one coalesced
 * request if one arrived while rendering. Never reschedules the
 * periodic chain; that is snapshot_tick_cb's job. A tick queued
 * before snapshot_shutdown() becomes a no-op here. */
static void snap_check_main_thread(void) {
#if defined(__aarch64__) && !defined(SNAPSHOT_HOST_TEST)
    static int warned;
    long tid = sys0(SYS_gettid), pid = sys0(SYS_getpid);
    if (tid != pid && warned < 5) {
        warned++;
        (void)pid;
        snap_log_str("[snapshot] MAIN-QUEUE TICK OFF MAIN THREAD\n");
    }
#endif
}

static void snap_tick_once(void *ctx) {
    int prev;
    (void)ctx;
    snap_check_main_thread();
    if (!g_started) {
        __atomic_store_n(&g_ticks_owed, 0, __ATOMIC_RELEASE);
        return;
    }
    snapshot_do_tick();
    prev = __atomic_fetch_sub(&g_ticks_owed, 1, __ATOMIC_ACQ_REL);
    if (prev > 1) {
        /* Another tick is owed: serve it with one more tick. */
        snap_dispatch_once();
    }
}

/* Post one tick to the main queue. Device: dispatch_async_f when
 * available, else dispatch_after_f with a zero delay. Host test:
 * dispatch_after_f with a zero delay (the mock records it). On
 * failure the owed unit is given back so the count stays exact. */
static void snap_dispatch_once(void) {
#ifndef SNAPSHOT_HOST_TEST
    if (g_async_f) {
        g_async_f(g_syms.dispatch_main_q, NULL, snap_tick_once);
        return;
    }
#endif
    if (g_syms.dispatch_after_f && g_syms.dispatch_time &&
        g_syms.dispatch_main_q) {
        uint64_t when = g_syms.dispatch_time(0, 0);
        g_syms.dispatch_after_f(when, g_syms.dispatch_main_q, NULL,
                                snap_tick_once);
    } else {
        __atomic_fetch_sub(&g_ticks_owed, 1, __ATOMIC_ACQ_REL);
    }
}

static void snapshot_tick_cb(void *ctx) {
    (void)ctx;
    if (!g_started) return;
    snapshot_do_tick_src(SNAP_TICK_SRC_PUMP);
    if (g_pump_enabled && g_syms.dispatch_after_f && g_syms.dispatch_time &&
        g_syms.dispatch_main_q) {
        uint64_t when = g_syms.dispatch_time(0, SNAP_INTERVAL_NS);
        g_syms.dispatch_after_f(when, g_syms.dispatch_main_q, NULL, snapshot_tick_cb);
    }
}

static void snapshot_request_frame_src(int src) {
    int prev;
    if (!g_started) return;
    g_tick_src = src;
    prev = __atomic_fetch_add(&g_ticks_owed, 1, __ATOMIC_ACQ_REL);
    if (prev == 0) {
        snap_dispatch_once();
    } else if (prev >= 2) {
        /* Already one tick running plus one coalesced: this
         * request is folded into the pending tick. */
        __atomic_fetch_sub(&g_ticks_owed, 1, __ATOMIC_ACQ_REL);
    }
    /* prev == 1: the running tick's post-render check will see
     * the owed unit and dispatch one more tick for it. */
}

void snapshot_request_frame(void) {
    snapshot_request_frame_src(SNAP_TICK_SRC_PUMP);
}

void snapshot_request_frame_ca(void) {
    snapshot_request_frame_src(SNAP_TICK_SRC_CA);
}

uint64_t snapshot_frame_no(void) {
    /* The carender engine thread reads this while the main thread
     * writes it (a tick completing), so the load is atomic; the
     * writer is single-threaded (main thread only) and the
     * coalescer's acquire/release atomics order the tick itself. */
    return __atomic_load_n(&g_frame_no, __ATOMIC_ACQUIRE);
}

int snapshot_is_started(void) {
    return __atomic_load_n(&g_started, __ATOMIC_ACQUIRE);
}

void snapshot_frame_counts(uint64_t *pump_frames, uint64_t *ca_frames) {
    if (pump_frames) *pump_frames = __atomic_load_n(&g_pump_frames, __ATOMIC_ACQUIRE);
    if (ca_frames) *ca_frames = __atomic_load_n(&g_ca_frames, __ATOMIC_ACQUIRE);
}

int snapshot_frame_pending(void) {
    if (!g_started) return 0;
    return __atomic_load_n(&g_ticks_owed, __ATOMIC_ACQUIRE) != 0;
}

void snapshot_shutdown(void) {
    g_started = 0;
    g_pump_stop = 1;
    __atomic_store_n(&g_ticks_owed, 0, __ATOMIC_RELEASE);
    if (g_buf) {
        snap_free_buf(g_buf, g_buf_size);
        g_buf = NULL;
        g_buf_size = 0;
    }
    if (g_scratch_buf) {
        snap_free_buf(g_scratch_buf, g_scratch_size);
        g_scratch_buf = NULL;
        g_scratch_size = 0;
    }
    if (g_blur_tmp) {
        snap_free_buf(g_blur_tmp, g_blur_tmp_size);
        g_blur_tmp = NULL;
        g_blur_tmp_size = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* IOSWINE_NSERROR_TRACE=1: log every -[NSError initWithDomain:code:userInfo:] (debug aid:   */
/* os_log goes nowhere without logd, NSErrors show where decoding/validation fails).          */
/* ------------------------------------------------------------------ */
typedef void *(*nserr_init_fn)(void *, void *, void *, long, void *);
static nserr_init_fn g_nserr_orig;
static void *g_sel_utf8, *g_sel_desc;
static void lb_add(char *buf, size_t *n, size_t cap, const char *t) {
    while (t && *t && *n + 1 < cap) { char c = *t++; buf[(*n)++] = (c == '\n') ? ' ' : c; }
}
static void *nserr_hook(void *self, void *cmd, void *domain, long code, void *info) {
    void *r = g_nserr_orig(self, cmd, domain, code, info);
    void *(*ms)(void *, void *) = (void *(*)(void *, void *))g_syms.objc_msgSend;
    char line[1400]; size_t n = 0; char nb[24]; size_t k;
    const char *d = domain ? (const char *)ms(domain, g_sel_utf8) : "(nil)";
    lb_add(line, &n, sizeof(line), "[nserror] ");
    lb_add(line, &n, sizeof(line), d ? d : "?");
    lb_add(line, &n, sizeof(line), " code=");
    if (code < 0) { lb_add(line, &n, sizeof(line), "-"); code = -code; }
    k = fmt_u64(nb, (uint64_t)code); nb[k] = 0; lb_add(line, &n, sizeof(line), nb);
    if (info) {
        void *desc = ms(info, g_sel_desc);
        const char *ds = desc ? (const char *)ms(desc, g_sel_utf8) : 0;
        if (ds) { char tmp[700]; size_t i = 0; while (ds[i] && i < 699) { tmp[i] = ds[i]; i++; } tmp[i] = 0;
                  lb_add(line, &n, sizeof(line), " info="); lb_add(line, &n, sizeof(line), tmp); }
    }
    {
        uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
        lb_add(line, &n, sizeof(line), " bt=");
        for (int i = 0; i < 10 && fp && ((uint64_t)fp & 7) == 0; i++) {
            uint64_t lr = fp[1] & 0x0000007fffffffffULL;
            char hb[20]; int kk = 0; char t[16]; int q = 0;
            do { int v = (int)(lr & 15); t[q++] = (char)(v < 10 ? '0' + v : 'a' + v - 10); lr >>= 4; } while (lr && q < 15);
            hb[kk++] = '0'; hb[kk++] = 'x'; while (q) hb[kk++] = t[--q]; hb[kk++] = ' '; hb[kk] = 0;
            lb_add(line, &n, sizeof(line), hb);
            fp = (uint64_t *)fp[0];
        }
    }
    line[n++] = '\n';
    snap_log_write(line, n);
    return r;
}

/* IOSWINE_NSERROR_TRACE=1 also logs every -[NSException initWithName:reason:userInfo:]. */
typedef void *(*nsexc_init_fn)(void *, void *, void *, void *, void *);
static nsexc_init_fn g_nsexc_orig;
static void *nsexc_hook(void *self, void *cmd, void *name, void *reason, void *info) {
    void *(*ms)(void *, void *) = (void *(*)(void *, void *))g_syms.objc_msgSend;
    char line[1200]; size_t n = 0;
    const char *nm = name ? (const char *)ms(name, g_sel_utf8) : "(nil)";
    const char *rs = reason ? (const char *)ms(reason, g_sel_utf8) : "(nil)";
    lb_add(line, &n, sizeof(line), "[nsexception] ");
    lb_add(line, &n, sizeof(line), nm ? nm : "?");
    lb_add(line, &n, sizeof(line), ": ");
    lb_add(line, &n, sizeof(line), rs ? rs : "?");
    line[n++] = '\n';
    snap_log_write(line, n);
    return g_nsexc_orig(self, cmd, name, reason, info);
}
static void nserror_trace_install(void) {
    const char *en = snap_getenv("IOSWINE_NSERROR_TRACE");
    if (!en || en[0] != '1') return;
    if (ensure_core_syms() != 0) return;
    void *(*class_getInstanceMethod)(void *, void *) =
        (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("class_getInstanceMethod"));
    void *(*method_setImplementation)(void *, void *) =
        (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("method_setImplementation"));
    if (!class_getInstanceMethod || !method_setImplementation) return;
    void *cls = g_syms.objc_getClass("NSError");
    void *sel = g_syms.sel_registerName("initWithDomain:code:userInfo:");
    g_sel_utf8 = g_syms.sel_registerName("UTF8String");
    g_sel_desc = g_syms.sel_registerName("description");
    void *m = cls ? class_getInstanceMethod(cls, sel) : 0;
    if (!m) { snap_log_str("[nserror] hook failed\n"); return; }
    g_nserr_orig = (nserr_init_fn)method_setImplementation(m, (void *)nserr_hook);
    {
        void *ec = g_syms.objc_getClass("NSException");
        void *em = ec ? class_getInstanceMethod(ec, g_syms.sel_registerName("initWithName:reason:userInfo:")) : 0;
        if (em) g_nsexc_orig = (nsexc_init_fn)method_setImplementation(em, (void *)nsexc_hook);
    }
    snap_log_str("[nserror] hook installed\n");
}

/* IOSWINE_OBJC_TRACE="Class -sel;Class -sel" (up to 4): log entry of instance methods. */
typedef uint64_t (*trace_fn)(void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static trace_fn g_tr_orig[4];
static char g_tr_name[4][96];
#define TR_HOOK(N) static uint64_t tr_hook##N(void *s, void *c, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f, uint64_t g) { \
    snap_log_str("[objc] -> "); snap_log_str(g_tr_name[N]); snap_log_str("\n"); \
    uint64_t r = g_tr_orig[N](s, c, a, b, d, e, f, g); \
    snap_log_str("[objc] <- "); snap_log_str(g_tr_name[N]); snap_log_str("\n"); return r; }
TR_HOOK(0) TR_HOOK(1) TR_HOOK(2) TR_HOOK(3)
static void objc_trace_install(void) {
    const char *en = snap_getenv("IOSWINE_OBJC_TRACE");
    if (!en || !en[0] || ensure_core_syms() != 0) return;
    void *(*gim)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("class_getInstanceMethod"));
    void *(*msi)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("method_setImplementation"));
    if (!gim || !msi) return;
    int n = 0; const char *p = en;
    while (*p && n < 4) {
        char cls[64], sel[96]; int i = 0, j = 0;
        while (*p && *p != ' ' && i < 63) { cls[i++] = *p++; }
        cls[i] = 0;
        while (*p == ' ' || *p == '-') p++;
        while (*p && *p != ';' && j < 95) { sel[j++] = *p++; }
        sel[j] = 0;
        if (*p == ';') p++;
        void *c = g_syms.objc_getClass(cls);
        void *m = c ? gim(c, g_syms.sel_registerName(sel)) : 0;
        { int k = 0, q = 0; while (cls[q] && k < 40) g_tr_name[n][k++] = cls[q++]; g_tr_name[n][k++] = ' ';
          q = 0; while (sel[q] && k < 94) g_tr_name[n][k++] = sel[q++]; g_tr_name[n][k] = 0; }
        if (!m) { snap_log_str("[objc] not found: "); snap_log_str(g_tr_name[n]); snap_log_str("\n"); continue; }
        void *h = n == 0 ? (void *)tr_hook0 : n == 1 ? (void *)tr_hook1 : n == 2 ? (void *)tr_hook2 : (void *)tr_hook3;
        g_tr_orig[n] = (trace_fn)msi(m, h);
        snap_log_str("[objc] hooked "); snap_log_str(g_tr_name[n]); snap_log_str("\n");
        n++;
    }
}

/* IOSWINE_RENDERS_LOCALLY=1: +[UIApplication rendersLocally] -> YES, so UIKit makes
 * local CAContexts instead of asking the (absent) render server for a remote one. */
static uint64_t rl_yes(void *s, void *c) { (void)s; (void)c; return 1; }
static uint64_t rl_nil(void *s, void *c) { (void)s; (void)c; return 0; }
static void renders_locally_install(void) {
    const char *en = snap_getenv("IOSWINE_RENDERS_LOCALLY");
    if (!en || en[0] != '1' || ensure_core_syms() != 0) return;
    void *(*gcm)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("class_getClassMethod"));
    void *(*msi)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("method_setImplementation"));
    if (!gcm || !msi) return;
    void *c = g_syms.objc_getClass("UIApplication");
    void *m = c ? gcm(c, g_syms.sel_registerName("rendersLocally")) : 0;
    if (!m) { snap_log_str("[rl] rendersLocally not found\n"); return; }
    msi(m, (void *)rl_yes);
    snap_log_str("[rl] +[UIApplication rendersLocally] -> YES\n");
    /* The BKSHIDEventDeliveryManager sharedInstance -> nil
     * hook used to live here. It is gone: bkshid.c now serves the
     * manager's BSXPC endpoint (com.apple.backboard.hid-services.xpc)
     * in-process, so the manager stays live and _UIEventDeferringManager
     * can save records without the assertion firing. rendersLocally
     * stays hooked. */
}

/* IOSWINE_OBJC_NIL="Class -sel;Class +sel;..." (up to 16): make methods return 0/nil. */
static void objc_nil_install(void) {
    const char *en = snap_getenv("IOSWINE_OBJC_NIL");
    if (!en || !en[0] || ensure_core_syms() != 0) return;
    void *(*gim)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("class_getInstanceMethod"));
    void *(*gcm)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("class_getClassMethod"));
    void *(*msi)(void *, void *) = (void *(*)(void *, void *))lsk_sign_fn(snap_dlsym("method_setImplementation"));
    if (!gim || !gcm || !msi) return;
    const char *p = en; int n = 0;
    while (*p && n < 16) {
        char cls[64], sel[256]; int i = 0, j = 0, is_cls = 0;
        while (*p && *p != ' ' && i < 63) { cls[i++] = *p++; }
        cls[i] = 0;
        while (*p == ' ') p++;
        if (*p == '+') is_cls = 1;
        if (*p == '+' || *p == '-') p++;
        while (*p && *p != ';' && j < 255) { sel[j++] = *p++; }
        sel[j] = 0;
        if (*p == ';') p++;
        n++;
        void *c = g_syms.objc_getClass(cls);
        void *sl = g_syms.sel_registerName(sel);
        void *m = 0;
        if (c) { if (is_cls) m = gcm(c, sl); else m = gim(c, sl); }
        if (c && !m) {
            /* Fallback: scan the method list by name (SEL identity can differ). */
            void *(*gcl)(void *) = (void *(*)(void *))lsk_sign_fn(snap_dlsym("object_getClass"));
            void **(*cml)(void *, unsigned *) = (void **(*)(void *, unsigned *))lsk_sign_fn(snap_dlsym("class_copyMethodList"));
            void *(*mgn)(void *) = (void *(*)(void *))lsk_sign_fn(snap_dlsym("method_getName"));
            const char *(*sgn)(void *) = (const char *(*)(void *))lsk_sign_fn(snap_dlsym("sel_getName"));
            void *k = is_cls && gcl ? gcl(c) : c;
            unsigned cnt = 0;
            void **ml = (cml && mgn && sgn && k) ? cml(k, &cnt) : 0;
            for (unsigned q = 0; ml && q < cnt && !m; q++) {
                const char *nm = sgn(mgn(ml[q]));
                int e = 0; while (nm[e] && nm[e] == sel[e]) e++;
                if (!nm[e] && !sel[e]) m = ml[q];
            }
            if (!m) { char nb[24]; size_t z = fmt_u64(nb, cnt); nb[z] = 0; snap_log_str("[objcnil] methods scanned: "); snap_log_str(nb); snap_log_str("\n"); }
        }
        snap_log_str(m ? "[objcnil] " : (c ? "[objcnil] no method: " : "[objcnil] no class: "));
        snap_log_str(cls); snap_log_str(is_cls ? " +" : " -"); snap_log_str(sel); snap_log_str("\n");
        if (m) msi(m, (void *)rl_nil);
    }
}

void snapshot_init(void) {
    {   /* debug: Aidoku GOT ___chkstk_darwin */
        const char *g = snap_getenv("IOSWINE_DUMP_GOT");
        if (g && g[0] == '1') {
            uint64_t v = *(volatile uint64_t *)0x10110b448ULL;
            char nb[24]; size_t k = fmt_u64(nb, v); nb[k] = 0;
            snap_log_str("[got] ___chkstk_darwin slot = "); snap_log_str(nb); snap_log_str("\n");
        }
    }
    nserror_trace_install();
    objc_trace_install();
    renders_locally_install();
    objc_nil_install();
    const char *en = snap_getenv("IOSWINE_SNAPSHOT");
    if (!en || en[0] != '1' || en[1] != '\0') {
        return;
    }
    if (g_started) return;
    g_started = 1;
    g_pump_stop = 0;
    {
        /* IOSWINE_SNAPSHOT_PUMP=0 disables the periodic 1 s pump
         * (pure on-demand mode: frames only on
         * snapshot_request_frame(), for the display-link path).
         * Default is on: the pump is the fallback that keeps the
         * app visible when no display-link hook is wired yet, and
         * the device run can A/B by setting PUMP=0. */
        const char *pump = snap_getenv("IOSWINE_SNAPSHOT_PUMP");
        g_pump_enabled = !(pump && pump[0] == '0' && pump[1] == '\0');
    }

    if (ensure_dispatch_syms() != 0) {
        snap_log_str("[snapshot] init failed: missing libdispatch symbols\n");
        return;
    }

#ifndef SNAPSHOT_HOST_TEST
    touch_init();
#endif

    if (!g_pump_enabled) {
        snap_log_str("[snapshot] pump off (on-demand mode)\n");
        return;
    }

#ifndef SNAPSHOT_HOST_TEST
    /* carender: when the vsync-driven engine is on,
     * it owns all ticks, so the 1 s pump must not also present.
     * Two producers would fight over the two present buffers and
     * each would see the other's frames as in flight. Defer the
     * pump; the engine calls snapshot_render_now() instead. */
    {
        const char *ca = snap_getenv("IOSWINE_CARENDER");
        if (ca && ca[0] == '1' && ca[1] == '\0') {
            snap_log_str("[snapshot] pump deferred to carender\n");
            return;
        }
    }
    {   /* Device: a plain pthread posts each tick to the main queue. dispatch_after
         * timers on the main queue stopped firing after the first frame. */
        int (*pc)(void *, void *, void *(*)(void *), void *) =
            (int (*)(void *, void *, void *(*)(void *), void *))lsk_sign_fn(snap_dlsym("pthread_create"));
        g_async_f = (void (*)(void *, void *, void (*)(void *)))lsk_sign_fn(snap_dlsym("dispatch_async_f"));
        uint64_t th;
        if (pc && g_async_f && pc(&th, 0, snap_pump_thread, 0) == 0) {
            snap_log_str("[snapshot] pump armed (thread, 1000 ms)\n");
            return;
        }
    }
#endif
    uint64_t when = g_syms.dispatch_time(0, SNAP_INTERVAL_NS);
    g_syms.dispatch_after_f(when, g_syms.dispatch_main_q, NULL, snapshot_tick_cb);
    snap_log_str("[snapshot] pump armed (1000 ms on main queue)\n");
}

/* ------------------------------------------------------------------ */
/* Host test helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef SNAPSHOT_HOST_TEST
void snapshot_test_configure(const snapshot_test_ops_t *ops) {
    g_test_ops = ops;
}

void snapshot_test_reset(void) {
    if (g_buf) {
        snap_free_buf(g_buf, g_buf_size);
        g_buf = NULL;
        g_buf_size = 0;
    }
    if (g_scratch_buf) {
        snap_free_buf(g_scratch_buf, g_scratch_size);
        g_scratch_buf = NULL;
        g_scratch_size = 0;
    }
    if (g_blur_tmp) {
        snap_free_buf(g_blur_tmp, g_blur_tmp_size);
        g_blur_tmp = NULL;
        g_blur_tmp_size = 0;
    }
    snap_memset(&g_syms, 0, sizeof(g_syms));
    g_started = 0;
    g_frame_no = 0;
    g_tick_src = SNAP_TICK_SRC_PUMP;
    g_pump_frames = 0;
    g_ca_frames = 0;
    g_logged_no_win = 0;
    g_last_no_win_ns = 0;
    g_dbg_nscenes = 0xffff;
    g_png_written = 0;
    g_ticks_owed = 0;
    g_pump_stop = 0;
    g_pump_enabled = 1;
    g_last_present_w = 0;
    g_last_present_h = 0;
    g_logged_info_scale = -1.0;
    g_png_settled_ticks = 0;
    g_png_last_w = 0;
    g_png_last_h = 0;
    g_nav_top_vc = 0;
    g_nav_top_name[0] = 0;
    g_nav_settle_ticks = 0;
}

void snapshot_test_tick(void) {
    snapshot_do_tick();
}
#endif

/* carender: render one frame on demand, on the
 * caller's thread. The carender engine calls this from the main
 * thread (dispatched from its engine thread) when a CATransaction
 * commit marked the tree dirty. Safe to call when the 1 s pump
 * was deferred; a no-op in practice when no windows exist yet. */
void snapshot_render_now(void) {
    snapshot_do_tick_src(SNAP_TICK_SRC_CA);
}
