/*
 * iokit.c: IOKit user-space to kernel wire emulation for ioswine
 *. See iokit.h for the design, sources and
 * [guess] list.
 *
 * Style: freestanding C, same as xpcwire.c / svchost.c /
 * fakeboard.c. No libc, no allocation, no blocking. All dispatch
 * is if/else-if chains; there are no function-pointer tables in
 * static data anywhere in this file.
 *
 * Locking: every entry point here runs under ipc_lock held by the
 * ipc.c dispatch thread (the kobject handler rules from svchost:
 * never take ipc_lock, never block, never allocate, never drop a
 * send-once reply). The tables below are therefore accessed
 * single-threaded and use plain static storage. The OOL scratch
 * buffer and the method output buffer are static for the same
 * reason.
 *
 * No em dashes are used in this file.
 */
#include "iokit.h"
#include "iosurface.h"

#if defined(__aarch64__)
#include "linux.h"
#endif

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static size_t ik_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int ik_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int ik_memeq(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != q[i]) return 0;
    }
    return 1;
}

static void ik_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void ik_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void ik_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    if (d == s) return;
    if (d < s) {
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

/* Little-endian helpers (byte-wise, no unaligned access). */
static uint32_t ik_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ik_rd64(const uint8_t *p) {
    return (uint64_t)ik_rd32(p) | ((uint64_t)ik_rd32(p + 4) << 32);
}

static void ik_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void ik_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void ik_wr64(uint8_t *p, uint64_t v) {
    ik_wr32(p, (uint32_t)(v & 0xffffffffu));
    ik_wr32(p + 4, (uint32_t)(v >> 32));
}

/* fd 2 output: raw syscall on the target, libc write() on host. */
#if defined(__aarch64__)
static long ik_fd_write(int fd, const void *buf, size_t n) {
    return sys_write(fd, buf, n);
}
#else
extern long write(int fd, const void *buf, unsigned long count);
static long ik_fd_write(int fd, const void *buf, size_t n) {
    return write(fd, buf, (unsigned long)n);
}
#endif

static void ik_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        long n = ik_fd_write(fd, p, len);
        if (n <= 0) return;
        p += (size_t)n;
        len -= (size_t)n;
    }
}

