/*
 * bplist.c: minimal binary plist (bplist00) reader/writer for ioswine.
 *
 * Freestanding, arena-backed, no libc: only <stdint.h> and <stddef.h>
 * are used, with static helpers for the few string and memory
 * operations needed. See bplist.h for the format notes, the
 * BP_UINT and date caveats, and the error codes.
 *
 * Reader safety: every read goes through explicit bounds checks
 * against the object region (bytes [8, offset_table_offset) of the
 * buffer). The object table is a flat list indexed from the trailer;
 * refs are validated before use, nesting is capped at BP_MAX_DEPTH,
 * and all strings and data are copied into the caller arena, so a
 * truncated or hostile buffer can return an error but never reads
 * or writes out of bounds.
 *
 * Writer layout: the root is object 0 and every other object
 * follows in pre-order (a dict entry's key string object comes
 * immediately before its value subtree), so the trailer's top
 * object is always 0. offsetIntSize is always 4; objectRefSize is
 * 1, 2 or 4 depending on the object count.
 *
 * No em dashes are used in this file.
 */
#include "bplist.h"

/* ------------------------------------------------------------------ */
/* Small static helpers (freestanding: no libc).                       */
/* ------------------------------------------------------------------ */

static void bp_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void bp_memset(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int bp_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static uint32_t bp_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int bp_strcmp(const char *a, const char *b) {
    uint32_t i = 0;
    for (;;) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
        i++;
    }
}

/* Double and float bit casts (union punning; no -Werror trip). */
static uint64_t bp_double_bits(double d) {
    union { double d; uint64_t u; } v;
    v.d = d;
    return v.u;
}

static double bp_bits_double(uint64_t u) {
    union { double d; uint64_t u; } v;
    v.u = u;
    return v.d;
}

static float bp_bits_float(uint32_t u) {
    union { float f; uint32_t u; } v;
    v.u = u;
    return v.f;
}

/* Seconds between the Unix epoch and the Apple reference date
 * (2001-01-01 00:00:00 UTC). */
#define BP_APPLE_EPOCH_OFFSET 978307200.0

/* ------------------------------------------------------------------ */
/* Arena.                                                              */
/* ------------------------------------------------------------------ */

void bp_arena_init(bp_arena *a, uint8_t *buf, uint32_t cap) {
    a->buf = buf;
    a->cap = cap;
    a->len = 0;
}

static void *bp_arena_alloc(bp_arena *a, uint32_t n, uint32_t align) {
    uint64_t off = (uint64_t)a->len + (uint64_t)(align - 1u);
    off &= ~((uint64_t)align - 1u);
    if (off > (uint64_t)a->cap) return NULL;
    if ((uint64_t)n > (uint64_t)a->cap - off) return NULL;
    a->len = (uint32_t)(off + (uint64_t)n);
    return a->buf + off;
}

/* ------------------------------------------------------------------ */
/* Constructors and container helpers.                                 */
/* ------------------------------------------------------------------ */

bp_value *bp_new(bp_arena *a, uint32_t type) {
    bp_value *v;
    if (a == NULL) return NULL;
    v = (bp_value *)bp_arena_alloc(a, (uint32_t)sizeof(bp_value), 8u);
    if (v == NULL) return NULL;
    bp_memset(v, 0, (uint32_t)sizeof(*v));
    v->type = type;
    return v;
}

bp_value *bp_new_string(bp_arena *a, const char *s) {
    bp_value *v;
    uint32_t sl;
    uint8_t *dst;
    if (s == NULL) return NULL;
    sl = bp_strlen(s);
    if (sl >= 0xffffffffu) return NULL;
    v = bp_new(a, BP_STRING);
    if (v == NULL) return NULL;
    dst = (uint8_t *)bp_arena_alloc(a, sl + 1u, 1u);
    if (dst == NULL) return NULL;
    bp_memcpy(dst, s, sl + 1u);
    v->s = dst;
    v->len = sl + 1u;
    return v;
}

bp_value *bp_new_int(bp_arena *a, int64_t val) {
    bp_value *v = bp_new(a, BP_INT);
    if (v == NULL) return NULL;
    v->i = val;
    return v;
}

bp_value *bp_new_uint(bp_arena *a, uint64_t val) {
    bp_value *v = bp_new(a, BP_UINT);
    if (v == NULL) return NULL;
    v->u = val;
    return v;
}

bp_value *bp_new_bool(bp_arena *a, int b) {
    bp_value *v = bp_new(a, BP_BOOL);
    if (v == NULL) return NULL;
    v->b = b ? 1 : 0;
    return v;
}

bp_value *bp_new_real(bp_arena *a, double val) {
    bp_value *v = bp_new(a, BP_REAL);
    if (v == NULL) return NULL;
    v->d = val;
    return v;
}

