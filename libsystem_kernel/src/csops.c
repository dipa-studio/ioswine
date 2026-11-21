/*
 * csops.c: parse an LC_CODE_SIGNATURE superblob and answer
 * csops/entitlement queries.
 *
 * Freestanding: no libc, only <stdint.h> and <stddef.h>, with the
 * same static xx_ helper convention as bplist.c and svchost.c.
 * Every multi-byte field is read big-endian through cs_rd32; no
 * multi-byte value is ever read through an unaligned pointer.
 *
 * Contents:
 *   1. Small static helpers and the big-endian reader.
 *   2. SHA-256 (FIPS 180-4), written fresh, for the cdhash.
 *   3. csops_parse and the CodeDirectory validator.
 *   4. Additive syscall answers: cdhash, STATUS flags, team id,
 *      identifier.
 *   5. Minimal XML plist reader (slot 5) plus the bplist sniff.
 *   6. csops_entitlements_plist and csops_has_entitlement.
 *
 * The XML reader handles the plist grammar only: plist, dict, key,
 * string, integer, real, true, false, array, data, date. It does
 * not handle CDATA sections or DTD internal subsets (a DOCTYPE
 * without an internal subset is skipped). Entity references
 * (amp, lt, gt, quot, apos, decimal and hex numeric) are decoded.
 * Nesting is capped at BP_MAX_DEPTH.
 *
 * No wire-string literals live in this file: every string the code
 * compares against comes from csops_names.h.
 *
 * No em dashes are used in this file.
 */

#include <stdint.h>
#include <stddef.h>
#include "csops.h"

/* ------------------------------------------------------------------ */
/* 1. Small static helpers.                                            */
/* ------------------------------------------------------------------ */

static void cs_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void cs_memset(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int cs_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static const void *cs_memchr(const void *p, int c, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == (uint8_t)c) return s + i;
    }
    return NULL;
}

static int cs_strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* Compare a byte span of known length against a NUL-terminated
 * word. Returns 0 on equality. */
static int cs_span_eq(const uint8_t *s, uint32_t len, const char *word) {
    uint32_t wlen = 0;
    uint32_t i;
    while (word[wlen] != 0) wlen++;
    if (len != wlen) return 1;
    for (i = 0; i < len; i++) {
        if (s[i] != (uint8_t)word[i]) return 1;
    }
    return 0;
}

/* Read a big-endian uint32. The caller guarantees 4 bytes are
 * readable. */
static uint32_t cs_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Reserve n bytes at the end of a bp_arena. Returns NULL when the
 * arena cannot hold them. Mirrors the bounds logic of bplist.c. */
static uint8_t *cs_arena_reserve(bp_arena *a, uint32_t n) {
    uint8_t *p;
    if (a == NULL || a->buf == NULL) return NULL;
    if (a->len > a->cap) return NULL;
    if ((uint64_t)n > (uint64_t)a->cap - a->len) return NULL;
    p = a->buf + a->len;
    a->len += n;
    return p;
}

/* ------------------------------------------------------------------ */
/* 2. SHA-256 (FIPS 180-4), written fresh.                            */
/* ------------------------------------------------------------------ */

#define CS_ROTR32(v, n) (((v) >> (n)) | ((v) << (32 - (n))))

