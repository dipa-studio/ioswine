/* present_test.c - host unit test for the lsk-side presenter client.
 *
 * Plain cc, no NDK. present.c itself uses no libc; this harness does
 * (stdio, stdlib, string, sockets, pthreads) only to drive it and to
 * report results. The host-shim linux.h (tests/host/host-shim/)
 * translates present.c's arm64 syscall numbers to x86_64 ones, so
 * the freestanding file builds and runs on the host unchanged.
 *
 * The fake server binds the real abstract socket @ioswine-surface
 * and speaks the protocol, so present_init() runs its true connect
 * path. Each sub-test gets its own listener and fake-server thread
 * with a scripted behaviour (normal flow, server ERROR, disconnect,
 * malformed messages, ...). A separate fuzz section feeds random
 * bytes into the parser through the PRESENT_TEST hook, without a
 * socket, plus a smaller socket-level fuzz with reconnects.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O1 -g -DPRESENT_TEST -I.. -Isrc/common \
 *      -Itests/host/host-shim -o present_test tests/host/present_test.c \
 *      src/present.c src/common/protocol.c -lpthread
 *   ./present_test [fuzz_iterations]
 *
 * Sanitizers (the fuzz loops must be clean under these):
 *   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DPRESENT_TEST -I.. -Isrc/common \
 *      -Itests/host/host-shim -o present_test_asan \
 *      tests/host/present_test.c src/present.c src/common/protocol.c \
 *      -lpthread
 *   ./present_test_asan [fuzz_iterations]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "present.h"
#include "protocol.h"
/* tools/host_tests.sh compiles present_test.c with -iquote $P (which holds
 * present_api.c) and links present.c + protocol.c; include present_api.c here
 * so ioswine_present_frame is tested under ASan/UBSan without modifying
 * host_tests.sh. */
#include "present_api.c"

/* PRESENT_TEST hooks in present.c (only present in the host build). */
extern int present_test_feed(const uint8_t *data, size_t len);
extern void present_test_reset_rx(void);
extern void present_test_set_socket_name(const char *name);

/* Per-process socket name, so concurrent test runs cannot cross-wire
 * (see present_test_set_socket_name in present.c). Set in main(). */
static char g_sock_name[64];

/* ------------------------------------------------------------------ */
/* Check accounting                                                     */
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

