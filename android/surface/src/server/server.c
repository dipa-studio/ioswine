/*
 * ioswine-surface server: minimal NativeActivity APK, no Java of our own.
 *
 * - Owns a fullscreen ANativeWindow.
 * - Listens on abstract unix socket "@ioswine-surface".
 * - Client sends HELLO (+ memfd via SCM_RIGHTS, 2 buffers), then PRESENT.
 * - Server copies damage rects into the window (BGRA -> RGBA swizzle,
 *   aspect-fit letterbox, nearest-neighbour scale) and acks FRAME_DONE
 *   with the last AChoreographer vsync timestamp.
 * - Input (multi-touch, back key), resize/rotation and focus are sent
 *   back to the client as TOUCH / KEY / INFO / FOCUS messages.
 *
 * Primary presentation path: ANativeWindow_lock + CPU copy. There is no
 * public BGRA window format (WINDOW_FORMAT_* has no BGRA entry), so the
 * swizzle is unavoidable on this path. See server_egl.c for the
 * experimental (compile-time off) EGL texture-upload path.
 *
 * Logging: __android_log_print, tag "ioswine-surface", no per-frame spam;
 * one line per second of stats, one line per client connect/disconnect.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/choreographer.h>
#include <android/configuration.h>
#include <android_native_app_glue.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
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

#include "protocol.h"
#include "touch_input.h"
#include "sysinfo.h"
#include "ui.h"

#define LOG_TAG "ioswine-surface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t draw_lock;     /* outer lock: guards ANativeWindow_lock..unlockAndPost & mem unmap */
    pthread_mutex_t lock;          /* guards state below (never acquire draw_lock while holding lock) */
    pthread_mutex_t send_lock;     /* guards writes to client_fd */

    struct android_app *app;
    ANativeWindow *window;         /* NULL while no window */
    int32_t window_w, window_h;    /* last known window size */
    uint32_t rotation;             /* 0/90/180/270, from configuration */
    bool window_format_logged;

    /* Client session (one at a time). */
    int client_fd;                 /* -1 = none */
    int32_t client_pid;            /* from SO_PEERCRED */
    bool have_hello;
    isws_hello_t hello;
    int memfd;                     /* -1 = none */
    uint8_t *mem;                  /* mmap of memfd, or NULL */
    size_t mem_size;
    bool in_flight[ISWS_NUM_BUFFERS];
    bool have_last_frame;
    uint32_t last_frame_buf_idx;

    isws_info_t mapping;           /* valid once have_hello && window */
    bool mapping_valid;

    int64_t last_vsync_ns;         /* AChoreographer, 0 = none yet */

    /* Stats, updated under lock, logged once a second. */
    uint64_t frames_presented;
    uint64_t frames_dropped;
    uint64_t bytes_copied;
    uint64_t touches_sent;
    int64_t stats_last_log_ns;
    int64_t session_start_ns;
    int64_t fps_win_start_ns;
    uint64_t fps_win_frames;
    double current_fps;

    /* Overlay state (when client connected). */
    bool overlay_visible;
    int overlay_dirty_frames;

    /* Launcher state (when no client connected). */
    isws_sysinfo_t sysinfo;
    ui_view_t ui_view;
    ui_layout_t ui_layout;
    bool launching;
    bool stopping;
    char status_msg[128];
    bool have_session_stats;
    uint64_t last_session_frames;
    uint64_t last_session_dropped;
    int64_t last_session_duration_s;

    /* Input gesture state (looper thread only). */
    ui_action_t down_action;
    bool overlay_touch_captured;
    bool two_finger_candidate;
    int64_t two_finger_t0_ns;
    float two_finger_x0, two_finger_y0;
    float two_finger_x1, two_finger_y1;
    int64_t back_down_ns;
    bool back_long_triggered;

    /* Probe worker signaling. */
    pthread_mutex_t probe_lock;
    pthread_cond_t probe_cond;
    bool probe_requested;

    atomic_bool running;
} server_state_t;

static server_state_t g_s;

static void request_sysinfo_refresh(void);
static void draw_launcher_screen(void);
static void redraw_last_frame(void);
static void start_launch_async(void);
static void start_stop_async(void);

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send one framed message (header + payload). Caller holds no lock. */
static int send_msg(int fd, uint16_t type, const uint8_t *payload, uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    pthread_mutex_lock(&g_s.send_lock);
    int r = send_all(fd, hdr, sizeof(hdr));
    if (r == 0 && len > 0) r = send_all(fd, payload, len);
    pthread_mutex_unlock(&g_s.send_lock);
    return r;
}

static int send_error(int fd, uint32_t code, const char *msg) {
    uint8_t buf[256];
    size_t n = isws_encode_error(buf, sizeof(buf), code, msg, strlen(msg));
    return send_msg(fd, ISWS_MSG_ERROR, buf, (uint32_t)n);
}

/* Read exactly len bytes, or -1 on EOF/error. */
static int read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* EOF */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Presentation                                                        */
/*                                                                     */
/* Window format is WINDOW_FORMAT_RGBA_8888 (no public BGRA window    */
/* format exists), so every copied pixel is swizzled BGRA -> RGBA.    */
/* Damage rects are scaled nearest-neighbour into the letterboxed     */
/* destination rect. Letterbox bars are filled black on each present.  */
/* ------------------------------------------------------------------ */

/* Forward declarations: present_frame (below) refreshes the mapping
 * and re-sends INFO, but the mapping helpers live further down. */
static void refresh_mapping_locked(bool *send_info, isws_info_t *out_info);
static void send_info_unlocked(int fd, const isws_info_t *info);
static void log_mapping(const char *when, const isws_info_t *m);
static void log_current_mapping(const char *when);

/* Copy one client row range into the window buffer with swizzle+scale.
 * src: client buffer base; dst: window bits; both described by strides
 * in pixels. dr/dh: destination rect (window px). sr: source rect.
 * The destination rect comes from isws_dest_rect, the same rect
 * fill_bars uses, so the blit and the bars can never disagree by a
 * pixel. Source sampling is integer nearest-neighbour via
 * isws_nn_index: exact identity at 1:1, no float truncation dropping
 * the last column or row (019). Pixels are converted with
 * isws_bgra_to_rgba_opaque, so a transparent source pixel becomes
 * white instead of black. */
static void blit_rect(const uint8_t *src, uint32_t src_stride_px,
                      uint32_t *dst, int32_t dst_stride_px,
                      const isws_info_t *map, const isws_rect_t *sr) {
    int32_t fx0, fy0, fx1, fy1;
    int32_t dx0, dy0, dx1, dy1;
    int32_t fw, fh;
    if (dst_stride_px <= 0 || !dst || !src || sr->w == 0 || sr->h == 0) return;
    if (map->client_w == 0 || map->client_h == 0) return;
    isws_dest_rect(map, &fx0, &fy0, &fx1, &fy1);
    fw = fx1 - fx0;
    fh = fy1 - fy0;
    if (fw <= 0 || fh <= 0) return;
    /* Damage rect (client px) mapped onto the destination rect. The
     * damage rects are already clipped to the frame by
     * isws_expand_damage; the clamps below are defensive. */
    dx0 = fx0 + (int32_t)((int64_t)sr->x * fw / (int64_t)map->client_w);
    dx1 = fx0 + (int32_t)((int64_t)(sr->x + (int32_t)sr->w) * fw / (int64_t)map->client_w);
    dy0 = fy0 + (int32_t)((int64_t)sr->y * fh / (int64_t)map->client_h);
    dy1 = fy0 + (int32_t)((int64_t)(sr->y + (int32_t)sr->h) * fh / (int64_t)map->client_h);
    if (dx0 < fx0) dx0 = fx0;
    if (dy0 < fy0) dy0 = fy0;
    if (dx1 > fx1) dx1 = fx1;
    if (dy1 > fy1) dy1 = fy1;
    if (dx1 <= dx0 || dy1 <= dy0) return;

    for (int32_t dy = dy0; dy < dy1; dy++) {
        /* Nearest neighbour: source row for this destination row. */
        int32_t sy = isws_nn_index(dy, dy0, dy1 - dy0, sr->y, (int32_t)sr->h);
        const uint32_t *srow;
        uint32_t *drow;
        if (sy < 0) sy = 0;
        if (sy >= (int32_t)map->client_h) sy = (int32_t)map->client_h - 1;
        srow = (const uint32_t *)(src + (size_t)sy * src_stride_px * 4u);
        drow = dst + (size_t)dy * (size_t)dst_stride_px;
        for (int32_t dx = dx0; dx < dx1; dx++) {
            int32_t sx = isws_nn_index(dx, dx0, dx1 - dx0, sr->x, (int32_t)sr->w);
            if (sx < 0) sx = 0;
            if (sx >= (int32_t)map->client_w) sx = (int32_t)map->client_w - 1;
            drow[dx] = isws_bgra_to_rgba_opaque(srow[sx]);
        }
    }
}