static const uint32_t cs_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void cs_sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = CS_ROTR32(w[i - 15], 7) ^
                      CS_ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = CS_ROTR32(w[i - 2], 17) ^
                      CS_ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    b = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = CS_ROTR32(e, 6) ^ CS_ROTR32(e, 11) ^
                      CS_ROTR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + cs_sha256_k[i] + w[i];
        uint32_t s0 = CS_ROTR32(a, 2) ^ CS_ROTR32(a, 13) ^
                      CS_ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void cs_sha256(const uint8_t *data, uint32_t len,
               uint8_t out[CS_SHA256_LEN]) {
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    uint8_t block[64];
    uint32_t off = 0;
    uint32_t rem;
    uint32_t i;
    uint64_t bits;
    if (out == NULL) return;
    if (data == NULL && len != 0) return;
    while (len - off >= 64) {
        cs_sha256_block(h, data + off);
        off += 64;
    }
    rem = len - off;
    cs_memset(block, 0, (uint32_t)sizeof(block));
    for (i = 0; i < rem; i++) block[i] = data[off + i];
    block[rem] = 0x80;
    if (rem >= 56) {
        cs_sha256_block(h, block);
        cs_memset(block, 0, (uint32_t)sizeof(block));
    }
    bits = (uint64_t)len * 8u;
    block[56] = (uint8_t)(bits >> 56);
    block[57] = (uint8_t)(bits >> 48);
    block[58] = (uint8_t)(bits >> 40);
    block[59] = (uint8_t)(bits >> 32);
    block[60] = (uint8_t)(bits >> 24);
    block[61] = (uint8_t)(bits >> 16);
    block[62] = (uint8_t)(bits >> 8);
    block[63] = (uint8_t)bits;
    cs_sha256_block(h, block);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

/* ------------------------------------------------------------------ */
/* 3. csops_parse and the CodeDirectory validator.                     */
/* ------------------------------------------------------------------ */

/* Validate a CodeDirectory blob. cd points at the blob start,
 * blob_len is the length from the SuperBlob index. The CD's own
 * length field is authoritative: it must fit inside blob_len, and
 * all offsets are validated against it. *cd_len_out receives the
 * CD internal length on success. Returns 0 or a negative XW_ERR_*. */
static int cs_validate_cd(const uint8_t *cd, uint32_t blob_len,
                          uint32_t *cd_len_out) {
    uint32_t internal_len, version, hash_offset, ident_offset;
    uint32_t n_special, n_code, code_limit;
    uint32_t hash_size, hash_type, page_size;
    uint64_t hash_bytes;
    if (blob_len < 8) return XW_ERR_TRUNCATED;
    if (cs_rd32(cd) != CS_MAGIC_CODEDIRECTORY) return XW_ERR_BAD_MAGIC;
    internal_len = cs_rd32(cd + 4);
    if (internal_len < 44 || internal_len > blob_len)
        return XW_ERR_TRUNCATED;
    version = cs_rd32(cd + 8);
    if (version < CS_CD_VERSION_20100 || version > CS_CD_VERSION_20500)
        return XW_ERR_BAD_VERSION;
    if (version >= CS_CD_VERSION_20100 && internal_len < 48)
        return XW_ERR_TRUNCATED;
    if (version >= CS_CD_VERSION_20200 && internal_len < 52)
        return XW_ERR_TRUNCATED;
    if (version >= CS_CD_VERSION_20300 && internal_len < 64)
        return XW_ERR_TRUNCATED;
    if (version >= CS_CD_VERSION_20400 && internal_len < 88)
        return XW_ERR_TRUNCATED;
    if (version >= CS_CD_VERSION_20500 && internal_len < 96)
        return XW_ERR_TRUNCATED;
    hash_offset = cs_rd32(cd + 16);
    ident_offset = cs_rd32(cd + 20);
    n_special = cs_rd32(cd + 24);
    n_code = cs_rd32(cd + 28);
    code_limit = cs_rd32(cd + 32);
    hash_size = cd[36];
    hash_type = cd[37];
    page_size = cd[39];
    if (hash_type < CS_HASH_TYPE_SHA1 || hash_type > CS_HASH_TYPE_SHA384)
        return XW_ERR_BAD_TYPE;
    if (hash_size == 0 || hash_size > 64) return XW_ERR_BAD_TYPE;
    if (hash_offset < 44 || hash_offset > internal_len)
        return XW_ERR_TRUNCATED;
    hash_bytes = (uint64_t)n_special * hash_size +
                 (uint64_t)n_code * hash_size;
    if ((uint64_t)hash_offset + hash_bytes > internal_len)
        return XW_ERR_TRUNCATED;
    if (ident_offset == 0 || ident_offset >= internal_len)
        return XW_ERR_TRUNCATED;
    if (cs_memchr(cd + ident_offset, 0,
                  internal_len - ident_offset) == NULL)
        return XW_ERR_TRUNCATED;
    if (page_size > 20) return XW_ERR_BAD_TYPE;
    if (code_limit > 0) {
        uint32_t page = 1u << page_size;
        uint32_t expect = (code_limit + page - 1u) / page;
        if (expect != n_code) return XW_ERR_BAD_TYPE;
    }
    if (version >= CS_CD_VERSION_20200) {
        uint32_t team_offset = cs_rd32(cd + 48);
        if (team_offset != 0) {
            if (team_offset >= internal_len) return XW_ERR_TRUNCATED;
            if (cs_memchr(cd + team_offset, 0,
                          internal_len - team_offset) == NULL)
                return XW_ERR_TRUNCATED;
        }
    }
    *cd_len_out = internal_len;
    return XW_OK;
}

int csops_parse(const uint8_t *superblob, uint32_t len,
                csops_info *out) {
    uint32_t sb_len, count, i;
    uint64_t idx_end;
    const uint8_t *cd = NULL;
    uint32_t cd_blob_len = 0;
    uint32_t cd_len = 0;
    const uint8_t *ent5 = NULL;
    uint32_t ent5_blob_len = 0;
    const uint8_t *ent7 = NULL;
    uint32_t ent7_blob_len = 0;
    int rc;
    if (superblob == NULL || out == NULL) return XW_ERR_BAD_TYPE;
    cs_memset(out, 0, (uint32_t)sizeof(*out));
    if (len < 12) return XW_ERR_TRUNCATED;
    if (cs_rd32(superblob) != CS_MAGIC_SUPERBLOB)
        return XW_ERR_BAD_MAGIC;
    sb_len = cs_rd32(superblob + 4);
    if (sb_len < 12 || sb_len > len) return XW_ERR_TRUNCATED;
    count = cs_rd32(superblob + 8);
    idx_end = 12ull + 8ull * (uint64_t)count;
    if (idx_end > sb_len) return XW_ERR_TRUNCATED;
    for (i = 0; i < count; i++) {
        const uint8_t *e = superblob + 12 + 8u * i;
        uint32_t type = cs_rd32(e);
        uint32_t off = cs_rd32(e + 4);
        uint32_t blen;
        if ((uint64_t)off < idx_end || (uint64_t)off + 8u > sb_len)
            return XW_ERR_TRUNCATED;
        blen = cs_rd32(superblob + off + 4);
        if (blen < 8 || (uint64_t)off + blen > sb_len)
            return XW_ERR_TRUNCATED;
        switch (type) {
        case CS_SLOT_CODEDIRECTORY:
            if (cd == NULL) {
                cd = superblob + off;
                cd_blob_len = blen;
            }
            break;
        case CS_SLOT_ENTITLEMENTS:
            if (ent5 == NULL) {
                ent5 = superblob + off;
                ent5_blob_len = blen;
            }
            break;
        case CS_SLOT_DER_ENTITLEMENTS:
            if (ent7 == NULL) {
                ent7 = superblob + off;
                ent7_blob_len = blen;
            }
            break;
        default:
            break;
        }
    }
    if (cd != NULL) {
        rc = cs_validate_cd(cd, cd_blob_len, &cd_len);
        if (rc != XW_OK) return rc;
        out->has_code_directory = 1;
        out->code_directory = cd;
        out->code_directory_len = cd_len;
    }
    /* Slot 5 is preferred over slot 7 whatever the index order. */
    if (ent5 != NULL) {
        out->has_entitlements = 1;
        out->entitlements_is_der = 0;
        out->entitlements_blob = ent5;
        out->entitlements_blob_len = ent5_blob_len;
        out->entitlements = ent5 + 8;
        out->entitlements_len = ent5_blob_len - 8;
    } else if (ent7 != NULL) {
        out->has_entitlements = 1;
        out->entitlements_is_der = 1;
        out->entitlements_blob = ent7;
        out->entitlements_blob_len = ent7_blob_len;
        out->entitlements = ent7 + 8;
        out->entitlements_len = ent7_blob_len - 8;
    }
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* 4. Additive syscall answers.                                        */
/* ------------------------------------------------------------------ */

int csops_cdhash(const csops_info *info, uint8_t *out, uint32_t out_cap,
                 uint32_t *out_len) {
    uint8_t digest[CS_SHA256_LEN];
    uint32_t hash_type, hash_size, need;
    if (info == NULL || !info->has_code_directory ||
        info->code_directory == NULL)
        return XW_ERR_NOT_FOUND;
    if (out == NULL || out_len == NULL) return XW_ERR_NOSPACE;
    hash_type = info->code_directory[37];
    hash_size = info->code_directory[36];
    if (hash_type == CS_HASH_TYPE_SHA256) {
        need = CS_SHA256_LEN;
    } else if (hash_type == CS_HASH_TYPE_SHA256_TRUNCATED) {
        if (hash_size == 0 || hash_size > CS_SHA256_LEN)
            return XW_ERR_BAD_TYPE;
        need = hash_size;
    } else {
        return XW_ERR_BAD_TYPE;
    }
    if (out_cap < need) return XW_ERR_NOSPACE;
    cs_sha256(info->code_directory, info->code_directory_len, digest);
    cs_memcpy(out, digest, need);
    *out_len = need;
    return XW_OK;
}

/* Read a NUL-terminated string out of the CodeDirectory at the
 * given field offset (the offset of a uint32 holding the string
 * offset). Returns 0 or a negative XW_ERR_*. */
static int cs_cd_string(const csops_info *info, uint32_t field_off,
                        char *out, uint32_t cap) {
    uint32_t off, avail, slen;
    const uint8_t *s;
    if (info == NULL || !info->has_code_directory ||
        info->code_directory == NULL)
        return XW_ERR_NOT_FOUND;
    if (out == NULL || cap == 0) return XW_ERR_NOSPACE;
    off = cs_rd32(info->code_directory + field_off);
    if (off == 0 || off >= info->code_directory_len)
        return XW_ERR_NOT_FOUND;
    s = info->code_directory + off;
    avail = info->code_directory_len - off;
    slen = 0;
    while (slen < avail && s[slen] != 0) slen++;
    if (slen >= avail) return XW_ERR_TRUNCATED;
    if (slen + 1 > cap) return XW_ERR_NOSPACE;
    cs_memcpy(out, s, slen + 1);
    return XW_OK;
}

int csops_team_id(const csops_info *info, char *out, uint32_t cap) {
    uint32_t version;
    if (info == NULL || !info->has_code_directory ||
        info->code_directory == NULL)
        return XW_ERR_NOT_FOUND;
    version = cs_rd32(info->code_directory + 8);
    if (version < CS_CD_VERSION_20200) return XW_ERR_NOT_FOUND;
    return cs_cd_string(info, 48, out, cap);
}

int csops_identifier(const csops_info *info, char *out, uint32_t cap) {
    return cs_cd_string(info, 20, out, cap);
}

uint32_t csops_status_flags(const csops_info *info, bp_arena *a) {
    uint32_t flags = CS_VALID | CS_ADHOC;
    bp_value *ents;
    bp_value *gta;
    if (info == NULL || !info->has_code_directory) return 0;
    if (a == NULL || !info->has_entitlements ||
        info->entitlements_is_der)
        return flags;
    ents = csops_entitlements_plist(info, a);
    if (ents == NULL) return flags;
    gta = bp_dict_get(ents, CSOPS_ENT_GET_TASK_ALLOW);
    if (gta != NULL && gta->type == BP_BOOL && gta->b != 0)
        flags |= CS_GET_TASK_ALLOW;
    return flags;
}

/* ------------------------------------------------------------------ */
/* 5. Minimal XML plist reader (slot 5) plus the bplist sniff.        */
/* ------------------------------------------------------------------ */

/* Encode one Unicode code point as UTF-8. Returns 0 or a negative
 * XW_ERR_*. */
static int cs_utf8_encode(uint32_t cp, uint8_t *dst, uint32_t cap,
                          uint32_t *n) {
    if (cp == 0 || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
        return XW_ERR_BAD_TYPE;
    if (cp < 0x80u) {
        if (cap < 1) return XW_ERR_NOSPACE;
        dst[0] = (uint8_t)cp;
        *n = 1;
    } else if (cp < 0x800u) {
        if (cap < 2) return XW_ERR_NOSPACE;
        dst[0] = (uint8_t)(0xc0u | (cp >> 6));
        dst[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        *n = 2;
    } else if (cp < 0x10000u) {
        if (cap < 3) return XW_ERR_NOSPACE;
        dst[0] = (uint8_t)(0xe0u | (cp >> 12));
        dst[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        dst[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        *n = 3;
    } else {
        if (cap < 4) return XW_ERR_NOSPACE;
        dst[0] = (uint8_t)(0xf0u | (cp >> 18));
        dst[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
        dst[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        dst[3] = (uint8_t)(0x80u | (cp & 0x3fu));
        *n = 4;
    }
    return XW_OK;
}

/* Decode entity references in a raw text span into dst. Raw UTF-8
 * is validated and passed through unchanged. The decoded output
 * never exceeds the raw span length, so cap == len + 1 always
 * suffices. Returns 0 or a negative XW_ERR_*; *out_len is the
 * decoded length, excluding any NUL terminator the caller adds. */
static int cs_xml_decode_span(const uint8_t *s, uint32_t len,
                              uint8_t *dst, uint32_t cap,
                              uint32_t *out_len) {
    uint32_t i = 0;
    uint32_t o = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c == '&') {
            uint32_t elen = 0;
            int rc;
            uint32_t n = 0;
            while (elen < 10 && i + 1 + elen < len &&
                   s[i + 1 + elen] != ';')
                elen++;
            if (i + 1 + elen >= len || s[i + 1 + elen] != ';')
                return XW_ERR_BAD_TYPE;
            if (elen == 0 || elen > 8) return XW_ERR_BAD_TYPE;
            if (s[i + 1] == '#') {
                uint32_t cp = 0;
                uint32_t j;
                uint32_t start = i + 2;
                uint32_t blen = elen - 1;
                int hex = 0;
                if (blen > 0 && (s[start] == 'x' || s[start] == 'X')) {
                    hex = 1;
                    start++;
                    blen--;
                }
                if (blen == 0) return XW_ERR_BAD_TYPE;
                for (j = 0; j < blen; j++) {
                    uint8_t d = s[start + j];
                    uint32_t v;
                    if (d >= '0' && d <= '9') {
                        v = (uint32_t)(d - '0');
                    } else if (hex && d >= 'a' && d <= 'f') {
                        v = (uint32_t)(d - 'a') + 10u;
                    } else if (hex && d >= 'A' && d <= 'F') {
                        v = (uint32_t)(d - 'A') + 10u;
                    } else {
                        return XW_ERR_BAD_TYPE;
                    }
                    if (hex) {
                        if (cp > (0x10ffffu - v) / 16u)
                            return XW_ERR_BAD_TYPE;
                        cp = cp * 16u + v;
                    } else {
                        if (cp > (0x10ffffu - v) / 10u)
                            return XW_ERR_BAD_TYPE;
                        cp = cp * 10u + v;
                    }
                }
                rc = cs_utf8_encode(cp, dst + o, cap - o, &n);
                if (rc != XW_OK) return rc;
                o += n;
            } else {
                uint8_t rep;
                if (cs_span_eq(s + i + 1, elen, CSOPS_XML_ENT_AMP) == 0) {
                    rep = '&';
                } else if (cs_span_eq(s + i + 1, elen,
                                       CSOPS_XML_ENT_LT) == 0) {
                    rep = '<';
                } else if (cs_span_eq(s + i + 1, elen,
                                       CSOPS_XML_ENT_GT) == 0) {
                    rep = '>';
                } else if (cs_span_eq(s + i + 1, elen,
                                       CSOPS_XML_ENT_QUOT) == 0) {
                    rep = '"';
                } else if (cs_span_eq(s + i + 1, elen,
                                       CSOPS_XML_ENT_APOS) == 0) {
                    rep = '\'';
                } else {
                    return XW_ERR_BAD_TYPE;
                }
                if (o + 1 > cap) return XW_ERR_NOSPACE;
                dst[o++] = rep;
            }
            i += 1 + elen + 1;
            continue;
        }
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            return XW_ERR_BAD_TYPE;
        if (c < 0x80) {
            if (o + 1 > cap) return XW_ERR_NOSPACE;
            dst[o++] = c;
            i++;
            continue;
        }
        {
            uint32_t seqlen, j;
            if (c >= 0xc2 && c <= 0xdf) {
                seqlen = 2;
            } else if (c >= 0xe0 && c <= 0xef) {
                seqlen = 3;
            } else if (c >= 0xf0 && c <= 0xf4) {
                seqlen = 4;
            } else {
                return XW_ERR_BAD_TYPE;
            }
            if (i + seqlen > len) return XW_ERR_TRUNCATED;
            for (j = 1; j < seqlen; j++) {
                if ((s[i + j] & 0xc0) != 0x80)
                    return XW_ERR_BAD_TYPE;
            }
            if (c == 0xe0 && s[i + 1] < 0xa0)
                return XW_ERR_BAD_TYPE;
            if (c == 0xed && s[i + 1] > 0x9f)
                return XW_ERR_BAD_TYPE;
            if (c == 0xf0 && s[i + 1] < 0x90)
                return XW_ERR_BAD_TYPE;
            if (c == 0xf4 && s[i + 1] > 0x8f)
                return XW_ERR_BAD_TYPE;
            if (o + seqlen > cap) return XW_ERR_NOSPACE;
            for (j = 0; j < seqlen; j++) dst[o++] = s[i + j];
            i += seqlen;
        }
    }
    *out_len = o;
    return XW_OK;
}

/* One base64 digit. Returns 0 or XW_ERR_BAD_TYPE. */
static int cs_b64_val(uint8_t c, uint32_t *v) {
    if (c >= 'A' && c <= 'Z') {
        *v = (uint32_t)(c - 'A');
        return XW_OK;
    }
    if (c >= 'a' && c <= 'z') {
        *v = (uint32_t)(c - 'a') + 26u;
        return XW_OK;
    }
    if (c >= '0' && c <= '9') {
        *v = (uint32_t)(c - '0') + 52u;
        return XW_OK;
    }
    if (c == '+') {
        *v = 62;
        return XW_OK;
    }
    if (c == '/') {
        *v = 63;
        return XW_OK;
    }
    return XW_ERR_BAD_TYPE;
}

/* Strict base64 decode: whitespace is ignored, padding may only
 * appear as the last one or two characters of the final quad, and
 * nothing may follow a padded quad. The decoded output never
 * exceeds the raw span length. Returns 0 or a negative XW_ERR_*. */
static int cs_b64_decode(const uint8_t *s, uint32_t len, uint8_t *dst,
                         uint32_t cap, uint32_t *out_len) {
    uint32_t vals[4];
    uint32_t q = 0;
    uint32_t pads = 0;
    uint32_t o = 0;
    uint32_t i;
    int closed = 0;
    for (i = 0; i < len; i++) {
        uint8_t c = s[i];
        uint32_t v;
        int rc;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (closed) return XW_ERR_BAD_TYPE;
        if (c == '=') {
            if (q < 2) return XW_ERR_BAD_TYPE;
            pads++;
            if (pads > 2) return XW_ERR_BAD_TYPE;
            vals[q++] = 0;
            if (q == 4) {
                if (pads == 1) {
                    if (o + 2 > cap) return XW_ERR_NOSPACE;
                    dst[o++] = (uint8_t)((vals[0] << 2) |
                                         (vals[1] >> 4));
                    dst[o++] = (uint8_t)((vals[1] << 4) |
                                         (vals[2] >> 2));
                } else {
                    if (o + 1 > cap) return XW_ERR_NOSPACE;
                    dst[o++] = (uint8_t)((vals[0] << 2) |
                                         (vals[1] >> 4));
                }
                closed = 1;
                q = 0;
                pads = 0;
            }
            continue;
        }
        if (pads != 0) return XW_ERR_BAD_TYPE;
        rc = cs_b64_val(c, &v);
        if (rc != XW_OK) return rc;
        vals[q++] = v;
        if (q == 4) {
            if (o + 3 > cap) return XW_ERR_NOSPACE;
            dst[o++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
            dst[o++] = (uint8_t)((vals[1] << 4) | (vals[2] >> 2));
            dst[o++] = (uint8_t)((vals[2] << 6) | vals[3]);
            q = 0;
        }
    }
    if (q != 0) return XW_ERR_BAD_TYPE;
    *out_len = o;
    return XW_OK;
}

/* Parse a decimal integer span into a BP_INT or BP_UINT node.
 * Returns 0 or a negative XW_ERR_*. */
static int cs_parse_int(const uint8_t *s, uint32_t len, bp_arena *a,
                        bp_value **out) {
    uint32_t i = 0;
    uint64_t mag = 0;
    int neg = 0;
    bp_value *v;
    if (len == 0) return XW_ERR_BAD_TYPE;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    if (i >= len) return XW_ERR_BAD_TYPE;
    for (; i < len; i++) {
        uint8_t c = s[i];
        uint32_t d;
        if (c < '0' || c > '9') return XW_ERR_BAD_TYPE;
        d = (uint32_t)(c - '0');
        if (mag > (UINT64_MAX - d) / 10u) return XW_ERR_OVERFLOW;
        mag = mag * 10u + d;
    }
    if (neg) {
        if (mag > 0x8000000000000000ull) return XW_ERR_OVERFLOW;
        v = bp_new(a, BP_INT);
        if (v == NULL) return XW_ERR_NOSPACE;
        if (mag == 0x8000000000000000ull) {
            v->i = (-9223372036854775807ll - 1);
        } else {
            v->i = -(int64_t)mag;
        }
    } else if (mag <= 0x7fffffffffffffffull) {
        v = bp_new(a, BP_INT);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->i = (int64_t)mag;
    } else {
        v = bp_new(a, BP_UINT);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->u = mag;
    }
    *out = v;
    return XW_OK;
}

/* Parse a decimal real span into a BP_REAL node. The mantissa is
 * kept to 19 significant digits and scaled by powers of ten, which
 * is exact for the short decimals entitlements use but is not a
 * correctly rounded strtod; document this where the result is
 * consumed. Returns 0 or a negative XW_ERR_*. */
static int cs_parse_real(const uint8_t *s, uint32_t len, bp_arena *a,
                         bp_value **out) {
    uint32_t i = 0;
    uint64_t mant = 0;
    uint32_t mdigits = 0;
    int32_t exp10 = 0;
    int32_t e;
    int neg = 0;
    int seen_digit = 0;
    double r;
    bp_value *v;
    if (len == 0) return XW_ERR_BAD_TYPE;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        uint32_t d = (uint32_t)(s[i] - '0');
        seen_digit = 1;
        if (mant == 0 && d == 0 && mdigits == 0) {
            i++;
            continue;
        }
        if (mdigits < 19) {
            mant = mant * 10u + d;
            mdigits++;
        } else {
            exp10++;
        }
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            uint32_t d = (uint32_t)(s[i] - '0');
            seen_digit = 1;
            if (mant == 0 && d == 0 && mdigits == 0) {
                exp10--;
                i++;
                continue;
            }
            if (mdigits < 19) {
                mant = mant * 10u + d;
                mdigits++;
                exp10--;
            }
            i++;
        }
    }
    if (!seen_digit) return XW_ERR_BAD_TYPE;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int eneg = 0;
        int32_t eval = 0;
        i++;
        if (i < len && (s[i] == '-' || s[i] == '+')) {
            eneg = (s[i] == '-');
            i++;
        }
        if (i >= len) return XW_ERR_BAD_TYPE;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            uint32_t d = (uint32_t)(s[i] - '0');
            if (eval > 100000) return XW_ERR_OVERFLOW;
            eval = eval * 10 + (int32_t)d;
            i++;
        }
        if (eneg) {
            exp10 -= eval;
        } else {
            exp10 += eval;
        }
    }
    if (i != len) return XW_ERR_BAD_TYPE;
    if (mant == 0 || exp10 < -308) {
        v = bp_new(a, BP_REAL);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->d = 0.0;
        *out = v;
        return XW_OK;
    }
    if (exp10 > 308) return XW_ERR_OVERFLOW;
    r = (double)mant;
    if (exp10 >= 0) {
        for (e = 0; e < exp10; e++) r *= 10.0;
    } else {
        for (e = 0; e < -exp10; e++) r /= 10.0;
    }
    v = bp_new(a, BP_REAL);
    if (v == NULL) return XW_ERR_NOSPACE;
    v->d = neg ? -r : r;
    *out = v;
    return XW_OK;
}

/* Days from civil date, Howard Hinnant's algorithm, written fresh
 * here. Returns days since 1970-01-01. */
static int64_t cs_days_from_civil(int64_t y, int64_t m, int64_t d) {
    int64_t era, yoe, doy, doe;
    if (m <= 2) y -= 1;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static const uint8_t cs_days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Read exactly count decimal digits at s[pos]. Returns 0 or
 * XW_ERR_BAD_TYPE. */
static int cs_digits(const uint8_t *s, uint32_t pos, uint32_t count,
                     uint32_t *val) {
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint8_t c = s[pos + i];
        if (c < '0' || c > '9') return XW_ERR_BAD_TYPE;
        v = v * 10u + (uint32_t)(c - '0');
    }
    *val = v;
    return XW_OK;
}

/* Parse an ISO-8601 date YYYY-MM-DDTHH:MM:SS[.frac]Z into a BP_DATE
 * node (nanoseconds since the Unix epoch). Years outside roughly
 * 1677-2262 overflow the int64 nanosecond range and give
 * XW_ERR_OVERFLOW; that is a property of the bp_value date
 * representation, not of this parser. Returns 0 or a negative
 * XW_ERR_*. */
static int cs_parse_date(const uint8_t *s, uint32_t len, bp_arena *a,
                         bp_value **out) {
    uint32_t year, mon, day, hh, mm, ss;
    uint32_t frac_ns = 0;
    uint32_t i;
    uint32_t dim;
    int64_t days, secs;
    int rc;
    bp_value *v;
    if (len < 20) return XW_ERR_BAD_TYPE;
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':')
        return XW_ERR_BAD_TYPE;
    rc = cs_digits(s, 0, 4, &year);
    if (rc != XW_OK) return rc;
    rc = cs_digits(s, 5, 2, &mon);
    if (rc != XW_OK) return rc;
    rc = cs_digits(s, 8, 2, &day);
    if (rc != XW_OK) return rc;
    rc = cs_digits(s, 11, 2, &hh);
    if (rc != XW_OK) return rc;
    rc = cs_digits(s, 14, 2, &mm);
    if (rc != XW_OK) return rc;
    rc = cs_digits(s, 17, 2, &ss);
    if (rc != XW_OK) return rc;
    if (s[19] == 'Z') {
        if (len != 20) return XW_ERR_BAD_TYPE;
    } else if (s[19] == '.') {
        uint32_t fdigits = 0;
        uint32_t fval = 0;
        i = 20;
        while (i < len && s[i] >= '0' && s[i] <= '9' && fdigits < 9) {
            fval = fval * 10u + (uint32_t)(s[i] - '0');
            fdigits++;
            i++;
        }
        if (fdigits == 0) return XW_ERR_BAD_TYPE;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        if (i + 1 != len || s[i] != 'Z') return XW_ERR_BAD_TYPE;
        while (fdigits < 9) {
            fval *= 10u;
            fdigits++;
        }
        frac_ns = fval;
    } else {
        return XW_ERR_BAD_TYPE;
    }
    if (mon < 1 || mon > 12) return XW_ERR_BAD_TYPE;
    dim = cs_days_in_month[mon - 1];
    if (mon == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) ||
                   (year % 400 == 0);
        if (leap) dim = 29;
    }
    if (day < 1 || day > dim) return XW_ERR_BAD_TYPE;
    if (hh > 23 || mm > 59 || ss > 59) return XW_ERR_BAD_TYPE;
    days = cs_days_from_civil((int64_t)year, (int64_t)mon,
                              (int64_t)day);
    secs = days * 86400 + (int64_t)hh * 3600 + (int64_t)mm * 60 +
           (int64_t)ss;
    if (secs > 9223372036ll || secs < -9223372036ll)
        return XW_ERR_OVERFLOW;
    v = bp_new(a, BP_DATE);
    if (v == NULL) return XW_ERR_NOSPACE;
    v->i = secs * 1000000000ll + (int64_t)frac_ns;
    *out = v;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* XML parser core.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bp_arena *a;
    uint32_t depth; /* number of enclosing containers */
} cs_xml;

static int cs_xml_skip_ws(cs_xml *x) {
    while (x->p < x->end &&
           (*x->p == ' ' || *x->p == '\t' || *x->p == '\n' ||
            *x->p == '\r'))
        x->p++;
    return XW_OK;
}

/* Skip one comment starting at the current position. Returns 1 when
 * a comment was skipped, 0 when the position does not start a
 * comment, negative on an unterminated comment. */
static int cs_xml_skip_comment(cs_xml *x) {
    const uint8_t *q;
    if ((uint64_t)(x->end - x->p) < 4) return 0;
    if (cs_memcmp(x->p, CSOPS_XML_COMMENT_OPEN, 4) != 0) return 0;
    q = x->p + 4;
    while (x->end - q >= 3) {
        if (q[0] == '-' && q[1] == '-' && q[2] == '>') {
            x->p = q + 3;
            return 1;
        }
        q++;
    }
    return XW_ERR_TRUNCATED;
}

/* Skip one processing instruction, <? ... ?>. Returns 1, 0 or
 * negative, as cs_xml_skip_comment. */
static int cs_xml_skip_pi(cs_xml *x) {
    const uint8_t *q;
    if ((uint64_t)(x->end - x->p) < 2) return 0;
    if (x->p[0] != '<' || x->p[1] != '?') return 0;
    q = x->p + 2;
    while (x->end - q >= 2) {
        if (q[0] == '?' && q[1] == '>') {
            x->p = q + 2;
            return 1;
        }
        q++;
    }
    return XW_ERR_TRUNCATED;
}

/* Skip one declaration, <! ... >, for example DOCTYPE. Square
 * brackets are tracked so an internal subset does not end the
 * declaration early. Returns 1, 0 or negative, as above. */
static int cs_xml_skip_decl(cs_xml *x) {
    uint32_t depth = 0;
    if ((uint64_t)(x->end - x->p) < 2) return 0;
    if (x->p[0] != '<' || x->p[1] != '!') return 0;
    if ((uint64_t)(x->end - x->p) >= 4 &&
        cs_memcmp(x->p, CSOPS_XML_COMMENT_OPEN, 4) == 0)
        return 0;
    x->p += 2;
    while (x->p < x->end) {
        uint8_t c = *x->p;
        if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (depth > 0) depth--;
        } else if (c == '>' && depth == 0) {
            x->p++;
            return 1;
        }
        x->p++;
    }
    return XW_ERR_TRUNCATED;
}