#define CHECK_EQ_INT(a, b) do { \
    int _a = (a); \
    int _b = (b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %d, want %d\n", \
               __FILE__, __LINE__, #a, _a, _b); \
    } \
} while (0)

#define CHECK_EQ_U64(a, b) do { \
    uint64_t _a = (uint64_t)(a); \
    uint64_t _b = (uint64_t)(b); \
    g_checks++; \
    if (_a != _b) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %llu, want %llu\n", \
               __FILE__, __LINE__, #a, \
               (unsigned long long)_a, (unsigned long long)_b); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Time helpers                                                         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (xorshift64star, fixed seed)                      */
/* ------------------------------------------------------------------ */

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

static uint32_t rng_u32(void) {
    return (uint32_t)(rng_next() >> 32);
}

/* ------------------------------------------------------------------ */
/* Fake server                                                          */
/* ------------------------------------------------------------------ */

enum {
    FAKE_NORMAL = 1,          /* READY+INFO, scripted events, ack PRESENTs */
    FAKE_MANY_TOUCHES,        /* READY+INFO, then 70 TOUCH messages */
    FAKE_ERROR_NO_READY,      /* ERROR instead of READY */
    FAKE_NO_READY,            /* accept HELLO, send nothing */
    FAKE_CLOSE_AFTER_READY,   /* READY, then close */
    FAKE_BAD_MAGIC,           /* READY, then a header with bad magic */
    FAKE_OVERSIZE,            /* READY, then a header with length 70000 */
    FAKE_MALFORMED_FRAME_DONE,/* READY, then FRAME_DONE with length 20 */
    FAKE_UNKNOWN_TYPE,        /* READY, then a header of type 0x99 */
    FAKE_ERROR_IN_PUMP,       /* READY, then an ERROR message */
    FAKE_DELAYED_ACK,         /* READY+INFO, ack PRESENTs after 100 ms */
    FAKE_DUPLICATE_READY,     /* READY twice, then behave like NORMAL */
    FAKE_RAW,                 /* READY, then random bytes (socket fuzz) */
    FAKE_API_COPY,            /* verify ioswine_present_frame row copies/clip */
    FAKE_API_NO_ACK           /* READY, then accept PRESENTs without FRAME_DONE */
};

#define TEST_W 320u
#define TEST_H 240u

/* Vsync values the fake server stamps on FRAME_DONEs. */
#define VSYNC_FIRST 111111111111LL
#define VSYNC_SECOND 222222222222LL

typedef struct {
    int mode;
    int listen_fd;
    pthread_t thread;
    uint32_t expect_w;
    uint32_t expect_h;
    /* Results, written by the fake thread, read after join. */
    int hello_ok;
    int hello_fd_ok;
    int memfd_size_ok;
    int presents_seen;
    uint64_t present_frame_ids[8];
    int present_buf[8];
    int present_dmg_count[8];
    isws_rect_t present_dmg0[8];
    int pattern_ok[8];
    int api_frames_ok[4];
    int goodbye_seen;
    int accepted;
} fake_ctx_t;

static int fake_send_all(int fd, const void *buf, size_t len) {
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

static int fake_send_msg(int fd, uint16_t type, const uint8_t *payload,
                         uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    if (fake_send_all(fd, hdr, sizeof(hdr)) != 0) return -1;
    if (len > 0 && fake_send_all(fd, payload, len) != 0) return -1;
    return 0;
}

static int fake_recv_exact(int fd, uint8_t *buf, size_t len) {
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

/* Read one message header with recvmsg so an SCM_RIGHTS fd is
 * captured, not discarded. Returns 0 and fills type and len, -1 on
 * EOF/error. Any fd found is stored in *passed_fd (-1 if none). */
static int fake_read_header(int fd, uint16_t *type, uint32_t *len,
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

static int fake_send_ready_info(int fd) {
    isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                        .max_in_flight = ISWS_MAX_IN_FLIGHT };
    isws_info_t info;
    uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
    uint8_t ib[ISWS_INFO_PAYLOAD_SIZE];
    size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
    if (fake_send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn) != 0)
        return -1;
    if (isws_compute_mapping(1080, 2400, TEST_W, TEST_H, 0, &info) != 0)
        return -1;
    {
        size_t in = isws_encode_info(ib, sizeof(ib), &info);
        if (fake_send_msg(fd, ISWS_MSG_INFO, ib, (uint32_t)in) != 0)
            return -1;
    }
    return 0;
}

static int fake_send_touch(int fd, uint32_t action, uint32_t pid,
                           float x, float y) {
    isws_touch_t t = { .action = action, .pointer_id = pid,
                       .pointer_count = 1, .x = x, .y = y,
                       .timestamp_ns = 777000111LL };
    uint8_t buf[ISWS_TOUCH_PAYLOAD_SIZE];
    size_t n = isws_encode_touch(buf, sizeof(buf), &t);
    return fake_send_msg(fd, ISWS_MSG_TOUCH, buf, (uint32_t)n);
}

static int fake_send_key(int fd, uint32_t action) {
    isws_key_t k = { .keycode = ISWS_KEY_BACK, .action = action,
                     .timestamp_ns = 777000222LL };
    uint8_t buf[ISWS_KEY_PAYLOAD_SIZE];
    size_t n = isws_encode_key(buf, sizeof(buf), &k);
    return fake_send_msg(fd, ISWS_MSG_KEY, buf, (uint32_t)n);
}

static int fake_send_focus(int fd, uint32_t focused) {
    isws_focus_t f = { .focused = focused, .reserved = 0 };
    uint8_t buf[ISWS_FOCUS_PAYLOAD_SIZE];
    size_t n = isws_encode_focus(buf, sizeof(buf), &f);
    return fake_send_msg(fd, ISWS_MSG_FOCUS, buf, (uint32_t)n);
}

static int fake_send_frame_done(int fd, uint64_t frame_id, int64_t vsync,
                                uint32_t status) {
    isws_frame_done_t d = { .frame_id = frame_id, .vsync_ns = vsync,
                            .status = status, .reserved = 0 };
    uint8_t buf[ISWS_FRAME_DONE_PAYLOAD_SIZE];
    size_t n = isws_encode_frame_done(buf, sizeof(buf), &d);
    return fake_send_msg(fd, ISWS_MSG_FRAME_DONE, buf, (uint32_t)n);
}

/* Drain the client socket until EOF or GOODBYE. Records goodbye. */
static void fake_drain_until_close(int fd, fake_ctx_t *ctx) {
    uint8_t payload[ISWS_MAX_PAYLOAD];
    for (;;) {
        uint16_t type;
        uint32_t len;
        int pfd = -1;
        if (fake_read_header(fd, &type, &len, &pfd) != 0) {
            if (pfd >= 0) close(pfd);
            return;
        }
        if (pfd >= 0) close(pfd);
        if (len > 0 && fake_recv_exact(fd, payload, len) != 0) return;
        if (type == ISWS_MSG_GOODBYE) {
            ctx->goodbye_seen = 1;
            return;
        }
    }
}

/* Read client messages until the first PRESENT (return 0) or
 * GOODBYE / EOF / error (return -1). Lets the fake server sync
 * with the client: the client only presents after READY, so a bad
 * payload sent after this point can only hit present_pump(), never
 * the init path. */
static int fake_wait_present(int fd, fake_ctx_t *ctx) {
    uint8_t payload[ISWS_MAX_PAYLOAD];
    for (;;) {
        uint16_t type;
        uint32_t len;
        int pfd = -1;
        if (fake_read_header(fd, &type, &len, &pfd) != 0) {
            if (pfd >= 0) close(pfd);
            return -1;
        }
        if (pfd >= 0) close(pfd);
        if (len > 0 && fake_recv_exact(fd, payload, len) != 0) return -1;
        if (type == ISWS_MSG_GOODBYE) {
            ctx->goodbye_seen = 1;
            return -1;
        }
        if (type == ISWS_MSG_PRESENT) return 0;
    }
}

static void *fake_server_thread(void *arg) {
    fake_ctx_t *ctx = (fake_ctx_t *)arg;
    int fd = accept(ctx->listen_fd, NULL, NULL);
    int mfd = -1;
    uint8_t *map = NULL;
    size_t map_len = 0;
    uint32_t want_w = ctx->expect_w ? ctx->expect_w : TEST_W;
    uint32_t want_h = ctx->expect_h ? ctx->expect_h : TEST_H;
    if (fd < 0) return NULL;
    ctx->accepted = 1;

    /* Read and validate the HELLO (+ memfd). */
    {
        uint16_t type;
        uint32_t len;
        int pfd = -1;
        uint8_t payload[ISWS_HELLO_PAYLOAD_SIZE];
        isws_hello_t hello;
        struct stat st;
        if (fake_read_header(fd, &type, &len, &pfd) != 0) goto out;
        if (type != ISWS_MSG_HELLO || len != ISWS_HELLO_PAYLOAD_SIZE)
            goto out;
        if (fake_recv_exact(fd, payload, len) != 0) goto out;
        if (isws_decode_hello(payload, len, &hello) != 0) goto out;
        ctx->hello_ok = (hello.version == ISWS_VERSION &&
                         hello.width == want_w &&
                         hello.height == want_h &&
                         hello.stride == want_w * 4u &&
                         hello.format == ISWS_FORMAT_BGRA8888_PREMUL &&
                         hello.flags == 0);
        ctx->hello_fd_ok = (pfd >= 0);
        if (pfd >= 0) {
            mfd = pfd;
            if (fstat(mfd, &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_size >= (off_t)(want_w * 4u * want_h * 2u))
                ctx->memfd_size_ok = 1;
            map_len = (size_t)want_w * 4u * want_h * 2u;
            map = (uint8_t *)mmap(NULL, map_len, PROT_READ, MAP_SHARED,
                                  mfd, 0);
            if (map == MAP_FAILED) map = NULL;
        }
    }

    switch (ctx->mode) {
        case FAKE_NORMAL:
        case FAKE_DUPLICATE_READY:
        case FAKE_DELAYED_ACK: {
            if (fake_send_ready_info(fd) != 0) goto out;
            if (ctx->mode == FAKE_DUPLICATE_READY) {
                isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                                    .max_in_flight = ISWS_MAX_IN_FLIGHT };
                uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
                size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
                if (fake_send_msg(fd, ISWS_MSG_READY, rb,
                                  (uint32_t)rn) != 0)
                    goto out;
            }
            /* Scripted events, in a fixed order. */
            if (fake_send_touch(fd, ISWS_TOUCH_DOWN, 0, 100.5f, 200.25f) != 0)
                goto out;
            if (fake_send_key(fd, ISWS_KEY_ACTION_DOWN) != 0) goto out;
            if (fake_send_focus(fd, 1) != 0) goto out;
            if (fake_send_touch(fd, ISWS_TOUCH_MOVE, 0, 150.0f, 250.0f) != 0)
                goto out;
            if (fake_send_touch(fd, ISWS_TOUCH_UP, 0, 150.0f, 250.0f) != 0)
                goto out;
            if (fake_send_key(fd, ISWS_KEY_ACTION_UP) != 0) goto out;
            if (fake_send_focus(fd, 0) != 0) goto out;
            /* Answer PRESENTs until GOODBYE / EOF. */
            for (;;) {
                uint16_t type;
                uint32_t len;
                int pfd = -1;
                uint8_t payload[ISWS_MAX_PAYLOAD];
                isws_present_t p;
                int idx;
                if (fake_read_header(fd, &type, &len, &pfd) != 0) {
                    if (pfd >= 0) close(pfd);
                    goto out;
                }
                if (pfd >= 0) close(pfd);
                if (type == ISWS_MSG_GOODBYE) {
                    ctx->goodbye_seen = 1;
                    goto out;
                }
                if (type != ISWS_MSG_PRESENT) goto out;
                if (len > 0 && fake_recv_exact(fd, payload, len) != 0)
                    goto out;
                if (isws_decode_present(payload, len, &p) != 0) goto out;
                idx = ctx->presents_seen;
                if (idx < 8) {
                    size_t one = (size_t)TEST_W * 4u * TEST_H;
                    uint32_t want = (p.buffer_index == 0) ? 0xDEADBEEFu
                                                          : 0xC0FFEE11u;
                    ctx->present_frame_ids[idx] = p.frame_id;
                    ctx->present_buf[idx] = (int)p.buffer_index;
                    ctx->present_dmg_count[idx] = (int)p.damage_count;
                    if (p.damage_count > 0)
                        ctx->present_dmg0[idx] = p.damage[0];
                    if (map != NULL) {
                        uint32_t got;
                        memcpy(&got,
                               map + (size_t)p.buffer_index * one,
                               sizeof(got));
                        ctx->pattern_ok[idx] = (got == want);
                    }
                }
                ctx->presents_seen++;
                if (ctx->mode == FAKE_DELAYED_ACK) sleep_ms(100);
                if (fake_send_frame_done(fd, p.frame_id,
                                         idx == 0 ? VSYNC_FIRST
                                                  : VSYNC_SECOND,
                                         idx == 0 ? 0 : 1) != 0)
                    goto out;
            }
        }
        case FAKE_MANY_TOUCHES: {
            int i;
            if (fake_send_ready_info(fd) != 0) goto out;
            for (i = 0; i < 70; i++) {
                if (fake_send_touch(fd, ISWS_TOUCH_MOVE,
                                    (uint32_t)(i % 10),
                                    (float)i, (float)i) != 0)
                    goto out;
            }
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_ERROR_NO_READY: {
            uint8_t eb[64];
            size_t en = isws_encode_error(eb, sizeof(eb),
                                          ISWS_ERR_VERSION, "bad version",
                                          11);
            fake_send_msg(fd, ISWS_MSG_ERROR, eb, (uint32_t)en);
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_NO_READY:
            /* Send nothing; the client must time out after 5 s. */
            fake_drain_until_close(fd, ctx);
            break;
        case FAKE_CLOSE_AFTER_READY: {
            isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                                .max_in_flight = ISWS_MAX_IN_FLIGHT };
            uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
            size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
            fake_send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn);
            break; /* close the socket by returning */
        }
        case FAKE_BAD_MAGIC: {
            uint8_t bad[ISWS_HEADER_SIZE];
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            memset(bad, 0, sizeof(bad));
            isws_wr_u32le(bad + 0, 0xDEADBEEFu);
            isws_wr_u16le(bad + 4, ISWS_MSG_FRAME_DONE);
            isws_wr_u32le(bad + 8, ISWS_FRAME_DONE_PAYLOAD_SIZE);
            fake_send_all(fd, bad, sizeof(bad));
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_OVERSIZE: {
            uint8_t hdr[ISWS_HEADER_SIZE];
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            isws_encode_header(hdr, ISWS_MSG_FRAME_DONE, 0, 70000);
            fake_send_all(fd, hdr, sizeof(hdr));
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_MALFORMED_FRAME_DONE: {
            uint8_t bad[20];
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            memset(bad, 0, sizeof(bad));
            if (fake_send_msg(fd, ISWS_MSG_FRAME_DONE, bad,
                              sizeof(bad)) != 0)
                goto out;
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_UNKNOWN_TYPE: {
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            fake_send_msg(fd, 0x99, NULL, 0);
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_ERROR_IN_PUMP: {
            uint8_t eb[64];
            size_t en;
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            en = isws_encode_error(eb, sizeof(eb), ISWS_ERR_INTERNAL,
                                   "boom", 4);
            fake_send_msg(fd, ISWS_MSG_ERROR, eb, (uint32_t)en);
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_RAW: {
            uint8_t raw[100];
            size_t i;
            if (fake_send_ready_info(fd) != 0) goto out;
            if (fake_wait_present(fd, ctx) != 0) goto out;
            for (i = 0; i < sizeof(raw); i++)
                raw[i] = (uint8_t)rng_u32();
            fake_send_all(fd, raw, sizeof(raw));
            fake_drain_until_close(fd, ctx);
            break;
        }
        case FAKE_API_COPY:
        case FAKE_API_NO_ACK: {
            isws_ready_t rd = { .version = ISWS_VERSION, .flags = 0,
                                .max_in_flight = ISWS_MAX_IN_FLIGHT };
            uint8_t rb[ISWS_READY_PAYLOAD_SIZE];
            size_t rn = isws_encode_ready(rb, sizeof(rb), &rd);
            if (fake_send_msg(fd, ISWS_MSG_READY, rb, (uint32_t)rn) != 0)
                goto out;
            for (;;) {
                uint16_t type;
                uint32_t len;
                int pfd = -1;
                uint8_t payload[ISWS_MAX_PAYLOAD];
                isws_present_t p;
                int idx;
                if (fake_read_header(fd, &type, &len, &pfd) != 0) {
                    if (pfd >= 0) close(pfd);
                    goto out;
                }
                if (pfd >= 0) close(pfd);
                if (type == ISWS_MSG_GOODBYE) {
                    ctx->goodbye_seen = 1;
                    goto out;
                }
                if (type != ISWS_MSG_PRESENT) goto out;
                if (len > 0 && fake_recv_exact(fd, payload, len) != 0)
                    goto out;
                if (isws_decode_present(payload, len, &p) != 0) goto out;
                idx = ctx->presents_seen;
                if (idx < 8) {
                    ctx->present_frame_ids[idx] = p.frame_id;
                    ctx->present_buf[idx] = (int)p.buffer_index;
                    ctx->present_dmg_count[idx] = (int)p.damage_count;
                }
                if (ctx->mode == FAKE_API_COPY && map != NULL && idx < 4) {
                    size_t one = (size_t)want_w * 4u * want_h;
                    const uint8_t *buf = map + (size_t)p.buffer_index * one;
                    int ok = 1;
                    uint32_t x, y;
                    for (y = 0; y < want_h && ok; y++) {
                        for (x = 0; x < want_w; x++) {
                            uint32_t got;
                            uint32_t expect;
                            memcpy(&got,
                                   buf + ((size_t)y * want_w + x) * 4u, 4);
                            if (idx == 0) {
                                expect = 0xFF000000u | (y << 8) | x;
                            } else if (idx == 1) {
                                if (x < 32u && y < 24u)
                                    expect = 0xFF110000u | (y << 8) | x;
                                else
                                    expect = 0u;
                            } else {
                                expect = 0xFF220000u | (y << 8) | x;
                            }
                            if (got != expect) {
                                ok = 0;
                                break;
                            }
                        }
                    }
                    ctx->api_frames_ok[idx] = ok;
                }
                ctx->presents_seen++;
                if (ctx->mode == FAKE_API_COPY) {
                    if (fake_send_frame_done(fd, p.frame_id,
                                             VSYNC_FIRST + idx, 0) != 0)
                        goto out;
                }
            }
        }
        default:
            break;
    }
out:
    if (map != NULL) munmap(map, map_len);
    if (mfd >= 0) close(mfd);
    close(fd);
    return NULL;
}

/* Bind the abstract socket and start the fake server thread. */
static int fake_start_dims(fake_ctx_t *ctx, int mode, uint32_t w, uint32_t h) {
    struct sockaddr_un addr;
    socklen_t alen;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = mode;
    ctx->expect_w = w;
    ctx->expect_h = h;
    ctx->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, g_sock_name, strlen(g_sock_name));
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
                       strlen(g_sock_name));
    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, alen) != 0) {
        close(ctx->listen_fd);
        return -1;
    }
    if (listen(ctx->listen_fd, 1) != 0) {
        close(ctx->listen_fd);
        return -1;
    }
    if (pthread_create(&ctx->thread, NULL, fake_server_thread, ctx) != 0) {
        close(ctx->listen_fd);
        return -1;
    }
    return 0;
}

static int fake_start(fake_ctx_t *ctx, int mode) {
    return fake_start_dims(ctx, mode, 0, 0);
}

static void fake_stop(fake_ctx_t *ctx) {
    pthread_join(ctx->thread, NULL);
    close(ctx->listen_fd);
    ctx->listen_fd = -1;
}

/* Count open fds of this process (for the leak check). */
static int count_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int dirfd_self;
    int n = 0;
    if (d == NULL) return -1;
    dirfd_self = dirfd(d);
    while ((e = readdir(d)) != NULL) {
        int fd;
        if (e->d_name[0] == '.') continue;
        fd = atoi(e->d_name);
        if (fd == dirfd_self) continue;
        n++;
    }
    closedir(d);
    return n;
}

/* Pump until `want` events are collected or the deadline passes.
 * Returns the number collected. */
static int drain_events(present_event_t *out, int want, int timeout_ms) {
    int got = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (got < want && now_ms() < deadline) {
        int r = present_pump();
        if (r != 0) break;
        while (got < want && present_poll_event(&out[got])) got++;
        if (got < want) sleep_ms(5);
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* Test cases                                                           */
/* ------------------------------------------------------------------ */

static int float_eq(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.001f;
}

static void test_before_init(void) {
    int64_t v = 0;
    present_event_t ev;
    CHECK_EQ_INT(present_connected(), 0);
    CHECK(present_framebuffer(0) == NULL);
    CHECK(present_framebuffer(1) == NULL);
    CHECK_EQ_INT((int)present_stride(), 0);
    CHECK_EQ_INT(present_present(0, NULL, 0), -1);
    CHECK_EQ_INT(present_pump(), -1);
    CHECK_EQ_INT(present_wait_readable(0), -1);
    CHECK_EQ_INT(present_wait_vsync(&v, 0), -1);
    CHECK_EQ_INT(present_poll_event(&ev), 0);
    CHECK_EQ_INT(present_poll_event(NULL), 0);
    CHECK_EQ_U64(present_dropped_events(), 0);
    CHECK_EQ_INT((int)present_last_vsync_ns(), -1);
    CHECK_EQ_U64(present_last_frame_id(), (uint64_t)-1);
    /* Shutdown when not initialised must be a safe no-op. */
    present_shutdown();
    CHECK_EQ_INT(present_connected(), 0);
}

static void test_bad_geometry(void) {
    CHECK_EQ_INT(present_init(0, 100), -1);
    CHECK_EQ_INT(present_connected(), 0);
    CHECK_EQ_INT(present_init(8193, 100), -1);
    CHECK_EQ_INT(present_init(100, 0), -1);
    CHECK_EQ_INT(present_init(100, 8193), -1);
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
}

static void test_no_server(void) {
    /* No listener bound: connect must fail and leave nothing open. */
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), -1);
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
}

/* Full loopback run against FAKE_NORMAL: connect, HELLO, READY,
 * INFO, scripted events, two PRESENTs with FRAME_DONEs (the second
 * dropped), GOODBYE. Verifies vsync timestamps, event contents,
 * the INFO mapping, and that the fake server saw the framebuffer
 * pattern through the shared memfd. */
static void test_normal(void) {
    fake_ctx_t ctx;
    present_event_t ev[8];
    isws_info_t info;
    isws_rect_t rect = { .x = 10, .y = 20, .w = 30, .h = 40 };
    int64_t vsync = -1;
    int got;

    CHECK_EQ_INT(fake_start(&ctx, FAKE_NORMAL), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    CHECK_EQ_INT(present_connected(), 1);
    CHECK_EQ_INT((int)present_stride(), (int)(TEST_W * 4u));
    CHECK(present_framebuffer(0) != NULL);
    CHECK(present_framebuffer(1) != NULL);
    CHECK(present_framebuffer(2) == NULL);
    CHECK(present_framebuffer(-1) == NULL);

    /* Validation errors on a live session. */
    CHECK_EQ_INT(present_present(2, NULL, 0), -1);
    CHECK_EQ_INT(present_present(-1, NULL, 0), -1);
    CHECK_EQ_INT(present_present(0, NULL, 17), -1);
    CHECK_EQ_INT(present_present(0, NULL, 1), -1);

    /* No ack yet: vsync state is empty, waits time out. */
    CHECK_EQ_INT((int)present_last_vsync_ns(), -1);
    CHECK_EQ_U64(present_last_frame_id(), (uint64_t)-1);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 50), 1);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 0), 1);

    /* Buffer 0, whole frame. The fake server checks the pattern. */
    {
        uint32_t *fb0 = (uint32_t *)present_framebuffer(0);
        CHECK(fb0 != NULL);
        if (fb0 != NULL) fb0[0] = 0xDEADBEEFu;
    }
    CHECK_EQ_INT(present_present(0, NULL, 0), 0);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 1000), 0);
    CHECK_EQ_INT((int)vsync, (int)VSYNC_FIRST);
    CHECK_EQ_INT((int)present_last_vsync_ns(), (int)VSYNC_FIRST);
    CHECK_EQ_U64(present_last_frame_id(), 0);

    /* Buffer 1, one damage rect. The ack is status=1 (dropped),
     * which still counts as an ack with its vsync. */
    {
        uint32_t *fb1 = (uint32_t *)present_framebuffer(1);
        CHECK(fb1 != NULL);
        if (fb1 != NULL) fb1[0] = 0xC0FFEE11u;
    }
    CHECK_EQ_INT(present_present(1, &rect, 1), 0);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 1000), 0);
    CHECK_EQ_INT((int)vsync, (int)VSYNC_SECOND);
    CHECK_EQ_INT((int)present_last_vsync_ns(), (int)VSYNC_SECOND);
    CHECK_EQ_U64(present_last_frame_id(), 1);

    /* Drain the 8 queued events: INFO + 7 scripted ones, in order. */
    got = drain_events(ev, 8, 2000);
    CHECK_EQ_INT(got, 8);
    if (got == 8) {
        CHECK_EQ_INT((int)ev[0].kind, (int)PRESENT_EV_INFO);
        CHECK_EQ_INT((int)ev[0].u.info.screen_w, 1080);
        CHECK_EQ_INT((int)ev[0].u.info.screen_h, 2400);
        CHECK_EQ_INT((int)ev[0].u.info.client_w, (int)TEST_W);
        CHECK_EQ_INT((int)ev[0].u.info.client_h, (int)TEST_H);
        CHECK(float_eq(ev[0].u.info.scale, 3.375f));
        CHECK(float_eq(ev[0].u.info.offset_x, 0.0f));
        CHECK(float_eq(ev[0].u.info.offset_y, 795.0f));
        CHECK_EQ_INT((int)ev[0].u.info.rotation, 0);
        CHECK_EQ_INT((int)ev[1].kind, (int)PRESENT_EV_TOUCH);
        CHECK_EQ_INT((int)ev[1].u.touch.action, (int)ISWS_TOUCH_DOWN);
        CHECK_EQ_INT((int)ev[1].u.touch.pointer_id, 0);
        CHECK_EQ_INT((int)ev[1].u.touch.pointer_count, 1);
        CHECK(float_eq(ev[1].u.touch.x, 100.5f));
        CHECK(float_eq(ev[1].u.touch.y, 200.25f));
        CHECK_EQ_INT((int)ev[2].kind, (int)PRESENT_EV_KEY);
        CHECK_EQ_INT((int)ev[2].u.key.keycode, (int)ISWS_KEY_BACK);
        CHECK_EQ_INT((int)ev[2].u.key.action, (int)ISWS_KEY_ACTION_DOWN);
        CHECK_EQ_INT((int)ev[3].kind, (int)PRESENT_EV_FOCUS);
        CHECK_EQ_INT((int)ev[3].u.focus.focused, 1);
        CHECK_EQ_INT((int)ev[4].kind, (int)PRESENT_EV_TOUCH);
        CHECK_EQ_INT((int)ev[4].u.touch.action, (int)ISWS_TOUCH_MOVE);
        CHECK(float_eq(ev[4].u.touch.x, 150.0f));
        CHECK(float_eq(ev[4].u.touch.y, 250.0f));
        CHECK_EQ_INT((int)ev[5].kind, (int)PRESENT_EV_TOUCH);
        CHECK_EQ_INT((int)ev[5].u.touch.action, (int)ISWS_TOUCH_UP);
        CHECK_EQ_INT((int)ev[6].kind, (int)PRESENT_EV_KEY);
        CHECK_EQ_INT((int)ev[6].u.key.action, (int)ISWS_KEY_ACTION_UP);
        CHECK_EQ_INT((int)ev[7].kind, (int)PRESENT_EV_FOCUS);
        CHECK_EQ_INT((int)ev[7].u.focus.focused, 0);
    }
    CHECK_EQ_INT(present_poll_event(&ev[0]), 0); /* queue empty */
    CHECK_EQ_U64(present_dropped_events(), 0);

    info = present_last_info();
    CHECK_EQ_INT((int)info.screen_w, 1080);
    CHECK_EQ_INT((int)info.client_w, (int)TEST_W);
    CHECK(float_eq(info.scale, 3.375f));

    present_shutdown();
    CHECK_EQ_INT(present_connected(), 0);
    fake_stop(&ctx);

    CHECK_EQ_INT(ctx.accepted, 1);
    CHECK_EQ_INT(ctx.hello_ok, 1);
    CHECK_EQ_INT(ctx.hello_fd_ok, 1);
    CHECK_EQ_INT(ctx.memfd_size_ok, 1);
    CHECK_EQ_INT(ctx.presents_seen, 2);
    CHECK_EQ_U64(ctx.present_frame_ids[0], 0);
    CHECK_EQ_U64(ctx.present_frame_ids[1], 1);
    CHECK_EQ_INT(ctx.present_buf[0], 0);
    CHECK_EQ_INT(ctx.present_buf[1], 1);
    CHECK_EQ_INT(ctx.present_dmg_count[0], 0);
    CHECK_EQ_INT(ctx.present_dmg_count[1], 1);
    CHECK_EQ_INT(ctx.present_dmg0[1].x, 10);
    CHECK_EQ_INT(ctx.present_dmg0[1].y, 20);
    CHECK_EQ_INT((int)ctx.present_dmg0[1].w, 30);
    CHECK_EQ_INT((int)ctx.present_dmg0[1].h, 40);
    CHECK_EQ_INT(ctx.pattern_ok[0], 1);
    CHECK_EQ_INT(ctx.pattern_ok[1], 1);
    CHECK_EQ_INT(ctx.goodbye_seen, 1);
}

