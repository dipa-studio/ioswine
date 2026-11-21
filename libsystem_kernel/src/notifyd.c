/*
 * notifyd.c: the notifyd MIG server for ioswine.
 *
 * See notifyd.h for the API and the documented sane defaults.
 * The routine table, the handler semantics and the [src:]/[guess]
 * tags all come from the Apple open-source Libnotify drop
 * (notify_ipc.defs, libnotify.c, notifyd.c, notify_internal.h).
 * Wire strings and protocol numbers live in notifyd_names.h;
 * this file carries no wire-string literal.
 *
 * Layout of a request: Mach header (24), then for a complex
 * message the body (descriptor count, 4) and the port descriptors
 * (12 each), then the NDR record (8: int_rep 1, char_rep 0,
 * float_rep 0), then the in arguments in .defs declaration order.
 * Variable-length data is a uint32 count followed by the bytes,
 * padded to a 4-byte boundary when more arguments follow. Fixed
 * arguments are aligned to their natural alignment (uint64 to 8).
 * A reply is: header (24), optional body + descriptor, NDR (8),
 * RetCode (4), then the out arguments in .defs order, with the
 * C-struct alignment including tail padding to 8 when a uint64
 * is present.
 *
 * The ipc.c NOTIFYD branch calls notifyd_process_msg under
 * ipc_lock, so nothing here calls a lock-taking ipc_* function.
 * The only ipc_* calls are the lock-held helpers from
 * notifyd-ipc.patch and ipc_port_from_name (a pure table lookup).
 *
 * No em dashes are used in this file.
 */
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "launchd.h"
#include "xpcwire.h"
#include "bsxpc.h"
#include "daemon_names.h"
#include "notifyd.h"
#include "linux.h"

/* AArch64 Linux syscall numbers used here. The target linux.h
 * defines SYS_kill/SYS_close/SYS_fcntl, but the host shim only
 * carries getpid/write/futex, so the numbers are repeated here
 * under nd_ names to keep both builds working. They are stable
 * ABI constants (aarch64). On the host the shim returns -1 for
 * these numbers; the call sites ignore the result (kill/close)
 * or guard on it (fcntl). */
#define ND_SYS_FCNTL 25
#define ND_SYS_CLOSE 57
#define ND_SYS_KILL 129

/* ------------------------------------------------------------------ */
/* Small freestanding helpers (no libc).                              */
/* ------------------------------------------------------------------ */

static uint32_t nd_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != 0) n++;
    return n;
}

static int nd_strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int nd_strncmp(const char *a, const char *b, uint32_t n) {
    uint32_t i = 0;
    while (i < n && a[i] != 0 && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

static void nd_memcpy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i = 0;
    while (i < n) { dst[i] = src[i]; i++; }
}

static void nd_memset(uint8_t *dst, uint8_t v, uint32_t n) {
    uint32_t i = 0;
    while (i < n) { dst[i] = v; i++; }
}

static uint32_t nd_htonl(uint32_t v) {
    return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24);
}

static uint32_t nd_roundup4(uint32_t v) { return (v + 3u) & ~3u; }
static uint32_t nd_roundup8(uint32_t v) { return (v + 7u) & ~7u; }

/* ------------------------------------------------------------------ */
/* State.                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[NOTIFYD_NAME_MAX + 1];
    uint64_t name_id;
    uint64_t state;
    uint64_t state_time;
    uint32_t val;
    uint32_t postcount;
    uint32_t uid;
    uint32_t gid;
    uint32_t access;
    uint32_t refcount;
    int in_use;
    int pinned;
    int controlled;
    char monitor_path[NOTIFYD_PATH_MAX + 1];
    int has_monitor;
} nd_name;

typedef struct {
    uint64_t cid;
    uint32_t pid;
    int token;
    int name_idx;
    uint8_t type;
    uint8_t suspend_count;
    uint8_t suspended;
    uint8_t pending;
    uint8_t timeout;
    uint32_t lastval;
    int sig;
    int fd;
    mach_port_name_t port;
    uint64_t event_token;
    char path[NOTIFYD_PATH_MAX + 1];
    uint32_t path_len;
    int path_flags;
    int has_path;
    int in_use;
} nd_client;

typedef struct {
    uint32_t pid;
    int suspended;
    mach_port_name_t common_port;
    int in_use;
} nd_proc;

typedef struct {
    mach_port_name_t send_name;
    mach_port_name_t recv_name;
    int is_common;
    int in_use;
} nd_gen_port;

typedef struct {
    mach_port_name_t port;
    int fd;
    int in_use;
} nd_fileport;

static nd_name g_names[NOTIFYD_MAX_NAMES];
static nd_client g_clients[NOTIFYD_MAX_CLIENTS];
static nd_proc g_procs[NOTIFYD_MAX_PROCS];
static nd_gen_port g_gen_ports[NOTIFYD_MAX_GEN_PORTS];
static nd_fileport g_fileports[NOTIFYD_MAX_FILEPORTS];
static uint64_t g_next_name_id;
static uint64_t g_state_clock;
static mach_port_name_t g_notifyd_port;
static uint32_t g_server_pid;
static int g_notifyd_inited;

/* ------------------------------------------------------------------ */
/* Name table.                                                        */
/* ------------------------------------------------------------------ */

static int nd_name_find(const char *name) {
    int i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        if (g_names[i].in_use && nd_strcmp(g_names[i].name, name) == 0)
            return i;
        i++;
    }
    return -1;
}

static int nd_name_find_by_id(uint64_t id) {
    int i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        if (g_names[i].in_use && g_names[i].name_id == id) return i;
        i++;
    }
    return -1;
}

/* Create a name record. val starts at 1 [src: notifyd.c, the
 * daemon creates new names with val 1]. refcount starts at 0;
 * the caller adds a reference for the client or pins it. */
static int nd_name_new(const char *name, uint32_t uid, uint32_t gid,
                       uint32_t access, int pinned, int controlled) {
    int i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        if (!g_names[i].in_use) {
            uint32_t len = nd_strlen(name);
            uint32_t j = 0;
            while (j < len && j < NOTIFYD_NAME_MAX) {
                g_names[i].name[j] = name[j];
                j++;
            }
            g_names[i].name[j] = 0;
            g_names[i].name_id = g_next_name_id;
            g_next_name_id++;
            g_names[i].state = 0;
            g_names[i].state_time = 0;
            g_names[i].val = 1;
            g_names[i].postcount = 0;
            g_names[i].uid = uid;
            g_names[i].gid = gid;
            g_names[i].access = access;
            g_names[i].refcount = 0;
            g_names[i].in_use = 1;
            g_names[i].pinned = pinned;
            g_names[i].controlled = controlled;
            g_names[i].monitor_path[0] = 0;
            g_names[i].has_monitor = 0;
            return i;
        }
        i++;
    }
    return -1;
}

static void nd_name_add_ref(int ni) {
    g_names[ni].refcount++;
}