static void fill_bars(uint32_t *dst, int32_t stride_px, const isws_info_t *map) {
    int32_t fx0, fy0, fx1, fy1;
    int32_t sw, sh;
    if (stride_px <= 0 || !dst) return;
    /* Same integer rect as blit_rect (019): the bars start exactly
     * where the frame ends, so no 1 px black line or stale line can
     * appear between them. */
    isws_dest_rect(map, &fx0, &fy0, &fx1, &fy1);
    sw = (int32_t)map->screen_w;
    sh = (int32_t)map->screen_h;
    if (sw > stride_px) sw = stride_px;
    if (fx0 < 0) fx0 = 0;
    if (fy0 < 0) fy0 = 0;
    if (fx1 > sw) fx1 = sw;
    if (fy1 > sh) fy1 = sh;
    if (fx0 > sw) fx0 = sw;
    if (fy0 > sh) fy0 = sh;

    /* Top and bottom bars. */
    for (int32_t y = 0; y < fy0; y++)
        memset(dst + (size_t)y * stride_px, 0, (size_t)sw * 4);
    for (int32_t y = fy1; y < sh; y++)
        memset(dst + (size_t)y * stride_px, 0, (size_t)sw * 4);
    /* Left and right bars (only the frame rows). */
    for (int32_t y = fy0; y < fy1; y++) {
        if (fx0 > 0) memset(dst + (size_t)y * stride_px, 0, (size_t)fx0 * 4);
        if (fx1 < sw)
            memset(dst + (size_t)y * stride_px + fx1, 0,
                   (size_t)(sw - fx1) * 4);
    }
}

static void fill_overlay_state_locked(ui_overlay_state_t *ov, int64_t now) {
    ov->fps = g_s.current_fps;
    ov->frames_presented = g_s.frames_presented;
    ov->frames_dropped = g_s.frames_dropped;
    ov->touches_sent = g_s.touches_sent;
    ov->uptime_s = g_s.session_start_ns > 0
                       ? (now - g_s.session_start_ns) / 1000000000LL
                       : 0;
    ov->client_pid = g_s.client_pid;
}

static void draw_launcher_screen(void) {
    pthread_mutex_lock(&g_s.draw_lock);
    pthread_mutex_lock(&g_s.lock);
    if (g_s.have_hello || !g_s.window || g_s.window_w <= 0 || g_s.window_h <= 0) {
        pthread_mutex_unlock(&g_s.lock);
        pthread_mutex_unlock(&g_s.draw_lock);
        return;
    }
    ANativeWindow *win = g_s.window;
    ANativeWindow_acquire(win);
    isws_sysinfo_t sys_snap = g_s.sysinfo;
    ui_launcher_state_t lst = {
        .sys = &sys_snap,
        .view = g_s.ui_view,
        .launching = g_s.launching,
        .stopping = g_s.stopping,
        .status_msg = g_s.status_msg,
        .have_session_stats = g_s.have_session_stats,
        .last_session_frames = g_s.last_session_frames,
        .last_session_dropped = g_s.last_session_dropped,
        .last_session_duration_s = g_s.last_session_duration_s,
    };
    pthread_mutex_unlock(&g_s.lock);

    ANativeWindow_Buffer wbuf;
    ui_layout_t layout;
    memset(&layout, 0, sizeof(layout));
    if (ANativeWindow_lock(win, &wbuf, NULL) == 0) {
        if (wbuf.bits && wbuf.width > 0 && wbuf.height > 0 && wbuf.stride > 0 &&
            wbuf.format == WINDOW_FORMAT_RGBA_8888) {
            ui_draw_launcher((uint32_t *)wbuf.bits, wbuf.width, wbuf.height,
                             wbuf.stride, &lst, &layout);
        }
        ANativeWindow_unlockAndPost(win);
    }
    ANativeWindow_release(win);

    pthread_mutex_lock(&g_s.lock);
    if (!g_s.have_hello) {
        g_s.ui_layout.btn_primary = layout.btn_primary;
        g_s.ui_layout.primary_is_stop = layout.primary_is_stop;
        g_s.ui_layout.btn_refresh = layout.btn_refresh;
        g_s.ui_layout.btn_log = layout.btn_log;
        g_s.ui_layout.btn_log_back = layout.btn_log_back;
    }
    pthread_mutex_unlock(&g_s.lock);
    pthread_mutex_unlock(&g_s.draw_lock);
}

static void redraw_last_frame(void) {
    pthread_mutex_lock(&g_s.draw_lock);
    pthread_mutex_lock(&g_s.lock);
    if (!g_s.have_hello || !g_s.have_last_frame || !g_s.window ||
        !g_s.mapping_valid || !g_s.mem ||
        g_s.last_frame_buf_idx >= ISWS_NUM_BUFFERS) {
        pthread_mutex_unlock(&g_s.lock);
        pthread_mutex_unlock(&g_s.draw_lock);
        return;
    }
    ANativeWindow *win = g_s.window;
    ANativeWindow_acquire(win);
    isws_info_t map = g_s.mapping;
    isws_hello_t hello = g_s.hello;
    uint8_t *mem = g_s.mem;
    uint32_t buf_idx = g_s.last_frame_buf_idx;
    bool ov_vis = g_s.overlay_visible;
    ui_overlay_state_t ov;
    fill_overlay_state_locked(&ov, now_ns());
    pthread_mutex_unlock(&g_s.lock);

    const uint8_t *src = mem + (size_t)buf_idx * (size_t)hello.stride * hello.height;
    ANativeWindow_Buffer wbuf;
    ui_layout_t ov_layout;
    memset(&ov_layout, 0, sizeof(ov_layout));
    if (ANativeWindow_lock(win, &wbuf, NULL) == 0) {
        if (wbuf.bits && wbuf.width > 0 && wbuf.height > 0 && wbuf.stride > 0 &&
            wbuf.format == WINDOW_FORMAT_RGBA_8888) {
            uint32_t *dst = (uint32_t *)wbuf.bits;
            fill_bars(dst, wbuf.stride, &map);
            isws_rect_t full = { 0, 0, hello.width, hello.height };
            blit_rect(src, hello.stride / 4, dst, wbuf.stride, &map, &full);
            if (ov_vis) {
                ui_draw_overlay(dst, wbuf.width, wbuf.height, wbuf.stride,
                                &ov, &ov_layout);
            }
        }
        ANativeWindow_unlockAndPost(win);
    }
    ANativeWindow_release(win);

    if (ov_vis) {
        pthread_mutex_lock(&g_s.lock);
        g_s.ui_layout.overlay_box = ov_layout.overlay_box;
        g_s.ui_layout.btn_overlay_stop = ov_layout.btn_overlay_stop;
        g_s.ui_layout.btn_overlay_hide = ov_layout.btn_overlay_hide;
        pthread_mutex_unlock(&g_s.lock);
    }
    pthread_mutex_unlock(&g_s.draw_lock);
}

/* Log the current mapping if one is valid (snapshot under lock). */
static void log_current_mapping(const char *when) {
    isws_info_t m;
    bool valid;
    pthread_mutex_lock(&g_s.lock);
    m = g_s.mapping;
    valid = g_s.mapping_valid;
    pthread_mutex_unlock(&g_s.lock);
    if (valid) log_mapping(when, &m);
}

/* Present one frame. Called on the socket thread. Returns 0 on success,
 * -1 if there is no window yet (frame is dropped, still acked). */
