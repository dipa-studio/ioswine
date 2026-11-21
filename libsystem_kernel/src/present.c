/*
 * present.c - lsk-side presenter client for the ioswine-surface protocol.
 *
 * Implements the fixed contract in present.h: connect to the abstract
 * socket @ioswine-surface, create a memfd double buffer, send HELLO +
 * memfd (SCM_RIGHTS), wait for READY, then PRESENT frames and feed
 * FRAME_DONE as the vsync source while TOUCH / KEY / FOCUS / INFO
 * messages land in the in-process event queue.
 *
 * Freestanding C11, no libc: all I/O is raw syscalls via linux.h.
 * The socket address, message header, ancillary data and poll
 * structures are defined locally (pw_ prefix) with the standard
 * Linux 64-bit layout, because no libc socket headers exist here.
 * No function pointers anywhere in this file.
 *
 * Receive path: bytes from the socket go through rx_feed(), which
 * assembles complete messages in g_rx and dispatches them. The same
 * rx_feed() backs the PRESENT_TEST feed hook used by the host test
 * to fuzz the parser without a socket.
 *
 * Failure model: present_init() either fully succeeds or leaves
 * nothing half-open (socket closed, memfd unmapped). A fatal error
 * after a successful init (disconnect, protocol error, server
 * ERROR, send failure) marks the connection dead and closes the
 * socket, but keeps the memfd mapped until present_shutdown() so
 * framebuffer pointers handed out earlier do not dangle.
 * present_init() after a dead connection does a full shutdown
 * first, then reconnects from scratch.
 */
#include "present.h"
#include "linux.h"
#include "protocol.h"

#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);

#ifdef PRESENT_TEST
extern char *getenv(const char *name);
#else
/* Strong definition lives in init.c; weak fallback keeps standalone host
 * tests that compile present.c without init.c or PRESENT_TEST linkable. */
__attribute__((weak))
const char *lsk_getenv(const char *name) {
    (void)name;
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Local constants (not in linux.h)                                     */
/* ------------------------------------------------------------------ */

/* memfd_create is 279 on arm64 (319 is x86_64). linux.h does not
 * define it, so it lives here. Host tests use host-shim-present/linux.h
 * which translates arm64 numbers to x86_64, except it maps 319 -> 319. */
#ifdef PRESENT_TEST
#define SYS_memfd_create 319
#else
#define SYS_memfd_create 279
#endif

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define MSG_NOSIGNAL 0x4000
#define MSG_DONTWAIT 0x40
#define MFD_CLOEXEC 0x0001u
#define SHUT_RDWR 2
#define CLOCK_MONOTONIC 1
#define POLLIN 0x0001

/* Kernel error numbers used here (same on arm64 and x86_64). */
#define LINUX_EINTR 4
#define LINUX_EAGAIN 11

/* READY wait bound, in nanoseconds (5 s, per the protocol). */
#define READY_TIMEOUT_NS 5000000000LL

/* Largest single recvmsg chunk in the pump path. A full message is
 * at most ISWS_HEADER_SIZE + ISWS_MAX_PAYLOAD, assembled across
 * calls by rx_feed(). */
#define RX_CHUNK_SIZE 8192u

/* ------------------------------------------------------------------ */
/* Local wire structures (standard Linux 64-bit layout)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} pw_timespec_t;

typedef struct {
    int fd;
    short events;
    short revents;
} pw_pollfd_t;

typedef struct {
    unsigned short sun_family;
    char sun_path[108];
} pw_sockaddr_un_t;

typedef struct {
    void *iov_base;
    size_t iov_len;
} pw_iovec_t;

typedef struct {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
} pw_cmsghdr_t;

typedef struct {
    void *msg_name;
    unsigned int msg_namelen;
    pw_iovec_t *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
} pw_msghdr_t;

#define PW_CMSG_ALIGN(len) \
    (((len) + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u))
#define PW_CMSG_SPACE(len) \
    (PW_CMSG_ALIGN(sizeof(pw_cmsghdr_t)) + PW_CMSG_ALIGN(len))
#define PW_CMSG_LEN(len) \
    (PW_CMSG_ALIGN(sizeof(pw_cmsghdr_t)) + (len))
#define PW_CMSG_DATA(cmsg) \
    ((uint8_t *)(cmsg) + PW_CMSG_ALIGN(sizeof(pw_cmsghdr_t)))

/* Next header in a control buffer, or NULL when there is none.
 * Control data comes from the kernel, so it is well formed; the
 * length guard below is belt and braces only. */
static pw_cmsghdr_t *pw_cmsg_nxthdr(const pw_msghdr_t *mh,
                                    const pw_cmsghdr_t *cmsg) {
    uint8_t *next;
    uint8_t *end;
    if (cmsg->cmsg_len < sizeof(pw_cmsghdr_t)) return NULL;
    next = (uint8_t *)cmsg + PW_CMSG_ALIGN(cmsg->cmsg_len);
    end = (uint8_t *)mh->msg_control + mh->msg_controllen;
    if (next + (ptrdiff_t)PW_CMSG_ALIGN(sizeof(pw_cmsghdr_t)) > end)
        return NULL;
    return (pw_cmsghdr_t *)next;
}

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int sock;               /* -1 when no socket is open */
    int mfd;                /* -1 when no memfd is open */
    uint8_t *mem;           /* NULL when unmapped */
    uint64_t mem_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;        /* width * 4 */
    uint64_t next_frame_id; /* frame id for the next PRESENT */
    int connected;          /* READY received, session live */
    int dead;               /* fatal error after init; socket closed */
    int ready;              /* READY received for this session */
    int64_t last_vsync_ns;  /* -1 until the first FRAME_DONE */
    uint64_t last_frame_id; /* (uint64_t)-1 until the first FRAME_DONE */
    uint64_t vsync_gen;     /* increments on every FRAME_DONE */
    isws_info_t last_info;  /* 0-filled until the first INFO */
    present_event_t queue[PRESENT_EVENT_QUEUE_LEN];
    uint32_t q_head;        /* index of the oldest event */
    uint32_t q_len;         /* events currently queued */
    uint64_t dropped;       /* events dropped because the queue was full */
    uint64_t touch_invalid; /* TOUCH payloads dropped as invalid (F1) */
} present_state_t;

