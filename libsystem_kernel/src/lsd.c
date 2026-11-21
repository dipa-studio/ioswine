/*
 * lsd.c: fake LaunchServices daemon for ioswine.
 *
 * Serves the running app own bundle record over the three
 * registered Mach services (xpc, mapdb, modifydb) so that
 * UIApplicationMain and the first UIKit/Foundation calls that
 * resolve the app own identity do not block. The record is built
 * fresh on every reply from the static table in lsd_names.h, the
 * container paths from container.h, and the entitlements from
 * csops.h when csops info has been supplied with
 * lsd_set_csops_info. The two containers must have different UUIDs,
 * and this service never fabricates a path: when a container path
 * is unset the record omits the URL fields instead.
 *
 * The request keys, operation codes and reply shape are PLACEHOLDERS
 *: no published trace
 * gives the real LS wire format, and the raw-dict vs NSXPC question
 * is unresolved. The handler tries both parsings: a plain XPC dict
 * with an operation key first, then a raw-byte scan of the body for
 * the own bundle id (the NSXPC fallback). Every incoming message is
 * logged verbatim with bsxpc_log_dict when tracing is enabled, so
 * the first device trace corrects the table in lsd_names.h with a
 * small diff. The host-test-only shape is never presented as the
 * real protocol.
 *
 * The other Mach names from the macOS plist (open, openurl, plugin,
 * icons, advertisingidentifiers, diagnostics, dissemination) are
 * deliberately NOT registered: a lookup for one of them fails fast
 * at the launchd shim instead of hanging, and a request for one of
 * those functions arriving on the xpc service is logged verbatim and
 * answered with an error, never dropped. This service cannot observe
 * a bootstrap_look_up call directly; the trace shows whether
 * anything asks for those functions by whether the client asks for
 * them on the xpc service.
 *
 * Handlers are pure dict transforms: no ipc_lock takers, no
 * blocking, no allocation. The entitlements plist is parsed into a
 * static arena buffer; that is safe because handlers run serialized
 * under ipc_lock (see svchost.c). Replies are built with the
 * xpcwire builder and must fit SVCHOST_MAX_REPLY_BODY whatever
 * buffer the caller passed (the ipc.c dispatch branch only provides
 * SVCHOST_MAX_REPLY bytes total); the builders below clamp the cap
 * to it. When the record with entitlements does not fit, the reply
 * is rebuilt without them, and only a reply that still does not
 * fit returns SVCHOST_ERR_NO_MEMORY.
 *
 * Reply budget trim (2026-11-12, measured): the full record
 * does NOT fit in 1000 bytes of body (the full record measured
 * 1204 bytes with entitlements). The record sent here keeps
 * only what the running app needs at launch (bundle id,
 * the three URLs, display name, the two version strings,
 * installed/launchable, URL schemes, teamID and entitlements when
 * they fit) and drops the duplicates and SpringBoard-side flags
 * (CFBundleName, CFBundleExecutable, MinimumOSVersion,
 * applicationType, isPlaceholder, LSApplicationHasRegistered,
 * LSApplicationReadyToBeFrontableKey, UIDeviceFamily). A dropped
 * key reads as nil/false on the client, which is tolerable for
 * those fields at launch; the kept record measures 924 bytes of
 * body with entitlements and 724 without (host test scenario 11),
 * inside the cap.
 *
 * Style: freestanding C, same as svchost.c / fakeboard.c. No libc.
 * All wire strings live in lsd_names.h; this file carries no string
 * literals in code. Dispatch is if/switch chains, never a
 * function-pointer table in static data.
 *
 * No em dashes are used in this file.
 */
#include "lsd.h"
#include "lsd_names.h"
#include "bsxpc.h"
#include "bplist.h"
#include "container.h"

/* Scratch sizes. */
#define LSD_URL_BUF 512        /* one file:// container URL */
#define LSD_BP_ARENA_BUF 4096  /* entitlements plist arena */
#define LSD_BP_MAX_DEPTH 8     /* entitlements bp to xw nesting cap */
#define LSD_TEAM_BUF 128       /* team identifier copy */
#define LSD_SCHEME_BUF 128     /* URL scheme copy */

/* ------------------------------------------------------------------ */
/* Freestanding helpers.                                               */
/* ------------------------------------------------------------------ */

