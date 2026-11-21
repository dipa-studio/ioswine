/* snapshot_pipeline_test.c - host test for snapshot and presenter plumbing.
 *
 * Covers, in one binary:
 *   1. Stride contract: present_stride_for_width is tight width*4;
 *      the IOSurface aligned stride is align64(width*4); the two
 *      agree exactly for 720/1440 and differ for 1080 (4320/4352).
 *   2. Snapshot pump modes: default pump arms the 1 s chain;
 *      IOSWINE_SNAPSHOT_PUMP=0 arms nothing (on-demand mode).
 *   3. snapshot_request_frame() coalescing: five requests before
 *      the queued tick runs produce exactly one frame; a request
 *      issued mid-render is drained by exactly one more tick.
 *   4. snapshot_shutdown(): queued ticks become no-ops, the
 *      bitmap buffer is freed, re-init re-arms.
 *   5. present_api against a fake presenter server: 1000 frames
 *      with no fd growth, pixel-exact row copies, geometry-change
 *      re-init (second HELLO), damage clipping, disconnect +
 *      connect backoff, flush and shutdown.
 *   6. Stride fuzz: 2000 random geometries, tight vs aligned
 *      stride invariants, plus a row-copy reference check.
 *
 * Plain cc, ASan/UBSan. Build with -DPRESENT_TEST
 * -DSNAPSHOT_HOST_TEST=1, -iquote a staging dir holding the
 * patched present.c/present_api.c/snapshot.c/protocol.c and
 * tests/host/host-shim-present/linux.h as linux.h.
 */
#define _GNU_SOURCE
#define SNAPSHOT_HOST_TEST 1

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "present.h"
#include "protocol.h"
#include "snapshot.h"

extern void present_test_set_socket_name(const char *name);

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    uint64_t _a = (uint64_t)(a); uint64_t _b = (uint64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", __FILE__, \
               __LINE__, #a, (unsigned long long)_a, \
               (unsigned long long)_b); \
    } \
} while (0)

#define CHECK_EQ_I64(a, b) do { \
    int64_t _a = (int64_t)(a); int64_t _b = (int64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, \
               __LINE__, #a, (long long)_a, (long long)_b); \
    } \
} while (0)

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static uint64_t g_rng = 0x123456789abcdefULL;

