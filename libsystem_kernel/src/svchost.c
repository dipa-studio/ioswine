/*
 * svchost.c: in-process XPC service host for ioswine.
 *
 * Services register a name and a handler. The name is published through
 * the existing bootstrap_register2 path (routine 0xcc on the bootstrap
 * port), so bootstrap_look_up finds the service with no launchd.c
 * changes. Incoming messages are dispatched synchronously in the
 * sender's thread, as a kobject, exactly like launchd.c and bks.c.
 *
 * Locking: the registry has its own futex lock. Lock order is
 * ipc_lock -> svchost lock, never the reverse. The svchost lock is
 * held only while copying table entries (register reserve/publish,
 * handler snapshot); it is NEVER held across ipc_msg, because ipc_msg
 * takes ipc_lock and the dispatch path takes the svchost lock while
 * already holding ipc_lock. A handler runs holding neither lock, but
 * it runs under ipc_lock held by the dispatching thread, so a handler
 * must never call anything that takes ipc_lock (no ipc_msg, no port
 * allocate/destroy, no mach_msg): handlers are pure dict transforms.
 *
 * Style: freestanding C, same as launchd.c / bks.c. No libc.
 */
#include "svchost.h"
#include "linux.h"
#include "launchd.h"

extern int g_ipc_trace;

/* Freestanding string / memory helpers. */
static size_t sh_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int sh_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int sh_memeq(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return 0;
    }
    return 1;
}

static void sh_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void sh_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

/* Freestanding logging helpers (gated on g_ipc_trace by callers). */
static void sh_print_str(const char *s) {
    if (!s) return;
    sys_write(2, s, sh_strlen(s));
}

static void sh_print_uint(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        while (val > 0) {
            buf[idx--] = '0' + (val % 10);
            val /= 10;
        }
    }
    sh_print_str(&buf[idx + 1]);
}

static void sh_print_hex(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        const char hex[] = "0123456789abcdef";
        while (val > 0) {
            buf[idx--] = hex[val & 0xf];
            val >>= 4;
        }
    }
    sh_print_str(&buf[idx + 1]);
}

/* Registry. */
typedef struct {
    char name[SVCHOST_NAME_MAX];
    svchost_handler_t handler;
    void *ctx;
    struct ipc_port *port;
    mach_port_name_t port_name;
    int in_use;
    int published;
} svchost_entry_t;

static svchost_entry_t g_svchost_services[SVCHOST_MAX_SERVICES];
static int g_svchost_inited = 0;

struct svchost_lock {
    int val;
};
static struct svchost_lock g_svc_lock = {0};