/* Decimal formatting for the trace line (no libc snprintf). */
static size_t ik_u32_dec(char *buf, size_t cap, uint32_t v) {
    char tmp[12];
    size_t n = 0;
    size_t i;
    if (v == 0) {
        if (cap > 0) buf[0] = '0';
        return 1;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

/* ------------------------------------------------------------------ */
/* Trace.                                                              */
/* ------------------------------------------------------------------ */

static int g_iokit_trace = -1; /* -1 = unresolved */

int iokit_trace_enabled(void) {
    const char *env;
    if (g_iokit_trace < 0) {
        env = lsk_getenv("IOSWINE_SVC_TRACE");
        g_iokit_trace = (env && env[0] == '1' && env[1] == '\0') ? 1 : 0;
    }
    return g_iokit_trace;
}

void iokit_set_trace(int on) {
    g_iokit_trace = on ? 1 : 0;
}

static void ik_trace_msg(uint32_t id, mach_port_name_t dest,
                         int has_selector, uint32_t selector) {
    char line[96];
    size_t n = 0;
    static const char pre[] = "iokit mig ";
    static const char mid[] = " dest ";
    static const char sel[] = " sel ";
    if (!iokit_trace_enabled()) return;
    ik_memcpy(line + n, pre, sizeof(pre) - 1);
    n += sizeof(pre) - 1;
    n += ik_u32_dec(line + n, sizeof(line) - n - 1, id);
    ik_memcpy(line + n, mid, sizeof(mid) - 1);
    n += sizeof(mid) - 1;
    n += ik_u32_dec(line + n, sizeof(line) - n - 1, dest);
    if (has_selector) {
        ik_memcpy(line + n, sel, sizeof(sel) - 1);
        n += sizeof(sel) - 1;
        n += ik_u32_dec(line + n, sizeof(line) - n - 1, selector);
    }
    line[n++] = '\n';
    ik_write_all(2, line, n);
}

/* ------------------------------------------------------------------ */
/* Tables.                                                             */
/* ------------------------------------------------------------------ */

static iokit_service g_services[IOKIT_MAX_SERVICES];
static iokit_connection g_conns[IOKIT_MAX_CONNECTIONS];
static iokit_iterator g_iters[IOKIT_MAX_ITERATORS];
static mach_port_name_t g_master_port = MACH_PORT_NULL;
static int g_inited = 0;

/* Static scratch: OOL reply data, method struct output, and the
 * OSSerialize validation/matching pools. All single-threaded under
 * ipc_lock, so static storage is safe and no allocation is needed. */
static uint8_t g_ool_scratch[IOKIT_OOL_SCRATCH_SIZE];
static uint8_t g_method_out_buf[IOKIT_MAX_STRUCT_IN];

static osser_value g_scratch_vals[128];
static osser_key g_scratch_keys[128];
static osser_value *g_scratch_objs[128];
static uint8_t g_scratch_text[2048];
static osser_value g_scratch2_vals[128];
static osser_key g_scratch2_keys[128];
static osser_value *g_scratch2_objs[128];
static uint8_t g_scratch2_text[2048];

static void ik_pool_reset(osser_pool *pool, osser_value *vals,
                          osser_key *keys, osser_value **objs,
                          uint8_t *text) {
    pool->vals = vals;
    pool->vals_cap = 128;
    pool->vals_used = 0;
    pool->keys = keys;
    pool->keys_cap = 128;
    pool->keys_used = 0;
    pool->objs = objs;
    pool->objs_cap = 128;
    pool->objs_used = 0;
    pool->text = text;
    pool->text_cap = 2048;
    pool->text_used = 0;
}

static void ik_scratch_pool(osser_pool *pool) {
    ik_pool_reset(pool, g_scratch_vals, g_scratch_keys, g_scratch_objs,
                  g_scratch_text);
}

static void ik_scratch2_pool(osser_pool *pool) {
    ik_pool_reset(pool, g_scratch2_vals, g_scratch2_keys, g_scratch2_objs,
                  g_scratch2_text);
}

static int ik_find_service_by_port(mach_port_name_t port) {
    int i;
    if (port == MACH_PORT_NULL) return -1;
    for (i = 0; i < IOKIT_MAX_SERVICES; i++) {
        if (g_services[i].in_use && g_services[i].port == port) return i;
    }
    return -1;
}

static int ik_find_service_by_name(const char *name) {
    int i;
    for (i = 0; i < IOKIT_MAX_SERVICES; i++) {
        if (g_services[i].in_use &&
            ik_strcmp(g_services[i].class_name, name) == 0) return i;
    }
    return -1;
}

static int ik_find_conn_by_port(mach_port_name_t port) {
    int i;
    if (port == MACH_PORT_NULL) return -1;
    for (i = 0; i < IOKIT_MAX_CONNECTIONS; i++) {
        if (g_conns[i].in_use && g_conns[i].port == port) return i;
    }
    return -1;
}

static int ik_find_iter_by_port(mach_port_name_t port) {
    int i;
    if (port == MACH_PORT_NULL) return -1;
    for (i = 0; i < IOKIT_MAX_ITERATORS; i++) {
        if (g_iters[i].in_use && g_iters[i].port == port) return i;
    }
    return -1;
}

/* Resolve any IOKit object port to (service index, object kind):
 * kind 0 = service, 1 = connection, 2 = iterator. Returns -1 when
 * the port is not an IOKit object at all. */
static int ik_resolve_object(mach_port_name_t port, int *kind_out) {
    int i = ik_find_service_by_port(port);
    if (i >= 0) {
        *kind_out = 0;
        return i;
    }
    i = ik_find_conn_by_port(port);
    if (i >= 0) {
        *kind_out = 1;
        return i;
    }
    i = ik_find_iter_by_port(port);
    if (i >= 0) {
        *kind_out = 2;
        return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Registry API.                                                       */
/* ------------------------------------------------------------------ */

int iokit_init(void) {
    mach_port_name_t port;
    uint8_t blob[128];
    osser_writer w;
    int rc;
    if (g_inited) return KERN_SUCCESS;
    /* The registry root entry is the IOResources service. Its port
     * is the master port: host_get_io_master returns it, and
     * io_registry_get_root_entry returns it too. Build its binary
     * property blob ({IOClass: "IOResources"}) with the writer. */
    if (osser_w_init(&w, blob, sizeof(blob)) != OSSER_ERR_OK) {
        return KERN_FAILURE;
    }
    if (osser_w_begin_dict(&w, 1) != OSSER_ERR_OK) return KERN_FAILURE;
    if (osser_w_key(&w, "IOClass") != OSSER_ERR_OK) return KERN_FAILURE;
    if (osser_w_string(&w, "IOResources") != OSSER_ERR_OK) {
        return KERN_FAILURE;
    }
    if (osser_w_end_dict(&w) != OSSER_ERR_OK) return KERN_FAILURE;
    rc = iokit_register_service("IOResources",
                                IOKIT_SERVICE_KIND_GENERIC,
                                blob, osser_w_len(&w), &port);
    if (rc != KERN_SUCCESS) return rc;
    g_master_port = port;
    g_inited = 1;
    return KERN_SUCCESS;
}

mach_port_name_t iokit_master_port(void) {
    return g_master_port;
}

int iokit_register_service(const char *class_name, uint32_t kind,
                           const uint8_t *props, uint32_t props_len,
                           mach_port_name_t *out_port) {
    int i;
    int slot = -1;
    mach_port_name_t port;
    uint8_t bin[IOKIT_MAX_PROPS];
    uint32_t bin_len = 0;
    osser_pool pool;
    osser_value *root = NULL;
    if (!class_name || !class_name[0] ||
        ik_strlen(class_name) >= IOKIT_CLASS_MAX) {
        return KERN_INVALID_ARGUMENT;
    }
    if (props_len > IOKIT_MAX_PROPS) return KERN_INVALID_ARGUMENT;
    if (props_len > 0) {
        if (!props) return KERN_INVALID_ARGUMENT;
        /* Validate the blob, and normalise XML input to binary so
         * the stored form is always OSSerializeBinary. */
        ik_scratch_pool(&pool);
        if (osser_decode_auto(props, props_len, &pool, &root) !=
            OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
        if (props_len >= 4 && ik_rd32(props) == OSSER_SIGNATURE) {
            ik_memcpy(bin, props, props_len);
            bin_len = props_len;
        } else {
            osser_writer w;
            if (osser_w_init(&w, bin, sizeof(bin)) != OSSER_ERR_OK) {
                return KERN_INVALID_ARGUMENT;
            }
            if (osser_w_value(&w, root) != OSSER_ERR_OK) {
                return KERN_INVALID_ARGUMENT;
            }
            bin_len = osser_w_len(&w);
        }
    }
    i = ik_find_service_by_name(class_name);
    if (i >= 0) {
        /* Re-registration updates properties and kind. */
        g_services[i].kind = kind;
        g_services[i].props_len = bin_len;
        if (bin_len > 0) ik_memcpy(g_services[i].props, bin, bin_len);
        if (out_port) *out_port = g_services[i].port;
        return KERN_SUCCESS;
    }
    for (i = 0; i < IOKIT_MAX_SERVICES; i++) {
        if (!g_services[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return KERN_NO_SPACE;
    port = ipc_iokit_port_create();
    if (port == MACH_PORT_NULL) return KERN_FAILURE;
    ik_memset(&g_services[slot], 0, sizeof(g_services[slot]));
    g_services[slot].in_use = 1;
    g_services[slot].port = port;
    g_services[slot].kind = kind;
    g_services[slot].refcount = 1;
    g_services[slot].props_len = bin_len;
    if (bin_len > 0) ik_memcpy(g_services[slot].props, bin, bin_len);
    {
        size_t n = ik_strlen(class_name);
        ik_memcpy(g_services[slot].class_name, class_name, n + 1);
    }
    if (out_port) *out_port = port;
    return KERN_SUCCESS;
}

int iokit_register_service_simple(const char *class_name, uint32_t kind,
                                  const char *const *keys,
                                  const uint64_t *vals, uint32_t n,
                                  mach_port_name_t *out_port) {
    uint8_t blob[IOKIT_MAX_PROPS];
    osser_writer w;
    uint32_t i;
    if (!class_name || n > 16) return KERN_INVALID_ARGUMENT;
    if (osser_w_init(&w, blob, sizeof(blob)) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    if (osser_w_begin_dict(&w, n + 1) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    if (osser_w_key(&w, "IOClass") != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    if (osser_w_string(&w, class_name) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    for (i = 0; i < n; i++) {
        if (!keys || !keys[i] || !vals) return KERN_INVALID_ARGUMENT;
        if (osser_w_key(&w, keys[i]) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
        if (osser_w_u64(&w, vals[i]) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
    }
    if (osser_w_end_dict(&w) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    return iokit_register_service(class_name, kind, blob,
                                  osser_w_len(&w), out_port);
}

mach_port_name_t iokit_lookup_service_port(const char *class_name) {
    int i;
    if (!class_name) return MACH_PORT_NULL;
    i = ik_find_service_by_name(class_name);
    if (i < 0) return MACH_PORT_NULL;
    return g_services[i].port;
}

/* Rewrite one service's property blob, replacing the value of
 * key with a u64 number. The dict must already contain the key. */
int iokit_service_set_property_u64(mach_port_name_t svc_port,
                                   const char *key, uint64_t val) {
    int i = ik_find_service_by_port(svc_port);
    osser_pool pool;
    osser_value *root = NULL;
    uint8_t blob[IOKIT_MAX_PROPS];
    osser_writer w;
    uint32_t j;
    if (i < 0) return KERN_INVALID_NAME;
    if (!key) return KERN_INVALID_ARGUMENT;
    ik_scratch_pool(&pool);
    if (g_services[i].props_len == 0 ||
        osser_decode(g_services[i].props, g_services[i].props_len,
                     &pool, &root) != OSSER_ERR_OK ||
        !root || root->type != OSSER_DICT) {
        return KERN_INVALID_ARGUMENT;
    }
    if (osser_w_init(&w, blob, sizeof(blob)) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    if (osser_w_begin_dict(&w, root->count) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    for (j = 0; j < root->count; j++) {
        if (osser_w_key_bytes(&w, root->keys[j].ptr,
                              root->keys[j].len) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
        if (root->keys[j].len == (uint32_t)ik_strlen(key) &&
            ik_memeq(root->keys[j].ptr, key, root->keys[j].len)) {
            if (osser_w_u64(&w, val) != OSSER_ERR_OK) {
                return KERN_INVALID_ARGUMENT;
            }
        } else {
            if (osser_w_value(&w, &root->items[j]) != OSSER_ERR_OK) {
                return KERN_INVALID_ARGUMENT;
            }
        }
    }
    if (osser_w_end_dict(&w) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    /* The key must have been present: verify by decoding again. */
    {
        osser_pool pool2;
        osser_value *root2 = NULL;
        const osser_value *found = NULL;
        ik_scratch2_pool(&pool2);
        if (osser_decode(blob, osser_w_len(&w), &pool2, &root2) !=
                OSSER_ERR_OK ||
            osser_dict_find(root2, key, &found) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
    }
    g_services[i].props_len = osser_w_len(&w);
    ik_memcpy(g_services[i].props, blob, g_services[i].props_len);
    return KERN_SUCCESS;
}

int iokit_connection_set_memory(mach_port_name_t conn_port,
                                uint64_t addr, uint64_t size) {
    int i = ik_find_conn_by_port(conn_port);
    if (i < 0) return KERN_INVALID_NAME;
    g_conns[i].memory_addr = addr;
    g_conns[i].memory_size = size;
    return KERN_SUCCESS;
}

int iokit_connection_service(mach_port_name_t conn_port,
                             uint32_t *kind_out,
                             mach_port_name_t *svc_port_out) {
    int i = ik_find_conn_by_port(conn_port);
    int s;
    if (i < 0) return KERN_INVALID_NAME;
    s = (int)g_conns[i].service_idx;
    if (s < 0 || s >= IOKIT_MAX_SERVICES || !g_services[s].in_use) {
        return KERN_INVALID_NAME;
    }
    if (kind_out) *kind_out = g_services[s].kind;
    if (svc_port_out) *svc_port_out = g_services[s].port;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Matching.                                                           */
/* ------------------------------------------------------------------ */

/* Compare one decoded value for equality (numbers by raw value,
 * strings by bytes, bools by value, data by bytes; dicts and
 * arrays are not compared element-wise in v1). */
static int ik_value_equal(const osser_value *a, const osser_value *b) {
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    if (a->type == OSSER_NUMBER) return a->num == b->num;
    if (a->type == OSSER_BOOL) return a->boolean == b->boolean;
    if (a->type == OSSER_STRING) {
        return a->len == b->len && ik_memeq(a->bytes, b->bytes, a->len);
    }
    if (a->type == OSSER_DATA) {
        return a->len == b->len && ik_memeq(a->bytes, b->bytes, a->len);
    }
    return 0;
}

/* Does service svc_idx match the decoded request dict req?
 * Class keys first, then the IOPropertyMatch sub-dict. Unknown
 * keys are ignored (v1 may over-match a dict that carries
 * criteria it does not evaluate, e.g. IOParentMatch; the
 * framework's IOSurfaceRoot lookup uses IOProviderClass only). */
static int ik_service_matches(int svc_idx, const osser_value *req) {
    const osser_value *v = NULL;
    const uint8_t *s = NULL;
    uint32_t len = 0;
    const char *want = NULL;
    uint32_t i;
    if (!req || req->type != OSSER_DICT) return 1; /* no dict: match all */
    if (req->count == 0) return 1;                 /* empty dict: match all */
    if (osser_dict_find(req, "IOProviderClass", &v) == OSSER_ERR_OK) {
        want = "IOProviderClass";
    } else if (osser_dict_find(req, "IOClass", &v) == OSSER_ERR_OK) {
        want = "IOClass";
    } else if (osser_dict_find(req, "IONameMatch", &v) == OSSER_ERR_OK) {
        want = "IONameMatch";
    }
    (void)want;
    if (v) {
        if (osser_as_string(v, &s, &len) != OSSER_ERR_OK) return 0;
        if (len != (uint32_t)ik_strlen(g_services[svc_idx].class_name) ||
            !ik_memeq(s, g_services[svc_idx].class_name, len)) {
            return 0;
        }
    }
    /* IOPropertyMatch: every listed property must exist on the
     * service with an equal value. */
    v = NULL;
    if (osser_dict_find(req, "IOPropertyMatch", &v) == OSSER_ERR_OK) {
        osser_pool pool;
        osser_value *props_root = NULL;
        if (!v || v->type != OSSER_DICT) return 0;
        if (g_services[svc_idx].props_len == 0) return 0;
        ik_scratch2_pool(&pool);
        if (osser_decode(g_services[svc_idx].props,
                         g_services[svc_idx].props_len, &pool,
                         &props_root) != OSSER_ERR_OK ||
            !props_root || props_root->type != OSSER_DICT) {
            return 0;
        }
        for (i = 0; i < v->count; i++) {
            char keybuf[64];
            const osser_value *have = NULL;
            uint32_t klen = v->keys[i].len;
            if (klen >= sizeof(keybuf)) return 0;
            ik_memcpy(keybuf, v->keys[i].ptr, klen);
            keybuf[klen] = '\0';
            if (osser_dict_find(props_root, keybuf, &have) !=
                    OSSER_ERR_OK ||
                !ik_value_equal(have, &v->items[i])) {
                return 0;
            }
        }
    }
    return 1;
}

/* Build the match list for a decoded request dict into out[]
 * (registry order). Returns the count. */
static uint32_t ik_build_matches(const osser_value *req,
                                 uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    int i;
    for (i = 0; i < IOKIT_MAX_SERVICES && n < max; i++) {
        if (!g_services[i].in_use) continue;
        if (ik_service_matches(i, req)) out[n++] = (uint32_t)i;
    }
    return n;
}

/* Allocate an iterator over matches[]. Returns the port or
 * MACH_PORT_NULL when the table is full. */
static mach_port_name_t ik_new_iterator(const uint32_t *matches,
                                        uint32_t count) {
    int i;
    int slot = -1;
    mach_port_name_t port;
    for (i = 0; i < IOKIT_MAX_ITERATORS; i++) {
        if (!g_iters[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return MACH_PORT_NULL;
    port = ipc_iokit_port_create();
    if (port == MACH_PORT_NULL) return MACH_PORT_NULL;
    ik_memset(&g_iters[slot], 0, sizeof(g_iters[slot]));
    g_iters[slot].in_use = 1;
    g_iters[slot].port = port;
    g_iters[slot].count = count;
    g_iters[slot].cursor = 0;
    g_iters[slot].refcount = 1;
    for (i = 0; (uint32_t)i < count && (uint32_t)i < IOKIT_MAX_SERVICES; i++) {
        g_iters[slot].matches[i] = matches[i];
    }
    return port;
}

/* Allocate a connection on service svc_idx. Returns the port or
 * MACH_PORT_NULL when the table is full. */
static mach_port_name_t ik_new_connection(int svc_idx, uint32_t type,
                                          mach_port_name_t owning_task) {
    int i;
    int slot = -1;
    mach_port_name_t port;
    for (i = 0; i < IOKIT_MAX_CONNECTIONS; i++) {
        if (!g_conns[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return MACH_PORT_NULL;
    port = ipc_iokit_port_create();
    if (port == MACH_PORT_NULL) return MACH_PORT_NULL;
    ik_memset(&g_conns[slot], 0, sizeof(g_conns[slot]));
    g_conns[slot].in_use = 1;
    g_conns[slot].port = port;
    g_conns[slot].service_idx = (uint32_t)svc_idx;
    g_conns[slot].type = type;
    g_conns[slot].owning_task = owning_task;
    g_conns[slot].refcount = 1;
    return port;
}

/* ------------------------------------------------------------------ */
/* OSSerializeBinary + XML plist (osser_*).                            */
/* ------------------------------------------------------------------ */

#define OSSER_TAG_DICT  0x01u
#define OSSER_TAG_ARRAY 0x02u
#define OSSER_TAG_SET   0x03u
#define OSSER_TAG_NUM   0x04u
#define OSSER_TAG_SYM   0x08u
#define OSSER_TAG_STR   0x09u
#define OSSER_TAG_DATA  0x0au
#define OSSER_TAG_BOOL  0x0bu
#define OSSER_TAG_REF   0x0cu
#define OSSER_TAG_END   0x80000000u

/* ---------------- reader ---------------- */

typedef struct {
    const uint8_t *buf;
    uint32_t len;
    uint32_t pos;
    osser_pool *pool;
    int err;
} osser_reader;

static osser_value *osser_pool_val(osser_pool *pool) {
    osser_value *v;
    if (pool->vals_used >= pool->vals_cap) return NULL;
    v = &pool->vals[pool->vals_used++];
    ik_memset(v, 0, sizeof(*v));
    return v;
}

static int osser_pool_obj(osser_pool *pool, osser_value *v) {
    if (pool->objs_used >= pool->objs_cap) return -1;
    pool->objs[pool->objs_used++] = v;
    return 0;
}

static int osser_rd_u32(osser_reader *r, uint32_t *out) {
    if (r->err) return -1;
    if (r->pos + 4 > r->len) {
        r->err = OSSER_ERR_TRUNCATED;
        return -1;
    }
    *out = ik_rd32(r->buf + r->pos);
    r->pos += 4;
    return 0;
}

static const uint8_t *osser_rd_bytes(osser_reader *r, uint32_t n) {
    const uint8_t *p;
    if (r->err) return NULL;
    if (n > r->len - r->pos) {
        r->err = OSSER_ERR_TRUNCATED;
        return NULL;
    }
    p = r->buf + r->pos;
    r->pos += n;
    return p;
}

/* Skip the 4-byte padding after a string/symbol/data payload. */
static void osser_rd_pad(osser_reader *r, uint32_t n) {
    uint32_t pad = (4u - (n & 3u)) & 3u;
    if (r->err) return;
    if (pad > r->len - r->pos) {
        r->err = OSSER_ERR_TRUNCATED;
        return;
    }
    r->pos += pad;
}

static osser_value *osser_read_value(osser_reader *r, uint32_t depth);

static osser_value *osser_read_string_like(osser_reader *r, uint32_t tag,
                                           uint32_t len, uint32_t depth) {
    osser_value *v;
    const uint8_t *p;
    (void)depth;
    v = osser_pool_val(r->pool);
    if (!v) {
        r->err = OSSER_ERR_NOSPACE;
        return NULL;
    }
    v->type = (tag == OSSER_TAG_DATA) ? OSSER_DATA : OSSER_STRING;
    p = osser_rd_bytes(r, len);
    if (!p) return NULL;
    v->bytes = p;
    v->len = len;
    osser_rd_pad(r, len);
    if (r->err) return NULL;
    if (osser_pool_obj(r->pool, v) != 0) {
        r->err = OSSER_ERR_NOSPACE;
        return NULL;
    }
    return v;
}

static osser_value *osser_read_value(osser_reader *r, uint32_t depth) {
    uint32_t tag;
    uint32_t kind;
    uint32_t count;
    osser_value *v;
    if (r->err) return NULL;
    if (depth > OSSER_MAX_DEPTH) {
        r->err = OSSER_ERR_OVERFLOW;
        return NULL;
    }
    if (osser_rd_u32(r, &tag) != 0) return NULL;
    kind = (tag >> 24) & 0x7fu; /* top byte minus the EndCollection bit */
    count = tag & 0x00ffffffu;
    if (kind == OSSER_TAG_DICT || kind == OSSER_TAG_ARRAY ||
        kind == OSSER_TAG_SET) {
        uint32_t i;
        v = osser_pool_val(r->pool);
        if (!v) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        v->type = (kind == OSSER_TAG_DICT) ? OSSER_DICT : OSSER_ARRAY;
        v->count = count;
        if (osser_pool_obj(r->pool, v) != 0) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        if (count > 0) {
            if (r->pool->vals_used + count > r->pool->vals_cap) {
                r->err = OSSER_ERR_NOSPACE;
                return NULL;
            }
            v->items = &r->pool->vals[r->pool->vals_used];
            r->pool->vals_used += count;
            if (kind == OSSER_TAG_DICT) {
                if (r->pool->keys_used + count > r->pool->keys_cap) {
                    r->err = OSSER_ERR_NOSPACE;
                    return NULL;
                }
                v->keys = &r->pool->keys[r->pool->keys_used];
                r->pool->keys_used += count;
            }
            for (i = 0; i < count; i++) {
                osser_value *child;
                if (kind == OSSER_TAG_DICT) {
                    osser_value *keyv = osser_read_value(r, depth + 1);
                    if (!keyv || keyv->type != OSSER_STRING) {
                        r->err = OSSER_ERR_BAD_TAG;
                        return NULL;
                    }
                    v->keys[i].ptr = keyv->bytes;
                    v->keys[i].len = keyv->len;
                }
                child = osser_read_value(r, depth + 1);
                if (!child) return NULL;
                v->items[i] = *child;
            }
        }
        return v;
    }
    if (kind == OSSER_TAG_NUM) {
        uint32_t nbytes;
        v = osser_pool_val(r->pool);
        if (!v) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        v->type = OSSER_NUMBER;
        if (count == 32) {
            uint32_t x;
            if (osser_rd_u32(r, &x) != 0) return NULL;
            v->num = x;
            v->num_bits = 32;
            nbytes = 4;
        } else if (count == 64) {
            const uint8_t *p = osser_rd_bytes(r, 8);
            if (!p) return NULL;
            v->num = ik_rd64(p);
            v->num_bits = 64;
            nbytes = 8;
        } else {
            r->err = OSSER_ERR_BAD_TAG;
            return NULL;
        }
        (void)nbytes;
        if (osser_pool_obj(r->pool, v) != 0) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        return v;
    }
    if (kind == OSSER_TAG_SYM || kind == OSSER_TAG_STR ||
        kind == OSSER_TAG_DATA) {
        return osser_read_string_like(r, kind, count, depth);
    }
    if (kind == OSSER_TAG_BOOL) {
        v = osser_pool_val(r->pool);
        if (!v) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        v->type = OSSER_BOOL;
        v->boolean = (count != 0) ? 1 : 0;
        if (osser_pool_obj(r->pool, v) != 0) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        return v;
    }
    if (kind == OSSER_TAG_REF) {
        osser_value *ref;
        if (count >= r->pool->objs_used) {
            r->err = OSSER_ERR_BAD_REF;
            return NULL;
        }
        ref = r->pool->objs[count];
        v = osser_pool_val(r->pool);
        if (!v) {
            r->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        /* A reference decodes as a struct copy of the referenced
         * node. The copy is NOT registered as a new object: the
         * reference IS the object, it does not create another one.
         * The object table is decode-order over every object the
         * reader meets, INCLUDING dictionary keys (symbols and
         * strings alike): the serializer walks keys through the
         * same object table [guess, see iokit.h]. */
        *v = *ref;
        return v;
    }
    r->err = OSSER_ERR_BAD_TAG;
    return NULL;
}

int osser_decode(const uint8_t *buf, uint32_t len, osser_pool *pool,
                 osser_value **root_out) {
    osser_reader r;
    osser_value *root;
    if (!buf || !pool || !root_out || len < 4) return OSSER_ERR_TRUNCATED;
    if (ik_rd32(buf) != OSSER_SIGNATURE) return OSSER_ERR_BAD_SIG;
    r.buf = buf;
    r.len = len;
    r.pos = 4;
    r.pool = pool;
    r.err = OSSER_ERR_OK;
    root = osser_read_value(&r, 0);
    if (!root) return r.err ? r.err : OSSER_ERR_BAD_TAG;
    /* Trailing bytes are tolerated (a blob may carry padding). */
    *root_out = root;
    return OSSER_ERR_OK;
}

/* ---------------- XML plist ---------------- */

typedef struct {
    const uint8_t *buf;
    uint32_t len;
    uint32_t pos;
    osser_pool *pool;
    int err;
} xml_reader;

static void xml_skip_ws(xml_reader *x) {
    while (x->pos < x->len) {
        uint8_t c = x->buf[x->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            x->pos++;
        } else {
            break;
        }
    }
}

/* Skip <?...?>, <!--...--> and <!DOCTYPE ...> prolog pieces. */
static void xml_skip_prolog(xml_reader *x) {
    for (;;) {
        xml_skip_ws(x);
        if (x->pos + 1 >= x->len || x->buf[x->pos] != '<') return;
        if (x->buf[x->pos + 1] == '?') {
            uint32_t i = x->pos + 2;
            while (i + 1 < x->len &&
                   !(x->buf[i] == '?' && x->buf[i + 1] == '>')) {
                i++;
            }
            if (i + 1 >= x->len) {
                x->err = OSSER_ERR_BAD_XML;
                return;
            }
            x->pos = i + 2;
        } else if (x->pos + 3 < x->len &&
                   x->buf[x->pos + 1] == '!' &&
                   x->buf[x->pos + 2] == '-' &&
                   x->buf[x->pos + 3] == '-') {
            uint32_t i = x->pos + 4;
            while (i + 2 < x->len &&
                   !(x->buf[i] == '-' && x->buf[i + 1] == '-' &&
                     x->buf[i + 2] == '>')) {
                i++;
            }
            if (i + 2 >= x->len) {
                x->err = OSSER_ERR_BAD_XML;
                return;
            }
            x->pos = i + 3;
        } else if (x->pos + 1 < x->len && x->buf[x->pos + 1] == '!') {
            uint32_t i = x->pos + 2;
            while (i < x->len && x->buf[i] != '>') i++;
            if (i >= x->len) {
                x->err = OSSER_ERR_BAD_XML;
                return;
            }
            x->pos = i + 1;
        } else {
            return;
        }
    }
}

/* Read "<name ...>" or "<name .../>". Returns 0 on success, sets
 * *self_closing and fills name (NUL terminated, up to 15 chars).
 * The position is left after the '>'. */
static int xml_read_open_tag(xml_reader *x, char *name, size_t name_cap,
                             int *self_closing) {
    size_t n = 0;
    *self_closing = 0;
    if (x->pos >= x->len || x->buf[x->pos] != '<') {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    x->pos++;
    if (x->pos < x->len && x->buf[x->pos] == '/') {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    while (x->pos < x->len) {
        uint8_t c = x->buf[x->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (n + 1 < name_cap) name[n++] = (char)c;
            x->pos++;
        } else {
            break;
        }
    }
    name[n] = '\0';
    if (n == 0) {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    /* Skip attributes (quoted values may contain '>'). */
    while (x->pos < x->len) {
        uint8_t c = x->buf[x->pos];
        if (c == '"' || c == '\'') {
            uint8_t q = c;
            x->pos++;
            while (x->pos < x->len && x->buf[x->pos] != q) x->pos++;
            if (x->pos >= x->len) {
                x->err = OSSER_ERR_BAD_XML;
                return -1;
            }
            x->pos++;
        } else if (c == '/') {
            if (x->pos + 1 < x->len && x->buf[x->pos + 1] == '>') {
                *self_closing = 1;
                x->pos += 2;
                return 0;
            }
            x->pos++;
        } else if (c == '>') {
            x->pos++;
            return 0;
        } else {
            x->pos++;
        }
    }
    x->err = OSSER_ERR_BAD_XML;
    return -1;
}

/* Expect a closing tag "</name>". */
static int xml_read_close_tag(xml_reader *x, const char *name) {
    size_t n = ik_strlen(name);
    xml_skip_ws(x);
    if (x->pos + 2 + n + 1 > x->len || x->buf[x->pos] != '<' ||
        x->buf[x->pos + 1] != '/' ||
        !ik_memeq(x->buf + x->pos + 2, name, n) ||
        x->buf[x->pos + 2 + n] != '>') {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    x->pos += (uint32_t)(2 + n + 1);
    return 0;
}

static uint8_t *xml_text_alloc(xml_reader *x, uint32_t len) {
    uint8_t *p;
    if (len + 1 > x->pool->text_cap - x->pool->text_used) {
        x->err = OSSER_ERR_NOSPACE;
        return NULL;
    }
    p = x->pool->text + x->pool->text_used;
    x->pool->text_used += len + 1;
    return p;
}

/* Read element text up to the next '<', entity-decoding into the
 * pool text buffer, trimming leading and trailing whitespace.
 * The result is NUL terminated; *len_out excludes the NUL. */
static int xml_read_text(xml_reader *x, const uint8_t **out,
                         uint32_t *len_out) {
    uint32_t start = x->pos;
    uint32_t end;
    uint32_t i;
    uint8_t *dst;
    uint32_t dlen = 0;
    uint32_t cap;
    if (x->pos >= x->len) {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    end = start;
    while (end < x->len && x->buf[end] != '<') end++;
    if (end >= x->len) {
        x->err = OSSER_ERR_BAD_XML;
        return -1;
    }
    /* Trim whitespace at both ends before decoding. */
    while (start < end) {
        uint8_t c = x->buf[start];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') start++;
        else break;
    }
    while (end > start) {
        uint8_t c = x->buf[end - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') end--;
        else break;
    }
    cap = (end > start) ? (end - start + 1) : 1;
    dst = xml_text_alloc(x, cap);
    if (!dst) return -1;
    i = start;
    while (i < end) {
        uint8_t c = x->buf[i];
        if (c == '&') {
            if (i + 5 <= end && ik_memeq(x->buf + i, "&amp;", 5)) {
                dst[dlen++] = '&';
                i += 5;
            } else if (i + 4 <= end &&
                       ik_memeq(x->buf + i, "&lt;", 4)) {
                dst[dlen++] = '<';
                i += 4;
            } else if (i + 4 <= end &&
                       ik_memeq(x->buf + i, "&gt;", 4)) {
                dst[dlen++] = '>';
                i += 4;
            } else if (i + 6 <= end &&
                       ik_memeq(x->buf + i, "&quot;", 6)) {
                dst[dlen++] = '"';
                i += 6;
            } else if (i + 6 <= end &&
                       ik_memeq(x->buf + i, "&apos;", 6)) {
                dst[dlen++] = '\'';
                i += 6;
            } else if (i + 2 < end && x->buf[i + 1] == '#') {
                uint32_t v = 0;
                uint32_t j = i + 2;
                int hex = 0;
                if (j < end && (x->buf[j] == 'x' || x->buf[j] == 'X')) {
                    hex = 1;
                    j++;
                }
                if (j >= end) {
                    x->err = OSSER_ERR_BAD_XML;
                    return -1;
                }
                while (j < end && x->buf[j] != ';') {
                    uint8_t d = x->buf[j];
                    if (hex) {
                        if (d >= '0' && d <= '9') v = v * 16 + (d - '0');
                        else if (d >= 'a' && d <= 'f') {
                            v = v * 16 + (d - 'a' + 10);
                        } else if (d >= 'A' && d <= 'F') {
                            v = v * 16 + (d - 'A' + 10);
                        } else {
                            x->err = OSSER_ERR_BAD_XML;
                            return -1;
                        }
                    } else {
                        if (d < '0' || d > '9') {
                            x->err = OSSER_ERR_BAD_XML;
                            return -1;
                        }
                        v = v * 10 + (d - '0');
                    }
                    j++;
                }
                if (j >= end || v > 255) {
                    x->err = OSSER_ERR_BAD_XML;
                    return -1;
                }
                dst[dlen++] = (uint8_t)v;
                i = j + 1;
            } else {
                x->err = OSSER_ERR_BAD_XML;
                return -1;
            }
        } else {
            dst[dlen++] = c;
            i++;
        }
    }
    dst[dlen] = '\0';
    x->pos = end;
    *out = dst;
    *len_out = dlen;
    return 0;
}

static int xml_parse_int(const uint8_t *s, uint32_t len, int64_t *out) {
    uint32_t i = 0;
    int neg = 0;
    uint64_t v = 0;
    if (len == 0) return -1;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    if (i >= len) return -1;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > 0x8000000000000000ULL) return -1;
    }
    if (neg) {
        if (v > 0x8000000000000000ULL) return -1;
        *out = (v == 0x8000000000000000ULL)
                   ? (int64_t)0x8000000000000000ULL
                   : -(int64_t)v;
    } else {
        if (v > 0x7fffffffffffffffULL) return -1;
        *out = (int64_t)v;
    }
    return 0;
}

/* Minimal decimal float parser (no libc strtod). */
static int xml_parse_real(const uint8_t *s, uint32_t len, double *out) {
    uint32_t i = 0;
    int neg = 0;
    double v = 0.0;
    double frac = 0.0;
    double div = 1.0;
    int expn = 0;
    int eneg = 0;
    int any = 0;
    if (len == 0) return -1;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + (double)(s[i] - '0');
        any = 1;
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10.0 + (double)(s[i] - '0');
            div *= 10.0;
            any = 1;
            i++;
        }
    }
    if (!any) return -1;
    v += frac / div;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        double mult = 1.0;
        int e;
        i++;
        if (i < len && (s[i] == '-' || s[i] == '+')) {
            eneg = (s[i] == '-');
            i++;
        }
        if (i >= len) return -1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            expn = expn * 10 + (s[i] - '0');
            if (expn > 308) return -1;
            i++;
        }
        for (e = 0; e < expn; e++) mult *= 10.0;
        v = eneg ? (v / mult) : (v * mult);
    }
    if (i != len) return -1;
    *out = neg ? -v : v;
    return 0;
}

static int xml_b64_val(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Base64-decode text (whitespace skipped) into the pool text
 * buffer. Returns 0 on success. */
static int xml_b64_decode(xml_reader *x, const uint8_t *s, uint32_t len,
                          const uint8_t **out, uint32_t *len_out) {
    uint32_t cap = (len / 4 + 1) * 3 + 1;
    uint8_t *dst = xml_text_alloc(x, cap);
    uint32_t dlen = 0;
    uint32_t i = 0;
    if (!dst) return -1;
    while (i < len) {
        int vals[4];
        int n = 0;
        int pad = 0;
        while (n < 4 && i < len) {
            uint8_t c = s[i++];
            int v;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c == '=') {
                vals[n++] = 0;
                pad++;
                continue;
            }
            v = xml_b64_val(c);
            if (v < 0) {
                x->err = OSSER_ERR_BAD_XML;
                return -1;
            }
            vals[n++] = v;
        }
        if (n == 0) break;
        if (n < 4) {
            x->err = OSSER_ERR_BAD_XML;
            return -1;
        }
        dst[dlen++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
        if (pad < 2) {
            dst[dlen++] = (uint8_t)(((vals[1] & 0xf) << 4) | (vals[2] >> 2));
        }
        if (pad < 1) {
            dst[dlen++] = (uint8_t)(((vals[2] & 0x3) << 6) | vals[3]);
        }
    }
    dst[dlen] = '\0';
    *out = dst;
    *len_out = dlen;
    return 0;
}

static osser_value *xml_parse_value(xml_reader *x, uint32_t depth);

/* Count entries of the dict starting at the current position (just
 * after the <dict> open tag) without consuming input. Counts <key>
 * tags at this dict's own level; nested dicts/arrays are tracked
 * by depth. Safe for well-formed XML: a literal '<' never appears
 * unescaped in element text or attribute values, so scanning for
 * tags cannot mistake text for markup. */
static uint32_t xml_count_dict_entries(xml_reader *x) {
    uint32_t pos = x->pos;
    uint32_t depth = 0;
    uint32_t count = 0;
    while (pos < x->len) {
        uint32_t i = pos;
        while (i < x->len && x->buf[i] != '<') i++;
        if (i >= x->len) break;
        pos = i + 1;
        if (pos < x->len && x->buf[pos] == '/') {
            /* Closing tag. */
            if (depth == 0 && pos + 5 < x->len &&
                ik_memeq(x->buf + pos + 1, "dict>", 5)) {
                break;
            }
            if (pos + 5 < x->len &&
                (ik_memeq(x->buf + pos + 1, "dict>", 5) ||
                 ik_memeq(x->buf + pos + 1, "array>", 6))) {
                if (depth > 0) depth--;
            }
        } else {
            /* Opening tag: read its name. */
            char nm[8];
            size_t n = 0;
            int self_close = 0;
            uint32_t j;
            while (pos < x->len && n < sizeof(nm) - 1) {
                uint8_t c = x->buf[pos];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                    nm[n++] = (char)c;
                    pos++;
                } else {
                    break;
                }
            }
            nm[n] = '\0';
            /* A self-closing tag has no closing tag and must not
             * change the depth. */
            j = pos;
            while (j < x->len && x->buf[j] != '>') {
                if (x->buf[j] == '/' && j + 1 < x->len &&
                    x->buf[j + 1] == '>') {
                    self_close = 1;
                }
                j++;
            }
            if (depth == 0 && ik_strcmp(nm, "key") == 0) count++;
            if (!self_close &&
                (ik_strcmp(nm, "dict") == 0 ||
                 ik_strcmp(nm, "array") == 0)) {
                depth++;
            }
        }
    }
    return count;
}

/* Count items of the array starting at the current position (just
 * after the <array> open tag). Counts opening tags at this array's
 * own level; nested dicts/arrays are tracked by depth. */
static uint32_t xml_count_array_items(xml_reader *x) {
    uint32_t pos = x->pos;
    uint32_t depth = 0;
    uint32_t count = 0;
    while (pos < x->len) {
        uint32_t i = pos;
        while (i < x->len && x->buf[i] != '<') i++;
        if (i >= x->len) break;
        pos = i + 1;
        if (pos < x->len && x->buf[pos] == '/') {
            if (depth == 0 && pos + 6 < x->len &&
                ik_memeq(x->buf + pos + 1, "array>", 6)) {
                break;
            }
            if (pos + 5 < x->len &&
                (ik_memeq(x->buf + pos + 1, "dict>", 5) ||
                 ik_memeq(x->buf + pos + 1, "array>", 6))) {
                if (depth > 0) depth--;
            }
        } else {
            char nm[8];
            size_t n = 0;
            int self_close = 0;
            while (pos < x->len && n < sizeof(nm) - 1) {
                uint8_t c = x->buf[pos];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                    nm[n++] = (char)c;
                    pos++;
                } else {
                    break;
                }
            }
            nm[n] = '\0';
            /* A self-closing tag at this level is one item and
             * does not change depth. Detect "/>" before the next
             * '<' or '>' by scanning to the tag end. */
            {
                uint32_t j = pos;
                while (j < x->len && x->buf[j] != '>') {
                    if (x->buf[j] == '/' && j + 1 < x->len &&
                        x->buf[j + 1] == '>') {
                        self_close = 1;
                    }
                    j++;
                }
            }
            if (depth == 0 && n > 0) count++;
            if (!self_close &&
                (ik_strcmp(nm, "dict") == 0 ||
                 ik_strcmp(nm, "array") == 0)) {
                depth++;
            }
        }
    }
    return count;
}

static osser_value *xml_parse_value(xml_reader *x, uint32_t depth) {
    char name[16];
    int self_closing = 0;
    osser_value *v;
    if (x->err) return NULL;
    if (depth > OSSER_MAX_DEPTH) {
        x->err = OSSER_ERR_OVERFLOW;
        return NULL;
    }
    xml_skip_prolog(x);
    if (x->err) return NULL;
    if (xml_read_open_tag(x, name, sizeof(name), &self_closing) != 0) {
        return NULL;
    }
    v = osser_pool_val(x->pool);
    if (!v) {
        x->err = OSSER_ERR_NOSPACE;
        return NULL;
    }
    if (ik_strcmp(name, "plist") == 0) {
        osser_value *child;
        if (self_closing) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        child = xml_parse_value(x, depth + 1);
        if (!child) return NULL;
        if (xml_read_close_tag(x, "plist") != 0) return NULL;
        return child;
    }
    if (ik_strcmp(name, "dict") == 0) {
        uint32_t count = 0;
        uint32_t total;
        osser_value *items = NULL;
        osser_key *keys = NULL;
        if (self_closing) {
            v->type = OSSER_DICT;
            v->count = 0;
            return v;
        }
        /* Two passes: count the entries first, then reserve the
         * contiguous items/keys runs (like the binary reader), so
         * child nodes allocate after the runs. */
        total = xml_count_dict_entries(x);
        if (total > x->pool->vals_cap - x->pool->vals_used ||
            total > x->pool->keys_cap - x->pool->keys_used) {
            x->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        items = &x->pool->vals[x->pool->vals_used];
        keys = &x->pool->keys[x->pool->keys_used];
        x->pool->vals_used += total;
        x->pool->keys_used += total;
        v->type = OSSER_DICT;
        for (;;) {
            char kname[16];
            int kself = 0;
            const uint8_t *ktext = NULL;
            uint32_t klen = 0;
            osser_value *val;
            xml_skip_prolog(x);
            if (x->err) return NULL;
            if (x->pos + 7 <= x->len &&
                ik_memeq(x->buf + x->pos, "</dict>", 7)) {
                x->pos += 7;
                break;
            }
            if (count >= total) {
                x->err = OSSER_ERR_BAD_XML;
                return NULL;
            }
            if (xml_read_open_tag(x, kname, sizeof(kname), &kself) != 0) {
                return NULL;
            }
            if (ik_strcmp(kname, "key") != 0 || kself) {
                x->err = OSSER_ERR_BAD_XML;
                return NULL;
            }
            if (xml_read_text(x, &ktext, &klen) != 0) return NULL;
            if (xml_read_close_tag(x, "key") != 0) return NULL;
            val = xml_parse_value(x, depth + 1);
            if (!val) return NULL;
            keys[count].ptr = ktext;
            keys[count].len = klen;
            items[count] = *val;
            count++;
        }
        if (count != total) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        v->items = items;
        v->keys = keys;
        v->count = count;
        return v;
    }
    if (ik_strcmp(name, "array") == 0) {
        uint32_t count = 0;
        uint32_t total;
        osser_value *items = NULL;
        if (self_closing) {
            v->type = OSSER_ARRAY;
            v->count = 0;
            return v;
        }
        total = xml_count_array_items(x);
        if (total > x->pool->vals_cap - x->pool->vals_used) {
            x->err = OSSER_ERR_NOSPACE;
            return NULL;
        }
        items = &x->pool->vals[x->pool->vals_used];
        x->pool->vals_used += total;
        v->type = OSSER_ARRAY;
        for (;;) {
            osser_value *child;
            xml_skip_prolog(x);
            if (x->err) return NULL;
            if (x->pos + 8 <= x->len &&
                ik_memeq(x->buf + x->pos, "</array>", 8)) {
                x->pos += 8;
                break;
            }
            if (count >= total) {
                x->err = OSSER_ERR_BAD_XML;
                return NULL;
            }
            child = xml_parse_value(x, depth + 1);
            if (!child) return NULL;
            items[count] = *child;
            count++;
        }
        if (count != total) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        v->items = items;
        v->keys = NULL;
        v->count = count;
        return v;
    }
    if (ik_strcmp(name, "string") == 0 || ik_strcmp(name, "date") == 0) {
        const uint8_t *t = NULL;
        uint32_t tlen = 0;
        if (self_closing) {
            v->type = OSSER_STRING;
            v->bytes = (const uint8_t *)"";
            v->len = 0;
            return v;
        }
        if (xml_read_text(x, &t, &tlen) != 0) return NULL;
        if (xml_read_close_tag(x, name) != 0) return NULL;
        v->type = OSSER_STRING;
        v->bytes = t;
        v->len = tlen;
        return v;
    }
    if (ik_strcmp(name, "integer") == 0) {
        const uint8_t *t = NULL;
        uint32_t tlen = 0;
        int64_t num;
        if (self_closing) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        if (xml_read_text(x, &t, &tlen) != 0) return NULL;
        if (xml_read_close_tag(x, "integer") != 0) return NULL;
        if (xml_parse_int(t, tlen, &num) != 0) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        v->type = OSSER_NUMBER;
        v->num = (uint64_t)num;
        v->num_bits = 64;
        return v;
    }
    if (ik_strcmp(name, "real") == 0) {
        const uint8_t *t = NULL;
        uint32_t tlen = 0;
        double d;
        if (self_closing) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        if (xml_read_text(x, &t, &tlen) != 0) return NULL;
        if (xml_read_close_tag(x, "real") != 0) return NULL;
        if (xml_parse_real(t, tlen, &d) != 0) {
            x->err = OSSER_ERR_BAD_XML;
            return NULL;
        }
        v->type = OSSER_REAL;
        v->dbl = d;
        return v;
    }
    if (ik_strcmp(name, "true") == 0 || ik_strcmp(name, "false") == 0) {
        v->type = OSSER_BOOL;
        v->boolean = (ik_strcmp(name, "true") == 0) ? 1 : 0;
        if (!self_closing) {
            if (xml_read_close_tag(x, name) != 0) return NULL;
        }
        return v;
    }
    if (ik_strcmp(name, "data") == 0) {
        const uint8_t *t = NULL;
        uint32_t tlen = 0;
        const uint8_t *d = NULL;
        uint32_t dlen = 0;
        if (self_closing) {
            v->type = OSSER_DATA;
            v->bytes = (const uint8_t *)"";
            v->len = 0;
            return v;
        }
        if (xml_read_text(x, &t, &tlen) != 0) return NULL;
        if (xml_read_close_tag(x, "data") != 0) return NULL;
        if (xml_b64_decode(x, t, tlen, &d, &dlen) != 0) return NULL;
        v->type = OSSER_DATA;
        v->bytes = d;
        v->len = dlen;
        return v;
    }
    x->err = OSSER_ERR_BAD_XML;
    return NULL;
}

int osser_xml_decode(const uint8_t *buf, uint32_t len, osser_pool *pool,
                     osser_value **root_out) {
    xml_reader x;
    osser_value *root;
    if (!buf || !pool || !root_out || len == 0) return OSSER_ERR_BAD_XML;
    x.buf = buf;
    x.len = len;
    x.pos = 0;
    x.pool = pool;
    x.err = OSSER_ERR_OK;
    root = xml_parse_value(&x, 0);
    if (!root) return x.err ? x.err : OSSER_ERR_BAD_XML;
    *root_out = root;
    return OSSER_ERR_OK;
}

int osser_decode_auto(const uint8_t *buf, uint32_t len, osser_pool *pool,
                      osser_value **root_out) {
    if (!buf || len == 0) return OSSER_ERR_TRUNCATED;
    if (len >= 4 && ik_rd32(buf) == OSSER_SIGNATURE) {
        return osser_decode(buf, len, pool, root_out);
    }
    return osser_xml_decode(buf, len, pool, root_out);
}

int osser_dict_find(const osser_value *dict, const char *key,
                    const osser_value **out) {
    uint32_t i;
    uint32_t klen;
    if (!dict || dict->type != OSSER_DICT || !key || !out) {
        return OSSER_ERR_BAD_TYPE;
    }
    klen = (uint32_t)ik_strlen(key);
    for (i = 0; i < dict->count; i++) {
        if (dict->keys[i].len == klen &&
            ik_memeq(dict->keys[i].ptr, key, klen)) {
            *out = &dict->items[i];
            return OSSER_ERR_OK;
        }
    }
    return OSSER_ERR_NOT_FOUND;
}

int osser_as_uint64(const osser_value *v, uint64_t *out) {
    if (!v || !out) return OSSER_ERR_BAD_TYPE;
    if (v->type == OSSER_NUMBER) {
        *out = v->num;
        return OSSER_ERR_OK;
    }
    if (v->type == OSSER_BOOL) {
        *out = v->boolean ? 1 : 0;
        return OSSER_ERR_OK;
    }
    return OSSER_ERR_BAD_TYPE;
}

int osser_as_int64(const osser_value *v, int64_t *out) {
    if (!v || !out) return OSSER_ERR_BAD_TYPE;
    if (v->type == OSSER_NUMBER) {
        *out = (int64_t)v->num;
        return OSSER_ERR_OK;
    }
    if (v->type == OSSER_BOOL) {
        *out = v->boolean ? 1 : 0;
        return OSSER_ERR_OK;
    }
    return OSSER_ERR_BAD_TYPE;
}

int osser_as_double(const osser_value *v, double *out) {
    if (!v || !out) return OSSER_ERR_BAD_TYPE;
    if (v->type == OSSER_REAL) {
        *out = v->dbl;
        return OSSER_ERR_OK;
    }
    if (v->type == OSSER_NUMBER) {
        *out = (double)v->num;
        return OSSER_ERR_OK;
    }
    return OSSER_ERR_BAD_TYPE;
}

int osser_as_bool(const osser_value *v, int *out) {
    if (!v || !out) return OSSER_ERR_BAD_TYPE;
    if (v->type == OSSER_BOOL) {
        *out = v->boolean;
        return OSSER_ERR_OK;
    }
    if (v->type == OSSER_NUMBER) {
        *out = (v->num != 0) ? 1 : 0;
        return OSSER_ERR_OK;
    }
    return OSSER_ERR_BAD_TYPE;
}

int osser_as_string(const osser_value *v, const uint8_t **out,
                    uint32_t *len_out) {
    if (!v || !out || !len_out) return OSSER_ERR_BAD_TYPE;
    if (v->type != OSSER_STRING) return OSSER_ERR_BAD_TYPE;
    *out = v->bytes;
    *len_out = v->len;
    return OSSER_ERR_OK;
}

/* ---------------- writer ---------------- */

static int osser_w_put(osser_writer *w, const uint8_t *p, uint32_t n);

int osser_w_init(osser_writer *w, uint8_t *buf, uint32_t cap) {
    uint8_t sig[4];
    if (!w || !buf || cap < 4) return OSSER_ERR_BAD_TYPE;
    ik_memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = OSSER_ERR_OK;
    /* Every standalone OSSerializeBinary blob starts with the
     * 0x000000d3 signature; the reader requires it. */
    ik_wr32(sig, OSSER_SIGNATURE);
    if (osser_w_put(w, sig, 4) != OSSER_ERR_OK) {
        return OSSER_ERR_NOSPACE;
    }
    return OSSER_ERR_OK;
}

uint32_t osser_w_len(const osser_writer *w) {
    return w ? w->len : 0;
}

int osser_w_error(const osser_writer *w) {
    return w ? w->err : OSSER_ERR_BAD_TYPE;
}

static int osser_w_put(osser_writer *w, const uint8_t *p, uint32_t n) {
    if (w->err) return w->err;
    if (n > w->cap - w->len) {
        w->err = OSSER_ERR_NOSPACE;
        return w->err;
    }
    ik_memcpy(w->buf + w->len, p, n);
    w->len += n;
    return OSSER_ERR_OK;
}

static int osser_w_put32(osser_writer *w, uint32_t v) {
    uint8_t b[4];
    ik_wr32(b, v);
    return osser_w_put(w, b, 4);
}

static int osser_w_put64(osser_writer *w, uint64_t v) {
    uint8_t b[8];
    ik_wr64(b, v);
    return osser_w_put(w, b, 8);
}

static int osser_w_put_pad(osser_writer *w, uint32_t n) {
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t pad = (4u - (n & 3u)) & 3u;
    if (pad == 0) return OSSER_ERR_OK;
    return osser_w_put(w, zeros, pad);
}

static int osser_w_put_tag(osser_writer *w, uint32_t kind, uint32_t count,
                           int end_bit) {
    uint32_t tag = (kind << 24) | (count & 0x00ffffffu);
    if (end_bit) tag |= OSSER_TAG_END;
    return osser_w_put32(w, tag);
}

/* Is the value about to be written the last value of its parent
 * collection (or the root)? Decides the EndCollection bit. */
static int osser_w_is_last(osser_writer *w) {
    if (w->depth == 0) return 1;
    if (w->stack[w->depth - 1].kind == OSSER_ARRAY) {
        return w->stack[w->depth - 1].remaining == 1;
    }
    return w->stack[w->depth - 1].remaining == 1 &&
           w->stack[w->depth - 1].need_value != 0;
}

/* A value was completed at the current level: consume one slot of
 * the parent collection, or mark the root done. */
static void osser_w_value_done(osser_writer *w) {
    if (w->depth == 0) {
        w->root_done = 1;
        return;
    }
    if (w->stack[w->depth - 1].remaining > 0) {
        w->stack[w->depth - 1].remaining--;
    }
    if (w->stack[w->depth - 1].kind == OSSER_DICT) {
        w->stack[w->depth - 1].need_value = 0;
    }
}

/* Check that a value may be written at the current level. */
static int osser_w_value_ok(osser_writer *w) {
    if (w->err) return w->err;
    if (w->root_done) {
        w->err = OSSER_ERR_OVERFLOW;
        return w->err;
    }
    if (w->depth == 0) return OSSER_ERR_OK;
    if (w->stack[w->depth - 1].kind == OSSER_DICT &&
        !w->stack[w->depth - 1].need_value) {
        w->err = OSSER_ERR_BAD_TYPE;
        return w->err;
    }
    if (w->stack[w->depth - 1].remaining == 0) {
        w->err = OSSER_ERR_OVERFLOW;
        return w->err;
    }
    return OSSER_ERR_OK;
}

static int osser_w_begin(osser_writer *w, uint32_t kind, uint32_t count) {
    int end_bit;
    if (osser_w_value_ok(w) != OSSER_ERR_OK) return w->err;
    if (w->depth >= OSSER_MAX_DEPTH) {
        w->err = OSSER_ERR_OVERFLOW;
        return w->err;
    }
    end_bit = osser_w_is_last(w);
    if (osser_w_put_tag(w, kind, count, end_bit) != OSSER_ERR_OK) {
        return w->err;
    }
    w->stack[w->depth].kind = kind;
    w->stack[w->depth].remaining = count;
    w->stack[w->depth].need_value = 0;
    w->depth++;
    return OSSER_ERR_OK;
}

static int osser_w_end(osser_writer *w, uint32_t kind) {
    if (w->err) return w->err;
    if (w->depth == 0 || w->stack[w->depth - 1].kind != kind ||
        w->stack[w->depth - 1].remaining != 0) {
        w->err = OSSER_ERR_BAD_TYPE;
        return w->err;
    }
    w->depth--;
    osser_w_value_done(w);
    return OSSER_ERR_OK;
}

int osser_w_begin_dict(osser_writer *w, uint32_t count) {
    if (!w) return OSSER_ERR_BAD_TYPE;
    return osser_w_begin(w, OSSER_DICT, count);
}

int osser_w_end_dict(osser_writer *w) {
    if (!w) return OSSER_ERR_BAD_TYPE;
    return osser_w_end(w, OSSER_DICT);
}

int osser_w_begin_array(osser_writer *w, uint32_t count) {
    if (!w) return OSSER_ERR_BAD_TYPE;
    return osser_w_begin(w, OSSER_ARRAY, count);
}

int osser_w_end_array(osser_writer *w) {
    if (!w) return OSSER_ERR_BAD_TYPE;
    return osser_w_end(w, OSSER_ARRAY);
}

int osser_w_key_bytes(osser_writer *w, const uint8_t *p, uint32_t len) {
    if (!w || !p) return OSSER_ERR_BAD_TYPE;
    if (w->err) return w->err;
    if (w->root_done || w->depth == 0 ||
        w->stack[w->depth - 1].kind != OSSER_DICT ||
        w->stack[w->depth - 1].need_value ||
        w->stack[w->depth - 1].remaining == 0) {
        w->err = OSSER_ERR_BAD_TYPE;
        return w->err;
    }
    /* Keys never carry the EndCollection bit. */
    if (osser_w_put_tag(w, OSSER_TAG_SYM, len, 0) != OSSER_ERR_OK) {
        return w->err;
    }
    if (osser_w_put(w, p, len) != OSSER_ERR_OK) return w->err;
    if (osser_w_put_pad(w, len) != OSSER_ERR_OK) return w->err;
    w->stack[w->depth - 1].need_value = 1;
    return OSSER_ERR_OK;
}

int osser_w_key(osser_writer *w, const char *key) {
    if (!key) return OSSER_ERR_BAD_TYPE;
    return osser_w_key_bytes(w, (const uint8_t *)key,
                             (uint32_t)ik_strlen(key));
}

static int osser_w_string_like(osser_writer *w, uint32_t kind,
                               const uint8_t *p, uint32_t len) {
    int end_bit;
    if (osser_w_value_ok(w) != OSSER_ERR_OK) return w->err;
    end_bit = osser_w_is_last(w);
    if (osser_w_put_tag(w, kind, len, end_bit) != OSSER_ERR_OK) {
        return w->err;
    }
    if (osser_w_put(w, p, len) != OSSER_ERR_OK) return w->err;
    if (osser_w_put_pad(w, len) != OSSER_ERR_OK) return w->err;
    osser_w_value_done(w);
    return OSSER_ERR_OK;
}

int osser_w_string(osser_writer *w, const char *s) {
    if (!w || !s) return OSSER_ERR_BAD_TYPE;
    return osser_w_string_like(w, OSSER_TAG_STR, (const uint8_t *)s,
                               (uint32_t)ik_strlen(s));
}

int osser_w_bytes(osser_writer *w, const uint8_t *p, uint32_t len) {
    if (!w || !p) return OSSER_ERR_BAD_TYPE;
    return osser_w_string_like(w, OSSER_TAG_STR, p, len);
}

int osser_w_u32(osser_writer *w, uint32_t v) {
    int end_bit;
    if (!w) return OSSER_ERR_BAD_TYPE;
    if (osser_w_value_ok(w) != OSSER_ERR_OK) return w->err;
    end_bit = osser_w_is_last(w);
    if (osser_w_put_tag(w, OSSER_TAG_NUM, 32, end_bit) != OSSER_ERR_OK) {
        return w->err;
    }
    if (osser_w_put32(w, v) != OSSER_ERR_OK) return w->err;
    osser_w_value_done(w);
    return OSSER_ERR_OK;
}

int osser_w_u64(osser_writer *w, uint64_t v) {
    int end_bit;
    if (!w) return OSSER_ERR_BAD_TYPE;
    if (osser_w_value_ok(w) != OSSER_ERR_OK) return w->err;
    end_bit = osser_w_is_last(w);
    if (osser_w_put_tag(w, OSSER_TAG_NUM, 64, end_bit) != OSSER_ERR_OK) {
        return w->err;
    }
    if (osser_w_put64(w, v) != OSSER_ERR_OK) return w->err;
    osser_w_value_done(w);
    return OSSER_ERR_OK;
}

int osser_w_s64(osser_writer *w, int64_t v) {
    return osser_w_u64(w, (uint64_t)v);
}

int osser_w_bool(osser_writer *w, int v) {
    int end_bit;
    if (!w) return OSSER_ERR_BAD_TYPE;
    if (osser_w_value_ok(w) != OSSER_ERR_OK) return w->err;
    end_bit = osser_w_is_last(w);
    if (osser_w_put_tag(w, OSSER_TAG_BOOL, v ? 1 : 0, end_bit) !=
        OSSER_ERR_OK) {
        return w->err;
    }
    osser_w_value_done(w);
    return OSSER_ERR_OK;
}

int osser_w_data(osser_writer *w, const uint8_t *p, uint32_t len) {
    if (!w || !p) return OSSER_ERR_BAD_TYPE;
    return osser_w_string_like(w, OSSER_TAG_DATA, p, len);
}

int osser_w_value(osser_writer *w, const osser_value *v) {
    uint32_t i;
    if (!w || !v) {
        if (w) w->err = OSSER_ERR_BAD_TYPE;
        return OSSER_ERR_BAD_TYPE;
    }
    if (w->err) return w->err;
    if (v->type == OSSER_DICT) {
        if (osser_w_begin_dict(w, v->count) != OSSER_ERR_OK) {
            return w->err;
        }
        for (i = 0; i < v->count; i++) {
            if (osser_w_key_bytes(w, v->keys[i].ptr,
                                  v->keys[i].len) != OSSER_ERR_OK) {
                return w->err;
            }
            if (osser_w_value(w, &v->items[i]) != OSSER_ERR_OK) {
                return w->err;
            }
        }
        return osser_w_end_dict(w);
    }
    if (v->type == OSSER_ARRAY) {
        if (osser_w_begin_array(w, v->count) != OSSER_ERR_OK) {
            return w->err;
        }
        for (i = 0; i < v->count; i++) {
            if (osser_w_value(w, &v->items[i]) != OSSER_ERR_OK) {
                return w->err;
            }
        }
        return osser_w_end_array(w);
    }
    if (v->type == OSSER_STRING) {
        return osser_w_bytes(w, v->bytes, v->len);
    }
    if (v->type == OSSER_NUMBER) {
        if (v->num_bits == 32) return osser_w_u32(w, (uint32_t)v->num);
        return osser_w_u64(w, v->num);
    }
    if (v->type == OSSER_BOOL) return osser_w_bool(w, v->boolean);
    if (v->type == OSSER_DATA) return osser_w_data(w, v->bytes, v->len);
    /* OSSER_REAL has no binary form: record the failure in w->err,
     * because the dict/array parents above return w->err (not the
     * child's return value), so a bare return here used to report
     * success with a truncated blob. */
    w->err = OSSER_ERR_BAD_TYPE;
    return OSSER_ERR_BAD_TYPE;
}

/* ------------------------------------------------------------------ */
/* MIG request parsing and reply building.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const mach_msg_header_t *hdr;
    uint32_t id;
    mach_port_name_t dest;
    mach_port_name_t reply;
    uint32_t n_ports;
    mach_port_name_t ports[4];
    uint32_t port_disp[4];
    uint32_t n_ools;
    uint64_t ool_addr[2];
    uint32_t ool_size[2];
    const uint8_t *body; /* first byte after the NDR record */
    uint32_t body_len;
    int err;
} mig_req;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int err;
} mig_cur;

static void mig_cur_init(mig_cur *c, const uint8_t *p, uint32_t len) {
    c->p = p;
    c->end = p + len;
    c->err = 0;
}

static uint32_t mig_cur_u32(mig_cur *c) {
    uint32_t v;
    if (c->err) return 0;
    if ((uint32_t)(c->end - c->p) < 4) {
        c->err = 1;
        return 0;
    }
    v = ik_rd32(c->p);
    c->p += 4;
    return v;
}

static uint64_t mig_cur_u64(mig_cur *c) {
    uint64_t v;
    if (c->err) return 0;
    if ((uint32_t)(c->end - c->p) < 8) {
        c->err = 1;
        return 0;
    }
    v = ik_rd64(c->p);
    c->p += 8;
    return v;
}

/* Read count bytes, advancing past them plus 4-byte padding.
 * Returns NULL on truncation. */
static const uint8_t *mig_cur_bytes(mig_cur *c, uint32_t n) {
    const uint8_t *p;
    uint32_t pad;
    if (c->err) return NULL;
    if (n > (uint32_t)(c->end - c->p)) {
        c->err = 1;
        return NULL;
    }
    p = c->p;
    c->p += n;
    pad = (4u - (n & 3u)) & 3u;
    if (pad > (uint32_t)(c->end - c->p)) {
        c->err = 1;
        return NULL;
    }
    c->p += pad;
    return p;
}

/* Read a counted string (io_string_t / io_name_t): u32 count
 * (including the terminating NUL) + count bytes. Copies into out
 * (NUL terminated, truncated to cap-1). Returns 0 on success. */
static int mig_cur_string(mig_cur *c, char *out, uint32_t cap) {
    uint32_t count = mig_cur_u32(c);
    const uint8_t *p;
    uint32_t copy;
    if (c->err) return -1;
    if (count == 0) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    if (count > (uint32_t)(c->end - c->p)) {
        c->err = 1;
        return -1;
    }
    p = c->p;
    c->p += count;
    copy = count;
    if (copy > cap - 1) copy = cap - 1;
    ik_memcpy(out, p, copy);
    out[copy] = '\0';
    return 0;
}

/* Parse the incoming message into a mig_req. Header (24 bytes),
 * optional descriptor body, NDR record (8 bytes, accepted
 * unchecked), then the routine fields at body/body_len. */
static void mig_req_parse(const mach_msg_header_t *hdr, uint32_t send_size,
                          mig_req *req) {
    const uint8_t *p = (const uint8_t *)hdr;
    uint32_t pos = 24;
    ik_memset(req, 0, sizeof(*req));
    req->hdr = hdr;
    req->err = 0;
    if (send_size < 24) {
        req->err = 1;
        return;
    }
    req->id = hdr->msgh_id;
    req->dest = hdr->msgh_remote_port;
    req->reply = hdr->msgh_local_port;
    if (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        uint32_t ndesc;
        uint32_t i;
        if (send_size < pos + 4) {
            req->err = 1;
            return;
        }
        ndesc = ik_rd32(p + pos);
        pos += 4;
        if (ndesc > 8) {
            req->err = 1;
            return;
        }
        /* Descriptor layout is the 64-bit Mach layout (xnu
         * osfmk/mach/message.h, mach_msg_ool_descriptor64_t, and
         * the in-tree ipc.h structs, which agree): the type byte
         * sits at offset 11 in BOTH kinds, so one read tells the
         * kind apart. A port descriptor is 12 bytes (name u32
         * @0, pad1 u32 @4, pad2 u16 @8, disposition @10, type
         * @11). An OOL descriptor is 16 bytes (address u64 @0,
         * deallocate @8, copy @9, pad1 @10, type @11, size u32
         * @12); OOL-ports carries a count at @12 instead of a
         * size. The earlier version read the type at +15 and the
         * port disposition at +14 and advanced 16 bytes for
         * every descriptor: a real port descriptor then pushed
         * the body pointer 4 bytes late, and an OOL descriptor
         * was only recognised when its size's top byte (at +15)
         * happened to be 1, i.e. size >= 0x01000000 (found by
         * the host test on 2026-11-12). */
        for (i = 0; i < ndesc; i++) {
            uint8_t type;
            if (send_size < pos + 12) {
                req->err = 1;
                return;
            }
            type = p[pos + 11];
            if (type == MACH_MSG_PORT_DESCRIPTOR) {
                if (req->n_ports < 4) {
                    req->ports[req->n_ports] = ik_rd32(p + pos);
                    req->port_disp[req->n_ports] = p[pos + 10];
                    req->n_ports++;
                }
                pos += 12;
            } else if (type == MACH_MSG_OOL_DESCRIPTOR ||
                       type == MACH_MSG_OOL_PORTS_DESCRIPTOR ||
                       type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) {
                if (send_size < pos + 16) {
                    req->err = 1;
                    return;
                }
                if (req->n_ools < 2) {
                    req->ool_addr[req->n_ools] = ik_rd64(p + pos);
                    req->ool_size[req->n_ools] = ik_rd32(p + pos + 12);
                    req->n_ools++;
                }
                pos += 16;
            } else {
                /* Unknown descriptor type: fail closed. */
                req->err = 1;
                return;
            }
        }
    }
    if (send_size < pos + 8) {
        req->err = 1;
        return;
    }
    pos += 8; /* NDR record, accepted unchecked */
    req->body = p + pos;
    req->body_len = send_size - pos;
}

/* ---------------- reply builder ---------------- */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t retcode;
    uint32_t n_descs;
    uint32_t desc_size; /* bytes per descriptor slot: 12 (port) or 16 (OOL) */
    int err;
} mig_rep;

static void mig_rep_init(mig_rep *rep, uint8_t *buf, uint32_t cap,
                         const mig_req *req) {
    ik_memset(rep, 0, sizeof(*rep));
    rep->buf = buf;
    rep->cap = cap;
    rep->retcode = KERN_SUCCESS;
    if (cap < 36) {
        rep->err = 1;
        return;
    }
    /* Header (24): bits and size are set at finish. */
    ik_wr32(buf + 0, 0);
    ik_wr32(buf + 4, 0);
    ik_wr32(buf + 8, req->reply);
    ik_wr32(buf + 12, 0);
    ik_wr32(buf + 16, 0);
    ik_wr32(buf + 20, req->id + 100); /* reply id = request + 100 [guess] */
    /* NDR record (8): mig_vers 0, if_vers 0, reserved1 0,
     * mig_encoding 0, int_rep 1 (LE), char_rep 0, float_rep 0,
     * reserved2 0 [guess: standard NDR values]. */
    buf[24] = 0;
    buf[25] = 0;
    buf[26] = 0;
    buf[27] = 0;
    buf[28] = 1;
    buf[29] = 0;
    buf[30] = 0;
    buf[31] = 0;
    /* RetCode placeholder (4). */
    ik_wr32(buf + 32, 0);
    rep->len = 36;
}

/* Reserve room for n_descs descriptors between the header and the
 * NDR record. Must be called before any payload is written: it
 * memmoves the NDR + RetCode forward. desc_size is the slot size
 * in bytes: 12 for a port descriptor, 16 for an OOL descriptor.
 * All descriptors in one reply currently share one size (every
 * reply carries at most one descriptor). */
static int mig_rep_begin_complex(mig_rep *rep, uint32_t n,
                                 uint32_t desc_size) {
    uint32_t shift;
    if (rep->err) return rep->err;
    if (n == 0) return 0;
    if (desc_size != 12 && desc_size != 16) {
        rep->err = 1;
        return rep->err;
    }
    shift = 4u + desc_size * n;
    if (shift > rep->cap - rep->len) {
        rep->err = 1;
        return rep->err;
    }
    ik_memmove(rep->buf + 24 + shift, rep->buf + 24, 12);
    ik_wr32(rep->buf + 24, n);
    rep->n_descs = n;
    rep->desc_size = desc_size;
    rep->len += shift;
    return 0;
}

static int mig_rep_port_desc(mig_rep *rep, uint32_t idx,
                             mach_port_name_t name, uint32_t disp) {
    uint8_t *d;
    if (rep->err) return rep->err;
    if (idx >= rep->n_descs) {
        rep->err = 1;
        return rep->err;
    }
    d = rep->buf + 28 + rep->desc_size * idx;
    ik_wr32(d + 0, name);
    ik_wr32(d + 4, 0);   /* pad1 */
    ik_wr16(d + 8, 0);   /* pad2 */
    d[10] = (uint8_t)disp;
    d[11] = MACH_MSG_PORT_DESCRIPTOR;
    return 0;
}

static int mig_rep_ool_desc(mig_rep *rep, uint32_t idx, uint64_t addr,
                            uint32_t size, uint8_t dealloc, uint8_t copy) {
    uint8_t *d;
    if (rep->err) return rep->err;
    if (idx >= rep->n_descs) {
        rep->err = 1;
        return rep->err;
    }
    d = rep->buf + 28 + rep->desc_size * idx;
    ik_wr64(d + 0, addr);
    d[8] = dealloc;
    d[9] = copy;
    d[10] = 0; /* pad1 */
    d[11] = MACH_MSG_OOL_DESCRIPTOR;
    ik_wr32(d + 12, size);
    return 0;
}

static int mig_rep_put(mig_rep *rep, const void *p, uint32_t n) {
    if (rep->err) return rep->err;
    if (n > rep->cap - rep->len) {
        rep->err = 1;
        return rep->err;
    }
    ik_memcpy(rep->buf + rep->len, p, n);
    rep->len += n;
    return 0;
}

static int mig_rep_u32(mig_rep *rep, uint32_t v) {
    uint8_t b[4];
    ik_wr32(b, v);
    return mig_rep_put(rep, b, 4);
}

static int mig_rep_u64(mig_rep *rep, uint64_t v) {
    uint8_t b[8];
    ik_wr64(b, v);
    return mig_rep_put(rep, b, 8);
}

static int mig_rep_bytes(mig_rep *rep, const void *p, uint32_t n) {
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t pad = (4u - (n & 3u)) & 3u;
    if (mig_rep_put(rep, p, n) != 0) return rep->err;
    if (pad > 0 && mig_rep_put(rep, zeros, pad) != 0) return rep->err;
    return 0;
}

/* Counted string: u32 count (including NUL) + bytes + NUL + pad. */
static int mig_rep_string(mig_rep *rep, const char *s) {
    uint32_t len = (uint32_t)ik_strlen(s);
    if (mig_rep_u32(rep, len + 1) != 0) return rep->err;
    if (mig_rep_put(rep, s, len) != 0) return rep->err;
    {
        uint8_t nul = 0;
        if (mig_rep_put(rep, &nul, 1) != 0) return rep->err;
    }
    {
        uint32_t total = len + 1;
        uint32_t pad = (4u - (total & 3u)) & 3u;
        static const uint8_t zeros[4] = {0, 0, 0, 0};
        if (pad > 0 && mig_rep_put(rep, zeros, pad) != 0) {
            return rep->err;
        }
    }
    return 0;
}

static void mig_rep_finish(mig_rep *rep) {
    uint32_t ret_off;
    if (rep->err) return;
    ret_off = 24 + (rep->n_descs ? (4u + rep->desc_size * rep->n_descs) : 0) + 8;
    ik_wr32(rep->buf + ret_off, rep->retcode);
    ik_wr32(rep->buf + 4, rep->len);
    ik_wr32(rep->buf + 0,
            MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) |
                (rep->n_descs ? MACH_MSGH_BITS_COMPLEX : 0));
}

/* Build a minimal error reply (header + NDR + RetCode, 36 bytes). */
static void mig_rep_error(mig_rep *rep, uint8_t *buf, uint32_t cap,
                          const mig_req *req, uint32_t retcode) {
    mig_rep_init(rep, buf, cap, req);
    if (rep->err) return;
    rep->retcode = retcode;
    mig_rep_finish(rep);
}

/* ---------------- shared handler helpers ---------------- */

/* Class name of an object (service / connection / iterator). */
static const char *ik_obj_class(int kind, int idx) {
    if (kind == 0) return g_services[idx].class_name;
    if (kind == 1) {
        int s = (int)g_conns[idx].service_idx;
        if (s >= 0 && s < IOKIT_MAX_SERVICES && g_services[s].in_use) {
            return g_services[s].class_name;
        }
        return "IOUserClient";
    }
    return "IOIterator";
}

static uint32_t ik_obj_refcount(int kind, int idx) {
    if (kind == 0) return g_services[idx].refcount;
    if (kind == 1) return g_conns[idx].refcount;
    return g_iters[idx].refcount;
}

/* Reply with one port descriptor (MOVE_SEND) and no payload. */
static void ik_reply_port(mig_rep *rep, mach_port_name_t name) {
    if (mig_rep_begin_complex(rep, 1, 12) != 0) return;
    (void)mig_rep_port_desc(rep, 0, name, MACH_MSG_TYPE_MOVE_SEND);
}

/* Reply with one OOL descriptor pointing at data already copied
 * into g_ool_scratch. copy=1 (physical copy), deallocate=0. */
static void ik_reply_ool(mig_rep *rep, uint32_t size) {
    if (mig_rep_begin_complex(rep, 1, 16) != 0) return;
    (void)mig_rep_ool_desc(rep, 0, (uint64_t)(uintptr_t)g_ool_scratch,
                           size, 0, 1);
}

/* Decode a service's property blob into the given pool. Returns
 * the root dict or NULL. */
static osser_value *ik_props_decode(int svc_idx, osser_pool *pool) {
    osser_value *root = NULL;
    if (g_services[svc_idx].props_len == 0) return NULL;
    if (osser_decode(g_services[svc_idx].props,
                     g_services[svc_idx].props_len, pool, &root) !=
        OSSER_ERR_OK) {
        return NULL;
    }
    if (!root || root->type != OSSER_DICT) return NULL;
    return root;
}

/* Encode one decoded value into g_ool_scratch. Returns the byte
 * length, or 0 on failure (the caller then answers an error). */
static uint32_t ik_ool_encode_value(const osser_value *v) {
    osser_writer w;
    if (osser_w_init(&w, g_ool_scratch, sizeof(g_ool_scratch)) !=
        OSSER_ERR_OK) {
        return 0;
    }
    if (osser_w_value(&w, v) != OSSER_ERR_OK) return 0;
    return osser_w_len(&w);
}

/* Look a property value up on a service and encode it into
 * g_ool_scratch. Returns the encoded length, or 0 when the key is
 * missing or cannot be encoded. */
static uint32_t ik_ool_encode_property(int svc_idx, const char *key) {
    osser_pool pool;
    osser_value *root;
    const osser_value *v = NULL;
    ik_scratch_pool(&pool);
    root = ik_props_decode(svc_idx, &pool);
    if (!root) return 0;
    if (osser_dict_find(root, key, &v) != OSSER_ERR_OK) return 0;
    return ik_ool_encode_value(v);
}

/* Property-table match: every entry of dict must hold on the
 * service. IOProviderClass / IOClass / IONameMatch entries are
 * class checks; every other entry is a property equality check. */
static int ik_table_matches(int svc_idx, const osser_value *dict) {
    osser_pool pool;
    osser_value *props = NULL;
    uint32_t i;
    if (!dict || dict->type != OSSER_DICT) return 0;
    if (!ik_service_matches(svc_idx, dict)) return 0;
    ik_scratch2_pool(&pool);
    props = ik_props_decode(svc_idx, &pool);
    if (!props) return 0;
    for (i = 0; i < dict->count; i++) {
        char keybuf[64];
        const osser_value *have = NULL;
        uint32_t klen = dict->keys[i].len;
        if (klen == 15 && ik_memeq(dict->keys[i].ptr, "IOProviderClass", 15)) {
            continue;
        }
        if (klen == 7 && ik_memeq(dict->keys[i].ptr, "IOClass", 7)) {
            continue;
        }
        if (klen == 11 && ik_memeq(dict->keys[i].ptr, "IONameMatch", 11)) {
            continue;
        }
        if (klen >= sizeof(keybuf)) return 0;
        ik_memcpy(keybuf, dict->keys[i].ptr, klen);
        keybuf[klen] = '\0';
        if (osser_dict_find(props, keybuf, &have) != OSSER_ERR_OK ||
            !ik_value_equal(have, &dict->items[i])) {
            return 0;
        }
    }
    return 1;
}

static uint8_t g_merge_blob[IOKIT_MAX_PROPS];

/* Merge a decoded dict into a service's property blob: entries in
 * the new dict overwrite same-named entries; other entries are
 * kept. Returns KERN_SUCCESS or a KERN_* code. */
static int ik_merge_props(int svc_idx, const osser_value *dict) {
    osser_pool pool;
    osser_value *root = NULL;
    osser_writer w;
    uint32_t i;
    uint32_t j;
    uint32_t count;
    ik_scratch_pool(&pool);
    root = ik_props_decode(svc_idx, &pool);
    if (osser_w_init(&w, g_merge_blob, sizeof(g_merge_blob)) !=
        OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    /* Count the merged entries: old entries plus new keys that are
     * not already present. */
    count = root ? root->count : 0;
    for (i = 0; i < dict->count; i++) {
        char keybuf[64];
        const osser_value *found = NULL;
        if (dict->keys[i].len >= sizeof(keybuf)) {
            return KERN_INVALID_ARGUMENT;
        }
        ik_memcpy(keybuf, dict->keys[i].ptr, dict->keys[i].len);
        keybuf[dict->keys[i].len] = '\0';
        if (!root ||
            osser_dict_find(root, keybuf, &found) != OSSER_ERR_OK) {
            count++;
        }
    }
    if (osser_w_begin_dict(&w, count) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    if (root) {
        for (i = 0; i < root->count; i++) {
            char keybuf[64];
            const osser_value *nv = NULL;
            if (root->keys[i].len >= sizeof(keybuf)) {
                return KERN_INVALID_ARGUMENT;
            }
            ik_memcpy(keybuf, root->keys[i].ptr, root->keys[i].len);
            keybuf[root->keys[i].len] = '\0';
            if (osser_w_key_bytes(&w, root->keys[i].ptr,
                                  root->keys[i].len) != OSSER_ERR_OK) {
                return KERN_INVALID_ARGUMENT;
            }
            if (osser_dict_find(dict, keybuf, &nv) == OSSER_ERR_OK) {
                if (osser_w_value(&w, nv) != OSSER_ERR_OK) {
                    return KERN_INVALID_ARGUMENT;
                }
            } else {
                if (osser_w_value(&w, &root->items[i]) != OSSER_ERR_OK) {
                    return KERN_INVALID_ARGUMENT;
                }
            }
        }
    }
    for (i = 0; i < dict->count; i++) {
        char keybuf[64];
        const osser_value *found = NULL;
        if (dict->keys[i].len >= sizeof(keybuf)) {
            return KERN_INVALID_ARGUMENT;
        }
        ik_memcpy(keybuf, dict->keys[i].ptr, dict->keys[i].len);
        keybuf[dict->keys[i].len] = '\0';
        if (root &&
            osser_dict_find(root, keybuf, &found) == OSSER_ERR_OK) {
            continue; /* already written above */
        }
        if (osser_w_key_bytes(&w, dict->keys[i].ptr,
                              dict->keys[i].len) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
        if (osser_w_value(&w, &dict->items[i]) != OSSER_ERR_OK) {
            return KERN_INVALID_ARGUMENT;
        }
    }
    if (osser_w_end_dict(&w) != OSSER_ERR_OK) {
        return KERN_INVALID_ARGUMENT;
    }
    (void)j;
    g_services[svc_idx].props_len = osser_w_len(&w);
    ik_memcpy(g_services[svc_idx].props, g_merge_blob,
              g_services[svc_idx].props_len);
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Routine handlers. Each sets rep->retcode and writes its payload.   */
/* A handler that cannot parse its fields answers                     */
/* KERN_INVALID_ARGUMENT. Routines v1 does not implement answer      */
/* kIOReturnUnsupported.                                              */
/* ------------------------------------------------------------------ */

static void ik_h_unsupported(mig_req *req, mig_rep *rep) {
    (void)req;
    rep->retcode = kIOReturnUnsupported;
}

static void ik_h_object_get_class(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_string(rep, ik_obj_class(kind, idx));
}

static void ik_h_object_conforms_to(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char name[IOKIT_CLASS_MAX];
    int conforms = 0;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, name, sizeof(name)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (ik_strcmp(name, ik_obj_class(kind, idx)) == 0) {
        conforms = 1;
    } else if (ik_strcmp(name, "IORegistryEntry") == 0 ||
               ik_strcmp(name, "IOService") == 0 ||
               ik_strcmp(name, "IOObject") == 0) {
        /* [guess] superclass chain: every service is an
         * IORegistryEntry, an IOService and an IOObject. */
        if (kind == 0) conforms = 1;
    } else if (ik_strcmp(name, "IOUserClient") == 0 && kind == 1) {
        conforms = 1;
    } else if (ik_strcmp(name, "IOIterator") == 0 && kind == 2) {
        conforms = 1;
    }
    (void)mig_rep_u32(rep, conforms ? 1 : 0);
}

static void ik_h_iterator_next(mig_req *req, mig_rep *rep) {
    int idx = ik_find_iter_by_port(req->dest);
    mach_port_name_t out = MACH_PORT_NULL;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (g_iters[idx].cursor < g_iters[idx].count) {
        uint32_t svc = g_iters[idx].matches[g_iters[idx].cursor++];
        out = g_services[svc].port;
    }
    ik_reply_port(rep, out);
}

static void ik_h_iterator_reset(mig_req *req, mig_rep *rep) {
    int idx = ik_find_iter_by_port(req->dest);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    g_iters[idx].cursor = 0;
}

static void ik_h_iterator_is_valid(mig_req *req, mig_rep *rep) {
    int idx = ik_find_iter_by_port(req->dest);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_u32(rep, (g_iters[idx].cursor < g_iters[idx].count)
                               ? 1
                               : 0);
}

/* Build an iterator from a decoded matching dict and reply with
 * its port. Returns 0 on success. */
static int ik_reply_matching_iterator(mig_rep *rep, const osser_value *dict) {
    uint32_t matches[IOKIT_MAX_SERVICES];
    uint32_t count = ik_build_matches(dict, matches, IOKIT_MAX_SERVICES);
    mach_port_name_t port = ik_new_iterator(matches, count);
    if (port == MACH_PORT_NULL) return KERN_NO_SPACE;
    ik_reply_port(rep, port);
    return 0;
}

static void ik_h_service_get_matching_services(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char xml[IOKIT_MAX_STRING];
    osser_pool pool;
    osser_value *root = NULL;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, xml, sizeof(xml)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    if (osser_decode_auto((const uint8_t *)xml,
                          (uint32_t)ik_strlen(xml), &pool, &root) !=
        OSSER_ERR_OK) {
        rep->retcode = kIOReturnBadArgument;
        return;
    }
    if (ik_reply_matching_iterator(rep, root) != 0) {
        rep->retcode = KERN_NO_SPACE;
    }
}

/* Decode a matching dict from an OOL descriptor (index 0). Returns
 * the root or NULL (rep->retcode set by the caller on failure). */
static osser_value *ik_matching_from_ool(mig_req *req, osser_pool *pool) {
    osser_value *root = NULL;
    const uint8_t *blob;
    if (req->n_ools < 1 || req->ool_size[0] == 0 ||
        req->ool_size[0] > IOKIT_OOL_SCRATCH_SIZE) {
        return NULL;
    }
    blob = (const uint8_t *)(uintptr_t)req->ool_addr[0];
    if (osser_decode_auto(blob, req->ool_size[0], pool, &root) !=
        OSSER_ERR_OK) {
        return NULL;
    }
    return root;
}

static void ik_h_service_get_matching_services_ool(mig_req *req,
                                                   mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    osser_pool pool;
    osser_value *root;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_matching_from_ool(req, &pool);
    if (!root) {
        rep->retcode = kIOReturnBadArgument;
        return;
    }
    if (ik_reply_matching_iterator(rep, root) != 0) {
        rep->retcode = KERN_NO_SPACE;
    }
}

static void ik_h_service_get_matching_service_bin(mig_req *req,
                                                  mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    osser_pool pool;
    osser_value *root;
    uint32_t matches[IOKIT_MAX_SERVICES];
    uint32_t count;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_matching_from_ool(req, &pool);
    if (!root) {
        rep->retcode = kIOReturnBadArgument;
        return;
    }
    count = ik_build_matches(root, matches, IOKIT_MAX_SERVICES);
    if (count == 0) {
        /* [guess] the singular call errors when nothing matches;
         * the plural calls answer with an empty iterator. */
        rep->retcode = kIOReturnNotFound;
        return;
    }
    ik_reply_port(rep, g_services[matches[0]].port);
}

/* Shared property lookup for 2805 / 2873 / 2855: encode the named
 * property of a service into g_ool_scratch and reply with the OOL
 * descriptor. */
static void ik_h_registry_get_property_named(int svc_idx, const char *name,
                                             mig_rep *rep) {
    uint32_t len;
    /* ik_ool_encode_property returns 0 both when the key is missing
     * and when the value cannot be encoded (> 8 KB); registry
     * properties are small, so 0 here means "not found" in v1. */
    len = ik_ool_encode_property(svc_idx, name);
    if (len == 0) {
        rep->retcode = kIOReturnNotFound;
        return;
    }
    ik_reply_ool(rep, len);
}

static void ik_h_registry_get_property(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char name[IOKIT_MAX_NAME];
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, name, sizeof(name)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_h_registry_get_property_named(idx, name, rep);
}

static void ik_h_registry_get_property_bin(mig_req *req, mig_rep *rep) {
    /* Same reply shape as 2805; the binary form is the same OOL
     * blob. */
    ik_h_registry_get_property(req, rep);
}

static void ik_h_registry_get_property_recursively(mig_req *req,
                                                   mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char plane[IOKIT_MAX_NAME];
    char name[IOKIT_MAX_NAME];
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, plane, sizeof(plane)) != 0 ||
        mig_cur_string(&cur, name, sizeof(name)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_cur_u32(&cur); /* options, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* Plane is ignored in v1: the property is looked up on the
     * entry itself, same as 2805. */
    (void)plane;
    ik_h_registry_get_property_named(idx, name, rep);
}

static void ik_h_registry_get_properties(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    uint32_t len;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (g_services[idx].props_len > 0) {
        ik_memcpy(g_ool_scratch, g_services[idx].props,
                  g_services[idx].props_len);
        len = g_services[idx].props_len;
    } else {
        osser_writer w;
        if (osser_w_init(&w, g_ool_scratch, sizeof(g_ool_scratch)) !=
                OSSER_ERR_OK ||
            osser_w_begin_dict(&w, 0) != OSSER_ERR_OK ||
            osser_w_end_dict(&w) != OSSER_ERR_OK) {
            rep->retcode = KERN_NO_SPACE;
            return;
        }
        len = osser_w_len(&w);
    }
    ik_reply_ool(rep, len);
}

static void ik_h_registry_get_property_bytes(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char name[IOKIT_MAX_NAME];
    osser_pool pool;
    osser_value *root;
    const osser_value *v = NULL;
    uint32_t len = 0;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, name, sizeof(name)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_props_decode(idx, &pool);
    if (!root || osser_dict_find(root, name, &v) != OSSER_ERR_OK) {
        rep->retcode = kIOReturnNotFound;
        return;
    }
    /* Raw bytes of the value: strings and data as-is, numbers as
     * little-endian bytes of their width, bools as one byte,
     * containers as their binary encoding. */
    if (v->type == OSSER_STRING || v->type == OSSER_DATA) {
        if (v->len > sizeof(g_ool_scratch)) {
            rep->retcode = KERN_NO_SPACE;
            return;
        }
        ik_memcpy(g_ool_scratch, v->bytes, v->len);
        len = v->len;
    } else if (v->type == OSSER_NUMBER) {
        if (v->num_bits == 32) {
            ik_wr32(g_ool_scratch, (uint32_t)v->num);
            len = 4;
        } else {
            ik_wr64(g_ool_scratch, v->num);
            len = 8;
        }
    } else if (v->type == OSSER_BOOL) {
        g_ool_scratch[0] = v->boolean ? 1 : 0;
        len = 1;
    } else {
        len = ik_ool_encode_value(v);
        if (len == 0) {
            rep->retcode = KERN_NO_SPACE;
            return;
        }
    }
    ik_reply_ool(rep, len);
}

static void ik_h_registry_create_iterator(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    uint32_t matches[IOKIT_MAX_SERVICES];
    uint32_t count;
    mach_port_name_t port;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* Plane and options are ignored in v1: the iterator covers
     * every registered service. */
    count = ik_build_matches(NULL, matches, IOKIT_MAX_SERVICES);
    port = ik_new_iterator(matches, count);
    if (port == MACH_PORT_NULL) {
        rep->retcode = KERN_NO_SPACE;
        return;
    }
    ik_reply_port(rep, port);
}

static void ik_h_registry_iterator_enter_exit(mig_req *req, mig_rep *rep) {
    int idx = ik_find_iter_by_port(req->dest);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* v1 iterators have no current-entry state: no-op success. */
}

static void ik_h_registry_entry_from_path(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char plane[IOKIT_MAX_NAME];
    char path[IOKIT_MAX_STRING];
    const char *last = NULL;
    int i;
    int svc = -1;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, plane, sizeof(plane)) != 0 ||
        mig_cur_string(&cur, path, sizeof(path)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)plane; /* plane is ignored in v1 */
    /* The service name is the last path component. */
    last = path;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/') last = path + i + 1;
    }
    svc = ik_find_service_by_name(last);
    if (svc < 0) {
        rep->retcode = kIOReturnNotFound;
        return;
    }
    ik_reply_port(rep, g_services[svc].port);
}

static void ik_h_registry_get_name(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_string(rep, ik_obj_class(kind, idx));
}

static void ik_h_registry_get_path(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    char path[IOKIT_MAX_STRING];
    const char *cls;
    size_t n;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* [guess] path shape: IOService:/IOResources/<ClassName>. */
    cls = ik_obj_class(kind, idx);
    {
        static const char pre[] = "IOService:/IOResources/";
        n = sizeof(pre) - 1;
        if (ik_strcmp(cls, "IOResources") == 0) {
            /* The root entry itself: no trailing component. */
            n = sizeof(pre) - 2; /* drop the trailing '/' */
        }
        if (n + ik_strlen(cls) + 1 > sizeof(path)) {
            rep->retcode = KERN_NO_SPACE;
            return;
        }
        ik_memcpy(path, pre, n);
        if (ik_strcmp(cls, "IOResources") != 0) {
            ik_memcpy(path + n, cls, ik_strlen(cls) + 1);
        } else {
            path[n] = '\0';
        }
    }
    (void)mig_rep_string(rep, path);
}

static void ik_h_registry_get_root_entry(mig_req *req, mig_rep *rep) {
    int idx = ik_find_service_by_name("IOResources");
    (void)req;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_reply_port(rep, g_services[idx].port);
}

static void ik_h_registry_in_plane(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    char plane[IOKIT_MAX_NAME];
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, plane, sizeof(plane)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* [guess] every entry is in the IOService plane and in no
     * other plane in v1. */
    (void)mig_rep_u32(rep, (ik_strcmp(plane, "IOService") == 0) ? 1 : 0);
}

static void ik_h_registry_get_location_in_plane(mig_req *req,
                                                mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_string(rep, "");
}

static void ik_h_object_get_retain_count(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_u32(rep, ik_obj_refcount(kind, idx));
}

static void ik_h_object_get_superclass(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    const char *sup = NULL;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* [guess] direct superclasses: services are IORegistryEntry,
     * connections are IOUserClient, iterators are IOIterator. */
    if (kind == 0) sup = "IORegistryEntry";
    else if (kind == 1) sup = "IOUserClient";
    else sup = "IOIterator";
    (void)mig_rep_string(rep, sup);
}

static void ik_h_object_get_bundle_identifier(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_string(rep, "");
}

static void ik_h_service_get_busy_state(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_u32(rep, 0);
}

static void ik_h_service_wait_quiet(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
}

static void ik_h_service_request_probe(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    (void)mig_cur_u32(&cur); /* options, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
    }
}

static void ik_h_service_get_state(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    (void)mig_cur_u32(&cur); /* options, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* [guess] state 0, busy 0: the neutral registered state. */
    (void)mig_rep_u64(rep, 0);
    (void)mig_rep_u32(rep, 0);
}

static void ik_h_make_matching(mig_req *req, mig_rep *rep) {
    mig_cur cur;
    char name[IOKIT_MAX_NAME];
    char xml[IOKIT_MAX_STRING];
    size_t n;
    static const char pre[] =
        "<plist version=\"1.0\"><dict><key>IOProviderClass</key>"
        "<string>";
    static const char post[] = "</string></dict></plist>";
    (void)req;
    mig_cur_init(&cur, req->body, req->body_len);
    if (mig_cur_string(&cur, name, sizeof(name)) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_cur_u32(&cur); /* options, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    n = sizeof(pre) - 1 + ik_strlen(name) + sizeof(post) - 1;
    if (n + 1 > sizeof(xml)) {
        rep->retcode = KERN_NO_SPACE;
        return;
    }
    ik_memcpy(xml, pre, sizeof(pre) - 1);
    ik_memcpy(xml + sizeof(pre) - 1, name, ik_strlen(name));
    ik_memcpy(xml + sizeof(pre) - 1 + ik_strlen(name), post,
              sizeof(post) - 1);
    xml[n] = '\0';
    (void)mig_rep_string(rep, xml);
}

static void ik_h_service_match_property_table(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    osser_pool pool;
    osser_value *root;
    mig_cur cur;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_matching_from_ool(req, &pool);
    if (!root) {
        rep->retcode = kIOReturnBadArgument;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    (void)mig_cur_u32(&cur); /* options, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    (void)mig_rep_u32(rep, ik_table_matches(idx, root) ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Open / close / connect handlers.                                    */
/* ------------------------------------------------------------------ */

static void ik_h_service_open(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    uint32_t type;
    mach_port_name_t port;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    type = mig_cur_u32(&cur);
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    port = ik_new_connection(idx, type, req->ports[0]);
    if (port == MACH_PORT_NULL) {
        rep->retcode = KERN_NO_SPACE;
        return;
    }
    ik_reply_port(rep, port);
}

static void ik_h_service_open_extended(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    mig_cur cur;
    uint32_t type;
    mach_port_name_t port;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    type = mig_cur_u32(&cur);
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* The properties OOL blob (descriptor 0) is passed to the user
     * client constructor; v1 validates that it parses and ignores
     * the contents. It is usually empty for IOSurfaceRoot. */
    if (req->n_ools >= 1 && req->ool_size[0] > 0) {
        osser_pool pool;
        osser_value *root = NULL;
        const uint8_t *blob =
            (const uint8_t *)(uintptr_t)req->ool_addr[0];
        ik_scratch_pool(&pool);
        if (req->ool_size[0] > IOKIT_OOL_SCRATCH_SIZE ||
            osser_decode_auto(blob, req->ool_size[0], &pool, &root) !=
                OSSER_ERR_OK) {
            rep->retcode = kIOReturnBadArgument;
            return;
        }
    }
    port = ik_new_connection(idx, type, req->ports[0]);
    if (port == MACH_PORT_NULL) {
        rep->retcode = KERN_NO_SPACE;
        return;
    }
    ik_reply_port(rep, port);
    (void)mig_rep_u32(rep, 0); /* result */
}

static void ik_h_service_close(mig_req *req, mig_rep *rep) {
    int idx = ik_find_conn_by_port(req->dest);
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* Free the slot. The port name itself is destroyed by ipc.c
     * when the client deallocates it; a later message to this port
     * will resolve to nothing and answer KERN_INVALID_ARGUMENT. */
    g_conns[idx].in_use = 0;
}

static void ik_h_connect_get_service(mig_req *req, mig_rep *rep) {
    int idx = ik_find_conn_by_port(req->dest);
    int s;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    s = (int)g_conns[idx].service_idx;
    if (s < 0 || s >= IOKIT_MAX_SERVICES || !g_services[s].in_use) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_reply_port(rep, g_services[s].port);
}

static void ik_h_connect_set_notification_port(mig_req *req, mig_rep *rep) {
    int idx = ik_find_conn_by_port(req->dest);
    mig_cur cur;
    uint32_t type;
    uint64_t ref;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* 2818 and 2867 carry the same field stream after the NDR:
     * notification_type u32, reference u64; the port itself is
     * descriptor 0. v1 records the notification and never
     * delivers one. */
    mig_cur_init(&cur, req->body, req->body_len);
    type = mig_cur_u32(&cur);
    ref = mig_cur_u64(&cur);
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    g_conns[idx].notify_port = req->ports[0];
    g_conns[idx].notify_type = type;
    g_conns[idx].notify_ref = ref;
}

static void ik_h_connect_add_client(mig_req *req, mig_rep *rep) {
    int idx = ik_find_conn_by_port(req->dest);
    if (idx < 0 || req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* v1 accepts the client and records nothing about it. */
}

static void ik_h_connect_set_properties(mig_req *req, mig_rep *rep) {
    int idx = ik_find_conn_by_port(req->dest);
    osser_pool pool;
    osser_value *root;
    if (idx < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_matching_from_ool(req, &pool);
    if (!root || root->type != OSSER_DICT) {
        rep->retcode = kIOReturnBadArgument;
        (void)mig_rep_u32(rep, kIOReturnBadArgument);
        return;
    }
    /* v1 validates the blob and stores nothing. */
    (void)mig_rep_u32(rep, 0); /* result */
}

static void ik_h_registry_set_properties(mig_req *req, mig_rep *rep) {
    int kind;
    int idx = ik_resolve_object(req->dest, &kind);
    osser_pool pool;
    osser_value *root;
    int rc;
    if (idx < 0 || kind != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_scratch_pool(&pool);
    root = ik_matching_from_ool(req, &pool);
    if (!root || root->type != OSSER_DICT) {
        rep->retcode = kIOReturnBadArgument;
        (void)mig_rep_u32(rep, kIOReturnBadArgument);
        return;
    }
    rc = ik_merge_props(idx, root);
    if (rc != KERN_SUCCESS) {
        rep->retcode = kIOReturnBadArgument;
        (void)mig_rep_u32(rep, kIOReturnBadArgument);
        return;
    }
    (void)mig_rep_u32(rep, 0); /* result */
}

/* ------------------------------------------------------------------ */
/* io_connect_method / async_method / map_memory.                      */
/*                                                                     */
/* Wire layout (MIG declaration order, see iokit.h): request tail     */
/* after inband_input is ool_input u64, ool_input_size u64,           */
/* inband_outputCnt u32, scalar_outputCnt u32, ool_output u64,        */
/* ool_output_size u64. Reply is RetCode, inband_outputCnt u32 +     */
/* bytes, scalar_outputCnt u32 + u64s, ool_output_size u64.           */
/* ------------------------------------------------------------------ */

static int ik_parse_method_fields(mig_cur *cur, iokit_method_call *call) {
    uint32_t i;
    ik_memset(call, 0, sizeof(*call));
    call->selector = mig_cur_u32(cur);
    call->scalar_in_cnt = mig_cur_u32(cur);
    if (cur->err || call->scalar_in_cnt > IOKIT_MAX_SCALARS) return -1;
    for (i = 0; i < call->scalar_in_cnt; i++) {
        call->scalar_in[i] = mig_cur_u64(cur);
    }
    call->struct_in_len = mig_cur_u32(cur);
    if (cur->err || call->struct_in_len > IOKIT_MAX_STRUCT_IN) return -1;
    call->struct_in = mig_cur_bytes(cur, call->struct_in_len);
    if (cur->err) return -1;
    call->ool_in = mig_cur_u64(cur);
    call->ool_in_size = (uint32_t)mig_cur_u64(cur);
    call->struct_out_cap = mig_cur_u32(cur); /* inband_outputCnt */
    call->scalar_out_cap = mig_cur_u32(cur); /* scalar_outputCnt */
    call->ool_out = mig_cur_u64(cur);
    call->ool_out_size_cap = (uint32_t)mig_cur_u64(cur);
    if (cur->err) return -1;
    if (call->struct_out_cap > IOKIT_MAX_STRUCT_IN) {
        call->struct_out_cap = IOKIT_MAX_STRUCT_IN;
    }
    if (call->scalar_out_cap > IOKIT_MAX_SCALARS) {
        call->scalar_out_cap = IOKIT_MAX_SCALARS;
    }
    call->struct_out = g_method_out_buf;
    call->ool_out_addr = call->ool_out;
    call->ool_out_size = 0;
    call->retcode = KERN_SUCCESS;
    return 0;
}

/* Run one parsed method call on its connection and write the reply
 * fields (inband output first, then scalar output, then the OOL
 * output size). On a handler error the counts are 0, which the
 * client stub accepts: it checks RetCode first. */
static void ik_run_method(mig_req *req, int conn_idx,
                          iokit_method_call *call, mig_rep *rep) {
    uint32_t kind = 0;
    mach_port_name_t svc = MACH_PORT_NULL;
    uint32_t inband_len;
    uint32_t scalar_cnt;
    (void)conn_idx;
    if (iokit_connection_service(req->dest, &kind, &svc) !=
        KERN_SUCCESS) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (kind == IOKIT_SERVICE_KIND_IOSURFACE_ROOT) {
        iosurface_connect_method(req->dest, call);
    } else {
        call->retcode = kIOReturnUnsupported;
    }
    rep->retcode = call->retcode;
    inband_len = call->struct_out_len;
    if (inband_len > call->struct_out_cap) inband_len = call->struct_out_cap;
    scalar_cnt = call->scalar_out_cnt;
    if (scalar_cnt > call->scalar_out_cap) scalar_cnt = call->scalar_out_cap;
    (void)mig_rep_u32(rep, inband_len);
    if (inband_len > 0) {
        (void)mig_rep_bytes(rep, call->struct_out, inband_len);
    }
    (void)mig_rep_u32(rep, scalar_cnt);
    {
        uint32_t i;
        for (i = 0; i < scalar_cnt; i++) {
            (void)mig_rep_u64(rep, call->scalar_out[i]);
        }
    }
    {
        uint32_t ool_size = call->ool_out_size;
        if (ool_size > call->ool_out_size_cap) {
            ool_size = call->ool_out_size_cap;
        }
        (void)mig_rep_u64(rep, ool_size);
    }
}

static void ik_h_connect_method(mig_req *req, mig_rep *rep) {
    mig_cur cur;
    iokit_method_call call;
    int conn = ik_find_conn_by_port(req->dest);
    if (conn < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    if (ik_parse_method_fields(&cur, &call) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_run_method(req, conn, &call, rep);
}

static void ik_h_connect_async_method(mig_req *req, mig_rep *rep) {
    mig_cur cur;
    iokit_method_call call;
    int conn = ik_find_conn_by_port(req->dest);
    int i;
    if (conn < 0 || req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* wake_port is descriptor 0; reference is a fixed 8 x u64
     * array at the front of the field stream, before the selector.
     * v1 records both and runs the method synchronously; no async
     * completion is ever delivered. */
    g_conns[conn].async_port = req->ports[0];
    mig_cur_init(&cur, req->body, req->body_len);
    for (i = 0; i < IOKIT_MAX_ASYNC_REF; i++) {
        g_conns[conn].async_ref[i] = mig_cur_u64(&cur);
    }
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (ik_parse_method_fields(&cur, &call) != 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    ik_run_method(req, conn, &call, rep);
}

static void ik_h_connect_map_memory_into_task(mig_req *req, mig_rep *rep) {
    mig_cur cur;
    uint32_t mem_type;
    uint64_t addr = 0;
    uint64_t size = 0;
    uint32_t kind = 0;
    mach_port_name_t svc = MACH_PORT_NULL;
    int conn = ik_find_conn_by_port(req->dest);
    if (conn < 0) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    mem_type = mig_cur_u32(&cur);
    (void)mig_cur_u64(&cur); /* address hint, ignored */
    (void)mig_cur_u64(&cur); /* size hint, ignored */
    (void)mig_cur_u32(&cur); /* flags, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (iokit_connection_service(req->dest, &kind, &svc) !=
        KERN_SUCCESS) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    if (kind == IOKIT_SERVICE_KIND_IOSURFACE_ROOT) {
        /* Candidate A: memory_type is the surface id. */
        int rc = iosurface_map_memory(req->dest, mem_type, &addr, &size);
        if (rc == KERN_INVALID_ARGUMENT) {
            rep->retcode = kIOReturnBadArgument;
            return;
        }
        if (rc != KERN_SUCCESS) {
            rep->retcode = kIOReturnError;
            return;
        }
    } else if (g_conns[conn].memory_size > 0) {
        addr = g_conns[conn].memory_addr;
        size = g_conns[conn].memory_size;
    } else {
        rep->retcode = kIOReturnNotFound;
        return;
    }
    (void)mig_rep_u64(rep, addr);
    (void)mig_rep_u64(rep, size);
}

static void ik_h_connect_unmap_memory_from_task(mig_req *req, mig_rep *rep) {
    mig_cur cur;
    int conn = ik_find_conn_by_port(req->dest);
    if (conn < 0 || req->n_ports < 1) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    mig_cur_init(&cur, req->body, req->body_len);
    (void)mig_cur_u64(&cur); /* address, ignored */
    (void)mig_cur_u64(&cur); /* size, ignored */
    if (cur.err) {
        rep->retcode = KERN_INVALID_ARGUMENT;
        return;
    }
    /* v1 never unmaps: the mapping is in-process and the caller
     * simply stops using the address. No-op success. */
}

/* ------------------------------------------------------------------ */
/* Dispatch and the kobject entry point.                               */
/* ------------------------------------------------------------------ */

static void ik_dispatch(mig_req *req, mig_rep *rep) {
    uint32_t id = req->id;
    if (id == IOKIT_MIG_OBJECT_GET_CLASS) {
        ik_h_object_get_class(req, rep);
    } else if (id == IOKIT_MIG_OBJECT_CONFORMS_TO) {
        ik_h_object_conforms_to(req, rep);
    } else if (id == IOKIT_MIG_ITERATOR_NEXT) {
        ik_h_iterator_next(req, rep);
    } else if (id == IOKIT_MIG_ITERATOR_RESET) {
        ik_h_iterator_reset(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES) {
        ik_h_service_get_matching_services(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PROPERTY) {
        ik_h_registry_get_property(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_CREATE_ITERATOR ||
               id == IOKIT_MIG_REGISTRY_ENTRY_CREATE_ITERATOR) {
        ik_h_registry_create_iterator(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_ITERATOR_ENTER_ENTRY ||
               id == IOKIT_MIG_REGISTRY_ITERATOR_EXIT_ENTRY) {
        ik_h_registry_iterator_enter_exit(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_ENTRY_FROM_PATH) {
        ik_h_registry_entry_from_path(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_NAME ||
               id == IOKIT_MIG_REGISTRY_GET_NAME_IN_PLANE) {
        ik_h_registry_get_name(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PROPERTIES) {
        ik_h_registry_get_properties(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PROPERTY_BYTES) {
        ik_h_registry_get_property_bytes(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_CHILD_ITERATOR ||
               id == IOKIT_MIG_REGISTRY_GET_PARENT_ITERATOR) {
        ik_h_registry_create_iterator(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_OPEN) {
        ik_h_service_open(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_CLOSE) {
        ik_h_service_close(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_GET_SERVICE) {
        ik_h_connect_get_service(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_SET_NOTIFICATION_PORT ||
               id == IOKIT_MIG_CONNECT_SET_NOTIFICATION_PORT_64) {
        ik_h_connect_set_notification_port(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_ADD_CLIENT) {
        ik_h_connect_add_client(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_SET_PROPERTIES) {
        ik_h_connect_set_properties(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PATH) {
        ik_h_registry_get_path(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_ROOT_ENTRY) {
        ik_h_registry_get_root_entry(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_SET_PROPERTIES) {
        ik_h_registry_set_properties(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_IN_PLANE) {
        ik_h_registry_in_plane(req, rep);
    } else if (id == IOKIT_MIG_OBJECT_GET_RETAIN_COUNT) {
        ik_h_object_get_retain_count(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_GET_BUSY_STATE) {
        ik_h_service_get_busy_state(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_WAIT_QUIET) {
        ik_h_service_wait_quiet(req, rep);
    } else if (id == IOKIT_MIG_ITERATOR_IS_VALID) {
        ik_h_iterator_is_valid(req, rep);
    } else if (id == IOKIT_MIG_MAKE_MATCHING) {
        ik_h_make_matching(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_REQUEST_PROBE) {
        ik_h_service_request_probe(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_NAME_IN_PLANE) {
        ik_h_registry_get_name(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_MATCH_PROPERTY_TABLE) {
        ik_h_service_match_property_table(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_LOCATION_IN_PLANE) {
        ik_h_registry_get_location_in_plane(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PROPERTY_RECURSIVELY) {
        ik_h_registry_get_property_recursively(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_GET_STATE) {
        ik_h_service_get_state(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES_OOL ||
               id == IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES_BIN) {
        ik_h_service_get_matching_services_ool(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_MATCH_PROPERTY_TABLE_OOL) {
        ik_h_service_match_property_table(req, rep);
    } else if (id == IOKIT_MIG_OBJECT_GET_SUPERCLASS) {
        ik_h_object_get_superclass(req, rep);
    } else if (id == IOKIT_MIG_OBJECT_GET_BUNDLE_IDENTIFIER) {
        ik_h_object_get_bundle_identifier(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_OPEN_EXTENDED) {
        ik_h_service_open_extended(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_MAP_MEMORY_INTO_TASK) {
        ik_h_connect_map_memory_into_task(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_UNMAP_MEMORY_FROM_TASK) {
        ik_h_connect_unmap_memory_from_task(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_METHOD) {
        ik_h_connect_method(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_ASYNC_METHOD) {
        ik_h_connect_async_method(req, rep);
    } else if (id == IOKIT_MIG_SERVICE_GET_MATCHING_SERVICE_BIN) {
        ik_h_service_get_matching_service_bin(req, rep);
    } else if (id == IOKIT_MIG_REGISTRY_GET_PROPERTY_BIN) {
        ik_h_registry_get_property_bin(req, rep);
    } else if (id == IOKIT_MIG_CONNECT_MAP_MEMORY ||
               id == IOKIT_MIG_CONNECT_METHOD_SCALAR_I_SCALAR_O ||
               id == IOKIT_MIG_CONNECT_METHOD_SCALAR_I_STRUCT_O ||
               id == IOKIT_MIG_CONNECT_METHOD_SCALAR_I_STRUCT_I ||
               id == IOKIT_MIG_CONNECT_METHOD_STRUCT_I_STRUCT_O ||
               id == IOKIT_MIG_CATALOG_SEND_DATA ||
               id == IOKIT_MIG_CATALOG_TERMINATE ||
               id == IOKIT_MIG_CATALOG_GET_DATA ||
               id == IOKIT_MIG_CATALOG_GET_GEN_COUNT ||
               id == IOKIT_MIG_CATALOG_MODULE_LOADED ||
               id == IOKIT_MIG_CATALOG_RESET ||
               id == IOKIT_MIG_ASYNC_METHOD_SCALAR_I_SCALAR_O ||
               id == IOKIT_MIG_ASYNC_METHOD_SCALAR_I_STRUCT_O ||
               id == IOKIT_MIG_ASYNC_METHOD_SCALAR_I_STRUCT_I ||
               id == IOKIT_MIG_ASYNC_METHOD_STRUCT_I_STRUCT_O ||
               id == IOKIT_MIG_SERVICE_ADD_NOTIFICATION ||
               id == IOKIT_MIG_SERVICE_ADD_INTEREST_NOTIF ||
               id == IOKIT_MIG_SERVICE_ACK_NOTIFICATION ||
               id == IOKIT_MIG_CONNECT_GET_NOTIFICATION_SEMAPHORE ||
               id == IOKIT_MIG_CONNECT_UNMAP_MEMORY ||
               id == IOKIT_MIG_SERVICE_ADD_NOTIFICATION_OOL ||
               id == IOKIT_MIG_SERVICE_ADD_NOTIFICATION_64 ||
               id == IOKIT_MIG_SERVICE_ADD_INTEREST_NOTIF_64 ||
               id == IOKIT_MIG_SERVICE_ADD_NOTIFICATION_OOL_64) {
        ik_h_unsupported(req, rep);
    } else {
        rep->retcode = KERN_INVALID_ARGUMENT;
    }
}

int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size,
                      uint32_t *out_reply_size) {
    mig_req req;
    mig_rep rep;
    if (!hdr || !reply_buf || !out_reply_size) return 0;
    *out_reply_size = 0;
    if (send_size < 24 || max_reply_size < 36) return 0;
    mig_req_parse(hdr, send_size, &req);
    if (req.err) {
        mig_rep_error(&rep, reply_buf, max_reply_size, &req,
                      KERN_INVALID_ARGUMENT);
        if (rep.err) return 0;
        *out_reply_size = rep.len;
        return 1;
    }
    mig_rep_init(&rep, reply_buf, max_reply_size, &req);
    if (rep.err) return 0;
    if (req.id == IOKIT_MIG_CONNECT_METHOD) {
        uint32_t sel = 0;
        if (req.body_len >= 4) sel = ik_rd32(req.body);
        ik_trace_msg(req.id, req.dest, 1, sel);
    } else if (req.id == IOKIT_MIG_CONNECT_ASYNC_METHOD) {
        uint32_t sel = 0;
        /* wake reference is 8 x u64, the selector follows it. */
        if (req.body_len >= 68) sel = ik_rd32(req.body + 64);
        ik_trace_msg(req.id, req.dest, 1, sel);
    } else {
        ik_trace_msg(req.id, req.dest, 0, 0);
    }
    ik_dispatch(&req, &rep);
    if (rep.err) {
        /* The handler overflowed the reply buffer: rebuild as a
         * minimal error reply. */
        mig_rep_error(&rep, reply_buf, max_reply_size, &req,
                      KERN_NO_SPACE);
        if (rep.err) return 0;
    } else {
        mig_rep_finish(&rep);
    }
    if (req.reply == MACH_PORT_NULL) {
        /* One-way message: the handler ran, no reply is sent. */
        *out_reply_size = 0;
        return 1;
    }
    *out_reply_size = rep.len;
    return 1;
}