static uint64_t rng_next(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

static int count_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------ */
/* Mock Objective-C / CoreGraphics / ImageIO (adapted from the        */
/* snapshot_test mocks: one window, one layer, configurable size)    */
/* ------------------------------------------------------------------ */

static const char *g_env_snapshot;
static const char *g_env_pump;
static uint64_t g_now_ns;
static char g_log_buf[8192];
static size_t g_log_len;

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
    if (strcmp(name, "IOSWINE_SNAPSHOT_PUMP") == 0) return g_env_pump;
    if (strcmp(name, "IOSWINE_SNAPSHOT_PNG") == 0) return NULL;
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
static int g_dispatch_after_calls;
static uint64_t g_last_dispatch_when;
static void (*g_last_dispatch_cb)(void *);
static void *g_last_dispatch_ctx;

static int g_render_count;
static int g_request_during_render;

static int g_pool_push_calls;
static int g_pool_pop_calls;

static uint64_t mock_dispatch_time(uint64_t when, int64_t delta) {
    return when + (uint64_t)delta;
}

static void mock_dispatch_after_f(uint64_t when, void *q, void *ctx,
                                  void (*cb)(void *)) {
    (void)q;
    g_dispatch_after_calls++;
    g_last_dispatch_when = when;
    g_last_dispatch_ctx = ctx;
    g_last_dispatch_cb = cb;
}

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
        g_render_count++;
        if (g_request_during_render) {
            g_request_during_render = 0;
            snapshot_request_frame();
        }
        if (ctx && ctx->data && o->fill_pixel != 0) {
            uint32_t *px = (uint32_t *)ctx->data;
            px[0] = o->fill_pixel;
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
    return ctx;
}

static void mock_cg_ctx_release(void *c) { free(c); }

static void mock_cg_translate(void *c, double tx, double ty) {
    mock_cg_ctx_t *ctx = (mock_cg_ctx_t *)c;
    ctx->tx = tx;
    ctx->ty = ty;
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

/* One window on one scene, painted by the mock layer. */
static mock_obj_t g_scr;
static mock_obj_t g_layer;
static mock_obj_t g_win;
static mock_obj_t g_scene_wins;
static mock_obj_t g_scene;
static mock_obj_t g_scenes_arr;
static mock_obj_t g_scenes_set;
static mock_obj_t g_app;

static void setup_mock_window(double wpt, double hpt, double scale,
                              uint32_t fill) {
    memset(&g_scr, 0, sizeof(g_scr));
    memset(&g_layer, 0, sizeof(g_layer));
    memset(&g_win, 0, sizeof(g_win));
    memset(&g_scene_wins, 0, sizeof(g_scene_wins));
    memset(&g_scene, 0, sizeof(g_scene));
    memset(&g_scenes_arr, 0, sizeof(g_scenes_arr));
    memset(&g_scenes_set, 0, sizeof(g_scenes_set));
    memset(&g_app, 0, sizeof(g_app));
    g_scr.kind = KIND_SCREEN;
    g_scr.scale = scale;
    g_layer.kind = KIND_LAYER;
    g_layer.tag = 1;
    g_layer.fill_pixel = fill;
    g_win.kind = KIND_WINDOW;
    g_win.hidden = 0;
    g_win.alpha = 1.0;
    g_win.bounds.origin.x = 0;
    g_win.bounds.origin.y = 0;
    g_win.bounds.size.width = wpt;
    g_win.bounds.size.height = hpt;
    g_win.frame = g_win.bounds;
    g_win.level = 0.0;
    g_win.screen = &g_scr;
    g_win.layer = &g_layer;
    g_win.content_scale = scale;
    g_scene_wins.kind = KIND_ARRAY;
    g_scene_wins.count = 1;
    g_scene_wins.items[0] = &g_win;
    g_scene.kind = KIND_SCENE;
    g_scene.items[0] = &g_scene_wins;
    g_scenes_arr.kind = KIND_ARRAY;
    g_scenes_arr.count = 1;
    g_scenes_arr.items[0] = &g_scene;
    g_scenes_set.kind = KIND_SET;
    g_scenes_set.items[0] = &g_scenes_arr;
    g_app.kind = KIND_APP;
    g_app.scenes_set = &g_scenes_set;
    g_app.legacy_windows = NULL;
    g_shared_app = &g_app;
    g_main_screen = &g_scr;
}

static void reset_snapshot_state(void) {
    snapshot_test_reset();
    snapshot_test_configure(&g_ops);
    g_env_snapshot = NULL;
    g_env_pump = NULL;
    g_now_ns = 0;
    g_dispatch_after_calls = 0;
    g_last_dispatch_when = 0;
    g_last_dispatch_cb = NULL;
    g_last_dispatch_ctx = NULL;
    g_render_count = 0;
    g_request_during_render = 0;
    g_pool_push_calls = 0;
    g_pool_pop_calls = 0;
    reset_log();
}

/* Run the most recently recorded dispatch callback, if any. */
static void run_last_cb(void) {
    void (*cb)(void *) = g_last_dispatch_cb;
    void *ctx = g_last_dispatch_ctx;
    g_last_dispatch_cb = NULL;
    g_last_dispatch_ctx = NULL;
    if (cb) cb(ctx);
}

/* ------------------------------------------------------------------ */
/* Fake presenter server (adapted from present_test.c)                */
/* ------------------------------------------------------------------ */

enum {
    SRV_NORMAL = 1,
    SRV_NO_ACK,
};

typedef struct {
    int mode;
    int listen_fd;
    pthread_t thread;
    volatile int stop;
    volatile int conn_fd;
    int hellos;
    uint32_t hello_w[4];
    uint32_t hello_h[4];
    uint32_t hello_stride[4];
    int hello_fd_ok[4];
    volatile int presents_seen;
    uint64_t present_frame_ids[128];
    int present_buf[128];
    volatile int present_dmg_count[128];
    isws_rect_t present_dmg0[128];
    volatile int goodbye_seen;
    volatile int accepted;
    uint8_t *map;
    size_t map_len;
    int map_fd;
} srv_ctx_t;

static int srv_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int srv_send_msg(int fd, uint16_t type, const uint8_t *payload,
                        uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    if (srv_send_all(fd, hdr, sizeof(hdr)) != 0) return -1;
    if (len > 0 && srv_send_all(fd, payload, len) != 0) return -1;
    return 0;
}

static int srv_recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int srv_read_header(int fd, uint16_t *type, uint32_t *len,
                           int *passed_fd) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    uint8_t cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
    struct msghdr mh;
    size_t got = 0;
    *passed_fd = -1;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    while (got < sizeof(hdr)) {
        ssize_t n;
        iov.iov_base = hdr + got;
        iov.iov_len = sizeof(hdr) - got;
        mh.msg_controllen = sizeof(cbuf);
        n = recvmsg(fd, &mh, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c;
             c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET &&
                c->cmsg_type == SCM_RIGHTS) {
                if (*passed_fd >= 0) close(*passed_fd);
                memcpy(passed_fd, CMSG_DATA(c), sizeof(*passed_fd));
            }
        }
        got += (size_t)n;
    }
    if (isws_decode_header(hdr, sizeof(hdr), type, NULL, len) != 0)
        return -1;
    return 0;
}