static size_t lsd_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int lsd_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* Copy src into dst (NUL terminated). 0 or -1 when it does not fit. */
static int lsd_copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0 || !src) return -1;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    if (src[i]) return -1;
    dst[i] = 0;
    return 0;
}

/* Append src to the NUL terminated dst. 0 or -1 when it does not fit. */
static int lsd_cat_str(char *dst, size_t cap, const char *src) {
    size_t d = lsd_strlen(dst);
    size_t i = 0;
    if (d >= cap) return -1;
    while (src[i] && d + i + 1 < cap) {
        dst[d + i] = src[i];
        i++;
    }
    if (src[i]) return -1;
    dst[d + i] = 0;
    return 0;
}

/* Bounded substring search. Returns a pointer into hay, or NULL. */
static const void *lsd_memmem(const void *hay, size_t haylen,
                              const void *needle, size_t nlen) {
    const uint8_t *h = (const uint8_t *)hay;
    const uint8_t *n = (const uint8_t *)needle;
    size_t i, j;
    if (!hay || !needle || nlen == 0 || haylen < nlen) return NULL;
    for (i = 0; i + nlen <= haylen; i++) {
        for (j = 0; j < nlen; j++) {
            if (h[i + j] != n[j]) break;
        }
        if (j == nlen) return h + i;
    }
    return NULL;
}

/* Build out = file:// + root + suffix. 0 or -1 when it does not fit. */
static int lsd_build_url(char *out, size_t cap, const char *root,
                         const char *suffix) {
    out[0] = 0;
    if (lsd_cat_str(out, cap, LSD_URL_FILE_PREFIX) != 0) return -1;
    if (lsd_cat_str(out, cap, root) != 0) return -1;
    if (lsd_cat_str(out, cap, suffix) != 0) return -1;
    return 0;
}

/* Copy the scheme part of url (chars before the first colon) into
 * out; when there is no colon, copy the whole string. 0 or -1 when
 * it does not fit. */