/* Skip whitespace, comments, processing instructions and
 * declarations until the next element or the end of input. */
static int cs_xml_skip_misc(cs_xml *x) {
    int rc;
    for (;;) {
        cs_xml_skip_ws(x);
        rc = cs_xml_skip_comment(x);
        if (rc < 0) return rc;
        if (rc == 1) continue;
        rc = cs_xml_skip_pi(x);
        if (rc < 0) return rc;
        if (rc == 1) continue;
        rc = cs_xml_skip_decl(x);
        if (rc < 0) return rc;
        if (rc == 1) continue;
        return XW_OK;
    }
}

/* Read an element name starting at the current position into buf
 * (NUL-terminated). Returns 0 or a negative XW_ERR_*. */
static int cs_xml_read_name(cs_xml *x, char *buf, uint32_t cap) {
    uint32_t n = 0;
    uint8_t c;
    if (x->p >= x->end) return XW_ERR_TRUNCATED;
    c = *x->p;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          c == '_' || c == ':'))
        return XW_ERR_BAD_TYPE;
    while (x->p < x->end) {
        c = *x->p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == ':' ||
              c == '.' || c == '-'))
            break;
        if (n + 1 >= cap) return XW_ERR_OVERFLOW;
        buf[n++] = (char)c;
        x->p++;
    }
    if (n == 0) return XW_ERR_BAD_TYPE;
    buf[n] = 0;
    return XW_OK;
}

