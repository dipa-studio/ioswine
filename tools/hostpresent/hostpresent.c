/*
 * hostpresent.c - Host presenter stub for qemu runs (frames + touch on the PC).
 *
 * Listens on the ioswine-surface abstract unix socket, answers HELLO with
 * READY and INFO (1440x3200 logical window by default), maps the shared memfd,
 * acks every PRESENT with FRAME_DONE, keeps the latest composited RGBA frame
 * in memory, and exposes a control FIFO accepting:
 *   tap X Y
 *   swipe X1 Y1 X2 Y2 MS
 *   shot
 *
 * Touch commands go through touch_input.c (ti_mapper_feed) so emitted TOUCH
 * messages match server.c ti_send_touch in the 1440x3200 window space.
 */
#define _GNU_SOURCE
#include "hostpresent.h"
#include "protocol.h"
#include "touch_input.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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

#define DEFAULT_CTL_PATH "/data/local/tmp/ioswine/hostpresent.ctl"
#define DEFAULT_OUT_PATH "/data/local/tmp/ioswine/host-frame.png"
#define DEFAULT_WIN_W    1440u
#define DEFAULT_WIN_H    3200u
#define DEFAULT_VSYNC_HZ 60u

struct hostpresent_ctx {
    char socket_name[108];
    char ctl_path[512];
    char out_path[512];
    uint32_t win_w;
    uint32_t win_h;
    uint32_t vsync_hz;
    int64_t vsync_period_ns;
    int64_t last_vsync_ns;

    int srv_fd;
    int ctl_rd_fd;
    int ctl_wr_fd;
    int stop_pipe[2];
    int stop_requested;

    pthread_t accept_thr;
    pthread_t ctl_thr;
    pthread_t vsync_thr;
    int threads_started;
    int vsync_started;

    /* Lock order: lock -> send_lock (never acquire lock under send_lock). */
    pthread_mutex_t lock;
    pthread_mutex_t send_lock;
    pthread_cond_t clients_done_cv;
    pthread_cond_t vsync_cv;
    int vsync_cv_init;
    int active_clients;

    int client_fd;
    int have_hello;
    isws_hello_t hello;
    int memfd;
    uint8_t *mem;
    size_t mem_size;
    int in_flight[ISWS_NUM_BUFFERS];

    uint8_t *last_frame; /* RGBA8888, hello.width * hello.height * 4 */
    uint32_t last_w;
    uint32_t last_h;
    int have_last_frame;
    uint64_t frames_presented;
    uint64_t frames_dropped;

    isws_info_t mapping;
    int mapping_valid;
    ti_mapper_t mapper;
};

typedef struct {
    hostpresent_ctx_t *ctx;
    int fd;
} client_arg_t;

typedef struct {
    hostpresent_ctx_t *ctx;
    int fd;
} ti_emit_ctx_t;

/* ------------------------------------------------------------------ */
/* Time & small helpers                                                */
/* ------------------------------------------------------------------ */

static int64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void sleep_until_ns(int64_t target_ns) {
    for (;;) {
        int64_t now = now_ns();
        if (now >= target_ns) break;
        int64_t diff = target_ns - now;
        struct timespec ts;
        ts.tv_sec = diff / 1000000000LL;
        ts.tv_nsec = diff % 1000000000LL;
        if (nanosleep(&ts, NULL) == 0) break;
        if (errno != EINTR) break;
    }
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    sleep_until_ns(now_ns() + (int64_t)ms * 1000000LL);
}

static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal zero-dependency PNG writer (RGBA8, stored DEFLATE blocks)   */
/* ------------------------------------------------------------------ */

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t adler32_update(uint32_t adler, const uint8_t *data, size_t len) {
    uint32_t s1 = adler & 0xffffu;
    uint32_t s2 = (adler >> 16) & 0xffffu;
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 5552u) chunk = 5552u;
        for (size_t j = 0; j < chunk; j++) {
            s1 += data[i + j];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        i += chunk;
    }
    return (s2 << 16) | s1;
}

static int write_chunk(FILE *fp, const char type[4],
                       const uint8_t *data, uint32_t len) {
    uint8_t len_buf[4];
    uint8_t crc_buf[4];
    wr_be32(len_buf, len);
    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    if (len > 0) crc = crc32_update(crc, data, len);
    wr_be32(crc_buf, crc);
    if (fwrite(len_buf, 1, 4, fp) != 4) return -1;
    if (fwrite(type, 1, 4, fp) != 4) return -1;
    if (len > 0 && fwrite(data, 1, len, fp) != len) return -1;
    if (fwrite(crc_buf, 1, 4, fp) != 4) return -1;
    return 0;
}