static void svc_lock_acquire(void) {
    while (__atomic_exchange_n(&g_svc_lock.val, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&g_svc_lock.val, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

static void svc_lock_release(void) {
    __atomic_store_n(&g_svc_lock.val, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&g_svc_lock.val, FUTEX_WAKE_PRIVATE, 1);
}

/* Build the minimal {"error": code} reply body with the xpcwire
 * builder. Returns 0 and sets *out_len, or -1 if it does not fit. */
static int sh_build_error_body(uint8_t *body, size_t cap, int64_t code,
                               uint32_t *out_len) {
    xw_builder b;
    if (xw_builder_init(&b, body, cap) != XW_OK) return -1;
    if (xw_dict_begin(&b) != XW_OK) return -1;
    if (xw_dict_add_int64(&b, "error", code) != XW_OK) return -1;
    if (xw_dict_end(&b) != XW_OK) return -1;
    *out_len = (uint32_t)xw_builder_len(&b);
    return 0;
}

/* Fill a reply header in reply_buf for a body of body_len bytes.
 * The reply goes to the request's msgh_local_port with MOVE_SEND_ONCE
 * and msgh_id 0x20000000, which xpc_pipe_routine requires. The body
 * bytes themselves are the caller's responsibility. */
static void sh_fill_reply_header(uint8_t *reply_buf, uint32_t body_len,
                                 mach_port_name_t reply_port) {
    mach_msg_header_t *rep = (mach_msg_header_t *)reply_buf;
    rep->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    rep->msgh_size = sizeof(mach_msg_header_t) + body_len;
    rep->msgh_remote_port = reply_port;
    rep->msgh_local_port = MACH_PORT_NULL;
    rep->msgh_voucher_port = MACH_PORT_NULL;
    rep->msgh_id = SVCHOST_REPLY_MSGH_ID;
}

/* Publish a service name with the launchd shim, through the same
 * bootstrap_register2 message a real client would send (routine 0xcc,
 * dict {type:7, handle:0, name, port:mach_send, flags:0}). The launchd
 * shim stores the port in its g_registered_services table, so a later
 * bootstrap_look_up returns a send right to it with no launchd.c
 * changes. Returns 0 on success, else the "error" value the shim
 * returned (or a Mach error code from ipc_msg).
 *
 * This relies on synchronous kobject dispatch: the reply is already
 * queued on the reply port when ipc_msg moves from its send phase to
 * its receive phase. */
static int sh_bootstrap_register(const char *name,
                                 mach_port_name_t port_name) {
    /* Request buffer: header (24) + body (4) + one port descriptor
     * (12) + XPC dict (<= 200). Reply reuses the same buffer. */
    uint8_t msg[512];
    uint8_t dict[256];
    xw_builder b;
    int r;

    if (xw_builder_init(&b, dict, sizeof(dict)) != XW_OK)
        return KERN_NO_SPACE;
    r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "type", 7);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "handle", 0);
    if (r == XW_OK) r = xw_dict_add_string(&b, "name", name);
    /* The builder records this descriptor in b.descs (obj = the port
     * name, COPY_SEND); the envelope below builds the same descriptor
     * by hand, so the two stay consistent. */
    if (r == XW_OK) r = xw_dict_add_mach_send(&b, "port", (uint64_t)port_name,
                                              MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK) r = xw_dict_add_uint64(&b, "flags", 0);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return KERN_NO_SPACE; /* dict did not fit */
    uint32_t dict_len = (uint32_t)xw_builder_len(&b);

    mach_port_name_t reply_port = ipc_mach_reply_port();
    if (reply_port == MACH_PORT_NULL) return KERN_RESOURCE_SHORTAGE;

    mach_msg_header_t *hdr = (mach_msg_header_t *)msg;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                    MACH_MSG_TYPE_MAKE_SEND_ONCE) |
                     MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
                     sizeof(mach_msg_port_descriptor_t) + dict_len;
    hdr->msgh_remote_port = SVCHOST_BOOTSTRAP_PORT;
    hdr->msgh_local_port = reply_port;
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = SVCHOST_REQUEST_MSGH_ID | LAUNCHD_ROUTINE_REGISTER;

    mach_msg_body_t *body = (mach_msg_body_t *)(hdr + 1);
    body->msgh_descriptor_count = 1;
    mach_msg_port_descriptor_t *desc =
        (mach_msg_port_descriptor_t *)(body + 1);
    desc->name = port_name;
    desc->pad1 = 0;
    desc->pad2 = 0;
    desc->disposition = MACH_MSG_TYPE_COPY_SEND;
    desc->type = MACH_MSG_PORT_DESCRIPTOR;
    sh_memcpy((uint8_t *)(desc + 1), dict, dict_len);

    int kr = ipc_msg(msg, MACH_SEND_MSG | MACH_RCV_MSG, hdr->msgh_size,
                     sizeof(msg), reply_port, MACH_MSG_TIMEOUT_NONE, 0,
                     NULL, 0);
    if (kr != MACH_MSG_SUCCESS) {
        if (g_ipc_trace) {
            sh_print_str("[svchost] register \"");
            sh_print_str(name);
            sh_print_str("\" ipc_msg failed kr=0x");
            sh_print_hex((uint32_t)kr);
            sh_print_str("\n");
        }
        (void)ipc_port_deallocate(ipc_task_self(), reply_port);
        return kr;
    }

    /* The reply now sits in msg: parse {"error": N}. */
    int64_t err = 0;
    int ret = 0;
    xw_envelope env;
    xw_reader rd;
    xw_value v;
    if (xw_envelope_parse(hdr, hdr->msgh_size, &env) != XW_OK ||
        xw_reader_init(&rd, env.payload, env.payload_len,
                       env.descs, env.ndescs) != XW_OK ||
        xw_dict_find(&rd, "error", &v) != XW_OK ||
        xw_as_int64(&v, &err) != XW_OK) {
        /* A reply we cannot parse is a failure: the shim always
         * answers {"error": N} to a well-formed register. */
        ret = KERN_FAILURE;
    } else if (err != 0) {
        ret = (int)err;
    }

    if (g_ipc_trace) {
        sh_print_str("[svchost] register \"");
        sh_print_str(name);
        sh_print_str("\" port=0x");
        sh_print_hex(port_name);
        sh_print_str(" -> err=");
        sh_print_uint((uint64_t)ret);
        sh_print_str("\n");
    }

    (void)ipc_port_deallocate(ipc_task_self(), reply_port);
    return ret;
}