/* Read the element name starting just after the current '<'
 * without moving the cursor. */
static int cs_xml_peek_name(cs_xml *x, char *buf, uint32_t cap) {
    cs_xml tmp = *x;
    tmp.p++;
    return cs_xml_read_name(&tmp, buf, cap);
}

/* Skip an opening tag tail after the element name: attributes are
 * ignored, quoted values may contain '>'. Sets *self_close for a
 * trailing slash-angle. Returns 0 or a negative XW_ERR_*. */
static int cs_xml_skip_tag_tail(cs_xml *x, int *self_close) {
    *self_close = 0;
    for (;;) {
        uint8_t c;
        cs_xml_skip_ws(x);
        if (x->p >= x->end) return XW_ERR_TRUNCATED;
        c = *x->p;
        if (c == '>') {
            x->p++;
            return XW_OK;
        }
        if (c == '/' && x->p + 1 < x->end && x->p[1] == '>') {
            x->p += 2;
            *self_close = 1;
            return XW_OK;
        }
        if (c == '"' || c == '\'') {
            uint8_t q = c;
            x->p++;
            while (x->p < x->end && *x->p != q) x->p++;
            if (x->p >= x->end) return XW_ERR_TRUNCATED;
            x->p++;
            continue;
        }
        x->p++;
    }
}