/* The contract promises -1 / (uint64_t)-1 for the vsync state
 * before any FRAME_DONE arrives, so the never-initialised state
 * must already carry those sentinels (plain zero-init would give
 * 0, which is a plausible vsync timestamp and frame id). The
 * socket and memfd start closed. */
static present_state_t g_s = {
    .sock = -1,
    .mfd = -1,
    .last_vsync_ns = -1,
    .last_frame_id = (uint64_t)-1
};

/* Spinlock protecting g_s and g_rx when a background touch reader
 * thread shares the presenter socket with ioswine_present_frame().
 * Never held across blocking ppoll(). */
static volatile int g_lock;

static void lock_acquire(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&g_lock, __ATOMIC_RELAXED) != 0) {
            /* spin */
        }
    }
}

static void lock_release(void) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

/* Receive assembly buffer: one message at most. */
static uint8_t g_rx[ISWS_HEADER_SIZE + ISWS_MAX_PAYLOAD];
static size_t g_rx_fill;        /* bytes buffered for the current message */
static uint32_t g_rx_len;       /* payload length once the header parsed */
static uint16_t g_rx_type;      /* message type once the header parsed */
static int g_rx_have_header;    /* header parsed for the current message */
static uint8_t g_rx_tmp[RX_CHUNK_SIZE];

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static int64_t now_ns(void) {
    pw_timespec_t ts;
    long r = sys3(SYS_clock_gettime, CLOCK_MONOTONIC, &ts, 0);
    if (r != 0) return 0;
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t present_now_ns(void) {
    return now_ns();
}

/* Single source of truth for the presenter stride: tight, width * 4.
 * The wire protocol allows stride >= width * 4, but this client
 * always sends exactly width * 4, and the memfd layout (buffer 1 at
 * offset stride * height) is built on it. IOSurface bytes_per_row is
 * a different number (64-byte aligned) and is never used here. */
uint32_t present_stride_for_width(uint32_t width) {
    uint64_t stride;
    if (width == 0 || width > ISWS_MAX_WIDTH) return 0;
    stride = (uint64_t)width * 4u;
    if (stride > ISWS_MAX_STRIDE) return 0;
    return (uint32_t)stride;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        sys1(SYS_close, *fd);
        *fd = -1;
    }
}

/* Full teardown: close the socket, unmap and close the memfd, reset
 * every field. Used by present_shutdown() and by the failure path of
 * present_init(). Caller holds g_lock (or runs during single-threaded init). */
static void teardown(void) {
    if (g_s.sock >= 0) {
        sys2(SYS_shutdown, g_s.sock, SHUT_RDWR);
    }
    close_fd(&g_s.sock);
    if (g_s.mem != NULL) {
        sys_munmap(g_s.mem, (size_t)g_s.mem_size);
        g_s.mem = NULL;
    }
    close_fd(&g_s.mfd);
    g_s.mem_size = 0;
    g_s.width = 0;
    g_s.height = 0;
    g_s.stride = 0;
    g_s.next_frame_id = 0;
    __atomic_store_n(&g_s.connected, 0, __ATOMIC_RELEASE);
    g_s.dead = 0;
    g_s.ready = 0;
    g_s.last_vsync_ns = -1;
    g_s.last_frame_id = (uint64_t)-1;
    g_s.vsync_gen = 0;
    memset(&g_s.last_info, 0, sizeof(g_s.last_info));
    g_s.q_head = 0;
    g_s.q_len = 0;
    g_s.dropped = 0;
    g_rx_fill = 0;
    g_rx_len = 0;
    g_rx_type = 0;
    g_rx_have_header = 0;
}

/* Mark the session dead after a fatal error: close the socket so
 * nothing is left half-open, but keep the memfd mapped until
 * present_shutdown() so outstanding framebuffer pointers stay valid.
 * Returns -1 for convenience. Caller holds g_lock. */
static int mark_dead(void) {
    if (g_s.sock >= 0) {
        sys2(SYS_shutdown, g_s.sock, SHUT_RDWR);
    }
    close_fd(&g_s.sock);
    __atomic_store_n(&g_s.connected, 0, __ATOMIC_RELEASE);
    g_s.dead = 1;
    return -1;
}

static int live(void) {
    return g_s.connected && !g_s.dead && g_s.sock >= 0;
}

/* ------------------------------------------------------------------ */
/* Event queue                                                          */
/* ------------------------------------------------------------------ */

/* [present] geometry log: on the
 * first INFO, and whenever the mapping changes, log the whole chain
 * so a black bar or wrong scale can be diagnosed from the device log
 * alone:
 * "[present] geometry screen=<WxH> client=<WxH> scale=<S>
 *  offset=<X>,<Y> hello=<WxH> stride=<N>"
 * screen/client/scale/offset come from the INFO mapping; hello is
 * the session size from the last HELLO and stride is the memfd row
 * stride (width * 4). The line is re-logged when any of these
 * change. Scale prints with two decimals; offsets round to whole
 * screen pixels (never negative under aspect-fit; clamped to 0).
 * Written straight to fd 2: present.c has no log_fn op. */