bp_value *bp_new_data(bp_arena *a, const void *p, uint32_t len) {
    bp_value *v;
    uint8_t *dst;
    if (len > 0 && p == NULL) return NULL;
    v = bp_new(a, BP_DATA);
    if (v == NULL) return NULL;
    if (len == 0) {
        v->s = NULL;
        v->len = 0;
        return v;
    }
    dst = (uint8_t *)bp_arena_alloc(a, len, 1u);
    if (dst == NULL) return NULL;
    bp_memcpy(dst, p, len);
    v->s = dst;
    v->len = len;
    return v;
}

bp_value *bp_dict_get(const bp_value *dict, const char *key) {
    const bp_value *c;
    if (dict == NULL || dict->type != BP_DICT || key == NULL) return NULL;
    for (c = dict->child; c != NULL; c = c->next) {
        if (c->key != NULL && bp_strcmp(c->key, key) == 0)
            return (bp_value *)c;
    }
    return NULL;
}

int bp_dict_set(bp_arena *a, bp_value *dict, const char *key,
                bp_value *val) {
    bp_value *c;
    bp_value *prev = NULL;
    uint8_t *kcopy;
    uint32_t klen;
    if (dict == NULL || dict->type != BP_DICT || key == NULL ||
        val == NULL)
        return XW_ERR_BAD_TYPE;
    for (c = dict->child; c != NULL; c = c->next) {
        if (c->key != NULL && bp_strcmp(c->key, key) == 0) {
            /* Replace in place, keeping the existing key string. */
            val->key = c->key;
            val->next = c->next;
            if (prev != NULL) prev->next = val;
            else dict->child = val;
            return XW_OK;
        }
        prev = c;
    }
    klen = bp_strlen(key);
    if (klen >= 0xffffffffu) return XW_ERR_NOSPACE;
    kcopy = (uint8_t *)bp_arena_alloc(a, klen + 1u, 1u);
    if (kcopy == NULL) return XW_ERR_NOSPACE;
    bp_memcpy(kcopy, key, klen + 1u);
    val->key = (const char *)kcopy;
    val->next = NULL;
    if (prev != NULL) prev->next = val;
    else dict->child = val;
    return XW_OK;
}

int bp_array_append(bp_arena *a, bp_value *arr, bp_value *val) {
    bp_value *c;
    (void)a;
    if (arr == NULL || arr->type != BP_ARRAY || val == NULL)
        return XW_ERR_BAD_TYPE;
    val->next = NULL;
    val->key = NULL;
    if (arr->child == NULL) {
        arr->child = val;
        return XW_OK;
    }
    for (c = arr->child; c->next != NULL; c = c->next)
        ;
    c->next = val;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Reader.                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *buf;
    uint32_t table_off;    /* offset table start */
    uint32_t table_end;    /* trailer start (== len - 32) */
    uint32_t num_objects;
    uint32_t top_object;
    uint32_t offset_size;  /* offsetIntSize, 1/2/4/8 */
    uint32_t ref_size;     /* objectRefSize, 1/2/4/8 */
    bp_arena *arena;
    int err;               /* first error, XW_OK while healthy */
} bp_reader;

/* Is [off, off + n) inside the object region? */
static int bp_in_objects(const bp_reader *r, uint32_t off, uint32_t n) {
    if (off < 8u) return 0;
    if ((uint64_t)off + (uint64_t)n > (uint64_t)r->table_off) return 0;
    return 1;
}

/* Read n big-endian bytes at off into a uint64. Caller checks bounds. */
static uint64_t bp_read_be(const bp_reader *r, uint32_t off, uint32_t n) {
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < n; i++) v = (v << 8) | (uint64_t)r->buf[off + i];
    return v;
}

/* Read the offset-table entry for object idx. */
static int bp_read_offset(bp_reader *r, uint32_t idx, uint32_t *off_out) {
    uint64_t pos;
    uint64_t v;
    if (idx >= r->num_objects) {
        r->err = XW_ERR_TRUNCATED;
        return r->err;
    }
    pos = (uint64_t)r->table_off + (uint64_t)idx * (uint64_t)r->offset_size;
    v = bp_read_be(r, (uint32_t)pos, r->offset_size);
    if (v < 8u || v >= (uint64_t)r->table_off) {
        r->err = XW_ERR_BAD_TYPE;
        return r->err;
    }
    *off_out = (uint32_t)v;
    return XW_OK;
}

/* Read a size/count field: the marker's low nibble, or 0xF followed
 * by an int object. On success *size_out holds the value and
 * *payload_off_out the offset of the payload that follows it. */
