/* xpcwire.c - freestanding libxpc wire-format library for ioswine.
 *
 * No libc is used: all helpers are local and xw_ prefixed, all
 * multi-byte values go through byte-wise little-endian helpers, and
 * every reader access is bounds checked against the buffer end.
 * See xpcwire.h for the format.
 */
#include "xpcwire.h"

/* ------------------------------------------------------------------ */
/* Small freestanding helpers                                          */
/* ------------------------------------------------------------------ */

static uint32_t xw_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t xw_rd64(const uint8_t *p) {
    return (uint64_t)xw_rd32(p) | ((uint64_t)xw_rd32(p + 4) << 32);
}

static void xw_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void xw_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void xw_wr64(uint8_t *p, uint64_t v) {
    xw_wr32(p, (uint32_t)v);
    xw_wr32(p + 4, (uint32_t)(v >> 32));
}

/* Bytes of zero padding needed to bring n up to a multiple of 4. */
static uint32_t xw_pad4(uint32_t n) {
    return (4u - (n & 3u)) & 3u;
}

/* Double bit patterns via byte access (no union punning, no libc). */
static uint64_t xw_double_bits(double d) {
    const uint8_t *p = (const uint8_t *)&d;
    uint64_t u = 0;
    int i;
    for (i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    return u;
}

static double xw_bits_double(uint64_t u) {
    double d;
    uint8_t *p = (uint8_t *)&d;
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (8 * i));
    return d;
}

static int xw_key_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

int xw_builder_init(xw_builder *b, uint8_t *buf, uint32_t cap) {
    if (!b || !buf) return XW_ERR_BAD_TYPE;
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->err = XW_OK;
    b->depth = 0;
    b->ndescs = 0;
    if (cap < 8) {
        b->err = XW_ERR_NOSPACE;
        return b->err;
    }
    xw_wr32(buf, XW_MAGIC_CPX);
    xw_wr32(buf + 4, XW_VERSION);
    b->len = 8;
    return XW_OK;
}

uint32_t xw_builder_len(const xw_builder *b) {
    return b ? b->len : 0;
}

int xw_builder_error(const xw_builder *b) {
    return b ? b->err : XW_ERR_BAD_TYPE;
}

static int xw_b_reserve(xw_builder *b, uint32_t n) {
    if (b->err) return 0;
    if (n > b->cap - b->len) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    return 1;
}

static int xw_b_put_u32(xw_builder *b, uint32_t v) {
    if (!xw_b_reserve(b, 4)) return 0;
    xw_wr32(b->buf + b->len, v);
    b->len += 4;
    return 1;
}

static int xw_b_put_u64(xw_builder *b, uint64_t v) {
    if (!xw_b_reserve(b, 8)) return 0;
    xw_wr64(b->buf + b->len, v);
    b->len += 8;
    return 1;
}

static int xw_b_put_bytes(xw_builder *b, const void *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i;
    if (!xw_b_reserve(b, n)) return 0;
    for (i = 0; i < n; i++) b->buf[b->len + i] = s[i];
    b->len += n;
    return 1;
}

static int xw_b_put_zero(xw_builder *b, uint32_t n) {
    uint32_t i;
    if (!xw_b_reserve(b, n)) return 0;
    for (i = 0; i < n; i++) b->buf[b->len + i] = 0;
    b->len += n;
    return 1;
}

static int xw_b_record_desc(xw_builder *b, uint32_t dtype, uint32_t disp,
                            uint64_t obj, uint32_t size) {
    if (b->err) return 0;
    if (b->ndescs >= XW_MAX_DESCS) {
        b->err = XW_ERR_OVERFLOW;
        return 0;
    }
    b->descs[b->ndescs].dtype = dtype;
    b->descs[b->ndescs].disposition = disp;
    b->descs[b->ndescs].obj = obj;
    b->descs[b->ndescs].size = size;
    b->ndescs++;
    return 1;
}

/* Write a dictionary key: NUL terminated bytes padded to 4. The key
 * scan is bounded by the remaining buffer space, so a caller string
 * without a NUL in range fails with NOSPACE instead of running away.
 * Requires the innermost container to be a dictionary and bumps its
 * entry count on success.
 */
static int xw_b_put_key(xw_builder *b, const char *key) {
    uint32_t avail, klen = 0, total, i;
    if (b->err) return 0;
    if (!key || b->depth == 0 ||
        b->stack[b->depth - 1].kind != XW_TYPE_DICTIONARY) {
        b->err = XW_ERR_BAD_TYPE;
        return 0;
    }
    avail = b->cap - b->len;
    while (klen < avail && key[klen] != '\0') klen++;
    if (klen >= avail) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    klen++; /* include the NUL */
    total = (klen + 3u) & ~3u;
    if (total > avail) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    for (i = 0; i < klen; i++) b->buf[b->len + i] = (uint8_t)key[i];
    for (i = klen; i < total; i++) b->buf[b->len + i] = 0;
    b->len += total;
    b->stack[b->depth - 1].count++;
    return 1;
}

/* Requires the innermost container to be an array. The caller bumps
 * the item count after the value is fully written.
 */
