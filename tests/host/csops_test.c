/* csops_test.c: host test for csops.
 *
 * What it covers:
 *   1. Happy path, XML entitlements (CD 0x20500, SHA-256, slot 5
 *      XML plist, CMS slot); parse, plist lookup, STATUS flags,
 *      team id, identifier, cdhash against a host-computed value.
 *   2. DER-only entitlements (slot 7): plist NULL, lookup negative.
 *   3. Both slots present, in both index orders: slot 5 preferred.
 *   4. No entitlements: lookup 0, plist NULL, STATUS base flags.
 *   5. No CodeDirectory: parse still 0, STATUS 0, cdhash NOT_FOUND.
 *   6. Truncated superblob (length field, mid-index cut, cut inside
 *      a blob, blob length overrunning the superblob, CD length too
 *      small for its hash array, CD shorter than its header).
 *   7. Bad magic (superblob, CodeDirectory).
 *   8. Lying count.
 *   9. Index offset out of bounds (past the end, into the index
 *      array, into the header).
 *  10. Duplicate slot 5 and duplicate CodeDirectory: first wins.
 *  11. Entitlement lookup edges: get-task-allow false, malformed
 *      XML, binary-plist slot 5, rich XML (nested dict/array, data,
 *      date, real, integer incl. uint64 and INT64_MIN, entities,
 *      comments, DOCTYPE, empty containers).
 *  12. Malformed XML payloads: mismatched close tag, unknown
 *      entity, bad base64, unclosed comment, trailing junk,
 *      non-dict root, integer overflow, bad dates, empty integer,
 *      nesting past BP_MAX_DEPTH, bad UTF-8, junk inside true,
 *      CDATA (unsupported).
 *  13. cdhash: FIPS 180-4 vectors (empty, "abc"), SHA-1 CD refused,
 *      SHA-256-truncated CD returns the first hashSize bytes,
 *      small output buffer, NULL arguments.
 *  14. Additive API edges: NULL arguments, tiny output buffers,
 *      version 0x20100 CD (no team id), unknown CD version, unknown
 *      hash type, hash/identifier offsets out of range, nCodeSlots
 *      inconsistent with codeLimit, pageSize out of range.
 *  15. Fuzz csops_parse (10000 iterations) and the XML plist
 *      reader (5000 iterations): no crash, no hang, never a
 *      positive return, never an out-of-bounds pointer.
 *
 * The test builds every superblob from hand-written bytes; no Apple
 * binary is involved. The expected cdhash for the synthetic happy
 * path CD was computed independently with a Python replica of the
 * CD builder (hashlib.sha256):
 *   5d2e10a8883e6c1fd4879baa051c791b15cadf1c5ee9ea6659e85c0ab4779f0c
 *
 * Build:
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/csops_test tests/host/csops_test.c \
 *        src/csops.c src/bplist.c
 *     /tmp/csops_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/csops_test_asan \
 *        tests/host/csops_test.c src/csops.c src/bplist.c
 *     /tmp/csops_test_asan
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "csops.h"
#include "bplist.h"

/* ------------------------------------------------------------------ */
/* Harness.                                                            */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

static int g_scen_failures_before = 0;

#define SCEN_BEGIN(n, name) do { \
    g_scen_failures_before = g_failures; \
    printf("SCENARIO %d: %s ... ", (n), (name)); \
    fflush(stdout); \
} while (0)

#define SCEN_END(n) do { \
    if (g_failures == g_scen_failures_before) { \
        printf("PASS\n"); \
    } else { \
        printf("FAIL (%d failed checks)\n", \
               g_failures - g_scen_failures_before); \
    } \
    fflush(stdout); \
} while (0)

/* Deterministic xorshift PRNG (same shape as the xpcwire test). */
static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* ------------------------------------------------------------------ */
/* Blob builders (test-side, hand-written bytes).                      */
/* ------------------------------------------------------------------ */

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_u64be(uint8_t *p, uint64_t v) {
    put_u32be(p, (uint32_t)(v >> 32));
    put_u32be(p + 4, (uint32_t)v);
}

static uint32_t get_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

typedef struct {
    uint32_t version;
    uint32_t hash_type;
    uint32_t hash_size;
    uint32_t n_code;
    uint32_t n_special;
    uint32_t code_limit;
    uint32_t page_size;
    uint32_t flags;
    const char *ident;
    const char *team; /* NULL when the version has no team field */
} test_cd_params;

static uint32_t cd_header_size(uint32_t version) {
    uint32_t s = 44;
    if (version >= 0x20100) s += 4;
    if (version >= 0x20200) s += 4;
    if (version >= 0x20300) s += 12;
    if (version >= 0x20400) s += 24;
    if (version >= 0x20500) s += 8;
    return s;
}

/* Build a CodeDirectory. Returns its length, or 0 when the buffer
 * is too small. Hash bytes are sequential (j & 0xff), matching the
 * Python replica used for the expected cdhash. */
static uint32_t build_cd(uint8_t *buf, uint32_t cap,
                         const test_cd_params *p) {
    uint32_t hdr = cd_header_size(p->version);
    uint32_t hash_off = hdr;
    uint32_t hash_len = (p->n_special + p->n_code) * p->hash_size;
    uint32_t ident_off = hash_off + hash_len;
    uint32_t ident_len = (uint32_t)strlen(p->ident) + 1;
    uint32_t team_off = 0;
    uint32_t total;
    uint32_t o = 0;
    uint32_t j;
    if (p->team != NULL) {
        team_off = ident_off + ident_len;
        total = team_off + (uint32_t)strlen(p->team) + 1;
    } else {
        total = ident_off + ident_len;
    }
    if (cap < total) return 0;
    put_u32be(buf + o, 0xfade0c02u); o += 4;
    put_u32be(buf + o, total); o += 4;
    put_u32be(buf + o, p->version); o += 4;
    put_u32be(buf + o, p->flags); o += 4;
    put_u32be(buf + o, hash_off); o += 4;
    put_u32be(buf + o, ident_off); o += 4;
    put_u32be(buf + o, p->n_special); o += 4;
    put_u32be(buf + o, p->n_code); o += 4;
    put_u32be(buf + o, p->code_limit); o += 4;
    buf[o++] = (uint8_t)p->hash_size;
    buf[o++] = (uint8_t)p->hash_type;
    buf[o++] = 0; /* platform */
    buf[o++] = (uint8_t)p->page_size;
    put_u32be(buf + o, 0); o += 4; /* spare2 */
    if (p->version >= 0x20100) {
        put_u32be(buf + o, 0); o += 4; /* scatterOffset */
    }
    if (p->version >= 0x20200) {
        put_u32be(buf + o, team_off); o += 4; /* teamOffset */
    }
    if (p->version >= 0x20300) {
        put_u32be(buf + o, 0); o += 4; /* spare3 */
        put_u64be(buf + o, 0); o += 8; /* codeLimit64 */
    }
    if (p->version >= 0x20400) {
        put_u64be(buf + o, 0); o += 8; /* execSegBase */
        put_u64be(buf + o, 0); o += 8; /* execSegLimit */
        put_u64be(buf + o, 0); o += 8; /* execSegFlags */
    }
    if (p->version >= 0x20500) {
        put_u32be(buf + o, 0); o += 4; /* runtime */
        put_u32be(buf + o, 0); o += 4; /* preEncryptOffset */
    }
    if (o != hdr) return 0;
    for (j = 0; j < hash_len; j++) buf[o++] = (uint8_t)(j & 0xff);
    memcpy(buf + o, p->ident, ident_len); o += ident_len;
    if (p->team != NULL) {
        memcpy(buf + o, p->team, strlen(p->team) + 1);
        o += (uint32_t)strlen(p->team) + 1;
    }
    if (o != total) return 0;
    return total;
}

