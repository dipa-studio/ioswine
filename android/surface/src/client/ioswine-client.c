/*
 * ioswine-client: plain C test client for the ioswine-surface presenter.
 *
 * Runs as a native binary from adb shell (no Java): connects to the
 * abstract socket "@ioswine-surface", creates one memfd holding two
 * BGRA8888 buffers, draws an animated 60 fps pattern, draws a dot at
 * the last touch point, and prints FPS + PRESENT->FRAME_DONE latency
 * once a second.
 *
 * Exit: back key (KEY message), SIGINT/SIGTERM, or --frames N / --seconds N.
 *
 * Host loopback test mode: build on a Linux host with -DIOSWINE_HOST_TEST
 * and run with IOSWINE_LOOPBACK=1. The client then creates a socketpair
 * and spawns a fake-server thread that answers HELLO with READY+INFO,
 * acks every PRESENT with a synthetic vsync FRAME_DONE, and injects a
 * synthetic TOUCH (moving dot) every 500 ms. This exercises the full
 * encode/decode + pacing + drawing path without Android. On a host the
 * memfd is created with memfd_create() (Linux >= 3.17); the same call
 * is used on Android (bionic exposes memfd_create since API 30).
 *
 * Usage (device):
 *   adb push ioswine-client /data/local/tmp/ioswine-client
 *   adb shell chmod 755 /data/local/tmp/ioswine-client
 *   adb shell /data/local/tmp/ioswine-client [--width W] [--height H]
 *       [--frames N] [--seconds N] [--fps F]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "protocol.h"

#ifdef __ANDROID__
#include <sys/syscall.h>
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif

static int memfd_create_portable(const char *name, unsigned flags) {
#ifdef __ANDROID__
    return (int)syscall(SYS_memfd_create, name, flags);
#else
    return memfd_create(name, flags);
#endif
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(int64_t ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000LL,
                           .tv_nsec = ns % 1000000000LL };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* ------------------------------------------------------------------ */
/* Options / shared state                                              */
/* ------------------------------------------------------------------ */

static uint32_t g_width = 1080, g_height = 2400;
static uint32_t g_target_fps = 60;
static int64_t g_max_frames = -1;   /* --frames */
static int64_t g_max_seconds = -1;  /* --seconds */
static atomic_bool g_stop = false;
static atomic_bool g_got_back = false;

/* Last touch, in client pixels; -1 = none yet. */
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_touch_x = -1.0f, g_touch_y = -1.0f;
static bool g_touch_down = false;
static isws_info_t g_info;
static bool g_have_info = false;

static void on_sigint(int sig) {
    (void)sig;
    atomic_store(&g_stop, true);
}

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

static int send_msg(int fd, uint16_t type, const uint8_t *payload, uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    if (send_all(fd, hdr, sizeof(hdr)) != 0) return -1;
    if (len > 0 && send_all(fd, payload, len) != 0) return -1;
    return 0;
}

static int read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
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

/* ------------------------------------------------------------------ */
/* Frame drawing (BGRA8888 premultiplied, in the client frame)         */
/*                                                                     */
/* Animated pattern: vertical colour bars sliding with time, a        */
/* horizontal sine band, and a frame counter in the top-left corner   */
/* drawn as a row of blocks (cheap, no font). A red dot is drawn at   */
/* the last touch point.                                               */
/* ------------------------------------------------------------------ */