static int xw_b_array_top(xw_builder *b) {
    if (b->err) return 0;
    if (b->depth == 0 || b->stack[b->depth - 1].kind != XW_TYPE_ARRAY) {
        b->err = XW_ERR_BAD_TYPE;
        return 0;
    }
    return 1;
}

static int xw_b_start_container(xw_builder *b, uint32_t kind) {
    uint32_t pos;
    if (b->err) return b->err;
    if (b->depth >= XW_MAX_DEPTH) {
        b->err = XW_ERR_OVERFLOW;
        return b->err;
    }
    pos = b->len;
    if (!xw_b_reserve(b, 12)) return b->err;
    xw_wr32(b->buf + pos, kind);
    xw_wr32(b->buf + pos + 4, 0); /* size, patched by the matching end */
    xw_wr32(b->buf + pos + 8, 0); /* count, patched by the matching end */
    b->len += 12;
    b->stack[b->depth].size_off = pos + 4;
    b->stack[b->depth].count_off = pos + 8;
    b->stack[b->depth].count = 0;
    b->stack[b->depth].kind = kind;
    b->depth++;
    return XW_OK;
}

static int xw_b_end_container(xw_builder *b, uint32_t kind) {
    uint32_t size_off, count_off, count, size;
    if (b->err) return b->err;
    if (b->depth == 0 || b->stack[b->depth - 1].kind != kind) {
        b->err = XW_ERR_BAD_TYPE;
        return b->err;
    }
    size_off = b->stack[b->depth - 1].size_off;
    count_off = b->stack[b->depth - 1].count_off;
    count = b->stack[b->depth - 1].count;
    b->depth--;
    size = b->len - count_off; /* count word through the last entry */
    xw_wr32(b->buf + size_off, size);
    xw_wr32(b->buf + count_off, count);
    return XW_OK;
}

int xw_dict_begin(xw_builder *b) {
    if (b->err) return b->err;
    if (b->depth > 0) {
        if (b->stack[b->depth - 1].kind == XW_TYPE_DICTIONARY) {
            b->err = XW_ERR_BAD_TYPE; /* need a key: use dict_add_dict_begin */
            return b->err;
        }
        b->stack[b->depth - 1].count++; /* array element */
    }
    return xw_b_start_container(b, XW_TYPE_DICTIONARY);
}

int xw_dict_end(xw_builder *b) {
    return xw_b_end_container(b, XW_TYPE_DICTIONARY);
}

int xw_array_begin(xw_builder *b) {
    if (b->err) return b->err;
    if (b->depth > 0) {
        if (b->stack[b->depth - 1].kind == XW_TYPE_DICTIONARY) {
            b->err = XW_ERR_BAD_TYPE; /* need a key: use dict_add_array_begin */
            return b->err;
        }
        b->stack[b->depth - 1].count++; /* array element */
    }
    return xw_b_start_container(b, XW_TYPE_ARRAY);
}

int xw_array_end(xw_builder *b) {
    return xw_b_end_container(b, XW_TYPE_ARRAY);
}

int xw_dict_add_dict_begin(xw_builder *b, const char *key) {
    if (!xw_b_put_key(b, key)) return b->err;
    return xw_b_start_container(b, XW_TYPE_DICTIONARY);
}

int xw_dict_add_array_begin(xw_builder *b, const char *key) {
    if (!xw_b_put_key(b, key)) return b->err;
    return xw_b_start_container(b, XW_TYPE_ARRAY);
}

/* Value payload writers. Each writes only the value bytes (the caller
 * handles the key and the container count).
 */
static int xw_b_val_null(xw_builder *b) {
    return xw_b_put_u32(b, XW_TYPE_NULL);
}

static int xw_b_val_bool(xw_builder *b, int v) {
    if (!xw_b_put_u32(b, XW_TYPE_BOOL)) return 0;
    if (!xw_b_reserve(b, 4)) return 0;
    b->buf[b->len] = v ? (uint8_t)1 : (uint8_t)0;
    b->buf[b->len + 1] = 0;
    b->buf[b->len + 2] = 0;
    b->buf[b->len + 3] = 0;
    b->len += 4;
    return 1;
}

static int xw_b_val_int64(xw_builder *b, int64_t v) {
    if (!xw_b_put_u32(b, XW_TYPE_INT64)) return 0;
    return xw_b_put_u64(b, (uint64_t)v);
}

static int xw_b_val_uint64(xw_builder *b, uint64_t v) {
    if (!xw_b_put_u32(b, XW_TYPE_UINT64)) return 0;
    return xw_b_put_u64(b, v);
}

static int xw_b_val_double(xw_builder *b, double v) {
    if (!xw_b_put_u32(b, XW_TYPE_DOUBLE)) return 0;
    return xw_b_put_u64(b, xw_double_bits(v));
}

static int xw_b_val_string(xw_builder *b, const char *s) {
    uint32_t avail, slen = 0;
    uint64_t need;
    if (b->err) return 0;
    if (!s) {
        b->err = XW_ERR_BAD_TYPE;
        return 0;
    }
    avail = b->cap - b->len;
    while (slen < avail && s[slen] != '\0') slen++;
    if (slen >= avail) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    slen++; /* include the NUL: the wire length counts it */
    need = 8u + (uint64_t)slen + xw_pad4(slen);
    if (need > avail) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    if (!xw_b_put_u32(b, XW_TYPE_STRING)) return 0;
    if (!xw_b_put_u32(b, slen)) return 0;
    if (!xw_b_put_bytes(b, s, slen)) return 0;
    if (!xw_b_put_zero(b, xw_pad4(slen))) return 0;
    return 1;
}

