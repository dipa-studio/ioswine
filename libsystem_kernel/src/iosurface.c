/*
 * iosurface.c: IOSurfaceRoot user-client emulation for ioswine
 *. See iosurface.h for the design, sources and
 * [guess] list.
 *
 * Style: freestanding C, same as iokit.c. No libc, no allocation,
 * no blocking. Dispatch is an if-chain over the IOSURFACE_SEL_*
 * defines; there are no function-pointer tables in static data.
 * Surfaces live in one global fixed table (see iosurface.h for
 * the per-client vs global id scope note).
 *
 * No em dashes are used in this file.
 */
#include "iosurface.h"

#if defined(__aarch64__) && !defined(IOSURFACE_HOST)
#include "linux.h"
#endif

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static size_t is_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int is_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void is_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void is_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static uint32_t is_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void is_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void is_wr64(uint8_t *p, uint64_t v) {
    is_wr32(p, (uint32_t)(v & 0xffffffffu));
    is_wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t is_align_up(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* fd 2 output: raw syscall on the target, libc write() on host. */
#if defined(__aarch64__) && !defined(IOSURFACE_HOST)
static long is_fd_write(int fd, const void *buf, size_t n) {
    return sys_write(fd, buf, n);
}
#else
extern long write(int fd, const void *buf, unsigned long count);
static long is_fd_write(int fd, const void *buf, size_t n) {
    return write(fd, buf, (unsigned long)n);
}
#endif

static void is_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        long n = is_fd_write(fd, p, len);
        if (n <= 0) return;
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static size_t is_u32_dec(char *buf, size_t cap, uint32_t v) {
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
/* Backing store: memfd on the target, host hook otherwise.            */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__) && !defined(IOSURFACE_HOST)
#define IS_MEMFD_CREATE_NR 279
#define IS_PROT_READ  1
#define IS_PROT_WRITE 2
#define IS_MAP_SHARED 1

static int is_alloc_backing(uint64_t size, int *fd_out, void **ptr_out) {
    long fd;
    void *addr;
    if (size == 0 || size > 0x100000000ULL) return -1;
    fd = sys2(IS_MEMFD_CREATE_NR, "ioswine-surface", 0);
    if (fd < 0) return -1;
    if (sys2(SYS_ftruncate, fd, (long)size) != 0) {
        sys1(SYS_close, fd);
        return -1;
    }
    addr = sys_mmap(NULL, (size_t)size, IS_PROT_READ | IS_PROT_WRITE,
                    IS_MAP_SHARED, (int)fd, 0);
    if ((uintptr_t)addr >= (uintptr_t)-4095) {
        sys1(SYS_close, fd);
        return -1;
    }
    *fd_out = (int)fd;
    *ptr_out = addr;
    return 0;
}

static void is_free_backing(int fd, void *ptr, uint64_t size) {
    if (ptr) {
        (void)sys_munmap(ptr, (size_t)size);
    }
    if (fd >= 0) {
        (void)sys1(SYS_close, fd);
    }
}
#else
static int is_alloc_backing(uint64_t size, int *fd_out, void **ptr_out) {
    return iosurface_host_alloc(size, fd_out, ptr_out);
}

static void is_free_backing(int fd, void *ptr, uint64_t size) {
    iosurface_host_free(fd, ptr, size);
}
#endif

/* ------------------------------------------------------------------ */
/* Surface table.                                                      */
/* ------------------------------------------------------------------ */

static iosurface_surface g_surfaces[IOSURFACE_MAX_SURFACES];
static uint32_t g_next_id = 1;
static int g_inited = 0;
static mach_port_name_t g_root_port = MACH_PORT_NULL;

/* OSSerialize scratch pools for this file (single-threaded under
 * ipc_lock, like iokit.c's). */
static osser_value g_is_vals[128];
static osser_key g_is_keys[128];
static osser_value *g_is_objs[128];
static uint8_t g_is_text[2048];

static void is_pool_reset(osser_pool *pool) {
    pool->vals = g_is_vals;
    pool->vals_cap = 128;
    pool->vals_used = 0;
    pool->keys = g_is_keys;
    pool->keys_cap = 128;
    pool->keys_used = 0;
    pool->objs = g_is_objs;
    pool->objs_cap = 128;
    pool->objs_used = 0;
    pool->text = g_is_text;
    pool->text_cap = 2048;
    pool->text_used = 0;
}

static int is_find(uint32_t id) {
    int i;
    if (id == 0) return -1;
    for (i = 0; i < IOSURFACE_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use && g_surfaces[i].id == id) return i;
    }
    return -1;
}

static int is_find_by_port(mach_port_name_t port) {
    int i;
    uint32_t j;
    if (port == MACH_PORT_NULL) return -1;
    for (i = 0; i < IOSURFACE_MAX_SURFACES; i++) {
        if (!g_surfaces[i].in_use) continue;
        for (j = 0; j < g_surfaces[i].port_count; j++) {
            if (g_surfaces[i].ports[j] == port) return i;
        }
    }
    return -1;
}

/* Keep the root service's SurfaceCount property in step with the
 * live surface count. The property is named "SurfaceCount"
 * [guess: the root properties include a surface count, per the
 * Part 1 doc; the key name is not pinned down by any source]. */
static void is_update_root_count(void) {
    uint32_t n = iosurface_surface_count();
    if (g_root_port != MACH_PORT_NULL) {
        (void)iokit_service_set_property_u64(g_root_port, "SurfaceCount",
                                             n);
    }
}

static void is_destroy(int idx) {
    iosurface_surface *s = &g_surfaces[idx];
    is_free_backing(s->fd, s->addr, s->map_size);
    is_memset(s, 0, sizeof(*s));
    is_update_root_count();
}

int iosurface_init(void) {
    int rc;
    const char *keys[1];
    uint64_t vals[1];
    mach_port_name_t port;
    if (g_inited) return KERN_SUCCESS;
    rc = iokit_init();
    if (rc != KERN_SUCCESS) return rc;
    keys[0] = "SurfaceCount";
    vals[0] = 0;
    rc = iokit_register_service_simple("IOSurfaceRoot",
                                       IOKIT_SERVICE_KIND_IOSURFACE_ROOT,
                                       keys, vals, 1, &port);
    if (rc != KERN_SUCCESS) return rc;
    g_root_port = port;
    g_inited = 1;
    return KERN_SUCCESS;
}

mach_port_name_t iosurface_root_port(void) {
    return g_root_port;
}

uint32_t iosurface_surface_count(void) {
    uint32_t n = 0;
    int i;
    for (i = 0; i < IOSURFACE_MAX_SURFACES; i++) {
        if (g_surfaces[i].in_use) n++;
    }
    return n;
}

int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out) {
    int idx;
    if (!out) return KERN_INVALID_ARGUMENT;
    idx = is_find(surface_id);
    if (idx < 0) return KERN_INVALID_NAME;
    *out = g_surfaces[idx];
    return KERN_SUCCESS;
}