static void test_reconnect(void) {
    /* A second full run after shutdown: fresh HELLO, frame ids
     * restart at 0. test_normal() already covers the assertions;
     * run it again against a fresh listener. */
    test_normal();
}

static void test_vsync_delayed_ack(void) {
    fake_ctx_t ctx;
    int64_t vsync = -1;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_DELAYED_ACK), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    /* No PRESENT outstanding: waits time out, nothing is acked. */
    CHECK_EQ_INT(present_wait_vsync(&vsync, 50), 1);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 0), 1);
    CHECK_EQ_INT((int)present_last_vsync_ns(), -1);
    /* The fake server acks 100 ms after the PRESENT. */
    CHECK_EQ_INT(present_present(0, NULL, 0), 0);
    CHECK_EQ_INT(present_wait_vsync(&vsync, 2000), 0);
    CHECK_EQ_INT((int)vsync, (int)VSYNC_FIRST);
    present_shutdown();
    fake_stop(&ctx);
    CHECK_EQ_INT(ctx.presents_seen, 1);
    CHECK_EQ_INT(ctx.goodbye_seen, 1);
}

static void test_many_touches(void) {
    fake_ctx_t ctx;
    present_event_t ev;
    int i;
    int got = 0;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_MANY_TOUCHES), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    /* Pump without polling until everything has arrived: the fake
     * server sends INFO + 70 touches in one tight loop, so 300 ms
     * is far more than enough for all of them to be queued. */
    for (i = 0; i < 30; i++) {
        CHECK_EQ_INT(present_pump(), 0);
        sleep_ms(10);
    }
    /* 71 events into a 64 slot queue: the 7 oldest (INFO + touches
     * 0..5) are dropped, touches 6..69 survive in order. */
    CHECK_EQ_U64(present_dropped_events(), 7);
    while (present_poll_event(&ev)) {
        CHECK_EQ_INT((int)ev.kind, (int)PRESENT_EV_TOUCH);
        CHECK(float_eq(ev.u.touch.x, (float)(got + 6)));
        CHECK(float_eq(ev.u.touch.y, (float)(got + 6)));
        got++;
    }
    CHECK_EQ_INT(got, 64);
    CHECK_EQ_INT(present_poll_event(&ev), 0);
    present_shutdown();
    fake_stop(&ctx);
    CHECK_EQ_INT(ctx.goodbye_seen, 1);
}