static int xw_b_val_data(xw_builder *b, const void *p, uint32_t len) {
    uint64_t need;
    if (b->err) return 0;
    if (len > 0 && !p) {
        b->err = XW_ERR_BAD_TYPE;
        return 0;
    }
    need = 8u + (uint64_t)len + xw_pad4(len);
    if (need > (uint64_t)(b->cap - b->len)) {
        b->err = XW_ERR_NOSPACE;
        return 0;
    }
    if (!xw_b_put_u32(b, XW_TYPE_DATA)) return 0;
    if (!xw_b_put_u32(b, len)) return 0;
    if (!xw_b_put_bytes(b, p, len)) return 0;
    if (!xw_b_put_zero(b, xw_pad4(len))) return 0;
    return 1;
}

static int xw_b_val_uuid(xw_builder *b, const uint8_t uuid[16]) {
    if (b->err) return 0;
    if (!uuid) {
        b->err = XW_ERR_BAD_TYPE;
        return 0;
    }
    if (!xw_b_put_u32(b, XW_TYPE_UUID)) return 0;
    return xw_b_put_bytes(b, uuid, 16);
}

static int xw_b_val_date(xw_builder *b, int64_t ns) {
    if (!xw_b_put_u32(b, XW_TYPE_DATE)) return 0;
    return xw_b_put_u64(b, (uint64_t)ns);
}

/* Port carrying value: type word only in the body, one port descriptor
 * recorded for the envelope layer.
 */
static int xw_b_val_port(xw_builder *b, uint32_t type, uint64_t obj,
                         uint32_t disp, uint32_t size) {
    if (!xw_b_put_u32(b, type)) return 0;
    return xw_b_record_desc(b, XW_DESC_PORT, disp, obj, size);
}

/* OOL data value: type + length in the body, no inline bytes; the
 * bytes travel in a memory entry port recorded as a descriptor.
 */
static int xw_b_val_data_ool(xw_builder *b, uint32_t len, uint64_t obj,
                             uint32_t disp) {
    if (!xw_b_put_u32(b, XW_TYPE_DATA)) return 0;
    if (!xw_b_put_u32(b, len)) return 0;
    return xw_b_record_desc(b, XW_DESC_PORT, disp, obj, len);
}

/* Shmem value: type + region length in the body, one port descriptor
 * (the memory entry port) recorded for the envelope layer.
 */
static int xw_b_val_shmem(xw_builder *b, uint64_t obj, uint32_t disp,
                          uint32_t len) {
    if (!xw_b_put_u32(b, XW_TYPE_SHMEM)) return 0;
    if (!xw_b_put_u32(b, len)) return 0;
    return xw_b_record_desc(b, XW_DESC_PORT, disp, obj, len);
}

/* Unkeyed adders (array elements). */
int xw_add_null(xw_builder *b) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_null(b)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_bool(xw_builder *b, int v) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_bool(b, v)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_int64(xw_builder *b, int64_t v) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_int64(b, v)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_uint64(xw_builder *b, uint64_t v) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_uint64(b, v)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_double(xw_builder *b, double v) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_double(b, v)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_string(xw_builder *b, const char *s) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_string(b, s)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_data(xw_builder *b, const void *p, uint32_t len) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_data(b, p, len)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_data_ool(xw_builder *b, uint32_t len, uint64_t obj, uint32_t disp) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_data_ool(b, len, obj, disp)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_uuid(xw_builder *b, const uint8_t uuid[16]) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_uuid(b, uuid)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_date(xw_builder *b, int64_t ns_since_unix_epoch) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_date(b, ns_since_unix_epoch)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_fd(xw_builder *b, uint64_t obj, uint32_t disp) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_FD, obj, disp, 0)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_mach_send(xw_builder *b, uint64_t obj, uint32_t disp) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_SEND, obj, disp, 0)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_mach_recv(xw_builder *b, uint64_t obj, uint32_t disp) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_RECV, obj, disp, 0)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_mach_send_once(xw_builder *b, uint64_t obj, uint32_t disp) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_SEND_ONCE, obj, disp, 0)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

int xw_add_shmem(xw_builder *b, uint64_t obj, uint32_t disp, uint32_t len) {
    if (!xw_b_array_top(b)) return b->err;
    if (!xw_b_val_shmem(b, obj, disp, len)) return b->err;
    b->stack[b->depth - 1].count++;
    return XW_OK;
}

/* Keyed adders (dictionary entries). xw_b_put_key bumps the count. */
int xw_dict_add_null(xw_builder *b, const char *key) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_null(b)) return b->err;
    return XW_OK;
}

int xw_dict_add_bool(xw_builder *b, const char *key, int v) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_bool(b, v)) return b->err;
    return XW_OK;
}

int xw_dict_add_int64(xw_builder *b, const char *key, int64_t v) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_int64(b, v)) return b->err;
    return XW_OK;
}

int xw_dict_add_uint64(xw_builder *b, const char *key, uint64_t v) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_uint64(b, v)) return b->err;
    return XW_OK;
}

