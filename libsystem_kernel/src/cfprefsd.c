/*
 * cfprefsd.c: fake CFPreferences daemon (XPC service) for ioswine (S1).
 *
 * See cfprefsd.h for the design decisions, the operation table and
 * the known limitations.
 *
 * Style: freestanding C, same as svchost.c / fakeboard.c. No libc.
 * No allocation: all state lives in static tables and static
 * buffers. Handlers run under ipc_lock held by the dispatching
 * thread (see svchost.c), so the static file/arena buffers below
 * are never used re-entrantly on target; the host test is
 * single-threaded.
 *
 * No em dashes are used in this file.
 */
#include "cfprefsd.h"
#include "bplist.h"
#include "bsxpc.h"
#include "cfprefs_names.h"
#include "linux.h"

/* ------------------------------------------------------------------ */
/* File I/O indirection (see cfprefsd.h). Defaults are the plain      */
/* Darwin names bsd.c provides on target; the host test redirects     */
/* them with -D to its own translators.                                */
/* ------------------------------------------------------------------ */

#ifndef CFP_OPENAT
#define CFP_OPENAT openat
#endif
#ifndef CFP_READ
#define CFP_READ read
#endif
#ifndef CFP_WRITE
#define CFP_WRITE write
#endif
#ifndef CFP_CLOSE
#define CFP_CLOSE close
#endif
#ifndef CFP_RENAMEAT
#define CFP_RENAMEAT renameat
#endif
#ifndef CFP_FCNTL
#define CFP_FCNTL fcntl
#endif

extern int CFP_OPENAT(int dirfd, const char *path, int flags, int mode);
extern long CFP_READ(int fd, void *buf, unsigned long count);
extern long CFP_WRITE(int fd, const void *buf, unsigned long count);
extern int CFP_CLOSE(int fd);
extern int CFP_RENAMEAT(int oldfd, const char *oldpath,
                        int newfd, const char *newpath);
extern int CFP_FCNTL(int fd, int cmd, void *arg);

/* Darwin flag values (bsd.c translates them to Linux on target). */
#define CFP_O_RDONLY    0
#define CFP_O_WRONLY    1
#define CFP_O_RDWR      2
#define CFP_O_CREAT     0x0200
#define CFP_O_TRUNC     0x0400
#define CFP_O_DIRECTORY 0x100000
#define CFP_F_GETFL     3
#define CFP_F_GETPATH   50
#define CFP_AT_FDCWD    (-2)

/* container.h provides the path translation. */
#include "container.h"

/* ------------------------------------------------------------------ */
/* Freestanding string / memory helpers.                               */
/* ------------------------------------------------------------------ */

static size_t cf_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int cf_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int cf_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static void cf_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void cf_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

/* Append s to buf (NUL terminated, cap bytes total). Returns 0 or
 * -1 when the result would not fit. */