/* Expect a closing tag </name> at the current position, allowing
 * whitespace before the '>'. Returns 0 or a negative XW_ERR_*. */
static int cs_xml_expect_close(cs_xml *x, const char *name) {
    char got[16];
    int rc;
    cs_xml_skip_ws(x);
    if ((uint64_t)(x->end - x->p) < 2) return XW_ERR_TRUNCATED;
    if (x->p[0] != '<' || x->p[1] != '/') return XW_ERR_BAD_TYPE;
    x->p += 2;
    rc = cs_xml_read_name(x, got, (uint32_t)sizeof(got));
    if (rc != XW_OK) return rc;
    if (cs_strcmp(got, name) != 0) return XW_ERR_BAD_TYPE;
    cs_xml_skip_ws(x);
    if (x->p >= x->end) return XW_ERR_TRUNCATED;
    if (*x->p != '>') return XW_ERR_BAD_TYPE;
    x->p++;
    return XW_OK;
}

/* Take the raw text span up to the next '<' or the end of input.
 * Returns 0; the span may be empty. */
static int cs_xml_text_span(cs_xml *x, const uint8_t **start,
                            uint32_t *len) {
    const uint8_t *s = x->p;
    while (x->p < x->end && *x->p != '<') x->p++;
    *start = s;
    *len = (uint32_t)(x->p - s);
    return XW_OK;
}