static void draw_frame(uint32_t *px, uint32_t stride_px, uint32_t w, uint32_t h,
                       uint64_t frame, int64_t t_ns) {
    double t = (double)t_ns / 1e9;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = px + (size_t)y * stride_px;
        double band = sin((double)y * 0.01 + t * 3.0) * 0.5 + 0.5;
        for (uint32_t x = 0; x < w; x++) {
            double slide = fmod((double)x / (double)w * 4.0 + t * 0.25, 4.0);
            uint8_t r, g, b;
            if (slide < 1.0)      { r = 200; g = 60;  b = 60; }
            else if (slide < 2.0) { r = 60;  g = 200; b = 60; }
            else if (slide < 3.0) { r = 60;  g = 60;  b = 200; }
            else                  { r = 200; g = 200; b = 60; }
            /* Sine band brightens the row; top strip shows frame count. */
            uint8_t bright = (uint8_t)(180.0 + 75.0 * band);
            r = (uint8_t)((uint16_t)r * bright / 255);
            g = (uint8_t)((uint16_t)g * bright / 255);
            b = (uint8_t)((uint16_t)b * bright / 255);
            if (y < 24) {
                /* Frame counter: one white block per set bit (bit 0 at left). */
                int bit = (int)(x / 24);
                if (bit < 32 && (frame & (1u << bit)))
                    { r = 255; g = 255; b = 255; }
                else
                    { r = 20; g = 20; b = 20; }
            }
            /* BGRA8888 premultiplied, opaque: bytes B,G,R,A. */
            row[x] = ((uint32_t)b) | ((uint32_t)g << 8) |
                     ((uint32_t)r << 16) | 0xff000000u;
        }
    }
    /* Touch dot: solid red disc, radius ~2% of the smaller dimension. */
    pthread_mutex_lock(&g_touch_lock);
    float tx = g_touch_x, ty = g_touch_y;
    bool down = g_touch_down;
    pthread_mutex_unlock(&g_touch_lock);
    if (tx >= 0.0f && ty >= 0.0f) {
        int32_t rad = (int32_t)((w < h ? w : h) * 0.02);
        if (rad < 8) rad = 8;
        int32_t cx = (int32_t)tx, cy = (int32_t)ty;
        for (int32_t dy = -rad; dy <= rad; dy++) {
            int32_t y = cy + dy;
            if (y < 0 || y >= (int32_t)h) continue;
            for (int32_t dx = -rad; dx <= rad; dx++) {
                int32_t x = cx + dx;
                if (x < 0 || x >= (int32_t)w) continue;
                if (dx * dx + dy * dy > rad * rad) continue;
                uint32_t *p = px + (size_t)y * stride_px + x;
                *p = down ? 0xff0000ffu /* red, opaque */ : 0xff00ffffu /* yellow */;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Inbound message handling (server -> client)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    /* Pacing: FRAME_DONE for the most recent PRESENT. */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint64_t last_done_id;
    int64_t last_done_vsync;
    uint32_t last_done_status;
    /* High-water mark of acked frames (FRAME_DONEs arrive in order).
     * -1 until the first ack. The pacing wait uses this, not have_done:
     * comparing the latest ack id against the in-flight id breaks when
     * drawing is slow, because the ack for frame N+1 can already have
     * arrived by the time frame N's ack is checked. */
    int64_t acked_up_to;
    bool have_ready;
    /* PRESENT send times, slot = frame_id % 2 (2 buffers). */
    uint64_t present_id[ISWS_NUM_BUFFERS];
    int64_t present_ns[ISWS_NUM_BUFFERS];
    /* Stats: PRESENT -> FRAME_DONE latency. */
    uint64_t frames_acked;
    uint64_t lat_samples;
    int64_t lat_sum_ns;
    int64_t lat_max_ns;
    int64_t lat_min_ns;
} rx_state_t;

static void handle_message(rx_state_t *rx, uint16_t type,
                           const uint8_t *payload, uint32_t len) {
    switch (type) {
        case ISWS_MSG_READY: {
            isws_ready_t r;
            if (isws_decode_ready(payload, len, &r) == 0) {
                fprintf(stderr, "ready: server protocol v%u, max_in_flight=%u\n",
                        r.version, r.max_in_flight);
                pthread_mutex_lock(&rx->lock);
                rx->have_ready = true;
                pthread_cond_signal(&rx->cond);
                pthread_mutex_unlock(&rx->lock);
            }
            break;
        }
        case ISWS_MSG_FRAME_DONE: {
            isws_frame_done_t d;
            if (isws_decode_frame_done(payload, len, &d) != 0) break;
            pthread_mutex_lock(&rx->lock);
            rx->last_done_id = d.frame_id;
            rx->last_done_vsync = d.vsync_ns;
            rx->last_done_status = d.status;
            rx->frames_acked++;
            if ((int64_t)d.frame_id > rx->acked_up_to)
                rx->acked_up_to = (int64_t)d.frame_id;
            /* PRESENT -> FRAME_DONE latency, matched by frame id. */
            for (int i = 0; i < (int)ISWS_NUM_BUFFERS; i++) {
                if (rx->present_id[i] == d.frame_id) {
                    int64_t lat = now_ns() - rx->present_ns[i];
                    if (lat >= 0) {
                        rx->lat_samples++;
                        rx->lat_sum_ns += lat;
                        if (lat > rx->lat_max_ns) rx->lat_max_ns = lat;
                        if (lat < rx->lat_min_ns) rx->lat_min_ns = lat;
                    }
                    break;
                }
            }
            pthread_cond_signal(&rx->cond);
            pthread_mutex_unlock(&rx->lock);
            break;
        }
        case ISWS_MSG_TOUCH: {
            isws_touch_t t;
            if (isws_decode_touch(payload, len, &t) != 0) break;
            pthread_mutex_lock(&g_touch_lock);
            g_touch_x = t.x;
            g_touch_y = t.y;
            g_touch_down = (t.action == ISWS_TOUCH_DOWN ||
                            t.action == ISWS_TOUCH_MOVE);
            pthread_mutex_unlock(&g_touch_lock);
            fprintf(stderr, "touch: action=%u id=%u count=%u x=%.1f y=%.1f\n",
                    t.action, t.pointer_id, t.pointer_count, t.x, t.y);
            break;
        }
        case ISWS_MSG_KEY: {
            isws_key_t k;
            if (isws_decode_key(payload, len, &k) != 0) break;
            fprintf(stderr, "key: code=%u action=%u\n", k.keycode, k.action);
            if (k.keycode == ISWS_KEY_BACK && k.action == ISWS_KEY_ACTION_UP) {
                atomic_store(&g_got_back, true);
                atomic_store(&g_stop, true);
            }
            break;
        }
        case ISWS_MSG_INFO: {
            isws_info_t info;
            if (isws_decode_info(payload, len, &info) != 0) break;
            pthread_mutex_lock(&g_touch_lock);
            g_info = info;
            g_have_info = true;
            pthread_mutex_unlock(&g_touch_lock);
            fprintf(stderr,
                    "info: screen %ux%u client %ux%u scale=%.4f off=(%.1f,%.1f) rot=%u\n",
                    info.screen_w, info.screen_h, info.client_w, info.client_h,
                    info.scale, info.offset_x, info.offset_y, info.rotation);
            break;
        }
        case ISWS_MSG_FOCUS: {
            isws_focus_t f;
            if (isws_decode_focus(payload, len, &f) == 0)
                fprintf(stderr, "focus: %s\n", f.focused ? "gained" : "lost");
            break;
        }
        case ISWS_MSG_ERROR: {
            uint32_t code;
            const uint8_t *msg;
            size_t mlen;
            if (isws_decode_error(payload, len, &code, &msg, &mlen) == 0)
                fprintf(stderr, "server error %u: %.*s\n", code, (int)mlen, msg);
            break;
        }
        default:
            fprintf(stderr, "unknown message type 0x%02x (%u bytes)\n", type, len);
            break;
    }
}

static void *rx_thread(void *arg) {
    rx_state_t *rx = (rx_state_t *)arg;
    uint8_t payload[ISWS_MAX_PAYLOAD];
    for (;;) {
        uint8_t hdr[ISWS_HEADER_SIZE];
        if (read_exact(rx->fd, hdr, sizeof(hdr)) != 0) break;
        uint16_t type, flags;
        uint32_t len;
        if (isws_decode_header(hdr, sizeof(hdr), &type, &flags, &len) != 0) {
            fprintf(stderr, "rx: bad header, disconnecting\n");
            break;
        }
        if (len > 0 && read_exact(rx->fd, payload, len) != 0) break;
        handle_message(rx, type, payload, len);
        if (atomic_load(&g_stop)) break;
    }
    atomic_store(&g_stop, true);
    pthread_mutex_lock(&rx->lock);
    pthread_cond_signal(&rx->cond);
    pthread_mutex_unlock(&rx->lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Host loopback fake server (IOSWINE_LOOPBACK=1, host builds only)    */
/* ------------------------------------------------------------------ */
#ifdef IOSWINE_HOST_TEST
static void *fake_server_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint8_t payload[ISWS_MAX_PAYLOAD];
    uint64_t frame = 0;
    (void)frame;
    int64_t next_touch_ns = now_ns() + 500000000LL;
    float tx = 100.0f, ty = 100.0f;
    for (;;) {
        /* Short poll so synthetic touches can be injected between reads. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        int64_t now = now_ns();
        if (now >= next_touch_ns) {
            next_touch_ns = now + 500000000LL;
            tx += 37.0f; ty += 23.0f;
            if (tx > (float)g_width) tx = 50.0f;
            if (ty > (float)g_height) ty = 50.0f;
            isws_touch_t t = { .action = ISWS_TOUCH_MOVE, .pointer_id = 0,
                               .pointer_count = 1, .x = tx, .y = ty,
                               .timestamp_ns = now };
            uint8_t buf[ISWS_TOUCH_PAYLOAD_SIZE];
            size_t n = isws_encode_touch(buf, sizeof(buf), &t);
            send_msg(fd, ISWS_MSG_TOUCH, buf, (uint32_t)n);
        }
        if (r <= 0) {
            if (atomic_load(&g_stop)) break;
            continue;
        }
        uint8_t hdr[ISWS_HEADER_SIZE];
        /* Read the header with recvmsg: the HELLO's memfd travels as
         * ancillary data on the first bytes of the message, and a plain
         * recv() would silently discard it. */
        struct iovec hiov = { .iov_base = hdr, .iov_len = sizeof(hdr) };
        uint8_t hcbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr hmh;
        memset(&hmh, 0, sizeof(hmh));
        hmh.msg_iov = &hiov;
        hmh.msg_iovlen = 1;
        hmh.msg_control = hcbuf;
        hmh.msg_controllen = sizeof(hcbuf);
        size_t hgot = 0;
        int passed_fd = -1;
        while (hgot < sizeof(hdr)) {
            hiov.iov_base = hdr + hgot;
            hiov.iov_len = sizeof(hdr) - hgot;
            hmh.msg_controllen = sizeof(hcbuf);
            ssize_t n = recvmsg(fd, &hmh, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (passed_fd >= 0) close(passed_fd);
                goto out;
            }
            if (n == 0) {
                if (passed_fd >= 0) close(passed_fd);
                goto out;
            }
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&hmh); c;
                 c = CMSG_NXTHDR(&hmh, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    memcpy(&passed_fd, CMSG_DATA(c), sizeof(passed_fd));
            }
            hgot += (size_t)n;
        }
        uint16_t type, flags;
        uint32_t len;
        if (isws_decode_header(hdr, sizeof(hdr), &type, &flags, &len) != 0) {
            if (passed_fd >= 0) close(passed_fd);
            break;
        }
        if (len > 0) {
            size_t got = 0;
            while (got < len) {
                ssize_t n = recv(fd, payload + got, len - got, 0);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    if (passed_fd >= 0) close(passed_fd);
                    goto out;
                }
                got += (size_t)n;
            }
        }
        if (type == ISWS_MSG_HELLO) {
            if (passed_fd >= 0) close(passed_fd); /* fake server ignores pixels */
            isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                                .max_in_flight = ISWS_MAX_IN_FLIGHT };
            uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
            size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
            send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn);
            isws_info_t info;
            isws_compute_mapping(1080, 2400, g_width, g_height, 0, &info);
            uint8_t ib[ISWS_INFO_PAYLOAD_SIZE];
            size_t in = isws_encode_info(ib, sizeof(ib), &info);
            send_msg(fd, ISWS_MSG_INFO, ib, (uint32_t)in);
            continue;
        }
        if (passed_fd >= 0) close(passed_fd);
        if (type == ISWS_MSG_PRESENT) {
            isws_present_t p;
            if (isws_decode_present(payload, len, &p) != 0) break;
            frame++;
            /* Synthetic vsync: 16.666 ms grid from now. */
            int64_t vsync = (now / 16666666LL) * 16666666LL;
            isws_frame_done_t d = { .frame_id = p.frame_id, .vsync_ns = vsync,
                                    .status = 0, .reserved = 0 };
            uint8_t db[ISWS_FRAME_DONE_PAYLOAD_SIZE];
            size_t dn = isws_encode_frame_done(db, sizeof(db), &d);
            send_msg(fd, ISWS_MSG_FRAME_DONE, db, (uint32_t)dn);
        } else if (type == ISWS_MSG_GOODBYE) {
            break;
        }
    }