static int bp_read_size(bp_reader *r, uint32_t pos, uint32_t info,
                        uint32_t *size_out, uint32_t *payload_off_out) {
    uint32_t marker;
    uint32_t isize;
    uint64_t v;
    if (info != 0xfu) {
        *size_out = info;
        *payload_off_out = pos;
        return XW_OK;
    }
    if (!bp_in_objects(r, pos, 1u)) {
        r->err = XW_ERR_TRUNCATED;
        return r->err;
    }
    marker = r->buf[pos];
    if ((marker >> 4) != 0x1u || (marker & 0xfu) > 3u) {
        r->err = XW_ERR_BAD_TYPE;
        return r->err;
    }
    isize = 1u << (marker & 0xfu);
    if (!bp_in_objects(r, pos + 1u, isize)) {
        r->err = XW_ERR_TRUNCATED;
        return r->err;
    }
    v = bp_read_be(r, pos + 1u, isize);
    if (v > 0xffffffffu) {
        r->err = XW_ERR_BAD_TYPE;
        return r->err;
    }
    *size_out = (uint32_t)v;
    *payload_off_out = pos + 1u + isize;
    return XW_OK;
}

/* Read an object reference (ref_size bytes, big-endian) at off. */
static int bp_read_ref(bp_reader *r, uint32_t off, uint32_t *idx_out) {
    uint64_t v;
    if (!bp_in_objects(r, off, r->ref_size)) {
        r->err = XW_ERR_TRUNCATED;
        return r->err;
    }
    v = bp_read_be(r, off, r->ref_size);
    if (v >= (uint64_t)r->num_objects) {
        r->err = XW_ERR_TRUNCATED;
        return r->err;
    }
    *idx_out = (uint32_t)v;
    return XW_OK;
}