/* Parse a <string> or <key> style text element whose opening tag
 * was consumed. Decodes entities into freshly reserved arena space
 * and returns the NUL-terminated string and its decoded length
 * (excluding the NUL). A self-closing element is the empty string.
 * Returns 0 or a negative XW_ERR_*. */
static int cs_xml_text_value(cs_xml *x, const char *name, int self_close,
                             const char **str_out, uint32_t *dlen_out) {
    const uint8_t *span;
    uint32_t span_len;
    uint8_t *dst;
    uint32_t dlen = 0;
    int rc;
    if (self_close) {
        dst = cs_arena_reserve(x->a, 1);
        if (dst == NULL) return XW_ERR_NOSPACE;
        dst[0] = 0;
        *str_out = (const char *)dst;
        *dlen_out = 0;
        return XW_OK;
    }
    rc = cs_xml_text_span(x, &span, &span_len);
    if (rc != XW_OK) return rc;
    if (span_len == 0xffffffffu) return XW_ERR_NOSPACE;
    dst = cs_arena_reserve(x->a, span_len + 1);
    if (dst == NULL) return XW_ERR_NOSPACE;
    rc = cs_xml_decode_span(span, span_len, dst, span_len + 1, &dlen);
    if (rc != XW_OK) return rc;
    dst[dlen] = 0;
    rc = cs_xml_expect_close(x, name);
    if (rc != XW_OK) return rc;
    *str_out = (const char *)dst;
    *dlen_out = dlen;
    return XW_OK;
}