static void nd_name_release(int ni) {
    if (g_names[ni].refcount > 0) g_names[ni].refcount--;
    if (g_names[ni].refcount == 0 && !g_names[ni].pinned) {
        g_names[ni].in_use = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Client table.                                                      */
/* ------------------------------------------------------------------ */

static int nd_client_find(uint32_t pid, int token) {
    int i = 0;
    while (i < NOTIFYD_MAX_CLIENTS) {
        if (g_clients[i].in_use && g_clients[i].pid == pid &&
            g_clients[i].token == token)
            return i;
        i++;
    }
    return -1;
}

static void nd_client_free(int ci) {
    int ni = g_clients[ci].name_idx;
    if (ni >= 0 && ni < NOTIFYD_MAX_NAMES && g_names[ni].in_use)
        nd_name_release(ni);
    g_clients[ci].in_use = 0;
}

/* Cancel (and free) the client with this (pid, token), if any.
 * The real server's preflight step does exactly this before
 * every registration, so a client that re-registers after an
 * exec does not collide with its own stale entry [src:
 * notify_proc.c, server_preflight]. Returns 1 when one was
 * cancelled. */
static int nd_client_cancel(uint32_t pid, int token) {
    int ci = nd_client_find(pid, token);
    if (ci < 0) return 0;
    nd_client_free(ci);
    return 1;
}

static int nd_client_new(uint32_t pid, int token, int name_idx,
                         int type) {
    int i = 0;
    while (i < NOTIFYD_MAX_CLIENTS) {
        if (!g_clients[i].in_use) {
            g_clients[i].cid = ((uint64_t)pid << 32) | (uint32_t)token;
            g_clients[i].pid = pid;
            g_clients[i].token = token;
            g_clients[i].name_idx = name_idx;
            g_clients[i].type = (uint8_t)type;
            g_clients[i].suspend_count = 0;
            g_clients[i].suspended = 0;
            g_clients[i].pending = 0;
            g_clients[i].timeout = 0;
            /* lastval starts at the name's current val, so a check
             * immediately after registration reports unchanged.
             * Sane default: the client has not missed a
             * notification. Flag for the first device trace. */
            g_clients[i].lastval = g_names[name_idx].val;
            g_clients[i].sig = 0;
            g_clients[i].fd = -1;
            g_clients[i].port = MACH_PORT_NULL;
            g_clients[i].event_token = 0;
            g_clients[i].path[0] = 0;
            g_clients[i].path_len = 0;
            g_clients[i].path_flags = 0;
            g_clients[i].has_path = 0;
            g_clients[i].in_use = 1;
            nd_name_add_ref(name_idx);
            return i;
        }
        i++;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Proc table.                                                        */
/* ------------------------------------------------------------------ */

static int nd_proc_find(uint32_t pid) {
    int i = 0;
    while (i < NOTIFYD_MAX_PROCS) {
        if (g_procs[i].in_use && g_procs[i].pid == pid) return i;
        i++;
    }
    return -1;
}

static int nd_proc_find_or_new(uint32_t pid) {
    int i = nd_proc_find(pid);
    if (i >= 0) return i;
    i = 0;
    while (i < NOTIFYD_MAX_PROCS) {
        if (!g_procs[i].in_use) {
            g_procs[i].pid = pid;
            g_procs[i].suspended = 0;
            g_procs[i].common_port = MACH_PORT_NULL;
            g_procs[i].in_use = 1;
            return i;
        }
        i++;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Generated-port and fileport tables.                                */
/* ------------------------------------------------------------------ */

static int nd_gen_port_add(mach_port_name_t send_name,
                           mach_port_name_t recv_name, int is_common) {
    int i = 0;
    while (i < NOTIFYD_MAX_GEN_PORTS) {
        if (!g_gen_ports[i].in_use) {
            g_gen_ports[i].send_name = send_name;
            g_gen_ports[i].recv_name = recv_name;
            g_gen_ports[i].is_common = is_common;
            g_gen_ports[i].in_use = 1;
            return i;
        }
        i++;
    }
    return -1;
}

/* The send name stays valid after the receive name is moved to
 * the client (the branch frees the receive entry, not the send
 * entry), so the pointer is resolved through the send name. */
static int nd_gen_port_find_by_ptr(struct ipc_port *p) {
    int i = 0;
    while (i < NOTIFYD_MAX_GEN_PORTS) {
        if (g_gen_ports[i].in_use) {
            struct ipc_port *q =
                ipc_port_from_name(g_gen_ports[i].send_name);
            if (q != NULL && q == p) return i;
        }
        i++;
    }
    return -1;
}

static void nd_gen_port_remove(int gi) {
    g_gen_ports[gi].in_use = 0;
}

static int nd_fileport_find_by_ptr(struct ipc_port *p) {
    int i = 0;
    while (i < NOTIFYD_MAX_FILEPORTS) {
        if (g_fileports[i].in_use) {
            struct ipc_port *q = ipc_port_from_name(g_fileports[i].port);
            if (q != NULL && q == p) return g_fileports[i].fd;
        }
        i++;
    }
    return -1;
}

int notifyd_fileport_register(mach_port_name_t port_name, int fd) {
    int i = 0;
    while (i < NOTIFYD_MAX_FILEPORTS) {
        if (!g_fileports[i].in_use) {
            g_fileports[i].port = port_name;
            g_fileports[i].fd = fd;
            g_fileports[i].in_use = 1;
            return 1;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Access control.                                                    */
/* ------------------------------------------------------------------ */

/* The fake has no entitlement source (G7), so no caller ever
 * holds the root entitlement and claim_root_access never
 * promotes a uid. */
static int nd_has_root_entitlement(void) {
    return 0;
}

/* Mirrors _internal_check_access in libnotify.c [src]. Returns
 * NOTIFY_STATUS_OK or NOTIFY_STATUS_NOT_AUTHORIZED. */
static int nd_check_access(uint32_t uid, uint32_t gid, const char *name,
                           int req) {
    uint32_t prefix_len;
    int best;
    uint32_t best_len;
    int i;
    if (uid == 0) return NOTIFY_STATUS_OK;
    prefix_len = nd_strlen(NOTIFYD_USER_UID_PREFIX);
    if (nd_strncmp(name, NOTIFYD_USER_UID_PREFIX, prefix_len) == 0) {
        const char *p = name + prefix_len;
        uint32_t nuid = 0;
        int ndig = 0;
        while (*p >= '0' && *p <= '9') {
            nuid = nuid * 10u + (uint32_t)(*p - '0');
            p++;
            ndig++;
        }
        if (ndig == 0) return NOTIFY_STATUS_NOT_AUTHORIZED;
        if (nuid == uid) return NOTIFY_STATUS_OK;
        return NOTIFY_STATUS_NOT_AUTHORIZED;
    }
    best = -1;
    best_len = 0;
    i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        if (g_names[i].in_use && g_names[i].controlled) {
            uint32_t plen = nd_strlen(g_names[i].name);
            if (plen > best_len && nd_strlen(name) >= plen &&
                nd_strncmp(name, g_names[i].name, plen) == 0) {
                best = i;
                best_len = plen;
            }
        }
        i++;
    }
    if (best >= 0) {
        uint32_t access = g_names[best].access;
        if (g_names[best].uid == uid) {
            if (access & (uint32_t)(req << NOTIFYD_ACCESS_USER_SHIFT))
                return NOTIFY_STATUS_OK;
        } else if (g_names[best].gid == gid) {
            if (access & (uint32_t)(req << NOTIFYD_ACCESS_GROUP_SHIFT))
                return NOTIFY_STATUS_OK;
        } else {
            if (access & (uint32_t)(req << NOTIFYD_ACCESS_OTHER_SHIFT))
                return NOTIFY_STATUS_OK;
        }
        return NOTIFY_STATUS_NOT_AUTHORIZED;
    }
    return NOTIFY_STATUS_OK;
}

/* A restrict-post name may only be posted by a caller holding
 * the matching entitlement. The fake has no entitlement source
 * (G7), so these posts are always denied [src:
 * notify_internal.h, check_access_to_post_restricted_name]. */
static int nd_check_post_restricted(const char *name) {
    uint32_t plen = nd_strlen(NOTIFYD_RESTRICT_PREFIX);
    if (nd_strncmp(name, NOTIFYD_RESTRICT_PREFIX, plen) == 0)
        return NOTIFY_STATUS_NOT_AUTHORIZED;
    return NOTIFY_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Post fanout.                                                       */
/* ------------------------------------------------------------------ */

static void nd_post_to_client(int ci) {
    nd_client *c = &g_clients[ci];
    int pi;
    if (c->suspended) {
        c->pending = 1;
        return;
    }
    pi = nd_proc_find(c->pid);
    if (pi >= 0 && g_procs[pi].suspended) {
        c->suspend_count++;
        c->suspended = 1;
        c->pending = 1;
        return;
    }
    switch (c->type) {
    case NOTIFY_TYPE_MEMORY:
    case NOTIFY_TYPE_PLAIN:
    case NOTIFY_TYPE_NONE:
        break;
    case NOTIFY_TYPE_SIGNAL:
        (void)sys3(ND_SYS_KILL, (long)c->pid, (long)c->sig, 0);
        break;
    case NOTIFY_TYPE_FILE: {
        uint32_t tok = nd_htonl((uint32_t)c->token);
        long n = sys3(SYS_write, (long)c->fd, (long)(uintptr_t)&tok, 4);
        if (n != 4) {
            (void)sys1(ND_SYS_CLOSE, (long)c->fd);
            c->fd = -1;
        }
        break;
    }
    case NOTIFY_TYPE_PORT:
        if (!ipc_notifyd_deliver_locked(c->port, (uint32_t)c->token)) {
            c->suspended = 1;
            c->pending = 1;
        }
        break;
    case NOTIFY_TYPE_COMMON_PORT: {
        int pj = nd_proc_find(c->pid);
        if (pj >= 0 && g_procs[pj].common_port != MACH_PORT_NULL) {
            if (!ipc_notifyd_deliver_locked(g_procs[pj].common_port,
                                            (uint32_t)c->token)) {
                c->suspended = 1;
                c->pending = 1;
            }
        }
        break;
    }
    case NOTIFY_TYPE_XPC_EVENT:
        /* No launchd event publisher is wired (G6). No MIG
         * routine can create an XPC_EVENT client, so this case
         * is unreachable over MIG. */
        break;
    default:
        break;
    }
}

static void nd_post_name(int ni) {
    int i;
    g_names[ni].val++;
    g_names[ni].postcount++;
    i = 0;
    while (i < NOTIFYD_MAX_CLIENTS) {
        if (g_clients[i].in_use && g_clients[i].name_idx == ni)
            nd_post_to_client(i);
        i++;
    }
}

/* Deliver one pending notification to a client whose suspension
 * just cleared. Used by resume. */
static void nd_deliver_pending(int ci) {
    g_clients[ci].pending = 0;
    nd_post_to_client(ci);
}

/* ------------------------------------------------------------------ */
/* MIG cursor (bounds-checked sequential reader).                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *base;
    uint32_t msgh_size;
    uint32_t send_size;
    uint32_t off;
    int err;
} nd_cursor;

static int nd_cur_need(nd_cursor *c, uint32_t n) {
    if (c->err) return 0;
    if (c->off > c->msgh_size || n > c->msgh_size - c->off) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    return 1;
}

static uint32_t nd_cur_u32(nd_cursor *c) {
    uint32_t v;
    c->off = nd_roundup4(c->off);
    if (!nd_cur_need(c, 4)) return 0;
    v = (uint32_t)c->base[c->off] |
        ((uint32_t)c->base[c->off + 1] << 8) |
        ((uint32_t)c->base[c->off + 2] << 16) |
        ((uint32_t)c->base[c->off + 3] << 24);
    c->off += 4;
    return v;
}

static uint64_t nd_cur_u64(nd_cursor *c) {
    uint32_t lo;
    uint32_t hi;
    c->off = nd_roundup8(c->off);
    if (!nd_cur_need(c, 8)) return 0;
    lo = (uint32_t)c->base[c->off] |
         ((uint32_t)c->base[c->off + 1] << 8) |
         ((uint32_t)c->base[c->off + 2] << 16) |
         ((uint32_t)c->base[c->off + 3] << 24);
    hi = (uint32_t)c->base[c->off + 4] |
         ((uint32_t)c->base[c->off + 5] << 8) |
         ((uint32_t)c->base[c->off + 6] << 16) |
         ((uint32_t)c->base[c->off + 7] << 24);
    c->off += 8;
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Read a MIG c_string: uint32 count, then the bytes, padded to a
 * 4-byte boundary. The count includes the NUL. The string must be
 * NUL terminated within the count with no embedded NUL, and the
 * length must fit buf (buf_cap includes the NUL). */
static int nd_cur_cstring(nd_cursor *c, char *buf, uint32_t buf_cap,
                          uint32_t max_cnt, uint32_t *len_out) {
    uint32_t cnt;
    uint32_t len;
    uint32_t i;
    const uint8_t *s;
    c->off = nd_roundup4(c->off);
    if (!nd_cur_need(c, 4)) return 0;
    cnt = nd_cur_u32(c);
    if (cnt < 1 || cnt > max_cnt) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    if (!nd_cur_need(c, cnt)) return 0;
    s = c->base + c->off;
    if (s[cnt - 1] != 0) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    i = 0;
    while (i < cnt - 1) {
        if (s[i] == 0) {
            c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
            return 0;
        }
        i++;
    }
    len = cnt - 1;
    if (len >= buf_cap) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    i = 0;
    while (i < len) {
        buf[i] = (char)s[i];
        i++;
    }
    buf[len] = 0;
    if (len_out) *len_out = len;
    c->off += nd_roundup4(cnt);
    return 1;
}

/* Read a MIG byte array: uint32 count, then the bytes, padded to
 * a 4-byte boundary. The count may be zero. */
static int nd_cur_bytes(nd_cursor *c, uint8_t *buf, uint32_t buf_cap,
                        uint32_t max_cnt, uint32_t *len_out) {
    uint32_t cnt;
    uint32_t i;
    c->off = nd_roundup4(c->off);
    if (!nd_cur_need(c, 4)) return 0;
    cnt = nd_cur_u32(c);
    if (cnt > max_cnt || cnt > buf_cap) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    if (!nd_cur_need(c, cnt)) return 0;
    i = 0;
    while (i < cnt) {
        buf[i] = c->base[c->off + i];
        i++;
    }
    if (len_out) *len_out = cnt;
    c->off += nd_roundup4(cnt);
    return 1;
}

/* Read a port descriptor (12 bytes: name, pad, disposition at 10,
 * type at 11). Only the type is validated (MACH_MSG_PORT_DESCRIPTOR);
 * the disposition is the kernel's business. */
static int nd_cur_desc(nd_cursor *c, mach_port_name_t *name_out,
                       uint32_t *disp_out) {
    const uint8_t *d;
    if (!nd_cur_need(c, 12)) return 0;
    d = c->base + c->off;
    if (d[11] != MACH_MSG_PORT_DESCRIPTOR) {
        c->err = NOTIFYD_MIG_TYPE_ERROR;
        return 0;
    }
    if (name_out) {
        *name_out = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                    ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    }
    if (disp_out) *disp_out = d[10];
    c->off += 12;
    return 1;
}

static int nd_cur_ndr(nd_cursor *c) {
    uint8_t b[8];
    uint32_t i;
    if (!nd_cur_need(c, 8)) return 0;
    i = 0;
    while (i < 8) {
        b[i] = c->base[c->off + i];
        i++;
    }
    c->off += 8;
    if (b[4] != NOTIFYD_NDR_INT_REP || b[5] != NOTIFYD_NDR_CHAR_REP ||
        b[6] != NOTIFYD_NDR_FLOAT_REP) {
        c->err = NOTIFYD_MIG_BAD_ARGUMENTS;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reply builder.                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t off;
    int err;
} nd_builder;

static void nb_reserve(nd_builder *b, uint32_t n) {
    if (b->err) return;
    if (b->off > b->cap || n > b->cap - b->off) b->err = 1;
}

static void nb_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void nb_u32(nd_builder *b, uint32_t v) {
    if (b->err) return;
    nb_reserve(b, 4);
    if (b->err) return;
    nb_put_u32(b->buf + b->off, v);
    b->off += 4;
}

static void nb_i32(nd_builder *b, int32_t v) {
    nb_u32(b, (uint32_t)v);
}

static void nb_u64(nd_builder *b, uint64_t v) {
    uint32_t aligned;
    if (b->err) return;
    aligned = nd_roundup8(b->off);
    if (aligned > b->off) {
        nb_reserve(b, aligned - b->off);
        if (b->err) return;
        nd_memset(b->buf + b->off, 0, aligned - b->off);
        b->off = aligned;
    }
    nb_u32(b, (uint32_t)(v & 0xffffffffULL));
    nb_u32(b, (uint32_t)(v >> 32));
}

static void nb_ndr(nd_builder *b) {
    if (b->err) return;
    nb_reserve(b, 8);
    if (b->err) return;
    b->buf[b->off] = 0;
    b->buf[b->off + 1] = 0;
    b->buf[b->off + 2] = 0;
    b->buf[b->off + 3] = 0;
    b->buf[b->off + 4] = (uint8_t)NOTIFYD_NDR_INT_REP;
    b->buf[b->off + 5] = (uint8_t)NOTIFYD_NDR_CHAR_REP;
    b->buf[b->off + 6] = (uint8_t)NOTIFYD_NDR_FLOAT_REP;
    b->buf[b->off + 7] = 0;
    b->off += 8;
}

/* A port descriptor: name (4), six zero pad bytes, disposition
 * at 10, type at 11. */
static void nb_desc(nd_builder *b, mach_port_name_t name, uint32_t disp) {
    uint32_t i;
    if (b->err) return;
    nb_reserve(b, 12);
    if (b->err) return;
    nb_put_u32(b->buf + b->off, name);
    b->off += 4;
    i = 0;
    while (i < 6) {
        b->buf[b->off + i] = 0;
        i++;
    }
    b->off += 6;
    b->buf[b->off] = (uint8_t)disp;
    b->buf[b->off + 1] = (uint8_t)MACH_MSG_PORT_DESCRIPTOR;
    b->off += 2;
}

/* Patch the reply header (written byte by byte, so the buffer
 * needs no alignment) and return the final size. has_u64 tail
 * pads the size to 8 with zero bytes. */
static int nd_reply_finish(nd_builder *b, const mach_msg_header_t *req,
                           int is_complex, int has_u64,
                           uint32_t *out_size) {
    uint32_t size;
    uint32_t bits;
    if (b->err) return 0;
    size = b->off;
    if (has_u64) {
        uint32_t padded = nd_roundup8(size);
        if (padded > size) {
            nb_reserve(b, padded - size);
            if (b->err) return 0;
            nd_memset(b->buf + b->off, 0, padded - size);
            b->off = padded;
            size = padded;
        }
    }
    bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
    if (is_complex) bits |= MACH_MSGH_BITS_COMPLEX;
    nb_put_u32(b->buf + 0, bits);
    nb_put_u32(b->buf + 4, size);
    nb_put_u32(b->buf + 8, req->msgh_local_port);
    nb_put_u32(b->buf + 12, MACH_PORT_NULL);
    nb_put_u32(b->buf + 16, MACH_PORT_NULL);
    nb_put_u32(b->buf + 20, req->msgh_id + NOTIFYD_REPLY_ID_OFFSET);
    *out_size = size;
    return 1;
}

/* Build a 36-byte MIG error reply (mig_reply_error_t): header,
 * NDR, RetCode. Returns 1 on success. */
static int nd_build_error_reply(uint8_t *reply_buf, uint32_t reply_cap,
                                const mach_msg_header_t *req, int mig_err,
                                uint32_t *out_size) {
    nd_builder b;
    if (reply_cap < 36) return 0;
    b.buf = reply_buf;
    b.cap = reply_cap;
    b.off = 0;
    b.err = 0;
    nb_u32(&b, 0);
    nb_u32(&b, 0);
    nb_u32(&b, 0);
    nb_u32(&b, 0);
    nb_u32(&b, 0);
    nb_u32(&b, 0);
    nb_ndr(&b);
    nb_i32(&b, mig_err);
    if (b.err) return 0;
    return nd_reply_finish(&b, req, 0, 0, out_size);
}

/* ------------------------------------------------------------------ */
/* Audit trailer.                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
} nd_audit;

/* Read the audit trailer the ipc.c branch appended at
 * roundup4(msgh_size). The trailer is 52 bytes: w[0] type, w[1]
 * size, w[2] seqno, w[3..4] security token, w[5..12] audit token
 * (auid, euid, egid, uid, gid, pid, asid, flags). Returns 0 when
 * the trailer is missing or short. */
static int nd_parse_trailer(const mach_msg_header_t *hdr, uint32_t send_size,
                            nd_audit *out) {
    uint32_t toff = nd_roundup4(hdr->msgh_size);
    const uint8_t *t;
    uint32_t tsize;
    uint32_t w[13];
    uint32_t i;
    if (send_size < toff + 52) return 0;
    t = (const uint8_t *)hdr + toff;
    tsize = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
            ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
    if (tsize < 52) return 0;
    i = 0;
    while (i < 13) {
        w[i] = (uint32_t)t[i * 4] | ((uint32_t)t[i * 4 + 1] << 8) |
               ((uint32_t)t[i * 4 + 2] << 16) |
               ((uint32_t)t[i * 4 + 3] << 24);
        i++;
    }
    out->euid = w[6];
    out->uid = w[8];
    out->gid = w[9];
    out->pid = w[10];
    return 1;
}

/* ------------------------------------------------------------------ */
/* Trace.                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id;
    int is_simple;
    uint32_t audit_pid;
    uint32_t audit_uid;
    uint32_t audit_gid;
    uint32_t audit_euid;
    char name[NOTIFYD_NAME_MAX + 1];
    int has_name;
    char path[NOTIFYD_PATH_MAX + 1];
    uint32_t path_len;
    int has_path;
    int token;
    int has_token;
    uint64_t name_id;
    int has_name_id;
    uint64_t state;
    int sig;
    int claim_root_access;
    uint32_t reg_type;
    int prev_slot;
    uint64_t prev_state;
    uint64_t prev_time;
    int path_flags;
    mach_port_name_t port;
    uint32_t port_disp;
    int has_port;
    int pid_arg;
    uint64_t result;
    int has_result;
} nd_request;

/* Log the decoded request as a small XPC dict through
 * bsxpc_log_dict when tracing is enabled. A MIG message is not
 * an XPC dict, so the fields are re-encoded here; the dict
 * carries the verbatim decoded values, not the raw bytes. */
static void nd_trace_request(const nd_request *rq, uint32_t send_size,
                             int status) {
    uint8_t buf[512];
    xw_builder b;
    xw_reader rd;
    int r;
    if (!bsxpc_trace_enabled()) return;
    r = xw_builder_init(&b, buf, sizeof(buf));
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, NOTIFYD_TRACE_KEY_ID, rq->id);
    if (r == XW_OK && rq->has_name)
        r = xw_dict_add_string(&b, NOTIFYD_TRACE_KEY_NAME, rq->name);
    if (r == XW_OK && rq->has_token)
        r = xw_dict_add_int64(&b, NOTIFYD_TRACE_KEY_TOKEN, rq->token);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, NOTIFYD_TRACE_KEY_SIZE, send_size);
    if (r == XW_OK)
        r = xw_dict_add_int64(&b, NOTIFYD_TRACE_KEY_STATUS, status);
    if (r == XW_OK && rq->has_result)
        r = xw_dict_add_uint64(&b, NOTIFYD_TRACE_KEY_RESULT, rq->result);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return;
    if (xw_reader_init(&rd, buf, xw_builder_len(&b), NULL, 0) != XW_OK)
        return;
    (void)bsxpc_log_dict(&rd, NOTIFYD_TRACE_PREFIX);
}

/* ------------------------------------------------------------------ */
/* Registration core.                                                 */
/* ------------------------------------------------------------------ */

/* Find the name, creating it (owned by the caller) when missing.
 * Returns the index, or -1 when the table is full. */
static int nd_name_find_or_new(const char *name, uint32_t uid, uint32_t gid) {
    int ni = nd_name_find(name);
    if (ni >= 0) return ni;
    return nd_name_new(name, uid, gid, NOTIFYD_ACCESS_DEFAULT, 0, 0);
}

/* Shared registration core. The name must already exist (index
 * ni). Runs the preflight cancel, the READ access check and the
 * duplicate check, then allocates the client. Returns a
 * NOTIFY_STATUS_* code; on success *client_out gets the client
 * index. */
static int nd_register(int ni, uint32_t pid, int token, int type,
                       uint32_t uid, uint32_t gid,
                       int sig, int fd, mach_port_name_t port,
                       int *client_out) {
    int ci;
    nd_client_cancel(pid, token);
    if (nd_check_access(uid, gid, g_names[ni].name,
                        NOTIFYD_ACCESS_READ) != NOTIFY_STATUS_OK)
        return NOTIFY_STATUS_NOT_AUTHORIZED;
    if (nd_client_find(pid, token) >= 0)
        return NOTIFY_STATUS_NEW_CLIENT_FAILED;
    ci = nd_client_new(pid, token, ni, type);
    if (ci < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
    g_clients[ci].sig = sig;
    g_clients[ci].fd = fd;
    g_clients[ci].port = port;
    if (client_out) *client_out = ci;
    return NOTIFY_STATUS_OK;
}

/* The effective uid for a call that carries claim_root_access:
 * root when the caller asked for it and holds the root
 * entitlement, otherwise the audit uid. The fake never grants
 * the entitlement (G7), so this is always the audit uid. */
static uint32_t nd_effective_uid(const nd_request *rq) {
    if (rq->claim_root_access && nd_has_root_entitlement()) return 0;
    return rq->audit_uid;
}

/* Linux fcntl commands, for the O_NONBLOCK set on a registered
 * file descriptor. Defined locally; linux.h carries only the
 * syscall numbers. */
#define ND_F_GETFL 3
#define ND_F_SETFL 4
#define ND_O_NONBLOCK 2048

/* Find a generated-port record by its send name. */
static int nd_gen_port_find_by_send(mach_port_name_t send_name) {
    int i = 0;
    while (i < NOTIFYD_MAX_GEN_PORTS) {
        if (g_gen_ports[i].in_use &&
            g_gen_ports[i].send_name == send_name)
            return i;
        i++;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Request parsing.                                                   */
/* ------------------------------------------------------------------ */

static int nd_routine_is_simple(uint32_t id) {
    switch (id) {
    case NOTIFYD_SUSPEND_PID:
    case NOTIFYD_RESUME_PID:
    case NOTIFYD_POST_3:
    case NOTIFYD_POST_4:
    case NOTIFYD_REGISTER_PLAIN_2:
    case NOTIFYD_REGISTER_SIGNAL_2:
    case NOTIFYD_REGISTER_FILE_DESCRIPTOR_2:
    case NOTIFYD_REGISTER_MACH_PORT_2:
    case NOTIFYD_CANCEL_2:
    case NOTIFYD_SET_STATE_2:
    case NOTIFYD_MONITOR_FILE_2:
    case NOTIFYD_DUMP:
    case NOTIFYD_REGISTER_COMMON_PORT:
    case NOTIFYD_FILTERED_POST:
    case NOTIFYD_FILTERED_SET_STATE_2:
        return 1;
    default:
        return 0;
    }
}

static int nd_routine_n_descs(uint32_t id) {
    switch (id) {
    case NOTIFYD_REGISTER_FILE_DESCRIPTOR_2:
    case NOTIFYD_REGISTER_MACH_PORT_2:
    case NOTIFYD_REGENERATE:
    case NOTIFYD_DUMP:
    case NOTIFYD_FILTERED_REGENERATE:
        return 1;
    default:
        return 0;
    }
}

/* Parse a request into rq. Returns 0, or a MIG error code
 * (NOTIFYD_MIG_*). The id and the audit fields are filled before
 * any error return, so the trace can still name the request. */
static int nd_parse_request(const mach_msg_header_t *hdr, uint32_t send_size,
                            nd_request *rq) {
    nd_cursor c;
    nd_audit au;
    uint32_t id;
    int ndescs;
    uint32_t i;
    if (send_size < sizeof(mach_msg_header_t))
        return NOTIFYD_MIG_BAD_ARGUMENTS;
    if (hdr->msgh_size < sizeof(mach_msg_header_t) ||
        hdr->msgh_size > send_size)
        return NOTIFYD_MIG_BAD_ARGUMENTS;
    id = hdr->msgh_id;
    rq->id = id;
    if (id < NOTIFYD_ROUTINE_FIRST || id > NOTIFYD_ROUTINE_LAST)
        return NOTIFYD_MIG_BAD_ID;
    if (id == 1000 || id == 1001)
        return NOTIFYD_MIG_BAD_ID;
    rq->is_simple = nd_routine_is_simple(id);
    if (!nd_parse_trailer(hdr, send_size, &au))
        return NOTIFYD_MIG_BAD_ARGUMENTS;
    rq->audit_pid = au.pid;
    rq->audit_uid = au.uid;
    rq->audit_gid = au.gid;
    rq->audit_euid = au.euid;
    c.base = (const uint8_t *)hdr;
    c.msgh_size = hdr->msgh_size;
    c.send_size = send_size;
    c.off = sizeof(mach_msg_header_t);
    c.err = 0;
    ndescs = nd_routine_n_descs(id);
    if (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        uint32_t cnt;
        if (!nd_cur_need(&c, 4)) return c.err;
        cnt = nd_cur_u32(&c);
        if ((int)cnt != ndescs) {
            c.err = NOTIFYD_MIG_TYPE_ERROR;
            return c.err;
        }
        i = 0;
        while (i < cnt) {
            if (!nd_cur_desc(&c, &rq->port, &rq->port_disp)) return c.err;
            rq->has_port = 1;
            i++;
        }
    } else if (ndescs != 0) {
        return NOTIFYD_MIG_TYPE_ERROR;
    }
    if (!nd_cur_ndr(&c)) return c.err;
    switch (id) {
    case NOTIFYD_CHECK:
    case NOTIFYD_GET_STATE:
    case NOTIFYD_SUSPEND:
    case NOTIFYD_RESUME:
    case NOTIFYD_GET_STATE_3:
    case NOTIFYD_CANCEL_2:
        rq->token = (int)nd_cur_u32(&c);
        rq->has_token = 1;
        break;
    case NOTIFYD_SUSPEND_PID:
    case NOTIFYD_RESUME_PID:
        rq->pid_arg = (int)nd_cur_u32(&c);
        break;
    case NOTIFYD_POST_2:
    case NOTIFYD_POST_4:
    case NOTIFYD_FILTERED_POST:
        if (!nd_cur_cstring(&c, rq->name, sizeof(rq->name),
                            NOTIFYD_NAME_MAX, NULL))
            return c.err;
        rq->has_name = 1;
        rq->claim_root_access = (int)nd_cur_u32(&c);
        break;
    case NOTIFYD_POST_3:
        rq->name_id = nd_cur_u64(&c);
        rq->has_name_id = 1;
        rq->claim_root_access = (int)nd_cur_u32(&c);
        break;
    case NOTIFYD_REGISTER_PLAIN_2:
    case NOTIFYD_REGISTER_CHECK_2:
    case NOTIFYD_REGISTER_SIGNAL_2:
    case NOTIFYD_REGISTER_FILE_DESCRIPTOR_2:
    case NOTIFYD_REGISTER_MACH_PORT_2:
    case NOTIFYD_REGISTER_COMMON_PORT:
    case NOTIFYD_REGISTER_MACH_PORT_3:
        if (!nd_cur_cstring(&c, rq->name, sizeof(rq->name),
                            NOTIFYD_NAME_MAX, NULL))
            return c.err;
        rq->has_name = 1;
        rq->token = (int)nd_cur_u32(&c);
        rq->has_token = 1;
        if (id == NOTIFYD_REGISTER_SIGNAL_2)
            rq->sig = (int)nd_cur_u32(&c);
        break;
    case NOTIFYD_GET_STATE_2:
        rq->name_id = nd_cur_u64(&c);
        rq->has_name_id = 1;
        break;
    case NOTIFYD_SET_STATE_2:
    case NOTIFYD_FILTERED_SET_STATE_2:
        rq->name_id = nd_cur_u64(&c);
        rq->has_name_id = 1;
        rq->state = nd_cur_u64(&c);
        rq->claim_root_access = (int)nd_cur_u32(&c);
        break;
    case NOTIFYD_SET_STATE_3:
    case NOTIFYD_FILTERED_SET_STATE_3:
        rq->token = (int)nd_cur_u32(&c);
        rq->has_token = 1;
        rq->state = nd_cur_u64(&c);
        break;
    case NOTIFYD_MONITOR_FILE_2: {
        uint8_t *path_bytes = (uint8_t *)rq->path;
        rq->token = (int)nd_cur_u32(&c);
        rq->has_token = 1;
        if (!nd_cur_bytes(&c, path_bytes, sizeof(rq->path),
                          NOTIFYD_PATH_MAX, &rq->path_len))
            return c.err;
        rq->has_path = 1;
        rq->path[rq->path_len] = 0;
        rq->path_flags = (int)nd_cur_u32(&c);
        break;
    }
    case NOTIFYD_REGENERATE:
    case NOTIFYD_FILTERED_REGENERATE: {
        uint8_t *path_bytes = (uint8_t *)rq->path;
        if (!nd_cur_cstring(&c, rq->name, sizeof(rq->name),
                            NOTIFYD_NAME_MAX, NULL))
            return c.err;
        rq->has_name = 1;
        rq->token = (int)nd_cur_u32(&c);
        rq->has_token = 1;
        rq->reg_type = nd_cur_u32(&c);
        rq->sig = (int)nd_cur_u32(&c);
        rq->prev_slot = (int)nd_cur_u32(&c);
        rq->prev_state = nd_cur_u64(&c);
        rq->prev_time = nd_cur_u64(&c);
        if (!nd_cur_bytes(&c, path_bytes, sizeof(rq->path),
                          NOTIFYD_PATH_MAX, &rq->path_len))
            return c.err;
        rq->has_path = 1;
        rq->path[rq->path_len] = 0;
        rq->path_flags = (int)nd_cur_u32(&c);
        break;
    }
    case NOTIFYD_CHECKIN:
    case NOTIFYD_FILTERED_CHECKIN:
    case NOTIFYD_GENERATE_COMMON_PORT:
    case NOTIFYD_DUMP:
        break;
    default:
        break;
    }
    if (c.err) return c.err;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reply start helper.                                                */
/* ------------------------------------------------------------------ */

/* Start a reply: reserve the header (24), write the optional
 * descriptor, the NDR and the RetCode. Returns 1 on success. */
static int nd_reply_start(nd_builder *b, uint8_t *reply_buf,
                          uint32_t reply_cap, int has_desc,
                          mach_port_name_t desc_name, uint32_t desc_disp) {
    b->buf = reply_buf;
    b->cap = reply_cap;
    b->off = 0;
    b->err = 0;
    nb_u32(b, 0);
    nb_u32(b, 0);
    nb_u32(b, 0);
    nb_u32(b, 0);
    nb_u32(b, 0);
    nb_u32(b, 0);
    if (has_desc) {
        nb_u32(b, 1);
        nb_desc(b, desc_name, desc_disp);
    }
    nb_ndr(b);
    nb_i32(b, KERN_SUCCESS);
    return !b->err;
}

/* ------------------------------------------------------------------ */
/* Handlers for routines with a reply.                                */
/* Returns the NOTIFY status (for the trace). The reply is built */
/* into reply_buf; *out_size is set on success.                  */
/* ------------------------------------------------------------------ */

static int nd_handle_routine(const mach_msg_header_t *hdr, nd_request *rq,
                             uint8_t *reply_buf, uint32_t reply_cap,
                             uint32_t *out_size) {
    nd_builder b;
    int status = NOTIFY_STATUS_OK;
    *out_size = 0;
    switch (rq->id) {
    case NOTIFYD_CHECK: {
        int check = 0;
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            check = 0;
            status = NOTIFY_STATUS_INVALID_TOKEN;
        } else {
            int ni = g_clients[ci].name_idx;
            check = (g_names[ni].val != g_clients[ci].lastval) ? 1 : 0;
            if (check) g_clients[ci].lastval = g_names[ni].val;
            status = NOTIFY_STATUS_OK;
        }
        rq->result = (uint64_t)check;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_i32(&b, check);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 0, out_size)) return status;
        return status;
    }
    case NOTIFYD_GET_STATE: {
        uint64_t state = 0;
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            status = NOTIFY_STATUS_CLIENT_NOT_FOUND;
        } else {
            state = g_names[g_clients[ci].name_idx].state;
            status = NOTIFY_STATUS_OK;
        }
        rq->result = state;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u64(&b, state);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_SUSPEND: {
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            status = NOTIFY_STATUS_CLIENT_NOT_FOUND;
        } else {
            if (g_clients[ci].suspend_count < 255)
                g_clients[ci].suspend_count++;
            g_clients[ci].suspended = 1;
            status = NOTIFY_STATUS_OK;
        }
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 0, out_size)) return status;
        return status;
    }
    case NOTIFYD_RESUME: {
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            status = NOTIFY_STATUS_CLIENT_NOT_FOUND;
        } else {
            if (g_clients[ci].suspend_count > 0)
                g_clients[ci].suspend_count--;
            if (g_clients[ci].suspend_count == 0) {
                g_clients[ci].suspended = 0;
                if (g_clients[ci].pending) nd_deliver_pending(ci);
            }
            status = NOTIFY_STATUS_OK;
        }
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 0, out_size)) return status;
        return status;
    }
    case NOTIFYD_POST_2: {
        uint64_t nid_out = 0xffffffffffffffffULL;
        int ni = nd_name_find(rq->name);
        uint32_t euid;
        if (ni < 0) {
            status = NOTIFY_STATUS_NO_NID;
        } else {
            nid_out = g_names[ni].name_id;
            euid = nd_effective_uid(rq);
            if (nd_check_access(euid, rq->audit_gid, rq->name,
                                NOTIFYD_ACCESS_WRITE) !=
                NOTIFY_STATUS_OK) {
                status = NOTIFY_STATUS_NOT_AUTHORIZED;
            } else if (nd_check_post_restricted(rq->name) !=
                       NOTIFY_STATUS_OK) {
                status = NOTIFY_STATUS_NOT_AUTHORIZED;
            } else {
                nd_post_name(ni);
                status = NOTIFY_STATUS_OK;
            }
        }
        rq->result = nid_out;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u64(&b, nid_out);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_REGISTER_CHECK_2: {
        int32_t size_out = -1;
        int32_t slot_out = -1;
        uint64_t nid_out = 0xffffffffffffffffULL;
        int ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                     rq->audit_gid);
        if (ni < 0) {
            status = NOTIFY_STATUS_NEW_CLIENT_FAILED;
        } else {
            nid_out = g_names[ni].name_id;
            status = nd_register(ni, rq->audit_pid, rq->token,
                                 NOTIFY_TYPE_PLAIN, rq->audit_uid,
                                 rq->audit_gid, 0, -1, MACH_PORT_NULL,
                                 NULL);
        }
        /* The shared-memory page is out of scope (G2): the
         * server-check fallback, size -1 and slot -1, exactly as
         * the real server answers when nslots == 0. The client is
         * registered as PLAIN. The name_id is the real one, not 0
         * (documented deviation: the real server leaves it at its
         * zero initializer on this path, but the client stores nid
         * and uses it for regen/post_3/get_state_2/set_state_2). */
        rq->result = nid_out;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_i32(&b, size_out);
        nb_i32(&b, slot_out);
        nb_u64(&b, nid_out);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_GET_STATE_2: {
        uint64_t state = 0;
        int ni = nd_name_find_by_id(rq->name_id);
        if (ni < 0) {
            status = NOTIFY_STATUS_INVALID_NAME;
        } else if (nd_check_access(rq->audit_uid, rq->audit_gid,
                                   g_names[ni].name,
                                   NOTIFYD_ACCESS_READ) !=
                   NOTIFY_STATUS_OK) {
            /* [guess] The handler internals for get_state_2 were
             * not read for the research doc; the READ check follows
             * the other by-name_id accessors. */
            status = NOTIFY_STATUS_NOT_AUTHORIZED;
        } else {
            state = g_names[ni].state;
            status = NOTIFY_STATUS_OK;
        }
        rq->result = state;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u64(&b, state);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_GET_STATE_3: {
        uint64_t state = 0;
        uint64_t nid_out = 0xffffffffffffffffULL;
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            status = NOTIFY_STATUS_CLIENT_NOT_FOUND;
        } else {
            int ni = g_clients[ci].name_idx;
            state = g_names[ni].state;
            nid_out = g_names[ni].name_id;
            status = NOTIFY_STATUS_OK;
        }
        rq->result = state;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u64(&b, state);
        nb_u64(&b, nid_out);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_SET_STATE_3:
    case NOTIFYD_FILTERED_SET_STATE_3: {
        uint64_t nid_out = 0xffffffffffffffffULL;
        int ci = nd_client_find(rq->audit_pid, rq->token);
        if (ci < 0) {
            status = NOTIFY_STATUS_CLIENT_NOT_FOUND;
        } else {
            int ni = g_clients[ci].name_idx;
            uint32_t euid = nd_effective_uid(rq);
            nid_out = g_names[ni].name_id;
            if (nd_check_post_restricted(g_names[ni].name) !=
                NOTIFY_STATUS_OK) {
                status = NOTIFY_STATUS_NOT_AUTHORIZED;
            } else if (nd_check_access(euid, rq->audit_gid,
                                       g_names[ni].name,
                                       NOTIFYD_ACCESS_WRITE) !=
                       NOTIFY_STATUS_OK) {
                status = NOTIFY_STATUS_NOT_AUTHORIZED;
            } else {
                g_names[ni].state = rq->state;
                g_state_clock++;
                g_names[ni].state_time = g_state_clock;
                status = NOTIFY_STATUS_OK;
            }
        }
        rq->result = nid_out;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u64(&b, nid_out);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_REGENERATE:
    case NOTIFYD_FILTERED_REGENERATE: {
        int32_t new_slot = -1;
        uint64_t new_nid = 0xffffffffffffffffULL;
        int ni = -1;
        int ci = -1;
        int path_ok = 1;
        uint32_t k;
        /* The filtered variant zeroes prev_state and prev_time
         * when the caller may not post the name. The fake has no
         * sandbox, so the post check always allows and the values
         * stand. An empty path (count 0) means no path and is not
         * validated. */
        if (rq->has_path && rq->path_len > 0) {
            path_ok = 0;
            k = 0;
            while (k < rq->path_len) {
                if (rq->path[k] == 0) {
                    path_ok = 1;
                    break;
                }
                k++;
            }
            if (!path_ok) status = NOTIFY_STATUS_INVALID_REQUEST;
        }
        if (status == NOTIFY_STATUS_OK &&
            nd_client_find(rq->audit_pid, rq->token) >= 0)
            status = NOTIFY_STATUS_DUP_CLIENT;
        if (status == NOTIFY_STATUS_OK) {
            ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                     rq->audit_gid);
            if (ni < 0) {
                status = NOTIFY_STATUS_NEW_CLIENT_FAILED;
            } else {
                new_nid = g_names[ni].name_id;
            }
        }
        if (status == NOTIFY_STATUS_OK) {
            switch (rq->reg_type) {
            case NOTIFY_TYPE_MEMORY:
                /* No shared memory in the fake (G2): the
                 * prev_slot < nslots check always fails with
                 * nslots == 0. */
                status = NOTIFY_STATUS_INVALID_REQUEST;
                break;
            case NOTIFY_TYPE_PLAIN:
                status = nd_register(ni, rq->audit_pid, rq->token,
                                     NOTIFY_TYPE_PLAIN, rq->audit_uid,
                                     rq->audit_gid, 0, -1,
                                     MACH_PORT_NULL, &ci);
                break;
            case NOTIFY_TYPE_SIGNAL:
                status = nd_register(ni, rq->audit_pid, rq->token,
                                     NOTIFY_TYPE_SIGNAL, rq->audit_uid,
                                     rq->audit_gid, rq->sig, -1,
                                     MACH_PORT_NULL, &ci);
                break;
            case NOTIFY_TYPE_COMMON_PORT: {
                int pi = nd_proc_find(rq->audit_pid);
                if (pi < 0 ||
                    g_procs[pi].common_port == MACH_PORT_NULL) {
                    status = NOTIFY_STATUS_INVALID_PORT_INTERNAL;
                } else {
                    status = nd_register(ni, rq->audit_pid, rq->token,
                                         NOTIFY_TYPE_COMMON_PORT,
                                         rq->audit_uid, rq->audit_gid,
                                         0, -1, MACH_PORT_NULL, &ci);
                }
                break;
            }
            default:
                status = NOTIFY_STATUS_TYPE_ISSUE;
                break;
            }
        }
        if (status == NOTIFY_STATUS_OK) {
            if (rq->has_path && rq->path_len > 0 && ci >= 0) {
                k = 0;
                while (k < rq->path_len &&
                       k < NOTIFYD_PATH_MAX) {
                    g_clients[ci].path[k] = rq->path[k];
                    k++;
                }
                g_clients[ci].path[k] = 0;
                g_clients[ci].path_len = rq->path_len;
                g_clients[ci].path_flags = rq->path_flags;
                g_clients[ci].has_path = 1;
            }
            if (rq->prev_time > g_names[ni].state_time) {
                g_names[ni].state = rq->prev_state;
                g_names[ni].state_time = rq->prev_time;
            }
            new_nid = g_names[ni].name_id;
        }
        rq->result = new_nid;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_i32(&b, new_slot);
        nb_u64(&b, new_nid);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 1, out_size)) return status;
        return status;
    }
    case NOTIFYD_CHECKIN:
    case NOTIFYD_FILTERED_CHECKIN: {
        uint32_t version = NOTIFYD_IPC_VERSION;
        uint32_t server_pid = g_server_pid;
        rq->result = (uint64_t)version;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 0, 0, 0))
            return status;
        nb_u32(&b, version);
        nb_u32(&b, server_pid);
        nb_i32(&b, status);
        if (!nd_reply_finish(&b, hdr, 0, 0, out_size)) return status;
        return status;
    }
    case NOTIFYD_GENERATE_COMMON_PORT: {
        uint32_t st = 0;
        mach_port_name_t recv_name = MACH_PORT_NULL;
        mach_port_name_t send_name = MACH_PORT_NULL;
        int pi = nd_proc_find_or_new(rq->audit_pid);
        if (pi < 0) {
            st = (uint32_t)KERN_RESOURCE_SHORTAGE;
        } else {
            if (g_procs[pi].common_port != MACH_PORT_NULL) {
                int gi = nd_gen_port_find_by_send(
                    g_procs[pi].common_port);
                int i = 0;
                while (i < NOTIFYD_MAX_CLIENTS) {
                    if (g_clients[i].in_use &&
                        g_clients[i].pid == rq->audit_pid &&
                        g_clients[i].type == NOTIFY_TYPE_COMMON_PORT)
                        nd_client_free(i);
                    i++;
                }
                if (gi >= 0) nd_gen_port_remove(gi);
                /* The old port's entries leak: there is no
                 * lock-safe destroy from under ipc_lock. */
                g_procs[pi].common_port = MACH_PORT_NULL;
            }
            recv_name = ipc_notifyd_gen_port_locked(
                NOTIFYD_GEN_PORT_QLIMIT, &send_name);
            if (recv_name == MACH_PORT_NULL ||
                send_name == MACH_PORT_NULL) {
                st = (uint32_t)KERN_RESOURCE_SHORTAGE;
                recv_name = MACH_PORT_NULL;
            } else if (nd_gen_port_add(send_name, recv_name, 1) < 0) {
                st = (uint32_t)KERN_RESOURCE_SHORTAGE;
                recv_name = MACH_PORT_NULL;
            } else {
                g_procs[pi].common_port = send_name;
                st = 0;
            }
        }
        if (!nd_reply_start(&b, reply_buf, reply_cap, 1, recv_name,
                            MACH_MSG_TYPE_MOVE_RECEIVE))
            return (int)st;
        nb_u32(&b, st);
        if (!nd_reply_finish(&b, hdr, 1, 0, out_size)) return (int)st;
        return (int)st;
    }
    case NOTIFYD_REGISTER_MACH_PORT_3: {
        uint32_t st = 0;
        mach_port_name_t recv_name = MACH_PORT_NULL;
        mach_port_name_t send_name = MACH_PORT_NULL;
        int ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                     rq->audit_gid);
        int ci = -1;
        if (ni < 0) {
            st = (uint32_t)NOTIFY_STATUS_NEW_CLIENT_FAILED;
        } else {
            recv_name = ipc_notifyd_gen_port_locked(
                NOTIFYD_GEN_PORT_QLIMIT, &send_name);
            if (recv_name == MACH_PORT_NULL ||
                send_name == MACH_PORT_NULL) {
                st = (uint32_t)KERN_RESOURCE_SHORTAGE;
                recv_name = MACH_PORT_NULL;
            } else if (nd_gen_port_add(send_name, recv_name, 0) < 0) {
                st = (uint32_t)KERN_RESOURCE_SHORTAGE;
                recv_name = MACH_PORT_NULL;
            } else {
                int rc = nd_register(ni, rq->audit_pid, rq->token,
                                     NOTIFY_TYPE_PORT, rq->audit_uid,
                                     rq->audit_gid, 0, -1, send_name,
                                     &ci);
                if (rc != NOTIFY_STATUS_OK) {
                    int gi = nd_gen_port_find_by_send(send_name);
                    if (gi >= 0) nd_gen_port_remove(gi);
                    /* The port entries leak (no lock-safe destroy
                     * from under ipc_lock); the record is gone so
                     * the port is never used. */
                    st = (uint32_t)rc;
                    recv_name = MACH_PORT_NULL;
                } else {
                    st = 0;
                }
            }
        }
        rq->result = (uint64_t)st;
        rq->has_result = 1;
        if (!nd_reply_start(&b, reply_buf, reply_cap, 1, recv_name,
                            MACH_MSG_TYPE_MOVE_RECEIVE))
            return (int)st;
        nb_u32(&b, st);
        if (!nd_reply_finish(&b, hdr, 1, 0, out_size)) return (int)st;
        return (int)st;
    }
    default:
        break;
    }
    return NOTIFY_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Handlers for simpleroutines (no reply). Returns the NOTIFY      */
/* status, for the trace only.                                    */
/* ------------------------------------------------------------------ */

static int nd_handle_simple(nd_request *rq) {
    int status = NOTIFY_STATUS_OK;
    switch (rq->id) {
    case NOTIFYD_SUSPEND_PID: {
        int i;
        int pi;
        if (rq->audit_euid != 0) return NOTIFY_STATUS_OK;
        pi = nd_proc_find_or_new((uint32_t)rq->pid_arg);
        if (pi >= 0) g_procs[pi].suspended = 1;
        i = 0;
        while (i < NOTIFYD_MAX_CLIENTS) {
            if (g_clients[i].in_use &&
                g_clients[i].pid == (uint32_t)rq->pid_arg) {
                if (g_clients[i].suspend_count < 255)
                    g_clients[i].suspend_count++;
                g_clients[i].suspended = 1;
            }
            i++;
        }
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_RESUME_PID: {
        int i;
        int pi;
        if (rq->audit_euid != 0) return NOTIFY_STATUS_OK;
        pi = nd_proc_find((uint32_t)rq->pid_arg);
        if (pi >= 0) g_procs[pi].suspended = 0;
        i = 0;
        while (i < NOTIFYD_MAX_CLIENTS) {
            if (g_clients[i].in_use &&
                g_clients[i].pid == (uint32_t)rq->pid_arg) {
                if (g_clients[i].suspend_count > 0)
                    g_clients[i].suspend_count--;
                if (g_clients[i].suspend_count == 0) {
                    g_clients[i].suspended = 0;
                    if (g_clients[i].pending) nd_deliver_pending(i);
                }
            }
            i++;
        }
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_POST_3: {
        int ni = nd_name_find_by_id(rq->name_id);
        uint32_t euid;
        if (ni < 0) return NOTIFY_STATUS_OK;
        euid = nd_effective_uid(rq);
        if (nd_check_access(euid, rq->audit_gid, g_names[ni].name,
                            NOTIFYD_ACCESS_WRITE) != NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        if (nd_check_post_restricted(g_names[ni].name) !=
            NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        nd_post_name(ni);
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_POST_4:
    case NOTIFYD_FILTERED_POST: {
        int ni = nd_name_find(rq->name);
        uint32_t euid;
        if (ni < 0) return NOTIFY_STATUS_OK;
        euid = nd_effective_uid(rq);
        if (nd_check_access(euid, rq->audit_gid, rq->name,
                            NOTIFYD_ACCESS_WRITE) != NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        if (nd_check_post_restricted(rq->name) != NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        nd_post_name(ni);
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_REGISTER_PLAIN_2: {
        int ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                     rq->audit_gid);
        if (ni < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
        status = nd_register(ni, rq->audit_pid, rq->token,
                             NOTIFY_TYPE_PLAIN, rq->audit_uid,
                             rq->audit_gid, 0, -1, MACH_PORT_NULL,
                             NULL);
        return status;
    }
    case NOTIFYD_REGISTER_SIGNAL_2: {
        int ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                     rq->audit_gid);
        if (ni < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
        status = nd_register(ni, rq->audit_pid, rq->token,
                             NOTIFY_TYPE_SIGNAL, rq->audit_uid,
                             rq->audit_gid, rq->sig, -1,
                             MACH_PORT_NULL, NULL);
        return status;
    }
    case NOTIFYD_REGISTER_FILE_DESCRIPTOR_2: {
        int ni;
        int fd;
        long fl;
        struct ipc_port *p;
        if (!rq->has_port) return NOTIFY_STATUS_OK;
        p = ipc_port_from_name(rq->port);
        if (p == NULL) return NOTIFY_STATUS_OK;
        fd = nd_fileport_find_by_ptr(p);
        if (fd < 0) return NOTIFY_STATUS_OK;
        ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                 rq->audit_gid);
        if (ni < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
        fl = sys3(ND_SYS_FCNTL, (long)fd, ND_F_GETFL, 0);
        if (fl >= 0) {
            (void)sys3(ND_SYS_FCNTL, (long)fd, ND_F_SETFL,
                       fl | ND_O_NONBLOCK);
        }
        status = nd_register(ni, rq->audit_pid, rq->token,
                             NOTIFY_TYPE_FILE, rq->audit_uid,
                             rq->audit_gid, 0, fd, MACH_PORT_NULL,
                             NULL);
        return status;
    }
    case NOTIFYD_REGISTER_MACH_PORT_2: {
        int ni;
        struct ipc_port *p;
        if (!rq->has_port) return NOTIFY_STATUS_OK;
        p = ipc_port_from_name(rq->port);
        if (p == NULL) return NOTIFY_STATUS_OK;
        /* The port must have been created by notifyd itself
         * (register_mach_port_3). Otherwise the call is ignored;
         * the port is not deallocated here (no lock-safe
         * deallocate from under ipc_lock). */
        if (nd_gen_port_find_by_ptr(p) < 0) return NOTIFY_STATUS_OK;
        ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                 rq->audit_gid);
        if (ni < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
        status = nd_register(ni, rq->audit_pid, rq->token,
                             NOTIFY_TYPE_PORT, rq->audit_uid,
                             rq->audit_gid, 0, -1, rq->port, NULL);
        return status;
    }
    case NOTIFYD_CANCEL_2:
        nd_client_cancel(rq->audit_pid, rq->token);
        return NOTIFY_STATUS_OK;
    case NOTIFYD_SET_STATE_2:
    case NOTIFYD_FILTERED_SET_STATE_2: {
        int ni = nd_name_find_by_id(rq->name_id);
        uint32_t euid;
        if (ni < 0) return NOTIFY_STATUS_OK;
        /* [guess] The handler internals for set_state_2 were not
         * read for the research doc; the restricted and WRITE
         * checks follow set_state_3. */
        if (nd_check_post_restricted(g_names[ni].name) !=
            NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        euid = nd_effective_uid(rq);
        if (nd_check_access(euid, rq->audit_gid, g_names[ni].name,
                            NOTIFYD_ACCESS_WRITE) != NOTIFY_STATUS_OK)
            return NOTIFY_STATUS_NOT_AUTHORIZED;
        g_names[ni].state = rq->state;
        g_state_clock++;
        g_names[ni].state_time = g_state_clock;
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_MONITOR_FILE_2: {
        int ci = nd_client_find(rq->audit_pid, rq->token);
        uint32_t k;
        int ok = 1;
        if (ci < 0) return NOTIFY_STATUS_OK;
        if (!rq->has_path || rq->path_len == 0) return NOTIFY_STATUS_OK;
        k = 0;
        while (k < rq->path_len) {
            if (rq->path[k] == 0) break;
            k++;
        }
        if (k >= rq->path_len) ok = 0;
        if (!ok) return NOTIFY_STATUS_INVALID_REQUEST;
        k = 0;
        while (k < rq->path_len && k < NOTIFYD_PATH_MAX) {
            g_clients[ci].path[k] = rq->path[k];
            k++;
        }
        g_clients[ci].path[k] = 0;
        g_clients[ci].path_len = rq->path_len;
        g_clients[ci].path_flags = rq->path_flags;
        g_clients[ci].has_path = 1;
        /* Path monitoring is a stub (G8): the path is stored, no
         * watch is created. */
        return NOTIFY_STATUS_OK;
    }
    case NOTIFYD_DUMP:
        /* Root-only in the real server, and it writes a dump to
         * the fileport. The fake has no dump writer: a silent
         * no-op for every caller. The fileport is not deallocated
         * (no lock-safe deallocate from under ipc_lock). */
        return NOTIFY_STATUS_OK;
    case NOTIFYD_REGISTER_COMMON_PORT: {
        int ni;
        int pi;
        ni = nd_name_find_or_new(rq->name, rq->audit_uid,
                                 rq->audit_gid);
        if (ni < 0) return NOTIFY_STATUS_NEW_CLIENT_FAILED;
        pi = nd_proc_find(rq->audit_pid);
        if (pi < 0 || g_procs[pi].common_port == MACH_PORT_NULL)
            return NOTIFY_STATUS_INVALID_PORT_INTERNAL;
        status = nd_register(ni, rq->audit_pid, rq->token,
                             NOTIFY_TYPE_COMMON_PORT, rq->audit_uid,
                             rq->audit_gid, 0, -1, MACH_PORT_NULL,
                             NULL);
        return status;
    }
    default:
        break;
    }
    return NOTIFY_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Entry point.                                                       */
/* ------------------------------------------------------------------ */

int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap,
                        uint32_t *out_reply_size) {
    nd_request rq;
    int prc;
    int status;
    if (!msg || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        reply_cap < 36 || !out_reply_size)
        return 0;
    *out_reply_size = 0;
    nd_memset((uint8_t *)&rq, 0, sizeof(rq));
    prc = nd_parse_request(msg, send_size, &rq);
    if (prc != 0) {
        nd_trace_request(&rq, send_size, prc);
        if (msg->msgh_local_port != MACH_PORT_NULL) {
            if (nd_build_error_reply(reply_buf, reply_cap, msg, prc,
                                     out_reply_size))
                return 1;
        }
        return 1;
    }
    if (rq.is_simple) {
        status = nd_handle_simple(&rq);
        nd_trace_request(&rq, send_size, status);
        return 1;
    }
    status = nd_handle_routine(msg, &rq, reply_buf, reply_cap,
                               out_reply_size);
    nd_trace_request(&rq, send_size, status);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Init.                                                              */
/* ------------------------------------------------------------------ */

mach_port_name_t notifyd_lookup_port(void) {
    return g_notifyd_port;
}

/* Pre-seed the name table from notify.conf.iPhone [src]. The
 * reserve lines create controlled (prefix) names with their owner
 * and access; the monitor line registers the timezone name and
 * records its watch path (stub, G8); the set lines register names
 * with state 0. Pre-seeded names are pinned: they are never
 * freed. Idempotent: a name that already exists is left alone
 * (its state is refreshed for the ipc_version name). */
static void nd_seed(const char *name, uint32_t uid, uint32_t gid,
                    uint32_t access, int controlled, uint64_t state) {
    int ni = nd_name_find(name);
    if (ni >= 0) return;
    ni = nd_name_new(name, uid, gid, access, 1, controlled);
    if (ni < 0) return;
    nd_name_add_ref(ni);
    g_names[ni].state = state;
}

static void nd_preseed(void) {
    int ni;
    nd_seed(NOTIFYD_CFG_SYSTEM_PREFIX, 0, 0,
            NOTIFYD_ACCESS_SYSTEM_PREFIX, 1, 0);
    nd_seed(NOTIFYD_CFG_CLOCK_SET, 0, 266,
            NOTIFYD_ACCESS_CLOCK_SET, 1, 0);
    nd_seed(NOTIFYD_CFG_TIMEZONE, 0, 0, NOTIFYD_ACCESS_DEFAULT, 0, 0);
    ni = nd_name_find(NOTIFYD_CFG_TIMEZONE);
    if (ni >= 0 && !g_names[ni].has_monitor) {
        uint32_t k = 0;
        while (NOTIFYD_CFG_TIMEZONE_PATH[k] != 0 &&
               k < NOTIFYD_PATH_MAX) {
            g_names[ni].monitor_path[k] = NOTIFYD_CFG_TIMEZONE_PATH[k];
            k++;
        }
        g_names[ni].monitor_path[k] = 0;
        g_names[ni].has_monitor = 1;
    }
    nd_seed(NOTIFYD_CFG_BATTERYSAVERMODE, 0, 0,
            NOTIFYD_ACCESS_DEFAULT, 0, 0);
    nd_seed(NOTIFYD_CFG_BATTERYSAVERMODE_DISC, 0, 0,
            NOTIFYD_ACCESS_DEFAULT, 0, 0);
    nd_seed(NOTIFYD_CFG_LOWPOWERMODE, 0, 0,
            NOTIFYD_ACCESS_DEFAULT, 0, 0);
    nd_seed(NOTIFYD_CFG_LOWPOWERMODE_DISC, 0, 0,
            NOTIFYD_ACCESS_DEFAULT, 0, 0);
    nd_seed(NOTIFYD_IPC_VERSION_NAME, 0, 0, NOTIFYD_ACCESS_DEFAULT, 0,
            ((uint64_t)g_server_pid << 32) | NOTIFYD_IPC_VERSION);
}

/* Register the service name with launchd, through the same
 * bootstrap_register2 message a real client would send (routine
 * 0xcc, dict {type:7, handle:0, name, port:mach_send,
 * flags:0}). Same shape as svchost's sh_bootstrap_register.
 * Returns 0 on success, else the "error" value the shim returned
 * (or a Mach error code from ipc_msg). */
static int nd_bootstrap_register(const char *name,
                                 mach_port_name_t port_name) {
    uint8_t msg[512];
    uint8_t dict[256];
    xw_builder b;
    int r;
    mach_port_name_t reply_port;
    mach_msg_header_t *hdr;
    mach_msg_body_t *body;
    mach_msg_port_descriptor_t *desc;
    uint32_t dict_len;
    int kr;
    int64_t err = 0;
    int ret = 0;
    xw_envelope env;
    xw_reader rd;
    xw_value v;

    if (xw_builder_init(&b, dict, sizeof(dict)) != XW_OK)
        return KERN_NO_SPACE;
    r = xw_dict_begin(&b);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, NOTIFYD_BOOTSTRAP_KEY_TYPE, 7);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, NOTIFYD_BOOTSTRAP_KEY_HANDLE, 0);
    if (r == XW_OK)
        r = xw_dict_add_string(&b, NOTIFYD_BOOTSTRAP_KEY_NAME, name);
    if (r == XW_OK)
        r = xw_dict_add_mach_send(&b, NOTIFYD_BOOTSTRAP_KEY_PORT,
                                  (uint64_t)port_name,
                                  MACH_MSG_TYPE_COPY_SEND);
    if (r == XW_OK)
        r = xw_dict_add_uint64(&b, NOTIFYD_BOOTSTRAP_KEY_FLAGS, 0);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return KERN_NO_SPACE;
    dict_len = xw_builder_len(&b);

    reply_port = ipc_mach_reply_port();
    if (reply_port == MACH_PORT_NULL) return KERN_RESOURCE_SHORTAGE;

    hdr = (mach_msg_header_t *)msg;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                    MACH_MSG_TYPE_MAKE_SEND_ONCE) |
                     MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = (uint32_t)sizeof(mach_msg_header_t) +
                     (uint32_t)sizeof(mach_msg_body_t) +
                     (uint32_t)sizeof(mach_msg_port_descriptor_t) +
                     dict_len;
    hdr->msgh_remote_port = (mach_port_name_t)NOTIFYD_BOOTSTRAP_PORT;
    hdr->msgh_local_port = reply_port;
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = NOTIFYD_REQUEST_MSGH_ID | LAUNCHD_ROUTINE_REGISTER;

    body = (mach_msg_body_t *)(hdr + 1);
    body->msgh_descriptor_count = 1;
    desc = (mach_msg_port_descriptor_t *)(body + 1);
    desc->name = port_name;
    desc->pad1 = 0;
    desc->pad2 = 0;
    desc->disposition = MACH_MSG_TYPE_COPY_SEND;
    desc->type = MACH_MSG_PORT_DESCRIPTOR;
    nd_memcpy((uint8_t *)(desc + 1), dict, dict_len);

    kr = ipc_msg(msg, MACH_SEND_MSG | MACH_RCV_MSG, hdr->msgh_size,
                 sizeof(msg), reply_port, MACH_MSG_TIMEOUT_NONE, 0,
                 NULL, 0);
    if (kr != MACH_MSG_SUCCESS) {
        (void)ipc_port_deallocate(ipc_task_self(), reply_port);
        return kr;
    }
    if (xw_envelope_parse(hdr, hdr->msgh_size, &env) != XW_OK ||
        xw_reader_init(&rd, env.payload, env.payload_len,
                       env.descs, env.ndescs) != XW_OK ||
        xw_dict_find(&rd, DAEMON_KEY_ERROR, &v) != XW_OK ||
        xw_as_int64(&v, &err) != XW_OK) {
        ret = KERN_FAILURE;
    } else if (err != 0) {
        ret = (int)err;
    }
    (void)ipc_port_deallocate(ipc_task_self(), reply_port);
    return ret;
}

int notifyd_init(void) {
    int kr;
    if (g_notifyd_inited) return KERN_SUCCESS;
    g_server_pid = (uint32_t)sys0(SYS_getpid);
    g_notifyd_port = ipc_notifyd_port_create();
    if (g_notifyd_port == MACH_PORT_NULL)
        return KERN_RESOURCE_SHORTAGE;
    kr = nd_bootstrap_register(NOTIFYD_SERVICE_NAME, g_notifyd_port);
    if (kr != KERN_SUCCESS) {
        (void)ipc_port_destroy(ipc_task_self(), g_notifyd_port);
        g_notifyd_port = MACH_PORT_NULL;
        return kr;
    }
    nd_preseed();
    g_notifyd_inited = 1;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Test hooks.                                                        */
/* ------------------------------------------------------------------ */

void notifyd_test_reset(void) {
    int i;
    i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        g_names[i].in_use = 0;
        i++;
    }
    i = 0;
    while (i < NOTIFYD_MAX_CLIENTS) {
        g_clients[i].in_use = 0;
        i++;
    }
    i = 0;
    while (i < NOTIFYD_MAX_PROCS) {
        g_procs[i].in_use = 0;
        i++;
    }
    i = 0;
    while (i < NOTIFYD_MAX_GEN_PORTS) {
        g_gen_ports[i].in_use = 0;
        i++;
    }
    i = 0;
    while (i < NOTIFYD_MAX_FILEPORTS) {
        g_fileports[i].in_use = 0;
        i++;
    }
    g_next_name_id = 0;
    g_state_clock = 0;
    g_server_pid = (uint32_t)sys0(SYS_getpid);
    nd_preseed();
}

int notifyd_test_name_count(void) {
    int n = 0;
    int i = 0;
    while (i < NOTIFYD_MAX_NAMES) {
        if (g_names[i].in_use) n++;
        i++;
    }
    return n;
}

int notifyd_test_client_count(void) {
    int n = 0;
    int i = 0;
    while (i < NOTIFYD_MAX_CLIENTS) {
        if (g_clients[i].in_use) n++;
        i++;
    }
    return n;
}

int notifyd_test_find_name(const char *name, uint64_t *name_id_out,
                           uint64_t *state_out, uint32_t *val_out,
                           uint32_t *postcount_out) {
    int ni = nd_name_find(name);
    if (ni < 0) return 0;
    if (name_id_out) *name_id_out = g_names[ni].name_id;
    if (state_out) *state_out = g_names[ni].state;
    if (val_out) *val_out = g_names[ni].val;
    if (postcount_out) *postcount_out = g_names[ni].postcount;
    return 1;
}

int notifyd_test_client_info(uint32_t pid, int token, int *type_out,
                             int *suspended_out, int *pending_out,
                             uint64_t *name_id_out) {
    int ci = nd_client_find(pid, token);
    if (ci < 0) return 0;
    if (type_out) *type_out = g_clients[ci].type;
    if (suspended_out) *suspended_out = g_clients[ci].suspended;
    if (pending_out) *pending_out = g_clients[ci].pending;
    if (name_id_out) {
        int ni = g_clients[ci].name_idx;
        *name_id_out = g_names[ni].name_id;
    }
    return 1;
}

mach_port_name_t notifyd_test_proc_common_port(uint32_t pid) {
    int pi = nd_proc_find(pid);
    if (pi < 0) return MACH_PORT_NULL;
    return g_procs[pi].common_port;
}