/* Encode one Unicode code point as UTF-8. Returns bytes written. */
static uint32_t bp_utf8_encode(uint32_t cp, uint8_t *dst) {
    if (cp < 0x80u) {
        dst[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (uint8_t)(0xc0u | (cp >> 6));
        dst[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (uint8_t)(0xe0u | (cp >> 12));
        dst[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        dst[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 3;
    }
    dst[0] = (uint8_t)(0xf0u | (cp >> 18));
    dst[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
    dst[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
    dst[3] = (uint8_t)(0x80u | (cp & 0x3fu));
    return 4;
}

static bp_value *bp_parse_object(bp_reader *r, uint32_t idx,
                                 uint32_t depth);

static bp_value *bp_parse_string_utf16(bp_reader *r, uint32_t off,
                                       uint32_t units) {
    uint64_t need64 = (uint64_t)units * 3u + 1u;
    uint8_t *dst;
    uint32_t written = 0;
    uint32_t k;
    bp_value *v;
    if (need64 > 0xffffffffu) {
        r->err = XW_ERR_NOSPACE;
        return NULL;
    }
    dst = (uint8_t *)bp_arena_alloc(r->arena, (uint32_t)need64, 1u);
    if (dst == NULL) {
        r->err = XW_ERR_NOSPACE;
        return NULL;
    }
    for (k = 0; k < units; k++) {
        uint32_t u0 = (uint32_t)bp_read_be(r, off + k * 2u, 2u);
        uint32_t cp;
        if (u0 >= 0xd800u && u0 <= 0xdbffu) {
            uint32_t u1;
            if (k + 1u >= units) {
                r->err = XW_ERR_BAD_TYPE;
                return NULL;
            }
            u1 = (uint32_t)bp_read_be(r, off + (k + 1u) * 2u, 2u);
            if (u1 < 0xdc00u || u1 > 0xdfffu) {
                r->err = XW_ERR_BAD_TYPE;
                return NULL;
            }
            cp = 0x10000u + ((u0 - 0xd800u) << 10) + (u1 - 0xdc00u);
            k++;
        } else if (u0 >= 0xdc00u && u0 <= 0xdfffu) {
            r->err = XW_ERR_BAD_TYPE;
            return NULL;
        } else {
            cp = u0;
        }
        written += bp_utf8_encode(cp, dst + written);
    }
    dst[written] = 0;
    v = bp_new(r->arena, BP_STRING);
    if (v == NULL) {
        r->err = XW_ERR_NOSPACE;
        return NULL;
    }
    v->s = dst;
    v->len = written + 1u;
    return v;
}

static bp_value *bp_parse_object(bp_reader *r, uint32_t idx,
                                 uint32_t depth) {
    uint32_t off;
    uint32_t marker;
    uint32_t type;
    uint32_t info;
    bp_value *v;
    if (r->err != XW_OK) return NULL;
    if (depth > BP_MAX_DEPTH) {
        r->err = XW_ERR_OVERFLOW;
        return NULL;
    }
    if (bp_read_offset(r, idx, &off) != XW_OK) return NULL;
    if (!bp_in_objects(r, off, 1u)) {
        r->err = XW_ERR_TRUNCATED;
        return NULL;
    }
    marker = r->buf[off];
    type = marker >> 4;
    info = marker & 0xfu;
    switch (type) {
    case 0x0: /* simple */
        v = bp_new(r->arena, BP_NULL);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        if (info == 0x0) {
            v->type = BP_NULL;
        } else if (info == 0x8) {
            v->type = BP_BOOL;
            v->b = 0;
        } else if (info == 0x9) {
            v->type = BP_BOOL;
            v->b = 1;
        } else {
            r->err = XW_ERR_BAD_TYPE;
            return NULL;
        }
        return v;
    case 0x1: { /* int */
        uint32_t size;
        uint64_t raw;
        if (info > 3u) {
            r->err = XW_ERR_BAD_TYPE;
            return NULL;
        }
        size = 1u << info;
        if (!bp_in_objects(r, off + 1u, size)) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        raw = bp_read_be(r, off + 1u, size);
        v = bp_new(r->arena, BP_INT);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        if (size == 8u) v->i = (int64_t)raw;
        else if (size == 4u) v->i = (int32_t)(uint32_t)raw;
        else if (size == 2u) v->i = (int16_t)(uint16_t)raw;
        else v->i = (int8_t)(uint8_t)raw;
        return v;
    }
    case 0x2: { /* real */
        uint32_t size;
        if (info != 2u && info != 3u) {
            r->err = XW_ERR_BAD_TYPE;
            return NULL;
        }
        size = 1u << info;
        if (!bp_in_objects(r, off + 1u, size)) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        v = bp_new(r->arena, BP_REAL);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        if (size == 4u) {
            v->d = (double)bp_bits_float(
                (uint32_t)bp_read_be(r, off + 1u, 4u));
        } else {
            v->d = bp_bits_double(bp_read_be(r, off + 1u, 8u));
        }
        return v;
    }
    case 0x3: { /* date */
        double secs;
        double ns;
        if (info != 3u || !bp_in_objects(r, off + 1u, 8u)) {
            r->err = info != 3u ? XW_ERR_BAD_TYPE : XW_ERR_TRUNCATED;
            return NULL;
        }
        secs = bp_bits_double(bp_read_be(r, off + 1u, 8u));
        ns = (secs + BP_APPLE_EPOCH_OFFSET) * 1000000000.0;
        if (ns < -9223372036854775808.0 ||
            ns >= 9223372036854775808.0) {
            r->err = XW_ERR_BAD_TYPE;
            return NULL;
        }
        v = bp_new(r->arena, BP_DATE);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        v->i = (int64_t)ns;
        return v;
    }
    case 0x4: { /* data */
        uint32_t size;
        uint32_t payload;
        uint8_t *dst;
        if (bp_read_size(r, off + 1u, info, &size, &payload) != XW_OK)
            return NULL;
        if (!bp_in_objects(r, payload, size)) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        v = bp_new(r->arena, BP_DATA);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        if (size == 0) {
            v->s = NULL;
            v->len = 0;
            return v;
        }
        dst = (uint8_t *)bp_arena_alloc(r->arena, size, 1u);
        if (dst == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        bp_memcpy(dst, r->buf + payload, size);
        v->s = dst;
        v->len = size;
        return v;
    }
    case 0x5: { /* ASCII string */
        uint32_t size;
        uint32_t payload;
        uint8_t *dst;
        uint32_t i;
        if (bp_read_size(r, off + 1u, info, &size, &payload) != XW_OK)
            return NULL;
        if (!bp_in_objects(r, payload, size)) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        for (i = 0; i < size; i++) {
            if (r->buf[payload + i] >= 0x80u) {
                r->err = XW_ERR_BAD_TYPE;
                return NULL;
            }
        }
        dst = (uint8_t *)bp_arena_alloc(r->arena, size + 1u, 1u);
        if (dst == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        bp_memcpy(dst, r->buf + payload, size);
        dst[size] = 0;
        v = bp_new(r->arena, BP_STRING);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        v->s = dst;
        v->len = size + 1u;
        return v;
    }
    case 0x6: { /* UTF-16BE string */
        uint32_t units;
        uint32_t payload;
        if (bp_read_size(r, off + 1u, info, &units, &payload) != XW_OK)
            return NULL;
        if ((uint64_t)units * 2u > (uint64_t)r->table_off ||
            !bp_in_objects(r, payload, units * 2u)) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        return bp_parse_string_utf16(r, payload, units);
    }
    case 0x8: /* UID: not in our type set */
        r->err = XW_ERR_BAD_TYPE;
        return NULL;
    case 0xa: { /* array */
        uint32_t count;
        uint32_t payload;
        uint32_t k;
        bp_value *last = NULL;
        if (bp_read_size(r, off + 1u, info, &count, &payload) != XW_OK)
            return NULL;
        if ((uint64_t)count * (uint64_t)r->ref_size >
            (uint64_t)r->table_off) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        v = bp_new(r->arena, BP_ARRAY);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        for (k = 0; k < count; k++) {
            uint32_t child_idx;
            bp_value *child;
            if (bp_read_ref(r, payload + k * r->ref_size,
                            &child_idx) != XW_OK)
                return NULL;
            child = bp_parse_object(r, child_idx, depth + 1u);
            if (child == NULL) return NULL;
            child->next = NULL;
            child->key = NULL;
            if (last != NULL) last->next = child;
            else v->child = child;
            last = child;
        }
        return v;
    }
    case 0xd: { /* dict */
        uint32_t count;
        uint32_t payload;
        uint32_t k;
        bp_value *last = NULL;
        if (bp_read_size(r, off + 1u, info, &count, &payload) != XW_OK)
            return NULL;
        if ((uint64_t)count * 2u * (uint64_t)r->ref_size >
            (uint64_t)r->table_off) {
            r->err = XW_ERR_TRUNCATED;
            return NULL;
        }
        v = bp_new(r->arena, BP_DICT);
        if (v == NULL) {
            r->err = XW_ERR_NOSPACE;
            return NULL;
        }
        for (k = 0; k < count; k++) {
            uint32_t key_idx;
            uint32_t val_idx;
            bp_value *key_obj;
            bp_value *child;
            if (bp_read_ref(r, payload + k * r->ref_size,
                            &key_idx) != XW_OK)
                return NULL;
            if (bp_read_ref(r, payload + (count + k) * r->ref_size,
                            &val_idx) != XW_OK)
                return NULL;
            key_obj = bp_parse_object(r, key_idx, depth + 1u);
            if (key_obj == NULL) return NULL;
            if (key_obj->type != BP_STRING || key_obj->s == NULL) {
                r->err = XW_ERR_BAD_TYPE;
                return NULL;
            }
            child = bp_parse_object(r, val_idx, depth + 1u);
            if (child == NULL) return NULL;
            child->key = (const char *)key_obj->s;
            child->next = NULL;
            if (last != NULL) last->next = child;
            else v->child = child;
            last = child;
        }
        return v;
    }
    default:
        r->err = XW_ERR_BAD_TYPE;
        return NULL;
    }
}

int bp_parse(const uint8_t *buf, uint32_t len, bp_arena *a,
             bp_value **root_out) {
    bp_reader r;
    const uint8_t *trailer;
    uint64_t num_objects;
    uint64_t top_object;
    uint64_t table_off;
    bp_value *root;
    if (buf == NULL || a == NULL || root_out == NULL)
        return XW_ERR_BAD_TYPE;
    if (len < 40u) return XW_ERR_TRUNCATED;
    if (bp_memcmp(buf, "bplist00", 8u) != 0) return XW_ERR_BAD_MAGIC;
    trailer = buf + len - 32u;
    bp_memset(&r, 0, (uint32_t)sizeof(r));
    r.buf = buf;
    r.table_end = len - 32u;
    r.arena = a;
    r.err = XW_OK;
    r.offset_size = trailer[6];
    r.ref_size = trailer[7];
    if (r.offset_size != 1u && r.offset_size != 2u &&
        r.offset_size != 4u && r.offset_size != 8u)
        return XW_ERR_BAD_TYPE;
    if (r.ref_size != 1u && r.ref_size != 2u && r.ref_size != 4u &&
        r.ref_size != 8u)
        return XW_ERR_BAD_TYPE;
    num_objects = bp_read_be(&r, len - 24u, 8u);
    top_object = bp_read_be(&r, len - 16u, 8u);
    table_off = bp_read_be(&r, len - 8u, 8u);
    if (num_objects == 0 || num_objects > 0xffffffffu)
        return XW_ERR_BAD_TYPE;
    if (top_object >= num_objects) return XW_ERR_BAD_TYPE;
    if (table_off < 8u || table_off > (uint64_t)r.table_end)
        return XW_ERR_BAD_TYPE;
    if (table_off + num_objects * (uint64_t)r.offset_size >
        (uint64_t)r.table_end)
        return XW_ERR_TRUNCATED;
    r.num_objects = (uint32_t)num_objects;
    r.top_object = (uint32_t)top_object;
    r.table_off = (uint32_t)table_off;
    root = bp_parse_object(&r, r.top_object, 0u);
    if (root == NULL) return r.err;
    *root_out = root;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Writer.                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t len;
    int err;               /* first error, XW_OK while healthy */
    uint32_t ref_size;     /* objectRefSize, 1/2/4 */
    uint32_t count;        /* total objects */
    uint32_t offsets[BP_WRITE_MAX_OBJECTS];
} bp_writer;

static void bp_emit_u8(bp_writer *w, uint8_t v) {
    if (w->err != XW_OK) return;
    if (w->len >= w->cap) {
        w->err = XW_ERR_NOSPACE;
        return;
    }
    w->out[w->len++] = v;
}

static void bp_emit_bytes(bp_writer *w, const uint8_t *p, uint32_t n) {
    uint32_t i;
    if (w->err != XW_OK) return;
    if (n > w->cap - w->len) {
        w->err = XW_ERR_NOSPACE;
        return;
    }
    for (i = 0; i < n; i++) w->out[w->len++] = p[i];
}

static void bp_emit_u16be(bp_writer *w, uint32_t v) {
    bp_emit_u8(w, (uint8_t)((v >> 8) & 0xffu));
    bp_emit_u8(w, (uint8_t)(v & 0xffu));
}

static void bp_emit_u32be(bp_writer *w, uint32_t v) {
    bp_emit_u8(w, (uint8_t)((v >> 24) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 16) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 8) & 0xffu));
    bp_emit_u8(w, (uint8_t)(v & 0xffu));
}

static void bp_emit_u64be(bp_writer *w, uint64_t v) {
    bp_emit_u8(w, (uint8_t)((v >> 56) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 48) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 40) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 32) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 24) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 16) & 0xffu));
    bp_emit_u8(w, (uint8_t)((v >> 8) & 0xffu));
    bp_emit_u8(w, (uint8_t)(v & 0xffu));
}

/* Emit a ref_size-byte big-endian object reference. */
static void bp_emit_ref(bp_writer *w, uint32_t idx) {
    if (w->ref_size == 1u) {
        bp_emit_u8(w, (uint8_t)idx);
    } else if (w->ref_size == 2u) {
        bp_emit_u16be(w, idx);
    } else {
        bp_emit_u32be(w, idx);
    }
}

/* Emit a size/count field: the low nibble when it fits, else 0xF
 * followed by an int object. base is the marker with a zero low
 * nibble (0x40, 0x50, 0x60, 0xa0 or 0xd0). */
static void bp_emit_size(bp_writer *w, uint8_t base, uint32_t size) {
    if (size < 15u) {
        bp_emit_u8(w, (uint8_t)(base | (uint8_t)size));
        return;
    }
    bp_emit_u8(w, (uint8_t)(base | 0x0fu));
    if (size <= 0xffu) {
        bp_emit_u8(w, 0x10);
        bp_emit_u8(w, (uint8_t)size);
    } else if (size <= 0xffffu) {
        bp_emit_u8(w, 0x11);
        bp_emit_u16be(w, size);
    } else {
        bp_emit_u8(w, 0x12);
        bp_emit_u32be(w, size);
    }
}

/* Emit an int object for a signed value, smallest fitting size. */
static void bp_emit_int_obj(bp_writer *w, int64_t v) {
    uint64_t raw = (uint64_t)v;
    if (v >= -128 && v <= 127) {
        bp_emit_u8(w, 0x10);
        bp_emit_u8(w, (uint8_t)raw);
    } else if (v >= -32768 && v <= 32767) {
        bp_emit_u8(w, 0x11);
        bp_emit_u16be(w, (uint32_t)raw);
    } else if (v >= -2147483648LL && v <= 2147483647LL) {
        bp_emit_u8(w, 0x12);
        bp_emit_u32be(w, (uint32_t)raw);
    } else {
        bp_emit_u8(w, 0x13);
        bp_emit_u64be(w, raw);
    }
}

/* Emit an int object for an unsigned value. Sizes are chosen by
 * SIGNED fit, not unsigned fit: the reader sign-extends by the
 * encoded size, so a value must fit in the signed range of its
 * size to read back positive. Values up to INT64_MAX therefore
 * round-trip as BP_INT with the same numeric value; only values
 * above INT64_MAX come back negative (same bits, see bplist.h). */
static void bp_emit_uint_obj(bp_writer *w, uint64_t v) {
    if (v <= 127u) {
        bp_emit_u8(w, 0x10);
        bp_emit_u8(w, (uint8_t)v);
    } else if (v <= 32767u) {
        bp_emit_u8(w, 0x11);
        bp_emit_u16be(w, (uint32_t)v);
    } else if (v <= 2147483647u) {
        bp_emit_u8(w, 0x12);
        bp_emit_u32be(w, (uint32_t)v);
    } else {
        bp_emit_u8(w, 0x13);
        bp_emit_u64be(w, v);
    }
}

/* Decode one UTF-8 code point at *pos (advancing it). Returns 0 or
 * a negative XW_ERR_*. */
static int bp_utf8_decode(const uint8_t *s, uint32_t len, uint32_t *pos,
                          uint32_t *cp_out) {
    uint32_t p = *pos;
    uint8_t c0;
    uint32_t cp;
    uint32_t n;
    uint32_t i;
    if (p >= len) return XW_ERR_BAD_TYPE;
    c0 = s[p];
    if (c0 < 0x80u) {
        *cp_out = c0;
        *pos = p + 1u;
        return XW_OK;
    }
    if ((c0 & 0xe0u) == 0xc0u) {
        cp = c0 & 0x1fu;
        n = 2u;
        if (cp < 2u) return XW_ERR_BAD_TYPE; /* overlong */
    } else if ((c0 & 0xf0u) == 0xe0u) {
        cp = c0 & 0x0fu;
        n = 3u;
    } else if ((c0 & 0xf8u) == 0xf0u) {
        cp = c0 & 0x07u;
        n = 4u;
    } else {
        return XW_ERR_BAD_TYPE;
    }
    if (p + n > len) return XW_ERR_BAD_TYPE;
    for (i = 1; i < n; i++) {
        uint8_t cc = s[p + i];
        if ((cc & 0xc0u) != 0x80u) return XW_ERR_BAD_TYPE;
        cp = (cp << 6) | (uint32_t)(cc & 0x3fu);
    }
    if ((n == 3u && cp < 0x800u) || (n == 4u && cp < 0x10000u))
        return XW_ERR_BAD_TYPE; /* overlong */
    if (cp >= 0xd800u && cp <= 0xdfffu) return XW_ERR_BAD_TYPE;
    if (cp > 0x10ffffu) return XW_ERR_BAD_TYPE;
    *cp_out = cp;
    *pos = p + n;
    return XW_OK;
}

/* Emit a string object (ASCII or UTF-16BE) and record its offset.
 * s holds slen content bytes (no NUL). */
static void bp_emit_string_obj(bp_writer *w, const uint8_t *s,
                               uint32_t slen, uint32_t idx) {
    uint32_t i;
    int ascii = 1;
    if (w->err != XW_OK) return;
    if (idx >= BP_WRITE_MAX_OBJECTS) {
        w->err = XW_ERR_OVERFLOW;
        return;
    }
    w->offsets[idx] = w->len;
    for (i = 0; i < slen; i++) {
        if (s[i] >= 0x80u) {
            ascii = 0;
            break;
        }
    }
    if (ascii) {
        bp_emit_size(w, 0x50, slen);
        bp_emit_bytes(w, s, slen);
        return;
    }
    /* UTF-16BE: first pass counts code units, second pass emits. */
    {
        uint32_t units = 0;
        uint32_t pos = 0;
        uint32_t cp;
        while (pos < slen) {
            if (bp_utf8_decode(s, slen, &pos, &cp) != XW_OK) {
                w->err = XW_ERR_BAD_TYPE;
                return;
            }
            units += (cp >= 0x10000u) ? 2u : 1u;
        }
        bp_emit_size(w, 0x60, units);
        pos = 0;
        while (pos < slen) {
            uint32_t c;
            if (bp_utf8_decode(s, slen, &pos, &c) != XW_OK) {
                w->err = XW_ERR_BAD_TYPE;
                return;
            }
            if (c >= 0x10000u) {
                uint32_t v = c - 0x10000u;
                bp_emit_u16be(w, 0xd800u + (v >> 10));
                bp_emit_u16be(w, 0xdc00u + (v & 0x3ffu));
            } else {
                bp_emit_u16be(w, c);
            }
        }
    }
}

/* Count nodes in a subtree (dict entries add one node for the key
 * string object). Depth capped at BP_MAX_DEPTH; returns
 * 0xffffffffu when the tree is too deep. */
static uint32_t bp_subtree_count(const bp_value *v, uint32_t depth) {
    uint32_t n = 1;
    const bp_value *c;
    if (depth > BP_MAX_DEPTH) return 0xffffffffu;
    if (v->type == BP_ARRAY || v->type == BP_DICT) {
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub;
            if (v->type == BP_DICT) {
                if (n == 0xffffffffu) return 0xffffffffu;
                n += 1u; /* key string object */
            }
            sub = bp_subtree_count(c, depth + 1u);
            if (sub == 0xffffffffu) return 0xffffffffu;
            n += sub;
        }
    }
    return n;
}