static void *srv_thread(void *arg) {
    srv_ctx_t *ctx = (srv_ctx_t *)arg;
    while (!ctx->stop && ctx->hellos < 4) {
        struct pollfd pfd;
        int pr;
        int fd;
        pfd.fd = ctx->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;
        fd = accept(ctx->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        ctx->accepted = 1;
        ctx->conn_fd = fd;
        {
            uint16_t type;
            uint32_t len;
            int pfd2 = -1;
            uint8_t payload[ISWS_HELLO_PAYLOAD_SIZE];
            isws_hello_t hello;
            struct stat st;
            int hi = ctx->hellos;
            if (srv_read_header(fd, &type, &len, &pfd2) != 0) goto conn_done;
            if (type != ISWS_MSG_HELLO || len != ISWS_HELLO_PAYLOAD_SIZE)
                goto conn_done;
            if (srv_recv_exact(fd, payload, len) != 0) goto conn_done;
            if (isws_decode_hello(payload, len, &hello) != 0) goto conn_done;
            ctx->hello_w[hi] = hello.width;
            ctx->hello_h[hi] = hello.height;
            ctx->hello_stride[hi] = hello.stride;
            ctx->hello_fd_ok[hi] = (pfd2 >= 0);
            ctx->hellos++;
            if (pfd2 >= 0) {
                size_t want = (size_t)hello.stride * hello.height * 2;
                if (fstat(pfd2, &st) == 0 && st.st_size >= (off_t)want) {
                    if (ctx->map) {
                        munmap(ctx->map, ctx->map_len);
                        ctx->map = NULL;
                    }
                    if (ctx->map_fd >= 0) close(ctx->map_fd);
                    ctx->map_fd = pfd2;
                    ctx->map_len = want;
                    ctx->map = mmap(NULL, want, PROT_READ, MAP_SHARED,
                                    pfd2, 0);
                    if (ctx->map == MAP_FAILED) ctx->map = NULL;
                } else {
                    close(pfd2);
                }
            }
            {
                isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                                    .max_in_flight = ISWS_MAX_IN_FLIGHT };
                uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
                size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
                if (srv_send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn) != 0)
                    goto conn_done;
            }
            for (;;) {
                uint8_t pay[ISWS_MAX_PAYLOAD];
                isws_present_t pr;
                if (srv_read_header(fd, &type, &len, &pfd2) != 0) {
                    if (pfd2 >= 0) close(pfd2);
                    break;
                }
                if (pfd2 >= 0) close(pfd2);
                if (len > 0 && srv_recv_exact(fd, pay, len) != 0) break;
                if (type == ISWS_MSG_GOODBYE) {
                    ctx->goodbye_seen = 1;
                    break;
                }
                if (type != ISWS_MSG_PRESENT) break;
                if (isws_decode_present(pay, len, &pr) != 0) break;
                {
                    /* Ring of the last 128 presents: the 1000-frame
                     * test overflows a flat array, the damage checks
                     * only need the most recent records. */
                    int idx = ctx->presents_seen % 128;
                    ctx->present_frame_ids[idx] = pr.frame_id;
                    ctx->present_buf[idx] = (int)pr.buffer_index;
                    ctx->present_dmg_count[idx] = (int)pr.damage_count;
                    if (pr.damage_count > 0)
                        ctx->present_dmg0[idx] = pr.damage[0];
                    ctx->presents_seen++;
                }
                if (ctx->mode == SRV_NORMAL) {
                    isws_frame_done_t d = {
                        .frame_id = pr.frame_id,
                        .vsync_ns = 1000000000LL +
                            (int64_t)pr.frame_id * 16666667LL,
                        .status = 0, .reserved = 0 };
                    uint8_t db[ISWS_FRAME_DONE_PAYLOAD_SIZE];
                    size_t dn = isws_encode_frame_done(db, sizeof(db), &d);
                    if (srv_send_msg(fd, ISWS_MSG_FRAME_DONE, db,
                                     (uint32_t)dn) != 0)
                        break;
                }
            }
        }
    conn_done:
        close(fd);
        ctx->conn_fd = -1;
    }
    return NULL;
}