static void test_duplicate_ready(void) {
    fake_ctx_t ctx;
    int64_t vsync = -1;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_DUPLICATE_READY), 0);
    /* A second READY after the first is tolerated, not an error. */
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    CHECK_EQ_INT(present_connected(), 1);
    CHECK_EQ_INT(present_present(0, NULL, 0), 0);
    /* Wait for the ack before shutting down: without it the client
     * can close the socket while the fake server is still sending
     * its scripted events, and the PRESENT is never read. */
    CHECK_EQ_INT(present_wait_vsync(&vsync, 1000), 0);
    CHECK_EQ_INT((int)vsync, (int)VSYNC_FIRST);
    present_shutdown();
    fake_stop(&ctx);
    CHECK_EQ_INT(ctx.presents_seen, 1);
}

static void test_error_no_ready(void) {
    fake_ctx_t ctx;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_ERROR_NO_READY), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), -1);
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
    fake_stop(&ctx);
}

static void test_no_ready_timeout(void) {
    fake_ctx_t ctx;
    int64_t t0, t1;
    int before, after;
    before = count_fds();
    CHECK_EQ_INT(fake_start(&ctx, FAKE_NO_READY), 0);
    t0 = now_ms();
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), -1);
    t1 = now_ms();
    /* The READY wait is bounded at 5 s. */
    CHECK(t1 - t0 >= 4900 && t1 - t0 < 9000);
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
    fake_stop(&ctx);
    after = count_fds();
    /* The failed init left nothing half-open: same fd count as
     * before the run (socket + memfd + mapping all released). */
    CHECK_EQ_INT(after, before);
}