int svchost_register(const char *name, svchost_handler_t handler,
                     void *ctx, mach_port_name_t *out_port) {
    if (!name || !name[0] || !handler) return KERN_INVALID_ARGUMENT;
    if (sh_strlen(name) >= SVCHOST_NAME_MAX) return KERN_INVALID_ARGUMENT;

    svc_lock_acquire();
    int slot = -1;
    for (int i = 0; i < SVCHOST_MAX_SERVICES; i++) {
        if (g_svchost_services[i].in_use &&
            sh_strcmp(g_svchost_services[i].name, name) == 0) {
            if (!g_svchost_services[i].published) {
                /* Another thread is mid-registration of this name
                 * (slot reserved, port not yet created/published). */
                svc_lock_release();
                return KERN_NAME_EXISTS;
            }
            /* Re-register: update the handler, keep the port and the
             * launchd entry (its 0xcc path overwrites the same slot). */
            g_svchost_services[i].handler = handler;
            g_svchost_services[i].ctx = ctx;
            mach_port_name_t pn = g_svchost_services[i].port_name;
            svc_lock_release();
            if (out_port) *out_port = pn;
            return KERN_SUCCESS;
        }
        if (slot < 0 && !g_svchost_services[i].in_use) slot = i;
    }
    if (slot < 0) {
        svc_lock_release();
        return KERN_NO_SPACE; /* table full */
    }
    /* Reserve the slot (unpublished: no client can reach the port yet,
     * because the launchd table only learns the name below). */
    svchost_entry_t *e = &g_svchost_services[slot];
    sh_memset(e, 0, sizeof(*e));
    sh_memcpy(e->name, name, sh_strlen(name));
    e->handler = handler;
    e->ctx = ctx;
    e->in_use = 1;
    e->published = 0;
    svc_lock_release();

    /* Create the service port. This is the one step svchost cannot do
     * itself: struct ipc_port lives in ipc.c. See svchost.h. */
    mach_port_name_t port_name = ipc_svchost_port_create();
    if (port_name == MACH_PORT_NULL) {
        svc_lock_acquire();
        e->in_use = 0;
        svc_lock_release();
        return KERN_RESOURCE_SHORTAGE;
    }

    /* The svchost lock is NOT held across ipc_msg (lock order:
     * ipc_lock -> svchost lock; see the file header comment). */
    int r = sh_bootstrap_register(name, port_name);
    if (r != 0) {
        (void)ipc_port_destroy(ipc_task_self(), port_name);
        svc_lock_acquire();
        e->in_use = 0;
        svc_lock_release();
        return r;
    }

    svc_lock_acquire();
    e->port = ipc_port_from_name(port_name);
    e->port_name = port_name;
    e->published = 1;
    svc_lock_release();

    if (out_port) *out_port = port_name;
    return KERN_SUCCESS;
}

mach_port_name_t svchost_lookup_port(const char *name) {
    if (!name) return MACH_PORT_NULL;
    svc_lock_acquire();
    mach_port_name_t pn = MACH_PORT_NULL;
    for (int i = 0; i < SVCHOST_MAX_SERVICES; i++) {
        if (g_svchost_services[i].in_use &&
            g_svchost_services[i].published &&
            sh_strcmp(g_svchost_services[i].name, name) == 0) {
            pn = g_svchost_services[i].port_name;
            break;
        }
    }
    svc_lock_release();
    return pn;
}

/* The example ping service. Accepts two request shapes:
 *   {"ping": <any>}                              (presence of "ping")
 *   {"type": 7, "handle": 0, "message": "ping"}  (bootstrap-style)
 * Replies {"error": 0, "pong": true}, plus an echo of a "payload"
 * string when one is present. Anything else returns
 * SVCHOST_ERR_UNKNOWN_MESSAGE (1), which the dispatcher turns into
 * {"error": 1}. */