int iosurface_fd(uint32_t surface_id, int *fd_out) {
    int idx;
    if (!fd_out) return KERN_INVALID_ARGUMENT;
    idx = is_find(surface_id);
    if (idx < 0) return KERN_INVALID_NAME;
    *fd_out = g_surfaces[idx].fd;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Per-surface property table.                                         */
/* ------------------------------------------------------------------ */

static int is_prop_find(iosurface_surface *s, const char *key) {
    uint32_t i;
    for (i = 0; i < s->prop_count; i++) {
        if (is_strcmp(s->props[i].key, key) == 0) return (int)i;
    }
    return -1;
}

static int is_prop_set(iosurface_surface *s, const char *key,
                       uint32_t kind, uint64_t num, const char *str,
                       const uint8_t *data, uint32_t data_len) {
    int idx = is_prop_find(s, key);
    iosurface_prop *p;
    size_t klen;
    if (idx < 0) {
        if (s->prop_count >= IOSURFACE_MAX_PROPS) return -1;
        idx = (int)s->prop_count++;
    }
    p = &s->props[idx];
    is_memset(p, 0, sizeof(*p));
    klen = is_strlen(key);
    if (klen >= IOSURFACE_PROP_KEY_MAX) klen = IOSURFACE_PROP_KEY_MAX - 1;
    is_memcpy(p->key, key, klen);
    p->key[klen] = '\0';
    p->kind = kind;
    p->num = num;
    if (kind == IOSURFACE_PROP_STRING && str) {
        size_t slen = is_strlen(str);
        if (slen >= IOSURFACE_PROP_STR_MAX) {
            slen = IOSURFACE_PROP_STR_MAX - 1;
        }
        is_memcpy(p->str, str, slen);
        p->str[slen] = '\0';
    }
    if (kind == IOSURFACE_PROP_DATA && data) {
        uint32_t dlen = data_len;
        if (dlen > IOSURFACE_PROP_DATA_MAX) dlen = IOSURFACE_PROP_DATA_MAX;
        is_memcpy(p->data, data, dlen);
        p->data_len = dlen;
    }
    return 0;
}

static int is_prop_set_number(iosurface_surface *s, const char *key,
                              uint64_t num) {
    return is_prop_set(s, key, IOSURFACE_PROP_NUMBER, num, NULL, NULL, 0);
}

/* Store one decoded value as a surface property. Dicts, arrays and
 * reals cannot be stored in the fixed table and are skipped
 * (returns 0, not an error). Returns 0 on success/skip, -1 when
 * the table is full. */
static int is_prop_from_value(iosurface_surface *s, const char *key,
                              const osser_value *v) {
    if (v->type == OSSER_NUMBER) {
        return is_prop_set_number(s, key, v->num);
    }
    if (v->type == OSSER_BOOL) {
        return is_prop_set_number(s, key, v->boolean ? 1 : 0);
    }
    if (v->type == OSSER_STRING) {
        char tmp[IOSURFACE_PROP_STR_MAX];
        uint32_t len = v->len;
        if (len >= sizeof(tmp)) len = (uint32_t)sizeof(tmp) - 1;
        is_memcpy(tmp, v->bytes, len);
        tmp[len] = '\0';
        return is_prop_set(s, key, IOSURFACE_PROP_STRING, 0, tmp, NULL, 0);
    }
    if (v->type == OSSER_DATA) {
        return is_prop_set(s, key, IOSURFACE_PROP_DATA, 0, NULL, v->bytes,
                           v->len);
    }
    return 0;
}

/* Encode one surface's properties as an OSSerializeBinary dict into
 * buf (cap bytes): the prop-table entries plus a PlaneInfo array
 * built from the planes array. Returns the byte length, or 0 on
 * failure. */
static uint32_t is_surface_props_blob(iosurface_surface *s, uint8_t *buf,
                                      uint32_t cap) {
    osser_writer w;
    uint32_t i;
    uint32_t j;
    if (osser_w_init(&w, buf, cap) != OSSER_ERR_OK) return 0;
    if (osser_w_begin_dict(&w, s->prop_count + 1) != OSSER_ERR_OK) {
        return 0;
    }
    for (i = 0; i < s->prop_count; i++) {
        iosurface_prop *p = &s->props[i];
        if (osser_w_key(&w, p->key) != OSSER_ERR_OK) return 0;
        if (p->kind == IOSURFACE_PROP_NUMBER) {
            if (osser_w_u64(&w, p->num) != OSSER_ERR_OK) return 0;
        } else if (p->kind == IOSURFACE_PROP_STRING) {
            if (osser_w_string(&w, p->str) != OSSER_ERR_OK) return 0;
        } else {
            if (osser_w_data(&w, p->data, p->data_len) != OSSER_ERR_OK) {
                return 0;
            }
        }
    }
    if (osser_w_key(&w, "PlaneInfo") != OSSER_ERR_OK) return 0;
    if (osser_w_begin_array(&w, s->plane_count) != OSSER_ERR_OK) {
        return 0;
    }
    for (j = 0; j < s->plane_count; j++) {
        iosurface_plane *pl = &s->planes[j];
        if (osser_w_begin_dict(&w, 5) != OSSER_ERR_OK) return 0;
        if (osser_w_key(&w, "PlaneWidth") != OSSER_ERR_OK) return 0;
        if (osser_w_u32(&w, pl->width) != OSSER_ERR_OK) return 0;
        if (osser_w_key(&w, "PlaneHeight") != OSSER_ERR_OK) return 0;
        if (osser_w_u32(&w, pl->height) != OSSER_ERR_OK) return 0;
        if (osser_w_key(&w, "PlaneBytesPerRow") != OSSER_ERR_OK) {
            return 0;
        }
        if (osser_w_u32(&w, pl->bytes_per_row) != OSSER_ERR_OK) {
            return 0;
        }
        if (osser_w_key(&w, "PlaneOffset") != OSSER_ERR_OK) return 0;
        if (osser_w_u32(&w, pl->offset) != OSSER_ERR_OK) return 0;
        if (osser_w_key(&w, "PlaneSize") != OSSER_ERR_OK) return 0;
        if (osser_w_u32(&w, pl->size) != OSSER_ERR_OK) return 0;
        if (osser_w_end_dict(&w) != OSSER_ERR_OK) return 0;
    }
    if (osser_w_end_array(&w) != OSSER_ERR_OK) return 0;
    if (osser_w_end_dict(&w) != OSSER_ERR_OK) return 0;
    return osser_w_len(&w);
}

/* ------------------------------------------------------------------ */
/* Create-dict parsing.                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row; /* 0 = derive */
    uint32_t bytes_per_element;
    uint32_t pixel_format;
    uint64_t alloc_size; /* 0 = derive */
    uint32_t cache_mode;
    uint32_t offset;
    uint32_t plane_count; /* 0 = derive a single plane */
    iosurface_plane planes[IOSURFACE_MAX_PLANES];
} is_create_args;