static int srv_start(srv_ctx_t *ctx, int mode, const char *name) {
    struct sockaddr_un addr;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = mode;
    ctx->map_fd = -1;
    ctx->conn_fd = -1;
    ctx->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, strlen(name));
    {
        size_t alen = (size_t)((char *)&addr.sun_path - (char *)&addr) +
                      1 + strlen(name);
        if (bind(ctx->listen_fd, (struct sockaddr *)&addr,
                 (socklen_t)alen) != 0) {
            close(ctx->listen_fd);
            return -1;
        }
    }
    if (listen(ctx->listen_fd, 4) != 0) {
        close(ctx->listen_fd);
        return -1;
    }
    if (pthread_create(&ctx->thread, NULL, srv_thread, ctx) != 0) {
        close(ctx->listen_fd);
        return -1;
    }
    return 0;
}

static void srv_stop(srv_ctx_t *ctx) {
    ctx->stop = 1;
    if (ctx->conn_fd >= 0) shutdown(ctx->conn_fd, SHUT_RDWR);
    if (ctx->listen_fd >= 0) {
        shutdown(ctx->listen_fd, SHUT_RDWR);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    pthread_join(ctx->thread, NULL);
    if (ctx->map) {
        munmap(ctx->map, ctx->map_len);
        ctx->map = NULL;
    }
    if (ctx->map_fd >= 0) {
        close(ctx->map_fd);
        ctx->map_fd = -1;
    }
}

/* Wait until present_last_frame_id() reaches want (or timeout). */
static int wait_last_frame_id(uint64_t want, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (present_last_frame_id() == want) return 0;
        (void)present_pump();
        sleep_ms(2);
    }
    return present_last_frame_id() == want ? 0 : -1;
}

static uint32_t srv_pixel(srv_ctx_t *ctx, int buf, uint32_t stride,
                          uint32_t h, uint32_t x, uint32_t y) {
    uint32_t v = 0;
    const uint8_t *p;
    if (!ctx->map) return 0;
    p = ctx->map + (size_t)buf * stride * h + (size_t)y * stride +
        (size_t)x * 4;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* Test sections                                                      */
/* ------------------------------------------------------------------ */

static void test_stride_contract(void) {
    static const uint32_t widths[] = { 1, 2, 3, 90, 180, 360, 720, 1080,
                                       1440, 1920, 4096, 8192 };
    size_t i;
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        uint32_t w = widths[i];
        uint32_t tight = present_stride_for_width(w);
        uint32_t aligned = (w * 4u + 63u) & ~63u;
        CHECK_EQ_U64(tight, (uint64_t)w * 4u);
        CHECK(aligned >= tight);
        CHECK(aligned - tight < 64u);
    }
    /* Known real geometries. */
    CHECK_EQ_U64(present_stride_for_width(1080), 4320u);
    CHECK_EQ_U64((1080u * 4u + 63u) & ~63u, 4352u);
    CHECK_EQ_U64(present_stride_for_width(1440), 5760u);
    CHECK_EQ_U64((1440u * 4u + 63u) & ~63u, 5760u);
    CHECK_EQ_U64(present_stride_for_width(720), 2880u);
    CHECK_EQ_U64((720u * 4u + 63u) & ~63u, 2880u);
    CHECK_EQ_U64(present_stride_for_width(0), 0u);
    CHECK_EQ_U64(present_stride_for_width(8193), 0u);
}

