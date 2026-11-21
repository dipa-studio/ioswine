/*
 * pathmap.c - Remap iOS root-filesystem paths to the on-device sysroot.
 *
 * Freestanding C for libsystem_kernel.dylib (arm64e, no libc, no malloc,
 * no __thread, no static function-pointer tables).
 */
#include "pathmap.h"
#include <stdint.h>
#include <stddef.h>

#ifdef PATHMAP_HOST_TEST
#include <sys/stat.h>
#include <unistd.h>
#else
#include "linux.h"
#endif

extern const char *lsk_getenv(const char *name);

#define DEFAULT_SYSROOT "/data/local/tmp/ioswine/root"

static size_t pm_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int pm_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int pm_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int pm_starts_with_len(const char *s, const char *prefix, size_t plen) {
    for (size_t i = 0; i < plen; i++) {
        if (s[i] != prefix[i]) return 0;
    }
    return 1;
}

static int pm_ends_with(const char *s, size_t slen, const char *suffix, size_t suflen) {
    if (slen < suflen) return 0;
    for (size_t i = 0; i < suflen; i++) {
        if (s[slen - suflen + i] != suffix[i]) return 0;
    }
    return 1;
}

static void pm_write_stderr(const char *s, size_t len) {
    if (!s || len == 0) return;
#ifdef PATHMAP_HOST_TEST
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(2, s + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
#else
    sys_write(2, s, len);
#endif
}

static int pm_trace_enabled(void) {
    const char *v = lsk_getenv("IOSWINE_PATH_TRACE");
    return (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
}

static void pm_trace_log(const char *in, const char *out, int is_miss) {
    if (!pm_trace_enabled()) return;
    /* Format on stack so each log line is a single write(2) call. */
    char line[LSK_PATH_MAX * 2 + 32];
    size_t pos = 0;
    const char *hdr = "[path] ";
    for (size_t i = 0; hdr[i] && pos < sizeof(line); i++) line[pos++] = hdr[i];
    for (size_t i = 0; in[i] && pos < sizeof(line) - 16; i++) line[pos++] = in[i];
    const char *arrow = " -> ";
    for (size_t i = 0; arrow[i] && pos < sizeof(line); i++) line[pos++] = arrow[i];
    for (size_t i = 0; out[i] && pos < sizeof(line) - 10; i++) line[pos++] = out[i];
    if (is_miss) {
        const char *miss = " (miss)";
        for (size_t i = 0; miss[i] && pos < sizeof(line) - 1; i++) line[pos++] = miss[i];
    }
    if (pos < sizeof(line)) line[pos++] = '\n';
    pm_write_stderr(line, pos);
}

static size_t pm_get_sysroot(const char **out_root) {
    const char *sr = lsk_getenv("IOSWINE_SYSROOT");
    if (!sr || !sr[0]) sr = DEFAULT_SYSROOT;
    size_t len = pm_strlen(sr);
    while (len > 0 && sr[len - 1] == '/') len--;
    *out_root = sr;
    return len;
}

/*
 * Check whether `path` is under one of the remappable iOS system prefixes.
 * Returns 1 if remappable (setting *need_reg = 1 for /usr/lib/ files only),
 * 0 if excluded/unmatched.
 */
static int pm_match_prefix(const char *path, size_t len, int *need_reg) {
    *need_reg = 0;
    if (!path || path[0] != '/') return 0;

    /* Explicit exclusions: /data/, /proc, /dev, and relative paths. */
    if (pm_starts_with(path, "/data/")) return 0;
    if (pm_strcmp(path, "/proc") == 0 || pm_starts_with(path, "/proc/")) return 0;
    if (pm_strcmp(path, "/dev") == 0 || pm_starts_with(path, "/dev/")) return 0;

    if (pm_strcmp(path, "/System") == 0 || pm_starts_with(path, "/System/")) return 1;
    if (pm_strcmp(path, "/Library") == 0 || pm_starts_with(path, "/Library/")) return 1;
    if (pm_starts_with(path, "/usr/share/")) return 1;
    if (pm_starts_with(path, "/private/var/preferences/")) return 1;
    if (pm_starts_with(path, "/AppleInternal/")) return 1;

    if (pm_starts_with(path, "/usr/lib/")) {
        /* Exclude /usr/lib/ itself and dylibs handled by the loader. */
        if (len == 9) return 0;
        if (pm_ends_with(path, len, ".dylib", 6)) return 0;
        *need_reg = 1;
        return 1;
    }

    return 0;
}

#ifndef PATHMAP_HOST_TEST
struct pm_linux_stat64 {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned int   st_mode;
    unsigned int   st_nlink;
    unsigned int   st_uid;
    unsigned int   st_gid;
    unsigned long  st_rdev;
    unsigned long  __pad1;
    long           st_size;
    int            st_blksize;
    int            __pad2;
    long           st_blocks;
    long           st_atime;
    unsigned long  st_atimensec;
    long           st_mtime;
    unsigned long  st_mtimensec;
    long           st_ctime;
    unsigned long  st_ctimensec;
    unsigned int   __pad3[2];
};
#endif

static int pm_path_exists(const char *candidate, int need_reg) {
#ifdef PATHMAP_HOST_TEST
    struct stat st;
    int r = need_reg ? stat(candidate, &st) : lstat(candidate, &st);
    if (r != 0) return 0;
    if (need_reg && !S_ISREG(st.st_mode)) return 0;
    return 1;
#else
    struct pm_linux_stat64 st;
    int flags = need_reg ? 0 : 0x100 /* AT_SYMLINK_NOFOLLOW */;
    long r = sys4(SYS_newfstatat, AT_FDCWD, (long)candidate, (long)&st, flags);
    if (r != 0) return 0;
    if (need_reg && ((st.st_mode & 0170000) != 0100000)) return 0;
    return 1;
#endif
}

const char *lsk_path_remap(const char *in, char *buf, size_t cap) {
    if (!in) return NULL;
    if (in[0] != '/') return in;
    /* Collapse leading "//..." to "/" (CoreEmoji builds "/" + "/System/..." at 0x1d5cc7188). */
    while (in[1] == '/') in++;

    size_t in_len = pm_strlen(in);
    int need_reg = 0;
    if (!pm_match_prefix(in, in_len, &need_reg)) return in;

    const char *sysroot = NULL;
    size_t sr_len = pm_get_sysroot(&sysroot);
    if (sr_len == 0) return in;

    /* Never double-remap if input already starts with sysroot. */
    if (in_len >= sr_len && pm_starts_with_len(in, sysroot, sr_len) &&
        (in[sr_len] == '/' || in[sr_len] == '\0')) {
        return in;
    }

    /* Check buffer capacity for sysroot + in + NUL without overflow. */
    if (!buf || cap == 0 || in_len >= cap || sr_len >= cap - in_len) {
        pm_trace_log(in, in, 1);
        return in;
    }

    for (size_t i = 0; i < sr_len; i++) buf[i] = sysroot[i];
    for (size_t i = 0; i <= in_len; i++) buf[sr_len + i] = in[i];

    if (pm_path_exists(buf, need_reg)) {
        pm_trace_log(in, buf, 0);
        return buf;
    }

    pm_trace_log(in, in, 1);
    return in;
}

void lsk_path_unmap_inplace(char *path) {
    if (!path) return;
    const char *sysroot = NULL;
    size_t sr_len = pm_get_sysroot(&sysroot);
    if (sr_len == 0) return;

    size_t plen = pm_strlen(path);
    if (plen <= sr_len) return;
    if (!pm_starts_with_len(path, sysroot, sr_len) || path[sr_len] != '/') return;

    const char *rest = path + sr_len;
    size_t rlen = plen - sr_len;
    int dummy = 0;
    if (!pm_match_prefix(rest, rlen, &dummy)) return;

    for (size_t i = 0; i <= rlen; i++) {
        path[i] = rest[i];
    }
}
