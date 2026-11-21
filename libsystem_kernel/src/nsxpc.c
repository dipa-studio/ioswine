/*
 * nsxpc.c: NSXPC wire protocol encoder/decoder for ioswine.
 *
 * Encapsulates the bplist17 serialization format used by Foundation's
 * NSXPCConnection in iOS 26.6.2 (build 23G90).
 *
 * Freestanding C. No libc headers beyond <stdint.h>, <stddef.h>, <stdarg.h>.
 * No em dashes are used in this file.
 */
#include "nsxpc.h"
#include "mg_names.h"

/* ------------------------------------------------------------------ */
/* Local freestanding string / memory helpers.                        */
/* ------------------------------------------------------------------ */

static size_t ns_strlen(const char *s) {
    size_t n = 0;
    if (s == NULL) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static void ns_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void ns_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int ns_memeq(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return 0;
    }
    return 1;
}

static int ns_strcmp(const char *a, const char *b) {
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static uint64_t ns_read_u64_le(const uint8_t *p) {
    return ((uint64_t)p[0]) |
           (((uint64_t)p[1]) << 8) |
           (((uint64_t)p[2]) << 16) |
           (((uint64_t)p[3]) << 24) |
           (((uint64_t)p[4]) << 32) |
           (((uint64_t)p[5]) << 40) |
           (((uint64_t)p[6]) << 48) |
           (((uint64_t)p[7]) << 56);
}

static void ns_write_u64_le(uint8_t *p, uint64_t val) {
    p[0] = (uint8_t)(val & 0xff);
    p[1] = (uint8_t)((val >> 8) & 0xff);
    p[2] = (uint8_t)((val >> 16) & 0xff);
    p[3] = (uint8_t)((val >> 24) & 0xff);
    p[4] = (uint8_t)((val >> 32) & 0xff);
    p[5] = (uint8_t)((val >> 40) & 0xff);
    p[6] = (uint8_t)((val >> 48) & 0xff);
    p[7] = (uint8_t)((val >> 56) & 0xff);
}

static void ns_write_u32_le(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val & 0xff);
    p[1] = (uint8_t)((val >> 8) & 0xff);
    p[2] = (uint8_t)((val >> 16) & 0xff);
    p[3] = (uint8_t)((val >> 24) & 0xff);
}

static void ns_write_u16_le(uint8_t *p, uint16_t val) {
    p[0] = (uint8_t)(val & 0xff);
    p[1] = (uint8_t)((val >> 8) & 0xff);
}

/* ------------------------------------------------------------------ */
/* nsxpc_is_message                                                   */
/* ------------------------------------------------------------------ */