static void present_log_u32(char *buf, size_t *pos, size_t cap, uint32_t v) {
    char rev[12];
    size_t rn = 0;
    if (v == 0) {
        if (*pos < cap) buf[(*pos)++] = '0';
        return;
    }
    while (v > 0 && rn < sizeof(rev)) {
        rev[rn++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (rn > 0 && *pos < cap) buf[(*pos)++] = rev[--rn];
}

static void present_log_str(char *buf, size_t *pos, size_t cap,
                            const char *s) {
    while (*s && *pos < cap) buf[(*pos)++] = *s++;
}

static void present_log_geometry(const isws_info_t *in) {
    static isws_info_t logged;
    static uint32_t logged_hello_w;
    static uint32_t logged_hello_h;
    static uint32_t logged_stride;
    static int have_logged;
    char buf[192];
    size_t pos = 0;
    uint32_t scale_whole;
    uint32_t scale_frac;
    uint32_t off_x;
    uint32_t off_y;
    if (have_logged && logged.screen_w == in->screen_w &&
        logged.screen_h == in->screen_h &&
        logged.client_w == in->client_w &&
        logged.client_h == in->client_h && logged.scale == in->scale &&
        logged.offset_x == in->offset_x &&
        logged.offset_y == in->offset_y &&
        logged_hello_w == g_s.width && logged_hello_h == g_s.height &&
        logged_stride == g_s.stride) {
        return;
    }
    logged = *in;
    logged_hello_w = g_s.width;
    logged_hello_h = g_s.height;
    logged_stride = g_s.stride;
    have_logged = 1;
    present_log_str(buf, &pos, sizeof(buf) - 1, "[present] geometry screen=");
    present_log_u32(buf, &pos, sizeof(buf) - 1, in->screen_w);
    if (pos < sizeof(buf) - 1) buf[pos++] = 'x';
    present_log_u32(buf, &pos, sizeof(buf) - 1, in->screen_h);
    present_log_str(buf, &pos, sizeof(buf) - 1, " client=");
    present_log_u32(buf, &pos, sizeof(buf) - 1, in->client_w);
    if (pos < sizeof(buf) - 1) buf[pos++] = 'x';
    present_log_u32(buf, &pos, sizeof(buf) - 1, in->client_h);
    present_log_str(buf, &pos, sizeof(buf) - 1, " scale=");
    scale_whole = (uint32_t)in->scale;
    scale_frac = (uint32_t)((in->scale - (float)scale_whole) * 100.0f + 0.5f);
    if (scale_frac >= 100) {
        scale_whole++;
        scale_frac -= 100;
    }
    present_log_u32(buf, &pos, sizeof(buf) - 1, scale_whole);
    if (pos < sizeof(buf) - 1) buf[pos++] = '.';
    if (scale_frac < 10 && pos < sizeof(buf) - 1) buf[pos++] = '0';
    present_log_u32(buf, &pos, sizeof(buf) - 1, scale_frac);
    present_log_str(buf, &pos, sizeof(buf) - 1, " offset=");
    off_x = in->offset_x < 0.0f ? 0 : (uint32_t)(in->offset_x + 0.5f);
    off_y = in->offset_y < 0.0f ? 0 : (uint32_t)(in->offset_y + 0.5f);
    present_log_u32(buf, &pos, sizeof(buf) - 1, off_x);
    if (pos < sizeof(buf) - 1) buf[pos++] = ',';
    present_log_u32(buf, &pos, sizeof(buf) - 1, off_y);
    present_log_str(buf, &pos, sizeof(buf) - 1, " hello=");
    present_log_u32(buf, &pos, sizeof(buf) - 1, g_s.width);
    if (pos < sizeof(buf) - 1) buf[pos++] = 'x';
    present_log_u32(buf, &pos, sizeof(buf) - 1, g_s.height);
    present_log_str(buf, &pos, sizeof(buf) - 1, " stride=");
    present_log_u32(buf, &pos, sizeof(buf) - 1, g_s.stride);
    if (pos < sizeof(buf) - 1) buf[pos++] = '\n';
    (void)sys_write(2, buf, pos);
}

static void queue_push(const present_event_t *ev) {
    uint32_t idx;
    if (g_s.q_len >= PRESENT_EVENT_QUEUE_LEN) {
        /* Full: when the incoming event is a state transition (DOWN /
         * UP / CANCEL, or a non-touch event), prefer dropping the
         * oldest MOVE to make room for it. A MOVE is lossy by design
         * (the next MOVE supersedes it), while DOWN / UP / CANCEL are
         * state transitions: dropping a DOWN strands the finger, and
         * dropping an UP strands it down. When the incoming event is
         * itself a MOVE, drop the oldest event as before (FIFO): a
         * MOVE flood must not evict the INFO or the DOWN that precede
         * it, which is what test_many_touches pins. Only when no MOVE
         * is queued do we drop the oldest event. */
        int incoming_is_transition =
            (ev->kind != PRESENT_EV_TOUCH ||
             ev->u.touch.action != ISWS_TOUCH_MOVE);
        uint32_t drop_off = g_s.q_len;
        uint32_t off;
        if (incoming_is_transition) {
            for (off = 0; off < g_s.q_len; off++) {
                uint32_t at = (g_s.q_head + off) % PRESENT_EVENT_QUEUE_LEN;
                if (g_s.queue[at].kind == PRESENT_EV_TOUCH &&
                    g_s.queue[at].u.touch.action == ISWS_TOUCH_MOVE) {
                    drop_off = off;
                    break;
                }
            }
        }
        if (drop_off < g_s.q_len) {
            /* Remove the MOVE at drop_off, shifting later events one
             * slot toward the head to keep the queue ordered. */
            for (off = drop_off; off + 1 < g_s.q_len; off++) {
                uint32_t dst = (g_s.q_head + off) % PRESENT_EVENT_QUEUE_LEN;
                uint32_t src = (g_s.q_head + off + 1) % PRESENT_EVENT_QUEUE_LEN;
                g_s.queue[dst] = g_s.queue[src];
            }
            g_s.q_len--;
        } else {
            g_s.q_head = (g_s.q_head + 1) % PRESENT_EVENT_QUEUE_LEN;
            g_s.q_len--;
        }
        g_s.dropped++;
    }
    idx = (g_s.q_head + g_s.q_len) % PRESENT_EVENT_QUEUE_LEN;
    g_s.queue[idx] = *ev;
    g_s.q_len++;
}

/* ------------------------------------------------------------------ */
/* Message dispatch                                                     */
/* ------------------------------------------------------------------ */

/* Dispatch one complete message. Returns 0, or -1 on a protocol
 * error (bad payload, unknown type, client-to-server type from the
 * server) or on ERROR from the server. */
static int dispatch(uint16_t type, const uint8_t *payload, uint32_t len) {
    present_event_t ev;
    switch (type) {
        case ISWS_MSG_READY: {
            isws_ready_t r;
            if (isws_decode_ready(payload, len, &r) != 0) return -1;
            if (r.version != ISWS_VERSION) return -1;
            if (r.max_in_flight == 0 ||
                r.max_in_flight > ISWS_MAX_IN_FLIGHT) return -1;
            g_s.ready = 1;
            return 0;
        }
        case ISWS_MSG_FRAME_DONE: {
            isws_frame_done_t d;
            if (isws_decode_frame_done(payload, len, &d) != 0) return -1;
            /* status 1 (dropped) still counts as an ack: the buffer
             * may be reused immediately, and the vsync it carries
             * is the latest one the server reported. */
            g_s.last_frame_id = d.frame_id;
            g_s.last_vsync_ns = d.vsync_ns;
            g_s.vsync_gen++;
            return 0;
        }
        case ISWS_MSG_TOUCH: {
            isws_touch_t t;
            if (isws_decode_touch(payload, len, &t) != 0) {
                /* A single invalid TOUCH payload used to kill the
                 * whole session: dispatch returned -1, rx_feed left
                 * the assembler holding the failed message, and every
                 * later pump re-dispatched it and failed again, so no
                 * further message of any kind was ever processed. A
                 * bad payload is a sender bug, not stream corruption
                 * (the header already validated), so drop just this
                 * message, count it, and keep the session alive. */
                g_s.touch_invalid++;
                return 0;
            }
            ev.kind = PRESENT_EV_TOUCH;
            ev.u.touch = t;
            queue_push(&ev);
            return 0;
        }
        case ISWS_MSG_KEY: {
            isws_key_t k;
            if (isws_decode_key(payload, len, &k) != 0) return -1;
            ev.kind = PRESENT_EV_KEY;
            ev.u.key = k;
            queue_push(&ev);
            return 0;
        }
        case ISWS_MSG_TEXT: {
            isws_text_t t;
            if (isws_decode_text(payload, len, &t) != 0) return -1;
            ev.kind = PRESENT_EV_TEXT;
            ev.u.text = t;
            queue_push(&ev);
            return 0;
        }
        case ISWS_MSG_INFO: {
            isws_info_t in;
            if (isws_decode_info(payload, len, &in) != 0) return -1;
            g_s.last_info = in;
            present_log_geometry(&in);
            ev.kind = PRESENT_EV_INFO;
            ev.u.info = in;
            queue_push(&ev);
            return 0;
        }
        case ISWS_MSG_FOCUS: {
            isws_focus_t f;
            if (isws_decode_focus(payload, len, &f) != 0) return -1;
            ev.kind = PRESENT_EV_FOCUS;
            ev.u.focus = f;
            queue_push(&ev);
            return 0;
        }
        case ISWS_MSG_ERROR: {
            uint32_t code;
            const uint8_t *msg;
            size_t msg_len;
            if (isws_decode_error(payload, len, &code, &msg, &msg_len) != 0)
                return -1;
            /* Any ERROR from the server is fatal for the session. */
            return -1;
        }
        default:
            /* Unknown type, or a client-to-server type (HELLO /
             * PRESENT / GOODBYE) sent by the server. */
            return -1;
    }
}

/* Feed bytes into the receive assembler. Assembles complete
 * messages and dispatches each. Returns 0, or -1 on a protocol
 * error (bad magic, oversize length, bad payload, unknown type,
 * server ERROR). On -1 the assembler state is left as-is; the
 * caller tears the session down. */
static int rx_feed(const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t want;
        size_t take;
        if (!g_rx_have_header) {
            want = ISWS_HEADER_SIZE - g_rx_fill;
        } else {
            want = ISWS_HEADER_SIZE + g_rx_len - g_rx_fill;
        }
        take = len < want ? len : want;
        memcpy(g_rx + g_rx_fill, data, take);
        g_rx_fill += take;
        data += take;
        len -= take;
        if (!g_rx_have_header && g_rx_fill == ISWS_HEADER_SIZE) {
            uint16_t type;
            uint16_t flags;
            uint32_t plen;
            if (isws_decode_header(g_rx, ISWS_HEADER_SIZE, &type, &flags,
                                   &plen) != 0)
                return -1;
            g_rx_type = type;
            g_rx_len = plen;
            g_rx_have_header = 1;
        }
        if (g_rx_have_header &&
            g_rx_fill == ISWS_HEADER_SIZE + g_rx_len) {
            if (dispatch(g_rx_type, g_rx + ISWS_HEADER_SIZE, g_rx_len) != 0)
                return -1;
            g_rx_fill = 0;
            g_rx_len = 0;
            g_rx_type = 0;
            g_rx_have_header = 0;
        }
    }
    return 0;
}

#ifdef PRESENT_TEST
/* Host-test hook: feed bytes straight into the receive assembler
 * without a socket, so the parser can be fuzzed without reconnects.
 * Only compiled into the host test build (-DPRESENT_TEST); the NDK
 * build never defines it. Returns 0, or -1 on a protocol error. */
int present_test_feed(const uint8_t *data, size_t len) {
    return rx_feed(data, len);
}

/* Host-test hook: reset the receive assembler and the event queue
 * between fuzz iterations. */
void present_test_reset_rx(void) {
    g_rx_fill = 0;
    g_rx_len = 0;
    g_rx_type = 0;
    g_rx_have_header = 0;
    g_s.q_head = 0;
    g_s.q_len = 0;
    g_s.dropped = 0;
    g_s.touch_invalid = 0;
}

/* Host-test hook: override the abstract socket name used by
 * present_init (default ISWS_SOCKET_NAME). Concurrent test processes
 * share the abstract socket namespace, so without a per-process name
 * two test runs cross-wire: one process's client connects to the
 * other's fake server, the server closes on the unexpected message,
 * and the client session dies spuriously. Only in the host test
 * build; the NDK build never defines it. */
static char g_test_socket_name[64];
static size_t g_test_socket_name_len = 0;

void present_test_set_socket_name(const char *name) {
    size_t i = 0;
    if (name == NULL) {
        g_test_socket_name_len = 0;
        return;
    }
    while (name[i] != '\0' && i < sizeof(g_test_socket_name) - 1) {
        g_test_socket_name[i] = name[i];
        i++;
    }
    g_test_socket_name[i] = '\0';
    g_test_socket_name_len = i;
}
#endif /* PRESENT_TEST */

/* One non-blocking recvmsg into the assembler. Returns 0 when data
 * was processed or nothing was available, -1 on EOF, a socket
 * error, or a protocol error (including any fd arriving via
 * SCM_RIGHTS: the server must never send one). Caller holds g_lock. */
static int rx_read_once(void) {
    pw_iovec_t iov;
    uint8_t cbuf[PW_CMSG_SPACE(sizeof(int))];
    pw_msghdr_t mh;
    long n;
    iov.iov_base = g_rx_tmp;
    iov.iov_len = sizeof(g_rx_tmp);
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    n = sys3(SYS_recvmsg, g_s.sock, &mh, MSG_DONTWAIT);
    if (n == 0) return -1; /* EOF: server closed the connection */
    if (n < 0) {
        if (n == -LINUX_EINTR || n == -LINUX_EAGAIN) return 0;
        return -1;
    }
    /* Any ancillary data is a protocol error. The server never sends
     * an fd; close any that arrives so nothing leaks. */
    if (mh.msg_controllen > 0) {
        pw_cmsghdr_t *c = (pw_cmsghdr_t *)mh.msg_control;
        if (mh.msg_controllen >= sizeof(pw_cmsghdr_t)) {
            while (c != NULL) {
                if (c->cmsg_level == SOL_SOCKET &&
                    c->cmsg_type == SCM_RIGHTS) {
                    int fd;
                    memcpy(&fd, PW_CMSG_DATA(c), sizeof(fd));
                    if (fd >= 0) sys1(SYS_close, fd);
                    return -1;
                }
                c = pw_cmsg_nxthdr(&mh, c);
            }
        }
    }
    return rx_feed(g_rx_tmp, (size_t)n);
}

/* Poll the socket once with a timeout, then read and dispatch what
 * arrived. timeout_ns < 0 waits forever, 0 polls once. Returns 0
 * when data was processed (or nothing arrived without an error),
 * 1 on timeout, -1 on a fatal error or disconnect. */
static int rx_poll(int64_t timeout_ns) {
    pw_pollfd_t pfd;
    pw_timespec_t ts;
    pw_timespec_t *tsp = NULL;
    long r;
    if (timeout_ns >= 0) {
        ts.tv_sec = timeout_ns / 1000000000LL;
        ts.tv_nsec = timeout_ns % 1000000000LL;
        tsp = &ts;
    }
    pfd.fd = g_s.sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = sys5(SYS_ppoll, &pfd, 1, tsp, 0, 0);
    if (r == 0) return 1; /* timeout */
    if (r < 0) {
        if (r == -LINUX_EINTR) return 0; /* caller retries with fresh time */
        return -1;
    }
    return rx_read_once();
}

/* ------------------------------------------------------------------ */
/* Sending                                                              */
/* ------------------------------------------------------------------ */

/* Send a whole buffer over fd (blocking). Returns 0, -1. */
static int send_all_fd(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        pw_iovec_t iov;
        pw_msghdr_t mh;
        long n;
        iov.iov_base = (void *)buf;
        iov.iov_len = len;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        n = sys3(SYS_sendmsg, fd, &mh, MSG_NOSIGNAL);
        if (n < 0) {
            if (n == -LINUX_EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send a whole buffer over the session socket. Returns 0, -1. */
static int send_all(const uint8_t *buf, size_t len) {
    return send_all_fd(g_s.sock, buf, len);
}

/* Send one message: 12-byte header + payload, in a single sendmsg
 * when it fits (it always does: payloads are at most 272 bytes).
 * Returns 0, -1. */
static int send_msg(uint16_t type, const uint8_t *payload, uint32_t len) {
    uint8_t hdr[ISWS_HEADER_SIZE];
    isws_encode_header(hdr, type, 0, len);
    for (;;) {
        pw_iovec_t iov[2];
        pw_msghdr_t mh;
        long n;
        size_t sent;
        iov[0].iov_base = hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = len;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = iov;
        mh.msg_iovlen = len > 0 ? 2 : 1;
        n = sys3(SYS_sendmsg, g_s.sock, &mh, MSG_NOSIGNAL);
        if (n < 0) {
            /* EINTR before any byte was sent: retry the whole message. */
            if (n == -LINUX_EINTR) continue;
            return -1;
        }
        sent = (size_t)n;
        if (sent >= sizeof(hdr) + len) return 0;
        /* Short send on a stream socket is possible in theory;
         * finish the remainder with plain sends (the header and
         * payload are already framed, and this message carries no
         * ancillary data). */
        if (sent < sizeof(hdr)) {
            if (send_all(hdr + sent, sizeof(hdr) - sent) != 0) return -1;
            if (len > 0 && send_all(payload, len) != 0) return -1;
        } else if (len > 0) {
            size_t off = sent - sizeof(hdr);
            if (send_all(payload + off, len - (uint32_t)off) != 0) return -1;
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/* Wait for READY, dispatching any other messages that arrive first
 * (INFO may precede nothing, but be liberal). Bounded at timeout_ns.
 * Returns 0 on READY, -1 on timeout, error, disconnect or ERROR. */
static int wait_for_ready(int64_t timeout_ns) {
    int64_t deadline = now_ns() + timeout_ns;
    for (;;) {
        int64_t remain;
        int r;
        if (g_s.ready) return 0;
        remain = deadline - now_ns();
        if (remain <= 0) return -1;
        r = rx_poll(remain);
        if (r < 0) return -1;
        if (r == 1) return -1; /* timeout */
    }
}

int present_init_timeout(uint32_t width, uint32_t height, int ready_timeout_ms) {
    isws_hello_t hello;
    uint64_t mem_size;
    int sock = -1;
    int mfd = -1;
    uint8_t *mem = NULL;
    pw_sockaddr_un_t addr;
    size_t addr_len;
    uint8_t hbuf[ISWS_HELLO_PAYLOAD_SIZE];
    size_t hn;
    uint8_t hdr[ISWS_HEADER_SIZE];
    pw_iovec_t iov[2];
    uint8_t cbuf[PW_CMSG_SPACE(sizeof(int))];
    pw_msghdr_t mh;
    pw_cmsghdr_t *c;
    long n;

    /* A live session is replaced: shut it down first so a second
     * init never leaves two sockets or two memfds behind. */
    lock_acquire();
    if (g_s.sock >= 0 || g_s.mem != NULL) teardown();
    lock_release();

    hello.version = ISWS_VERSION;
    hello.width = width;
    hello.height = height;
    hello.stride = present_stride_for_width(width);
    hello.format = ISWS_FORMAT_BGRA8888_PREMUL;
    hello.flags = 0;
    if (hello.stride == 0 ||
        isws_validate_hello(&hello) != ISWS_ERR_NONE) return -1;
    mem_size = isws_memfd_size(&hello);
    if (mem_size == 0) return -1;

    sock = (int)sys3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    {
        const char *sock_name = ISWS_SOCKET_NAME;
        size_t sock_name_len = sizeof(ISWS_SOCKET_NAME) - 1;
#ifdef PRESENT_TEST
        const char *env_sock = getenv("IOSWINE_SURFACE_SOCKET");
#else
        const char *env_sock = lsk_getenv("IOSWINE_SURFACE_SOCKET");
#endif
        if (env_sock != NULL && env_sock[0] != '\0') {
            size_t elen = 0;
            while (env_sock[elen] != '\0' &&
                   elen < sizeof(addr.sun_path) - 1) {
                elen++;
            }
            if (env_sock[elen] != '\0') {
                sys1(SYS_close, sock);
                return -1;
            }
            sock_name = env_sock;
            sock_name_len = elen;
        }
#ifdef PRESENT_TEST
        if (g_test_socket_name_len > 0) {
            sock_name = g_test_socket_name;
            sock_name_len = g_test_socket_name_len;
        }
#endif
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        memcpy(addr.sun_path + 1, sock_name, sock_name_len);
        addr_len = (size_t)((char *)&addr.sun_path - (char *)&addr) + 1 +
                   sock_name_len;
    }
    if (sys3(SYS_connect, sock, &addr, (long)addr_len) != 0) {
        sys1(SYS_close, sock);
        return -1;
    }

    mfd = (int)sys2(SYS_memfd_create, "ioswine-surface", MFD_CLOEXEC);
    if (mfd < 0) {
        sys1(SYS_close, sock);
        return -1;
    }
    if (sys2(SYS_ftruncate, mfd, (long)mem_size) != 0) {
        sys1(SYS_close, mfd);
        sys1(SYS_close, sock);
        return -1;
    }
    mem = (uint8_t *)sys6(SYS_mmap, 0, (long)mem_size,
                           PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if ((long)mem < 0) {
        sys1(SYS_close, mfd);
        sys1(SYS_close, sock);
        return -1;
    }
    memset(mem, 0, (size_t)mem_size);

    /* HELLO + memfd in one sendmsg with SCM_RIGHTS. */
    hn = isws_encode_hello(hbuf, sizeof(hbuf), &hello);
    isws_encode_header(hdr, ISWS_MSG_HELLO, 0, (uint32_t)hn);
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = hbuf;
    iov[1].iov_len = hn;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    c = (pw_cmsghdr_t *)cbuf;
    c->cmsg_len = PW_CMSG_LEN(sizeof(int));
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    memcpy(PW_CMSG_DATA(c), &mfd, sizeof(mfd));
    n = sys3(SYS_sendmsg, sock, &mh, MSG_NOSIGNAL);
    if (n < 0) {
        sys_munmap(mem, (size_t)mem_size);
        sys1(SYS_close, mfd);
        sys1(SYS_close, sock);
        return -1;
    }
    if ((size_t)n < sizeof(hdr) + hn) {
        /* Short send: finish the rest with plain sends. The fd
         * already travelled with the first chunk, so the remainder
         * carries no ancillary data. */
        size_t sent = (size_t)n;
        int bad = 0;
        if (sent < sizeof(hdr)) {
            uint8_t tail[ISWS_HEADER_SIZE + ISWS_HELLO_PAYLOAD_SIZE];
            size_t tlen = 0;
            size_t hleft = sizeof(hdr) - sent;
            memcpy(tail, hdr + sent, hleft);
            tlen += hleft;
            memcpy(tail + tlen, hbuf, hn);
            tlen += hn;
            if (send_all_fd(sock, tail, tlen) != 0) bad = 1;
        } else {
            size_t off = sent - sizeof(hdr);
            if (send_all_fd(sock, hbuf + off, hn - off) != 0) bad = 1;
        }
        if (bad) {
            sys_munmap(mem, (size_t)mem_size);
            sys1(SYS_close, mfd);
            sys1(SYS_close, sock);
            return -1;
        }
    }

    /* Commit the state (connected stays 0 until READY arrives so any
     * background reader waits), then wait for READY. */
    lock_acquire();
    g_s.sock = sock;
    g_s.mfd = mfd;
    g_s.mem = mem;
    g_s.mem_size = mem_size;
    g_s.width = width;
    g_s.height = height;
    g_s.stride = present_stride_for_width(width);
    g_s.next_frame_id = 0;
    __atomic_store_n(&g_s.connected, 0, __ATOMIC_RELEASE);
    g_s.dead = 0;
    g_s.ready = 0;
    g_s.last_vsync_ns = -1;
    g_s.last_frame_id = (uint64_t)-1;
    g_s.vsync_gen = 0;
    memset(&g_s.last_info, 0, sizeof(g_s.last_info));
    g_s.q_head = 0;
    g_s.q_len = 0;
    g_s.dropped = 0;
    g_rx_fill = 0;
    g_rx_len = 0;
    g_rx_type = 0;
    g_rx_have_header = 0;
    lock_release();

    {
        int64_t timeout_ns = ready_timeout_ms < 0
            ? READY_TIMEOUT_NS
            : (int64_t)ready_timeout_ms * 1000000LL;
        if (wait_for_ready(timeout_ns) != 0) {
            lock_acquire();
            teardown();
            lock_release();
            return -1;
        }
    }
    lock_acquire();
    __atomic_store_n(&g_s.connected, 1, __ATOMIC_RELEASE);
    lock_release();
    return 0;
}

int present_init(uint32_t width, uint32_t height) {
    return present_init_timeout(width, height, -1);
}

int present_send_keyboard_request(uint32_t show, uint32_t traits,
                                  float frame_h_pts) {
    isws_keyboard_req_t r;
    uint8_t pb[ISWS_KEYBOARD_PAYLOAD_SIZE];
    size_t n;
    if (!present_connected()) return -1;
    r.show = show ? 1u : 0u;
    r.traits = traits;
    r.frame_h_pts = frame_h_pts;
    r.reserved = 0;
    n = isws_encode_keyboard_req(pb, sizeof(pb), &r);
    if (n != ISWS_KEYBOARD_PAYLOAD_SIZE) return -1;
    return send_msg(ISWS_MSG_KEYBOARD, pb, (uint32_t)n);
}

void present_shutdown(void) {
    lock_acquire();
    if (g_s.sock >= 0 && !g_s.dead) {
        /* Best effort GOODBYE; a dead peer makes this fail silently. */
        send_msg(ISWS_MSG_GOODBYE, NULL, 0);
    }
    teardown();
    lock_release();
}

int present_connected(void) {
    int r;
    lock_acquire();
    r = live() ? 1 : 0;
    lock_release();
    return r;
}

/* ------------------------------------------------------------------ */
/* Framebuffer and presenting                                           */
/* ------------------------------------------------------------------ */

uint8_t *present_framebuffer(int index) {
    uint8_t *p = NULL;
    lock_acquire();
    if (live() && index >= 0 && index < (int)ISWS_NUM_BUFFERS) {
        p = g_s.mem + (size_t)index * g_s.stride * g_s.height;
    }
    lock_release();
    return p;
}

uint32_t present_stride(void) {
    uint32_t r = 0;
    lock_acquire();
    if (live()) r = g_s.stride;
    lock_release();
    return r;
}

uint32_t present_height(void) {
    uint32_t r = 0;
    lock_acquire();
    if (live()) r = g_s.height;
    lock_release();
    return r;
}

int present_buffer_fd(int index) {
    int fd = -1;
    lock_acquire();
    if (live() && index >= 0 && index < (int)ISWS_NUM_BUFFERS) {
        fd = g_s.mfd;
    }
    lock_release();
    return fd;
}

int present_present(int index, const isws_rect_t *damage,
                    uint32_t damage_count) {
    isws_present_t p;
    uint8_t pb[16 + 16 * ISWS_MAX_DAMAGE];
    size_t pn;
    if (index < 0 || index >= (int)ISWS_NUM_BUFFERS) return -1;
    if (damage_count > ISWS_MAX_DAMAGE) return -1;
    if (damage_count > 0 && damage == NULL) return -1;
    lock_acquire();
    if (!live()) {
        lock_release();
        return -1;
    }
    memset(&p, 0, sizeof(p));
    p.frame_id = g_s.next_frame_id++;
    p.buffer_index = (uint32_t)index;
    p.damage_count = damage_count;
    if (damage_count > 0) {
        uint32_t i;
        for (i = 0; i < damage_count; i++) p.damage[i] = damage[i];
    }
    pn = isws_encode_present(pb, sizeof(pb), &p);
    if (pn == 0) {
        lock_release();
        return -1;
    }
    if (send_msg(ISWS_MSG_PRESENT, pb, (uint32_t)pn) != 0) {
        /* The peer is gone: mark the session dead and close the
         * socket. The memfd stays mapped until present_shutdown(). */
        mark_dead();
        lock_release();
        return -1;
    }
    lock_release();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Vsync                                                                */
/* ------------------------------------------------------------------ */

int present_wait_vsync(int64_t *vsync_ns_out, int timeout_ms) {
    uint64_t gen;
    int64_t deadline = 0;
    int first = 1;
    if (vsync_ns_out == NULL) return -1;
    lock_acquire();
    if (!live()) {
        lock_release();
        return -1;
    }
    gen = g_s.vsync_gen;
    lock_release();
    if (timeout_ms >= 0)
        deadline = now_ns() + (int64_t)timeout_ms * 1000000LL;
    for (;;) {
        int64_t remain;
        int sock;
        pw_pollfd_t pfd;
        pw_timespec_t ts;
        pw_timespec_t *tsp = NULL;
        long pr;
        int rr;

        lock_acquire();
        if (!live()) {
            lock_release();
            return -1;
        }
        if (g_s.vsync_gen != gen) {
            *vsync_ns_out = g_s.last_vsync_ns;
            lock_release();
            return 0;
        }
        sock = g_s.sock;
        lock_release();

        if (timeout_ms < 0) {
            remain = 10000000LL; /* 10 ms slice so another reader waking vsync_gen is noticed */
        } else {
            remain = deadline - now_ns();
            if (remain <= 0) {
                if (first && timeout_ms == 0) {
                    remain = 0; /* poll once */
                } else {
                    return 1; /* timeout */
                }
            } else if (remain > 10000000LL) {
                remain = 10000000LL;
            }
        }
        first = 0;

        ts.tv_sec = remain / 1000000000LL;
        ts.tv_nsec = remain % 1000000000LL;
        tsp = &ts;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = sys5(SYS_ppoll, &pfd, 1, tsp, 0, 0);
        if (pr == 0) {
            if (timeout_ms == 0) return 1;
            continue;
        }
        if (pr < 0) {
            if (pr == -LINUX_EINTR) continue;
            lock_acquire();
            if (live() && g_s.sock == sock) mark_dead();
            lock_release();
            return -1;
        }

        lock_acquire();
        if (!live() || g_s.sock != sock) {
            lock_release();
            return -1;
        }
        if (g_s.vsync_gen != gen) {
            *vsync_ns_out = g_s.last_vsync_ns;
            lock_release();
            return 0;
        }
        rr = rx_read_once();
        if (rr < 0) {
            mark_dead();
            lock_release();
            return -1;
        }
        if (g_s.vsync_gen != gen) {
            *vsync_ns_out = g_s.last_vsync_ns;
            lock_release();
            return 0;
        }
        lock_release();
    }
}

int64_t present_last_vsync_ns(void) {
    int64_t r;
    lock_acquire();
    r = g_s.last_vsync_ns;
    lock_release();
    return r;
}

uint64_t present_last_frame_id(void) {
    uint64_t r;
    lock_acquire();
    r = g_s.last_frame_id;
    lock_release();
    return r;
}

uint64_t present_next_frame_id(void) {
    uint64_t r;
    lock_acquire();
    r = g_s.next_frame_id;
    lock_release();
    return r;
}

/* ------------------------------------------------------------------ */
/* Event pump and queue                                                 */
/* ------------------------------------------------------------------ */

int present_pump(void) {
    int r;
    lock_acquire();
    if (!live()) {
        lock_release();
        return -1;
    }
    for (int iter = 0; iter < 16; iter++) {
        r = rx_poll(0);
        if (r < 0) {
            mark_dead();
            lock_release();
            return -1;
        }
        if (r == 1) break; /* no more data available */
    }
    lock_release();
    return 0;
}

int present_wait_readable(int timeout_ms) {
    int sock;
    pw_pollfd_t pfd;
    pw_timespec_t ts;
    pw_timespec_t *tsp = NULL;
    long r;

    lock_acquire();
    sock = live() ? g_s.sock : -1;
    lock_release();
    if (sock < 0) return -1;

    if (timeout_ms >= 0) {
        ts.tv_sec = (int64_t)timeout_ms / 1000LL;
        ts.tv_nsec = ((int64_t)timeout_ms % 1000LL) * 1000000LL;
        tsp = &ts;
    }
    pfd.fd = sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = sys5(SYS_ppoll, &pfd, 1, tsp, 0, 0);
    if (r > 0) return 1;
    if (r == 0 || r == -LINUX_EINTR) return 0;
    return -1;
}

int present_poll_event(present_event_t *out) {
    present_event_t ev;
    if (out == NULL) return 0;
    lock_acquire();
    if (g_s.q_len == 0) {
        lock_release();
        return 0;
    }
    ev = g_s.queue[g_s.q_head];
    g_s.q_head = (g_s.q_head + 1) % PRESENT_EVENT_QUEUE_LEN;
    g_s.q_len--;
    lock_release();
    *out = ev;
    return 1;
}

uint64_t present_dropped_events(void) {
    uint64_t r;
    lock_acquire();
    r = g_s.dropped;
    lock_release();
    return r;
}

/* TOUCH payloads dropped as invalid instead of killing the session
 * (F1). Additive; the counter is per process, not per session. */
uint64_t present_touch_invalid_dropped(void) {
    uint64_t r;
    lock_acquire();
    r = g_s.touch_invalid;
    lock_release();
    return r;
}

isws_info_t present_last_info(void) {
    isws_info_t info;
    lock_acquire();
    info = g_s.last_info;
    lock_release();
    return info;
}