static int cs_xml_parse_value(cs_xml *x, bp_value **out);
static int cs_xml_parse_key(cs_xml *x, const char **key_out);

/* Parse one plist value element at the current position (which
 * must start an element, after misc). Returns 0 or a negative
 * XW_ERR_*. */
static int cs_xml_parse_value(cs_xml *x, bp_value **out) {
    char name[16];
    int self_close = 0;
    int rc;
    bp_value *v;
    rc = cs_xml_skip_misc(x);
    if (rc != XW_OK) return rc;
    if (x->p >= x->end || *x->p != '<') return XW_ERR_BAD_TYPE;
    if (x->p + 1 < x->end && x->p[1] == '/') return XW_ERR_BAD_TYPE;
    x->p++;
    rc = cs_xml_read_name(x, name, (uint32_t)sizeof(name));
    if (rc != XW_OK) return rc;
    rc = cs_xml_skip_tag_tail(x, &self_close);
    if (rc != XW_OK) return rc;
    if (cs_strcmp(name, CSOPS_XML_TRUE) == 0 ||
        cs_strcmp(name, CSOPS_XML_FALSE) == 0) {
        int b = (cs_strcmp(name, CSOPS_XML_TRUE) == 0) ? 1 : 0;
        if (!self_close) {
            rc = cs_xml_expect_close(x, name);
            if (rc != XW_OK) return rc;
        }
        v = bp_new(x->a, BP_BOOL);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->b = b;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_STRING) == 0) {
        const char *str = NULL;
        uint32_t dlen = 0;
        rc = cs_xml_text_value(x, name, self_close, &str, &dlen);
        if (rc != XW_OK) return rc;
        v = bp_new(x->a, BP_STRING);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->s = (const uint8_t *)str;
        v->len = dlen + 1;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_INTEGER) == 0) {
        const uint8_t *span;
        uint32_t span_len;
        uint8_t num[64];
        uint32_t dlen = 0;
        if (self_close) return XW_ERR_BAD_TYPE;
        rc = cs_xml_text_span(x, &span, &span_len);
        if (rc != XW_OK) return rc;
        rc = cs_xml_decode_span(span, span_len, num,
                                (uint32_t)sizeof(num), &dlen);
        if (rc != XW_OK) return rc;
        rc = cs_parse_int(num, dlen, x->a, &v);
        if (rc != XW_OK) return rc;
        rc = cs_xml_expect_close(x, name);
        if (rc != XW_OK) return rc;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_REAL) == 0) {
        const uint8_t *span;
        uint32_t span_len;
        uint8_t num[64];
        uint32_t dlen = 0;
        if (self_close) return XW_ERR_BAD_TYPE;
        rc = cs_xml_text_span(x, &span, &span_len);
        if (rc != XW_OK) return rc;
        rc = cs_xml_decode_span(span, span_len, num,
                                (uint32_t)sizeof(num), &dlen);
        if (rc != XW_OK) return rc;
        rc = cs_parse_real(num, dlen, x->a, &v);
        if (rc != XW_OK) return rc;
        rc = cs_xml_expect_close(x, name);
        if (rc != XW_OK) return rc;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_DATA) == 0) {
        const uint8_t *span = x->p;
        uint32_t span_len = 0;
        uint8_t *dst;
        uint32_t dlen = 0;
        if (!self_close) {
            rc = cs_xml_text_span(x, &span, &span_len);
            if (rc != XW_OK) return rc;
        }
        dst = cs_arena_reserve(x->a, span_len);
        if (dst == NULL) return XW_ERR_NOSPACE;
        rc = cs_b64_decode(span, span_len, dst, span_len, &dlen);
        if (rc != XW_OK) return rc;
        if (!self_close) {
            rc = cs_xml_expect_close(x, name);
            if (rc != XW_OK) return rc;
        }
        v = bp_new(x->a, BP_DATA);
        if (v == NULL) return XW_ERR_NOSPACE;
        v->s = dst;
        v->len = dlen;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_DATE) == 0) {
        const uint8_t *span;
        uint32_t span_len;
        uint8_t num[64];
        uint32_t dlen = 0;
        if (self_close) return XW_ERR_BAD_TYPE;
        rc = cs_xml_text_span(x, &span, &span_len);
        if (rc != XW_OK) return rc;
        rc = cs_xml_decode_span(span, span_len, num,
                                (uint32_t)sizeof(num), &dlen);
        if (rc != XW_OK) return rc;
        rc = cs_parse_date(num, dlen, x->a, &v);
        if (rc != XW_OK) return rc;
        rc = cs_xml_expect_close(x, name);
        if (rc != XW_OK) return rc;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_ARRAY) == 0) {
        v = bp_new(x->a, BP_ARRAY);
        if (v == NULL) return XW_ERR_NOSPACE;
        if (self_close) {
            *out = v;
            return XW_OK;
        }
        if (x->depth >= BP_MAX_DEPTH) return XW_ERR_OVERFLOW;
        x->depth++;
        for (;;) {
            bp_value *item = NULL;
            rc = cs_xml_skip_misc(x);
            if (rc != XW_OK) return rc;
            if (x->p + 1 < x->end && x->p[0] == '<' && x->p[1] == '/')
                break;
            if (x->p >= x->end) return XW_ERR_TRUNCATED;
            rc = cs_xml_parse_value(x, &item);
            if (rc != XW_OK) return rc;
            rc = bp_array_append(x->a, v, item);
            if (rc != XW_OK) return rc;
        }
        x->depth--;
        rc = cs_xml_expect_close(x, name);
        if (rc != XW_OK) return rc;
        *out = v;
        return XW_OK;
    }
    if (cs_strcmp(name, CSOPS_XML_DICT) == 0) {
        v = bp_new(x->a, BP_DICT);
        if (v == NULL) return XW_ERR_NOSPACE;
        if (self_close) {
            *out = v;
            return XW_OK;
        }
        if (x->depth >= BP_MAX_DEPTH) return XW_ERR_OVERFLOW;
        x->depth++;
        for (;;) {
            const char *key = NULL;
            bp_value *val = NULL;
            rc = cs_xml_skip_misc(x);
            if (rc != XW_OK) return rc;
            if (x->p + 1 < x->end && x->p[0] == '<' && x->p[1] == '/')
                break;
            if (x->p >= x->end) return XW_ERR_TRUNCATED;
            rc = cs_xml_parse_key(x, &key);
            if (rc != XW_OK) return rc;
            rc = cs_xml_parse_value(x, &val);
            if (rc != XW_OK) return rc;
            rc = bp_dict_set(x->a, v, key, val);
            if (rc != XW_OK) return rc;
        }
        x->depth--;
        rc = cs_xml_expect_close(x, name);
        if (rc != XW_OK) return rc;
        *out = v;
        return XW_OK;
    }
    /* key and plist are only valid in their own contexts, and
     * anything else is not plist grammar. */
    return XW_ERR_BAD_TYPE;
}