/* Build a generic blob: magic + length + payload. */
static uint32_t build_blob(uint8_t *buf, uint32_t cap, uint32_t magic,
                           const uint8_t *payload, uint32_t payload_len) {
    if (cap < 8 + payload_len) return 0;
    put_u32be(buf, magic);
    put_u32be(buf + 4, 8 + payload_len);
    if (payload_len > 0) memcpy(buf + 8, payload, payload_len);
    return 8 + payload_len;
}

typedef struct {
    uint32_t type;
    const uint8_t *data;
    uint32_t len;
} test_slot;

/* Build a superblob from prebuilt blobs, in index order. */
static uint32_t build_superblob(uint8_t *out, uint32_t cap,
                                const test_slot *slots,
                                uint32_t nslots) {
    uint32_t hdr = 12 + 8 * nslots;
    uint32_t o = hdr;
    uint32_t i;
    uint32_t total = hdr;
    for (i = 0; i < nslots; i++) total += slots[i].len;
    if (cap < total) return 0;
    put_u32be(out, 0xfade0cc0u);
    put_u32be(out + 4, total);
    put_u32be(out + 8, nslots);
    for (i = 0; i < nslots; i++) {
        put_u32be(out + 12 + 8 * i, slots[i].type);
        put_u32be(out + 12 + 8 * i + 4, o);
        memcpy(out + o, slots[i].data, slots[i].len);
        o += slots[i].len;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Fixtures.                                                           */
/* ------------------------------------------------------------------ */

static const char k_ent_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>application-identifier</key>\n"
    "\t<string>AAAAAAAAAA.com.example.app</string>\n"
    "\t<key>get-task-allow</key>\n"
    "\t<true/>\n"
    "</dict>\n"
    "</plist>\n";

static const uint8_t k_cdhash_expected[32] = {
    0x5d, 0x2e, 0x10, 0xa8, 0x88, 0x3e, 0x6c, 0x1f,
    0xd4, 0x87, 0x9b, 0xaa, 0x05, 0x1c, 0x79, 0x1b,
    0x15, 0xca, 0xdf, 0x1c, 0x5e, 0xe9, 0xea, 0x66,
    0x59, 0xe8, 0x5c, 0x0a, 0xb4, 0x77, 0x9f, 0x0c
};

/* FIPS 180-4 vectors. */
static const uint8_t k_sha256_empty[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
    0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
    0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};
static const uint8_t k_sha256_abc[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
};

/* 2026-11-12T10:00:00Z in nanoseconds since the Unix epoch. */
#define K_DATE_2026_NS 1794477600000000000ll

static uint8_t g_cd[1024];
static uint32_t g_cd_len;
static uint8_t g_ent_blob[2048];
static uint32_t g_ent_blob_len;
static uint8_t g_cms_blob[64];
static uint32_t g_cms_blob_len;
static uint8_t g_sb[4096];
static uint32_t g_sb_len;

static const test_cd_params k_cd_happy = {
    0x20500, 2, 32, 2, 7, 8192, 12, 0,
    "com.example.app", "AAAAAAAAAA"
};

static int build_fixtures(void) {
    test_slot slots[3];
    g_cd_len = build_cd(g_cd, sizeof(g_cd), &k_cd_happy);
    if (g_cd_len == 0) return -1;
    g_ent_blob_len = build_blob(g_ent_blob, sizeof(g_ent_blob),
                                0xfade7171u,
                                (const uint8_t *)k_ent_xml,
                                (uint32_t)strlen(k_ent_xml));
    if (g_ent_blob_len == 0) return -1;
    g_cms_blob_len = build_blob(g_cms_blob, sizeof(g_cms_blob),
                                0xfade0b01u,
                                (const uint8_t *)"cms-bytes", 9);
    if (g_cms_blob_len == 0) return -1;
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 5;
    slots[1].data = g_ent_blob;
    slots[1].len = g_ent_blob_len;
    slots[2].type = 0x10000;
    slots[2].data = g_cms_blob;
    slots[2].len = g_cms_blob_len;
    g_sb_len = build_superblob(g_sb, sizeof(g_sb), slots, 3);
    if (g_sb_len == 0) return -1;
    return 0;
}

/* Bounded tree walk for fuzz results. */
static int walk_tree(const bp_value *v, uint32_t depth, uint32_t *count) {
    const bp_value *c;
    if (v == NULL) return -1;
    if (depth > 40) return -1;
    if (v->type > BP_DICT) return -1;
    (*count)++;
    if (*count > 100000) return -1;
    for (c = v->child; c != NULL; c = c->next) {
        if (walk_tree(c, depth + 1, count) != 0) return -1;
    }
    return 0;
}

/* Build a superblob holding one slot 5 payload and a valid CD, then
 * check that csops_entitlements_plist returns NULL for it. */
static void check_plist_null(const uint8_t *payload, uint32_t len) {
    uint8_t ent[4096];
    uint8_t sb[8192];
    test_slot slots[2];
    uint32_t ent_len;
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[8192];
    bp_value *root;
    ent_len = build_blob(ent, sizeof(ent), 0xfade7171u, payload, len);
    CHECK(ent_len != 0);
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 5;
    slots[1].data = ent;
    slots[1].len = ent_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 2);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = csops_entitlements_plist(&info, &a);
    CHECK(root == NULL);
}

/* ------------------------------------------------------------------ */
/* Scenario 1: happy path, XML entitlements.                           */
/* ------------------------------------------------------------------ */

static void scen_1_happy(void) {
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[16384];
    bp_value *root;
    bp_value *v;
    uint8_t hash[32];
    uint32_t hash_len = 0;
    char team[32];
    char ident[64];
    SCEN_BEGIN(1, "happy path, XML entitlements");
    CHECK(csops_parse(g_sb, g_sb_len, &info) == XW_OK);
    CHECK(info.has_code_directory == 1);
    CHECK(info.has_entitlements == 1);
    CHECK(info.entitlements_is_der == 0);
    CHECK(info.entitlements_len == (uint32_t)strlen(k_ent_xml));
    CHECK(memcmp(info.entitlements, k_ent_xml,
                 info.entitlements_len) == 0);
    CHECK(info.entitlements_blob_len == info.entitlements_len + 8);
    CHECK(get_u32be(info.entitlements_blob) == 0xfade7171u);
    CHECK(info.code_directory_len == g_cd_len);
    CHECK(memcmp(info.code_directory, g_cd, g_cd_len) == 0);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = csops_entitlements_plist(&info, &a);
    CHECK(root != NULL);
    CHECK(root != NULL && root->type == BP_DICT);
    v = bp_dict_get(root, "application-identifier");
    CHECK(v != NULL && v->type == BP_STRING);
    CHECK(v != NULL &&
          strcmp((const char *)v->s, "AAAAAAAAAA.com.example.app") == 0);
    CHECK(csops_has_entitlement(&info, "application-identifier",
                                &a) == 1);
    CHECK(csops_has_entitlement(&info, "get-task-allow", &a) == 1);
    CHECK(csops_has_entitlement(&info,
                                "com.apple.developer.nonexistent",
                                &a) == 0);
    CHECK(csops_status_flags(&info, &a) ==
          (CS_VALID | CS_ADHOC | CS_GET_TASK_ALLOW));
    CHECK(csops_team_id(&info, team, sizeof(team)) == XW_OK);
    CHECK(strcmp(team, "AAAAAAAAAA") == 0);
    CHECK(csops_identifier(&info, ident, sizeof(ident)) == XW_OK);
    CHECK(strcmp(ident, "com.example.app") == 0);
    CHECK(csops_cdhash(&info, hash, sizeof(hash), &hash_len) == XW_OK);
    CHECK(hash_len == 32);
    CHECK(memcmp(hash, k_cdhash_expected, 32) == 0);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: DER-only entitlements (slot 7).                         */
/* ------------------------------------------------------------------ */

static void scen_2_der_only(void) {
    static const uint8_t der[] = { 0x30, 0x03, 0x02, 0x01, 0x05 };
    uint8_t ent[64];
    uint8_t sb[4096];
    test_slot slots[3];
    uint32_t ent_len;
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[4096];
    SCEN_BEGIN(2, "DER-only entitlements (slot 7)");
    ent_len = build_blob(ent, sizeof(ent), 0xfade7172u, der,
                         (uint32_t)sizeof(der));
    CHECK(ent_len != 0);
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 7;
    slots[1].data = ent;
    slots[1].len = ent_len;
    slots[2].type = 0x10000;
    slots[2].data = g_cms_blob;
    slots[2].len = g_cms_blob_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 3);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    CHECK(info.has_code_directory == 1);
    CHECK(info.has_entitlements == 1);
    CHECK(info.entitlements_is_der == 1);
    CHECK(info.entitlements_len == (uint32_t)sizeof(der));
    CHECK(memcmp(info.entitlements, der, sizeof(der)) == 0);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    CHECK(csops_entitlements_plist(&info, &a) == NULL);
    CHECK(csops_has_entitlement(&info, "application-identifier",
                                &a) < 0);
    CHECK(csops_status_flags(&info, &a) == (CS_VALID | CS_ADHOC));
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: both slots present, in both index orders.               */
/* ------------------------------------------------------------------ */

static void scen_3_both_slots(void) {
    static const uint8_t der[] = { 0x30, 0x03, 0x02, 0x01, 0x05 };
    uint8_t ent7[64];
    uint8_t sb[4096];
    test_slot slots[4];
    uint32_t ent7_len;
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[8192];
    int order;
    SCEN_BEGIN(3, "both slots present, slot 5 preferred");
    ent7_len = build_blob(ent7, sizeof(ent7), 0xfade7172u, der,
                          (uint32_t)sizeof(der));
    CHECK(ent7_len != 0);
    for (order = 0; order < 2; order++) {
        slots[0].type = 0;
        slots[0].data = g_cd;
        slots[0].len = g_cd_len;
        if (order == 0) {
            slots[1].type = 7;
            slots[1].data = ent7;
            slots[1].len = ent7_len;
            slots[2].type = 5;
            slots[2].data = g_ent_blob;
            slots[2].len = g_ent_blob_len;
        } else {
            slots[1].type = 5;
            slots[1].data = g_ent_blob;
            slots[1].len = g_ent_blob_len;
            slots[2].type = 7;
            slots[2].data = ent7;
            slots[2].len = ent7_len;
        }
        slots[3].type = 0x10000;
        slots[3].data = g_cms_blob;
        slots[3].len = g_cms_blob_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 4);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        CHECK(info.has_entitlements == 1);
        CHECK(info.entitlements_is_der == 0);
        CHECK(info.entitlements_len == (uint32_t)strlen(k_ent_xml));
        CHECK(memcmp(info.entitlements, k_ent_xml,
                     info.entitlements_len) == 0);
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        CHECK(csops_entitlements_plist(&info, &a) != NULL);
        CHECK(csops_has_entitlement(&info, "application-identifier",
                                    &a) == 1);
    }
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: no entitlements.                                        */
/* ------------------------------------------------------------------ */

static void scen_4_no_entitlements(void) {
    uint8_t sb[4096];
    test_slot slots[2];
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[4096];
    SCEN_BEGIN(4, "no entitlements");
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 0x10000;
    slots[1].data = g_cms_blob;
    slots[1].len = g_cms_blob_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 2);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    CHECK(info.has_code_directory == 1);
    CHECK(info.has_entitlements == 0);
    CHECK(info.entitlements == NULL);
    CHECK(info.entitlements_len == 0);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    CHECK(csops_entitlements_plist(&info, &a) == NULL);
    CHECK(csops_has_entitlement(&info, "application-identifier",
                                &a) == 0);
    CHECK(csops_status_flags(&info, &a) == (CS_VALID | CS_ADHOC));
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: no CodeDirectory.                                       */
/* ------------------------------------------------------------------ */

static void scen_5_no_cd(void) {
    uint8_t sb[4096];
    test_slot slots[2];
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[8192];
    uint8_t hash[32];
    uint32_t hash_len = 0;
    char team[32];
    SCEN_BEGIN(5, "no CodeDirectory");
    slots[0].type = 5;
    slots[0].data = g_ent_blob;
    slots[0].len = g_ent_blob_len;
    slots[1].type = 0x10000;
    slots[1].data = g_cms_blob;
    slots[1].len = g_cms_blob_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 2);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    CHECK(info.has_code_directory == 0);
    CHECK(info.has_entitlements == 1);
    CHECK(info.code_directory == NULL);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    CHECK(csops_entitlements_plist(&info, &a) != NULL);
    CHECK(csops_has_entitlement(&info, "application-identifier",
                                &a) == 1);
    CHECK(csops_status_flags(&info, &a) == 0);
    CHECK(csops_cdhash(&info, hash, sizeof(hash), &hash_len) ==
          XW_ERR_NOT_FOUND);
    CHECK(csops_team_id(&info, team, sizeof(team)) ==
          XW_ERR_NOT_FOUND);
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: truncated superblob.                                    */
/* ------------------------------------------------------------------ */

static void scen_6_truncated(void) {
    uint8_t sb[4096];
    csops_info info;
    SCEN_BEGIN(6, "truncated superblob");
    /* Length field larger than the buffer. */
    memcpy(sb, g_sb, g_sb_len);
    put_u32be(sb + 4, g_sb_len + 100);
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    /* Buffer cut mid-index (count 3 needs 24 index bytes). */
    CHECK(csops_parse(g_sb, 12 + 8, &info) < 0);
    /* Buffer cut inside the last blob. */
    CHECK(csops_parse(g_sb, g_sb_len - 10, &info) < 0);
    /* A blob length overrunning the superblob. */
    memcpy(sb, g_sb, g_sb_len);
    {
        uint32_t off5 = get_u32be(sb + 12 + 8 + 4);
        put_u32be(sb + off5 + 4, g_sb_len);
    }
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    /* CD length too small for its own fields. The CD length field
     * is the blob length (the index stores no separate length), so
     * an overrun against the superblob is the previous case; here
     * the length is shrunk below what the hash array needs. */
    memcpy(sb, g_sb, g_sb_len);
    {
        uint32_t off0 = get_u32be(sb + 12 + 4);
        put_u32be(sb + off0 + 4, 100);
    }
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    /* CD shorter than its header. */
    {
        uint8_t tiny_cd[20];
        uint8_t sb2[4096];
        test_slot slots[2];
        uint32_t sb2_len;
        put_u32be(tiny_cd, 0xfade0c02u);
        put_u32be(tiny_cd + 4, 20);
        memset(tiny_cd + 8, 0, 12);
        slots[0].type = 0;
        slots[0].data = tiny_cd;
        slots[0].len = sizeof(tiny_cd);
        slots[1].type = 0x10000;
        slots[1].data = g_cms_blob;
        slots[1].len = g_cms_blob_len;
        sb2_len = build_superblob(sb2, sizeof(sb2), slots, 2);
        CHECK(sb2_len != 0);
        CHECK(csops_parse(sb2, sb2_len, &info) < 0);
    }
    /* Empty and tiny buffers. */
    CHECK(csops_parse(g_sb, 0, &info) < 0);
    CHECK(csops_parse(g_sb, 11, &info) < 0);
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: bad magic.                                              */
/* ------------------------------------------------------------------ */

static void scen_7_bad_magic(void) {
    uint8_t sb[4096];
    csops_info info;
    SCEN_BEGIN(7, "bad magic");
    memcpy(sb, g_sb, g_sb_len);
    put_u32be(sb, 0xdeadbeefu);
    CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_MAGIC);
    memcpy(sb, g_sb, g_sb_len);
    {
        uint32_t off0 = get_u32be(sb + 12 + 4);
        put_u32be(sb + off0, 0xdeadbeefu);
    }
    CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_MAGIC);
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: lying count.                                            */
/* ------------------------------------------------------------------ */

static void scen_8_lying_count(void) {
    uint8_t sb[2048];
    csops_info info;
    SCEN_BEGIN(8, "lying count");
    /* Count so large the index runs past the superblob length. */
    memset(sb, 0, sizeof(sb));
    put_u32be(sb, 0xfade0cc0u);
    put_u32be(sb + 4, 100);
    put_u32be(sb + 8, 100);
    CHECK(csops_parse(sb, 100, &info) < 0);
    /* Count fits the length, but the index entries are zero, so the
     * first entry points into the header. */
    memset(sb, 0, sizeof(sb));
    put_u32be(sb, 0xfade0cc0u);
    put_u32be(sb + 4, 812);
    put_u32be(sb + 8, 100);
    CHECK(csops_parse(sb, 812, &info) < 0);
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: index offset out of bounds.                             */
/* ------------------------------------------------------------------ */

static void scen_9_offset_oob(void) {
    uint8_t sb[4096];
    csops_info info;
    SCEN_BEGIN(9, "index offset out of bounds");
    /* Offset past the superblob length. */
    memcpy(sb, g_sb, g_sb_len);
    put_u32be(sb + 12 + 8 + 4, g_sb_len + 50);
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    /* Offset inside the index array. */
    memcpy(sb, g_sb, g_sb_len);
    put_u32be(sb + 12 + 8 + 4, 16);
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    /* Offset inside the header. */
    memcpy(sb, g_sb, g_sb_len);
    put_u32be(sb + 12 + 8 + 4, 4);
    CHECK(csops_parse(sb, g_sb_len, &info) < 0);
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* Scenario 10: duplicate slots, first wins.                           */
/* ------------------------------------------------------------------ */

static void scen_10_duplicate(void) {
    static const char ent_b[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>"
        "<key>application-identifier</key>"
        "<string>BBBBBBBBBB.com.other.app</string>"
        "</dict></plist>\n";
    static const test_cd_params cd_b = {
        0x20500, 2, 32, 2, 7, 8192, 12, 0,
        "com.other.app", "BBBBBBBBBB"
    };
    uint8_t ent_b_blob[1024];
    uint8_t cd_b_buf[1024];
    uint8_t sb[8192];
    test_slot slots[4];
    uint32_t ent_b_len;
    uint32_t cd_b_len;
    uint32_t sb_len;
    csops_info info;
    char ident[64];
    SCEN_BEGIN(10, "duplicate slots, first wins");
    ent_b_len = build_blob(ent_b_blob, sizeof(ent_b_blob), 0xfade7171u,
                           (const uint8_t *)ent_b, (uint32_t)strlen(ent_b));
    CHECK(ent_b_len != 0);
    cd_b_len = build_cd(cd_b_buf, sizeof(cd_b_buf), &cd_b);
    CHECK(cd_b_len != 0);
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 0;
    slots[1].data = cd_b_buf;
    slots[1].len = cd_b_len;
    slots[2].type = 5;
    slots[2].data = g_ent_blob;
    slots[2].len = g_ent_blob_len;
    slots[3].type = 5;
    slots[3].data = ent_b_blob;
    slots[3].len = ent_b_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 4);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    CHECK(info.entitlements_len == (uint32_t)strlen(k_ent_xml));
    CHECK(memcmp(info.entitlements, k_ent_xml,
                 info.entitlements_len) == 0);
    CHECK(csops_identifier(&info, ident, sizeof(ident)) == XW_OK);
    CHECK(strcmp(ident, "com.example.app") == 0);
    SCEN_END(10);
}

/* ------------------------------------------------------------------ */
/* Scenario 11: entitlement lookup edges.                              */
/* ------------------------------------------------------------------ */

static const char k_ent_gta_false[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\"><dict>"
    "<key>application-identifier</key>"
    "<string>AAAAAAAAAA.com.example.app</string>"
    "<key>get-task-allow</key><false/>"
    "</dict></plist>\n";

static const char k_ent_rich[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- a comment -->\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>nested</key>\n"
    "  <dict>\n"
    "    <key>inner</key>\n"
    "    <string>a &amp; b &lt;tag&gt; &quot;q&quot; "
    "&apos;s&apos; &#65;&#x42;</string>\n"
    "  </dict>\n"
    "  <key>list</key>\n"
    "  <array>\n"
    "    <string>one</string>\n"
    "    <integer>-42</integer>\n"
    "    <integer>18446744073709551615</integer>\n"
    "    <real>3.25</real>\n"
    "    <real>1.5e3</real>\n"
    "    <true/>\n"
    "    <false/>\n"
    "    <data>SGVsbG8=</data>\n"
    "    <date>2026-11-12T10:00:00Z</date>\n"
    "  </array>\n"
    "  <key>empty-string</key>\n"
    "  <string/>\n"
    "  <key>empty-array</key>\n"
    "  <array/>\n"
    "  <key>empty-dict</key>\n"
    "  <dict/>\n"
    "  <key>big-neg</key>\n"
    "  <integer>-9223372036854775808</integer>\n"
    "</dict>\n"
    "</plist>\n";

static void check_rich_plist(void) {
    uint8_t ent[4096];
    uint8_t sb[8192];
    test_slot slots[2];
    uint32_t ent_len;
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[16384];
    bp_value *root;
    bp_value *nested;
    bp_value *inner;
    bp_value *list;
    bp_value *it;
    bp_value *v;
    ent_len = build_blob(ent, sizeof(ent), 0xfade7171u,
                         (const uint8_t *)k_ent_rich,
                         (uint32_t)strlen(k_ent_rich));
    CHECK(ent_len != 0);
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 5;
    slots[1].data = ent;
    slots[1].len = ent_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 2);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    root = csops_entitlements_plist(&info, &a);
    CHECK(root != NULL);
    CHECK(root != NULL && root->type == BP_DICT);
    nested = bp_dict_get(root, "nested");
    CHECK(nested != NULL && nested->type == BP_DICT);
    inner = bp_dict_get(nested, "inner");
    CHECK(inner != NULL && inner->type == BP_STRING);
    CHECK(inner != NULL &&
          strcmp((const char *)inner->s,
                 "a & b <tag> \"q\" 's' AB") == 0);
    list = bp_dict_get(root, "list");
    CHECK(list != NULL && list->type == BP_ARRAY);
    it = list != NULL ? list->child : NULL;
    CHECK(it != NULL && it->type == BP_STRING &&
          strcmp((const char *)it->s, "one") == 0);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_INT && it->i == -42);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_UINT &&
          it->u == 18446744073709551615ull);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_REAL && it->d == 3.25);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_REAL && it->d == 1500.0);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_BOOL && it->b == 1);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_BOOL && it->b == 0);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_DATA && it->len == 5);
    CHECK(it != NULL && it->len == 5 &&
          memcmp(it->s, "Hello", 5) == 0);
    it = it != NULL ? it->next : NULL;
    CHECK(it != NULL && it->type == BP_DATE &&
          it->i == K_DATE_2026_NS);
    it = it != NULL ? it->next : NULL;
    CHECK(it == NULL);
    v = bp_dict_get(root, "empty-string");
    CHECK(v != NULL && v->type == BP_STRING && v->len == 1 &&
          v->s[0] == 0);
    v = bp_dict_get(root, "empty-array");
    CHECK(v != NULL && v->type == BP_ARRAY && v->child == NULL);
    v = bp_dict_get(root, "empty-dict");
    CHECK(v != NULL && v->type == BP_DICT && v->child == NULL);
    v = bp_dict_get(root, "big-neg");
    CHECK(v != NULL && v->type == BP_INT &&
          v->i == (-9223372036854775807ll - 1));
}

