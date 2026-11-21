/*
 * bsxpc.c: BSXPC coder layer for ioswine.
 *
 * See bsxpc.h for the API, the sources, and the [guess] list. All
 * helpers here are bs_ prefixed. No libc, no unaligned reads.
 *
 * Output: trace logging writes to fd 2. On the target that is the
 * raw Linux write syscall (linux.h, aarch64 only). On the host
 * (plain cc, x86_64) the same file compiles with the libc write()
 * call instead; that branch exists only so the host test can build
 * this file, and it is never compiled on the target.
 * lsk_getenv is declared in ipc.h and defined in init.c on the
 * target; the host test provides its own definition reading the
 * real process environment.
 */
#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"
#include "bsxpc.h"
#include "ipc.h"

#if defined(__aarch64__)
#include "linux.h"
#endif

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static size_t bs_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void *bs_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* Little-endian u32 helpers (byte-wise, no unaligned access). */
static uint32_t bs_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bs_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* fd 2 output. bs_fd_write is the raw syscall on the target and the
 * libc write() on the host build (see the file header). */
#if defined(__aarch64__)
static long bs_fd_write(int fd, const void *buf, size_t n) {
    return sys_write(fd, buf, n);
}
#else
extern long write(int fd, const void *buf, unsigned long count);
static long bs_fd_write(int fd, const void *buf, size_t n) {
    return write(fd, buf, (unsigned long)n);
}
#endif

static void bs_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        long n = bs_fd_write(fd, p, len);
        if (n <= 0) return;
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static void bs_write_str(const char *s) {
    if (s) bs_write_all(2, s, bs_strlen(s));
}

static void bs_write_u64(uint64_t v) {
    char tmp[20];
    int i = 0, j;
    if (v == 0) {
        bs_write_all(2, "0", 1);
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (j = 0; j < i / 2; j++) {
        char c = tmp[j];
        tmp[j] = tmp[i - 1 - j];
        tmp[i - 1 - j] = c;
    }
    bs_write_all(2, tmp, (size_t)i);
}

static void bs_write_i64(int64_t v) {
    if (v < 0) {
        uint64_t mag;
        bs_write_all(2, "-", 1);
        /* -(v + 1) + 1 avoids overflow for INT64_MIN. */
        mag = (uint64_t)(-(v + 1)) + 1u;
        bs_write_u64(mag);
    } else {
        bs_write_u64((uint64_t)v);
    }
}

static void bs_write_hex2(uint8_t b) {
    static const char hexd[] = "0123456789abcdef";
    char tmp[2];
    tmp[0] = hexd[b >> 4];
    tmp[1] = hexd[b & 0xfu];
    bs_write_all(2, tmp, 2);
}

static void bs_write_hex32(uint32_t w) {
    bs_write_hex2((uint8_t)(w >> 24));
    bs_write_hex2((uint8_t)(w >> 16));
    bs_write_hex2((uint8_t)(w >> 8));
    bs_write_hex2((uint8_t)w);
}

static void bs_write_hex_bytes(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (i != 0) bs_write_all(2, " ", 1);
        bs_write_hex2(p[i]);
    }
}

/* Double formatting, %f style with 6 fractional digits (printf %f
 * default precision). No libc, so this is hand rolled. Writes at
 * most cap - 1 bytes plus a NUL, returns the length written
 * excluding the NUL. NaN prints as "nan", infinities as "inf" /
 * "-inf". Values with |v| >= 1e18 print in scientific form with 6
 * fractional digits (d.dddddd e+XX); the integer-part cast below
 * is only used below 1e18, where it is exact. Rounding is half up
 * (printf rounds half to even); the difference is one unit in the
 * last place and does not matter for a trace log.
 */