static void test_close_after_ready(void) {
    fake_ctx_t ctx;
    int rc;
    int64_t deadline;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_CLOSE_AFTER_READY), 0);
    rc = present_init(TEST_W, TEST_H);
    /* Either the init already saw the EOF (returns -1) or it
     * processed READY first and the pump sees the EOF next. Both
     * are correct: the session is dead either way. */
    if (rc == 0) {
        int r = 0;
        deadline = now_ms() + 2000;
        while (now_ms() < deadline) {
            r = present_pump();
            if (r != 0) break;
            sleep_ms(5);
        }
        CHECK_EQ_INT(r, -1);
    }
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
    fake_stop(&ctx);
}

/* Modes where init succeeds and the pump must fail once the bad
 * payload arrives (the fake server syncs on the first PRESENT). */
static void test_pump_fails(int mode) {
    fake_ctx_t ctx;
    int r = 0;
    int64_t deadline;
    CHECK_EQ_INT(fake_start(&ctx, mode), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    CHECK_EQ_INT(present_present(0, NULL, 0), 0);
    deadline = now_ms() + 2000;
    while (now_ms() < deadline) {
        r = present_pump();
        if (r != 0) break;
        sleep_ms(5);
    }
    CHECK_EQ_INT(r, -1);
    CHECK_EQ_INT(present_connected(), 0);
    present_shutdown();
    fake_stop(&ctx);
}

static void test_fd_leak_ok_path(void) {
    fake_ctx_t ctx;
    int before = count_fds();
    int after;
    CHECK_EQ_INT(fake_start(&ctx, FAKE_NORMAL), 0);
    CHECK_EQ_INT(present_init(TEST_W, TEST_H), 0);
    CHECK_EQ_INT(present_present(0, NULL, 0), 0);
    present_shutdown();
    fake_stop(&ctx);
    after = count_fds();
    CHECK_EQ_INT(after, before);
}

/* Socket-level fuzz: the fake server sends 100 random bytes after
 * the first PRESENT; the pump must fail cleanly and the client
 * must reconnect fine afterwards. */
static void test_socket_fuzz(int iters) {
    int i;
    int inits_ok = 0;
    int pumps_failed = 0;
    for (i = 0; i < iters; i++) {
        fake_ctx_t ctx;
        int r = 0;
        int64_t deadline;
        if (fake_start(&ctx, FAKE_RAW) != 0) {
            CHECK(0);
            break;
        }
        if (present_init(TEST_W, TEST_H) == 0) {
            inits_ok++;
            present_present(0, NULL, 0);
            deadline = now_ms() + 2000;
            while (now_ms() < deadline) {
                r = present_pump();
                if (r != 0) break;
                sleep_ms(2);
            }
            if (r != 0) pumps_failed++;
        }
        present_shutdown();
        fake_stop(&ctx);
    }
    printf("socket fuzz: %d iterations, %d inits ok, %d pumps failed\n",
           iters, inits_ok, pumps_failed);
    CHECK_EQ_INT(inits_ok, iters);
    CHECK_EQ_INT(pumps_failed, iters);
}

/* Parser fuzz through the PRESENT_TEST feed hook: random bytes,
 * valid-magic headers with random types and lengths, and valid
 * messages with random field values, fed whole and in 1..7 byte
 * chunks. No socket involved, so no reconnects are needed. */
static void test_feed_fuzz(int iters) {
    static const uint16_t kTypes[] = {
        ISWS_MSG_READY, ISWS_MSG_FRAME_DONE, ISWS_MSG_TOUCH,
        ISWS_MSG_KEY, ISWS_MSG_INFO, ISWS_MSG_FOCUS, ISWS_MSG_ERROR
    };
    static const uint32_t kLens[] = {
        ISWS_READY_PAYLOAD_SIZE, ISWS_FRAME_DONE_PAYLOAD_SIZE,
        ISWS_TOUCH_PAYLOAD_SIZE, ISWS_KEY_PAYLOAD_SIZE,
        ISWS_INFO_PAYLOAD_SIZE, ISWS_FOCUS_PAYLOAD_SIZE, 4
    };
    uint8_t buf[128];
    int i;
    present_shutdown(); /* no live session; the hook is independent */
    for (i = 0; i < iters; i++) {
        size_t len = (size_t)(rng_u32() % 65);
        size_t j;
        int mode = (int)(rng_u32() % 3);
        if (mode == 0) {
            for (j = 0; j < len; j++) buf[j] = (uint8_t)rng_u32();
        } else if (mode == 1) {
            if (len >= 12) {
                isws_wr_u32le(buf + 0, ISWS_MAGIC);
                isws_wr_u16le(buf + 4, (uint16_t)rng_u32());
                isws_wr_u16le(buf + 6, 0);
                isws_wr_u32le(buf + 8, rng_u32() % 70000);
                for (j = 12; j < len; j++) buf[j] = (uint8_t)rng_u32();
            } else {
                for (j = 0; j < len; j++) buf[j] = (uint8_t)rng_u32();
            }
        } else {
            /* Valid message shape, random field values. */
            int t = (int)(rng_u32() % 7);
            uint32_t plen = kLens[t];
            size_t total = ISWS_HEADER_SIZE + plen;
            if (total > sizeof(buf)) total = sizeof(buf);
            isws_encode_header(buf, kTypes[t], 0, plen);
            for (j = ISWS_HEADER_SIZE; j < total; j++)
                buf[j] = (uint8_t)rng_u32();
            len = total;
        }
        if ((rng_u32() % 2) == 0) {
            /* Feed in 1..7 byte chunks to exercise partial assembly. */
            size_t off = 0;
            int rc = 0;
            while (off < len) {
                size_t chunk = 1 + (size_t)(rng_u32() % 7);
                if (chunk > len - off) chunk = len - off;
                rc = present_test_feed(buf + off, chunk);
                if (rc != 0) break;
                off += chunk;
            }
            if (rc != 0) present_test_reset_rx();
        } else {
            if (present_test_feed(buf, len) != 0) present_test_reset_rx();
        }
        if ((rng_u32() % 97) == 0) present_test_reset_rx();
    }
    present_test_reset_rx();
    printf("feed fuzz: %d iterations done\n", iters);
    CHECK(1);
}

static void fill_api_pattern(uint32_t *buf, uint32_t w, uint32_t h,
                             uint32_t stride_bytes, uint32_t tag) {
    uint32_t y, x;
    memset(buf, 0xA5, (size_t)stride_bytes * h);
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)buf + (size_t)y * stride_bytes);
        for (x = 0; x < w; x++) {
            row[x] = tag | (y << 8) | x;
        }
    }
}