int xw_dict_add_double(xw_builder *b, const char *key, double v) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_double(b, v)) return b->err;
    return XW_OK;
}

int xw_dict_add_string(xw_builder *b, const char *key, const char *s) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_string(b, s)) return b->err;
    return XW_OK;
}

int xw_dict_add_data(xw_builder *b, const char *key, const void *p, uint32_t len) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_data(b, p, len)) return b->err;
    return XW_OK;
}

int xw_dict_add_data_ool(xw_builder *b, const char *key, uint32_t len,
                         uint64_t obj, uint32_t disp) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_data_ool(b, len, obj, disp)) return b->err;
    return XW_OK;
}

int xw_dict_add_uuid(xw_builder *b, const char *key, const uint8_t uuid[16]) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_uuid(b, uuid)) return b->err;
    return XW_OK;
}

int xw_dict_add_date(xw_builder *b, const char *key, int64_t ns_since_unix_epoch) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_date(b, ns_since_unix_epoch)) return b->err;
    return XW_OK;
}

int xw_dict_add_fd(xw_builder *b, const char *key, uint64_t obj, uint32_t disp) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_FD, obj, disp, 0)) return b->err;
    return XW_OK;
}

int xw_dict_add_mach_send(xw_builder *b, const char *key, uint64_t obj, uint32_t disp) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_SEND, obj, disp, 0)) return b->err;
    return XW_OK;
}

int xw_dict_add_mach_recv(xw_builder *b, const char *key, uint64_t obj, uint32_t disp) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_RECV, obj, disp, 0)) return b->err;
    return XW_OK;
}

int xw_dict_add_mach_send_once(xw_builder *b, const char *key, uint64_t obj, uint32_t disp) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_port(b, XW_TYPE_MACH_SEND_ONCE, obj, disp, 0)) return b->err;
    return XW_OK;
}