static void test_stride_fuzz(void) {
    int i;
    for (i = 0; i < 2000; i++) {
        uint32_t w = 1 + (uint32_t)(rng_next() % 8192u);
        uint32_t h = 1 + (uint32_t)(rng_next() % 8192u);
        uint32_t tight = present_stride_for_width(w);
        uint32_t aligned = (w * 4u + 63u) & ~63u;
        uint64_t tight_bytes = (uint64_t)tight * h;
        uint64_t aligned_bytes = (uint64_t)aligned * h;
        CHECK(tight == w * 4u);
        CHECK(aligned >= tight && aligned - tight < 64u);
        CHECK(tight_bytes <= aligned_bytes);
        /* Row-copy reference: copy h rows of w*4 bytes from an
         * aligned-stride source into a tight-stride destination
         * and check a random pixel lands at the same (x, y).
         * Only for small frames, to keep the fuzz fast. */
        if ((i % 10) == 0 && (uint64_t)w * h <= 4000000u) {
            uint32_t x = (uint32_t)(rng_next() % w);
            uint32_t y = (uint32_t)(rng_next() % h);
            uint8_t *src = malloc(aligned_bytes ? aligned_bytes : 1);
            uint8_t *dst = malloc(tight_bytes ? tight_bytes : 1);
            uint32_t want = 0x11223344u;
            uint32_t got = 0;
            CHECK(src != NULL && dst != NULL);
            if (src && dst) {
                size_t r;
                memset(src, 0xA5, aligned_bytes);
                memset(dst, 0, tight_bytes);
                memcpy(src + (size_t)y * aligned + (size_t)x * 4,
                       &want, sizeof(want));
                for (r = 0; r < h; r++) {
                    memcpy(dst + (size_t)r * tight,
                           src + (size_t)r * aligned, (size_t)w * 4u);
                }
                memcpy(&got, dst + (size_t)y * tight + (size_t)x * 4,
                       sizeof(got));
                CHECK_EQ_U64(got, want);
                /* No shear: the byte after the row end in the
                 * destination still belongs to the next row. */
                if (y + 1 < h) {
                    uint32_t next_row_first = 0;
                    memcpy(&next_row_first,
                           dst + (size_t)(y + 1) * tight, sizeof(uint32_t));
                    CHECK_EQ_U64(next_row_first, 0xA5A5A5A5u);
                }
            }
            free(src);
            free(dst);
        }
    }
}

static void test_snapshot_modes(void) {
    /* Default pump: arms the 1 s dispatch_after chain. */
    reset_snapshot_state();
    setup_mock_window(360.0, 800.0, 3.0, 0xff112233u);
    g_env_snapshot = "1";
    snapshot_init();
    CHECK_EQ_I64(g_dispatch_after_calls, 1);
    CHECK_EQ_U64(g_last_dispatch_when, 1000000000ULL);
    CHECK(strstr(g_log_buf, "[snapshot] pump armed") != NULL);
    CHECK(snapshot_frame_pending() == 0);

    /* One on-demand request: exactly one extra dispatch, one frame. */
    snapshot_request_frame();
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    CHECK_EQ_U64(g_last_dispatch_when, 0u);
    CHECK(snapshot_frame_pending() != 0);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 1);
    CHECK(snapshot_frame_pending() == 0);

    /* Five requests before the queued tick runs: one frame. */
    reset_snapshot_state();
    setup_mock_window(360.0, 800.0, 3.0, 0xff112233u);
    g_env_snapshot = "1";
    g_env_pump = "0";
    snapshot_init();
    CHECK_EQ_I64(g_dispatch_after_calls, 0);
    CHECK(strstr(g_log_buf, "[snapshot] pump off") != NULL);
    snapshot_request_frame();
    snapshot_request_frame();
    snapshot_request_frame();
    snapshot_request_frame();
    snapshot_request_frame();
    CHECK_EQ_I64(g_dispatch_after_calls, 1);
    CHECK(snapshot_frame_pending() != 0);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 1);
    /* The coalesced request is drained by exactly one more tick. */
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 2);
    CHECK(snapshot_frame_pending() == 0);

    /* A request issued mid-render is drained by one more tick. */
    reset_snapshot_state();
    setup_mock_window(360.0, 800.0, 3.0, 0xff112233u);
    g_env_snapshot = "1";
    g_env_pump = "0";
    snapshot_init();
    g_request_during_render = 1;
    snapshot_request_frame();
    CHECK_EQ_I64(g_dispatch_after_calls, 1);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 1);
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 2);
    CHECK(snapshot_frame_pending() == 0);

    /* Shutdown: a tick queued before shutdown is a no-op, and
     * re-init re-arms. */
    reset_snapshot_state();
    setup_mock_window(360.0, 800.0, 3.0, 0xff112233u);
    g_env_snapshot = "1";
    snapshot_init();
    snapshot_request_frame();
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    snapshot_shutdown();
    CHECK(snapshot_frame_pending() == 0);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 0);
    snapshot_request_frame();
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    snapshot_init();
    CHECK_EQ_I64(g_dispatch_after_calls, 3);
    snapshot_request_frame();
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 1);
    snapshot_shutdown();
}