static int cf_strappend(char *buf, size_t cap, const char *s) {
    size_t bl = cf_strlen(buf);
    size_t sl = cf_strlen(s);
    if (bl + sl + 1 > cap) return -1;
    cf_memcpy(buf + bl, s, sl + 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Static buffers (see the file header comment).                       */
/* ------------------------------------------------------------------ */

static uint8_t g_cfp_file_buf[CFP_MAX_PLIST_BYTES];
static uint8_t g_cfp_arena_buf[CFP_ARENA_BYTES];
static int g_cfp_inited = 0;

/* ------------------------------------------------------------------ */
/* Shared-memory generation page.                  */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)
#define CFP_SYS_MEMFD_CREATE 279
#elif defined(__x86_64__)
#define CFP_SYS_MEMFD_CREATE 319
#else
#define CFP_SYS_MEMFD_CREATE (-1)
#endif

typedef struct {
    char file[CFP_MAX_DOMAIN + 16];
} cfp_shmem_slot_entry_t;

static volatile uint32_t *g_cfp_shmem = NULL;
static uint32_t g_cfp_shmem_fallback[CFP_SHMEM_SLOTS];
static char g_cfp_shmem_name[16];
static int g_cfp_shmem_fd = -1;
static int g_cfp_shmem_backed = 0;
static int g_cfp_shmem_inited = 0;
static cfp_shmem_slot_entry_t g_cfp_slots[CFP_MAX_SHMEM_DOMAINS];
static uint32_t g_cfp_slot_count = 0;

static void cfp_build_shmem_name(void) {
    static const char hex[] = CFP_HEX_DIGITS;
    long pid = sys0(SYS_getpid);
    uint32_t v = (pid > 0) ? (uint32_t)pid : 1u;
    char rev[12];
    size_t rlen = 0;
    size_t pos = 0;
    const char *pfx = CFP_SHMEM_PREFIX;
    while (pfx[pos] && pos + 1 < sizeof(g_cfp_shmem_name)) {
        g_cfp_shmem_name[pos] = pfx[pos];
        pos++;
    }
    while (v != 0 && rlen < sizeof(rev)) {
        rev[rlen++] = hex[v & 0xfu];
        v >>= 4;
    }
    if (rlen == 0) rev[rlen++] = hex[0];
    while (rlen > 0 && pos + 1 < sizeof(g_cfp_shmem_name)) {
        g_cfp_shmem_name[pos++] = rev[--rlen];
    }
    g_cfp_shmem_name[pos] = '\0';
}

/* Write CFP_SHMEM_SIZE bytes of initial generation value 1u to fd.
 * Returns 0 on full write, -1 on failure. */
static int cfp_write_initial_shmem(int fd) {
    uint32_t chunk[256];
    uint32_t total = 0;
    for (size_t i = 0; i < 256; i++) chunk[i] = 1u;
    while (total < CFP_SHMEM_SIZE) {
        uint32_t rem = CFP_SHMEM_SIZE - total;
        uint32_t want = (rem < sizeof(chunk)) ? rem : (uint32_t)sizeof(chunk);
        uint32_t done = 0;
        while (done < want) {
            long n = CFP_WRITE(fd, (const uint8_t *)chunk + done,
                               (unsigned long)(want - done));
            if (n <= 0) return -1;
            done += (uint32_t)n;
        }
        total += want;
    }
    return 0;
}

static void cfp_shmem_init(void) {
    char shm_path[64];
    int fd = -1;
    void *mapped = NULL;
    if (g_cfp_shmem_inited) return;
    cfp_build_shmem_name();

    /* Prefer /dev/shm/<name> so CoreFoundation's shm_open() in bsd.c
     * maps the exact same physical page. */
    shm_path[0] = '\0';
    if (cf_strappend(shm_path, sizeof(shm_path), CFP_DEV_SHM_DIR) == 0 &&
        cf_strappend(shm_path, sizeof(shm_path), g_cfp_shmem_name) == 0) {
        fd = CFP_OPENAT(CFP_AT_FDCWD, shm_path,
                        CFP_O_RDWR | CFP_O_CREAT | CFP_O_TRUNC, 0600);
        if (fd >= 0) {
            if (cfp_write_initial_shmem(fd) == 0) {
                mapped = sys_mmap(NULL, CFP_SHMEM_SIZE,
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if ((uintptr_t)mapped < (uintptr_t)-4095L) {
                    g_cfp_shmem = (volatile uint32_t *)mapped;
                    g_cfp_shmem_backed = 1;
                }
            }
            CFP_CLOSE(fd);
        }
    }

    /* Fallback when /dev/shm is unavailable (e.g. stock Android):
     * create a memfd-backed shared page (or anonymous mmap) and expose
     * its fd via cfprefsd_shm_fd(). Leave g_cfp_shmem_backed = 0 so
     * replies do not advertise a name that lsk_shm_open would turn
     * into a 0-byte memfd (which would SIGBUS on ldar). */
    if (g_cfp_shmem == NULL && CFP_SYS_MEMFD_CREATE >= 0) {
        int mfd = (int)sys2(CFP_SYS_MEMFD_CREATE, (long)g_cfp_shmem_name, 0);
        if (mfd >= 0) {
            if (cfp_write_initial_shmem(mfd) == 0) {
                mapped = sys_mmap(NULL, CFP_SHMEM_SIZE,
                                  PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
                if ((uintptr_t)mapped < (uintptr_t)-4095L) {
                    g_cfp_shmem = (volatile uint32_t *)mapped;
                    g_cfp_shmem_fd = mfd;
                    /* bsd.c lsk_shm_open hands out a dup of this fd for our name. */
                    g_cfp_shmem_backed = 1;
                }
            }
            if (g_cfp_shmem == NULL) CFP_CLOSE(mfd);
        }
    }

    if (g_cfp_shmem == NULL) {
        mapped = sys_mmap(NULL, CFP_SHMEM_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((uintptr_t)mapped < (uintptr_t)-4095L) {
            g_cfp_shmem = (volatile uint32_t *)mapped;
        } else {
            g_cfp_shmem = g_cfp_shmem_fallback;
        }
        for (uint32_t i = 0; i < CFP_SHMEM_SLOTS; i++) {
            g_cfp_shmem[i] = 1u;
        }
    }

    g_cfp_shmem_inited = 1;
}

static uint32_t cfp_shmem_slot_for(const char *file) {
    uint32_t i = 0;
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)file;
    if (!g_cfp_shmem_inited) cfp_shmem_init();
    for (i = 0; i < g_cfp_slot_count; i++) {
        if (cf_strcmp(g_cfp_slots[i].file, file) == 0) return i + 1u;
    }
    if (g_cfp_slot_count < CFP_MAX_SHMEM_DOMAINS) {
        size_t flen = cf_strlen(file);
        if (flen + 1 <= sizeof(g_cfp_slots[0].file)) {
            cf_memcpy(g_cfp_slots[g_cfp_slot_count].file, file, flen + 1);
            g_cfp_slot_count++;
            return g_cfp_slot_count;
        }
    }
    while (*p) {
        h ^= (uint32_t)*p++;
        h *= 16777619u;
    }
    return (uint32_t)(CFP_MAX_SHMEM_DOMAINS + 1u +
                      (h % (CFP_SHMEM_SLOTS - CFP_MAX_SHMEM_DOMAINS - 1u)));
}

static uint32_t cfp_shmem_state(uint32_t slot) {
    return __atomic_load_n(&g_cfp_shmem[slot], __ATOMIC_ACQUIRE);
}

/* Increment the domain's generation counter, skipping 0 and 0xffffffff
 * (the client treats 0xffffffff as invalid sentinel and skips 0 on
 * write-reply optimization). Mirrors 0x18356b208 / 0x18356a3b4. */
static void cfp_shmem_bump(const char *file) {
    uint32_t slot = cfp_shmem_slot_for(file);
    uint32_t prev = __atomic_fetch_add(&g_cfp_shmem[slot], 1u, __ATOMIC_ACQ_REL);
    uint32_t next = prev + 1u;
    if (next == 0u || next == 0xffffffffu) {
        __atomic_store_n(&g_cfp_shmem[slot], 1u, __ATOMIC_RELEASE);
    }
}

const char *cfprefsd_shm_name(void) {
    return g_cfp_shmem_name;
}

int cfprefsd_shm_fd(const char *name) {
    if (!g_cfp_shmem_inited || g_cfp_shmem_fd < 0 || name == NULL)
        return -1;
    if (cf_strcmp(name, g_cfp_shmem_name) != 0) return -1;
    return g_cfp_shmem_fd;
}

/* ------------------------------------------------------------------ */
/* Domain and path handling.                                           */
/* ------------------------------------------------------------------ */

/* A domain is a file stem, never a path: non-empty, at most
 * CFP_MAX_DOMAIN bytes, no '/' anywhere, not "." or "..". */
static int cfp_domain_ok(const char *domain, uint32_t len) {
    uint32_t i;
    if (len < 2 || len - 1 > CFP_MAX_DOMAIN) return 0;
    for (i = 0; i + 1 < len; i++) {
        if (domain[i] == '/') return 0;
    }
    if (cf_strcmp(domain, ".") == 0 || cf_strcmp(domain, "..") == 0)
        return 0;
    return 1;
}

/* Build the storage directory and plist path for a domain.
 * dir_out is the Preferences directory (plus "/ByHost" when
 * by_host); file_out is the file stem; path_out is dir + "/" + file.
 * NSGlobalDomain maps to .GlobalPreferences[.hostid].plist, every
 * other domain to <domain>[.hostid].plist.
 * Returns 0 or -1 (container lookup failed or a buffer too small). */
static int cfp_domain_paths(const char *domain, int by_host,
                            char *dir_out, size_t dir_cap,
                            char *file_out, size_t file_cap,
                            char *path_out, size_t path_cap) {
    char prefs[CFP_MAX_PATH];
    if (container_preferences(prefs, sizeof(prefs)) != 0) return -1;
    dir_out[0] = '\0';
    if (cf_strappend(dir_out, dir_cap, prefs) != 0) return -1;
    if (by_host) {
        if (cf_strappend(dir_out, dir_cap, "/") != 0) return -1;
        if (cf_strappend(dir_out, dir_cap, CFP_DIR_BYHOST) != 0)
            return -1;
    }
    file_out[0] = '\0';
    if (cf_strcmp(domain, CFP_DOMAIN_GLOBAL) == 0 ||
        cf_strcmp(domain, CFP_DOMAIN_ANY_APP) == 0) {
        if (cf_strappend(file_out, file_cap, CFP_FILE_GLOBAL) != 0)
            return -1;
    } else {
        if (cf_strappend(file_out, file_cap, domain) != 0) return -1;
    }
    if (by_host) {
        if (cf_strappend(file_out, file_cap, ".") != 0) return -1;
        if (cf_strappend(file_out, file_cap, CFP_HOST_ID) != 0)
            return -1;
    }
    if (cf_strappend(file_out, file_cap, CFP_FILE_SUFFIX) != 0)
        return -1;
    path_out[0] = '\0';
    if (cf_strappend(path_out, path_cap, dir_out) != 0) return -1;
    if (cf_strappend(path_out, path_cap, "/") != 0) return -1;
    if (cf_strappend(path_out, path_cap, file_out) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Access-token validation.                  */
/* ------------------------------------------------------------------ */

/* Validate the CFPreferencesAccessToken fd carried by dict against
 * the target plist path: the fd must be opened O_RDWR on exactly
 * that path (F_GETFL access mode 2, F_GETPATH equal). Returns 0
 * when the token is present and valid, -1 otherwise. The fd number
 * comes from the Mach message descriptor (the XPC fd value consumes
 * one port descriptor, in wire order); on target that descriptor
 * carries a fileport port until ipc.c gains a fileport_makefd step,
 * so a token fails closed with -1 there (see cfprefsd.h). */
static int cfp_validate_token(xw_reader *r, const xw_value *dict,
                              const char *plist_path) {
    xw_value v;
    uint32_t desc_idx = 0;
    int fd = -1;
    int flags = -1;
    char fdpath[CFP_MAX_PATH];
    if (xw_dict_find_v(dict, CFP_KEY_ACCESS_TOKEN, &v) != XW_OK)
        return -1;
    if (xw_as_fd(&v, &desc_idx) != XW_OK) return -1;
    if (desc_idx >= r->ndescs) return -1;
    fd = (int)r->descs[desc_idx].obj;
    if (fd < 0) return -1;
    flags = CFP_FCNTL(fd, CFP_F_GETFL, NULL);
    if (flags < 0 || (flags & 3) != CFP_O_RDWR) return -1;
    if (CFP_FCNTL(fd, CFP_F_GETPATH, fdpath) != 0) return -1;
    /* Linux /proc readlink appends " (deleted)" when the file has
     * been replaced since the fd was opened. Our own temp-file plus
     * rename does exactly that between the sub-messages of a multi,
     * so a token fd opened before the first sub-message reports the
     * suffixed path for the second. Darwin's F_GETPATH has no such
     * suffix, so strip it before comparing: the fd still proves the
     * client could open the file at that path for writing. */
    {
        static const char deleted_suffix[] = " (deleted)";
        size_t flen = cf_strlen(fdpath);
        size_t slen = cf_strlen(deleted_suffix);
        if (flen >= slen &&
            cf_strcmp(fdpath + flen - slen, deleted_suffix) == 0)
            fdpath[flen - slen] = '\0';
    }
    if (cf_strcmp(fdpath, plist_path) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Domain plist read / write.                                          */
/*                                                                     */
/* Return convention for the helpers below: 0 on success, else the    */
/* SVCHOST_ERR_* value to answer with.                                 */
/* ------------------------------------------------------------------ */

/* Read a domain plist into the arena. On success *root_out is a
 * BP_DICT (fresh and empty when the file is missing or zero-length)
 * and *existed_out is 1 when the file was present, 0 when it was
 * not. A missing directory fails with SVCHOST_ERR_BAD_MESSAGE (the
 * fake never creates directories, cfprefsd.md section 5.4); a
 * non-empty file that is not a bplist00 dict fails the same way; a
 * file over CFP_MAX_PLIST_BYTES fails with SVCHOST_ERR_NO_MEMORY. */
static int cfp_read_domain(const char *dir, const char *file,
                           bp_arena *arena, bp_value **root_out,
                           int *existed_out) {
    int dirfd = -1;
    int fd = -1;
    uint32_t len = 0;
    bp_value *root = NULL;
    *root_out = NULL;
    *existed_out = 0;
    bp_arena_init(arena, g_cfp_arena_buf, sizeof(g_cfp_arena_buf));
    dirfd = CFP_OPENAT(CFP_AT_FDCWD, dir, CFP_O_RDONLY, 0);
    if (dirfd < 0) return SVCHOST_ERR_BAD_MESSAGE;
    fd = CFP_OPENAT(dirfd, file, CFP_O_RDONLY, 0);
    if (fd < 0) {
        /* No file yet, but the directory exists: fresh empty dict. */
        CFP_CLOSE(dirfd);
        root = bp_new(arena, BP_DICT);
        if (root == NULL) return SVCHOST_ERR_NO_MEMORY;
        *root_out = root;
        return 0;
    }
    *existed_out = 1;
    for (;;) {
        long n;
        if (len >= CFP_MAX_PLIST_BYTES) {
            /* One more byte would overflow: the file is too big. */
            CFP_CLOSE(fd);
            CFP_CLOSE(dirfd);
            return SVCHOST_ERR_NO_MEMORY;
        }
        n = CFP_READ(fd, g_cfp_file_buf + len,
                     (unsigned long)(CFP_MAX_PLIST_BYTES - len));
        if (n < 0) {
            CFP_CLOSE(fd);
            CFP_CLOSE(dirfd);
            return SVCHOST_ERR_BAD_MESSAGE;
        }
        if (n == 0) break;
        len += (uint32_t)n;
    }
    CFP_CLOSE(fd);
    CFP_CLOSE(dirfd);
    if (len == 0) {
        root = bp_new(arena, BP_DICT);
        if (root == NULL) return SVCHOST_ERR_NO_MEMORY;
        *root_out = root;
        return 0;
    }
    if (bp_parse(g_cfp_file_buf, len, arena, &root) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (root == NULL || root->type != BP_DICT)
        return SVCHOST_ERR_BAD_MESSAGE;
    *root_out = root;
    return 0;
}

/* Write a domain dict: serialize to bplist00, write a temp file in
 * the same directory, rename it onto the target (never write the
 * target in place, cfprefsd.md section 5.3). Returns 0,
 * SVCHOST_ERR_NO_MEMORY (serialized dict over the cap) or
 * SVCHOST_ERR_BAD_MESSAGE (any I/O failure). A failed write may
 * leave the temp file behind; the next write overwrites it. */
static int cfp_write_domain(const char *dir, const char *file,
                            const bp_value *root) {
    uint32_t wlen = 0;
    int dirfd = -1;
    int tmpfd = -1;
    char tmp[CFP_MAX_DOMAIN + 16];
    uint32_t done = 0;
    if (bp_write(root, g_cfp_file_buf, sizeof(g_cfp_file_buf),
                 &wlen) != 0)
        return SVCHOST_ERR_NO_MEMORY;
    dirfd = CFP_OPENAT(CFP_AT_FDCWD, dir, CFP_O_RDONLY, 0);
    if (dirfd < 0) return SVCHOST_ERR_BAD_MESSAGE;
    tmp[0] = '\0';
    if (cf_strappend(tmp, sizeof(tmp), file) != 0 ||
        cf_strappend(tmp, sizeof(tmp), CFP_TMP_SUFFIX) != 0) {
        CFP_CLOSE(dirfd);
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    tmpfd = CFP_OPENAT(dirfd, tmp,
                       CFP_O_WRONLY | CFP_O_CREAT | CFP_O_TRUNC, 0644);
    if (tmpfd < 0) {
        CFP_CLOSE(dirfd);
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    while (done < wlen) {
        long n = CFP_WRITE(tmpfd, g_cfp_file_buf + done,
                           (unsigned long)(wlen - done));
        if (n <= 0) {
            CFP_CLOSE(tmpfd);
            CFP_CLOSE(dirfd);
            return SVCHOST_ERR_BAD_MESSAGE;
        }
        done += (uint32_t)n;
    }
    if (CFP_CLOSE(tmpfd) != 0) {
        CFP_CLOSE(dirfd);
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    if (CFP_RENAMEAT(dirfd, tmp, dirfd, file) != 0) {
        CFP_CLOSE(dirfd);
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    CFP_CLOSE(dirfd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Message field helpers.                                              */
/* ------------------------------------------------------------------ */

/* Read the domain, user scope and ByHost flag out of a message dict.
 * domain_out points into the message buffer (NUL terminated).
 * Missing CFPreferencesUser means CurrentUser. Returns 0 or
 * SVCHOST_ERR_BAD_MESSAGE. */
static int cfp_read_scope(const xw_value *dict, const char **domain_out,
                          uint32_t *domain_len_out, int *by_host_out) {
    xw_value v;
    const char *domain = NULL;
    uint32_t dlen = 0;
    int by_host = 0;
    if (xw_dict_find_v(dict, CFP_KEY_DOMAIN, &v) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_as_string(&v, &domain, &dlen) != XW_OK || domain == NULL)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (!cfp_domain_ok(domain, dlen)) return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_find_v(dict, CFP_KEY_USER, &v) == XW_OK) {
        const char *user = NULL;
        uint32_t ulen = 0;
        if (xw_as_string(&v, &user, &ulen) != XW_OK || user == NULL)
            return SVCHOST_ERR_BAD_MESSAGE;
        if (cf_strcmp(user, CFP_USER_CURRENT) != 0 &&
            cf_strcmp(user, CFP_USER_ANY) != 0)
            return SVCHOST_ERR_BAD_MESSAGE;
    }
    if (xw_dict_find_v(dict, CFP_KEY_BY_HOST, &v) == XW_OK) {
        if (xw_as_bool(&v, &by_host) != XW_OK)
            return SVCHOST_ERR_BAD_MESSAGE;
    }
    *domain_out = domain;
    *domain_len_out = dlen;
    *by_host_out = by_host;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Operation implementations.                                          */
/* ------------------------------------------------------------------ */

/* Op 1: set a single value. dict is the operation dict, token_dict
 * is the dict carrying the CFPreferencesAccessToken (the message
 * itself for a top-level set, the outer message for a sub-message
 * of a multi). The Value is stored as-is as a bplist DATA node
 * [guess: cfprefsd.md section 3.2]. */
static int cfp_apply_set(xw_reader *r, const xw_value *dict,
                         const xw_value *token_dict) {
    const char *domain = NULL;
    uint32_t dlen = 0;
    int by_host = 0;
    xw_value v;
    const char *key = NULL;
    uint32_t klen = 0;
    const uint8_t *val_ptr = NULL;
    uint32_t val_len = 0;
    uint32_t val_desc = XW_NO_DESC;
    char dir[CFP_MAX_PATH];
    char file[CFP_MAX_DOMAIN + 16];
    char path[CFP_MAX_PATH];
    bp_arena arena;
    bp_value *root = NULL;
    int existed = 0;
    int rc = 0;
    bp_value *val = NULL;
    if (cfp_read_scope(dict, &domain, &dlen, &by_host) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_find_v(dict, CFP_KEY_KEY, &v) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_as_string(&v, &key, &klen) != XW_OK || key == NULL)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (klen < 2 || klen - 1 > CFP_MAX_KEY)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_find_v(dict, CFP_KEY_VALUE, &v) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_as_data(&v, &val_ptr, &val_len, &val_desc) != XW_OK ||
        val_ptr == NULL)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (cfp_domain_paths(domain, by_host, dir, sizeof(dir),
                         file, sizeof(file), path, sizeof(path)) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    rc = cfp_read_domain(dir, file, &arena, &root, &existed);
    if (rc != 0) return rc;
    if (existed) {
        /* An existing domain file always needs a valid token. */
        if (cfp_validate_token(r, token_dict, path) != 0)
            return SVCHOST_ERR_BAD_MESSAGE;
    }
    val = bp_new_data(&arena, val_ptr, val_len);
    if (val == NULL) return SVCHOST_ERR_NO_MEMORY;
    if (bp_dict_set(&arena, root, key, val) != 0)
        return SVCHOST_ERR_NO_MEMORY;
    rc = cfp_write_domain(dir, file, root);
    if (rc == 0) cfp_shmem_bump(file);
    return rc;
}

/* Op 8 [guess]: set multiple values from inline key/value pairs.
 * Every entry of the dict other than the ten known message keys is
 * one pair. String, int, uint, bool, double and inline data values
 * are converted to the matching bplist node; any other type fails
 * the whole operation with SVCHOST_ERR_BAD_MESSAGE. */
static int cfp_apply_set_multiple(xw_reader *r, const xw_value *dict,
                                  const xw_value *token_dict) {
    const char *domain = NULL;
    uint32_t dlen = 0;
    int by_host = 0;
    char dir[CFP_MAX_PATH];
    char file[CFP_MAX_DOMAIN + 16];
    char path[CFP_MAX_PATH];
    bp_arena arena;
    bp_value *root = NULL;
    int existed = 0;
    int rc = 0;
    int pairs = 0;
    xw_dict_iter it;
    const char *key = NULL;
    xw_value v;
    if (cfp_read_scope(dict, &domain, &dlen, &by_host) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (cfp_domain_paths(domain, by_host, dir, sizeof(dir),
                         file, sizeof(file), path, sizeof(path)) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    rc = cfp_read_domain(dir, file, &arena, &root, &existed);
    if (rc != 0) return rc;
    if (existed) {
        if (cfp_validate_token(r, token_dict, path) != 0)
            return SVCHOST_ERR_BAD_MESSAGE;
    }
    if (xw_dict_iter_init_v(dict, &it) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    for (;;) {
        int next = xw_dict_next(&it, &key, &v);
        bp_value *val = NULL;
        if (next == 0) break;
        if (next < 0) return SVCHOST_ERR_BAD_MESSAGE;
        if (key == NULL) return SVCHOST_ERR_BAD_MESSAGE;
        /* Skip the known message keys: the rest are pairs. */
        if (cf_strcmp(key, CFP_KEY_OPERATION) == 0 ||
            cf_strcmp(key, CFP_KEY_DOMAIN) == 0 ||
            cf_strcmp(key, CFP_KEY_USER) == 0 ||
            cf_strcmp(key, CFP_KEY_MESSAGES) == 0 ||
            cf_strcmp(key, CFP_KEY_KEY) == 0 ||
            cf_strcmp(key, CFP_KEY_VALUE) == 0 ||
            cf_strcmp(key, CFP_KEY_CURRENT_APP_DOMAIN) == 0 ||
            cf_strcmp(key, CFP_KEY_SYNC_WRITE) == 0 ||
            cf_strcmp(key, CFP_KEY_BY_HOST) == 0 ||
            cf_strcmp(key, CFP_KEY_ACCESS_TOKEN) == 0 ||
            cf_strcmp(key, CFP_KEY_IS_MANAGED) == 0 ||
            cf_strcmp(key, CFP_KEY_HOST_BUNDLE_ID) == 0)
            continue;
        if (cf_strlen(key) == 0 || cf_strlen(key) > CFP_MAX_KEY)
            return SVCHOST_ERR_BAD_MESSAGE;
        if (v.type == XW_TYPE_STRING) {
            const char *s = NULL;
            uint32_t slen = 0;
            if (xw_as_string(&v, &s, &slen) != XW_OK || s == NULL)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_string(&arena, s);
        } else if (v.type == XW_TYPE_INT64) {
            int64_t n = 0;
            if (xw_as_int64(&v, &n) != XW_OK)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_int(&arena, n);
        } else if (v.type == XW_TYPE_UINT64) {
            uint64_t n = 0;
            if (xw_as_uint64(&v, &n) != XW_OK)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_uint(&arena, n);
        } else if (v.type == XW_TYPE_BOOL) {
            int b = 0;
            if (xw_as_bool(&v, &b) != XW_OK)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_bool(&arena, b);
        } else if (v.type == XW_TYPE_DOUBLE) {
            double d = 0;
            if (xw_as_double(&v, &d) != XW_OK)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_real(&arena, d);
        } else if (v.type == XW_TYPE_DATA) {
            const uint8_t *p = NULL;
            uint32_t dl = 0;
            uint32_t dd = XW_NO_DESC;
            if (xw_as_data(&v, &p, &dl, &dd) != XW_OK || p == NULL)
                return SVCHOST_ERR_BAD_MESSAGE;
            val = bp_new_data(&arena, p, dl);
        } else {
            return SVCHOST_ERR_BAD_MESSAGE;
        }
        if (val == NULL) return SVCHOST_ERR_NO_MEMORY;
        if (bp_dict_set(&arena, root, key, val) != 0)
            return SVCHOST_ERR_NO_MEMORY;
        pairs++;
    }
    if (pairs == 0) return SVCHOST_ERR_BAD_MESSAGE;
    rc = cfp_write_domain(dir, file, root);
    if (rc == 0) cfp_shmem_bump(file);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Op 1 read: raw bplist loading and reply generation.                 */
/* ------------------------------------------------------------------ */

/* Read the raw bplist bytes of a domain plist file into g_cfp_file_buf.
 * *out_len is set to the byte length. Returns 0 on success (valid bplist00
 * loaded into g_cfp_file_buf), or -1 if the directory or file is missing,
 * empty, unreadable, or not a bplist00 file. */
static int cfp_read_domain_raw(const char *dir, const char *file,
                              uint32_t *out_len) {
    int dirfd = -1;
    int fd = -1;
    uint32_t len = 0;
    *out_len = 0;
    dirfd = CFP_OPENAT(CFP_AT_FDCWD, dir, CFP_O_RDONLY, 0);
    if (dirfd < 0) return -1;
    fd = CFP_OPENAT(dirfd, file, CFP_O_RDONLY, 0);
    if (fd < 0) {
        CFP_CLOSE(dirfd);
        return -1;
    }
    for (;;) {
        long n;
        if (len >= CFP_MAX_PLIST_BYTES) {
            CFP_CLOSE(fd);
            CFP_CLOSE(dirfd);
            return -1;
        }
        n = CFP_READ(fd, g_cfp_file_buf + len,
                     (unsigned long)(CFP_MAX_PLIST_BYTES - len));
        if (n < 0) {
            CFP_CLOSE(fd);
            CFP_CLOSE(dirfd);
            return -1;
        }
        if (n == 0) break;
        len += (uint32_t)n;
    }
    CFP_CLOSE(fd);
    CFP_CLOSE(dirfd);
    if (len < 8 || cf_memcmp(g_cfp_file_buf, CFP_BPLIST_MAGIC, 8) != 0) {
        return -1;
    }
    *out_len = len;
    return 0;
}

/* Op 1 read: reply with domain plist data if present, or an empty dict.
 * [src: 0x183569874, 0x18357d12c, 0x18357d4d4].
 * Builds a dictionary in reply. Missing domain file or directory
 * succeeds with an empty dictionary {} (never error 22).
 * Returns 0 on success, or SVCHOST_ERR_BAD_MESSAGE if scope is invalid. */
static int cfp_apply_read(xw_reader *r, const xw_value *dict,
                          xw_builder *reply) {
    const char *domain = NULL;
    uint32_t dlen = 0;
    int by_host = 0;
    char dir[CFP_MAX_PATH];
    char file[CFP_MAX_DOMAIN + 16];
    char path[CFP_MAX_PATH];
    uint32_t raw_len = 0;
    uint32_t slot = 0;
    uint32_t state = 0;
    (void)r;
    if (cfp_read_scope(dict, &domain, &dlen, &by_host) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (cfp_domain_paths(domain, by_host, dir, sizeof(dir),
                         file, sizeof(file), path, sizeof(path)) != 0)
        return SVCHOST_ERR_BAD_MESSAGE;
    slot = cfp_shmem_slot_for(file);
    state = cfp_shmem_state(slot);
    if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    if (cfp_read_domain_raw(dir, file, &raw_len) == 0) {
        if (xw_dict_add_data(reply, CFP_KEY_PLIST_DATA,
                             g_cfp_file_buf, raw_len) != XW_OK) {
            return SVCHOST_ERR_NO_MEMORY;
        }
    }
    /* Emit shmem cache-validation fields when the backing /dev/shm page is
     * available and there is enough reply budget (keeps Op 5 multi batches
     * within SVCHOST_MAX_REPLY). */
    if (g_cfp_shmem_backed && reply->err == XW_OK &&
        reply->cap >= reply->len && (reply->cap - reply->len) >= 136u) {
        if (xw_dict_add_string(reply, CFP_KEY_SHMEM_NAME,
                               g_cfp_shmem_name) != XW_OK ||
            xw_dict_add_int64(reply, CFP_KEY_SHMEM_INDEX,
                              (int64_t)slot) != XW_OK ||
            xw_dict_add_uint64(reply, CFP_KEY_SHMEM_STATE,
                               (uint64_t)state) != XW_OK) {
            return SVCHOST_ERR_NO_MEMORY;
        }
    }
    if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* Op 5: multi-message batch. Applies each element of
 * CFPreferencesMessages in order, with the access token taken from the
 * outer message, and builds a reply dictionary containing a
 * CFPreferencesMessages array of per-message replies in order
 * [src: 0x183570fd8, 0x183571220, 0x18357127c, 0x18357cd8c].
 * Null entries in the request are answered with null [src: 0x183571228];
 * Op 1 read entries are answered with domain plist data or an empty dict
 * [src: 0x183569874, 0x18357d4d4]; Op 1 set entries and sync no-ops are
 * answered with an empty dict. */
static int cfp_handle_multi(xw_reader *r, const xw_value *root,
                            xw_builder *reply) {
    xw_value arr;
    uint32_t n = 0;
    uint32_t i = 0;
    if (xw_dict_find_v(root, CFP_KEY_MESSAGES, &arr) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (arr.type != XW_TYPE_ARRAY) return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_array_len_v(&arr, &n) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    if (xw_dict_add_array_begin(reply, CFP_KEY_MESSAGES) != XW_OK)
        return SVCHOST_ERR_NO_MEMORY;
    for (i = 0; i < n; i++) {
        xw_value elem;
        if (xw_array_at_v(&arr, i, &elem) != XW_OK)
            return SVCHOST_ERR_BAD_MESSAGE;
        if (elem.type == XW_TYPE_NULL) {
            /* [src: 0x183571228] xpc_null_create */
            if (xw_add_null(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        } else if (elem.type == XW_TYPE_DICTIONARY) {
            xw_value opv;
            int64_t sub_op = 0;
            if (xw_dict_find_v(&elem, CFP_KEY_OPERATION, &opv) != XW_OK ||
                xw_as_int64(&opv, &sub_op) != XW_OK) {
                if (xw_add_null(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            } else if (sub_op == CFP_OP_SET) {
                xw_value kv;
                if (xw_dict_find_v(&elem, CFP_KEY_KEY, &kv) == XW_OK) {
                    /* Sub-op 1 set */
                    int rc = cfp_apply_set(r, &elem, root);
                    if (rc != 0) return rc;
                    if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
                    if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
                } else {
                    /* Sub-op 1 read [src: 0x183565818, 0x183569874] */
                    int rc = cfp_apply_read(r, &elem, reply);
                    if (rc != 0) return rc;
                }
            } else if (sub_op == CFP_OP_SET_MULTIPLE) {
                int rc = cfp_apply_set_multiple(r, &elem, root);
                if (rc != 0) return rc;
                if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
                if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            } else if (sub_op == CFP_OP_NO_REPLY_A || sub_op == CFP_OP_NO_REPLY_B) {
                if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
                if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            } else {
                if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
                if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            }
        } else {
            if (xw_add_null(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        }
    }
    if (xw_array_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
    return 0;
}

/* ------------------------------------------------------------------ */
/* svchost handler.                                                    */
/* ------------------------------------------------------------------ */

int cfprefsd_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx) {
    xw_value root;
    xw_value opv;
    int64_t op = 0;
    int rc = 0;
    (void)ctx;
    /* Verbatim trace (no-op when tracing is off). */
    (void)bsxpc_log_dict(msg, CFP_TRACE_PREFIX);
    if (xw_reader_root(msg, &root) != XW_OK ||
        root.type != XW_TYPE_DICTIONARY)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_dict_find_v(&root, CFP_KEY_OPERATION, &opv) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (xw_as_int64(&opv, &op) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    switch (op) {
    case CFP_OP_SET: {
        xw_value kv;
        if (xw_dict_find_v(&root, CFP_KEY_KEY, &kv) == XW_OK) {
            /* Op 1 set single value */
            rc = cfp_apply_set(msg, &root, &root);
            if (rc != 0) return rc;
            if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
            return 0;
        } else {
            /* Op 1 read domain [src: 0x183565818, 0x183569874] */
            return cfp_apply_read(msg, &root, reply);
        }
    }
    case CFP_OP_SET_MULTIPLE:
        rc = cfp_apply_set_multiple(msg, &root, &root);
        if (rc != 0) return rc;
        if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        return 0;
    case CFP_OP_MULTI:
        return cfp_handle_multi(msg, &root, reply);
    case CFP_OP_NO_REPLY_A:
    case CFP_OP_NO_REPLY_B:
        /* [guess] synchronize-style no-op. */
        if (xw_dict_begin(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        if (xw_dict_end(reply) != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        return 0;
    default:
        /* Covers the unknown-number ops (get, copy-values, sync,
         * list): CFP_OP_GET etc. are -1 and can never match a real
         * operation number, so they land here until the first
         * device trace supplies the numbers. */
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    }
}

/* ------------------------------------------------------------------ */
/* Kobject entry point (svchost_process_msg shape).                    */
/* ------------------------------------------------------------------ */

/* Build the minimal {"error": code} reply body. Returns 0 and sets
 * *out_len, or -1 if it does not fit. */
static int cf_build_error_body(uint8_t *body, size_t cap, int64_t code,
                               uint32_t *out_len) {
    xw_builder b;
    if (xw_builder_init(&b, body, cap) != XW_OK) return -1;
    if (xw_dict_begin(&b) != XW_OK) return -1;
    if (xw_dict_add_int64(&b, CFP_KEY_ERROR, code) != XW_OK)
        return -1;
    if (xw_dict_end(&b) != XW_OK) return -1;
    *out_len = (uint32_t)xw_builder_len(&b);
    return 0;
}

/* Fill a reply header in reply_buf for a body of body_len bytes. */
static void cf_fill_reply_header(uint8_t *reply_buf, uint32_t body_len,
                                 mach_port_name_t reply_port) {
    mach_msg_header_t *rep = (mach_msg_header_t *)reply_buf;
    rep->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    rep->msgh_size = sizeof(mach_msg_header_t) + body_len;
    rep->msgh_remote_port = reply_port;
    rep->msgh_local_port = MACH_PORT_NULL;
    rep->msgh_voucher_port = MACH_PORT_NULL;
    rep->msgh_id = SVCHOST_REPLY_MSGH_ID;
}

int cfprefsd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                         uint8_t *reply_buf, uint32_t max_reply_size,
                         uint32_t *out_reply_size) {
    if (!hdr || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        !out_reply_size) {
        return 0;
    }
    /* The smallest reply we can ever build is the 40-byte error dict,
     * so a smaller buffer cannot satisfy the never-drop-a-reply rule. */
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;

    mach_port_name_t reply_port = hdr->msgh_local_port;
    uint8_t *body = reply_buf + sizeof(mach_msg_header_t);
    uint32_t body_cap = max_reply_size - sizeof(mach_msg_header_t);
    int one_way = (reply_port == MACH_PORT_NULL);
    int64_t err_code = 0;
    uint32_t body_len = 0;

    {
        xw_envelope env;
        xw_reader rd;
        int parse_ok = 0;
        /* Zero the reader first: bsxpc_log_dict needs a defined
         * state from a failed init (see bsxpc.h). */
        cf_memset(&rd, 0, sizeof(rd));
        if (xw_envelope_parse(hdr, send_size, &env) == XW_OK &&
            xw_reader_init(&rd, env.payload, env.payload_len,
                           env.descs, env.ndescs) == XW_OK) {
            parse_ok = 1;
        }
        if (!parse_ok) {
            err_code = SVCHOST_ERR_BAD_MESSAGE;
        } else {
            xw_builder b;
            if (xw_builder_init(&b, body, body_cap) != XW_OK) {
                err_code = SVCHOST_ERR_NO_MEMORY;
            } else {
                int hr = cfprefsd_svchost_handler(&rd, &b, NULL);
                if (hr == 0) {
                    body_len = (uint32_t)xw_builder_len(&b);
                } else {
                    err_code = hr;
                }
            }
        }
    }

    if (err_code != 0) {
        if (cf_build_error_body(body, body_cap, err_code,
                                &body_len) != 0) {
            return 0;
        }
    }

    if (one_way) {
        *out_reply_size = 0;
        return 1;
    }

    cf_fill_reply_header(reply_buf, body_len, reply_port);
    *out_reply_size = sizeof(mach_msg_header_t) + body_len;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Init.                                                               */
/* ------------------------------------------------------------------ */

int cfprefsd_init(void) {
    int rc = 0;
    if (g_cfp_inited) return KERN_SUCCESS;
    cfp_shmem_init();
    rc = svchost_register(CFP_SVC_AGENT, cfprefsd_svchost_handler,
                          NULL, NULL);
    if (rc != KERN_SUCCESS) return rc;
    rc = svchost_register(CFP_SVC_DAEMON, cfprefsd_svchost_handler,
                          NULL, NULL);
    if (rc != KERN_SUCCESS) return rc;
    g_cfp_inited = 1;
    return KERN_SUCCESS;
}