int xw_dict_add_shmem(xw_builder *b, const char *key, uint64_t obj, uint32_t disp,
                      uint32_t len) {
    if (!xw_b_put_key(b, key)) return b->err;
    if (!xw_b_val_shmem(b, obj, disp, len)) return b->err;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

/* Parse one value at *pos_io (inside [end) bounds), advance *pos_io
 * past it, and thread the descriptor cursor *next_desc_io. Containers
 * are validated eagerly: their children are scanned recursively so a
 * container value only ever comes back when its whole subtree is
 * well formed and its descriptor use is accounted for. depth is the
 * nesting level (root is 0) and is capped at XW_MAX_DEPTH.
 */
static int xw_scan_one(const uint8_t **pos_io, const uint8_t *end,
                       const xw_desc *descs, uint32_t ndescs,
                       uint32_t *next_desc_io, xw_value *out,
                       uint32_t depth) {
    const uint8_t *p = *pos_io;
    uint32_t type;

    if (depth > XW_MAX_DEPTH) return XW_ERR_OVERFLOW;
    if ((uint32_t)(end - p) < 4) return XW_ERR_TRUNCATED;
    type = xw_rd32(p);
    p += 4;

    out->type = type;
    out->i64 = 0;
    out->u64 = 0;
    out->dbl = 0.0;
    out->boolean = 0;
    out->ptr = NULL;
    out->len = 0;
    out->desc = XW_NO_DESC;
    out->body = NULL;
    out->body_len = 0;
    out->descs = descs;
    out->ndescs = ndescs;
    out->desc_cursor = *next_desc_io;
    out->depth = depth;

    switch (type) {
    case XW_TYPE_NULL:
        break;

    case XW_TYPE_BOOL:
        if ((uint32_t)(end - p) < 4) return XW_ERR_TRUNCATED;
        out->boolean = (p[0] != 0); /* other 3 bytes are padding */
        p += 4;
        break;

    case XW_TYPE_INT64:
        if ((uint32_t)(end - p) < 8) return XW_ERR_TRUNCATED;
        out->i64 = (int64_t)xw_rd64(p);
        p += 8;
        break;

    case XW_TYPE_UINT64:
        if ((uint32_t)(end - p) < 8) return XW_ERR_TRUNCATED;
        out->u64 = xw_rd64(p);
        p += 8;
        break;

    case XW_TYPE_DOUBLE:
        if ((uint32_t)(end - p) < 8) return XW_ERR_TRUNCATED;
        out->dbl = xw_bits_double(xw_rd64(p));
        p += 8;
        break;

    case XW_TYPE_DATE:
        if ((uint32_t)(end - p) < 8) return XW_ERR_TRUNCATED;
        out->i64 = (int64_t)xw_rd64(p);
        p += 8;
        break;

    case XW_TYPE_STRING: {
        uint32_t slen, i;
        if ((uint32_t)(end - p) < 4) return XW_ERR_TRUNCATED;
        slen = xw_rd32(p);
        p += 4;
        if (slen < 1) return XW_ERR_BAD_TYPE; /* must hold at least the NUL */
        if ((uint32_t)(end - p) < slen) return XW_ERR_TRUNCATED;
        if (p[slen - 1] != 0) return XW_ERR_BAD_TYPE;
        /* Real libxpc only checks the last byte (Azad, CVE-2018-4248);
         * we also reject embedded NULs, because the getter hands out
         * a C string and a shorter-than-claimed string would silently
         * truncate it.
         */
        for (i = 0; i + 1 < slen; i++) {
            if (p[i] == 0) return XW_ERR_BAD_TYPE;
        }
        out->ptr = p;
        out->len = slen;
        /* The padding must also fit: check before advancing so no
         * pointer is ever formed past the end of the buffer.
         */
        if ((uint64_t)slen + xw_pad4(slen) > (uint64_t)(end - p))
            return XW_ERR_TRUNCATED;
        p += slen + xw_pad4(slen);
        break;
    }

    case XW_TYPE_DATA: {
        uint32_t dlen;
        uint64_t need;
        if ((uint32_t)(end - p) < 4) return XW_ERR_TRUNCATED;
        dlen = xw_rd32(p);
        p += 4;
        need = (uint64_t)dlen + xw_pad4(dlen);
        if (need <= (uint64_t)(end - p)) {
            out->ptr = p;
            out->len = dlen;
            p += (uint32_t)need;
        } else {
            /* Out-of-line data: the inline bytes are absent and the
             * bytes live in a memory entry port, which is the next
             * port descriptor. The remaining-length rule is the only
             * marker: an inline value that does not fit must be OOL.
             */
            if (*next_desc_io >= ndescs) return XW_ERR_TRUNCATED;
            if (descs[*next_desc_io].dtype != XW_DESC_PORT)
                return XW_ERR_BAD_ENVELOPE;
            out->desc = *next_desc_io;
            (*next_desc_io)++;
            out->len = dlen;
            out->ptr = NULL;
        }
        break;
    }

    case XW_TYPE_UUID:
        if ((uint32_t)(end - p) < 16) return XW_ERR_TRUNCATED;
        out->ptr = p;
        out->len = 16;
        p += 16;
        break;

    case XW_TYPE_FD:
    case XW_TYPE_MACH_SEND:
    case XW_TYPE_MACH_RECV:
    case XW_TYPE_MACH_SEND_ONCE:
    case XW_TYPE_CONNECTION:
    case XW_TYPE_ENDPOINT:
        /* Type word only in the body; consumes one port descriptor. */
        if (*next_desc_io >= ndescs) return XW_ERR_BAD_ENVELOPE;
        if (descs[*next_desc_io].dtype != XW_DESC_PORT)
            return XW_ERR_BAD_ENVELOPE;
        out->desc = *next_desc_io;
        (*next_desc_io)++;
        break;

    case XW_TYPE_SHMEM: {
        uint32_t slen;
        if ((uint32_t)(end - p) < 4) return XW_ERR_TRUNCATED;
        slen = xw_rd32(p);
        p += 4;
        if (*next_desc_io >= ndescs) return XW_ERR_BAD_ENVELOPE;
        if (descs[*next_desc_io].dtype != XW_DESC_PORT)
            return XW_ERR_BAD_ENVELOPE;
        out->desc = *next_desc_io;
        (*next_desc_io)++;
        out->len = slen;
        break;
    }

    case XW_TYPE_ARRAY:
    case XW_TYPE_DICTIONARY: {
        uint32_t csize, count, i;
        const uint8_t *count_p, *content_end, *cp;
        uint32_t cursor;
        if ((uint32_t)(end - p) < 8) return XW_ERR_TRUNCATED;
        csize = xw_rd32(p);
        count_p = p + 4;
        count = xw_rd32(count_p);
        if (csize < 4) return XW_ERR_BAD_ENVELOPE;
        /* The size word counts the count word (4 bytes) plus all
         * entries, starting at the count word. The container therefore
         * occupies [p, p + 4 + csize).
         */
        if ((uint64_t)csize + 4 > (uint64_t)(end - p))
            return XW_ERR_TRUNCATED;
        /* Every entry/item is at least one 4-byte type word. */
        if (count > csize / 4u) return XW_ERR_BAD_ENVELOPE;
        content_end = p + 4 + csize;
        cp = count_p + 4;
        cursor = *next_desc_io;
        for (i = 0; i < count; i++) {
            xw_value tmp;
            int rc;
            if (type == XW_TYPE_DICTIONARY) {
                uint32_t klen = 0, kpad;
                while (cp + klen < content_end && cp[klen] != 0) klen++;
                if (cp + klen >= content_end) return XW_ERR_TRUNCATED;
                klen++; /* include the NUL */
                kpad = (klen + 3u) & ~3u;
                if ((uint64_t)kpad > (uint64_t)(content_end - cp))
                    return XW_ERR_TRUNCATED;
                cp += kpad;
            }
            rc = xw_scan_one(&cp, content_end, descs, ndescs, &cursor,
                             &tmp, depth + 1);
            if (rc != XW_OK) return rc;
        }
        if (cp != content_end) return XW_ERR_BAD_ENVELOPE;
        out->body = count_p;
        out->body_len = csize;
        *next_desc_io = cursor;
        p = content_end;
        break;
    }

    default:
        return XW_ERR_BAD_TYPE;
    }

    *pos_io = p;
    return XW_OK;
}

int xw_reader_init(xw_reader *r, const uint8_t *buf, uint32_t len,
                   const xw_desc *descs, uint32_t ndescs) {
    const uint8_t *p, *end;
    uint32_t cursor = 0, root_type;
    xw_value tmp;
    int rc;

    if (!r || !buf) return XW_ERR_BAD_TYPE;
    if (len < 8) return XW_ERR_TRUNCATED;
    if (xw_rd32(buf) != XW_MAGIC_CPX) return XW_ERR_BAD_MAGIC;
    if (xw_rd32(buf + 4) != XW_VERSION) return XW_ERR_BAD_VERSION;
    if (len < 12) return XW_ERR_TRUNCATED;
    root_type = xw_rd32(buf + 8);
    if (root_type != XW_TYPE_DICTIONARY && root_type != XW_TYPE_ARRAY &&
        root_type != XW_TYPE_ENDPOINT) {
        return XW_ERR_BAD_TYPE;
    }
    r->buf = buf;
    r->len = len;
    r->descs = descs;
    r->ndescs = ndescs;

    /* Full validation pass: every value, size, count and descriptor
     * use in the message must check out before the caller sees it.
     */
    p = buf + 8;
    end = buf + len;
    rc = xw_scan_one(&p, end, descs, ndescs, &cursor, &tmp, 0);
    if (rc != XW_OK) return rc;
    if (p != end) return XW_ERR_BAD_ENVELOPE; /* trailing bytes */
    if (cursor != ndescs) return XW_ERR_BAD_ENVELOPE; /* unused descriptors */
    return XW_OK;
}

int xw_reader_root(xw_reader *r, xw_value *out) {
    const uint8_t *p, *end;
    uint32_t cursor = 0;
    if (!r || !out || !r->buf) return XW_ERR_BAD_TYPE;
    p = r->buf + 8;
    end = r->buf + r->len;
    return xw_scan_one(&p, end, r->descs, r->ndescs, &cursor, out, 0);
}

/* Scan one dictionary entry at *pos_io (inside [end)): read the key,
 * then the value. On success *key_out points at the key inside the
 * buffer and *pos_io is past the value.
 */
static int xw_scan_dict_entry(const uint8_t **pos_io, const uint8_t *end,
                              const xw_desc *descs, uint32_t ndescs,
                              uint32_t *next_desc_io, const char **key_out,
                              xw_value *out, uint32_t depth) {
    const uint8_t *p = *pos_io;
    uint32_t klen = 0, kpad;
    int rc;

    if (p >= end) return XW_ERR_TRUNCATED;
    while (p + klen < end && p[klen] != 0) klen++;
    if (p + klen >= end) return XW_ERR_TRUNCATED;
    klen++;
    kpad = (klen + 3u) & ~3u;
    if ((uint64_t)kpad > (uint64_t)(end - p)) return XW_ERR_TRUNCATED;
    *key_out = (const char *)p;
    p += kpad;
    rc = xw_scan_one(&p, end, descs, ndescs, next_desc_io, out, depth);
    if (rc != XW_OK) return rc;
    *pos_io = p;
    return XW_OK;
}

int xw_dict_find_v(const xw_value *dict, const char *key, xw_value *out) {
    const uint8_t *p, *end;
    uint32_t count, cursor, i;

    if (!dict || !key || !out) return XW_ERR_BAD_TYPE;
    if (dict->type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    if (!dict->body || dict->body_len < 4) return XW_ERR_BAD_ENVELOPE;
    p = dict->body;
    end = dict->body + dict->body_len;
    count = xw_rd32(p);
    p += 4;
    cursor = dict->desc_cursor;
    for (i = 0; i < count; i++) {
        const char *k = NULL;
        xw_value v;
        int rc = xw_scan_dict_entry(&p, end, dict->descs, dict->ndescs,
                                    &cursor, &k, &v, dict->depth + 1);
        if (rc != XW_OK) return rc;
        if (xw_key_equal(k, key)) {
            *out = v;
            return XW_OK;
        }
    }
    return XW_ERR_NOT_FOUND;
}

int xw_dict_find(xw_reader *r, const char *key, xw_value *out) {
    xw_value root;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    return xw_dict_find_v(&root, key, out);
}

int xw_dict_iter_init_v(const xw_value *dict, xw_dict_iter *it) {
    if (!dict || !it) return XW_ERR_BAD_TYPE;
    if (dict->type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    if (!dict->body || dict->body_len < 4) return XW_ERR_BAD_ENVELOPE;
    it->pos = dict->body + 4;
    it->end = dict->body + dict->body_len;
    it->remaining = xw_rd32(dict->body);
    it->next_desc = dict->desc_cursor;
    it->descs = dict->descs;
    it->ndescs = dict->ndescs;
    it->depth = dict->depth + 1;
    return XW_OK;
}

int xw_dict_iter_init(xw_reader *r, xw_dict_iter *it) {
    xw_value root;
    int rc;
    if (!r || !it) return XW_ERR_BAD_TYPE;
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    return xw_dict_iter_init_v(&root, it);
}

int xw_dict_next(xw_dict_iter *it, const char **key_out, xw_value *out) {
    const char *k = NULL;
    uint32_t cursor;
    int rc;

    if (!it || !key_out || !out) return XW_ERR_BAD_TYPE;
    if (it->remaining == 0) return 0;
    cursor = it->next_desc;
    rc = xw_scan_dict_entry(&it->pos, it->end, it->descs, it->ndescs,
                            &cursor, &k, out, it->depth);
    if (rc != XW_OK) return rc;
    it->next_desc = cursor;
    it->remaining--;
    *key_out = k;
    return 1;
}

int xw_array_len_v(const xw_value *arr, uint32_t *out) {
    if (!arr || !out) return XW_ERR_BAD_TYPE;
    if (arr->type != XW_TYPE_ARRAY) return XW_ERR_BAD_TYPE;
    if (!arr->body || arr->body_len < 4) return XW_ERR_BAD_ENVELOPE;
    *out = xw_rd32(arr->body);
    return XW_OK;
}

int xw_array_len(xw_reader *r, uint32_t *out) {
    xw_value root;
    int rc;
    if (!r || !out) return XW_ERR_BAD_TYPE;
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    return xw_array_len_v(&root, out);
}

int xw_array_at_v(const xw_value *arr, uint32_t idx, xw_value *out) {
    const uint8_t *p, *end;
    uint32_t count, cursor, i;

    if (!arr || !out) return XW_ERR_BAD_TYPE;
    if (arr->type != XW_TYPE_ARRAY) return XW_ERR_BAD_TYPE;
    if (!arr->body || arr->body_len < 4) return XW_ERR_BAD_ENVELOPE;
    p = arr->body + 4;
    end = arr->body + arr->body_len;
    count = xw_rd32(arr->body);
    if (idx >= count) return XW_ERR_NOT_FOUND;
    cursor = arr->desc_cursor;
    for (i = 0; i <= idx; i++) {
        xw_value v;
        int rc = xw_scan_one(&p, end, arr->descs, arr->ndescs, &cursor,
                             &v, arr->depth + 1);
        if (rc != XW_OK) return rc;
        if (i == idx) {
            *out = v;
            return XW_OK;
        }
    }
    return XW_ERR_NOT_FOUND; /* unreachable */
}

int xw_array_at(xw_reader *r, uint32_t idx, xw_value *out) {
    xw_value root;
    int rc;
    if (!r || !out) return XW_ERR_BAD_TYPE;
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    return xw_array_at_v(&root, idx, out);
}

/* Typed getters. */
int xw_as_int64(const xw_value *v, int64_t *out) {
    if (!v || !out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_INT64) return XW_ERR_BAD_TYPE;
    *out = v->i64;
    return XW_OK;
}

int xw_as_uint64(const xw_value *v, uint64_t *out) {
    if (!v || !out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_UINT64) return XW_ERR_BAD_TYPE;
    *out = v->u64;
    return XW_OK;
}

int xw_as_bool(const xw_value *v, int *out) {
    if (!v || !out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_BOOL) return XW_ERR_BAD_TYPE;
    *out = v->boolean;
    return XW_OK;
}

int xw_as_double(const xw_value *v, double *out) {
    if (!v || !out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_DOUBLE) return XW_ERR_BAD_TYPE;
    *out = v->dbl;
    return XW_OK;
}

int xw_as_string(const xw_value *v, const char **out, uint32_t *len_out) {
    if (!v || !out || !len_out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_STRING) return XW_ERR_BAD_TYPE;
    *out = (const char *)v->ptr;
    *len_out = v->len; /* includes the NUL */
    return XW_OK;
}

int xw_as_data(const xw_value *v, const uint8_t **ptr_out, uint32_t *len_out,
               uint32_t *desc_out) {
    if (!v || !ptr_out || !len_out || !desc_out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_DATA) return XW_ERR_BAD_TYPE;
    *ptr_out = v->ptr; /* NULL for OOL data */
    *len_out = v->len;
    *desc_out = v->desc; /* XW_NO_DESC for inline data */
    return XW_OK;
}

int xw_as_uuid(const xw_value *v, const uint8_t **out) {
    if (!v || !out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_UUID) return XW_ERR_BAD_TYPE;
    *out = v->ptr;
    return XW_OK;
}

int xw_as_date(const xw_value *v, int64_t *ns_out) {
    if (!v || !ns_out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_DATE) return XW_ERR_BAD_TYPE;
    *ns_out = v->i64;
    return XW_OK;
}

static int xw_as_port(const xw_value *v, uint32_t want, uint32_t *desc_out) {
    if (!v || !desc_out) return XW_ERR_BAD_TYPE;
    if (v->type != want) return XW_ERR_BAD_TYPE;
    if (v->desc == XW_NO_DESC) return XW_ERR_BAD_ENVELOPE;
    *desc_out = v->desc;
    return XW_OK;
}

int xw_as_fd(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_FD, desc_out);
}

int xw_as_mach_send(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_MACH_SEND, desc_out);
}

int xw_as_mach_recv(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_MACH_RECV, desc_out);
}