static size_t bs_format_double(char *buf, size_t cap, double v) {
    uint64_t bits;
    int neg, exp2;
    double a;
    size_t n = 0;

    if (cap == 0) return 0;
    buf[0] = '\0';
    bs_memcpy(&bits, &v, 8);
    neg = (int)(bits >> 63);
    exp2 = (int)((bits >> 52) & 0x7ffu);
    if (exp2 == 0x7ff) {
        const char *s;
        if (bits & 0xfffffffffffffu) {
            s = "nan";
        } else if (neg) {
            s = "-inf";
        } else {
            s = "inf";
        }
        while (s[n] && n + 1 < cap) {
            buf[n] = s[n];
            n++;
        }
        buf[n] = '\0';
        return n;
    }

    a = neg ? -v : v;
    if (a == 0.0) {
        const char *s = neg ? "-0.000000" : "0.000000";
        while (s[n] && n + 1 < cap) {
            buf[n] = s[n];
            n++;
        }
        buf[n] = '\0';
        return n;
    }

    if (a >= 1e18) {
        /* Scientific form: mantissa in [1, 10), 6 frac digits. */
        double m = a;
        int e = 0;
        uint64_t ip, frac;
        double frac_d;
        int i;
        char digits[8];
        while (m >= 10.0) {
            m /= 10.0;
            e++;
        }
        while (m < 1.0) {
            m *= 10.0;
            e--;
        }
        ip = (uint64_t)m;
        frac_d = (m - (double)ip) * 1000000.0;
        frac = (uint64_t)(frac_d + 0.5);
        if (frac >= 1000000u) {
            frac -= 1000000u;
            ip += 1;
            if (ip >= 10u) {
                ip = 1;
                e += 1;
            }
        }
        if (neg && n + 1 < cap) buf[n++] = '-';
        if (n + 1 < cap) buf[n++] = (char)('0' + (int)ip);
        if (n + 1 < cap) buf[n++] = '.';
        for (i = 5; i >= 0; i--) {
            digits[i] = (char)('0' + (int)(frac % 10u));
            frac /= 10u;
        }
        for (i = 0; i < 6; i++) {
            if (n + 1 < cap) buf[n++] = digits[i];
        }
        if (n + 1 < cap) buf[n++] = 'e';
        if (n + 1 < cap) buf[n++] = (e < 0) ? '-' : '+';
        {
            int ae = (e < 0) ? -e : e;
            if (ae < 10 && n + 1 < cap) buf[n++] = '0';
            {
                char etmp[8];
                int ei = 0, ej;
                if (ae == 0) {
                    etmp[ei++] = '0';
                } else {
                    while (ae > 0) {
                        etmp[ei++] = (char)('0' + (ae % 10));
                        ae /= 10;
                    }
                }
                for (ej = ei - 1; ej >= 0; ej--) {
                    if (n + 1 < cap) buf[n++] = etmp[ej];
                }
            }
        }
        buf[n] = '\0';
        return n;
    }

    /* Plain %f form. */
    {
        uint64_t ip = (uint64_t)a;
        double frac_d = (a - (double)ip) * 1000000.0;
        uint64_t frac = (uint64_t)(frac_d + 0.5);
        int i;
        char digits[24];
        int di = 0;
        if (frac >= 1000000u) {
            frac -= 1000000u;
            ip += 1;
        }
        if (neg && n + 1 < cap) buf[n++] = '-';
        if (ip == 0) {
            if (n + 1 < cap) buf[n++] = '0';
        } else {
            while (ip > 0) {
                digits[di++] = (char)('0' + (int)(ip % 10u));
                ip /= 10u;
            }
            for (i = di - 1; i >= 0; i--) {
                if (n + 1 < cap) buf[n++] = digits[i];
            }
        }
        if (n + 1 < cap) buf[n++] = '.';
        for (i = 5; i >= 0; i--) {
            digits[i] = (char)('0' + (int)(frac % 10u));
            frac /= 10u;
        }
        for (i = 0; i < 6; i++) {
            if (n + 1 < cap) buf[n++] = digits[i];
        }
        buf[n] = '\0';
        return n;
    }
}

/* ------------------------------------------------------------------ */
/* BSSettings.                                                         */
/* ------------------------------------------------------------------ */

int bs_settings_begin(xw_builder *b) {
    if (!b) return XW_ERR_BAD_TYPE;
    return xw_dict_begin(b);
}

int bs_settings_end(xw_builder *b) {
    if (!b) return XW_ERR_BAD_TYPE;
    return xw_dict_end(b);
}

int bs_settings_set_flag(xw_builder *b, const char *key, int value) {
    if (!b || !key) return XW_ERR_BAD_TYPE;
    return xw_dict_add_bool(b, key, value);
}

int bs_settings_set_int(xw_builder *b, const char *key, int64_t value) {
    if (!b || !key) return XW_ERR_BAD_TYPE;
    return xw_dict_add_int64(b, key, value);
}

int bs_settings_set_uint(xw_builder *b, const char *key, uint64_t value) {
    if (!b || !key) return XW_ERR_BAD_TYPE;
    return xw_dict_add_uint64(b, key, value);
}

int bs_settings_set_double(xw_builder *b, const char *key, double value) {
    if (!b || !key) return XW_ERR_BAD_TYPE;
    return xw_dict_add_double(b, key, value);
}

int bs_settings_set_string(xw_builder *b, const char *key,
                           const char *value) {
    if (!b || !key || !value) return XW_ERR_BAD_TYPE;
    return xw_dict_add_string(b, key, value);
}

int bs_settings_set_data(xw_builder *b, const char *key,
                         const void *ptr, uint32_t len) {
    if (!b || !key || (!ptr && len != 0)) return XW_ERR_BAD_TYPE;
    return xw_dict_add_data(b, key, ptr, len);
}