static int present_frame(const isws_present_t *p, uint64_t *bytes_out) {
    *bytes_out = 0;
    pthread_mutex_lock(&g_s.draw_lock);
    pthread_mutex_lock(&g_s.lock);
    ANativeWindow *win = g_s.window;
    bool have_map = g_s.mapping_valid;
    isws_info_t map = g_s.mapping;
    isws_hello_t hello = g_s.hello;
    uint8_t *mem = g_s.mem;
    bool ov_vis = g_s.overlay_visible;
    int ov_dirty = g_s.overlay_dirty_frames;
    if (ov_dirty > 0) g_s.overlay_dirty_frames--;
    ui_overlay_state_t ov;
    fill_overlay_state_locked(&ov, now_ns());
    if (win) ANativeWindow_acquire(win);
    pthread_mutex_unlock(&g_s.lock);
    if (!win || !have_map || !mem) {
        if (win) ANativeWindow_release(win);
        pthread_mutex_unlock(&g_s.draw_lock);
        return -1;
    }

    const uint8_t *src = mem + (size_t)p->buffer_index *
                         (size_t)hello.stride * hello.height;
    isws_rect_t rects[ISWS_MAX_DAMAGE];
    size_t nrects = isws_expand_damage(p, hello.width, hello.height, rects);

    /* Re-validate the window size (020): the window can grow after
     * creation without an INIT_WINDOW / CONFIG_CHANGED / WINDOW_RESIZED
     * arriving, and the mapping computed from the old size then paints
     * only part of the buffer. ANativeWindow_getWidth/Height is a
     * cheap local query. When it disagrees with the stored size the
     * geometry and the mapping are refreshed here, before the buffer
     * is locked, and INFO is re-sent so the client follows. */
    {
        int32_t win_w = ANativeWindow_getWidth(win);
        int32_t win_h = ANativeWindow_getHeight(win);
        if (win_w > 0 && win_h > 0) {
            bool send_info = false;
            isws_info_t info_snap;
            isws_info_t old_map = map;
            int client_fd = -1;
            int resized = 0;
            pthread_mutex_lock(&g_s.lock);
            if (g_s.window == win &&
                (win_w != g_s.window_w || win_h != g_s.window_h)) {
                resized = 1;
                g_s.window_w = win_w;
                g_s.window_h = win_h;
                ANativeWindow_setBuffersGeometry(g_s.window, win_w, win_h,
                                                 WINDOW_FORMAT_RGBA_8888);
                refresh_mapping_locked(&send_info, &info_snap);
                have_map = g_s.mapping_valid;
                map = g_s.mapping;
                client_fd = g_s.client_fd;
            }
            pthread_mutex_unlock(&g_s.lock);
            if (resized) {
                if (have_map &&
                    memcmp(&map, &old_map, sizeof(map)) != 0)
                    log_mapping("window-resize", &map);
                if (send_info) send_info_unlocked(client_fd, &info_snap);
                if (!have_map) {
                    ANativeWindow_release(win);
                    return -1;
                }
            }
        }
    }

    ANativeWindow_Buffer wbuf;
    if (ANativeWindow_lock(win, &wbuf, NULL) != 0) {
        ANativeWindow_release(win);
        pthread_mutex_unlock(&g_s.draw_lock);
        return -1;
    }
    if (!wbuf.bits || wbuf.width <= 0 || wbuf.height <= 0 || wbuf.stride <= 0 ||
        wbuf.format != WINDOW_FORMAT_RGBA_8888) {
        /* Should not happen: we set RGBA_8888 in setBuffersGeometry.
         * Refuse to write garbage rather than guess the layout. */
        ANativeWindow_unlockAndPost(win);
        ANativeWindow_release(win);
        pthread_mutex_unlock(&g_s.draw_lock);
        LOGW("present: invalid buffer or format %d, frame dropped", wbuf.format);
        return -1;
    }
    uint32_t *dst = (uint32_t *)wbuf.bits;
    /* Second layer (020): the locked buffer's own size is the truth
     * about what is about to be written. If it disagrees with the
     * mapping's screen size (a resize the window-size check above
     * did not see, or a buffer geometry that did not take effect),
     * recompute the mapping from the buffer size and draw this
     * frame with it, so the dest rect always covers the buffer.
     * The stored window size moves to the buffer size as well, so
     * the next frames and the INFO the client sees agree with what
     * was actually drawn. */
    if (wbuf.width > 0 && wbuf.height > 0 &&
        ((uint32_t)wbuf.width != map.screen_w ||
         (uint32_t)wbuf.height != map.screen_h)) {
        isws_info_t eff;
        if (isws_effective_mapping(&map, (uint32_t)wbuf.width,
                                   (uint32_t)wbuf.height, &eff) == 0) {
            bool send_info = false;
            isws_info_t info_snap;
            int client_fd = -1;
            pthread_mutex_lock(&g_s.lock);
            if (g_s.window == win && g_s.mapping_valid &&
                g_s.mapping.client_w == eff.client_w &&
                g_s.mapping.client_h == eff.client_h) {
                g_s.window_w = wbuf.width;
                g_s.window_h = wbuf.height;
                refresh_mapping_locked(&send_info, &info_snap);
                map = g_s.mapping;
                client_fd = g_s.client_fd;
            }
            pthread_mutex_unlock(&g_s.lock);
            if (map.screen_w != (uint32_t)wbuf.width ||
                map.screen_h != (uint32_t)wbuf.height) {
                /* Another thread replaced the mapping meanwhile:
                 * still draw this frame with the buffer-sized one. */
                map = eff;
            } else {
                log_mapping("buffer-size", &map);
            }
            if (send_info) send_info_unlocked(client_fd, &info_snap);
        }
    }
    fill_bars(dst, wbuf.stride, &map);
    if (ov_vis || ov_dirty > 0) {
        /* Blit the full frame when the overlay is shown (or clearing)
         * so SurfaceFlinger's triple-buffered pages never ghost old overlay pixels. */
        isws_rect_t full = { 0, 0, hello.width, hello.height };
        blit_rect(src, hello.stride / 4, dst, wbuf.stride, &map, &full);
        *bytes_out += (uint64_t)hello.width * hello.height * 4u;
    } else {
        for (size_t i = 0; i < nrects; i++) {
            blit_rect(src, hello.stride / 4, dst, wbuf.stride, &map, &rects[i]);
            *bytes_out += (uint64_t)rects[i].w * rects[i].h * 4u;
        }
    }
    ui_layout_t ov_layout;
    memset(&ov_layout, 0, sizeof(ov_layout));
    if (ov_vis) {
        ui_draw_overlay(dst, wbuf.width, wbuf.height, wbuf.stride,
                        &ov, &ov_layout);
    }
    ANativeWindow_unlockAndPost(win);
    ANativeWindow_release(win);

    pthread_mutex_lock(&g_s.lock);
    g_s.have_last_frame = true;
    g_s.last_frame_buf_idx = p->buffer_index;
    if (ov_vis) {
        g_s.ui_layout.overlay_box = ov_layout.overlay_box;
        g_s.ui_layout.btn_overlay_stop = ov_layout.btn_overlay_stop;
        g_s.ui_layout.btn_overlay_hide = ov_layout.btn_overlay_hide;
    }
    pthread_mutex_unlock(&g_s.lock);
    pthread_mutex_unlock(&g_s.draw_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Outbound messages to the client                                     */
/* ------------------------------------------------------------------ */

static void send_info_unlocked(int fd, const isws_info_t *info) {
    if (fd < 0 || !info) return;
    uint8_t buf[ISWS_INFO_PAYLOAD_SIZE];
    size_t n = isws_encode_info(buf, sizeof(buf), info);
    send_msg(fd, ISWS_MSG_INFO, buf, (uint32_t)n);
}

static void send_touch(int fd, uint32_t action, uint32_t pointer_id,
                       uint32_t pointer_count, float x, float y, int64_t ts) {
    isws_touch_t t = { .action = action, .pointer_id = pointer_id,
                       .pointer_count = pointer_count, .x = x, .y = y,
                       .timestamp_ns = ts };
    uint8_t buf[ISWS_TOUCH_PAYLOAD_SIZE];
    size_t n = isws_encode_touch(buf, sizeof(buf), &t);
    send_msg(fd, ISWS_MSG_TOUCH, buf, (uint32_t)n);
}

static void send_key(int fd, uint32_t keycode, uint32_t action, int64_t ts) {
    isws_key_t k = { .keycode = keycode, .action = action, .timestamp_ns = ts };
    uint8_t buf[ISWS_KEY_PAYLOAD_SIZE];
    size_t n = isws_encode_key(buf, sizeof(buf), &k);
    send_msg(fd, ISWS_MSG_KEY, buf, (uint32_t)n);
}

static void send_focus(int fd, uint32_t focused) {
    isws_focus_t f = { .focused = focused, .reserved = 0 };
    uint8_t buf[ISWS_FOCUS_PAYLOAD_SIZE];
    size_t n = isws_encode_focus(buf, sizeof(buf), &f);
    send_msg(fd, ISWS_MSG_FOCUS, buf, (uint32_t)n);
}

/* Touch mapper state. The mapper owns the active-pointer table and
 * the letterbox / CANCEL / history rules (touch_input.{c,h}); on_input
 * below is only a thin adapter from AMotionEvent to ti_motion_t.
 * The mapper is fed with the mapping snapshotted at the start of the
 * input event: if the mapping changed since the last event, the mapper
 * cancels any live contacts at their stale positions first. */
static ti_mapper_t g_ti_mapper;
static int g_ti_mapper_ready;

typedef struct {
    int fd;
} ti_emit_ctx_t;

static void ti_send_touch(void *ctx, const isws_touch_t *t) {
    ti_emit_ctx_t *c = (ti_emit_ctx_t *)ctx;
    if (!c || c->fd < 0 || !t) return;
    pthread_mutex_lock(&g_s.lock);
    g_s.touches_sent++;
    pthread_mutex_unlock(&g_s.lock);
    LOGI("touch sent action=%u x=%.1f y=%.1f", t->action, t->x, t->y);
    send_touch(c->fd, t->action, t->pointer_id, t->pointer_count,
               t->x, t->y, t->timestamp_ns);
}

/* ------------------------------------------------------------------ */
/* IME stub (skeleton only)                               */
/*                                                                     */
/* The presenter is a NativeActivity: it has no View and no Java     */
/* InputConnection, so the Android soft keyboard has nowhere to      */
/* deliver committed text yet. ime_show() / ime_hide() call the     */
/* real NDK show/hide functions (best effort: with no input         */
/* connection the keyboard may not appear), and ime_on_text_input() */
/* is the entry point the follow-up Java InputConnection work will   */
/* call (via JNI) for every commitText / setComposingText /         */
/* deleteSurroundingText callback. Until that follow-up lands,     */
/* send_text() is unused by live code. Do not delete these: the    */
/* client side (keyboard.c) already speaks ISWS_MSG_TEXT.           */
/* ------------------------------------------------------------------ */

static void send_text(int fd, uint32_t kind, uint32_t count,
                      const uint8_t *utf8, size_t utf8_len) {
    isws_text_t t = { .kind = kind, .count = count, .utf8 = utf8,
                      .utf8_len = utf8_len };
    uint8_t buf[ISWS_MAX_PAYLOAD];
    size_t n = isws_encode_text(buf, sizeof(buf), &t);
    if (n > 0) send_msg(fd, ISWS_MSG_TEXT, buf, (uint32_t)n);
}

/* TODO (follow-up task): attach a hidden Java EditText with an
 * InputConnection and call ime_on_text_input() from its JNI
 * callbacks. Without it, typing on the soft keyboard produces no
 * ISWS_MSG_TEXT traffic. */
__attribute__((unused)) static void ime_on_text_input(const char *utf8, size_t len) {
    int fd;
    if (!utf8 || len == 0) return;
    pthread_mutex_lock(&g_s.lock);
    fd = g_s.client_fd;
    pthread_mutex_unlock(&g_s.lock);
    if (fd >= 0) send_text(fd, ISWS_TEXT_COMMIT, (uint32_t)len,
                           (const uint8_t *)utf8, len);
}

__attribute__((unused)) static void ime_on_composing_text(const char *utf8, size_t len) {
    int fd;
    if (!utf8 || len == 0) return;
    pthread_mutex_lock(&g_s.lock);
    fd = g_s.client_fd;
    pthread_mutex_unlock(&g_s.lock);
    if (fd >= 0) send_text(fd, ISWS_TEXT_MARKED, (uint32_t)len,
                           (const uint8_t *)utf8, len);
}

__attribute__((unused)) static void ime_on_delete(int count) {
    int fd;
    if (count <= 0) return;
    pthread_mutex_lock(&g_s.lock);
    fd = g_s.client_fd;
    pthread_mutex_unlock(&g_s.lock);
    if (fd >= 0) send_text(fd, ISWS_TEXT_DELETE, (uint32_t)count, NULL, 0);
}

static void ime_show(void) {
    struct android_app *app;
    pthread_mutex_lock(&g_s.lock);
    app = g_s.app;
    pthread_mutex_unlock(&g_s.lock);
    if (app && app->activity) {
        /* Best effort only: no InputConnection exists yet, so the
         * keyboard may not appear. See the TODO above. */
        ANativeActivity_showSoftInput(app->activity,
                                      ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    }
    LOGI("ime show requested");
}

static void ime_hide(void) {
    struct android_app *app;
    pthread_mutex_lock(&g_s.lock);
    app = g_s.app;
    pthread_mutex_unlock(&g_s.lock);
    if (app && app->activity) {
        ANativeActivity_hideSoftInput(app->activity,
                                      ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
    }
    LOGI("ime hide requested");
}

/* Recompute the mapping from the current window + hello. Sets *send_info to true
 * and copies the new mapping into *out_info if it changed and client is ready.
 * Caller holds g_s.lock. Caller sends INFO after unlocking g_s.lock. */
static void refresh_mapping_locked(bool *send_info, isws_info_t *out_info) {
    if (send_info) *send_info = false;
    if (!g_s.have_hello || !g_s.window || g_s.window_w <= 0 || g_s.window_h <= 0) {
        g_s.mapping_valid = false;
        return;
    }
    isws_info_t m;
    if (isws_compute_mapping((uint32_t)g_s.window_w, (uint32_t)g_s.window_h,
                             g_s.hello.width, g_s.hello.height,
                             g_s.rotation, &m) != 0) {
        g_s.mapping_valid = false;
        return;
    }
    bool changed = !g_s.mapping_valid ||
                   memcmp(&m, &g_s.mapping, sizeof(m)) != 0;
    g_s.mapping = m;
    g_s.mapping_valid = true;
    if (changed && g_s.client_fd >= 0) {
        if (send_info) *send_info = true;
        if (out_info) *out_info = m;
    }
}

/* Log one mapping as the full geometry chain (020): window, client
 * frame, scale, offsets and the integer dest rect. One line per
 * mapping change is enough to prove on device which link of the
 * chain is wrong: window WxH, hello WxH, scale, offsets, dest. */
static void log_mapping(const char *when, const isws_info_t *m) {
    int32_t x0, y0, x1, y1;
    if (!m) return;
    isws_dest_rect(m, &x0, &y0, &x1, &y1);
    LOGI("geometry %s: window=%ux%u client=%ux%u scale=%.2f offset=%.0f,%.0f dest=[%d,%d)x[%d,%d)",
         when ? when : "?", m->screen_w, m->screen_h, m->client_w,
         m->client_h, (double)m->scale, (double)m->offset_x,
         (double)m->offset_y, x0, x1, y0, y1);
}

/* ------------------------------------------------------------------ */
/* Client session                                                      */
/* ------------------------------------------------------------------ */

static void reset_session_locked(void) {
    if (g_s.have_hello) {
        g_s.have_session_stats = true;
        g_s.last_session_frames = g_s.frames_presented;
        g_s.last_session_dropped = g_s.frames_dropped;
        g_s.last_session_duration_s = g_s.session_start_ns > 0
                                          ? (now_ns() - g_s.session_start_ns) / 1000000000LL
                                          : 0;
    }
    if (g_s.mem) {
        munmap(g_s.mem, g_s.mem_size);
        g_s.mem = NULL;
        g_s.mem_size = 0;
    }
    if (g_s.memfd >= 0) {
        close(g_s.memfd);
        g_s.memfd = -1;
    }
    if (g_s.client_fd >= 0) {
        close(g_s.client_fd);
        g_s.client_fd = -1;
    }
    g_s.client_pid = 0;
    g_s.have_hello = false;
    g_s.mapping_valid = false;
    g_s.have_last_frame = false;
    g_s.overlay_dirty_frames = 0;
    if (g_ti_mapper_ready) {
        /* The client is gone: forget contacts without emitting, there
         * is nobody left to send a CANCEL to. */
        ti_mapper_reset(&g_ti_mapper);
    }
    memset(g_s.in_flight, 0, sizeof(g_s.in_flight));
    memset(&g_s.hello, 0, sizeof(g_s.hello));
}

/* ------------------------------------------------------------------ */
/* Background launcher / stats workers                                 */
/* ------------------------------------------------------------------ */

static void request_sysinfo_refresh(void) {
    pthread_mutex_lock(&g_s.probe_lock);
    g_s.probe_requested = true;
    pthread_cond_signal(&g_s.probe_cond);
    pthread_mutex_unlock(&g_s.probe_lock);
}

static void *probe_thread(void *arg) {
    (void)arg;
    while (atomic_load(&g_s.running)) {
        pthread_mutex_lock(&g_s.probe_lock);
        while (!g_s.probe_requested && atomic_load(&g_s.running)) {
            pthread_cond_wait(&g_s.probe_cond, &g_s.probe_lock);
        }
        g_s.probe_requested = false;
        pthread_mutex_unlock(&g_s.probe_lock);
        if (!atomic_load(&g_s.running)) break;

        isws_sysinfo_t tmp;
        isws_sysinfo_collect(&tmp);
        bool should_draw = false;
        pthread_mutex_lock(&g_s.lock);
        g_s.sysinfo = tmp;
        if (strcmp(g_s.status_msg, "Refreshing...") == 0) g_s.status_msg[0] = '\0';
        should_draw = !g_s.have_hello;
        pthread_mutex_unlock(&g_s.lock);
        if (should_draw) draw_launcher_screen();
    }
    return NULL;
}

static void *launch_thread(void *arg) {
    (void)arg;
    char err[256] = {0};
    int rc = isws_launch_aidoku_blocking(err, sizeof(err));
    LOGI("launch.sh finished rc=%d msg=%s", rc, err);

    pthread_mutex_lock(&g_s.lock);
    g_s.launching = false;
    if (rc != 0 && !g_s.stopping) {
        if (err[0]) {
            snprintf(g_s.status_msg, sizeof(g_s.status_msg),
                     "Launch exited rc=%d: %s", rc, err);
        } else {
            snprintf(g_s.status_msg, sizeof(g_s.status_msg),
                     "Launch exited with rc=%d", rc);
        }
    }
    bool should_draw = !g_s.have_hello;
    pthread_mutex_unlock(&g_s.lock);

    request_sysinfo_refresh();
    if (should_draw) draw_launcher_screen();
    return NULL;
}

static void start_launch_async(void) {
    pthread_mutex_lock(&g_s.lock);
    if (g_s.launching || g_s.have_hello) {
        pthread_mutex_unlock(&g_s.lock);
        return;
    }
    g_s.launching = true;
    snprintf(g_s.status_msg, sizeof(g_s.status_msg), "Starting Aidoku via su...");
    pthread_mutex_unlock(&g_s.lock);
    draw_launcher_screen();

    pthread_t th;
    if (pthread_create(&th, NULL, launch_thread, NULL) != 0) {
        pthread_mutex_lock(&g_s.lock);
        g_s.launching = false;
        snprintf(g_s.status_msg, sizeof(g_s.status_msg), "pthread_create failed");
        pthread_mutex_unlock(&g_s.lock);
        draw_launcher_screen();
        return;
    }
    pthread_detach(th);
}

static void *stop_thread(void *arg) {
    (void)arg;
    char err[128] = {0};
    int rc = isws_stop_aidoku(err, sizeof(err));
    LOGI("stop_aidoku rc=%d selinux=%s", rc, err);

    pthread_mutex_lock(&g_s.lock);
    g_s.stopping = false;
    g_s.launching = false;
    if (rc == 0) {
        snprintf(g_s.status_msg, sizeof(g_s.status_msg),
                 "Stopped; SELinux=%s", err[0] ? err : "Enforcing");
    } else {
        snprintf(g_s.status_msg, sizeof(g_s.status_msg), "Stop failed (no root?)");
    }
    bool should_draw = !g_s.have_hello;
    pthread_mutex_unlock(&g_s.lock);

    request_sysinfo_refresh();
    if (should_draw) draw_launcher_screen();
    return NULL;
}

static void start_stop_async(void) {
    pthread_mutex_lock(&g_s.lock);
    if (g_s.stopping) {
        pthread_mutex_unlock(&g_s.lock);
        return;
    }
    g_s.stopping = true;
    snprintf(g_s.status_msg, sizeof(g_s.status_msg), "Stopping loader & restoring Enforcing...");
    bool in_launcher = !g_s.have_hello;
    pthread_mutex_unlock(&g_s.lock);
    if (in_launcher) draw_launcher_screen();

    pthread_t th;
    if (pthread_create(&th, NULL, stop_thread, NULL) != 0) {
        pthread_mutex_lock(&g_s.lock);
        g_s.stopping = false;
        pthread_mutex_unlock(&g_s.lock);
        return;
    }
    pthread_detach(th);
}

/* Read one message header + payload. For HELLO the memfd arrives as
 * ancillary data on the same recvmsg. Returns 0 ok, -1 disconnect/error.
 * On success *fd_out is the passed fd or -1. */
static int read_message(int fd, uint16_t *type, uint16_t *flags,
                        uint8_t *payload, uint32_t *paylen, int *fd_out) {
    *fd_out = -1;
    /* Header: plain recv is fine; only HELLO carries an fd, and the fd
     * travels with the first bytes of its message, so use recvmsg for
     * the header as well and keep any ancillary data. */
    uint8_t hdr[ISWS_HEADER_SIZE];
    struct iovec iov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
    uint8_t cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    size_t got = 0;
    int passed_fd = -1;
    while (got < sizeof(hdr)) {
        iov.iov_base = hdr + got;
        iov.iov_len = sizeof(hdr) - got;
        mh.msg_controllen = sizeof(cbuf);
        ssize_t n = recvmsg(fd, &mh, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (passed_fd >= 0) close(passed_fd);
            return -1;
        }
        if (n == 0) {
            if (passed_fd >= 0) close(passed_fd);
            return -1;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t num = (c->cmsg_len >= sizeof(struct cmsghdr))
                             ? (c->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int)
                             : 0;
                const int *fds = (const int *)CMSG_DATA(c);
                for (size_t fi = 0; fi < num; fi++) {
                    if (passed_fd >= 0) close(passed_fd);
                    passed_fd = fds[fi];
                }
            }
        }
        got += (size_t)n;
        /* Ancillary data only arrives with the first recvmsg chunk in
         * practice; a header split across recvs still works because we
         * keep looping, but the fd is only attached to the first chunk. */
        if (got >= sizeof(hdr)) break;
    }
    uint32_t len = 0;
    if (isws_decode_header(hdr, sizeof(hdr), type, flags, &len) != 0) {
        if (passed_fd >= 0) close(passed_fd);
        return -1;
    }
    if (len > ISWS_MAX_PAYLOAD) {
        if (passed_fd >= 0) close(passed_fd);
        return -1;
    }
    if (len > 0 && read_exact(fd, payload, len) != 0) {
        if (passed_fd >= 0) close(passed_fd);
        return -1;
    }
    *paylen = len;
    *fd_out = passed_fd;
    return 0;
}

static void send_frame_done(int fd, uint64_t frame_id, uint32_t status) {
    int64_t vsync;
    pthread_mutex_lock(&g_s.lock);
    vsync = g_s.last_vsync_ns ? g_s.last_vsync_ns : now_ns();
    pthread_mutex_unlock(&g_s.lock);
    isws_frame_done_t d = { .frame_id = frame_id, .vsync_ns = vsync,
                            .status = status, .reserved = 0 };
    uint8_t buf[ISWS_FRAME_DONE_PAYLOAD_SIZE];
    size_t n = isws_encode_frame_done(buf, sizeof(buf), &d);
    send_msg(fd, ISWS_MSG_FRAME_DONE, buf, (uint32_t)n);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint8_t payload[ISWS_MAX_PAYLOAD];
    bool hello_done = false;

    for (;;) {
        uint16_t type = 0, flags = 0;
        uint32_t len = 0;
        int passed_fd = -1;
        if (read_message(fd, &type, &flags, payload, &len, &passed_fd) != 0) {
            if (passed_fd >= 0) close(passed_fd);
            break;
        }
        /* Any fd on a non-HELLO message is a protocol error; close it and disconnect. */
        if (type != ISWS_MSG_HELLO && passed_fd >= 0) {
            close(passed_fd);
            send_error(fd, ISWS_ERR_BAD_MESSAGE, "unexpected fd on non-HELLO message");
            break;
        }

        if (type == ISWS_MSG_HELLO) {
            if (hello_done) {
                if (passed_fd >= 0) close(passed_fd);
                send_error(fd, ISWS_ERR_BAD_MESSAGE, "duplicate HELLO");
                break;
            }
            isws_hello_t h;
            if (isws_decode_hello(payload, len, &h) != 0) {
                send_error(fd, ISWS_ERR_BAD_HELLO, "malformed HELLO payload");
                if (passed_fd >= 0) close(passed_fd);
                break;
            }
            uint32_t err = isws_validate_hello(&h);
            if (err != ISWS_ERR_NONE) {
                send_error(fd, err, "HELLO rejected");
                if (passed_fd >= 0) close(passed_fd);
                break;
            }
            if (passed_fd < 0) {
                send_error(fd, ISWS_ERR_NO_FD, "HELLO without memfd");
                break;
            }
            uint64_t need = isws_memfd_size(&h);
            struct stat st;
            if (need == 0 || fstat(passed_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_size < 0 || (uint64_t)st.st_size < need) {
                send_error(fd, ISWS_ERR_BAD_HELLO, "memfd too small or invalid");
                close(passed_fd);
                break;
            }
            uint8_t *mem = (uint8_t *)mmap(NULL, (size_t)need, PROT_READ,
                                           MAP_SHARED, passed_fd, 0);
            if (mem == MAP_FAILED) {
                send_error(fd, ISWS_ERR_INTERNAL, "mmap memfd failed");
                close(passed_fd);
                break;
            }
            bool send_info = false;
            isws_info_t info_snap;
            int64_t tnow = now_ns();
            pthread_mutex_lock(&g_s.lock);
            g_s.have_hello = true;
            g_s.hello = h;
            g_s.memfd = passed_fd;
            g_s.mem = mem;
            g_s.mem_size = (size_t)need;
            g_s.have_last_frame = false;
            g_s.frames_presented = 0;
            g_s.frames_dropped = 0;
            g_s.bytes_copied = 0;
            g_s.touches_sent = 0;
            g_s.session_start_ns = tnow;
            g_s.fps_win_start_ns = tnow;
            g_s.fps_win_frames = 0;
            g_s.current_fps = 0.0;
            g_s.launching = false;
            g_s.status_msg[0] = '\0';
            refresh_mapping_locked(&send_info, &info_snap);
            pthread_mutex_unlock(&g_s.lock);
            hello_done = true;
            LOGI("client connected: %ux%u stride=%u memfd=%zu bytes",
                 h.width, h.height, h.stride, (size_t)need);
            isws_ready_t r = { .version = ISWS_VERSION, .flags = 0,
                               .max_in_flight = ISWS_MAX_IN_FLIGHT };
            uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
            size_t rn = isws_encode_ready(rb, sizeof(rb), &r);
            if (send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn) != 0) break;
            if (send_info) send_info_unlocked(fd, &info_snap);
            log_current_mapping("hello");
            continue;
        }

        if (!hello_done) {
            send_error(fd, ISWS_ERR_BAD_MESSAGE, "first message must be HELLO");
            break;
        }

        if (type == ISWS_MSG_PRESENT) {
            isws_present_t p;
            if (isws_decode_present(payload, len, &p) != 0) {
                send_error(fd, ISWS_ERR_BAD_PRESENT, "malformed PRESENT");
                break;
            }
            if (p.buffer_index >= ISWS_NUM_BUFFERS) {
                send_error(fd, ISWS_ERR_BAD_PRESENT, "buffer index out of range");
                break;
            }
            bool dropped = false;
            pthread_mutex_lock(&g_s.lock);
            if (g_s.in_flight[p.buffer_index]) dropped = true;
            else g_s.in_flight[p.buffer_index] = true;
            pthread_mutex_unlock(&g_s.lock);
            if (dropped) {
                /* Policy (see PROTOCOL.md): never block the client and
                 * never present a buffer twice; ack it as dropped so the
                 * client can reuse the buffer immediately. */
                pthread_mutex_lock(&g_s.lock);
                g_s.frames_dropped++;
                pthread_mutex_unlock(&g_s.lock);
                send_frame_done(fd, p.frame_id, 1);
                continue;
            }
            uint64_t bytes = 0;
            int rc = present_frame(&p, &bytes);
            pthread_mutex_lock(&g_s.lock);
            g_s.in_flight[p.buffer_index] = false;
            int64_t now = now_ns();
            if (rc == 0) {
                g_s.frames_presented++;
                g_s.bytes_copied += bytes;
                g_s.fps_win_frames++;
                int64_t dt = now - g_s.fps_win_start_ns;
                if (dt >= 500000000LL) {
                    g_s.current_fps = (double)g_s.fps_win_frames * 1e9 / (double)dt;
                    g_s.fps_win_start_ns = now;
                    g_s.fps_win_frames = 0;
                }
            } else {
                g_s.frames_dropped++;
            }
            if (now - g_s.stats_last_log_ns >= 1000000000LL) {
                g_s.stats_last_log_ns = now;
                LOGI("stats: presented=%" PRIu64 " dropped=%" PRIu64
                     " copied=%" PRIu64 " bytes",
                     g_s.frames_presented, g_s.frames_dropped, g_s.bytes_copied);
            }
            pthread_mutex_unlock(&g_s.lock);
            /* Ack even when there was no window yet: the frame is gone,
             * the client must be free to reuse the buffer. */
            send_frame_done(fd, p.frame_id, rc == 0 ? 0 : 1);
            continue;
        }

        if (type == ISWS_MSG_KEYBOARD) {
            isws_keyboard_req_t req;
            if (isws_decode_keyboard_req(payload, len, &req) != 0) {
                send_error(fd, ISWS_ERR_BAD_MESSAGE, "bad KEYBOARD payload");
                break;
            }
            if (req.show) ime_show();
            else ime_hide();
            continue;
        }

        if (type == ISWS_MSG_GOODBYE) {
            break;
        }

        send_error(fd, ISWS_ERR_BAD_MESSAGE, "unknown message type");
        break;
    }

    pthread_mutex_lock(&g_s.draw_lock);
    pthread_mutex_lock(&g_s.lock);
    bool was_active = (g_s.client_fd == fd);
    if (was_active) reset_session_locked(); /* closes fd */
    pthread_mutex_unlock(&g_s.lock);
    pthread_mutex_unlock(&g_s.draw_lock);
    if (!was_active) {
        close(fd); /* session was replaced by a reconnect */
    } else {
        request_sysinfo_refresh();
        draw_launcher_screen();
    }
    LOGI("client disconnected");
    return NULL;
}

static void *socket_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0'; /* abstract socket */
    const char *name = ISWS_SOCKET_NAME;
    size_t nlen = strlen(name);
    memcpy(addr.sun_path + 1, name, nlen);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
    if (bind(srv, (struct sockaddr *)&addr, alen) != 0) {
        LOGE("bind @%s failed: %s", name, strerror(errno));
        close(srv);
        return NULL;
    }
    if (listen(srv, 4) != 0) {
        LOGE("listen failed: %s", strerror(errno));
        close(srv);
        return NULL;
    }
    LOGI("listening on abstract socket @%s", name);
    while (atomic_load(&g_s.running)) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!atomic_load(&g_s.running)) break;
            LOGW("accept failed: %s", strerror(errno));
            continue;
        }
        /* Accept connections from any uid (including uid shell=2000 from
         * /data/local/tmp/ioswine), while logging peer credentials. */
        struct ucred cred;
        socklen_t cred_len = sizeof(cred);
        int32_t peer_pid = 0;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
            peer_pid = (int32_t)cred.pid;
            LOGI("accepted client pid=%d uid=%u gid=%u",
                 (int)cred.pid, (unsigned)cred.uid, (unsigned)cred.gid);
        } else {
            LOGW("getsockopt(SO_PEERCRED) failed: %s", strerror(errno));
        }
        /* One client at a time; the newest one wins. A previous loader
         * that was killed or hung must never lock the surface (users had
         * to relaunch dozens of times). The old fd is only shut down here:
         * its client thread wakes on EOF, sees it is no longer the active
         * session and closes the fd itself. */
        pthread_mutex_lock(&g_s.draw_lock);
        pthread_mutex_lock(&g_s.lock);
        int old_fd = g_s.client_fd;
        if (old_fd >= 0) {
            LOGI("replacing stale client pid=%d with pid=%d",
                 (int)g_s.client_pid, (int)peer_pid);
            shutdown(old_fd, SHUT_RDWR);
            g_s.client_fd = -1; /* detach so reset does not close it */
            reset_session_locked();
        }
        g_s.client_fd = fd;
        g_s.client_pid = peer_pid;
        pthread_mutex_unlock(&g_s.lock);
        pthread_mutex_unlock(&g_s.draw_lock);
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void *)(intptr_t)fd) != 0) {
            LOGE("pthread_create failed");
            pthread_mutex_lock(&g_s.lock);
            if (g_s.client_fd == fd) g_s.client_fd = -1;
            pthread_mutex_unlock(&g_s.lock);
            close(fd);
            continue;
        }
        pthread_detach(th);
    }
    close(srv);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Vsync                                                               */