int xw_as_mach_send_once(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_MACH_SEND_ONCE, desc_out);
}

int xw_as_shmem(const xw_value *v, uint32_t *desc_out, uint32_t *len_out) {
    if (!v || !desc_out || !len_out) return XW_ERR_BAD_TYPE;
    if (v->type != XW_TYPE_SHMEM) return XW_ERR_BAD_TYPE;
    if (v->desc == XW_NO_DESC) return XW_ERR_BAD_ENVELOPE;
    *desc_out = v->desc;
    *len_out = v->len;
    return XW_OK;
}

int xw_as_connection(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_CONNECTION, desc_out);
}

int xw_as_endpoint(const xw_value *v, uint32_t *desc_out) {
    return xw_as_port(v, XW_TYPE_ENDPOINT, desc_out);
}

/* ------------------------------------------------------------------ */
/* Mach message envelope                                               */
/* ------------------------------------------------------------------ */

int xw_envelope_parse(const void *msg, uint32_t size, xw_envelope *env) {
    const uint8_t *p, *end, *d;
    uint32_t count, i;
    int first;

    if (!msg || !env) return XW_ERR_BAD_ENVELOPE;
    if (size < 24) return XW_ERR_BAD_ENVELOPE;
    p = (const uint8_t *)msg;
    env->bits = xw_rd32(p);
    env->size = xw_rd32(p + 4);
    env->remote = xw_rd32(p + 8);
    env->local = xw_rd32(p + 12);
    env->voucher = xw_rd32(p + 16);
    env->id = xw_rd32(p + 20);
    env->ndescs = 0;
    env->payload = NULL;
    env->payload_len = 0;
    env->body_is_ool = 0;
    if (env->size < 24 || env->size > size) return XW_ERR_BAD_ENVELOPE;
    end = p + env->size;

    if (!(env->bits & XW_MSGH_BITS_COMPLEX)) {
        env->payload = p + 24;
        env->payload_len = env->size - 24;
        return XW_OK;
    }

    if (env->size < 28) return XW_ERR_BAD_ENVELOPE;
    count = xw_rd32(p + 24);
    if (count > XW_MAX_DESCS) return XW_ERR_OVERFLOW;
    d = p + 28;
    first = 1;
    for (i = 0; i < count; i++) {
        uint8_t dtype;
        if ((uint32_t)(d - p) + 12 > env->size) return XW_ERR_BAD_ENVELOPE;
        dtype = d[11];
        if (dtype == XW_DESC_PORT) {
            xw_desc *dst = &env->descs[env->ndescs++];
            dst->dtype = dtype;
            dst->disposition = d[10];
            dst->obj = xw_rd64(d); /* name, or a smuggled pointer */
            dst->size = 0;
            d += 12;
        } else if (dtype == XW_DESC_OOL || dtype == XW_DESC_OOL_VOLATILE) {
            uint64_t addr;
            uint32_t osize;
            if ((uint32_t)(d - p) + 16 > env->size) return XW_ERR_BAD_ENVELOPE;
            if (!first) return XW_ERR_BAD_ENVELOPE; /* OOL body is first only */
            addr = xw_rd64(d);
            osize = xw_rd32(d + 12);
            /* The address is filled in by the kernel when the message
             * is received (the region is mapped into the receiver),
             * so it is trusted here, not a sender controlled pointer.
             */
            env->payload = (const uint8_t *)(uintptr_t)addr;
            env->payload_len = osize;
            env->body_is_ool = 1;
            d += 16;
        } else {
            /* OOL ports and guarded ports: unsupported, fail closed. */
            return XW_ERR_BAD_ENVELOPE;
        }
        first = 0;
    }
    if (!env->body_is_ool) {
        env->payload = d;
        env->payload_len = (uint32_t)(end - d);
    }
    return XW_OK;
}