static void scen_11_lookup_edges(void) {
    uint8_t ent[2048];
    uint8_t sb[8192];
    test_slot slots[2];
    uint32_t ent_len;
    uint32_t sb_len;
    csops_info info;
    bp_arena a;
    static uint8_t arena_buf[16384];
    bp_value *root;
    bp_value *v;
    SCEN_BEGIN(11, "entitlement lookup edges");
    /* get-task-allow false counts as not held. */
    ent_len = build_blob(ent, sizeof(ent), 0xfade7171u,
                         (const uint8_t *)k_ent_gta_false,
                         (uint32_t)strlen(k_ent_gta_false));
    CHECK(ent_len != 0);
    slots[0].type = 0;
    slots[0].data = g_cd;
    slots[0].len = g_cd_len;
    slots[1].type = 5;
    slots[1].data = ent;
    slots[1].len = ent_len;
    sb_len = build_superblob(sb, sizeof(sb), slots, 2);
    CHECK(sb_len != 0);
    CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
    bp_arena_init(&a, arena_buf, sizeof(arena_buf));
    CHECK(csops_has_entitlement(&info, "get-task-allow", &a) == 0);
    CHECK(csops_has_entitlement(&info, "application-identifier",
                                &a) == 1);
    CHECK(csops_status_flags(&info, &a) == (CS_VALID | CS_ADHOC));
    /* Malformed XML payload: lookup is a negative return. */
    check_plist_null(
        (const uint8_t *)"<plist><dict><key>foo</key>",
        (uint32_t)strlen("<plist><dict><key>foo</key>"));
    /* Binary-plist slot 5, built with bp_write. */
    {
        static uint8_t bp_arena_buf[4096];
        static uint8_t bp_buf[4096];
        bp_arena ba;
        bp_value *dict;
        bp_value *str;
        uint32_t bp_len = 0;
        int rc;
        bp_arena_init(&ba, bp_arena_buf, sizeof(bp_arena_buf));
        dict = bp_new(&ba, BP_DICT);
        CHECK(dict != NULL);
        rc = bp_dict_set(&ba, dict, "application-identifier",
                         bp_new_string(&ba, "AAAAAAAAAA.com.example.app"));
        CHECK(rc == XW_OK);
        str = bp_new_string(&ba, "production");
        CHECK(str != NULL);
        rc = bp_dict_set(&ba, dict, "aps-environment", str);
        CHECK(rc == XW_OK);
        rc = bp_dict_set(&ba, dict, "count", bp_new_int(&ba, 42));
        CHECK(rc == XW_OK);
        rc = bp_write(dict, bp_buf, sizeof(bp_buf), &bp_len);
        CHECK(rc == XW_OK);
        CHECK(bp_len > 8);
        CHECK(memcmp(bp_buf, "bplist00", 8) == 0);
        ent_len = build_blob(ent, sizeof(ent), 0xfade7171u, bp_buf,
                             bp_len);
        CHECK(ent_len != 0);
        slots[0].type = 0;
        slots[0].data = g_cd;
        slots[0].len = g_cd_len;
        slots[1].type = 5;
        slots[1].data = ent;
        slots[1].len = ent_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        root = csops_entitlements_plist(&info, &a);
        CHECK(root != NULL);
        v = bp_dict_get(root, "application-identifier");
        CHECK(v != NULL && v->type == BP_STRING &&
              strcmp((const char *)v->s,
                     "AAAAAAAAAA.com.example.app") == 0);
        v = bp_dict_get(root, "aps-environment");
        CHECK(v != NULL && v->type == BP_STRING &&
              strcmp((const char *)v->s, "production") == 0);
        v = bp_dict_get(root, "count");
        CHECK(v != NULL && v->type == BP_INT && v->i == 42);
        CHECK(csops_has_entitlement(&info, "aps-environment",
                                    &a) == 1);
    }
    /* Rich XML: every plist type, entities, comments, DOCTYPE. */
    check_rich_plist();
    SCEN_END(11);
}