static void test_ioswine_present_frame_errors_and_no_server(void) {
    uint32_t px[4] = { 0xFF010203u, 0xFF040506u, 0xFF070809u, 0xFF0A0B0Cu };
    int before, after;
    int64_t t0, t1;

    present_shutdown();
    api_reset();
    before = count_fds();

    /* Invalid inputs fail cleanly without touching sockets. */
    CHECK_EQ_INT(ioswine_present_frame(NULL, 4, 1, 16), -1);
    CHECK_EQ_INT(ioswine_present_frame(px, 0, 1, 16), -1);
    CHECK_EQ_INT(ioswine_present_frame(px, 4, 0, 16), -1);
    CHECK_EQ_INT(ioswine_present_frame(px, ISWS_MAX_WIDTH + 1u, 1,
                                       (ISWS_MAX_WIDTH + 1u) * 4u), -1);
    CHECK_EQ_INT(ioswine_present_frame(px, 1, ISWS_MAX_HEIGHT + 1u, 4), -1);
    CHECK_EQ_INT(ioswine_present_frame(px, 4, 1, 12), -1);

    /* No presenter listening: fails quickly (< 100 ms), no fd leak. */
    t0 = now_ms();
    CHECK_EQ_INT(ioswine_present_frame(px, 2, 2, 8), -1);
    t1 = now_ms();
    CHECK(t1 - t0 < 100);
    CHECK_EQ_INT(present_connected(), 0);
    after = count_fds();
    CHECK_EQ_INT(after, before);
}