/* Concurrent request hammering: 8 threads x 250
 * snapshot_request_frame() calls with the mock main queue stalled
 * must dispatch exactly one tick; the queued tick renders once,
 * and the coalesced request drains with exactly one more tick.
 * 2000 requests therefore produce exactly 2 frames, none lost,
 * none duplicated, and every thread joins (no deadlock in the
 * atomic coalescer). */
typedef struct {
    int n;
} hammer_arg_t;

static void *hammer_worker(void *arg) {
    hammer_arg_t *a = (hammer_arg_t *)arg;
    int i;
    for (i = 0; i < a->n; i++) snapshot_request_frame();
    return NULL;
}

static void test_request_frame_concurrent(void) {
    const int nthreads = 8;
    const int per_thread = 250;
    pthread_t th[8];
    hammer_arg_t args[8];
    int i;
    int joined = 0;
    int64_t deadline;

    reset_snapshot_state();
    setup_mock_window(360.0, 800.0, 3.0, 0xff112233u);
    g_env_snapshot = "1";
    g_env_pump = "0";
    snapshot_init();
    CHECK_EQ_I64(g_dispatch_after_calls, 0);

    for (i = 0; i < nthreads; i++) {
        args[i].n = per_thread;
        CHECK(pthread_create(&th[i], NULL, hammer_worker, &args[i]) == 0);
    }
    /* Join with a deadline: a hang in the coalescer must fail the
     * test, not hang the suite (the harness timeout is the backstop). */
    deadline = now_ms() + 10000;
    while (joined < nthreads && now_ms() < deadline) {
        for (i = 0; i < nthreads; i++) {
            if (th[i] != 0 && pthread_tryjoin_np(th[i], NULL) == 0) {
                th[i] = 0;
                joined++;
            }
        }
        if (joined < nthreads) sleep_ms(5);
    }
    CHECK_EQ_I64(joined, nthreads);
    CHECK_EQ_I64(g_dispatch_after_calls, 1);
    CHECK(snapshot_frame_pending() != 0);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 1);
    CHECK_EQ_I64(g_dispatch_after_calls, 2);
    run_last_cb();
    CHECK_EQ_I64(g_render_count, 2);
    CHECK(snapshot_frame_pending() == 0);
    /* Frame source accounting: both frames are pump ticks, and the
     * per-source totals add up to the frame counter. */
    {
        uint64_t pf = 0, cf = 0;
        snapshot_frame_counts(&pf, &cf);
        CHECK_EQ_U64(pf, 2u);
        CHECK_EQ_U64(cf, 0u);
        CHECK_EQ_U64(snapshot_frame_no(), 2u);
    }
    CHECK(strstr(g_log_buf, "src=pump") != NULL);
    CHECK(strstr(g_log_buf, "[snapshot] frame 1 1080x2400 windows=1 rc=-1 src=pump") != NULL);
    CHECK(strstr(g_log_buf, "[snapshot] frame 2 1080x2400 windows=1 rc=-1 src=pump") != NULL);
    snapshot_shutdown();
}