int svchost_ping_handler(xw_reader *msg, xw_builder *reply,
                         void *ctx) {
    (void)ctx;
    xw_value v;
    int is_ping = 0;

    if (xw_dict_find(msg, "message", &v) == XW_OK) {
        const char *s = NULL;
        uint32_t slen = 0;
        /* Length includes the NUL (xw_as_string), so "ping" arrives
         * with slen 5, not 4. */
        if (xw_as_string(&v, &s, &slen) == XW_OK && s != NULL &&
            slen == 5 && sh_memeq(s, "ping", 4)) {
            is_ping = 1;
        }
    }
    if (!is_ping && xw_dict_find(msg, "ping", &v) == XW_OK) {
        is_ping = 1;
    }
    if (!is_ping) return SVCHOST_ERR_UNKNOWN_MESSAGE;

    int r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_int64(reply, "error", 0);
    if (r == XW_OK) r = xw_dict_add_bool(reply, "pong", 1);
    if (r == XW_OK && xw_dict_find(msg, "payload", &v) == XW_OK) {
        const char *s = NULL;
        uint32_t slen = 0;
        if (xw_as_string(&v, &s, &slen) == XW_OK && s != NULL) {
            /* xpcwire strings are NUL-terminated in the body; the
             * length from the reader includes the terminator rules of
             * the wire format, so pass the pointer, not the length. */
            r = xw_dict_add_string(reply, "payload", s);
        }
    }
    if (r == XW_OK) r = xw_dict_end(reply);
    if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

int svchost_register_ping(void) {
    return svchost_register(SVCHOST_PING_SERVICE, svchost_ping_handler,
                            NULL, NULL);
}

int svchost_init(void) {
    if (g_svchost_inited) return KERN_SUCCESS;
    sh_memset(g_svchost_services, 0, sizeof(g_svchost_services));
    g_svchost_inited = 1;
    return svchost_register_ping();
}

int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    if (!hdr || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        !out_reply_size) {
        return 0;
    }
    /* The smallest reply we can ever build is the 40-byte error dict,
     * so a smaller buffer cannot satisfy the never-drop-a-reply rule. */
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;

    mach_port_name_t reply_port = hdr->msgh_local_port;
    uint8_t *body = reply_buf + sizeof(mach_msg_header_t);
    uint32_t body_cap = max_reply_size - sizeof(mach_msg_header_t);

    /* One-way message (no reply port): run the handler for its side
     * effects, send nothing. */
    int one_way = (reply_port == MACH_PORT_NULL);

    /* Snapshot the handler for the destination port. The destination
     * is hdr->msgh_remote_port, the name the client sent to; the
     * dispatch holds ipc_lock, so this only takes the svchost lock
     * (order ipc_lock -> svchost lock) and never blocks on IPC. */
    svchost_handler_t handler = NULL;
    void *ctx = NULL;
    int found = 0;
    svc_lock_acquire();
    for (int i = 0; i < SVCHOST_MAX_SERVICES; i++) {
        if (g_svchost_services[i].in_use &&
            g_svchost_services[i].published &&
            g_svchost_services[i].port_name == hdr->msgh_remote_port) {
            handler = g_svchost_services[i].handler;
            ctx = g_svchost_services[i].ctx;
            found = 1;
            break;
        }
    }
    svc_lock_release();

    int64_t err_code = 0;
    uint32_t body_len = 0;

    if (!found) {
        /* Unreachable in v1 (there is no unregister), but a stale
         * lookup after a table reset would land here. Answer 124, the
         * same code launchd uses for an unknown service. */
        err_code = 124;
    } else {
        xw_envelope env;
        xw_reader rd;
        if (xw_envelope_parse(hdr, send_size, &env) != XW_OK ||
            xw_reader_init(&rd, env.payload, env.payload_len,
                           env.descs, env.ndescs) != XW_OK) {
            err_code = SVCHOST_ERR_BAD_MESSAGE;
            if (g_ipc_trace) {
                const uint8_t *pb = (const uint8_t *)hdr;
                sh_print_str("[svchost] bad payload id=0x"); sh_print_hex(hdr->msgh_id);
                sh_print_str(" bits=0x"); sh_print_hex(hdr->msgh_bits);
                sh_print_str(" size="); sh_print_hex(send_size); sh_print_str(" bytes:");
                for (uint32_t k = 24; k < send_size && k < 24 + 96; k++) {
                    const char *hx = "0123456789abcdef";
                    char c[3] = { hx[pb[k] >> 4], hx[pb[k] & 15], 0 };
                    if ((k - 24) % 16 == 0) sh_print_str("\n   ");
                    sh_print_str(c);
                }
                sh_print_str("\n");
            }
        } else {
            xw_builder b;
            if (xw_builder_init(&b, body, body_cap) != XW_OK) {
                err_code = SVCHOST_ERR_NO_MEMORY;
            } else {
                int hr = handler(&rd, &b, ctx);
                if (hr == 0) {
                    body_len = (uint32_t)xw_builder_len(&b);
                } else {
                    err_code = hr;
                }
            }
        }
    }

    if (err_code != 0) {
        /* Handler error, malformed payload, or unknown service: answer
         * with {"error": code}. This path must not fail for a reply
         * that fits; the error dict is 40 bytes and the caller was
         * checked to have room for it above. */
        if (sh_build_error_body(body, body_cap, err_code, &body_len) != 0) {
            return 0;
        }
    }

    if (one_way) {
        *out_reply_size = 0;
        return 1;
    }

    /* v1 replies never carry port descriptors: handlers must not add
     * mach_send / mach_recv / mach_send_once values. The builder
     * records such values in b.descs, but this reply path does not
     * emit them, so a port value would be written without its
     * descriptor and the client would misparse the reply. */
    sh_fill_reply_header(reply_buf, body_len, reply_port);

    if (g_ipc_trace) {
        sh_print_str("[svchost] msg id=0x");
        sh_print_hex(hdr->msgh_id);
        sh_print_str(" dest=0x");
        sh_print_hex(hdr->msgh_remote_port);
        sh_print_str(" -> reply ");
        sh_print_uint(body_len);
        sh_print_str(" bytes, error=");
        sh_print_uint((uint64_t)err_code);
        sh_print_str("\n");
    }

    *out_reply_size = sizeof(mach_msg_header_t) + body_len;
    return 1;
}