static void test_ioswine_present_frame_ready_timeout_bounded(void) {
    fake_ctx_t ctx;
    uint32_t px[64 * 48];
    int before = count_fds();
    int64_t t0, t1;

    present_shutdown();
    api_reset();
    fill_api_pattern(px, 64, 48, 64u * 4u, 0xFF000000u);

    CHECK_EQ_INT(fake_start_dims(&ctx, FAKE_NO_READY, 64, 48), 0);
    t0 = now_ms();
    CHECK_EQ_INT(ioswine_present_frame(px, 64, 48, 64u * 4u), -1);
    t1 = now_ms();
    /* Bounded at ~80 ms (must never block > 100 ms; allow tiny sched slack). */
    CHECK(t1 - t0 >= 65 && t1 - t0 <= 120);
    CHECK_EQ_INT(present_connected(), 0);
    fake_stop(&ctx);
    CHECK_EQ_INT(count_fds(), before);
}

static void test_ioswine_present_frame_copy_and_clip(void) {
    fake_ctx_t ctx;
    const uint32_t w0 = 64, h0 = 48;
    const uint32_t s0 = w0 * 4u + 16u; /* padded source stride */
    uint8_t *f0 = (uint8_t *)malloc((size_t)s0 * h0);
    uint32_t *f1 = (uint32_t *)malloc((size_t)32u * 24u * 4u);
    uint32_t *f2 = (uint32_t *)malloc((size_t)80u * 60u * 4u);
    CHECK(f0 != NULL && f1 != NULL && f2 != NULL);
    if (!f0 || !f1 || !f2) {
        free(f0); free(f1); free(f2);
        return;
    }

    present_shutdown();
    api_reset();
    fill_api_pattern((uint32_t *)f0, w0, h0, s0, 0xFF000000u);
    fill_api_pattern(f1, 32, 24, 32u * 4u, 0xFF110000u);
    fill_api_pattern(f2, 80, 60, 80u * 4u, 0xFF220000u);

    CHECK_EQ_INT(fake_start_dims(&ctx, FAKE_API_COPY, w0, h0), 0);

    /* First call lazily initializes at 64x48 and copies with padded stride. */
    CHECK_EQ_INT(ioswine_present_frame(f0, w0, h0, s0), 0);
    CHECK_EQ_INT(present_connected(), 1);
    sleep_ms(20);

    /* Second call is smaller (32x24): zeroes outer area, copies 32x24. */
    CHECK_EQ_INT(ioswine_present_frame(f1, 32, 24, 32u * 4u), 0);
    sleep_ms(20);

    /* Third call is larger (80x60): clips safely to 64x48 without OOB. */
    CHECK_EQ_INT(ioswine_present_frame(f2, 80, 60, 80u * 4u), 0);
    sleep_ms(20);

    present_shutdown();
    api_reset();
    fake_stop(&ctx);

    CHECK_EQ_INT(ctx.accepted, 1);
    CHECK_EQ_INT(ctx.hello_ok, 1);
    CHECK_EQ_INT(ctx.hello_fd_ok, 1);
    CHECK_EQ_INT(ctx.memfd_size_ok, 1);
    CHECK_EQ_INT(ctx.presents_seen, 3);
    CHECK_EQ_INT(ctx.present_buf[0], 0);
    CHECK_EQ_INT(ctx.present_buf[1], 1);
    CHECK_EQ_INT(ctx.present_buf[2], 0);
    CHECK_EQ_INT(ctx.api_frames_ok[0], 1);
    CHECK_EQ_INT(ctx.api_frames_ok[1], 1);
    CHECK_EQ_INT(ctx.api_frames_ok[2], 1);
    CHECK_EQ_INT(ctx.goodbye_seen, 1);

    free(f0);
    free(f1);
    free(f2);
}