static void test_present_api(srv_ctx_t *srv, const char *sock_name) {
    int fds0, fds1, fds2, fds3;
    uint8_t *frame = NULL;
    int i;

    CHECK(srv_start(srv, SRV_NORMAL, sock_name) == 0);
    present_test_set_socket_name(sock_name);
    fds0 = count_fds();

    frame = malloc(320u * 240u * 4u);
    CHECK(frame != NULL);
    if (!frame) return;
    memset(frame, 0, 320u * 240u * 4u);

    /* Frame 0: pixel-exact copy into buffer 0. */
    {
        uint32_t *px = (uint32_t *)frame;
        px[0] = 0xAABBCCDDu;
        px[10 + 20 * 320] = 0x11223344u;
    }
    CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == 0);
    CHECK(wait_last_frame_id(0, 2000) == 0);
    CHECK_EQ_U64(srv_pixel(srv, 0, 1280, 240, 0, 0), 0xAABBCCDDu);
    CHECK_EQ_U64(srv_pixel(srv, 0, 1280, 240, 10, 20), 0x11223344u);
    CHECK_EQ_U64(srv_pixel(srv, 0, 1280, 240, 319, 239), 0u);
    fds1 = count_fds();
    CHECK(fds1 > 0);

    /* 1000 frames: no fd growth, exact frame ids. */
    for (i = 1; i < 1000; i++) {
        uint32_t fill = 0xFF000000u | (uint32_t)i;
        uint32_t *px = (uint32_t *)frame;
        size_t n;
        for (n = 0; n < 320u * 240u; n++) px[n] = fill;
        CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == 0);
        if (i % 100 == 0) {
            (void)present_pump();
        }
    }
    CHECK(wait_last_frame_id(999, 5000) == 0);
    CHECK_EQ_U64(present_next_frame_id(), 1000u);
    CHECK_EQ_U64(present_last_frame_id(), 999u);
    CHECK_EQ_U64(srv_pixel(srv, 1, 1280, 240, 0, 0), 0xFF000000u | 999u);
    fds2 = count_fds();
    CHECK_EQ_I64(fds2, fds1);

    /* Geometry change is deliberate, not implicit: shut the session
     * down, then present at the new size for a fresh HELLO. */
    ioswine_present_shutdown();
    {
        uint8_t *big = malloc(640u * 480u * 4u);
        int hellos_before = srv->hellos;
        CHECK(big != NULL);
        if (big) {
            memset(big, 0x77, 640u * 480u * 4u);
            CHECK(ioswine_present_frame(big, 640, 480, 640 * 4u) == 0);
            CHECK(ioswine_present_frame(big, 640, 480, 640 * 4u) == 0);
            CHECK(ioswine_present_frame(big, 640, 480, 640 * 4u) == 0);
            CHECK_EQ_I64(srv->hellos, hellos_before + 1);
            CHECK_EQ_U64(srv->hello_w[hellos_before], 640u);
            CHECK_EQ_U64(srv->hello_h[hellos_before], 480u);
            CHECK_EQ_U64(srv->hello_stride[hellos_before], 2560u);
            free(big);
        }
    }

    /* Damage clipping on the 640x480 session. */
    {
        uint8_t *big = malloc(640u * 480u * 4u);
        isws_rect_t r;
        int before;
        CHECK(big != NULL);
        if (big) {
            memset(big, 0x11, 640u * 480u * 4u);
            r.x = 10; r.y = 20; r.w = 30; r.h = 40;
            CHECK(ioswine_present_frame_damage(big, 640, 480, 640 * 4u,
                                               &r, 1) == 0);
            CHECK(wait_last_frame_id(present_next_frame_id() - 1,
                                     2000) == 0);
            /* Slot of the just-acked present. Sampling presents_seen
             * BEFORE the present raced with the server thread: when
             * the server lagged one present behind, the damage
             * landed in slot before+1 while the test read slot
             * before.
             * Presents here are strictly sequential, so after the
             * ack the server has recorded this present and no other
             * is in flight. */
            before = srv->presents_seen - 1;
            CHECK_EQ_I64(srv->present_dmg_count[before % 128], 1);
            CHECK_EQ_I64(srv->present_dmg0[before % 128].x, 10);
            CHECK_EQ_I64(srv->present_dmg0[before % 128].y, 20);
            CHECK_EQ_U64(srv->present_dmg0[before % 128].w, 30u);
            CHECK_EQ_U64(srv->present_dmg0[before % 128].h, 40u);

            r.x = -10; r.y = -5; r.w = 30; r.h = 40;
            CHECK(ioswine_present_frame_damage(big, 640, 480, 640 * 4u,
                                               &r, 1) == 0);
            CHECK(wait_last_frame_id(present_next_frame_id() - 1,
                                     2000) == 0);
            before = srv->presents_seen - 1;
            CHECK_EQ_I64(srv->present_dmg_count[before % 128], 1);
            CHECK_EQ_I64(srv->present_dmg0[before % 128].x, 0);
            CHECK_EQ_I64(srv->present_dmg0[before % 128].y, 0);
            CHECK_EQ_U64(srv->present_dmg0[before % 128].w, 20u);
            CHECK_EQ_U64(srv->present_dmg0[before % 128].h, 35u);

            r.x = 5000; r.y = 5000; r.w = 10; r.h = 10;
            CHECK(ioswine_present_frame_damage(big, 640, 480, 640 * 4u,
                                               &r, 1) == 0);
            CHECK(wait_last_frame_id(present_next_frame_id() - 1,
                                     2000) == 0);
            before = srv->presents_seen - 1;
            CHECK_EQ_I64(srv->present_dmg_count[before % 128], 0);

            {
                isws_rect_t many[20];
                int k;
                for (k = 0; k < 20; k++) {
                    many[k].x = 0; many[k].y = 0;
                    many[k].w = 1; many[k].h = 1;
                }
                CHECK(ioswine_present_frame_damage(big, 640, 480,
                                                   640 * 4u, many,
                                                   20) == 0);
                CHECK(wait_last_frame_id(present_next_frame_id() - 1,
                                         2000) == 0);
                before = srv->presents_seen - 1;
                CHECK_EQ_I64(srv->present_dmg_count[before % 128], 16);
            }
            free(big);
        }
    }

    /* Flush with a live server: all acked. Presenting 320x240 on
     * the 640x480 session clips to the session geometry (no
     * re-init); the frame must still succeed and flush clean. */
    {
        CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == 0);
        CHECK(ioswine_present_flush(1000) == 0);
    }

    /* Disconnect + backoff: after the server goes away, every call
     * fails fast (connect refused, no 80 ms READY wait per frame). */
    srv_stop(srv);
    {
        int64_t t0;
        int k;
        CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == -1);
        t0 = now_ms();
        for (k = 0; k < 5; k++) {
            int64_t a = now_ms();
            CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == -1);
            CHECK(now_ms() - a < 50);
        }
        CHECK(now_ms() - t0 < 250);
    }
    ioswine_present_shutdown();
    CHECK(present_connected() == 0);
    fds3 = count_fds();
    CHECK(fds3 <= fds0 + 1);
    free(frame);
}