int nsxpc_is_message(xw_reader *msg) {
    xw_value v;
    if (msg == NULL) return 0;
    if (xw_dict_find(msg, "root", &v) == XW_OK) {
        if (v.type == XW_TYPE_DATA) {
            const uint8_t *p = NULL;
            uint32_t len = 0;
            uint32_t desc = 0;
            if (xw_as_data(&v, &p, &len, &desc) == XW_OK && p != NULL && len >= 8) {
                if (ns_memeq(p, NSXPC_MAGIC, NSXPC_MAGIC_LEN)) return 1;
            }
        }
    }
    if (xw_dict_find(msg, "proxynum", &v) == XW_OK ||
        xw_dict_find(msg, "sequence", &v) == XW_OK) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* nsxpc_parse_request                                                */
/* ------------------------------------------------------------------ */

int nsxpc_parse_request(xw_reader *msg, nsxpc_request_t *req) {
    xw_value root_val;
    const uint8_t *p = NULL;
    uint32_t len = 0;
    uint32_t off = 0;
    uint8_t tag = 0;
    uint32_t sel_len = 0;

    if (msg == NULL || req == NULL) return -1;
    ns_memset(req, 0, sizeof(*req));

    {
        xw_value v;
        uint64_t u = 0;
        if (xw_dict_find(msg, "proxynum", &v) == XW_OK &&
            xw_as_uint64(&v, &u) == XW_OK) req->proxynum = u;
        if (xw_dict_find(msg, "sequence", &v) == XW_OK &&
            xw_as_uint64(&v, &u) == XW_OK) req->sequence = u;
        if (xw_dict_find(msg, "f", &v) == XW_OK &&
            xw_as_uint64(&v, &u) == XW_OK) req->flags = u;
        if (xw_dict_find(msg, "replysig", &v) == XW_OK) {
            const char *s = NULL;
            uint32_t slen = 0;
            if (xw_as_string(&v, &s, &slen) == XW_OK) req->replysig = s;
        }
    }

    if (xw_dict_find(msg, "root", &root_val) != XW_OK ||
        root_val.type != XW_TYPE_DATA) {
        return -1;
    }
    uint32_t desc = 0;
    if (xw_as_data(&root_val, &p, &len, &desc) != XW_OK ||
        p == NULL || len < 18) {
        return -1;
    }

    /* 1. Header: "bplist17" */
    if (!ns_memeq(p, NSXPC_MAGIC, NSXPC_MAGIC_LEN)) return -1;

    /* 2. Container header: 0xd0 tag + 8-byte uint64 end offset */
    if (p[8] != NSXPC_TAG_DICT) return -1;
    off = 17; /* 8 (magic) + 1 (tag) + 8 (end_offset) */

    /* 3. Selector */
    if (off >= len) return -1;
    tag = p[off++];
    if ((tag & 0xf0) != NSXPC_TAG_SELECTOR) return -1;
    sel_len = tag & 0x0f;
    if (sel_len == 0x0f) {
        if (off >= len) return -1;
        uint8_t len_tag = p[off++];
        if ((len_tag & 0xf0) != NSXPC_TAG_INT) return -1;
        uint8_t int_bytes = len_tag & 0x0f;
        if (int_bytes == 1) {
            if (off >= len) return -1;
            sel_len = p[off++];
        } else if (int_bytes == 2) {
            if (off + 2 > len) return -1;
            sel_len = p[off] | ((uint32_t)p[off + 1] << 8);
            off += 2;
        } else {
            return -1;
        }
    }
    if (off + sel_len > len) return -1;
    {
        size_t c = (sel_len < sizeof(req->selector) - 1) ?
                    sel_len : (sizeof(req->selector) - 1);
        ns_memcpy(req->selector, p + off, c);
        req->selector[c] = '\0';
    }
    off += sel_len;

    /* 4. Arguments array (optional) */
    if (off < len) {
        uint8_t arr_tag = p[off++];
        if ((arr_tag & 0xf0) == NSXPC_TAG_ARRAY) {
            uint32_t count = arr_tag & 0x0f;
            if (count == 0x0f) {
                if (off >= len) return 0;
                uint8_t len_tag = p[off++];
                if ((len_tag & 0xf0) != NSXPC_TAG_INT) return 0;
                uint8_t int_bytes = len_tag & 0x0f;
                if (int_bytes == 1) {
                    if (off >= len) return 0;
                    count = p[off++];
                } else if (int_bytes == 2) {
                    if (off + 2 > len) return 0;
                    count = p[off] | ((uint32_t)p[off + 1] << 8);
                    off += 2;
                }
            }
            if (off + 8 > len) return 0;
            off += 8; /* skip array end_offset */

            /* If array has elements, read element 0 as question string */
            if (count > 0 && off < len) {
                uint8_t q_tag = p[off++];
                if ((q_tag & 0xf0) == NSXPC_TAG_STRING_ASCII) {
                    uint32_t q_len = q_tag & 0x0f;
                    if (q_len == 0x0f) {
                        if (off < len) {
                            uint8_t len_tag = p[off++];
                            if ((len_tag & 0xf0) == NSXPC_TAG_INT) {
                                uint8_t int_bytes = len_tag & 0x0f;
                                if (int_bytes == 1 && off < len) {
                                    q_len = p[off++];
                                } else if (int_bytes == 2 && off + 2 <= len) {
                                    q_len = p[off] | ((uint32_t)p[off + 1] << 8);
                                    off += 2;
                                }
                            }
                        }
                    }
                    if (off + q_len <= len) {
                        size_t c = (q_len < sizeof(req->question) - 1) ?
                                    q_len : (sizeof(req->question) - 1);
                        ns_memcpy(req->question, p + off, c);
                        req->question[c] = '\0';
                        req->has_question = 1;
                    }
                } else if ((q_tag & 0xf0) == NSXPC_TAG_STRING_UTF16) {
                    uint32_t q_chars = q_tag & 0x0f;
                    if (q_chars == 0x0f) {
                        if (off < len) {
                            uint8_t len_tag = p[off++];
                            if ((len_tag & 0xf0) == NSXPC_TAG_INT) {
                                uint8_t int_bytes = len_tag & 0x0f;
                                if (int_bytes == 1 && off < len) {
                                    q_chars = p[off++];
                                } else if (int_bytes == 2 && off + 2 <= len) {
                                    q_chars = p[off] | ((uint32_t)p[off + 1] << 8);
                                    off += 2;
                                }
                            }
                        }
                    }
                    if (off + q_chars * 2 <= len) {
                        size_t c = (q_chars < sizeof(req->question) - 1) ?
                                    q_chars : (sizeof(req->question) - 1);
                        for (size_t ci = 0; ci < c; ci++) {
                            uint16_t ch = ((uint16_t)p[off + ci * 2] << 8) |
                                          p[off + ci * 2 + 1];
                            if (ch == 0 && p[off + ci * 2 + 1] != 0)
                                ch = p[off + ci * 2 + 1];
                            req->question[ci] = (char)(ch & 0x7f);
                        }
                        req->question[c] = '\0';
                        req->has_question = 1;
                    }
                }
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* nsxpc_encode_reply helpers                                         */
/* ------------------------------------------------------------------ */

static int ns_append_str(uint8_t *buf, uint32_t cap, uint32_t *poff,
                         const char *str) {
    size_t slen = ns_strlen(str);
    uint32_t off = *poff;
    if (slen < 15) {
        if (off + 1 + slen > cap) return XW_ERR_NOSPACE;
        buf[off++] = (uint8_t)(NSXPC_TAG_STRING_ASCII | slen);
    } else {
        if (off + 3 + slen > cap) return XW_ERR_NOSPACE;
        buf[off++] = NSXPC_TAG_STRING_ASCII | 0x0f;
        buf[off++] = NSXPC_TAG_INT | 0x01;
        buf[off++] = (uint8_t)slen;
    }
    ns_memcpy(buf + off, str, slen);
    off += (uint32_t)slen;
    *poff = off;
    return 0;
}

static int ns_append_int(uint8_t *buf, uint32_t cap, uint32_t *poff,
                         int64_t val) {
    uint32_t off = *poff;
    if (val >= 0 && val <= 0xff) {
        if (off + 2 > cap) return XW_ERR_NOSPACE;
        buf[off++] = NSXPC_TAG_INT | 0x01;
        buf[off++] = (uint8_t)val;
    } else if (val >= -0x8000 && val <= 0x7fff) {
        if (off + 3 > cap) return XW_ERR_NOSPACE;
        buf[off++] = NSXPC_TAG_INT | 0x02;
        ns_write_u16_le(buf + off, (uint16_t)val);
        off += 2;
    } else if (val >= -0x80000000LL && val <= 0x7fffffffLL) {
        if (off + 5 > cap) return XW_ERR_NOSPACE;
        buf[off++] = NSXPC_TAG_INT | 0x04;
        ns_write_u32_le(buf + off, (uint32_t)val);
        off += 4;
    } else {
        if (off + 9 > cap) return XW_ERR_NOSPACE;
        buf[off++] = NSXPC_TAG_INT | 0x08;
        ns_write_u64_le(buf + off, (uint64_t)val);
        off += 8;
    }
    *poff = off;
    return 0;
}

static int ns_append_artwork_dict(uint8_t *buf, uint32_t cap, uint32_t *poff) {
    uint32_t off = *poff;
    if (off + 9 > cap) return XW_ERR_NOSPACE;
    buf[off++] = NSXPC_TAG_DICT | 0x09; /* 9 entries */
    uint32_t dict_end_pos = off;
    off += 8; /* placeholder */
    *poff = off;

    int r = 0;
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_DEVICE_IDIOM);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_DEVICE_IDIOM);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_DEVICE_SUBTYPE);
    if (r == 0) r = ns_append_int(buf, cap, poff, MG_V_ART_DEVICE_SUBTYPE);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_DEVICE_SCALE_FACTOR);
    if (r == 0) r = ns_append_int(buf, cap, poff, MG_V_ART_DEVICE_SCALE_FACTOR);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_DISPLAY_GAMUT);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_DISPLAY_GAMUT);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_DYNAMIC_DISPLAY_MODE);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_DYNAMIC_DISPLAY_MODE);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_PRODUCT_DESCRIPTION);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_PRODUCT_DESCRIPTION);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_GRAPHICS_FEATURE_SET_CLASS);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_GRAPHICS_FEATURE_SET_CLASS);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_GRAPHICS_FEATURE_SET_FALLBACKS);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_GRAPHICS_FEATURE_SET_FALLBACKS);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_ART_COMPATIBLE_DEVICE_FALLBACK);
    if (r == 0) r = ns_append_str(buf, cap, poff, MG_V_ART_COMPATIBLE_DEVICE_FALLBACK);
    if (r != 0) return r;

    ns_write_u64_le(buf + dict_end_pos, (uint64_t)(*poff - 1));
    return 0;
}