static int is_dict_u32(const osser_value *dict, const char *key,
                       uint32_t *out) {
    const osser_value *v = NULL;
    uint64_t n;
    if (osser_dict_find(dict, key, &v) != OSSER_ERR_OK) return -1;
    if (osser_as_uint64(v, &n) != OSSER_ERR_OK) return -1;
    if (n > 0xffffffffULL) return -1;
    *out = (uint32_t)n;
    return 0;
}

static int is_dict_u64(const osser_value *dict, const char *key,
                       uint64_t *out) {
    const osser_value *v = NULL;
    if (osser_dict_find(dict, key, &v) != OSSER_ERR_OK) return -1;
    return osser_as_uint64(v, out);
}

/* Parse one PlaneInfo entry dict. Missing fields fall back to the
 * surface-level values passed in. */
static void is_parse_plane(const osser_value *dict, uint32_t surf_w,
                           uint32_t surf_h, uint32_t surf_bpr,
                           iosurface_plane *pl) {
    uint32_t v;
    pl->width = surf_w;
    pl->height = surf_h;
    pl->bytes_per_row = surf_bpr;
    pl->offset = 0;
    pl->size = 0;
    if (is_dict_u32(dict, "PlaneWidth", &v) == 0) pl->width = v;
    else if (is_dict_u32(dict, "Width", &v) == 0) pl->width = v;
    if (is_dict_u32(dict, "PlaneHeight", &v) == 0) pl->height = v;
    else if (is_dict_u32(dict, "Height", &v) == 0) pl->height = v;
    if (is_dict_u32(dict, "PlaneBytesPerRow", &v) == 0) {
        pl->bytes_per_row = v;
    } else if (is_dict_u32(dict, "BytesPerRow", &v) == 0) {
        pl->bytes_per_row = v;
    }
    if (is_dict_u32(dict, "PlaneOffset", &v) == 0) pl->offset = v;
    else if (is_dict_u32(dict, "Offset", &v) == 0) pl->offset = v;
    if (is_dict_u32(dict, "PlaneSize", &v) == 0) pl->size = v;
    else if (is_dict_u32(dict, "Size", &v) == 0) pl->size = v;
    if (pl->size == 0) {
        pl->size = pl->height * pl->bytes_per_row;
    }
}