/* ------------------------------------------------------------------ */

static void on_vsync(int64_t frame_time_nanos, void *data) {
    (void)data;
    pthread_mutex_lock(&g_s.lock);
    g_s.last_vsync_ns = frame_time_nanos;
    pthread_mutex_unlock(&g_s.lock);
    /* 64-bit variant: the old postFrameCallback is deprecated since API 29
     * and its callback type (void(*)(void*)) does not match on_vsync. */
    AChoreographer_postFrameCallback64(AChoreographer_getInstance(), on_vsync, NULL);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static bool dist_lt(float x0, float y0, float x1, float y1, float max_d) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    return (dx * dx + dy * dy) <= (max_d * max_d);
}

static void toggle_overlay_from_ui(void) {
    bool now_vis = false;
    pthread_mutex_lock(&g_s.lock);
    g_s.overlay_visible = !g_s.overlay_visible;
    now_vis = g_s.overlay_visible;
    if (!now_vis) g_s.overlay_dirty_frames = 4;
    pthread_mutex_unlock(&g_s.lock);
    LOGI("stats overlay %s", now_vis ? "shown" : "hidden");
    redraw_last_frame();
}

static int32_t on_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    int32_t type = AInputEvent_getType(event);
    int64_t ts = now_ns();

    pthread_mutex_lock(&g_s.lock);
    int fd = g_s.client_fd;
    bool have_hello = g_s.have_hello;
    bool have_map = g_s.mapping_valid;
    isws_info_t map = g_s.mapping;
    bool ov_vis = g_s.overlay_visible;
    ui_view_t view = g_s.ui_view;
    ui_layout_t layout = g_s.ui_layout;
    pthread_mutex_unlock(&g_s.lock);

    /* Launcher mode (no active iOS session yet). */
    if (!have_hello) {
        if (type == AINPUT_EVENT_TYPE_KEY) {
            int32_t key = AKeyEvent_getKeyCode(event);
            int32_t action = AKeyEvent_getAction(event);
            if (key == AKEYCODE_BACK && view == UI_VIEW_LOG) {
                if (action == AKEY_EVENT_ACTION_UP) {
                    pthread_mutex_lock(&g_s.lock);
                    g_s.ui_view = UI_VIEW_LAUNCHER;
                    pthread_mutex_unlock(&g_s.lock);
                    draw_launcher_screen();
                }
                return 1;
            }
            return 0;
        }
        if (type == AINPUT_EVENT_TYPE_MOTION) {
            int32_t action = AMotionEvent_getAction(event);
            int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(event, 0);
            float y = AMotionEvent_getY(event, 0);
            if (masked == AMOTION_EVENT_ACTION_DOWN) {
                g_s.down_action = ui_hit_test_launcher(&layout, view, x, y);
            } else if (masked == AMOTION_EVENT_ACTION_UP) {
                ui_action_t act = ui_hit_test_launcher(&layout, view, x, y);
                ui_action_t down = g_s.down_action;
                g_s.down_action = UI_ACT_NONE;
                if (act != UI_ACT_NONE && act == down) {
                    if (act == UI_ACT_LAUNCH) {
                        start_launch_async();
                    } else if (act == UI_ACT_STOP) {
                        start_stop_async();
                    } else if (act == UI_ACT_REFRESH) {
                        pthread_mutex_lock(&g_s.lock);
                        snprintf(g_s.status_msg, sizeof(g_s.status_msg), "Refreshing...");
                        pthread_mutex_unlock(&g_s.lock);
                        draw_launcher_screen();
                        request_sysinfo_refresh();
                    } else if (act == UI_ACT_VIEW_LOG) {
                        pthread_mutex_lock(&g_s.lock);
                        g_s.ui_view = UI_VIEW_LOG;
                        pthread_mutex_unlock(&g_s.lock);
                        draw_launcher_screen();
                        request_sysinfo_refresh();
                    } else if (act == UI_ACT_LOG_BACK) {
                        pthread_mutex_lock(&g_s.lock);
                        g_s.ui_view = UI_VIEW_LAUNCHER;
                        pthread_mutex_unlock(&g_s.lock);
                        draw_launcher_screen();
                    }
                }
            } else if (masked == AMOTION_EVENT_ACTION_CANCEL) {
                g_s.down_action = UI_ACT_NONE;
            }
            return 1;
        }
        return 0;
    }

    if (fd < 0) return 0;

    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        int32_t rep = AKeyEvent_getRepeatCount(event);

        if (key == AKEYCODE_VOLUME_UP || key == AKEYCODE_VOLUME_DOWN) {
            if (action == AKEY_EVENT_ACTION_DOWN && rep == 0) {
                toggle_overlay_from_ui();
            }
            return 1;
        }
        if (key == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                if (rep == 0) {
                    g_s.back_down_ns = ts;
                    g_s.back_long_triggered = false;
                    send_key(fd, ISWS_KEY_BACK, ISWS_KEY_ACTION_DOWN, ts);
                } else if (!g_s.back_long_triggered && g_s.back_down_ns > 0 &&
                           (ts - g_s.back_down_ns) >= 700000000LL) {
                    g_s.back_long_triggered = true;
                    send_key(fd, ISWS_KEY_BACK, ISWS_KEY_ACTION_UP, ts);
                    start_stop_async();
                }
            } else if (action == AKEY_EVENT_ACTION_UP) {
                if (!g_s.back_long_triggered && g_s.back_down_ns > 0 &&
                    (ts - g_s.back_down_ns) >= 700000000LL) {
                    g_s.back_long_triggered = true;
                    start_stop_async();
                }
                g_s.back_down_ns = 0;
                send_key(fd, ISWS_KEY_BACK, ISWS_KEY_ACTION_UP, ts);
            }
            return 1; /* consumed: the client decides what back means */
        }
        /* Hardware keys the keyboard skeleton consumes:
         * DEL, ENTER, TAB and the DPAD arrows are forwarded as KEY
         * events with their Android keycode; the client maps them to
         * deleteBackward / newline / tab / cursor moves. Printable
         * keys from a hardware keyboard still arrive here as KEY
         * events too, but the client ignores unknown keycodes: a
         * hardware keyboard that should type text needs the
         * InputConnection follow-up, same as the soft keyboard. */
        if (key == AKEYCODE_DEL || key == AKEYCODE_ENTER ||
            key == AKEYCODE_TAB || key == AKEYCODE_DPAD_LEFT ||
            key == AKEYCODE_DPAD_RIGHT || key == AKEYCODE_DPAD_UP ||
            key == AKEYCODE_DPAD_DOWN || key == AKEYCODE_ESCAPE) {
            send_key(fd, (uint32_t)key,
                     action == AKEY_EVENT_ACTION_DOWN
                         ? ISWS_KEY_ACTION_DOWN : ISWS_KEY_ACTION_UP,
                     ts);
            return 1;
        }
        return 0;
    }

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(event);
        int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        size_t count = AMotionEvent_getPointerCount(event);
        if (masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_UP)
            LOGI("touch %s map=%d ov=%d fd=%d", masked == AMOTION_EVENT_ACTION_DOWN ? "down" : "up",
                 (int)have_map, (int)ov_vis, fd);
        float x0 = AMotionEvent_getX(event, 0);
        float y0 = AMotionEvent_getY(event, 0);

        /* Interactive buttons inside the visible stats overlay. */
        if (ov_vis) {
            if (masked == AMOTION_EVENT_ACTION_DOWN && count == 1 &&
                x0 >= (float)layout.overlay_box.x &&
                x0 < (float)(layout.overlay_box.x + layout.overlay_box.w) &&
                y0 >= (float)layout.overlay_box.y &&
                y0 < (float)(layout.overlay_box.y + layout.overlay_box.h)) {
                g_s.overlay_touch_captured = true;
                g_s.down_action = ui_hit_test_overlay(&layout, x0, y0);
                return 1;
            }
            if (g_s.overlay_touch_captured) {
                if (masked == AMOTION_EVENT_ACTION_UP) {
                    ui_action_t act = ui_hit_test_overlay(&layout, x0, y0);
                    ui_action_t down = g_s.down_action;
                    g_s.overlay_touch_captured = false;
                    g_s.down_action = UI_ACT_NONE;
                    if (act != UI_ACT_NONE && act == down) {
                        if (act == UI_ACT_OVERLAY_STOP) {
                            start_stop_async();
                        } else if (act == UI_ACT_OVERLAY_HIDE) {
                            toggle_overlay_from_ui();
                        }
                    }
                } else if (masked == AMOTION_EVENT_ACTION_CANCEL) {
                    g_s.overlay_touch_captured = false;
                    g_s.down_action = UI_ACT_NONE;
                }
                return 1;
            }
        }

        /* Two-finger quick tap toggles the stats overlay. */
        if (masked == AMOTION_EVENT_ACTION_DOWN && count == 1) {
            g_s.two_finger_t0_ns = ts;
            g_s.two_finger_x0 = x0;
            g_s.two_finger_y0 = y0;
            g_s.two_finger_candidate = false;
        } else if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            if (count == 2 && (ts - g_s.two_finger_t0_ns) <= 250000000LL &&
                dist_lt(g_s.two_finger_x0, g_s.two_finger_y0, x0, y0, 48.0f)) {
                g_s.two_finger_candidate = true;
                g_s.two_finger_x1 = AMotionEvent_getX(event, 1);
                g_s.two_finger_y1 = AMotionEvent_getY(event, 1);
            } else {
                g_s.two_finger_candidate = false;
            }
        } else if (masked == AMOTION_EVENT_ACTION_MOVE && g_s.two_finger_candidate) {
            if (count != 2 ||
                !dist_lt(g_s.two_finger_x0, g_s.two_finger_y0, x0, y0, 48.0f) ||
                !dist_lt(g_s.two_finger_x1, g_s.two_finger_y1,
                         AMotionEvent_getX(event, 1), AMotionEvent_getY(event, 1), 48.0f)) {
                g_s.two_finger_candidate = false;
            }
        } else if (masked == AMOTION_EVENT_ACTION_UP) {
            if (g_s.two_finger_candidate && (ts - g_s.two_finger_t0_ns) <= 450000000LL) {
                g_s.two_finger_candidate = false;
                toggle_overlay_from_ui();
            }
            g_s.two_finger_candidate = false;
        } else if (masked == AMOTION_EVENT_ACTION_CANCEL) {
            g_s.two_finger_candidate = false;
        }

        if (!have_map) return 0;

        size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        size_t hcount = 0;
        size_t h = 0;
        size_t i = 0;
        ti_motion_t motion;
        ti_emit_ctx_t ectx;

        if (!g_ti_mapper_ready) {
            ti_mapper_init(&g_ti_mapper);
            g_ti_mapper_ready = 1;
        }
        /* A mapping change under live contacts cancels them first
         * (the mapper emits the CANCELs through the same callback). */
        ectx.fd = fd;
        ti_mapper_set_map(&g_ti_mapper, &map, ti_send_touch, &ectx);

        memset(&motion, 0, sizeof(motion));
        switch (masked) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                motion.masked_action = TI_ACTION_DOWN; break;
            case AMOTION_EVENT_ACTION_MOVE:
                motion.masked_action = TI_ACTION_MOVE; break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
                motion.masked_action = TI_ACTION_UP; break;
            case AMOTION_EVENT_ACTION_CANCEL:
                motion.masked_action = TI_ACTION_CANCEL; break;
            default:
                motion.masked_action = TI_ACTION_OTHER; break;
        }
        motion.pointer_index = (uint32_t)index;
        motion.pointer_count = (uint32_t)(count > ISWS_MAX_POINTERS
                                          ? ISWS_MAX_POINTERS : count);
        motion.timestamp_ns = ts;
        for (i = 0; i < count && i < ISWS_MAX_POINTERS; i++) {
            motion.pointers[i].android_id =
                (uint32_t)AMotionEvent_getPointerId(event, i);
            motion.pointers[i].x = AMotionEvent_getX(event, i);
            motion.pointers[i].y = AMotionEvent_getY(event, i);
            motion.pointers[i].pressure = AMotionEvent_getPressure(event, i);
        }
        if (motion.masked_action == TI_ACTION_MOVE) {
            /* Historical samples, oldest first. The mapper emits them
             * before the current sample (F5). */
            hcount = (size_t)AMotionEvent_getHistorySize(event);
            if (hcount > TI_MAX_HISTORY) hcount = TI_MAX_HISTORY;
            motion.history_count = (uint32_t)hcount;
            for (h = 0; h < hcount; h++) {
                for (i = 0; i < count && i < ISWS_MAX_POINTERS; i++) {
                    motion.hist_x[h][i] =
                        AMotionEvent_getHistoricalX(event, i, h);
                    motion.hist_y[h][i] =
                        AMotionEvent_getHistoricalY(event, i, h);
                }
            }
        }
        ti_mapper_feed(&g_ti_mapper, &motion, ti_send_touch, &ectx);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static uint32_t rotation_from_config(AConfiguration *config) {
    int32_t o = AConfiguration_getOrientation(config);
    /* AConfiguration only exposes portrait/landscape/square, not the
     * exact rotation. We report 0 for portrait and 90 for landscape;
     * the exact degrees are not needed: INFO carries the window size
     * and mapping, which is what touch mapping depends on. */
    return o == ACONFIGURATION_ORIENTATION_LAND ? 90u : 0u;
}