static void test_noack_flush(const char *sock_name) {
    srv_ctx_t srv;
    uint8_t *frame = malloc(320u * 240u * 4u);
    CHECK(frame != NULL);
    if (!frame) return;
    CHECK(srv_start(&srv, SRV_NO_ACK, sock_name) == 0);
    present_test_set_socket_name(sock_name);
    memset(frame, 0x55, 320u * 240u * 4u);
    CHECK(ioswine_present_frame(frame, 320, 240, 320 * 4u) == 0);
    /* No FRAME_DONE ever arrives: flush times out. */
    CHECK(ioswine_present_flush(50) == 1);
    ioswine_present_shutdown();
    CHECK(present_connected() == 0);
    srv_stop(&srv);
    free(frame);
}

int main(void) {
    char name1[64];
    char name2[64];
    srv_ctx_t srv;

    snprintf(name1, sizeof(name1), "pipe-test-normal-%d", (int)getpid());
    snprintf(name2, sizeof(name2), "pipe-test-noack-%d", (int)getpid());
    /* The snapshot sections present into the void: give them a
     * socket name nobody listens on, so connects fail fast. */
    present_test_set_socket_name(name1);

    test_stride_contract();
    test_stride_fuzz();
    test_snapshot_modes();
    test_request_frame_concurrent();

    test_present_api(&srv, name1);
    test_noack_flush(name2);

    if (g_failures == 0) {
        printf("snapshot_pipeline_test: all %d checks passed\n", g_checks);
        return 0;
    }
    printf("snapshot_pipeline_test: %d checks, %d FAILURES\n", g_checks,
           g_failures);
    return 1;
}