static void bp_emit_value(bp_writer *w, const bp_value *v, uint32_t idx,
                          uint32_t depth) {
    if (w->err != XW_OK) return;
    if (depth > BP_MAX_DEPTH || idx >= BP_WRITE_MAX_OBJECTS) {
        w->err = XW_ERR_OVERFLOW;
        return;
    }
    w->offsets[idx] = w->len;
    switch (v->type) {
    case BP_NULL:
        bp_emit_u8(w, 0x00);
        return;
    case BP_BOOL:
        bp_emit_u8(w, v->b ? 0x09 : 0x08);
        return;
    case BP_INT:
        bp_emit_int_obj(w, v->i);
        return;
    case BP_UINT:
        bp_emit_uint_obj(w, v->u);
        return;
    case BP_REAL:
        bp_emit_u8(w, 0x23);
        bp_emit_u64be(w, bp_double_bits(v->d));
        return;
    case BP_STRING: {
        uint32_t slen = v->len > 0 ? v->len - 1u : 0;
        const uint8_t *s = v->s != NULL ? v->s : (const uint8_t *)"";
        /* Re-record the offset: bp_emit_string_obj sets it again to
         * the same value. */
        bp_emit_string_obj(w, s, slen, idx);
        return;
    }
    case BP_DATA:
        bp_emit_size(w, 0x40, v->len);
        if (v->len > 0) bp_emit_bytes(w, v->s, v->len);
        return;
    case BP_DATE: {
        double secs = (double)v->i / 1000000000.0 - BP_APPLE_EPOCH_OFFSET;
        bp_emit_u8(w, 0x33);
        bp_emit_u64be(w, bp_double_bits(secs));
        return;
    }
    case BP_ARRAY: {
        uint32_t count = 0;
        const bp_value *c;
        uint32_t next;
        for (c = v->child; c != NULL; c = c->next) count++;
        bp_emit_size(w, 0xa0, count);
        next = idx + 1u;
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub = bp_subtree_count(c, depth + 1u);
            bp_emit_ref(w, next);
            next += sub;
        }
        next = idx + 1u;
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub = bp_subtree_count(c, depth + 1u);
            bp_emit_value(w, c, next, depth + 1u);
            next += sub;
        }
        return;
    }
    case BP_DICT: {
        uint32_t count = 0;
        const bp_value *c;
        uint32_t next;
        for (c = v->child; c != NULL; c = c->next) count++;
        bp_emit_size(w, 0xd0, count);
        /* Key refs first, then value refs. */
        next = idx + 1u;
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub;
            if (c->key == NULL) {
                w->err = XW_ERR_BAD_TYPE;
                return;
            }
            bp_emit_ref(w, next);
            sub = bp_subtree_count(c, depth + 1u);
            next += 1u + sub;
        }
        next = idx + 1u;
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub = bp_subtree_count(c, depth + 1u);
            bp_emit_ref(w, next + 1u);
            next += 1u + sub;
        }
        /* Children in entry order: key string, then value subtree. */
        next = idx + 1u;
        for (c = v->child; c != NULL; c = c->next) {
            uint32_t sub = bp_subtree_count(c, depth + 1u);
            uint32_t key_len = bp_strlen(c->key);
            bp_emit_string_obj(w, (const uint8_t *)c->key, key_len,
                               next);
            bp_emit_value(w, c, next + 1u, depth + 1u);
            next += 1u + sub;
        }
        return;
    }
    default:
        w->err = XW_ERR_BAD_TYPE;
        return;
    }
}