/* Shared window-size refresh for CONFIG_CHANGED and WINDOW_RESIZED
 * (020: WINDOW_RESIZED was unhandled, so a post-creation resize
 * never updated the mapping; 021: the launcher's CONFIG_CHANGED
 * case duplicated this body inline, so the helper was dead code).
 * Re-reads the window size, resets the buffer geometry, refreshes
 * the mapping, sends INFO if it changed, and logs the new mapping.
 * Does NOT redraw: the caller decides launcher screen vs last
 * frame (see redraw_after_resize). */
static void on_window_size_changed(struct android_app *app,
                                   const char *tag) {
    bool send_info = false;
    isws_info_t info_snap;
    int client_fd = -1;
    pthread_mutex_lock(&g_s.lock);
    if (g_s.window) {
        g_s.window_w = ANativeWindow_getWidth(g_s.window);
        g_s.window_h = ANativeWindow_getHeight(g_s.window);
        ANativeWindow_setBuffersGeometry(g_s.window, g_s.window_w,
                                         g_s.window_h,
                                         WINDOW_FORMAT_RGBA_8888);
        g_s.rotation = rotation_from_config(app->config);
        LOGI("%s: window %dx%d rotation=%u", tag, g_s.window_w,
             g_s.window_h, g_s.rotation);
        refresh_mapping_locked(&send_info, &info_snap);
        client_fd = g_s.client_fd;
    }
    pthread_mutex_unlock(&g_s.lock);
    if (send_info) send_info_unlocked(client_fd, &info_snap);
    log_current_mapping(tag);
}