int bs_settings_set_size(xw_builder *b, const char *key,
                         double width, double height) {
    int r;
    if (!b || !key) return XW_ERR_BAD_TYPE;
    r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_double(b, "width", width);
    if (r == XW_OK) r = xw_dict_add_double(b, "height", height);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

int bs_settings_set_rect(xw_builder *b, const char *key,
                         double x, double y, double width, double height) {
    int r;
    if (!b || !key) return XW_ERR_BAD_TYPE;
    r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_double(b, "x", x);
    if (r == XW_OK) r = xw_dict_add_double(b, "y", y);
    if (r == XW_OK) r = xw_dict_add_double(b, "width", width);
    if (r == XW_OK) r = xw_dict_add_double(b, "height", height);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

int bs_settings_set_point(xw_builder *b, const char *key,
                          double x, double y) {
    int r;
    if (!b || !key) return XW_ERR_BAD_TYPE;
    r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_double(b, "x", x);
    if (r == XW_OK) r = xw_dict_add_double(b, "y", y);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

int bs_settings_set_insets(xw_builder *b, const char *key,
                           double top, double left,
                           double bottom, double right) {
    int r;
    if (!b || !key) return XW_ERR_BAD_TYPE;
    r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_double(b, "top", top);
    if (r == XW_OK) r = xw_dict_add_double(b, "left", left);
    if (r == XW_OK) r = xw_dict_add_double(b, "bottom", bottom);
    if (r == XW_OK) r = xw_dict_add_double(b, "right", right);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

int bs_settings_set_frame(xw_builder *b, const char *key,
                          double x, double y, double width, double height) {
    return bs_settings_set_rect(b, key, x, y, width, height);
}

/* One double field of a nested geometry dictionary. */
static int bs_settings_get_double_v(const xw_value *dict, const char *key,
                                    double *out) {
    xw_value v;
    int rc;
    rc = xw_dict_find_v(dict, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_double(&v, out);
}

int bs_settings_get_flag(xw_reader *r, const char *key, int *out) {
    xw_value v;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_bool(&v, out);
}

int bs_settings_get_int(xw_reader *r, const char *key, int64_t *out) {
    xw_value v;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_int64(&v, out);
}

int bs_settings_get_uint(xw_reader *r, const char *key, uint64_t *out) {
    xw_value v;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_uint64(&v, out);
}

int bs_settings_get_double(xw_reader *r, const char *key, double *out) {
    xw_value v;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    return xw_as_double(&v, out);
}

int bs_settings_get_string(xw_reader *r, const char *key,
                           const char **out, uint32_t *len_out) {
    xw_value v;
    uint32_t dummy_len = 0;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    /* xw_as_string requires a non-NULL length pointer. */
    return xw_as_string(&v, out, len_out ? len_out : &dummy_len);
}

int bs_settings_get_size(xw_reader *r, const char *key,
                         double *width_out, double *height_out) {
    xw_value v;
    int rc;
    if (!r || !key || !width_out || !height_out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    if (v.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    rc = bs_settings_get_double_v(&v, "width", width_out);
    if (rc != XW_OK) return rc;
    return bs_settings_get_double_v(&v, "height", height_out);
}

int bs_settings_get_rect(xw_reader *r, const char *key,
                         double *x_out, double *y_out,
                         double *width_out, double *height_out) {
    xw_value v;
    int rc;
    if (!r || !key || !x_out || !y_out || !width_out || !height_out)
        return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    if (v.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    rc = bs_settings_get_double_v(&v, "x", x_out);
    if (rc != XW_OK) return rc;
    rc = bs_settings_get_double_v(&v, "y", y_out);
    if (rc != XW_OK) return rc;
    rc = bs_settings_get_double_v(&v, "width", width_out);
    if (rc != XW_OK) return rc;
    return bs_settings_get_double_v(&v, "height", height_out);
}

int bs_settings_get_point(xw_reader *r, const char *key,
                          double *x_out, double *y_out) {
    xw_value v;
    int rc;
    if (!r || !key || !x_out || !y_out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    if (v.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    rc = bs_settings_get_double_v(&v, "x", x_out);
    if (rc != XW_OK) return rc;
    return bs_settings_get_double_v(&v, "y", y_out);
}

int bs_settings_get_insets(xw_reader *r, const char *key,
                           double *top_out, double *left_out,
                           double *bottom_out, double *right_out) {
    xw_value v;
    int rc;
    if (!r || !key || !top_out || !left_out || !bottom_out || !right_out)
        return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    if (v.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    rc = bs_settings_get_double_v(&v, "top", top_out);
    if (rc != XW_OK) return rc;
    rc = bs_settings_get_double_v(&v, "left", left_out);
    if (rc != XW_OK) return rc;
    rc = bs_settings_get_double_v(&v, "bottom", bottom_out);
    if (rc != XW_OK) return rc;
    return bs_settings_get_double_v(&v, "right", right_out);
}

/* ------------------------------------------------------------------ */
/* BSAction.                                                           */
/* ------------------------------------------------------------------ */

int bsxpc_action_build(xw_builder *b, const bsxpc_action_keys *keys,
                       const char *kind, const char *target,
                       uint64_t timeout_ms) {
    int r;
    if (!b || !keys || !kind || !target) return XW_ERR_BAD_TYPE;
    if (!keys->key_kind || !keys->key_target || !keys->key_timeout)
        return XW_ERR_BAD_TYPE;
    r = xw_dict_begin(b);
    if (r == XW_OK) r = xw_dict_add_string(b, keys->key_kind, kind);
    if (r == XW_OK) r = xw_dict_add_string(b, keys->key_target, target);
    if (r == XW_OK) r = xw_dict_add_uint64(b, keys->key_timeout, timeout_ms);
    /* Dictionary stays open: the caller adds the optional info
     * nested dictionary and closes with xw_dict_end. */
    return r;
}

int bsxpc_action_parse(xw_reader *r, const bsxpc_action_keys *keys,
                       char *kind_buf, size_t kind_cap,
                       char *target_buf, size_t target_cap,
                       uint64_t *timeout_out) {
    xw_value v;
    const char *s;
    uint32_t slen = 0;
    int rc;
    if (!r || !keys || !kind_buf || !target_buf || !timeout_out)
        return XW_ERR_BAD_TYPE;
    if (!keys->key_kind || !keys->key_target || !keys->key_timeout)
        return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, keys->key_kind, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_string(&v, &s, &slen);
    if (rc != XW_OK) return rc;
    if (slen == 0 || (size_t)slen > kind_cap) return XW_ERR_NOSPACE;
    bs_memcpy(kind_buf, s, slen);
    rc = xw_dict_find(r, keys->key_target, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_string(&v, &s, &slen);
    if (rc != XW_OK) return rc;
    if (slen == 0 || (size_t)slen > target_cap) return XW_ERR_NOSPACE;
    bs_memcpy(target_buf, s, slen);
    rc = xw_dict_find(r, keys->key_timeout, &v);
    if (rc != XW_OK) return rc;
    return xw_as_uint64(&v, timeout_out);
}

/* ------------------------------------------------------------------ */
/* Audit tokens.                                                       */
/* ------------------------------------------------------------------ */

int bsxpc_audit_token_forge(bsxpc_audit_token *tok, uint32_t pid) {
    if (!tok) return XW_ERR_BAD_TYPE;
    tok->auid = 0;
    tok->euid = 0;
    tok->egid = 0;
    tok->ruid = 0;
    tok->rgid = 0;
    tok->pid = pid;
    tok->asid = 0;
    tok->pidversion = 1;
    return XW_OK;
}

int bsxpc_add_audit_token(xw_builder *b, const char *key,
                          const bsxpc_audit_token *tok) {
    uint8_t bytes[32];
    if (!b || !key || !tok) return XW_ERR_BAD_TYPE;
    bs_put32(bytes + 0, tok->auid);
    bs_put32(bytes + 4, tok->euid);
    bs_put32(bytes + 8, tok->egid);
    bs_put32(bytes + 12, tok->ruid);
    bs_put32(bytes + 16, tok->rgid);
    bs_put32(bytes + 20, tok->pid);
    bs_put32(bytes + 24, tok->asid);
    bs_put32(bytes + 28, tok->pidversion);
    return xw_dict_add_data(b, key, bytes, 32);
}

int bsxpc_get_audit_token(xw_reader *r, const char *key,
                          bsxpc_audit_token *out) {
    xw_value v;
    const uint8_t *p = NULL;
    uint32_t len = 0, desc = XW_NO_DESC;
    int rc;
    if (!r || !key || !out) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, key, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_data(&v, &p, &len, &desc);
    if (rc != XW_OK) return rc;
    if (len != 32 || !p) return XW_ERR_BAD_TYPE;
    out->auid = bs_rd32(p + 0);
    out->euid = bs_rd32(p + 4);
    out->egid = bs_rd32(p + 8);
    out->ruid = bs_rd32(p + 12);
    out->rgid = bs_rd32(p + 16);
    out->pid = bs_rd32(p + 20);
    out->asid = bs_rd32(p + 24);
    out->pidversion = bs_rd32(p + 28);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* BSProcessHandle / RBSProcessHandle.                                 */
/* ------------------------------------------------------------------ */

int bsxpc_handle_build(xw_builder *b, const bsxpc_handle_keys *keys,
                       const bsxpc_process_handle *h) {
    int r;
    if (!b || !keys || !h) return XW_ERR_BAD_TYPE;
    if (!keys->key_pid || !keys->key_bundle_id ||
        !keys->key_audit_token || !keys->key_is_running ||
        !keys->key_is_foreground || !keys->key_is_valid ||
        !keys->key_is_application)
        return XW_ERR_BAD_TYPE;
    r = xw_dict_begin(b);
    if (r == XW_OK) r = xw_dict_add_uint64(b, keys->key_pid, h->pid);
    if (r == XW_OK && h->bundle_id)
        r = xw_dict_add_string(b, keys->key_bundle_id, h->bundle_id);
    if (r == XW_OK)
        r = bsxpc_add_audit_token(b, keys->key_audit_token, &h->audit);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, keys->key_is_running, h->is_running);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, keys->key_is_foreground, h->is_foreground);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, keys->key_is_valid, h->is_valid);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, keys->key_is_application, h->is_application);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

int bsxpc_handle_parse(xw_reader *r, const bsxpc_handle_keys *keys,
                       bsxpc_process_handle *out,
                       char *bundle_buf, size_t bundle_cap) {
    xw_value v;
    uint64_t u = 0;
    int bo = 0;
    const char *s = NULL;
    uint32_t slen = 0;
    int rc;
    if (!r || !keys || !out || !bundle_buf) return XW_ERR_BAD_TYPE;
    if (!keys->key_pid || !keys->key_bundle_id ||
        !keys->key_audit_token || !keys->key_is_running ||
        !keys->key_is_foreground || !keys->key_is_valid ||
        !keys->key_is_application)
        return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, keys->key_pid, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_uint64(&v, &u);
    if (rc != XW_OK) return rc;
    out->pid = (uint32_t)u;
    rc = xw_dict_find(r, keys->key_bundle_id, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_string(&v, &s, &slen);
    if (rc != XW_OK) return rc;
    if (slen == 0 || (size_t)slen > bundle_cap) return XW_ERR_NOSPACE;
    bs_memcpy(bundle_buf, s, slen);
    out->bundle_id = bundle_buf;
    rc = bsxpc_get_audit_token(r, keys->key_audit_token, &out->audit);
    if (rc != XW_OK) return rc;
    rc = xw_dict_find(r, keys->key_is_running, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_bool(&v, &bo);
    if (rc != XW_OK) return rc;
    out->is_running = bo;
    rc = xw_dict_find(r, keys->key_is_foreground, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_bool(&v, &bo);
    if (rc != XW_OK) return rc;
    out->is_foreground = bo;
    rc = xw_dict_find(r, keys->key_is_valid, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_bool(&v, &bo);
    if (rc != XW_OK) return rc;
    out->is_valid = bo;
    rc = xw_dict_find(r, keys->key_is_application, &v);
    if (rc != XW_OK) return rc;
    rc = xw_as_bool(&v, &bo);
    if (rc != XW_OK) return rc;
    out->is_application = bo;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* BSXPCServiceConnection message envelope (real keys).     */
/* ------------------------------------------------------------------ */

/* Zero value for a parsed context that is absent. Static storage
 * duration gives all-zero fields; no function pointers involved. */
static const xw_value k_bs_zero_value;

/* Parse an incoming BSXPC message dict: extract the bsxpc kind
 * (string), bsxpc_SEL (string), bsxpc_CID (uint64), bsxpc_CIDr
 * (bool), and the bsxpc_context dict into out->ctx. Lenient by
 * design (shim header): a key that is present but has the wrong
 * type returns XW_ERR_BAD_TYPE; a key that is absent clears its
 * has_* flag and leaves the field at its zero value; a root that
 * is not a dictionary returns XW_ERR_BAD_TYPE. A well-formed
 * message may legitimately omit any key, so parse succeeds with
 * all flags clear on an empty dict. out is zeroed first, so a
 * caller that did not memset it still sees zero values for absent
 * keys. Returns XW_OK or a negative XW_ERR_*.
 */
int bsxpc_message_parse(xw_reader *r, const bsxpc_envelope_keys *keys,
                        bsxpc_message *out) {
    xw_value root;
    xw_value v;
    const char *s = NULL;
    uint32_t slen = 0;
    uint64_t u = 0;
    int bo = 0;
    int rc;
    if (!r || !keys || !out) return XW_ERR_BAD_TYPE;
    if (!keys->key_bsxpc || !keys->key_context || !keys->key_cid ||
        !keys->key_cidr || !keys->key_sel)
        return XW_ERR_BAD_TYPE;
    out->kind = NULL;
    out->sel = NULL;
    out->cid = 0;
    out->cidr = 0;
    out->has_kind = 0;
    out->has_sel = 0;
    out->has_cid = 0;
    out->has_cidr = 0;
    out->has_ctx = 0;
    out->ctx = k_bs_zero_value;
    rc = xw_reader_root(r, &root);
    if (rc != XW_OK) return rc;
    if (root.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
    rc = xw_dict_find(r, keys->key_bsxpc, &v);
    if (rc == XW_OK) {
        rc = xw_as_string(&v, &s, &slen);
        if (rc != XW_OK) return rc;
        out->kind = s;
        out->has_kind = 1;
    } else if (rc != XW_ERR_NOT_FOUND) {
        return rc;
    }
    rc = xw_dict_find(r, keys->key_sel, &v);
    if (rc == XW_OK) {
        rc = xw_as_string(&v, &s, &slen);
        if (rc != XW_OK) return rc;
        out->sel = s;
        out->has_sel = 1;
    } else if (rc != XW_ERR_NOT_FOUND) {
        return rc;
    }
    rc = xw_dict_find(r, keys->key_cid, &v);
    if (rc == XW_OK) {
        rc = xw_as_uint64(&v, &u);
        if (rc != XW_OK) return rc;
        out->cid = u;
        out->has_cid = 1;
    } else if (rc != XW_ERR_NOT_FOUND) {
        return rc;
    }
    rc = xw_dict_find(r, keys->key_cidr, &v);
    if (rc == XW_OK) {
        rc = xw_as_bool(&v, &bo);
        if (rc != XW_OK) return rc;
        out->cidr = bo;
        out->has_cidr = 1;
    } else if (rc != XW_ERR_NOT_FOUND) {
        return rc;
    }
    rc = xw_dict_find(r, keys->key_context, &v);
    if (rc == XW_OK) {
        if (v.type != XW_TYPE_DICTIONARY) return XW_ERR_BAD_TYPE;
        out->ctx = v;
        out->has_ctx = 1;
    } else if (rc != XW_ERR_NOT_FOUND) {
        return rc;
    }
    return XW_OK;
}

/* Build a completion reply dict for a given CID: open a dict, write
 * "bsxpc_CID" (the same CID) and "bsxpc_CIDr" (true), and leave the
 * dict OPEN for the caller to add result fields at the TOP LEVEL,
 * then close it with ONE xw_dict_end. The reply msgh_id 0x20000000
 * is set by the caller in the Mach header, not by bsxpc. The
 * top-level result shape is [guess] (bsxpc-wire.md open question 2:
 * the exact reply dict is unobserved); bsxpc_context is documented
 * as the handshake dict, not a general result container. Returns
 * XW_OK or a negative XW_ERR_*.
 */
int bsxpc_reply_build(xw_builder *b, const bsxpc_envelope_keys *keys,
                      uint64_t cid) {
    int r;
    if (!b || !keys) return XW_ERR_BAD_TYPE;
    if (!keys->key_bsxpc || !keys->key_context || !keys->key_cid ||
        !keys->key_cidr || !keys->key_sel)
        return XW_ERR_BAD_TYPE;
    r = xw_dict_begin(b);
    if (r == XW_OK) r = xw_dict_add_uint64(b, keys->key_cid, cid);
    if (r == XW_OK) r = xw_dict_add_bool(b, keys->key_cidr, 1);
    /* Dict stays open: the caller adds result fields and closes
     * with one xw_dict_end. */
    return r;
}

/* ------------------------------------------------------------------ */
/* Verbatim trace logger.                                              */
/* ------------------------------------------------------------------ */

/* -1 means no override / not read yet. */
static int g_bs_trace_override = -1;
static int g_bs_trace_cached = -1;

int bsxpc_trace_enabled(void) {
    const char *v;
    if (g_bs_trace_override != -1) return g_bs_trace_override;
    if (g_bs_trace_cached != -1) return g_bs_trace_cached;
    v = lsk_getenv("IOSWINE_SVC_TRACE");
    g_bs_trace_cached = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    return g_bs_trace_cached;
}

void bsxpc_set_trace(int on) {
    if (on < 0) {
        g_bs_trace_override = -1;
        g_bs_trace_cached = -1;
    } else {
        g_bs_trace_override = on ? 1 : 0;
    }
}

/* One line start: prefix (verbatim) plus 2 spaces per depth. */
static void bs_log_line_start(const char *prefix, uint32_t depth) {
    static const char spaces[] =
        "                                                                ";
    if (prefix && prefix[0]) {
        bs_write_all(2, prefix, bs_strlen(prefix));
        bs_write_all(2, " ", 1);
    }
    if (depth * 2u <= 64u) bs_write_all(2, spaces, depth * 2u);
}

static const char *bs_type_name(uint32_t type) {
    switch (type) {
    case XW_TYPE_NULL: return "null";
    case XW_TYPE_BOOL: return "bool";
    case XW_TYPE_INT64: return "int64";
    case XW_TYPE_UINT64: return "uint64";
    case XW_TYPE_DOUBLE: return "double";
    case XW_TYPE_DATE: return "date";
    case XW_TYPE_DATA: return "data";
    case XW_TYPE_STRING: return "string";
    case XW_TYPE_UUID: return "uuid";
    case XW_TYPE_FD: return "fd";
    case XW_TYPE_SHMEM: return "shmem";
    case XW_TYPE_MACH_SEND: return "mach_send";
    case XW_TYPE_MACH_RECV: return "mach_recv";
    case XW_TYPE_MACH_SEND_ONCE: return "mach_send_once";
    case XW_TYPE_CONNECTION: return "connection";
    case XW_TYPE_ENDPOINT: return "endpoint";
    case XW_TYPE_ARRAY: return "array";
    case XW_TYPE_DICTIONARY: return "dictionary";
    default: return "unknown";
    }
}

/* Quoted string with escaping: " and \ are backslash escaped,
 * control bytes print as \xHH. len is the byte count without the
 * terminator. */
static void bs_write_escaped(const uint8_t *s, size_t len) {
    size_t i;
    bs_write_all(2, "\"", 1);
    for (i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (c == '"') {
            bs_write_all(2, "\\\"", 2);
        } else if (c == '\\') {
            bs_write_all(2, "\\\\", 2);
        } else if (c < 0x20 || c == 0x7f) {
            bs_write_all(2, "\\x", 2);
            bs_write_hex2(c);
        } else {
            bs_write_all(2, &c, 1);
        }
    }
    bs_write_all(2, "\"", 1);
}

static void bs_write_uuid(const uint8_t *p) {
    static const int groups[5] = { 4, 2, 2, 2, 6 };
    int g, i;
    size_t off = 0;
    for (g = 0; g < 5; g++) {
        if (g != 0) bs_write_all(2, "-", 1);
        for (i = 0; i < groups[g]; i++) bs_write_hex2(p[off++]);
    }
}

/* "desc#N disp=0xDD" for a port value, or "desc#?" when the value
 * carries no usable descriptor (cannot happen for a validated
 * message, but the logger never crashes on it). */
static void bs_write_port_ref(const xw_value *v) {
    bs_write_str("desc#");
    if (v->desc == XW_NO_DESC || !v->descs || v->desc >= v->ndescs) {
        bs_write_str("?");
        return;
    }
    bs_write_u64(v->desc);
    bs_write_str(" disp=0x");
    bs_write_hex2((uint8_t)v->descs[v->desc].disposition);
}

/* The value half of a "key: type = value" line, for a scalar value
 * (containers are handled by bs_log_entry). */
static void bs_log_value_inline(const xw_value *v) {
    char num[48];
    switch (v->type) {
    case XW_TYPE_NULL:
        bs_write_str("null");
        break;
    case XW_TYPE_BOOL:
        bs_write_str(v->boolean ? "true" : "false");
        break;
    case XW_TYPE_INT64:
        bs_write_i64(v->i64);
        break;
    case XW_TYPE_UINT64:
        bs_write_u64(v->u64);
        break;
    case XW_TYPE_DOUBLE:
        (void)bs_format_double(num, sizeof(num), v->dbl);
        bs_write_str(num);
        break;
    case XW_TYPE_DATE:
        bs_write_i64(v->i64);
        bs_write_str("ns");
        break;
    case XW_TYPE_STRING:
        bs_write_escaped(v->ptr, v->len ? v->len - 1 : 0);
        break;
    case XW_TYPE_DATA:
        bs_write_str("data[");
        bs_write_u64(v->len);
        bs_write_str("]");
        if (v->ptr) {
            size_t n = v->len < 16 ? v->len : 16;
            bs_write_str(" = ");
            bs_write_hex_bytes(v->ptr, n);
            if (v->len > 16) bs_write_str(" ...");
        } else {
            bs_write_str(" OOL ");
            bs_write_port_ref(v);
        }
        break;
    case XW_TYPE_UUID:
        bs_write_uuid(v->ptr);
        break;
    case XW_TYPE_FD:
    case XW_TYPE_SHMEM:
        if (v->type == XW_TYPE_SHMEM) {
            bs_write_str("shmem[");
            bs_write_u64(v->len);
            bs_write_str("] ");
        }
        bs_write_port_ref(v);
        break;
    case XW_TYPE_MACH_SEND:
    case XW_TYPE_MACH_RECV:
    case XW_TYPE_MACH_SEND_ONCE:
    case XW_TYPE_CONNECTION:
    case XW_TYPE_ENDPOINT:
        bs_write_port_ref(v);
        break;
    default:
        bs_write_str("unknown type 0x");
        bs_write_hex32(v->type);
        break;
    }
}

#define BSXPC_LOG_MAX_DEPTH 16

static int bs_log_dict_value(const xw_value *dict, const char *prefix,
                             uint32_t depth);
static int bs_log_array_value(const xw_value *arr, const char *prefix,
                              uint32_t depth);

/* A key on a log line: dict keys are quoted ("key"), array items
 * use the bare "[12]" style key (see bs_format_index below) and are
 * not string keys, so they print unquoted. */
static void bs_write_key(const char *key, int quote_key) {
    if (quote_key) {
        bs_write_escaped((const uint8_t *)key, bs_strlen(key));
    } else {
        bs_write_all(2, key, bs_strlen(key));
    }
}

/* One entry: "key": type = value (quote_key non-zero), or
 * [n]: type = value for an array item (quote_key zero), or a
 * container with braces. */
static int bs_log_entry(const char *key, const xw_value *v,
                        const char *prefix, uint32_t depth, int quote_key) {
    int rc;
    if (v->type == XW_TYPE_DICTIONARY) {
        bs_log_line_start(prefix, depth);
        bs_write_key(key, quote_key);
        bs_write_str(": dictionary = {\n");
        rc = bs_log_dict_value(v, prefix, depth + 1);
        if (rc != XW_OK) return rc;
        bs_log_line_start(prefix, depth);
        bs_write_str("}\n");
        return XW_OK;
    }
    if (v->type == XW_TYPE_ARRAY) {
        uint32_t n = 0;
        rc = xw_array_len_v(v, &n);
        if (rc != XW_OK) return rc;
        bs_log_line_start(prefix, depth);
        bs_write_key(key, quote_key);
        bs_write_str(": array[");
        bs_write_u64(n);
        bs_write_str("] = {\n");
        rc = bs_log_array_value(v, prefix, depth + 1);
        if (rc != XW_OK) return rc;
        bs_log_line_start(prefix, depth);
        bs_write_str("}\n");
        return XW_OK;
    }
    bs_log_line_start(prefix, depth);
    bs_write_key(key, quote_key);
    bs_write_str(": ");
    bs_write_str(bs_type_name(v->type));
    bs_write_str(" = ");
    bs_log_value_inline(v);
    bs_write_str("\n");
    return XW_OK;
}

static int bs_log_dict_value(const xw_value *dict, const char *prefix,
                             uint32_t depth) {
    xw_dict_iter it;
    const char *key = NULL;
    xw_value v;
    int rc;
    if (depth > BSXPC_LOG_MAX_DEPTH) {
        bs_log_line_start(prefix, depth);
        bs_write_str("<too deep>\n");
        return XW_OK;
    }
    rc = xw_dict_iter_init_v(dict, &it);
    if (rc != XW_OK) return rc;
    for (;;) {
        rc = xw_dict_next(&it, &key, &v);
        if (rc == 0) break;
        if (rc < 0) return rc;
        rc = bs_log_entry(key, &v, prefix, depth, 1);
        if (rc != XW_OK) return rc;
    }
    return XW_OK;
}

/* "[12]" style key for array item i. */
static void bs_format_index(char *buf, size_t cap, uint32_t idx) {
    char tmp[12];
    int i = 0, j;
    size_t n = 0;
    if (cap == 0) return;
    buf[0] = '\0';
    if (n + 1 < cap) buf[n++] = '[';
    if (idx == 0) {
        if (n + 1 < cap) buf[n++] = '0';
    } else {
        while (idx > 0) {
            tmp[i++] = (char)('0' + (int)(idx % 10u));
            idx /= 10u;
        }
        for (j = i - 1; j >= 0; j--) {
            if (n + 1 < cap) buf[n++] = tmp[j];
        }
    }
    if (n + 1 < cap) buf[n++] = ']';
    buf[n] = '\0';
}

static int bs_log_array_value(const xw_value *arr, const char *prefix,
                              uint32_t depth) {
    uint32_t n = 0, i;
    int rc;
    if (depth > BSXPC_LOG_MAX_DEPTH) {
        bs_log_line_start(prefix, depth);
        bs_write_str("<too deep>\n");
        return XW_OK;
    }
    rc = xw_array_len_v(arr, &n);
    if (rc != XW_OK) return rc;
    for (i = 0; i < n; i++) {
        xw_value item;
        char keybuf[16];
        rc = xw_array_at_v(arr, i, &item);
        if (rc != XW_OK) return rc;
        bs_format_index(keybuf, sizeof(keybuf), i);
        rc = bs_log_entry(keybuf, &item, prefix, depth, 0);
        if (rc != XW_OK) return rc;
    }
    return XW_OK;
}

int bsxpc_log_dict(const xw_reader *r, const char *prefix) {
    xw_reader tmp;
    xw_value root;
    int rc;
    if (!bsxpc_trace_enabled()) return 0;
    if (!r) {
        bs_log_line_start(prefix, 0);
        bs_write_str("<malformed>\n");
        return XW_ERR_BAD_TYPE;
    }
    tmp = *r;
    if (!tmp.buf || tmp.len < 8) {
        bs_log_line_start(prefix, 0);
        bs_write_str("<malformed>\n");
        return XW_ERR_BAD_TYPE;
    }
    rc = xw_reader_root(&tmp, &root);
    if (rc != XW_OK) {
        bs_log_line_start(prefix, 0);
        bs_write_str("<malformed>\n");
        return rc;
    }
    if (root.type == XW_TYPE_DICTIONARY) {
        rc = bs_log_dict_value(&root, prefix, 0);
    } else if (root.type == XW_TYPE_ARRAY) {
        rc = bs_log_array_value(&root, prefix, 0);
    } else {
        rc = bs_log_entry("<root>", &root, prefix, 0, 1);
    }
    if (rc != XW_OK) {
        bs_log_line_start(prefix, 0);
        bs_write_str("<malformed>\n");
        return rc;
    }
    return XW_OK;
}