/* ------------------------------------------------------------------ */
/* Scenario 12: malformed XML payloads.                                */
/* ------------------------------------------------------------------ */

static void scen_12_malformed_xml(void) {
    SCEN_BEGIN(12, "malformed XML payloads");
    /* Mismatched close tag. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><string>x</string></array></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><string>x</string></array></plist>"));
    /* Unknown entity. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><string>x &bogus; y</string>"
        "</dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><string>x &bogus; y</string>"
        "</dict></plist>"));
    /* Bad base64 characters. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><data>!!!</data></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><data>!!!</data></dict></plist>"));
    /* Base64 length not a multiple of four. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><data>SGVsbG8</data></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><data>SGVsbG8</data></dict></plist>"));
    /* Unclosed comment. */
    check_plist_null(
        (const uint8_t *)
        "<!-- foo <plist><dict></dict></plist>",
        (uint32_t)strlen("<!-- foo <plist><dict></dict></plist>"));
    /* Trailing junk after the root element. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict></dict></plist>junk",
        (uint32_t)strlen("<plist><dict></dict></plist>junk"));
    /* Non-dict root. */
    check_plist_null(
        (const uint8_t *)
        "<plist><array><string>x</string></array></plist>",
        (uint32_t)strlen(
        "<plist><array><string>x</string></array></plist>"));
    /* Integer overflow. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key>"
        "<integer>99999999999999999999</integer></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key>"
        "<integer>99999999999999999999</integer></dict></plist>"));
    /* Bad dates: month 13, February 30. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key>"
        "<date>2026-13-01T00:00:00Z</date></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key>"
        "<date>2026-13-01T00:00:00Z</date></dict></plist>"));
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key>"
        "<date>2026-02-30T00:00:00Z</date></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key>"
        "<date>2026-02-30T00:00:00Z</date></dict></plist>"));
    /* Empty integer element. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><integer/></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><integer/></dict></plist>"));
    /* Junk inside a boolean element. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><true>junk</true></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><true>junk</true></dict></plist>"));
    /* CDATA sections are not supported. */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key>"
        "<string><![CDATA[x]]></string></dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key>"
        "<string><![CDATA[x]]></string></dict></plist>"));
    /* Key with no following value. */
    check_plist_null(
        (const uint8_t *)"<plist><dict><key>a</key></dict></plist>",
        (uint32_t)strlen("<plist><dict><key>a</key></dict></plist>"));
    /* Raw 0xff byte in a string (invalid UTF-8). */
    check_plist_null(
        (const uint8_t *)
        "<plist><dict><key>a</key><string>x\xffy</string>"
        "</dict></plist>",
        (uint32_t)strlen(
        "<plist><dict><key>a</key><string>x\xffy</string>"
        "</dict></plist>"));
    /* Nesting 40 dicts deep, past BP_MAX_DEPTH. */
    {
        static char deep[4096];
        uint32_t o = 0;
        int d;
        o += (uint32_t)snprintf(deep + o, sizeof(deep) - o, "<plist>");
        for (d = 0; d < 40; d++)
            o += (uint32_t)snprintf(deep + o, sizeof(deep) - o,
                                    "<dict><key>k%d</key>", d);
        o += (uint32_t)snprintf(deep + o, sizeof(deep) - o,
                                "<string>x</string>");
        for (d = 0; d < 40; d++)
            o += (uint32_t)snprintf(deep + o, sizeof(deep) - o,
                                    "</dict>");
        o += (uint32_t)snprintf(deep + o, sizeof(deep) - o, "</plist>");
        CHECK(o < sizeof(deep));
        check_plist_null((const uint8_t *)deep, o);
    }
    /* A bare dict root without a plist wrapper is accepted. */
    {
        static const char bare[] =
            "<dict><key>a</key><string>x</string></dict>";
        uint8_t ent[1024];
        uint8_t sb[4096];
        test_slot slots[2];
        uint32_t ent_len;
        uint32_t sb_len;
        csops_info info;
        bp_arena a;
        static uint8_t arena_buf[4096];
        bp_value *root;
        bp_value *v;
        ent_len = build_blob(ent, sizeof(ent), 0xfade7171u,
                             (const uint8_t *)bare,
                             (uint32_t)strlen(bare));
        CHECK(ent_len != 0);
        slots[0].type = 0;
        slots[0].data = g_cd;
        slots[0].len = g_cd_len;
        slots[1].type = 5;
        slots[1].data = ent;
        slots[1].len = ent_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        bp_arena_init(&a, arena_buf, sizeof(arena_buf));
        root = csops_entitlements_plist(&info, &a);
        CHECK(root != NULL);
        v = bp_dict_get(root, "a");
        CHECK(v != NULL && v->type == BP_STRING &&
              strcmp((const char *)v->s, "x") == 0);
    }
    SCEN_END(12);
}