static int write_png_rgba(const char *path, const uint8_t *rgba,
                          uint32_t w, uint32_t h) {
    if (!path || !rgba || w == 0 || h == 0) return -1;
    if (w > ISWS_MAX_WIDTH || h > ISWS_MAX_HEIGHT) return -1;

    size_t row_bytes = (size_t)w * 4u;
    size_t scan_row = 1u + row_bytes;
    if ((size_t)h > SIZE_MAX / scan_row) return -1;
    size_t raw_len = (size_t)h * scan_row;
    size_t nblocks = (raw_len + 65534u) / 65535u;
    if (nblocks == 0) nblocks = 1;
    if (nblocks > (SIZE_MAX - 6u - raw_len) / 5u) return -1;
    size_t idat_len = 2u + nblocks * 5u + raw_len + 4u;
    if (idat_len > UINT32_MAX) return -1;

    uint8_t *idat = (uint8_t *)malloc(idat_len);
    if (!idat) return -1;

    /* Zlib header (CM=8, CINFO=7, no preset dict, FCHECK valid). */
    size_t pos = 0;
    idat[pos++] = 0x78u;
    idat[pos++] = 0x01u;

    /* Emit stored DEFLATE blocks (up to 65535 bytes each) directly from rows. */
    uint32_t adler = 1u;
    size_t src_off = 0;
    while (src_off < raw_len) {
        size_t remain = raw_len - src_off;
        uint16_t blen = remain > 65535u ? 65535u : (uint16_t)remain;
        int is_last = (src_off + blen == raw_len);
        idat[pos++] = (uint8_t)(is_last ? 0x01u : 0x00u);
        idat[pos++] = (uint8_t)(blen & 0xffu);
        idat[pos++] = (uint8_t)((blen >> 8) & 0xffu);
        uint16_t nlen = (uint16_t)(~blen);
        idat[pos++] = (uint8_t)(nlen & 0xffu);
        idat[pos++] = (uint8_t)((nlen >> 8) & 0xffu);

        size_t block_start = pos;
        size_t left = blen;
        while (left > 0) {
            size_t y = src_off / scan_row;
            size_t col_off = src_off % scan_row;
            if (col_off == 0) {
                idat[pos++] = 0x00u; /* PNG filter type 0 (None) */
                src_off++;
                left--;
                if (left == 0) break;
                col_off = 1;
            }
            size_t avail_in_row = scan_row - col_off;
            size_t copy = left < avail_in_row ? left : avail_in_row;
            const uint8_t *row_ptr = rgba + y * row_bytes + (col_off - 1u);
            memcpy(idat + pos, row_ptr, copy);
            pos += copy;
            src_off += copy;
            left -= copy;
        }
        adler = adler32_update(adler, idat + block_start, blen);
    }
    wr_be32(idat + pos, adler);
    pos += 4;

    char tmp_path[576];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        free(idat);
        return -1;
    }

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t ihdr[13];
    wr_be32(ihdr + 0, w);
    wr_be32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* color type: RGBA */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */

    int err = 0;
    if (fwrite(sig, 1, sizeof(sig), fp) != sizeof(sig)) err = -1;
    if (err == 0 && write_chunk(fp, "IHDR", ihdr, sizeof(ihdr)) != 0) err = -1;
    if (err == 0 && write_chunk(fp, "IDAT", idat, (uint32_t)pos) != 0) err = -1;
    if (err == 0 && write_chunk(fp, "IEND", NULL, 0) != 0) err = -1;
    if (fclose(fp) != 0) err = -1;
    free(idat);

    if (err != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket I/O                                                          */
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

static int send_msg_locked(hostpresent_ctx_t *ctx, int fd, uint16_t type,
                           const uint8_t *payload, uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    pthread_mutex_lock(&ctx->send_lock);
    int r = send_all(fd, hdr, sizeof(hdr));
    if (r == 0 && len > 0) r = send_all(fd, payload, len);
    pthread_mutex_unlock(&ctx->send_lock);
    return r;
}

static int send_error(hostpresent_ctx_t *ctx, int fd, uint32_t code,
                      const char *msg) {
    uint8_t buf[256];
    size_t n = isws_encode_error(buf, sizeof(buf), code, msg, strlen(msg));
    return send_msg_locked(ctx, fd, ISWS_MSG_ERROR, buf, (uint32_t)n);
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

static int read_message(int fd, uint16_t *type, uint16_t *flags,
                        uint8_t *payload, uint32_t *paylen, int *fd_out) {
    *fd_out = -1;
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

/* ------------------------------------------------------------------ */
/* Session & frame management                                          */
/* ------------------------------------------------------------------ */

/* Detach active session resources without closing the client socket fd
 * (each client_thread owns and closes its own fd). Caller holds ctx->lock.
 * Synchronizes with send_lock after clearing client_fd so no concurrent
 * send_msg_locked can still be using the fd once client_thread closes it. */
static void detach_session_locked(hostpresent_ctx_t *ctx) {
    if (ctx->mem) {
        munmap(ctx->mem, ctx->mem_size);
        ctx->mem = NULL;
        ctx->mem_size = 0;
    }
    if (ctx->memfd >= 0) {
        close(ctx->memfd);
        ctx->memfd = -1;
    }
    if (ctx->client_fd >= 0) {
        int fd = ctx->client_fd;
        ctx->client_fd = -1;
        shutdown(fd, SHUT_RDWR);
        pthread_mutex_lock(&ctx->send_lock);
        pthread_mutex_unlock(&ctx->send_lock);
    }
    free(ctx->last_frame);
    ctx->last_frame = NULL;
    ctx->last_w = 0;
    ctx->last_h = 0;
    ctx->have_last_frame = 0;
    ctx->have_hello = 0;
    ctx->mapping_valid = 0;
    memset(ctx->in_flight, 0, sizeof(ctx->in_flight));
    ti_mapper_reset(&ctx->mapper);
}

static void send_frame_done(hostpresent_ctx_t *ctx, int fd,
                            uint64_t frame_id, uint32_t status) {
    /* Mirror server.c: stamp FRAME_DONE.vsync_ns from the steady vsync clock
     * (maintained at --vsync-hz, fallback to now_ns() if not yet ticked). */
    pthread_mutex_lock(&ctx->lock);
    int64_t vsync = ctx->last_vsync_ns ? ctx->last_vsync_ns : now_ns();
    pthread_mutex_unlock(&ctx->lock);
    isws_frame_done_t d = {
        .frame_id = frame_id,
        .vsync_ns = vsync,
        .status = status,
        .reserved = 0
    };
    uint8_t buf[ISWS_FRAME_DONE_PAYLOAD_SIZE];
    size_t n = isws_encode_frame_done(buf, sizeof(buf), &d);
    send_msg_locked(ctx, fd, ISWS_MSG_FRAME_DONE, buf, (uint32_t)n);
}

static void *vsync_thread(void *arg) {
    hostpresent_ctx_t *ctx = (hostpresent_ctx_t *)arg;
    int64_t period = ctx->vsync_period_ns;
    int64_t next_ns = now_ns();

    pthread_mutex_lock(&ctx->lock);
    ctx->last_vsync_ns = next_ns;
    while (!ctx->stop_requested) {
        next_ns += period;
        int64_t cur = now_ns();
        if (cur - next_ns > period) {
            /* Host stalled: re-sync rather than firing a catch-up burst. */
            next_ns = cur + period;
        }
        struct timespec ts;
        ts.tv_sec = next_ns / 1000000000LL;
        ts.tv_nsec = next_ns % 1000000000LL;
        int rc = pthread_cond_timedwait(&ctx->vsync_cv, &ctx->lock, &ts);
        if (ctx->stop_requested) break;
        if (rc == ETIMEDOUT || now_ns() >= next_ns) {
            ctx->last_vsync_ns = next_ns;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static void copy_present_to_last_frame(hostpresent_ctx_t *ctx,
                                       const isws_present_t *p) {
    isws_hello_t hello;
    const uint8_t *mem;
    uint8_t *dst;
    pthread_mutex_lock(&ctx->lock);
    if (!ctx->have_hello || !ctx->mem || !ctx->last_frame) {
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    hello = ctx->hello;
    mem = ctx->mem;
    dst = ctx->last_frame;

    const uint8_t *src = mem + (size_t)p->buffer_index * hello.stride * hello.height;
    isws_rect_t rects[ISWS_MAX_DAMAGE];
    size_t nrects = isws_expand_damage(p, hello.width, hello.height, rects);
    uint32_t src_stride_px = hello.stride / 4u;

    for (size_t r = 0; r < nrects; r++) {
        const isws_rect_t *dr = &rects[r];
        uint32_t x0 = (uint32_t)dr->x;
        uint32_t y0 = (uint32_t)dr->y;
        uint32_t x1 = x0 + dr->w;
        uint32_t y1 = y0 + dr->h;
        for (uint32_t y = y0; y < y1; y++) {
            const uint32_t *srow = (const uint32_t *)(src + (size_t)y * src_stride_px * 4u);
            uint8_t *drow = dst + (size_t)y * hello.width * 4u;
            for (uint32_t x = x0; x < x1; x++) {
                uint32_t rgba = isws_bgra_to_rgba_opaque(srow[x]);
                drow[x * 4u + 0] = (uint8_t)(rgba & 0xffu);
                drow[x * 4u + 1] = (uint8_t)((rgba >> 8) & 0xffu);
                drow[x * 4u + 2] = (uint8_t)((rgba >> 16) & 0xffu);
                drow[x * 4u + 3] = (uint8_t)((rgba >> 24) & 0xffu);
            }
        }
    }
    ctx->have_last_frame = 1;
    pthread_mutex_unlock(&ctx->lock);
}

static void *client_thread(void *arg) {
    client_arg_t *ca = (client_arg_t *)arg;
    hostpresent_ctx_t *ctx = ca->ctx;
    int fd = ca->fd;
    free(ca);

    uint8_t *payload = (uint8_t *)malloc(ISWS_MAX_PAYLOAD);
    if (!payload) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->client_fd == fd) detach_session_locked(ctx);
        else {
            pthread_mutex_lock(&ctx->send_lock);
            pthread_mutex_unlock(&ctx->send_lock);
        }
        close(fd);
        ctx->active_clients--;
        pthread_cond_broadcast(&ctx->clients_done_cv);
        pthread_mutex_unlock(&ctx->lock);
        return NULL;
    }

    int hello_done = 0;
    for (;;) {
        uint16_t type = 0, flags = 0;
        uint32_t len = 0;
        int passed_fd = -1;
        if (read_message(fd, &type, &flags, payload, &len, &passed_fd) != 0) {
            if (passed_fd >= 0) close(passed_fd);
            break;
        }
        if (type != ISWS_MSG_HELLO && passed_fd >= 0) {
            close(passed_fd);
            send_error(ctx, fd, ISWS_ERR_BAD_MESSAGE,
                       "unexpected fd on non-HELLO message");
            break;
        }

        if (type == ISWS_MSG_HELLO) {
            if (hello_done) {
                if (passed_fd >= 0) close(passed_fd);
                send_error(ctx, fd, ISWS_ERR_BAD_MESSAGE, "duplicate HELLO");
                break;
            }
            isws_hello_t h;
            if (isws_decode_hello(payload, len, &h) != 0) {
                if (passed_fd >= 0) close(passed_fd);
                send_error(ctx, fd, ISWS_ERR_BAD_HELLO, "malformed HELLO payload");
                break;
            }
            uint32_t err = isws_validate_hello(&h);
            if (err != ISWS_ERR_NONE) {
                if (passed_fd >= 0) close(passed_fd);
                send_error(ctx, fd, err, "HELLO rejected");
                break;
            }
            if (passed_fd < 0) {
                send_error(ctx, fd, ISWS_ERR_NO_FD, "HELLO without memfd");
                break;
            }
            uint64_t need = isws_memfd_size(&h);
            struct stat st;
            if (need == 0 || fstat(passed_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_size < 0 || (uint64_t)st.st_size < need) {
                send_error(ctx, fd, ISWS_ERR_BAD_HELLO, "memfd too small or invalid");
                close(passed_fd);
                break;
            }
            uint8_t *mem = (uint8_t *)mmap(NULL, (size_t)need, PROT_READ,
                                           MAP_SHARED, passed_fd, 0);
            if (mem == MAP_FAILED) {
                send_error(ctx, fd, ISWS_ERR_INTERNAL, "mmap memfd failed");
                close(passed_fd);
                break;
            }
            size_t frame_bytes = (size_t)h.width * (size_t)h.height * 4u;
            uint8_t *frame_buf = (uint8_t *)calloc(1, frame_bytes);
            if (!frame_buf) {
                send_error(ctx, fd, ISWS_ERR_INTERNAL, "oom frame_buf");
                munmap(mem, (size_t)need);
                close(passed_fd);
                break;
            }
            isws_info_t info;
            if (isws_compute_mapping(ctx->win_w, ctx->win_h,
                                     h.width, h.height, 0, &info) != 0) {
                send_error(ctx, fd, ISWS_ERR_BAD_HELLO, "mapping failed");
                free(frame_buf);
                munmap(mem, (size_t)need);
                close(passed_fd);
                break;
            }

            pthread_mutex_lock(&ctx->lock);
            if (ctx->client_fd != fd) {
                /* Replaced or stopped while handshaking. */
                pthread_mutex_unlock(&ctx->lock);
                free(frame_buf);
                munmap(mem, (size_t)need);
                close(passed_fd);
                break;
            }
            ctx->have_hello = 1;
            ctx->hello = h;
            ctx->memfd = passed_fd;
            ctx->mem = mem;
            ctx->mem_size = (size_t)need;
            ctx->last_frame = frame_buf;
            ctx->last_w = h.width;
            ctx->last_h = h.height;
            ctx->have_last_frame = 0;
            ctx->frames_presented = 0;
            ctx->frames_dropped = 0;
            memset(ctx->in_flight, 0, sizeof(ctx->in_flight));
            ctx->mapping = info;
            ctx->mapping_valid = 1;
            ti_mapper_init(&ctx->mapper);
            ti_mapper_set_map(&ctx->mapper, &info, NULL, NULL);
            pthread_mutex_unlock(&ctx->lock);

            hello_done = 1;
            fprintf(stderr,
                    "[hostpresent] client HELLO %ux%u stride=%u -> INFO %ux%u scale=%.2f\n",
                    h.width, h.height, h.stride, info.screen_w, info.screen_h,
                    (double)info.scale);

            isws_ready_t r = {
                .version = ISWS_VERSION,
                .flags = 0,
                .max_in_flight = ISWS_MAX_IN_FLIGHT
            };
            uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
            size_t rn = isws_encode_ready(rb, sizeof(rb), &r);
            if (send_msg_locked(ctx, fd, ISWS_MSG_READY, rb, (uint32_t)rn) != 0)
                break;

            uint8_t ib[ISWS_INFO_PAYLOAD_SIZE];
            size_t in = isws_encode_info(ib, sizeof(ib), &info);
            if (send_msg_locked(ctx, fd, ISWS_MSG_INFO, ib, (uint32_t)in) != 0)
                break;
            continue;
        }

        if (!hello_done) {
            send_error(ctx, fd, ISWS_ERR_BAD_MESSAGE, "first message must be HELLO");
            break;
        }

        if (type == ISWS_MSG_PRESENT) {
            isws_present_t p;
            if (isws_decode_present(payload, len, &p) != 0) {
                send_error(ctx, fd, ISWS_ERR_BAD_PRESENT, "malformed PRESENT");
                break;
            }
            if (p.buffer_index >= ISWS_NUM_BUFFERS) {
                send_error(ctx, fd, ISWS_ERR_BAD_PRESENT, "buffer index out of range");
                break;
            }
            int dropped = 0;
            pthread_mutex_lock(&ctx->lock);
            if (ctx->in_flight[p.buffer_index]) dropped = 1;
            else ctx->in_flight[p.buffer_index] = 1;
            pthread_mutex_unlock(&ctx->lock);

            if (dropped) {
                pthread_mutex_lock(&ctx->lock);
                ctx->frames_dropped++;
                pthread_mutex_unlock(&ctx->lock);
                send_frame_done(ctx, fd, p.frame_id, 1);
                continue;
            }

            copy_present_to_last_frame(ctx, &p);

            pthread_mutex_lock(&ctx->lock);
            ctx->in_flight[p.buffer_index] = 0;
            ctx->frames_presented++;
            pthread_mutex_unlock(&ctx->lock);

            send_frame_done(ctx, fd, p.frame_id, 0);
            continue;
        }

        if (type == ISWS_MSG_KEYBOARD) {
            isws_keyboard_req_t req;
            if (isws_decode_keyboard_req(payload, len, &req) != 0) {
                send_error(ctx, fd, ISWS_ERR_BAD_MESSAGE, "bad KEYBOARD payload");
                break;
            }
            continue;
        }

        if (type == ISWS_MSG_GOODBYE) {
            break;
        }

        send_error(ctx, fd, ISWS_ERR_BAD_MESSAGE, "unknown message type");
        break;
    }

    free(payload);

    pthread_mutex_lock(&ctx->lock);
    if (ctx->client_fd == fd) {
        detach_session_locked(ctx);
    } else {
        pthread_mutex_lock(&ctx->send_lock);
        pthread_mutex_unlock(&ctx->send_lock);
    }
    close(fd);
    ctx->active_clients--;
    pthread_cond_broadcast(&ctx->clients_done_cv);
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static void *accept_thread(void *arg) {
    hostpresent_ctx_t *ctx = (hostpresent_ctx_t *)arg;
    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = ctx->srv_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = ctx->stop_pipe[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        int fd = accept(ctx->srv_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        client_arg_t *ca = (client_arg_t *)malloc(sizeof(*ca));
        if (!ca) {
            close(fd);
            continue;
        }
        ca->ctx = ctx;
        ca->fd = fd;

        pthread_mutex_lock(&ctx->lock);
        if (ctx->stop_requested) {
            pthread_mutex_unlock(&ctx->lock);
            free(ca);
            close(fd);
            break;
        }
        if (ctx->client_fd >= 0) {
            detach_session_locked(ctx);
        }
        ctx->client_fd = fd;
        ctx->active_clients++;
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ca) != 0) {
            if (ctx->client_fd == fd) ctx->client_fd = -1;
            ctx->active_clients--;
            pthread_cond_broadcast(&ctx->clients_done_cv);
            pthread_mutex_unlock(&ctx->lock);
            free(ca);
            close(fd);
            continue;
        }
        pthread_detach(th);
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Touch emission & control FIFO                                       */
/* ------------------------------------------------------------------ */

/* Mirrors server.c ti_send_touch. Caller holds ctx->lock. */
static void ti_send_touch(void *vctx, const isws_touch_t *t) {
    ti_emit_ctx_t *c = (ti_emit_ctx_t *)vctx;
    if (!c || !c->ctx || c->fd < 0 || !t) return;
    uint8_t buf[ISWS_TOUCH_PAYLOAD_SIZE];
    size_t n = isws_encode_touch(buf, sizeof(buf), t);
    if (n == ISWS_TOUCH_PAYLOAD_SIZE) {
        send_msg_locked(c->ctx, c->fd, ISWS_MSG_TOUCH, buf, (uint32_t)n);
    }
}

static void send_motion_event(hostpresent_ctx_t *ctx, uint32_t masked_action,
                              float x, float y, int64_t ts_ns) {
    ti_motion_t motion;
    ti_emit_ctx_t ectx;
    memset(&motion, 0, sizeof(motion));
    motion.masked_action = masked_action;
    motion.pointer_index = 0;
    motion.pointer_count = 1;
    motion.timestamp_ns = ts_ns;
    motion.pointers[0].android_id = 0;
    motion.pointers[0].x = x;
    motion.pointers[0].y = y;
    motion.pointers[0].pressure = (masked_action == TI_ACTION_UP) ? 0.0f : 1.0f;

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->have_hello || !ctx->mapping_valid || ctx->client_fd < 0) {
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    ectx.ctx = ctx;
    ectx.fd = ctx->client_fd;
    ti_mapper_feed(&ctx->mapper, &motion, ti_send_touch, &ectx);
    pthread_mutex_unlock(&ctx->lock);
}

static void cmd_tap(hostpresent_ctx_t *ctx, float x, float y) {
    send_motion_event(ctx, TI_ACTION_DOWN, x, y, now_ns());
    sleep_ms(20);
    send_motion_event(ctx, TI_ACTION_UP, x, y, now_ns());
}

static void cmd_swipe(hostpresent_ctx_t *ctx, float x1, float y1,
                      float x2, float y2, long ms) {
    if (ms < 1) ms = 10;
    if (ms > 60000) ms = 60000;
    int steps = (int)(ms / 16);
    if (steps < 2) steps = 2;
    if (steps > 1000) steps = 1000;

    int64_t start_ns = now_ns();
    send_motion_event(ctx, TI_ACTION_DOWN, x1, y1, start_ns);
    for (int i = 1; i <= steps; i++) {
        int64_t target = start_ns + ((int64_t)ms * 1000000LL * i) / steps;
        sleep_until_ns(target);
        pthread_mutex_lock(&ctx->lock);
        int stop = ctx->stop_requested || ctx->client_fd < 0;
        pthread_mutex_unlock(&ctx->lock);
        if (stop) return;
        float t = (float)i / (float)steps;
        float x = x1 + (x2 - x1) * t;
        float y = y1 + (y2 - y1) * t;
        send_motion_event(ctx, TI_ACTION_MOVE, x, y, now_ns());
    }
    send_motion_event(ctx, TI_ACTION_UP, x2, y2, now_ns());
}

static void cmd_shot(hostpresent_ctx_t *ctx, const char *path) {
    const char *out = (path && path[0]) ? path : ctx->out_path;
    uint8_t *snap = NULL;
    uint32_t w = 0, h = 0;

    pthread_mutex_lock(&ctx->lock);
    if (ctx->have_last_frame && ctx->last_frame &&
        ctx->last_w > 0 && ctx->last_h > 0) {
        w = ctx->last_w;
        h = ctx->last_h;
        size_t sz = (size_t)w * (size_t)h * 4u;
        snap = (uint8_t *)malloc(sz);
        if (snap) memcpy(snap, ctx->last_frame, sz);
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!snap) {
        fprintf(stderr, "[hostpresent] shot ignored: no frame yet\n");
        return;
    }
    if (write_png_rgba(out, snap, w, h) == 0) {
        fprintf(stderr, "[hostpresent] shot wrote %ux%u -> %s\n", w, h, out);
    } else {
        fprintf(stderr, "[hostpresent] shot failed writing %s: %s\n",
                out, strerror(errno));
    }
    free(snap);
}

static void ctl_process_line(hostpresent_ctx_t *ctx, char *line) {
    /* Trim trailing CR/LF/whitespace and leading whitespace. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;

    if (strcmp(line, "shot") == 0) {
        cmd_shot(ctx, NULL);
        return;
    }
    char path[480];
    if (sscanf(line, "shot %479s", path) == 1) {
        cmd_shot(ctx, path);
        return;
    }
    float x = 0.0f, y = 0.0f;
    char extra = '\0';
    if (sscanf(line, "tap %f %f %c", &x, &y, &extra) == 2 &&
        isfinite(x) && isfinite(y)) {
        cmd_tap(ctx, x, y);
        return;
    }
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    long ms = 0;
    if (sscanf(line, "swipe %f %f %f %f %ld %c",
               &x1, &y1, &x2, &y2, &ms, &extra) == 5 &&
        isfinite(x1) && isfinite(y1) && isfinite(x2) && isfinite(y2)) {
        cmd_swipe(ctx, x1, y1, x2, y2, ms);
        return;
    }
    fprintf(stderr, "[hostpresent] unknown ctl command: '%s'\n", line);
}

static void *ctl_thread(void *arg) {
    hostpresent_ctx_t *ctx = (hostpresent_ctx_t *)arg;
    char buf[1024];
    size_t fill = 0;

    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = ctx->ctl_rd_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = ctx->stop_pipe[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        for (;;) {
            ssize_t n = read(ctx->ctl_rd_fd, buf + fill, sizeof(buf) - 1u - fill);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return NULL;
            }
            if (n == 0) break;
            fill += (size_t)n;
            buf[fill] = '\0';

            char *start = buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                ctl_process_line(ctx, start);
                start = nl + 1;
            }
            size_t rem = (size_t)(buf + fill - start);
            if (rem == sizeof(buf) - 1u) {
                /* Line too long without newline: drop it to avoid stall. */
                fill = 0;
            } else if (rem > 0) {
                memmove(buf, start, rem);
                fill = rem;
            } else {
                fill = 0;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

hostpresent_ctx_t *hostpresent_start(const hostpresent_cfg_t *cfg) {
    hostpresent_ctx_t *ctx = (hostpresent_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->srv_fd = -1;
    ctx->ctl_rd_fd = -1;
    ctx->ctl_wr_fd = -1;
    ctx->stop_pipe[0] = -1;
    ctx->stop_pipe[1] = -1;
    ctx->client_fd = -1;
    ctx->memfd = -1;

    const char *sname = (cfg && cfg->socket_name && cfg->socket_name[0])
                        ? cfg->socket_name : ISWS_SOCKET_NAME;
    const char *cpath = (cfg && cfg->ctl_path && cfg->ctl_path[0])
                        ? cfg->ctl_path : DEFAULT_CTL_PATH;
    const char *opath = (cfg && cfg->out_path && cfg->out_path[0])
                        ? cfg->out_path : DEFAULT_OUT_PATH;
    if (strlen(sname) >= sizeof(ctx->socket_name) ||
        strlen(cpath) >= sizeof(ctx->ctl_path) ||
        strlen(opath) >= sizeof(ctx->out_path)) {
        free(ctx);
        return NULL;
    }
    snprintf(ctx->socket_name, sizeof(ctx->socket_name), "%s", sname);
    snprintf(ctx->ctl_path, sizeof(ctx->ctl_path), "%s", cpath);
    snprintf(ctx->out_path, sizeof(ctx->out_path), "%s", opath);
    ctx->win_w = (cfg && cfg->win_w > 0) ? cfg->win_w : DEFAULT_WIN_W;
    ctx->win_h = (cfg && cfg->win_h > 0) ? cfg->win_h : DEFAULT_WIN_H;
    ctx->vsync_hz = (cfg && cfg->vsync_hz > 0) ? cfg->vsync_hz : DEFAULT_VSYNC_HZ;
    ctx->vsync_period_ns = 1000000000LL / (int64_t)ctx->vsync_hz;
    if (ctx->vsync_period_ns <= 0) ctx->vsync_period_ns = 1;
    ctx->last_vsync_ns = now_ns();

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->send_lock, NULL);
    pthread_cond_init(&ctx->clients_done_cv, NULL);
    {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&ctx->vsync_cv, &attr);
        pthread_condattr_destroy(&attr);
        ctx->vsync_cv_init = 1;
    }
    ti_mapper_init(&ctx->mapper);

    if (pipe(ctx->stop_pipe) != 0 ||
        set_nonblock_cloexec(ctx->stop_pipe[0]) != 0 ||
        set_nonblock_cloexec(ctx->stop_pipe[1]) != 0) {
        hostpresent_stop(ctx);
        return NULL;
    }

    ctx->srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->srv_fd < 0 || set_nonblock_cloexec(ctx->srv_fd) != 0) {
        hostpresent_stop(ctx);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    size_t nlen = strlen(ctx->socket_name);
    memcpy(addr.sun_path + 1, ctx->socket_name, nlen);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1u + nlen);
    if (bind(ctx->srv_fd, (struct sockaddr *)&addr, alen) != 0) {
        fprintf(stderr, "[hostpresent] bind @%s failed: %s\n",
                ctx->socket_name, strerror(errno));
        hostpresent_stop(ctx);
        return NULL;
    }
    if (listen(ctx->srv_fd, 4) != 0) {
        fprintf(stderr, "[hostpresent] listen failed: %s\n", strerror(errno));
        hostpresent_stop(ctx);
        return NULL;
    }

    unlink(ctx->ctl_path);
    if (mkfifo(ctx->ctl_path, 0666) != 0) {
        fprintf(stderr, "[hostpresent] mkfifo %s failed: %s\n",
                ctx->ctl_path, strerror(errno));
        hostpresent_stop(ctx);
        return NULL;
    }
    ctx->ctl_rd_fd = open(ctx->ctl_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ctx->ctl_rd_fd < 0) {
        fprintf(stderr, "[hostpresent] open %s rd failed: %s\n",
                ctx->ctl_path, strerror(errno));
        hostpresent_stop(ctx);
        return NULL;
    }
    /* Hold an O_WRONLY fd open so writers closing the FIFO never cause
     * EOF/POLLHUP churn on ctl_rd_fd. */
    ctx->ctl_wr_fd = open(ctx->ctl_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ctx->ctl_wr_fd < 0) {
        fprintf(stderr, "[hostpresent] open %s wr failed: %s\n",
                ctx->ctl_path, strerror(errno));
        hostpresent_stop(ctx);
        return NULL;
    }

    if (pthread_create(&ctx->accept_thr, NULL, accept_thread, ctx) != 0) {
        hostpresent_stop(ctx);
        return NULL;
    }
    if (pthread_create(&ctx->ctl_thr, NULL, ctl_thread, ctx) != 0) {
        uint8_t b = 1;
        (void)write(ctx->stop_pipe[1], &b, 1);
        pthread_join(ctx->accept_thr, NULL);
        hostpresent_stop(ctx);
        return NULL;
    }
    ctx->threads_started = 1;
    if (pthread_create(&ctx->vsync_thr, NULL, vsync_thread, ctx) != 0) {
        hostpresent_stop(ctx);
        return NULL;
    }
    ctx->vsync_started = 1;
    fprintf(stderr,
            "[hostpresent] listening on @%s ctl=%s out=%s (%ux%u, vsync=%uHz)\n",
            ctx->socket_name, ctx->ctl_path, ctx->out_path,
            ctx->win_w, ctx->win_h, ctx->vsync_hz);
    return ctx;
}

void hostpresent_stop(hostpresent_ctx_t *ctx) {
    if (!ctx) return;

    pthread_mutex_lock(&ctx->lock);
    ctx->stop_requested = 1;
    if (ctx->vsync_cv_init) {
        pthread_cond_broadcast(&ctx->vsync_cv);
    }
    if (ctx->stop_pipe[1] >= 0) {
        uint8_t b = 1;
        (void)write(ctx->stop_pipe[1], &b, 1);
    }
    if (ctx->client_fd >= 0) {
        shutdown(ctx->client_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&ctx->lock);

    if (ctx->vsync_started) {
        pthread_join(ctx->vsync_thr, NULL);
        ctx->vsync_started = 0;
    }
    if (ctx->threads_started) {
        pthread_join(ctx->accept_thr, NULL);
        pthread_join(ctx->ctl_thr, NULL);
        ctx->threads_started = 0;
    }

    pthread_mutex_lock(&ctx->lock);
    while (ctx->active_clients > 0) {
        pthread_cond_wait(&ctx->clients_done_cv, &ctx->lock);
    }
    detach_session_locked(ctx);
    pthread_mutex_unlock(&ctx->lock);

    if (ctx->ctl_wr_fd >= 0) close(ctx->ctl_wr_fd);
    if (ctx->ctl_rd_fd >= 0) close(ctx->ctl_rd_fd);
    if (ctx->ctl_path[0]) unlink(ctx->ctl_path);
    if (ctx->srv_fd >= 0) close(ctx->srv_fd);
    if (ctx->stop_pipe[0] >= 0) close(ctx->stop_pipe[0]);
    if (ctx->stop_pipe[1] >= 0) close(ctx->stop_pipe[1]);

    if (ctx->vsync_cv_init) pthread_cond_destroy(&ctx->vsync_cv);
    pthread_cond_destroy(&ctx->clients_done_cv);
    pthread_mutex_destroy(&ctx->send_lock);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

#ifndef HOSTPRESENT_LIB
static volatile sig_atomic_t g_quit = 0;

static void on_sig(int sig) {
    (void)sig;
    g_quit = 1;
}

static int parse_vsync_hz(const char *s, uint32_t *out) {
    if (!s || !s[0]) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1ul || v > 1000ul) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv) {
    hostpresent_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.socket_name = getenv("HOSTPRESENT_SOCKET");
    cfg.ctl_path = getenv("HOSTPRESENT_CTL");
    cfg.out_path = getenv("HOSTPRESENT_OUT");
    const char *dir = getenv("HOSTPRESENT_DIR");
    const char *vsync_env = getenv("HOSTPRESENT_VSYNC_HZ");
    if (vsync_env && vsync_env[0]) {
        if (parse_vsync_hz(vsync_env, &cfg.vsync_hz) != 0) {
            fprintf(stderr, "[hostpresent] invalid HOSTPRESENT_VSYNC_HZ: %s\n",
                    vsync_env);
            return 2;
        }
    }
    bool ctl_set = (cfg.ctl_path && cfg.ctl_path[0]);
    bool out_set = (cfg.out_path && cfg.out_path[0]);

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--socket") == 0) &&
            i + 1 < argc) {
            cfg.socket_name = argv[++i];
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--ctl") == 0) &&
                   i + 1 < argc) {
            cfg.ctl_path = argv[++i];
            ctl_set = true;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) &&
                   i + 1 < argc) {
            cfg.out_path = argv[++i];
            out_set = true;
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--vsync-hz") == 0 && i + 1 < argc) {
            if (parse_vsync_hz(argv[++i], &cfg.vsync_hz) != 0) {
                fprintf(stderr, "[hostpresent] invalid --vsync-hz: %s (expected 1..1000)\n",
                        argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [-s|--socket name] [--dir dir] [-c|--ctl ctl_fifo] [-o|--out out.png] [--vsync-hz N]\n",
                    argv[0]);
            return 2;
        }
    }

    char dir_ctl[512];
    char dir_out[512];
    if (dir && dir[0]) {
        size_t dlen = strlen(dir);
        while (dlen > 1 && dir[dlen - 1] == '/') dlen--;
        if (!ctl_set) {
            int n = snprintf(dir_ctl, sizeof(dir_ctl), "%.*s/hostpresent.ctl",
                             (int)dlen, dir);
            if (n < 0 || (size_t)n >= sizeof(dir_ctl)) {
                fprintf(stderr, "[hostpresent] --dir ctl path too long\n");
                return 2;
            }
            cfg.ctl_path = dir_ctl;
        }
        if (!out_set) {
            int n = snprintf(dir_out, sizeof(dir_out), "%.*s/host-frame.png",
                             (int)dlen, dir);
            if (n < 0 || (size_t)n >= sizeof(dir_out)) {
                fprintf(stderr, "[hostpresent] --dir out path too long\n");
                return 2;
            }
            cfg.out_path = dir_out;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    hostpresent_ctx_t *ctx = hostpresent_start(&cfg);
    if (!ctx) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return 1;
    }

    while (!g_quit) {
        sigsuspend(&oldmask);
    }
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    hostpresent_stop(ctx);
    return 0;
}
#endif /* !HOSTPRESENT_LIB */