static int lsd_scheme_of(const char *url, char *out, size_t cap) {
    size_t i = 0;
    if (!url || cap == 0) return -1;
    while (url[i] && url[i] != ':' && i + 1 < cap) {
        out[i] = url[i];
        i++;
    }
    if (url[i] && url[i] != ':') return -1;
    out[i] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Static state.                                                       */
/* ------------------------------------------------------------------ */

static int g_lsd_inited = 0;
static mach_port_name_t g_lsd_ports[LSD_NUM_SERVICES];
static int g_lsd_ctx[LSD_NUM_SERVICES] = { 0, 1, 2 };
static const csops_info *g_lsd_csops = NULL;
/* Entitlements plist arena (see the file header: handlers run
 * serialized under ipc_lock, so one static buffer is safe). */
static uint8_t g_lsd_bp_arena_buf[LSD_BP_ARENA_BUF];

void lsd_set_csops_info(const csops_info *info) {
    g_lsd_csops = info;
}

/* ------------------------------------------------------------------ */
/* Reply builders.                                                     */
/* ------------------------------------------------------------------ */

/* The reply body must fit SVCHOST_MAX_REPLY_BODY whatever buffer the
 * caller passed: the ipc.c dispatch branch only provides
 * SVCHOST_MAX_REPLY bytes total, and a bigger caller buffer must
 * not let a reply grow past it. */
static uint32_t lsd_reply_cap(const xw_builder *b) {
    if (b->cap > SVCHOST_MAX_REPLY_BODY) return SVCHOST_MAX_REPLY_BODY;
    return b->cap;
}

/* Build the minimal error dict body. Returns 0 and sets *out_len,
 * or -1 when it does not fit. */
static int lsd_build_error_body(uint8_t *body, size_t cap, int64_t code,
                                uint32_t *out_len) {
    xw_builder b;
    if (xw_builder_init(&b, body, (uint32_t)cap) != XW_OK) return -1;
    if (xw_dict_begin(&b) != XW_OK) return -1;
    if (xw_dict_add_int64(&b, LSD_KEY_ERROR, code) != XW_OK) return -1;
    if (xw_dict_end(&b) != XW_OK) return -1;
    *out_len = (uint32_t)xw_builder_len(&b);
    return 0;
}

/* Parse the entitlements plist from the csops info into the static
 * arena. NULL when no info is set, the app has no entitlements, or
 * the plist does not parse (DER-only blobs included). The returned
 * tree points into the static arena and is only valid until the
 * next call. */
static bp_value *lsd_entitlements_plist(void) {
    bp_arena a;
    if (!g_lsd_csops || !g_lsd_csops->has_entitlements) return NULL;
    bp_arena_init(&a, g_lsd_bp_arena_buf, sizeof(g_lsd_bp_arena_buf));
    return csops_entitlements_plist(g_lsd_csops, &a);
}

/* Append one bp_value as a dictionary value under key. */
static int lsd_bp_add_keyed(xw_builder *b, const char *key,
                            const bp_value *v, int depth);

/* Append one bp_value as an array element. */
static int lsd_bp_add_elem(xw_builder *b, const bp_value *v, int depth) {
    int r = XW_OK;
    const bp_value *child;
    if (!v) return XW_ERR_BAD_TYPE;
    if (depth > LSD_BP_MAX_DEPTH) return XW_ERR_OVERFLOW;
    if (v->type == BP_NULL) return xw_add_null(b);
    if (v->type == BP_BOOL) return xw_add_bool(b, v->b);
    if (v->type == BP_INT) return xw_add_int64(b, v->i);
    if (v->type == BP_UINT) return xw_add_uint64(b, v->u);
    if (v->type == BP_REAL) return xw_add_double(b, v->d);
    if (v->type == BP_STRING) {
        if (!v->s) return XW_ERR_BAD_TYPE;
        return xw_add_string(b, (const char *)v->s);
    }
    if (v->type == BP_DATA) {
        if (!v->s && v->len != 0) return XW_ERR_BAD_TYPE;
        return xw_add_data(b, v->s, v->len);
    }
    if (v->type == BP_DATE) return xw_add_date(b, v->i);
    if (v->type == BP_ARRAY) {
        if (xw_array_begin(b) != XW_OK) return XW_ERR_NOSPACE;
        for (child = v->child; child && r == XW_OK; child = child->next)
            r = lsd_bp_add_elem(b, child, depth + 1);
        if (r == XW_OK) r = xw_array_end(b);
        return r;
    }
    if (v->type == BP_DICT) {
        if (xw_dict_begin(b) != XW_OK) return XW_ERR_NOSPACE;
        for (child = v->child; child && r == XW_OK; child = child->next) {
            if (!child->key) {
                r = XW_ERR_BAD_TYPE;
                break;
            }
            r = lsd_bp_add_keyed(b, child->key, child, depth + 1);
        }
        if (r == XW_OK) r = xw_dict_end(b);
        return r;
    }
    return XW_ERR_BAD_TYPE;
}

static int lsd_bp_add_keyed(xw_builder *b, const char *key,
                            const bp_value *v, int depth) {
    int r = XW_OK;
    const bp_value *child;
    if (!key || !v) return XW_ERR_BAD_TYPE;
    if (depth > LSD_BP_MAX_DEPTH) return XW_ERR_OVERFLOW;
    if (v->type == BP_NULL) return xw_dict_add_null(b, key);
    if (v->type == BP_BOOL) return xw_dict_add_bool(b, key, v->b);
    if (v->type == BP_INT) return xw_dict_add_int64(b, key, v->i);
    if (v->type == BP_UINT) return xw_dict_add_uint64(b, key, v->u);
    if (v->type == BP_REAL) return xw_dict_add_double(b, key, v->d);
    if (v->type == BP_STRING) {
        if (!v->s) return XW_ERR_BAD_TYPE;
        return xw_dict_add_string(b, key, (const char *)v->s);
    }
    if (v->type == BP_DATA) {
        if (!v->s && v->len != 0) return XW_ERR_BAD_TYPE;
        return xw_dict_add_data(b, key, v->s, v->len);
    }
    if (v->type == BP_DATE) return xw_dict_add_date(b, key, v->i);
    if (v->type == BP_ARRAY) {
        if (xw_dict_add_array_begin(b, key) != XW_OK)
            return XW_ERR_NOSPACE;
        for (child = v->child; child && r == XW_OK; child = child->next)
            r = lsd_bp_add_elem(b, child, depth + 1);
        if (r == XW_OK) r = xw_array_end(b);
        return r;
    }
    if (v->type == BP_DICT) {
        if (xw_dict_add_dict_begin(b, key) != XW_OK)
            return XW_ERR_NOSPACE;
        for (child = v->child; child && r == XW_OK; child = child->next) {
            if (!child->key) {
                r = XW_ERR_BAD_TYPE;
                break;
            }
            r = lsd_bp_add_keyed(b, child->key, child, depth + 1);
        }
        if (r == XW_OK) r = xw_dict_end(b);
        return r;
    }
    return XW_ERR_BAD_TYPE;
}

/* Append the record fields (without the leading error key; the
 * caller adds it) to the innermost open dictionary. with_entitlements
 * adds teamID and the nested entitlements dict when csops info is
 * available. Returns XW_OK or a negative XW_ERR_*. URL fields are
 * omitted when their container path is unset rather than fabricated.
 * The entitlements tree is parsed once per call. */
static int lsd_add_record_fields(xw_builder *b, int with_entitlements) {
    int r = XW_OK;
    char urlbuf[LSD_URL_BUF];
    char bundle_root[LSD_URL_BUF];
    const char *home = NULL;
    bp_value *ent = NULL;
    char team[LSD_TEAM_BUF];
    int have_team = 0;

    if (with_entitlements) {
        ent = lsd_entitlements_plist();
        if (ent) {
            bp_value *t = bp_dict_get(ent, LSD_ENT_TEAM_ID);
            if (t && t->type == BP_STRING && t->s) {
                if (lsd_copy_str(team, sizeof(team),
                                 (const char *)t->s) == 0)
                    have_team = 1;
            }
        }
    }

    if (r == XW_OK)
        r = xw_dict_add_string(b, LSD_KEY_BUNDLE_ID, LSD_APP_BUNDLE_ID);

    /* Bundle URLs from container_bundle(). */
    if (r == XW_OK &&
        container_bundle(bundle_root, sizeof(bundle_root)) == 0 &&
        bundle_root[0] != 0) {
        if (lsd_build_url(urlbuf, sizeof(urlbuf), bundle_root,
                          LSD_URL_APP_DIR_SUFFIX) == 0)
            r = xw_dict_add_string(b, LSD_REC_BUNDLE_URL, urlbuf);
        if (r == XW_OK &&
            lsd_build_url(urlbuf, sizeof(urlbuf), bundle_root,
                          LSD_URL_SLASH) == 0)
            r = xw_dict_add_string(b, LSD_REC_BUNDLE_CONTAINER_URL,
                                   urlbuf);
    }

    /* Data container URL from container_home(). */
    if (r == XW_OK) {
        home = container_home();
        if (home && home[0] != 0 &&
            lsd_build_url(urlbuf, sizeof(urlbuf), home,
                          LSD_URL_SLASH) == 0)
            r = xw_dict_add_string(b, LSD_REC_DATA_CONTAINER_URL, urlbuf);
    }

    if (r == XW_OK)
        r = xw_dict_add_string(b, LSD_REC_LOCALIZED_NAME, LSD_APP_NAME);
    if (r == XW_OK)
        r = xw_dict_add_string(b, LSD_REC_CF_BUNDLE_SHORT_VERSION,
                               LSD_APP_SHORT_VERSION);
    if (r == XW_OK)
        r = xw_dict_add_string(b, LSD_REC_CF_BUNDLE_VERSION,
                               LSD_APP_VERSION);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, LSD_REC_IS_INSTALLED, 1);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, LSD_REC_IS_LAUNCHABLE, 1);

    /* URL types: one entry claiming the aidoku scheme. */
    if (r == XW_OK) r = xw_dict_add_array_begin(b, LSD_REC_URL_TYPES);
    if (r == XW_OK) r = xw_dict_begin(b);
    if (r == XW_OK)
        r = xw_dict_add_string(b, LSD_REC_URL_NAME, LSD_APP_NAME);
    if (r == XW_OK) r = xw_dict_add_array_begin(b, LSD_REC_URL_SCHEMES);
    if (r == XW_OK) r = xw_add_string(b, LSD_APP_URL_SCHEME);
    if (r == XW_OK) r = xw_array_end(b);
    if (r == XW_OK) r = xw_dict_end(b);
    if (r == XW_OK) r = xw_array_end(b);

    if (r == XW_OK && have_team)
        r = xw_dict_add_string(b, LSD_KEY_TEAM_ID, team);

    if (r == XW_OK && ent) {
        const bp_value *child;
        if (r == XW_OK) r = xw_dict_add_dict_begin(b, LSD_KEY_ENTITLEMENTS);
        for (child = ent->child; child && r == XW_OK; child = child->next) {
            if (!child->key) {
                r = XW_ERR_BAD_TYPE;
                break;
            }
            r = lsd_bp_add_keyed(b, child->key, child, 0);
        }
        if (r == XW_OK) r = xw_dict_end(b);
    }
    return r;
}