int bp_write(const bp_value *root, uint8_t *out, uint32_t cap,
             uint32_t *out_len) {
    bp_writer w;
    uint32_t count;
    uint32_t i;
    uint32_t table_off;
    if (root == NULL || out == NULL || out_len == NULL)
        return XW_ERR_BAD_TYPE;
    count = bp_subtree_count(root, 0u);
    if (count == 0xffffffffu || count > BP_WRITE_MAX_OBJECTS)
        return XW_ERR_OVERFLOW;
    bp_memset(&w, 0, (uint32_t)sizeof(w));
    w.out = out;
    w.cap = cap;
    w.err = XW_OK;
    w.count = count;
    w.ref_size = count <= 0xffu ? 1u : count <= 0xffffu ? 2u : 4u;
    bp_emit_bytes(&w, (const uint8_t *)"bplist00", 8u);
    bp_emit_value(&w, root, 0u, 0u);
    if (w.err != XW_OK) return w.err;
    table_off = w.len;
    for (i = 0; i < count; i++) bp_emit_u32be(&w, w.offsets[i]);
    if (w.err != XW_OK) return w.err;
    for (i = 0; i < 6u; i++) bp_emit_u8(&w, 0);
    bp_emit_u8(&w, 4); /* offsetIntSize */
    bp_emit_u8(&w, (uint8_t)w.ref_size);
    bp_emit_u64be(&w, (uint64_t)count);
    bp_emit_u64be(&w, 0); /* top object */
    bp_emit_u64be(&w, (uint64_t)table_off);
    if (w.err != XW_OK) return w.err;
    *out_len = w.len;
    return XW_OK;
}