/* ------------------------------------------------------------------ */
/* Scenario 13: cdhash.                                                */
/* ------------------------------------------------------------------ */

static void scen_13_cdhash(void) {
    uint8_t digest[32];
    uint8_t out[32];
    uint32_t out_len = 0;
    csops_info info;
    SCEN_BEGIN(13, "cdhash");
    /* FIPS 180-4 vectors. */
    cs_sha256(NULL, 0, digest);
    CHECK(memcmp(digest, k_sha256_empty, 32) == 0);
    cs_sha256((const uint8_t *)"abc", 3, digest);
    CHECK(memcmp(digest, k_sha256_abc, 32) == 0);
    /* Longer input crossing several blocks: 200 bytes. The
     * expected value is hashlib.sha256 of the same bytes. */
    {
        static uint8_t big[200];
        uint32_t i;
        for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 3 + 1);
        cs_sha256(big, sizeof(big), digest);
        static const uint8_t expect[32] = {
            0x05, 0xb7, 0xd5, 0x19, 0xd2, 0x60, 0xb6, 0x1a,
            0x48, 0x83, 0x82, 0x74, 0x42, 0xe4, 0xed, 0xe6,
            0xb8, 0xe7, 0x0f, 0x60, 0x9e, 0xa9, 0x2a, 0x9c,
            0x9c, 0xc3, 0x06, 0x69, 0xc6, 0x52, 0xd5, 0x4a
        };
        CHECK(memcmp(digest, expect, 32) == 0);
    }
    /* SHA-1 CD: no SHA-1 in the tree, so cdhash is refused. */
    {
        static const test_cd_params cd_sha1 = {
            0x20500, 1, 20, 2, 7, 8192, 12, 0,
            "com.example.app", "AAAAAAAAAA"
        };
        uint8_t cd[1024];
        uint8_t sb[4096];
        test_slot slots[2];
        uint32_t cd_len;
        uint32_t sb_len;
        cd_len = build_cd(cd, sizeof(cd), &cd_sha1);
        CHECK(cd_len != 0);
        slots[0].type = 0;
        slots[0].data = cd;
        slots[0].len = cd_len;
        slots[1].type = 0x10000;
        slots[1].data = g_cms_blob;
        slots[1].len = g_cms_blob_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        CHECK(csops_cdhash(&info, out, sizeof(out), &out_len) ==
              XW_ERR_BAD_TYPE);
    }
    /* SHA-256-truncated CD: first hashSize bytes of the SHA-256. */
    {
        static const test_cd_params cd_trunc = {
            0x20500, 3, 20, 2, 7, 8192, 12, 0,
            "com.example.app", "AAAAAAAAAA"
        };
        uint8_t cd[1024];
        uint8_t sb[4096];
        test_slot slots[2];
        uint32_t cd_len;
        uint32_t sb_len;
        uint8_t full[32];
        cd_len = build_cd(cd, sizeof(cd), &cd_trunc);
        CHECK(cd_len != 0);
        slots[0].type = 0;
        slots[0].data = cd;
        slots[0].len = cd_len;
        slots[1].type = 0x10000;
        slots[1].data = g_cms_blob;
        slots[1].len = g_cms_blob_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        cs_sha256(cd, cd_len, full);
        CHECK(csops_cdhash(&info, out, sizeof(out), &out_len) ==
              XW_OK);
        CHECK(out_len == 20);
        CHECK(memcmp(out, full, 20) == 0);
    }
    /* Output buffer too small, NULL arguments. */
    CHECK(csops_parse(g_sb, g_sb_len, &info) == XW_OK);
    CHECK(csops_cdhash(&info, out, 16, &out_len) == XW_ERR_NOSPACE);
    CHECK(csops_cdhash(&info, NULL, sizeof(out), &out_len) ==
          XW_ERR_NOSPACE);
    CHECK(csops_cdhash(&info, out, sizeof(out), NULL) ==
          XW_ERR_NOSPACE);
    CHECK(csops_cdhash(NULL, out, sizeof(out), &out_len) ==
          XW_ERR_NOT_FOUND);
    SCEN_END(13);
}