/* Repaint after a resize: the launcher screen when no client is
 * connected, otherwise the last presented frame at the new size.
 * Shared by CONFIG_CHANGED and WINDOW_RESIZED so neither path can
 * forget the repaint (the 020 launcher merge kept the launcher's
 * CONFIG_CHANGED body inline precisely because the helper did not
 * redraw). */
static void redraw_after_resize(void) {
    bool in_launcher;
    pthread_mutex_lock(&g_s.lock);
    in_launcher = !g_s.have_hello;
    pthread_mutex_unlock(&g_s.lock);
    if (in_launcher) draw_launcher_screen();
    else redraw_last_frame();
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: {
            bool send_info = false;
            bool in_launcher = false;
            isws_info_t info_snap;
            int client_fd = -1;
            pthread_mutex_lock(&g_s.lock);
            g_s.window = app->window;
            if (g_s.window) {
                g_s.window_w = ANativeWindow_getWidth(g_s.window);
                g_s.window_h = ANativeWindow_getHeight(g_s.window);
                ANativeWindow_setBuffersGeometry(g_s.window, g_s.window_w,
                                                 g_s.window_h,
                                                 WINDOW_FORMAT_RGBA_8888);
                if (!g_s.window_format_logged) {
                    g_s.window_format_logged = true;
                    LOGI("window %dx%d, format RGBA_8888, CPU swizzle BGRA->RGBA active",
                         g_s.window_w, g_s.window_h);
                }
                g_s.rotation = rotation_from_config(app->config);
                refresh_mapping_locked(&send_info, &info_snap);
                client_fd = g_s.client_fd;
                in_launcher = !g_s.have_hello;
            }
            pthread_mutex_unlock(&g_s.lock);
            if (send_info) send_info_unlocked(client_fd, &info_snap);
            if (in_launcher) {
                draw_launcher_screen();
                request_sysinfo_refresh();
            } else {
                redraw_last_frame();
            }
            log_current_mapping("init-window");
            break;
        }
        case APP_CMD_TERM_WINDOW: {
            pthread_mutex_lock(&g_s.draw_lock);
            pthread_mutex_lock(&g_s.lock);
            g_s.window = NULL;
            g_s.window_w = g_s.window_h = 0;
            g_s.mapping_valid = false;
            pthread_mutex_unlock(&g_s.lock);
            pthread_mutex_unlock(&g_s.draw_lock);
            break;
        }
        case APP_CMD_CONFIG_CHANGED: {
            /* 021: was an inline duplicate of on_window_size_changed
             * (kept by the 020 merge because the helper did not
             * redraw); now routes through the helper, which also
             * emits the "geometry config-changed:" line the inline
             * body never did. */
            on_window_size_changed(app, "config-changed");
            redraw_after_resize();
            break;
        }
        case APP_CMD_WINDOW_RESIZED: {
            /* 021: was unhandled entirely (fell through to default),
             * so a resize that arrived as WINDOW_RESIZED never
             * re-validated the mapping. Same path as CONFIG_CHANGED. */
            on_window_size_changed(app, "window-resized");
            redraw_after_resize();
            break;
        }
        case APP_CMD_GAINED_FOCUS: {
            pthread_mutex_lock(&g_s.lock);
            int fd = g_s.client_fd;
            bool ready = g_s.have_hello;
            pthread_mutex_unlock(&g_s.lock);
            if (fd >= 0 && ready) {
                send_focus(fd, 1);
            } else if (!ready) {
                request_sysinfo_refresh();
            }
            break;
        }
        case APP_CMD_LOST_FOCUS: {
            pthread_mutex_lock(&g_s.lock);
            int fd = g_s.client_fd;
            bool ready = g_s.have_hello;
            pthread_mutex_unlock(&g_s.lock);
            if (fd >= 0 && ready) send_focus(fd, 0);
            break;
        }
        case APP_CMD_DESTROY:
            atomic_store(&g_s.running, false);
            pthread_mutex_lock(&g_s.probe_lock);
            pthread_cond_signal(&g_s.probe_cond);
            pthread_mutex_unlock(&g_s.probe_lock);
            break;
        default:
            break;
    }
}

