/*
 * container.c: app container layout and path translation for ioswine.
 * Implements the container.h API.
 *
 * Pure path translation: no filesystem access, no service, no
 * allocation. All state lives in static buffers. The launcher
 * calls container_setup() once at startup; callers invoke the
 * path functions after that.
 *
 * String helpers are file-local (ct_ prefix), following the
 * svchost.c convention: this file is freestanding and does not
 * use libc. No function-pointer tables in static data.
 *
 * No em dashes are used in this file.
 */
#include "container.h"
#include "ipc.h" /* lsk_getenv */

/* lsk_getuid lives in bsd.c (asm symbol _getuid). Declared here
 * following the ipc.c / kevent.c asm-label convention for
 * cross-file libsystem_kernel symbols. */
extern uint32_t lsk_getuid(void) __asm__("_getuid");

/* ------------------------------------------------------------------ */
/* File-local string helpers (no libc in freestanding code).           */
/* ------------------------------------------------------------------ */

static size_t ct_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int ct_strcmp(const char *s1, const char *s2) {
    while (*s1 != '\0' && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static void ct_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

/* ------------------------------------------------------------------ */
/* Static state.                                                       */
/* ------------------------------------------------------------------ */

static char g_container_home[CONTAINER_MAX_PATH];
static int g_container_home_set = 0;

static char g_container_bundle[CONTAINER_MAX_PATH];
static int g_container_bundle_set = 0;

typedef struct {
    char id[CONTAINER_MAX_GROUP_ID];
    char path[CONTAINER_MAX_PATH];
    int used;
} ct_group_entry;

static ct_group_entry g_container_groups[CONTAINER_MAX_GROUPS];

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */

/* Validate an absolute container root and copy it into dst with
 * trailing '/' characters stripped. Validates before writing, so
 * a failed call leaves dst untouched. Returns 0 or a negative
 * XW_ERR_*. */
static int ct_store_root(char *dst, size_t dst_cap, const char *src) {
    size_t len;
    size_t n;
    if (src == NULL) return XW_ERR_BAD_TYPE;
    len = ct_strlen(src);
    if (len == 0) return XW_ERR_BAD_TYPE;
    if (src[0] != '/') return XW_ERR_BAD_TYPE;
    n = len;
    while (n > 0 && src[n - 1] == '/') n--;
    if (n == 0) return XW_ERR_BAD_TYPE;
    if (n + 1 > dst_cap) return XW_ERR_NOSPACE;
    ct_memcpy(dst, src, n);
    dst[n] = '\0';
    return XW_OK;
}

/* Validate rel and measure the joined output length (without the
 * NUL) after normalization: ".." components are rejected, "."
 * and empty components are skipped. Writes nothing. Returns 0 or
 * XW_ERR_BAD_TYPE. */
static int ct_join_measure(const char *home, const char *rel,
                           size_t *need_out) {
    size_t need;
    const char *p;
    if (rel == NULL) return XW_ERR_BAD_TYPE;
    if (rel[0] == '\0') return XW_ERR_BAD_TYPE;
    if (rel[0] == '/') return XW_ERR_BAD_TYPE;
    need = ct_strlen(home);
    p = rel;
    while (*p != '\0') {
        const char *q;
        size_t clen;
        while (*p == '/') p++;
        if (*p == '\0') break;
        q = p;
        while (*q != '/' && *q != '\0') q++;
        clen = (size_t)(q - p);
        if (clen == 2 && p[0] == '.' && p[1] == '.')
            return XW_ERR_BAD_TYPE;
        if (clen == 1 && p[0] == '.') {
            p = q;
            continue;
        }
        need += 1 + clen;
        p = q;
    }
    *need_out = need;
    return XW_OK;
}

/* Copy the joined path into out. The caller has already measured
 * with ct_join_measure, so this cannot overflow. A ".." component
 * cannot occur here: ct_join_measure rejected it. */
static void ct_join_copy(const char *home, const char *rel, char *out) {
    size_t n = ct_strlen(home);
    const char *p = rel;
    ct_memcpy(out, home, n);
    while (*p != '\0') {
        const char *q;
        size_t clen;
        while (*p == '/') p++;
        if (*p == '\0') break;
        q = p;
        while (*q != '/' && *q != '\0') q++;
        clen = (size_t)(q - p);
        if (clen == 1 && p[0] == '.') {
            p = q;
            continue;
        }
        out[n] = '/';
        n++;
        ct_memcpy(out + n, p, clen);
        n += clen;
        p = q;
    }
    out[n] = '\0';
}

/* Copy a stored root into out with a bounds check. Returns 0 or
 * XW_ERR_NOSPACE (out untouched) / XW_ERR_BAD_TYPE (out NULL). */
static int ct_copy_out(const char *src, char *out, size_t cap) {
    size_t len;
    if (out == NULL) return XW_ERR_BAD_TYPE;
    len = ct_strlen(src);
    if (len + 1 > cap) return XW_ERR_NOSPACE;
    ct_memcpy(out, src, len + 1);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Home.                                                               */
/* ------------------------------------------------------------------ */

int container_set_home(const char *home) {
    int rc = ct_store_root(g_container_home,
                           sizeof(g_container_home), home);
    if (rc != XW_OK) return rc;
    g_container_home_set = 1;
    return XW_OK;
}

const char *container_home(void) {
    return g_container_home_set ? g_container_home : NULL;
}

int container_setup(void) {
    const char *v = lsk_getenv(CONTAINER_ENV_FIXED_HOME);
    uint32_t uid;
    if (v == NULL) v = lsk_getenv(CONTAINER_ENV_HOME);
    if (v != NULL) return container_set_home(v);
    uid = lsk_getuid();
    if (uid == CONTAINER_UID_MOBILE)
        return container_set_home(CONTAINER_HOME_MOBILE);
    if (uid == CONTAINER_UID_ROOT)
        return container_set_home(CONTAINER_HOME_ROOT);
    return XW_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Joins and well-known directories.                                   */
/* ------------------------------------------------------------------ */

int container_join(const char *rel, char *out, size_t cap) {
    const char *home;
    size_t need = 0;
    int rc;
    if (!g_container_home_set) return XW_ERR_NOT_FOUND;
    if (out == NULL) return XW_ERR_BAD_TYPE;
    if (cap == 0) return XW_ERR_NOSPACE;
    home = g_container_home;
    rc = ct_join_measure(home, rel, &need);
    if (rc != XW_OK) return rc;
    if (need + 1 > cap) return XW_ERR_NOSPACE;
    ct_join_copy(home, rel, out);
    return XW_OK;
}

int container_documents(char *out, size_t cap) {
    return container_join(CONTAINER_REL_DOCUMENTS, out, cap);
}

int container_library(char *out, size_t cap) {
    return container_join(CONTAINER_REL_LIBRARY, out, cap);
}

int container_preferences(char *out, size_t cap) {
    return container_join(CONTAINER_REL_PREFERENCES, out, cap);
}

int container_caches(char *out, size_t cap) {
    return container_join(CONTAINER_REL_CACHES, out, cap);
}

int container_tmp(char *out, size_t cap) {
    return container_join(CONTAINER_REL_TMP, out, cap);
}

/* ------------------------------------------------------------------ */
/* Bundle container.                                                   */
/* ------------------------------------------------------------------ */

int container_set_bundle(const char *bundle_path) {
    int rc = ct_store_root(g_container_bundle,
                           sizeof(g_container_bundle), bundle_path);
    if (rc != XW_OK) return rc;
    g_container_bundle_set = 1;
    return XW_OK;
}

int container_bundle(char *out, size_t cap) {
    if (!g_container_bundle_set) return XW_ERR_NOT_FOUND;
    return ct_copy_out(g_container_bundle, out, cap);
}

/* ------------------------------------------------------------------ */
/* Group containers.                                                   */
/* ------------------------------------------------------------------ */

int container_set_group(const char *group_id, const char *path) {
    size_t id_len;
    int i;
    int slot = -1;
    int rc;
    if (group_id == NULL || path == NULL) return XW_ERR_BAD_TYPE;
    id_len = ct_strlen(group_id);
    if (id_len == 0) return XW_ERR_BAD_TYPE;
    if (id_len >= CONTAINER_MAX_GROUP_ID) return XW_ERR_NOSPACE;
    for (i = 0; i < CONTAINER_MAX_GROUPS; i++) {
        if (g_container_groups[i].used &&
            ct_strcmp(g_container_groups[i].id, group_id) == 0) {
            slot = i;
            break;
        }
        if (!g_container_groups[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return XW_ERR_NOSPACE;
    rc = ct_store_root(g_container_groups[slot].path,
                       sizeof(g_container_groups[slot].path), path);
    if (rc != XW_OK) return rc;
    ct_memcpy(g_container_groups[slot].id, group_id, id_len + 1);
    g_container_groups[slot].used = 1;
    return XW_OK;
}

int container_group(const char *group_id, char *out, size_t cap) {
    int i;
    if (group_id == NULL || group_id[0] == '\0')
        return XW_ERR_BAD_TYPE;
    for (i = 0; i < CONTAINER_MAX_GROUPS; i++) {
        if (g_container_groups[i].used &&
            ct_strcmp(g_container_groups[i].id, group_id) == 0)
            return ct_copy_out(g_container_groups[i].path, out, cap);
    }
    return XW_ERR_NOT_FOUND;
}