/* ------------------------------------------------------------------ */
/* nsxpc_reply_signature                                              */
/* ------------------------------------------------------------------ */

const char *nsxpc_reply_signature(const char *s) {
    if (s == NULL) return "v@?@";
    /* If already a method signature (e.g. starts with 'v' and contains '@?'), return as-is */
    if (s[0] == 'v' && s[1] == '@' && s[2] == '?') return s;
    /* Map known selectors to their reply signatures */
    if (ns_strcmp(s, "rebuildCache:") == 0 ||
        ns_strcmp(s, "setCacheSentinel:") == 0) {
        return "v@?B";
    }
    if (ns_strcmp(s, "getServerAnswerForQuestion:reply:") == 0 ||
        ns_strcmp(s, "getSpringboardRegionOverride:reply:") == 0) {
        return "v@?@";
    }
    /* If string starts with 'v' (generic void-returning signature), return as-is */
    if (s[0] == 'v') return s;
    /* Default fallback for object reply */
    return "v@?@";
}

/* ------------------------------------------------------------------ */
/* nsxpc_encode_reply                                                 */
/* ------------------------------------------------------------------ */

int nsxpc_encode_reply(const char *replysig_or_sel, const mg_answer_t *ans,
                       uint8_t *out_buf, uint32_t max_buf_len,
                       uint32_t *out_len) {
    if (replysig_or_sel == NULL || out_buf == NULL || out_len == NULL)
        return XW_ERR_BAD_TYPE;
    if (max_buf_len < 64) return XW_ERR_NOSPACE;

    uint32_t off = 0;

    /* 1. Magic Header: "bplist17" */
    ns_memcpy(out_buf + off, NSXPC_MAGIC, NSXPC_MAGIC_LEN);
    off += NSXPC_MAGIC_LEN;

    /* 2. Top-level Container: 0xd0 tag + 8-byte uint64 end offset */
    out_buf[off++] = NSXPC_TAG_DICT;
    uint32_t container_end_pos = off;
    off += 8; /* placeholder */

    /* 3. Invocation reply marker: 0xe0 */
    out_buf[off++] = NSXPC_TAG_REPLY_MARKER;

    /* 4. Reply method signature (tag 0x70) */
    const char *replysig = nsxpc_reply_signature(replysig_or_sel);
    size_t sig_len = ns_strlen(replysig);
    if (sig_len < 15) {
        if (off + 1 + sig_len > max_buf_len) return XW_ERR_NOSPACE;
        out_buf[off++] = (uint8_t)(NSXPC_TAG_SELECTOR | sig_len);
    } else {
        if (off + 3 + sig_len > max_buf_len) return XW_ERR_NOSPACE;
        out_buf[off++] = NSXPC_TAG_SELECTOR | 0x0f;
        out_buf[off++] = NSXPC_TAG_INT | 0x01;
        out_buf[off++] = (uint8_t)sig_len;
    }
    ns_memcpy(out_buf + off, replysig, sig_len);
    off += (uint32_t)sig_len;

    /* 5. Reply arguments array container */
    if (off + 9 > max_buf_len) return XW_ERR_NOSPACE;
    out_buf[off++] = NSXPC_TAG_ARRAY | 0x01; /* count = 1 */
    uint32_t arr_end_pos = off;
    off += 8; /* placeholder */

    /* 6. Argument 0 (the answer) */
    if (ans == NULL) {
        if (off + 1 > max_buf_len) return XW_ERR_NOSPACE;
        out_buf[off++] = NSXPC_TAG_NIL;
    } else {
        int r = 0;
        switch (ans->kind) {
        case MG_ANS_BOOL:
            if (off + 1 > max_buf_len) return XW_ERR_NOSPACE;
            out_buf[off++] = ans->b ? NSXPC_TAG_BOOL_TRUE : NSXPC_TAG_BOOL_FALSE;
            break;
        case MG_ANS_STRING:
            r = ns_append_str(out_buf, max_buf_len, &off, ans->s);
            if (r != 0) return r;
            break;
        case MG_ANS_INT:
            r = ns_append_int(out_buf, max_buf_len, &off, ans->i);
            if (r != 0) return r;
            break;
        case MG_ANS_REAL:
            if (off + 9 > max_buf_len) return XW_ERR_NOSPACE;
            out_buf[off++] = NSXPC_TAG_REAL | 0x03; /* 8-byte double */
            {
                uint64_t bits = 0;
                ns_memcpy(&bits, &ans->d, sizeof(double));
                ns_write_u64_le(out_buf + off, bits);
                off += 8;
            }
            break;
        case MG_ANS_DATA:
            if (ans->data_len < 15) {
                if (off + 1 + ans->data_len > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = (uint8_t)(NSXPC_TAG_DATA | ans->data_len);
            } else {
                if (off + 3 + ans->data_len > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = NSXPC_TAG_DATA | 0x0f;
                if (ans->data_len <= 0xff) {
                    out_buf[off++] = NSXPC_TAG_INT | 0x01;
                    out_buf[off++] = (uint8_t)ans->data_len;
                } else {
                    out_buf[off++] = NSXPC_TAG_INT | 0x02;
                    ns_write_u16_le(out_buf + off, (uint16_t)ans->data_len);
                    off += 2;
                }
            }
            if (ans->data_len > 0 && ans->data != NULL) {
                ns_memcpy(out_buf + off, ans->data, ans->data_len);
                off += ans->data_len;
            }
            break;
        case MG_ANS_STRING_ARRAY:
            if (ans->count < 15) {
                if (off + 9 > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = (uint8_t)(NSXPC_TAG_ARRAY | ans->count);
            } else {
                if (off + 11 > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = NSXPC_TAG_ARRAY | 0x0f;
                out_buf[off++] = NSXPC_TAG_INT | 0x01;
                out_buf[off++] = (uint8_t)ans->count;
            }
            {
                uint32_t sub_end_pos = off;
                off += 8;
                for (uint32_t j = 0; j < ans->count; j++) {
                    r = ns_append_str(out_buf, max_buf_len, &off, ans->strs[j]);
                    if (r != 0) return r;
                }
                ns_write_u64_le(out_buf + sub_end_pos, (uint64_t)(off - 1));
            }
            break;
        case MG_ANS_INT_ARRAY:
            if (ans->count < 15) {
                if (off + 9 > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = (uint8_t)(NSXPC_TAG_ARRAY | ans->count);
            } else {
                if (off + 11 > max_buf_len) return XW_ERR_NOSPACE;
                out_buf[off++] = NSXPC_TAG_ARRAY | 0x0f;
                out_buf[off++] = NSXPC_TAG_INT | 0x01;
                out_buf[off++] = (uint8_t)ans->count;
            }
            {
                uint32_t sub_end_pos = off;
                off += 8;
                for (uint32_t j = 0; j < ans->count; j++) {
                    r = ns_append_int(out_buf, max_buf_len, &off, ans->ints[j]);
                    if (r != 0) return r;
                }
                ns_write_u64_le(out_buf + sub_end_pos, (uint64_t)(off - 1));
            }
            break;
        case MG_ANS_ARTWORK_DICT:
            r = ns_append_artwork_dict(out_buf, max_buf_len, &off);
            if (r != 0) return r;
            break;
        default:
            return XW_ERR_BAD_TYPE;
        }
    }

    /* 7. Finalize container end offsets */
    ns_write_u64_le(out_buf + arr_end_pos, (uint64_t)(off - 1));
    ns_write_u64_le(out_buf + container_end_pos, (uint64_t)(off - 1));

    *out_len = off;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reply Dictionary Builders                                          */
/* ------------------------------------------------------------------ */

int nsxpc_build_reply_dict(xw_builder *reply, const char *replysig_or_sel,
                           const mg_answer_t *ans) {
    uint8_t bplist_buf[NSXPC_MAX_PAYLOAD_LEN];
    uint32_t bplist_len = 0;
    int r = nsxpc_encode_reply(replysig_or_sel, ans, bplist_buf,
                               sizeof(bplist_buf), &bplist_len);
    if (r != 0) return r;

    r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_data(reply, "root", bplist_buf, bplist_len);
    if (r == XW_OK) r = xw_dict_end(reply);
    return r;
}

int nsxpc_build_bool_reply_dict(xw_builder *reply, const char *replysig_or_sel,
                                int success) {
    mg_answer_t ans;
    ns_memset(&ans, 0, sizeof(ans));
    ans.kind = MG_ANS_BOOL;
    ans.b = success ? 1 : 0;
    return nsxpc_build_reply_dict(reply, replysig_or_sel, &ans);
}

int nsxpc_build_nil_reply_dict(xw_builder *reply, const char *replysig_or_sel) {
    return nsxpc_build_reply_dict(reply, replysig_or_sel, NULL);
}