/* ------------------------------------------------------------------ */
/* Scenario 14: additive API edges.                                    */
/* ------------------------------------------------------------------ */

static void scen_14_api_edges(void) {
    csops_info info;
    bp_arena a;
    char small[4];
    char team[32];
    char ident[64];
    SCEN_BEGIN(14, "additive API edges");
    CHECK(csops_parse(NULL, 10, &info) < 0);
    CHECK(csops_parse(g_sb, g_sb_len, NULL) < 0);
    CHECK(csops_parse(g_sb, 0, &info) < 0);
    CHECK(csops_has_entitlement(NULL, "x", &a) < 0);
    CHECK(csops_parse(g_sb, g_sb_len, &info) == XW_OK);
    CHECK(csops_has_entitlement(&info, NULL, &a) < 0);
    CHECK(csops_status_flags(NULL, NULL) == 0);
    CHECK(csops_status_flags(&info, NULL) == (CS_VALID | CS_ADHOC));
    CHECK(csops_team_id(&info, small, sizeof(small)) ==
          XW_ERR_NOSPACE);
    CHECK(csops_identifier(&info, small, sizeof(small)) ==
          XW_ERR_NOSPACE);
    /* Version 0x20100 CD: no teamOffset field. */
    {
        static const test_cd_params cd_old = {
            0x20100, 2, 32, 2, 7, 8192, 12, 0,
            "com.example.app", NULL
        };
        uint8_t cd[1024];
        uint8_t sb[4096];
        test_slot slots[2];
        uint32_t cd_len;
        uint32_t sb_len;
        uint8_t hash[32];
        uint32_t hash_len = 0;
        cd_len = build_cd(cd, sizeof(cd), &cd_old);
        CHECK(cd_len != 0);
        slots[0].type = 0;
        slots[0].data = cd;
        slots[0].len = cd_len;
        slots[1].type = 0x10000;
        slots[1].data = g_cms_blob;
        slots[1].len = g_cms_blob_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_OK);
        CHECK(csops_team_id(&info, team, sizeof(team)) ==
              XW_ERR_NOT_FOUND);
        CHECK(csops_identifier(&info, ident, sizeof(ident)) == XW_OK);
        CHECK(strcmp(ident, "com.example.app") == 0);
        CHECK(csops_cdhash(&info, hash, sizeof(hash), &hash_len) ==
              XW_OK);
        CHECK(hash_len == 32);
    }
    /* Unknown CD version, unknown hash type, offsets out of range,
     * nCodeSlots inconsistent with codeLimit, pageSize out of
     * range, hashSize zero. Each is patched into a copy of the
     * happy-path superblob. */
    {
        uint8_t sb[4096];
        uint32_t off0;
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        put_u32be(sb + off0 + 8, 0x20600u);
        CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_VERSION);
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        sb[off0 + 37] = 9;
        CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_TYPE);
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        put_u32be(sb + off0 + 16, g_cd_len + 100);
        CHECK(csops_parse(sb, g_sb_len, &info) < 0);
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        put_u32be(sb + off0 + 20, g_cd_len + 100);
        CHECK(csops_parse(sb, g_sb_len, &info) < 0);
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        sb[off0 + 36] = 0;
        CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_TYPE);
        memcpy(sb, g_sb, g_sb_len);
        off0 = get_u32be(sb + 12 + 4);
        sb[off0 + 39] = 25;
        CHECK(csops_parse(sb, g_sb_len, &info) == XW_ERR_BAD_TYPE);
    }
    /* nCodeSlots inconsistent with codeLimit and pageSize: a CD
     * built with 3 code slots but a codeLimit of 2 pages. */
    {
        static const test_cd_params cd_bad_slots = {
            0x20500, 2, 32, 3, 7, 8192, 12, 0,
            "com.example.app", "AAAAAAAAAA"
        };
        uint8_t cd[1024];
        uint8_t sb[4096];
        test_slot slots[2];
        uint32_t cd_len;
        uint32_t sb_len;
        cd_len = build_cd(cd, sizeof(cd), &cd_bad_slots);
        CHECK(cd_len != 0);
        slots[0].type = 0;
        slots[0].data = cd;
        slots[0].len = cd_len;
        slots[1].type = 0x10000;
        slots[1].data = g_cms_blob;
        slots[1].len = g_cms_blob_len;
        sb_len = build_superblob(sb, sizeof(sb), slots, 2);
        CHECK(sb_len != 0);
        CHECK(csops_parse(sb, sb_len, &info) == XW_ERR_BAD_TYPE);
    }
    SCEN_END(14);
}