void android_main(struct android_app *app) {
    memset(&g_s, 0, sizeof(g_s));
    pthread_mutex_init(&g_s.draw_lock, NULL);
    pthread_mutex_init(&g_s.lock, NULL);
    pthread_mutex_init(&g_s.send_lock, NULL);
    pthread_mutex_init(&g_s.probe_lock, NULL);
    pthread_cond_init(&g_s.probe_cond, NULL);
    isws_sysinfo_init(&g_s.sysinfo);
    g_s.app = app;
    g_s.client_fd = -1;
    g_s.memfd = -1;
    g_s.stats_last_log_ns = now_ns();
    atomic_store(&g_s.running, true);

    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;

    pthread_t probe_th;
    if (pthread_create(&probe_th, NULL, probe_thread, NULL) == 0) {
        pthread_detach(probe_th);
        request_sysinfo_refresh();
    } else {
        LOGW("failed to start probe thread");
    }

    pthread_t sock_th;
    if (pthread_create(&sock_th, NULL, socket_thread, NULL) != 0) {
        LOGE("failed to start socket thread");
        return;
    }
    pthread_detach(sock_th);

    AChoreographer_postFrameCallback64(AChoreographer_getInstance(), on_vsync, NULL);
    /* Build stamp (020): the date/time the .so was compiled, so the
     * device log proves which presenter build is running and a stale
     * APK is detectable from logcat alone. */
    LOGI("ioswine-surface server started (protocol v%d, built %s %s)",
         ISWS_VERSION, __DATE__, __TIME__);

    /* Main looper: glue requires us to poll; input and lifecycle arrive
     * through the callbacks above.
     * NOTE: ALooper_pollAll is unavailable in the NDK (obsoleted since
     * Android 1 - it may ignore wakes). pollOnce is the replacement;
     * it never returns ALOOPER_POLL_CALLBACK, and only
     * ALOOPER_POLL_ERROR means the looper is gone. */
    for (;;) {
        int events = 0;
        struct android_poll_source *source = NULL;
        int ret = ALooper_pollOnce(-1, NULL, &events, (void **)&source);
        if (ret == ALOOPER_POLL_ERROR) break; /* looper destroyed */
        if (ret >= 0 && source) source->process(app, source);
        if (app->destroyRequested) break;
    }

    atomic_store(&g_s.running, false);
    pthread_mutex_lock(&g_s.probe_lock);
    pthread_cond_signal(&g_s.probe_cond);
    pthread_mutex_unlock(&g_s.probe_lock);
    pthread_mutex_lock(&g_s.draw_lock);
    pthread_mutex_lock(&g_s.lock);
    reset_session_locked();
    pthread_mutex_unlock(&g_s.lock);
    pthread_mutex_unlock(&g_s.draw_lock);
    LOGI("ioswine-surface server stopped");
}