static void test_ioswine_present_frame_no_ack_and_reconnect(void) {
    fake_ctx_t ctx;
    const uint32_t w = 64, h = 48;
    uint32_t *buf = (uint32_t *)malloc((size_t)w * h * 4u);
    int i;
    CHECK(buf != NULL);
    if (!buf) return;

    present_shutdown();
    api_reset();
    fill_api_pattern(buf, w, h, w * 4u, 0xFF000000u);

    /* Server never sends FRAME_DONE: each call must still complete < 100 ms. */
    CHECK_EQ_INT(fake_start_dims(&ctx, FAKE_API_NO_ACK, w, h), 0);
    for (i = 0; i < 4; i++) {
        int64_t t0 = now_ms();
        CHECK_EQ_INT(ioswine_present_frame(buf, w, h, w * 4u), 0);
        int64_t dt = now_ms() - t0;
        CHECK(dt <= 100);
    }
    present_shutdown();
    api_reset();
    fake_stop(&ctx);
    CHECK_EQ_INT(ctx.presents_seen, 4);

    /* If server drops connection, ioswine_present_frame recovers on next server. */
    CHECK_EQ_INT(fake_start_dims(&ctx, FAKE_CLOSE_AFTER_READY, w, h), 0);
    (void)ioswine_present_frame(buf, w, h, w * 4u);
    sleep_ms(30);
    CHECK_EQ_INT(ioswine_present_frame(buf, w, h, w * 4u), -1);
    fake_stop(&ctx);

    CHECK_EQ_INT(fake_start_dims(&ctx, FAKE_API_COPY, w, h), 0);
    CHECK_EQ_INT(ioswine_present_frame(buf, w, h, w * 4u), 0);
    present_shutdown();
    api_reset();
    fake_stop(&ctx);
    CHECK_EQ_INT(ctx.presents_seen, 1);
    CHECK_EQ_INT(ctx.api_frames_ok[0], 1);

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int feed_iters = 10000;
    int sock_iters = 200;
    if (argc > 1) feed_iters = atoi(argv[1]);
    if (argc > 2) sock_iters = atoi(argv[2]);
    if (feed_iters < 0) feed_iters = 0;
    if (sock_iters < 0) sock_iters = 0;

    /* Unique abstract socket name for this process, so a concurrent
     * test run cannot cross-wire with this one. */
    snprintf(g_sock_name, sizeof(g_sock_name), "ioswine-surface-test-%d",
             (int)getpid());
    present_test_set_socket_name(g_sock_name);

    test_before_init();
    test_bad_geometry();
    test_no_server();
    test_normal();
    test_reconnect();
    test_vsync_delayed_ack();
    test_many_touches();
    test_duplicate_ready();
    test_error_no_ready();
    test_no_ready_timeout();
    test_close_after_ready();
    test_pump_fails(FAKE_BAD_MAGIC);
    test_pump_fails(FAKE_OVERSIZE);
    test_pump_fails(FAKE_MALFORMED_FRAME_DONE);
    test_pump_fails(FAKE_UNKNOWN_TYPE);
    test_pump_fails(FAKE_ERROR_IN_PUMP);
    test_fd_leak_ok_path();
    test_ioswine_present_frame_errors_and_no_server();
    test_ioswine_present_frame_ready_timeout_bounded();
    test_ioswine_present_frame_copy_and_clip();
    test_ioswine_present_frame_no_ack_and_reconnect();
    test_socket_fuzz(sock_iters);
    test_feed_fuzz(feed_iters);

    printf("present_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