/* ------------------------------------------------------------------ */
/* Scenario 15: fuzz.                                                  */
/* ------------------------------------------------------------------ */

static void scen_15_fuzz(void) {
    static uint8_t arena_buf[16384];
    uint8_t scratch[4096];
    int violations = 0;
    int i = 0;
    SCEN_BEGIN(15, "fuzz csops_parse (10000) + XML reader (5000)");
    CHECK(g_sb_len > 40u);
    CHECK(g_sb_len <= sizeof(scratch));
    for (i = 0; i < 10000; i++) {
        uint32_t len = g_sb_len;
        int nmut = 0, m = 0;
        uint32_t off = 0;
        csops_info info;
        int rc;
        memcpy(scratch, g_sb, g_sb_len);
        nmut = 1 + (int)(rng_next() % 8u);
        for (m = 0; m < nmut; m++) {
            off = rng_next() % g_sb_len;
            scratch[off] = (uint8_t)rng_next();
        }
        if ((rng_next() % 8u) == 0) {
            len = rng_next() % (g_sb_len + 1);
        }
        rc = csops_parse(scratch, len, &info);
        if (rc > 0) {
            if (violations < 5) {
                printf("\n  fuzz iter %d: positive return %d\n", i, rc);
            }
            violations++;
            continue;
        }
        if (rc == XW_OK) {
            if (info.has_code_directory &&
                (info.code_directory < scratch ||
                 info.code_directory + info.code_directory_len >
                     scratch + len)) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: CD pointer out of "
                           "bounds\n", i);
                }
                violations++;
                continue;
            }
            if (info.has_entitlements &&
                (info.entitlements < scratch ||
                 info.entitlements + info.entitlements_len >
                     scratch + len)) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: entitlements pointer "
                           "out of bounds\n", i);
                }
                violations++;
                continue;
            }
            {
                bp_arena a;
                bp_value *root;
                uint32_t count = 0;
                bp_arena_init(&a, arena_buf, sizeof(arena_buf));
                root = csops_entitlements_plist(&info, &a);
                if (root != NULL && walk_tree(root, 0, &count) != 0) {
                    if (violations < 5) {
                        printf("\n  fuzz iter %d: parsed tree does "
                               "not walk\n", i);
                    }
                    violations++;
                }
            }
        }
    }
    printf("  parse fuzz: 10000 iterations, %d violations\n",
           violations);
    CHECK(violations == 0);
    {
        uint32_t rich_len = (uint32_t)strlen(k_ent_rich);
        int xml_violations = 0;
        CHECK(rich_len < sizeof(scratch));
        for (i = 0; i < 5000; i++) {
            uint32_t len = rich_len;
            int nmut = 0, m = 0;
            uint32_t off = 0;
            csops_info fake;
            bp_arena a;
            bp_value *root;
            uint32_t count = 0;
            memcpy(scratch, k_ent_rich, rich_len);
            nmut = 1 + (int)(rng_next() % 8u);
            for (m = 0; m < nmut; m++) {
                off = rng_next() % rich_len;
                scratch[off] = (uint8_t)rng_next();
            }
            if ((rng_next() % 8u) == 0) {
                len = rng_next() % (rich_len + 1);
            }
            memset(&fake, 0, sizeof(fake));
            fake.has_entitlements = 1;
            fake.entitlements = scratch;
            fake.entitlements_len = len;
            fake.entitlements_is_der = 0;
            bp_arena_init(&a, arena_buf, sizeof(arena_buf));
            root = csops_entitlements_plist(&fake, &a);
            if (root != NULL &&
                (root->type != BP_DICT ||
                 walk_tree(root, 0, &count) != 0)) {
                if (xml_violations < 5) {
                    printf("\n  xml fuzz iter %d: bad tree\n", i);
                }
                xml_violations++;
            }
        }
        printf("  xml fuzz: 5000 iterations, %d violations\n",
               xml_violations);
        CHECK(xml_violations == 0);
    }
    SCEN_END(15);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("csops_test: host test\n");
    if (build_fixtures() != 0) {
        printf("fixture build failed\n");
        return 1;
    }
    printf("fixtures: cd %u bytes, superblob %u bytes\n",
           g_cd_len, g_sb_len);
    scen_1_happy();
    scen_2_der_only();
    scen_3_both_slots();
    scen_4_no_entitlements();
    scen_5_no_cd();
    scen_6_truncated();
    scen_7_bad_magic();
    scen_8_lying_count();
    scen_9_offset_oob();
    scen_10_duplicate();
    scen_11_lookup_edges();
    scen_12_malformed_xml();
    scen_13_cdhash();
    scen_14_api_edges();
    scen_15_fuzz();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