/* Build a whole reply dict for op (LSD_OP_RECORD or
 * LSD_OP_ALL_APPLICATIONS) with or without entitlements. The
 * builder is re-initialized on the same buffer, so a retry after
 * overflow starts clean. Returns XW_OK or a negative XW_ERR_*. */
static int lsd_build_reply(xw_builder *reply, int op, int with_ent) {
    int r = XW_OK;
    if (xw_builder_init(reply, reply->buf, lsd_reply_cap(reply)) != XW_OK)
        return XW_ERR_NOSPACE;
    if (xw_dict_begin(reply) != XW_OK) return XW_ERR_NOSPACE;
    if (r == XW_OK) r = xw_dict_add_int64(reply, LSD_KEY_ERROR, 0);
    if (op == LSD_OP_ALL_APPLICATIONS) {
        if (r == XW_OK) r = xw_dict_add_array_begin(reply, LSD_KEY_RECORDS);
        if (r == XW_OK) r = xw_dict_begin(reply);
        if (r == XW_OK) r = lsd_add_record_fields(reply, with_ent);
        if (r == XW_OK) r = xw_dict_end(reply);
        if (r == XW_OK) r = xw_array_end(reply);
    } else {
        if (r == XW_OK) r = lsd_add_record_fields(reply, with_ent);
    }
    if (r == XW_OK) r = xw_dict_end(reply);
    return r;
}