out:
    return NULL;
}
#endif /* IOSWINE_HOST_TEST */

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--width W] [--height H] [--fps F] [--frames N] [--seconds N]\n"
        "  defaults: 1080x2400 @ 60 fps, runs until back key / SIGINT\n"
        "  host loopback: build with -DIOSWINE_HOST_TEST and set IOSWINE_LOOPBACK=1\n",
        prog);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) g_width = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) g_height = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) g_target_fps = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) g_max_frames = atoll(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) g_max_seconds = atoll(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (g_width == 0 || g_height == 0 || g_target_fps == 0 || g_target_fps > 240) {
        fprintf(stderr, "bad geometry/fps\n");
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int fd = -1;
    bool loopback = false;
#ifdef IOSWINE_HOST_TEST
    const char *lb = getenv("IOSWINE_LOOPBACK");
    loopback = (lb && strcmp(lb, "1") == 0);
#endif
    if (loopback) {
#ifdef IOSWINE_HOST_TEST
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            perror("socketpair");
            return 1;
        }
        fd = sv[0];
        pthread_t fake;
        if (pthread_create(&fake, NULL, fake_server_thread, (void *)(intptr_t)sv[1]) != 0) {
            perror("pthread_create");
            return 1;
        }
        pthread_detach(fake);
        fprintf(stderr, "loopback mode: fake server on socketpair\n");
#else
        fprintf(stderr, "loopback requested but not built with -DIOSWINE_HOST_TEST\n");
        return 2;
#endif
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return 1;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        memcpy(addr.sun_path + 1, ISWS_SOCKET_NAME, strlen(ISWS_SOCKET_NAME));
        socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
                                     strlen(ISWS_SOCKET_NAME));
        if (connect(fd, (struct sockaddr *)&addr, alen) != 0) {
            fprintf(stderr, "connect @%s failed: %s (is the presenter app running?)\n",
                    ISWS_SOCKET_NAME, strerror(errno));
            return 1;
        }
    }

    /* Create the memfd: 2 buffers of stride*height BGRA8888. */
    isws_hello_t hello = { .version = ISWS_VERSION, .width = g_width,
                           .height = g_height, .stride = g_width * 4u,
                           .format = ISWS_FORMAT_BGRA8888_PREMUL, .flags = 0 };
    uint32_t err = isws_validate_hello(&hello);
    if (err != ISWS_ERR_NONE) {
        fprintf(stderr, "invalid hello geometry (err %u)\n", err);
        return 2;
    }
    uint64_t mem_size = isws_memfd_size(&hello);
    int mfd = memfd_create_portable("ioswine-surface", MFD_CLOEXEC);
    if (mfd < 0) {
        perror("memfd_create");
        return 1;
    }
    if (ftruncate(mfd, (off_t)mem_size) != 0) {
        perror("ftruncate memfd");
        return 1;
    }
    size_t one_buf = (size_t)hello.stride * hello.height;
    uint8_t *mem = (uint8_t *)mmap(NULL, (size_t)mem_size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mfd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap memfd");
        return 1;
    }
    memset(mem, 0, (size_t)mem_size);

    /* HELLO + memfd via SCM_RIGHTS, in a single sendmsg. */
    uint8_t hbuf[ISWS_HELLO_PAYLOAD_SIZE];
    size_t hn = isws_encode_hello(hbuf, sizeof(hbuf), &hello);
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, ISWS_MSG_HELLO, 0, (uint32_t)hn);
    struct iovec iov[2] = {
        { .iov_base = hdr, .iov_len = sizeof(hdr) },
        { .iov_base = hbuf, .iov_len = hn },
    };
    uint8_t cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &mfd, sizeof(mfd));
    if (sendmsg(fd, &mh, MSG_NOSIGNAL) < 0) {
        perror("sendmsg HELLO");
        return 1;
    }
    /* The server owns its own dup now; keep ours for the mmap. */

    rx_state_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.fd = fd;
    rx.acked_up_to = -1; /* no ack yet; 0 would falsely claim frame 0 done */
    pthread_mutex_init(&rx.lock, NULL);
    /* All deadlines in this program use CLOCK_MONOTONIC (now_ns), so
     * the condvar must too: the default CLOCK_REALTIME would make a
     * monotonic deadline look long past and time out immediately. */
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&rx.cond, &cattr);
    pthread_condattr_destroy(&cattr);
    rx.lat_min_ns = INT64_MAX;
    pthread_t rx_th;
    if (pthread_create(&rx_th, NULL, rx_thread, &rx) != 0) {
        perror("pthread_create rx");
        return 1;
    }

    /* Declared here (not after the READY wait): the `done:` exit path
     * below prints frame_id, and the early `goto done` on no-READY would
     * otherwise jump over its declaration and read it uninitialised. */
    uint64_t frame_id = 0;

    /* Wait for READY (bounded: 5 s). */
    bool ready = false;
    {
        pthread_mutex_lock(&rx.lock);
        struct timespec ts;
        int64_t dl = now_ns() + 5000000000LL;
        ts.tv_sec = dl / 1000000000LL;
        ts.tv_nsec = dl % 1000000000LL;
        while (!rx.have_ready && !atomic_load(&g_stop)) {
            if (pthread_cond_timedwait(&rx.cond, &rx.lock, &ts) == ETIMEDOUT) break;
        }
        ready = rx.have_ready;
        pthread_mutex_unlock(&rx.lock);
    }
    if (!ready || atomic_load(&g_stop)) {
        fprintf(stderr, "no READY from server, exiting\n");
        goto done;
    }

    int64_t frame_ns = 1000000000LL / (int64_t)g_target_fps;
    int64_t start_ns = now_ns();
    int64_t next_ns = start_ns;
    int64_t stat_window_start = start_ns;
    uint64_t frames_in_window = 0;
    uint32_t buf_idx = 0;
    uint64_t in_flight_id = 0;
    bool have_in_flight = false;

    while (!atomic_load(&g_stop)) {
        if (g_max_frames >= 0 && (int64_t)frame_id >= g_max_frames) break;
        if (g_max_seconds >= 0 && now_ns() - start_ns >= g_max_seconds * 1000000000LL) break;

        /* Pace on the previous frame's ack (max 2 in flight == 2 buffers).
         * Wait up to one frame period for the ack, then present anyway
         * on the other buffer; a dropped ack never stalls us forever. */
        if (have_in_flight) {
            pthread_mutex_lock(&rx.lock);
            struct timespec ts;
            int64_t dl = now_ns() + frame_ns;
            ts.tv_sec = dl / 1000000000LL;
            ts.tv_nsec = dl % 1000000000LL;
            while (rx.acked_up_to < (int64_t)in_flight_id &&
                   !atomic_load(&g_stop)) {
                if (pthread_cond_timedwait(&rx.cond, &rx.lock, &ts) == ETIMEDOUT) break;
            }
            pthread_mutex_unlock(&rx.lock);
        }

        int64_t t0 = now_ns();
        uint32_t *px = (uint32_t *)(mem + (size_t)buf_idx * one_buf);
        draw_frame(px, hello.stride / 4, g_width, g_height, frame_id, t0);

        isws_present_t p;
        memset(&p, 0, sizeof(p));
        p.frame_id = frame_id;
        p.buffer_index = buf_idx;
        p.damage_count = 0; /* whole frame for the test client */
        uint8_t pb[16 + 16 * ISWS_MAX_DAMAGE];
        size_t pn = isws_encode_present(pb, sizeof(pb), &p);
        int64_t present_ns = now_ns();
        if (send_msg(fd, ISWS_MSG_PRESENT, pb, (uint32_t)pn) != 0) {
            fprintf(stderr, "send PRESENT failed, exiting\n");
            break;
        }
        /* Record the send time so the rx thread can measure
         * PRESENT -> FRAME_DONE latency for this frame id. */
        pthread_mutex_lock(&rx.lock);
        rx.present_id[frame_id % ISWS_NUM_BUFFERS] = frame_id;
        rx.present_ns[frame_id % ISWS_NUM_BUFFERS] = present_ns;
        pthread_mutex_unlock(&rx.lock);
        frame_id++;
        buf_idx = (buf_idx + 1) % ISWS_NUM_BUFFERS;
        in_flight_id = frame_id - 1;
        have_in_flight = true;
        frames_in_window++;

        /* Stats once a second. */
        int64_t now = now_ns();
        if (now - stat_window_start >= 1000000000LL) {
            double elapsed = (double)(now - stat_window_start) / 1e9;
            double fps = (double)frames_in_window / elapsed;
            pthread_mutex_lock(&rx.lock);
            uint64_t acked = rx.frames_acked;
            uint64_t samples = rx.lat_samples;
            double lat_avg_ms = samples
                ? (double)rx.lat_sum_ns / (double)samples / 1e6 : -1.0;
            /* min/max are cumulative over the run (no per-window reset:
             * a window with no new samples must not print INT64_MAX). */
            double lat_min_ms = samples ? (double)rx.lat_min_ns / 1e6 : -1.0;
            double lat_max_ms = samples ? (double)rx.lat_max_ns / 1e6 : -1.0;
            pthread_mutex_unlock(&rx.lock);
            fprintf(stderr,
                    "fps=%.1f frames=%" PRIu64 " acked=%" PRIu64
                    " latency_ms avg=%.2f min=%.2f max=%.2f (n=%" PRIu64 ")\n",
                    fps, frame_id, acked, lat_avg_ms, lat_min_ms, lat_max_ms,
                    samples);
            stat_window_start = now;
            frames_in_window = 0;
        }

        /* Sleep until the next frame slot. */
        next_ns += frame_ns;
        int64_t delay = next_ns - now_ns();
        if (delay > 0) sleep_ns(delay);
        else next_ns = now_ns(); /* fell behind: resync, do not spiral */
    }

done:
    {
        uint8_t gb[1] = { 0 };
        send_msg(fd, ISWS_MSG_GOODBYE, gb, 0);
    }
    atomic_store(&g_stop, true);
    shutdown(fd, SHUT_RDWR);
    pthread_join(rx_th, NULL);
    munmap(mem, (size_t)mem_size);
    close(mfd);
    close(fd);
    fprintf(stderr, "client exiting after %" PRIu64 " frames%s\n", frame_id,
            atomic_load(&g_got_back) ? " (back key)" : "");
    return 0;
}