static int is_parse_create_dict(const osser_value *dict,
                                is_create_args *args) {
    const osser_value *v = NULL;
    uint32_t tmp;
    is_memset(args, 0, sizeof(*args));
    args->bytes_per_element = 1; /* IOSurfaceAPI.h default */
    args->cache_mode = IOSURFACE_CACHE_DEFAULT;
    if (!dict || dict->type != OSSER_DICT) return -1;
    if (is_dict_u32(dict, "Width", &args->width) != 0) return -1;
    if (is_dict_u32(dict, "Height", &args->height) != 0) return -1;
    if (args->width == 0 || args->height == 0) return -1;
    if (is_dict_u32(dict, "PixelFormat", &args->pixel_format) != 0) {
        args->pixel_format = IOSURFACE_PIXEL_FORMAT_NONE;
    }
    if (is_dict_u32(dict, "BytesPerElement", &tmp) == 0) {
        args->bytes_per_element = tmp;
    } else if (args->pixel_format == IOSURFACE_PIXEL_FORMAT_BGRA) {
        /* [guess] sensible default when the dict omits it: BGRA
         * is 4 bytes per element. The framework normally sends
         * BytesPerRow explicitly, so this rarely matters. */
        args->bytes_per_element = 4;
    }
    if (args->bytes_per_element == 0) args->bytes_per_element = 1;
    if (is_dict_u32(dict, "BytesPerRow", &args->bytes_per_row) != 0) {
        args->bytes_per_row = args->width * args->bytes_per_element;
    }
    if (is_dict_u64(dict, "AllocSize", &args->alloc_size) != 0) {
        args->alloc_size = 0; /* derive below */
    }
    if (is_dict_u32(dict, "CacheMode", &args->cache_mode) != 0) {
        args->cache_mode = IOSURFACE_CACHE_DEFAULT;
    }
    if (is_dict_u32(dict, "Offset", &args->offset) != 0) {
        args->offset = 0;
    }
    /* PlaneInfo: an array of plane dicts, or absent (single
     * derived plane). */
    if (osser_dict_find(dict, "PlaneInfo", &v) == OSSER_ERR_OK &&
        v->type == OSSER_ARRAY && v->count > 0) {
        uint32_t n = v->count;
        uint32_t i;
        uint32_t bpr = is_align_up(args->bytes_per_row,
                                   IOSURFACE_ROW_ALIGN);
        if (n > IOSURFACE_MAX_PLANES) n = IOSURFACE_MAX_PLANES;
        args->plane_count = n;
        for (i = 0; i < n; i++) {
            if (v->items[i].type != OSSER_DICT) return -1;
            is_parse_plane(&v->items[i], args->width, args->height,
                           bpr, &args->planes[i]);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Method handlers.                                                    */
/* ------------------------------------------------------------------ */

/* Read scalar_in[idx] as a u32 (surface ids and port names are
 * 32-bit). Returns 0 on success, -1 when the scalar is missing or
 * does not fit in 32 bits. */
static int is_scalar(const iokit_method_call *call, uint32_t idx,
                     uint32_t *out) {
    if (idx >= call->scalar_in_cnt) return -1;
    if (call->scalar_in[idx] > 0xffffffffULL) return -1;
    *out = (uint32_t)call->scalar_in[idx];
    return 0;
}

static void is_h_create(iokit_method_call *call) {
    osser_pool pool;
    osser_value *root = NULL;
    is_create_args args;
    uint32_t bpr_aligned;
    uint64_t alloc_size;
    int fd = -1;
    void *addr = NULL;
    int slot = -1;
    int i;
    uint32_t id;
    uint32_t wlen;
    iosurface_surface *s;
    if (call->struct_in_len == 0 || !call->struct_in) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    is_pool_reset(&pool);
    if (osser_decode_auto(call->struct_in, call->struct_in_len, &pool,
                          &root) != OSSER_ERR_OK) {
        /* Not an XML/binary dict: this is the fast-struct form
         * (IOSurfaceFastCreateArgs), which v1 does not implement. */
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (is_parse_create_dict(root, &args) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (args.width > IOSURFACE_LIMIT_MAX_WIDTH ||
        args.height > IOSURFACE_LIMIT_MAX_HEIGHT) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    bpr_aligned = is_align_up(args.bytes_per_row, IOSURFACE_ROW_ALIGN);
    if (bpr_aligned > IOSURFACE_LIMIT_MAX_BYTES_PER_ROW ||
        bpr_aligned == 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    alloc_size = args.alloc_size;
    if (alloc_size == 0) {
        alloc_size = (uint64_t)args.height * bpr_aligned;
    }
    if (alloc_size < (uint64_t)args.height * bpr_aligned) {
        /* The caller's AllocSize does not cover the aligned
         * stride: grow it so every row fits. The kernel would
         * not hand out a surface whose planes overrun its
         * backing store. */
        alloc_size = (uint64_t)args.height * bpr_aligned;
    }
    if (alloc_size == 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (args.plane_count == 0) {
        uint64_t psz = alloc_size;
        if (psz > 0xffffffffULL) psz = 0xffffffffULL;
        args.plane_count = 1;
        args.planes[0].offset = args.offset;
        args.planes[0].size = (uint32_t)psz;
        args.planes[0].bytes_per_row = bpr_aligned;
        args.planes[0].width = args.width;
        args.planes[0].height = args.height;
    }
    for (i = 0; i < IOSURFACE_MAX_SURFACES; i++) {
        if (!g_surfaces[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        call->retcode = kIOReturnNoResources;
        return;
    }
    if (is_alloc_backing(alloc_size, &fd, &addr) != 0) {
        call->retcode = kIOReturnNoMemory;
        return;
    }
    id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1; /* never hand out id 0 */
    is_memset(&g_surfaces[slot], 0, sizeof(g_surfaces[slot]));
    s = &g_surfaces[slot];
    s->in_use = 1;
    s->id = id;
    s->width = args.width;
    s->height = args.height;
    s->bytes_per_row = bpr_aligned;
    s->pixel_format = args.pixel_format;
    s->bytes_per_element = args.bytes_per_element;
    s->alloc_size = alloc_size;
    s->cache_mode = args.cache_mode;
    s->plane_count = args.plane_count;
    for (i = 0; i < (int)args.plane_count; i++) {
        s->planes[i] = args.planes[i];
    }
    s->lock_count = 0;
    s->seed = 0;
    s->use_count = 1;
    s->fd = fd;
    s->addr = addr;
    s->map_size = alloc_size;
    (void)is_prop_set_number(s, "Width", args.width);
    (void)is_prop_set_number(s, "Height", args.height);
    (void)is_prop_set_number(s, "BytesPerRow", bpr_aligned);
    (void)is_prop_set_number(s, "BytesPerElement", args.bytes_per_element);
    (void)is_prop_set_number(s, "AllocSize", alloc_size);
    (void)is_prop_set_number(s, "PixelFormat", args.pixel_format);
    (void)is_prop_set_number(s, "CacheMode", args.cache_mode);
    (void)is_prop_set_number(s, "Offset", args.offset);
    /* Copy any other dict entries that fit the fixed table.
     * PlaneInfo lives in the planes array and IOSurfaceClass is an
     * accepted no-op; both are skipped here. */
    for (i = 0; i < (int)root->count; i++) {
        char keybuf[64];
        uint32_t klen = root->keys[i].len;
        if (klen >= sizeof(keybuf)) continue;
        is_memcpy(keybuf, root->keys[i].ptr, klen);
        keybuf[klen] = '\0';
        if (is_strcmp(keybuf, "Width") == 0 ||
            is_strcmp(keybuf, "Height") == 0 ||
            is_strcmp(keybuf, "BytesPerRow") == 0 ||
            is_strcmp(keybuf, "BytesPerElement") == 0 ||
            is_strcmp(keybuf, "AllocSize") == 0 ||
            is_strcmp(keybuf, "PixelFormat") == 0 ||
            is_strcmp(keybuf, "CacheMode") == 0 ||
            is_strcmp(keybuf, "Offset") == 0 ||
            is_strcmp(keybuf, "PlaneInfo") == 0 ||
            is_strcmp(keybuf, "IOSurfaceClass") == 0) {
            continue;
        }
        (void)is_prop_from_value(s, keybuf, &root->items[i]);
    }
    is_update_root_count();
    /* Fixed-size output struct: id @0, alloc size @8, properties
     * blob @16, zero-filled to the offered size (capped at the
     * checkStructureOutputSize). */
    wlen = call->struct_out_cap;
    if (wlen > IOSURFACE_CREATE_OUT_SIZE) wlen = IOSURFACE_CREATE_OUT_SIZE;
    is_memset(call->struct_out, 0, wlen);
    if (wlen >= 4) is_wr32(call->struct_out + 0, id);
    if (wlen >= 16) {
        is_wr64(call->struct_out + 8, alloc_size);
        (void)is_surface_props_blob(s, call->struct_out + 16, wlen - 16);
    }
    call->struct_out_len = wlen;
    call->scalar_out[0] = id;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_release(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    if (g_surfaces[idx].lock_count > 0) {
        call->retcode = kIOReturnBusy;
        return;
    }
    if (g_surfaces[idx].use_count > 0) g_surfaces[idx].use_count--;
    if (g_surfaces[idx].use_count == 0) is_destroy(idx);
    call->retcode = kIOReturnSuccess;
}

static void is_h_set_value(iokit_method_call *call) {
    uint32_t id;
    const uint8_t *arr;
    uint32_t arr_len;
    osser_pool pool;
    osser_value *root = NULL;
    int idx;
    const osser_value *keyv;
    char keybuf[64];
    uint32_t klen;
    if (call->struct_in_len < 8 || !call->struct_in) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    id = is_rd32(call->struct_in + 0);
    /* The struct input is an 8-byte prefix then an OSArray [key,
     * value]. If the array does not decode at offset 8, try offset
     * 4 (in case the prefix is only the 4-byte id). */
    arr = call->struct_in + 8;
    arr_len = call->struct_in_len - 8;
    is_pool_reset(&pool);
    if (osser_decode_auto(arr, arr_len, &pool, &root) != OSSER_ERR_OK) {
        if (call->struct_in_len > 4) {
            is_pool_reset(&pool);
            arr = call->struct_in + 4;
            arr_len = call->struct_in_len - 4;
            if (osser_decode_auto(arr, arr_len, &pool, &root) !=
                OSSER_ERR_OK) {
                call->retcode = kIOReturnBadArgument;
                return;
            }
        } else {
            call->retcode = kIOReturnBadArgument;
            return;
        }
    }
    if (!root || root->type != OSSER_ARRAY || root->count != 2) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    keyv = &root->items[0];
    if (keyv->type != OSSER_STRING) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    klen = keyv->len;
    if (klen >= sizeof(keybuf)) klen = (uint32_t)sizeof(keybuf) - 1;
    is_memcpy(keybuf, keyv->bytes, klen);
    keybuf[klen] = '\0';
    if (is_prop_from_value(&g_surfaces[idx], keybuf, &root->items[1]) != 0) {
        call->retcode = kIOReturnNoResources;
        return;
    }
    call->retcode = kIOReturnSuccess;
}

static void is_h_get_value(iokit_method_call *call) {
    uint32_t id;
    const uint8_t *key = NULL;
    uint32_t key_off = 0;
    char keybuf[128];
    uint32_t klen = 0;
    int idx;
    int pidx;
    iosurface_prop *p;
    osser_writer w;
    uint32_t blob_len;
    if (call->struct_in_len < 5 || !call->struct_in) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    id = is_rd32(call->struct_in + 0);
    /* The key follows the id. It is at offset 8 when the id is a
     * u64 (or the prefix is 8 bytes); if the byte at 8 is NUL but
     * offset 4 holds text, the id was a u32 and the key is at 4. */
    if (call->struct_in_len > 8 && call->struct_in[8] != 0) {
        key = call->struct_in + 8;
        key_off = 8;
    } else if (call->struct_in_len > 4 && call->struct_in[4] != 0) {
        key = call->struct_in + 4;
        key_off = 4;
    } else {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    while (key_off + klen < call->struct_in_len &&
           klen < sizeof(keybuf) - 1 && key[klen] != 0) {
        keybuf[klen] = (char)key[klen];
        klen++;
    }
    keybuf[klen] = '\0';
    if (klen == 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    pidx = is_prop_find(&g_surfaces[idx], keybuf);
    if (pidx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    p = &g_surfaces[idx].props[pidx];
    /* struct_out: u32 blob length @0, binary blob @4. The caller
     * must offer room for both. */
    if (call->struct_out_cap < 4) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (osser_w_init(&w, call->struct_out + 4,
                     call->struct_out_cap - 4) != OSSER_ERR_OK) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (p->kind == IOSURFACE_PROP_NUMBER) {
        if (osser_w_u64(&w, p->num) != OSSER_ERR_OK) {
            call->retcode = kIOReturnBadArgument;
            return;
        }
    } else if (p->kind == IOSURFACE_PROP_STRING) {
        if (osser_w_string(&w, p->str) != OSSER_ERR_OK) {
            call->retcode = kIOReturnBadArgument;
            return;
        }
    } else {
        if (osser_w_data(&w, p->data, p->data_len) != OSSER_ERR_OK) {
            call->retcode = kIOReturnBadArgument;
            return;
        }
    }
    blob_len = osser_w_len(&w);
    is_wr32(call->struct_out + 0, blob_len);
    call->struct_out_len = 4 + blob_len;
    call->retcode = kIOReturnSuccess;
}

static void is_h_get_limits(iokit_method_call *call) {
    uint32_t wlen = call->struct_out_cap;
    if (wlen > 24) wlen = 24;
    if (wlen >= 8) {
        is_wr64(call->struct_out + 0, IOSURFACE_LIMIT_MAX_WIDTH);
    }
    if (wlen >= 16) {
        is_wr64(call->struct_out + 8, IOSURFACE_LIMIT_MAX_HEIGHT);
    }
    if (wlen >= 24) {
        is_wr64(call->struct_out + 16, IOSURFACE_LIMIT_MAX_BYTES_PER_ROW);
    }
    call->struct_out_len = wlen;
    call->retcode = kIOReturnSuccess;
}

static void is_h_get_use_count(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    call->scalar_out[0] = g_surfaces[idx].use_count;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_set_notify(iokit_method_call *call) {
    uint32_t id;
    /* scalar_in[0] = surface id, scalar_in[1] = notification type
     * [guess]. v1 records nothing and never delivers a
     * notification. */
    if (call->scalar_in_cnt > 0) {
        if (is_scalar(call, 0, &id) != 0) {
            call->retcode = kIOReturnBadArgument;
            return;
        }
        if (is_find(id) < 0) {
            call->retcode = kIOReturnNotFound;
            return;
        }
    }
    call->retcode = kIOReturnSuccess;
}

static void is_h_lookup_from_port(iokit_method_call *call) {
    uint32_t portname;
    int idx;
    uint32_t len;
    if (is_scalar(call, 0, &portname) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find_by_port((mach_port_name_t)portname);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    len = is_surface_props_blob(&g_surfaces[idx], call->struct_out,
                                call->struct_out_cap);
    if (len == 0) {
        call->retcode = kIOReturnError;
        return;
    }
    call->struct_out_len = len;
    call->scalar_out[0] = g_surfaces[idx].id;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_create_port(iokit_method_call *call) {
    uint32_t id;
    int idx;
    mach_port_name_t port;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    port = ipc_iokit_port_create();
    if (port == MACH_PORT_NULL) {
        call->retcode = kIOReturnNoResources;
        return;
    }
    if (g_surfaces[idx].port_count < 4) {
        g_surfaces[idx].ports[g_surfaces[idx].port_count++] = port;
    }
    g_surfaces[idx].use_count++;
    call->scalar_out[0] = port;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_lock(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    g_surfaces[idx].lock_count++;
    call->scalar_out[0] = g_surfaces[idx].seed;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_unlock(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    if (g_surfaces[idx].lock_count > 0) {
        g_surfaces[idx].lock_count--;
        g_surfaces[idx].seed++;
    }
    call->scalar_out[0] = g_surfaces[idx].seed;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_lookup(iokit_method_call *call) {
    uint32_t id;
    int idx;
    uint32_t len;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    len = is_surface_props_blob(&g_surfaces[idx], call->struct_out,
                                call->struct_out_cap);
    if (len == 0) {
        call->retcode = kIOReturnError;
        return;
    }
    call->struct_out_len = len;
    call->scalar_out[0] = g_surfaces[idx].id;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_get_seed(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    call->scalar_out[0] = g_surfaces[idx].seed;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_get_plane_info(iokit_method_call *call) {
    uint32_t id;
    int idx;
    uint32_t j;
    uint32_t wlen;
    uint32_t need;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    need = g_surfaces[idx].plane_count * 20;
    wlen = call->struct_out_cap;
    if (wlen > need) wlen = need;
    wlen = (wlen / 20) * 20; /* whole records only */
    for (j = 0; j < wlen / 20; j++) {
        iosurface_plane *pl = &g_surfaces[idx].planes[j];
        uint8_t *p = call->struct_out + j * 20;
        is_wr32(p + 0, pl->offset);
        is_wr32(p + 4, pl->size);
        is_wr32(p + 8, pl->bytes_per_row);
        is_wr32(p + 12, pl->width);
        is_wr32(p + 16, pl->height);
    }
    call->struct_out_len = wlen;
    call->scalar_out[0] = g_surfaces[idx].plane_count;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_inc_use_count(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    g_surfaces[idx].use_count++;
    call->scalar_out[0] = g_surfaces[idx].use_count;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_dec_use_count(iokit_method_call *call) {
    uint32_t id;
    int idx;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    idx = is_find(id);
    if (idx < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    if (g_surfaces[idx].use_count > 0) g_surfaces[idx].use_count--;
    call->scalar_out[0] = g_surfaces[idx].use_count;
    call->scalar_out_cnt = 1;
    call->retcode = kIOReturnSuccess;
}

static void is_h_purge(iokit_method_call *call) {
    uint32_t id;
    if (is_scalar(call, 0, &id) != 0) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (is_find(id) < 0) {
        call->retcode = kIOReturnNotFound;
        return;
    }
    /* v1 no-op: the backing store stays mapped and is not zeroed. */
    call->retcode = kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* Entry points.                                                       */
/* ------------------------------------------------------------------ */

void iosurface_connect_method(mach_port_name_t conn_port,
                              iokit_method_call *call) {
    uint32_t kind = 0;
    mach_port_name_t svc = MACH_PORT_NULL;
    uint32_t sel;
    if (!call) return;
    sel = call->selector;
    if (iokit_connection_service(conn_port, &kind, &svc) !=
            KERN_SUCCESS ||
        kind != IOKIT_SERVICE_KIND_IOSURFACE_ROOT) {
        call->retcode = kIOReturnBadArgument;
        return;
    }
    if (iokit_trace_enabled()) {
        char line[64];
        size_t n = 0;
        uint32_t id_for_trace = 0;
        static const char pre[] = "iosurface sel ";
        static const char mid[] = " id ";
        if (call->scalar_in_cnt > 0) {
            id_for_trace = (uint32_t)call->scalar_in[0];
        }
        is_memcpy(line + n, pre, sizeof(pre) - 1);
        n += sizeof(pre) - 1;
        n += is_u32_dec(line + n, sizeof(line) - n - 1, sel);
        is_memcpy(line + n, mid, sizeof(mid) - 1);
        n += sizeof(mid) - 1;
        n += is_u32_dec(line + n, sizeof(line) - n - 1, id_for_trace);
        line[n++] = '\n';
        is_write_all(2, line, n);
    }
    call->retcode = kIOReturnSuccess;
    if (sel == IOSURFACE_SEL_CREATE) {
        is_h_create(call);
    } else if (sel == IOSURFACE_SEL_RELEASE) {
        is_h_release(call);
    } else if (sel == IOSURFACE_SEL_SET_VALUE) {
        is_h_set_value(call);
    } else if (sel == IOSURFACE_SEL_GET_VALUE) {
        is_h_get_value(call);
    } else if (sel == IOSURFACE_SEL_GET_LIMITS) {
        is_h_get_limits(call);
    } else if (sel == IOSURFACE_SEL_GET_USE_COUNT) {
        is_h_get_use_count(call);
    } else if (sel == IOSURFACE_SEL_SET_NOTIFY) {
        is_h_set_notify(call);
    } else if (sel == IOSURFACE_SEL_LOOKUP_FROM_PORT) {
        is_h_lookup_from_port(call);
    } else if (sel == IOSURFACE_SEL_CREATE_PORT) {
        is_h_create_port(call);
    } else if (sel == IOSURFACE_SEL_LOCK) {
        is_h_lock(call);
    } else if (sel == IOSURFACE_SEL_UNLOCK) {
        is_h_unlock(call);
    } else if (sel == IOSURFACE_SEL_LOOKUP) {
        is_h_lookup(call);
    } else if (sel == IOSURFACE_SEL_GET_SEED) {
        is_h_get_seed(call);
    } else if (sel == IOSURFACE_SEL_GET_PLANE_INFO) {
        is_h_get_plane_info(call);
    } else if (sel == IOSURFACE_SEL_INCREMENT_USE_COUNT) {
        is_h_inc_use_count(call);
    } else if (sel == IOSURFACE_SEL_DECREMENT_USE_COUNT) {
        is_h_dec_use_count(call);
    } else if (sel == IOSURFACE_SEL_PURGE) {
        is_h_purge(call);
    } else {
        call->retcode = kIOReturnUnsupported;
    }
}

int iosurface_map_memory(mach_port_name_t conn_port, uint32_t memory_type,
                         uint64_t *addr_out, uint64_t *size_out) {
    uint32_t kind = 0;
    mach_port_name_t svc = MACH_PORT_NULL;
    int idx;
    if (!addr_out || !size_out) return KERN_INVALID_ARGUMENT;
    if (iokit_connection_service(conn_port, &kind, &svc) !=
        KERN_SUCCESS) {
        return KERN_INVALID_NAME;
    }
    if (kind != IOKIT_SERVICE_KIND_IOSURFACE_ROOT) {
        return KERN_INVALID_ARGUMENT;
    }
    /* Candidate A: memory_type is the surface id. */
    idx = is_find(memory_type);
    if (idx < 0) return KERN_INVALID_ARGUMENT;
    if (!g_surfaces[idx].addr) return KERN_FAILURE;
    *addr_out = (uint64_t)(uintptr_t)g_surfaces[idx].addr;
    *size_out = g_surfaces[idx].map_size;
    return KERN_SUCCESS;
}