/* Build a record or all-applications reply, retrying once without
 * entitlements when the first attempt does not fit. Returns 0 or a
 * SVCHOST_ERR_* code for the handler interface. */
static int lsd_reply_with_retry(xw_builder *reply, int op) {
    int r = lsd_build_reply(reply, op, 1);
    if (r == XW_ERR_NOSPACE) r = lsd_build_reply(reply, op, 0);
    if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* Reply with only an error key. Returns 0 or SVCHOST_ERR_NO_MEMORY. */
static int lsd_reply_error_only(xw_builder *reply, int64_t code) {
    int r;
    if (xw_builder_init(reply, reply->buf, lsd_reply_cap(reply)) != XW_OK)
        return SVCHOST_ERR_NO_MEMORY;
    r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_int64(reply, LSD_KEY_ERROR, code);
    if (r == XW_OK) r = xw_dict_end(reply);
    if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* Reply with an empty records list. Returns 0 or
 * SVCHOST_ERR_NO_MEMORY. */
static int lsd_reply_empty_list(xw_builder *reply) {
    int r;
    if (xw_builder_init(reply, reply->buf, lsd_reply_cap(reply)) != XW_OK)
        return SVCHOST_ERR_NO_MEMORY;
    r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_int64(reply, LSD_KEY_ERROR, 0);
    if (r == XW_OK) r = xw_dict_add_array_begin(reply, LSD_KEY_RECORDS);
    if (r == XW_OK) r = xw_array_end(reply);
    if (r == XW_OK) r = xw_dict_end(reply);
    if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* Reply with a boolean under key. Returns 0 or
 * SVCHOST_ERR_NO_MEMORY. */
static int lsd_reply_bool(xw_builder *reply, const char *key, int v) {
    int r;
    if (xw_builder_init(reply, reply->buf, lsd_reply_cap(reply)) != XW_OK)
        return SVCHOST_ERR_NO_MEMORY;
    r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_int64(reply, LSD_KEY_ERROR, 0);
    if (r == XW_OK) r = xw_dict_add_bool(reply, key, v);
    if (r == XW_OK) r = xw_dict_end(reply);
    if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Operations.                                                         */
/* ------------------------------------------------------------------ */

/* Own-bundle record lookup by bundleIdentifier. */
static int lsd_op_record(xw_reader *msg, xw_builder *reply) {
    xw_value v;
    const char *bid = NULL;
    uint32_t blen = 0;
    if (xw_dict_find(msg, LSD_KEY_BUNDLE_ID, &v) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_as_string(&v, &bid, &blen) != XW_OK || bid == NULL)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (lsd_strcmp(bid, LSD_APP_BUNDLE_ID) != 0)
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return lsd_reply_with_retry(reply, LSD_OP_RECORD);
}

/* URL-scheme handler lookup. Accepts a bare scheme or a full URL;
 * the running app handles its own scheme and nothing else. */
static int lsd_op_scheme(xw_reader *msg, xw_builder *reply) {
    xw_value v;
    const char *s = NULL;
    uint32_t slen = 0;
    char scheme[LSD_SCHEME_BUF];
    if (xw_dict_find(msg, LSD_KEY_SCHEME, &v) == XW_OK) {
        if (xw_as_string(&v, &s, &slen) != XW_OK || s == NULL)
            return SVCHOST_ERR_BAD_MESSAGE;
        if (lsd_scheme_of(s, scheme, sizeof(scheme)) != 0)
            return SVCHOST_ERR_BAD_MESSAGE;
    } else if (xw_dict_find(msg, LSD_KEY_URL, &v) == XW_OK) {
        if (xw_as_string(&v, &s, &slen) != XW_OK || s == NULL)
            return SVCHOST_ERR_BAD_MESSAGE;
        if (lsd_scheme_of(s, scheme, sizeof(scheme)) != 0)
            return SVCHOST_ERR_BAD_MESSAGE;
    } else {
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    if (lsd_strcmp(scheme, LSD_APP_URL_SCHEME) != 0)
        return LSD_ERR_NO_HANDLER;
    return lsd_reply_with_retry(reply, LSD_OP_RECORD);
}

/* NSXPC fallback (Hypothesis B): the body is a valid XPC dict but
 * carries no operation key, so it may be a serialized NSXPC
 * invocation. Scan the raw body bytes for the own bundle id; when
 * found, answer the own record (best effort). The verbatim trace
 * above already logged the whole dict for the trace correction. */
static int lsd_handle_nsxpc(xw_reader *msg, xw_builder *reply) {
    if (msg->buf && msg->len > 0 &&
        lsd_memmem(msg->buf, msg->len, LSD_APP_BUNDLE_ID,
                   lsd_strlen(LSD_APP_BUNDLE_ID)) != NULL) {
        return lsd_reply_with_retry(reply, LSD_OP_RECORD);
    }
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

/* True when the root dictionary has no entries. */
static int lsd_dict_is_empty(xw_reader *msg) {
    xw_dict_iter it;
    const char *key = NULL;
    xw_value v;
    int rc;
    if (xw_dict_iter_init(msg, &it) != XW_OK) return 1;
    rc = xw_dict_next(&it, &key, &v);
    return rc == 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch core.                                                      */
/* ------------------------------------------------------------------ */

static int lsd_handle_request(xw_reader *msg, xw_builder *reply,
                              int svc_idx) {
    xw_value root, v;
    int64_t op = 0;
    (void)svc_idx; /* all three services answer the same queries */
    if (bsxpc_trace_enabled()) {
        (void)bsxpc_log_dict(msg, LSD_TRACE_PREFIX);
    }
    if (xw_reader_root(msg, &root) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (root.type != XW_TYPE_DICTIONARY)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_find(msg, LSD_KEY_OPERATION, &v) != XW_OK) {
        /* No operation key: an empty dict is malformed; anything
         * else may be an NSXPC invocation. */
        if (lsd_dict_is_empty(msg)) return SVCHOST_ERR_BAD_MESSAGE;
        return lsd_handle_nsxpc(msg, reply);
    }
    if (xw_as_int64(&v, &op) != XW_OK) return SVCHOST_ERR_BAD_MESSAGE;
    if (op == LSD_OP_RECORD) return lsd_op_record(msg, reply);
    if (op == LSD_OP_ALL_APPLICATIONS)
        return lsd_reply_with_retry(reply, LSD_OP_ALL_APPLICATIONS);
    if (op == LSD_OP_HANDLER_FOR_SCHEME) return lsd_op_scheme(msg, reply);
    if (op == LSD_OP_PLUGINS) return lsd_reply_empty_list(reply);
    if (op == LSD_OP_REGISTER) return lsd_reply_error_only(reply, 0);
    if (op == LSD_OP_OPEN)
        return lsd_reply_bool(reply, LSD_KEY_SUCCESS, 0);
    if (op == LSD_OP_OBSERVE_DB) return lsd_reply_error_only(reply, 0);
    /* Functions of the unregistered services: logged verbatim above,
     * answered with an error, never implemented here. */
    if (op == LSD_OP_OPEN_URL) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    if (op == LSD_OP_ICONS) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    if (op == LSD_OP_AD_ID) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

int lsd_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx) {
    int svc_idx = 0;
    if (ctx != NULL) svc_idx = *(const int *)ctx;
    return lsd_handle_request(msg, reply, svc_idx);
}

int lsd_init(void) {
    int rc;
    if (g_lsd_inited) return KERN_SUCCESS;
    rc = svchost_register(LSD_SVC_XPC, lsd_svchost_handler,
                          &g_lsd_ctx[0], &g_lsd_ports[0]);
    if (rc != KERN_SUCCESS) return rc;
    rc = svchost_register(LSD_SVC_MAPDB, lsd_svchost_handler,
                          &g_lsd_ctx[1], &g_lsd_ports[1]);
    if (rc != KERN_SUCCESS) return rc;
    rc = svchost_register(LSD_SVC_MODIFYDB, lsd_svchost_handler,
                          &g_lsd_ctx[2], &g_lsd_ports[2]);
    if (rc != KERN_SUCCESS) return rc;
    g_lsd_inited = 1;
    return KERN_SUCCESS;
}

int lsd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                    uint8_t *reply_buf, uint32_t max_reply_size,
                    uint32_t *out_reply_size) {
    mach_port_name_t reply_port;
    int one_way;
    uint8_t *body;
    uint32_t body_cap;
    int idx = -1;
    int i;
    int64_t err_code = 0;
    uint32_t body_len = 0;
    if (!hdr || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        !out_reply_size) {
        return 0;
    }
    /* The smallest reply is the 40-byte error dict. */
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;
    reply_port = hdr->msgh_local_port;
    one_way = (reply_port == MACH_PORT_NULL);
    body = reply_buf + sizeof(mach_msg_header_t);
    body_cap = max_reply_size - sizeof(mach_msg_header_t);

    /* Dispatch by destination port. */
    for (i = 0; i < LSD_NUM_SERVICES; i++) {
        if (g_lsd_ports[i] != MACH_PORT_NULL &&
            g_lsd_ports[i] == hdr->msgh_remote_port) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Not one of our ports. Same code launchd uses for an
         * unknown service. */
        err_code = 124;
    } else {
        xw_envelope env;
        xw_reader rd = { 0 };
        if (xw_envelope_parse(hdr, send_size, &env) != XW_OK ||
            xw_reader_init(&rd, env.payload, env.payload_len,
                           env.descs, env.ndescs) != XW_OK) {
            err_code = SVCHOST_ERR_BAD_MESSAGE;
        } else {
            xw_builder b;
            if (xw_builder_init(&b, body, body_cap) != XW_OK) {
                err_code = SVCHOST_ERR_NO_MEMORY;
            } else {
                int hr = lsd_handle_request(&rd, &b, idx);
                if (hr == 0) {
                    body_len = (uint32_t)xw_builder_len(&b);
                } else {
                    err_code = hr;
                }
            }
        }
    }

    if (err_code != 0) {
        if (lsd_build_error_body(body, body_cap, err_code,
                                 &body_len) != 0) {
            return 0;
        }
    }

    if (one_way) {
        *out_reply_size = 0;
        return 1;
    }

    {
        mach_msg_header_t *rep = (mach_msg_header_t *)reply_buf;
        rep->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep->msgh_size = sizeof(mach_msg_header_t) + body_len;
        rep->msgh_remote_port = reply_port;
        rep->msgh_local_port = MACH_PORT_NULL;
        rep->msgh_voucher_port = MACH_PORT_NULL;
        rep->msgh_id = SVCHOST_REPLY_MSGH_ID;
    }
    *out_reply_size = sizeof(mach_msg_header_t) + body_len;
    return 1;
}