int xw_envelope_build_reply(const xw_envelope *req, uint8_t *out, uint32_t cap,
                            const uint8_t *body, uint32_t body_len,
                            const xw_desc *descs, uint32_t ndescs,
                            uint32_t *out_len) {
    uint64_t total;
    uint32_t bits, i;
    uint8_t *d;

    if (!req || !out || !out_len) return XW_ERR_BAD_ENVELOPE;
    if (!body && body_len > 0) return XW_ERR_BAD_TYPE;
    if (ndescs > XW_MAX_DESCS) return XW_ERR_OVERFLOW;
    total = 24u + (uint64_t)body_len;
    if (ndescs > 0) total += 4u + 12u * (uint64_t)ndescs;
    if (total > cap) return XW_ERR_NOSPACE;

    bits = XW_MSG_TYPE_MOVE_SEND_ONCE; /* remote bits: the reply port */
    if (ndescs > 0) bits |= XW_MSGH_BITS_COMPLEX;
    xw_wr32(out, bits);
    xw_wr32(out + 4, (uint32_t)total);
    xw_wr32(out + 8, req->local);  /* remote: the request's reply port */
    xw_wr32(out + 12, 0);         /* local: none */
    xw_wr32(out + 16, 0);         /* voucher: none */
    xw_wr32(out + 20, XW_MSGH_ID_XPC_REPLY);

    d = out + 24;
    if (ndescs > 0) {
        xw_wr32(d, ndescs);
        d += 4;
        for (i = 0; i < ndescs; i++) {
            xw_wr64(d, descs[i].obj);
            xw_wr16(d + 8, 0);
            d[10] = (uint8_t)descs[i].disposition;
            d[11] = (uint8_t)XW_DESC_PORT;
            d += 12;
        }
    }
    for (i = 0; i < body_len; i++) d[i] = body[i];
    *out_len = (uint32_t)total;
    return XW_OK;
}