/* Parse one <key> element at the current position and return the
 * decoded key string (arena-owned, NUL-terminated). Returns 0 or a
 * negative XW_ERR_*. */
static int cs_xml_parse_key(cs_xml *x, const char **key_out) {
    char name[16];
    int self_close = 0;
    int rc;
    const char *str = NULL;
    uint32_t dlen = 0;
    rc = cs_xml_skip_misc(x);
    if (rc != XW_OK) return rc;
    if (x->p >= x->end || *x->p != '<') return XW_ERR_BAD_TYPE;
    if (x->p + 1 < x->end && x->p[1] == '/') return XW_ERR_BAD_TYPE;
    x->p++;
    rc = cs_xml_read_name(x, name, (uint32_t)sizeof(name));
    if (rc != XW_OK) return rc;
    if (cs_strcmp(name, CSOPS_XML_KEY) != 0) return XW_ERR_BAD_TYPE;
    rc = cs_xml_skip_tag_tail(x, &self_close);
    if (rc != XW_OK) return rc;
    rc = cs_xml_text_value(x, name, self_close, &str, &dlen);
    if (rc != XW_OK) return rc;
    *key_out = str;
    return XW_OK;
}

/* Parse a whole XML plist document: optional BOM, optional prolog,
 * then either a <plist> wrapper with exactly one value child or a
 * bare root value, then only misc to the end of input. Returns 0
 * or a negative XW_ERR_*. */
static int cs_xml_parse_document(const uint8_t *buf, uint32_t len,
                                 bp_arena *a, bp_value **root_out) {
    cs_xml x;
    char name[16];
    int self_close = 0;
    int rc;
    bp_value *root = NULL;
    if (buf == NULL || a == NULL || root_out == NULL)
        return XW_ERR_BAD_TYPE;
    x.p = buf;
    x.end = buf + len;
    x.a = a;
    x.depth = 0;
    if (len >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf)
        x.p += 3;
    rc = cs_xml_skip_misc(&x);
    if (rc != XW_OK) return rc;
    if (x.p >= x.end || *x.p != '<') return XW_ERR_BAD_TYPE;
    rc = cs_xml_peek_name(&x, name, (uint32_t)sizeof(name));
    if (rc != XW_OK) return rc;
    if (cs_strcmp(name, CSOPS_XML_PLIST) == 0) {
        x.p++;
        rc = cs_xml_read_name(&x, name, (uint32_t)sizeof(name));
        if (rc != XW_OK) return rc;
        rc = cs_xml_skip_tag_tail(&x, &self_close);
        if (rc != XW_OK) return rc;
        if (self_close) return XW_ERR_BAD_TYPE;
        rc = cs_xml_parse_value(&x, &root);
        if (rc != XW_OK) return rc;
        rc = cs_xml_skip_misc(&x);
        if (rc != XW_OK) return rc;
        rc = cs_xml_expect_close(&x, name);
        if (rc != XW_OK) return rc;
    } else {
        rc = cs_xml_parse_value(&x, &root);
        if (rc != XW_OK) return rc;
    }
    rc = cs_xml_skip_misc(&x);
    if (rc != XW_OK) return rc;
    if (x.p != x.end) return XW_ERR_BAD_TYPE;
    *root_out = root;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* 6. csops_entitlements_plist and csops_has_entitlement.              */
/* ------------------------------------------------------------------ */

bp_value *csops_entitlements_plist(const csops_info *info,
                                   bp_arena *a) {
    bp_value *root = NULL;
    const uint8_t *p;
    uint32_t len;
    int rc;
    if (info == NULL || a == NULL) return NULL;
    if (!info->has_entitlements || info->entitlements == NULL)
        return NULL;
    if (info->entitlements_is_der) return NULL;
    p = info->entitlements;
    len = info->entitlements_len;
    if (len == 0) return NULL;
    if (len >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
        p += 3;
        len -= 3;
    }
    while (len > 0 &&
           (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
        len--;
    }
    if (len == 0) return NULL;
    if (len >= 8 && cs_memcmp(p, CSOPS_BPLIST_MAGIC, 8) == 0) {
        rc = bp_parse(p, len, a, &root);
        if (rc != XW_OK) return NULL;
    } else if (*p == '<') {
        rc = cs_xml_parse_document(info->entitlements,
                                   info->entitlements_len, a, &root);
        if (rc != XW_OK) return NULL;
    } else {
        return NULL;
    }
    if (root == NULL || root->type != BP_DICT) return NULL;
    return root;
}

int csops_has_entitlement(const csops_info *info, const char *key,
                          bp_arena *a) {
    bp_value *ents;
    bp_value *v;
    if (info == NULL || key == NULL) return XW_ERR_BAD_TYPE;
    if (!info->has_entitlements) return 0;
    if (info->entitlements_is_der) return XW_ERR_BAD_TYPE;
    ents = csops_entitlements_plist(info, a);
    if (ents == NULL) return XW_ERR_BAD_TYPE;
    v = bp_dict_get(ents, key);
    if (v == NULL) return 0;
    if (v->type == BP_BOOL && v->b == 0) return 0;
    return 1;
}
