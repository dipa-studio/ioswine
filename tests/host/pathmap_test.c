/*
 * pathmap_test.c - Host unit test for libsystem_kernel/src/pathmap.c.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pathmap.h"

const char *lsk_getenv(const char *name) {
    if (!name) return NULL;
    return getenv(name);
}

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) abort();
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0 && mkdir(tmp, 0755) != 0) abort();
            tmp[i] = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0 && mkdir(tmp, 0755) != 0) abort();
}

static void touch_file(const char *path) {
    char dir[1024];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        mkdir_p(dir);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) abort();
    close(fd);
}

static void rm_rf(const char *path) {
    /* Only used on our private temp dir inside build/tmp. */
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) {
        /* ignore */
    }
}

static void join2(char *out, size_t cap, const char *a, const char *b) {
    snprintf(out, cap, "%s%s", a, b);
}

int main(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "build/tmp";
    mkdir_p(tmpdir);

    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s/pathmap_test_XXXXXX", tmpdir);
    char *root = mkdtemp(tmpl);
    CHECK(root != NULL);
    if (!root) return 1;

    /* Populate fake iOS sysroot. */
    char p[1024];
    join2(p, sizeof(p), root, "/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist");
    touch_file(p);
    join2(p, sizeof(p), root, "/Library/Preferences/com.apple.test.plist");
    touch_file(p);
    join2(p, sizeof(p), root, "/usr/share/zoneinfo/UTC");
    touch_file(p);
    join2(p, sizeof(p), root, "/usr/lib/charset.alias");
    touch_file(p);
    join2(p, sizeof(p), root, "/usr/lib/libSystem.B.dylib");
    touch_file(p);
    join2(p, sizeof(p), root, "/usr/lib/system");
    mkdir_p(p);
    join2(p, sizeof(p), root, "/usr/lib/link_to_file");
    CHECK(symlink("charset.alias", p) == 0);
    join2(p, sizeof(p), root, "/usr/lib/link_to_dir");
    CHECK(symlink("system", p) == 0);
    join2(p, sizeof(p), root, "/private/var/preferences/com.apple.networkd.plist");
    touch_file(p);
    join2(p, sizeof(p), root, "/AppleInternal/Library/Test.plist");
    touch_file(p);
    join2(p, sizeof(p), root, "/System/Library/Frameworks/BrokenLink");
    mkdir_p((join2(p, sizeof(p), root, "/System/Library/Frameworks"), p));
    join2(p, sizeof(p), root, "/System/Library/Frameworks/BrokenLink");
    CHECK(symlink("/System/Library/DoesNotExistOnHost", p) == 0);

    char buf[LSK_PATH_MAX];
    char expected[LSK_PATH_MAX];
    const char *in;
    const char *out;

    /* 1. Default sysroot (unset) falls back to original path when missing. */
    unsetenv("IOSWINE_SYSROOT");
    unsetenv("IOSWINE_PATH_TRACE");
    in = "/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    /* The PC may host the qemu mirror (tools/qrun) at the default sysroot. */
    if (access("/data/local/tmp/ioswine/root", F_OK) != 0) CHECK(out == in);

    /* 2. Custom sysroot remaps existing files/dirs under allowed prefixes. */
    setenv("IOSWINE_SYSROOT", root, 1);

    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/System";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/Library";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/Library/Preferences/com.apple.test.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/usr/share/zoneinfo/UTC";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/private/var/preferences/com.apple.networkd.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/AppleInternal/Library/Test.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    /* Symlink inside sysroot pointing to iOS path must still remap (lstat). */
    in = "/System/Library/Frameworks/BrokenLink";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    /* 3. /usr/lib/ rules: non-dylib files remap; dylibs and dirs do NOT. */
    in = "/usr/lib/charset.alias";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/usr/lib/link_to_file";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    in = "/usr/lib/libSystem.B.dylib";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    in = "/usr/lib/system";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    in = "/usr/lib/link_to_dir";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    in = "/usr/lib/";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    in = "/usr/lib";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    /* 4. Exists-check fallback for missing paths under allowed prefixes. */
    in = "/System/Library/DoesNotExist.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    in = "/usr/share/missing.dat";
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);

    /* 5. Trailing slashes in IOSWINE_SYSROOT are normalized; "/" does not remap. */
    char root_slash[1024];
    snprintf(root_slash, sizeof(root_slash), "%s///", root);
    setenv("IOSWINE_SYSROOT", root_slash, 1);
    in = "/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist";
    out = lsk_path_remap(in, buf, sizeof(buf));
    join2(expected, sizeof(expected), root, in);
    CHECK(out == buf && strcmp(out, expected) == 0);

    setenv("IOSWINE_SYSROOT", "/", 1);
    out = lsk_path_remap(in, buf, sizeof(buf));
    CHECK(out == in);
    setenv("IOSWINE_SYSROOT", root, 1);

    /* 6. Never remap NULL, relative, /data/, /proc, /dev, bundle, non-matching, or already-remapped. */
    CHECK(lsk_path_remap(NULL, buf, sizeof(buf)) == NULL);
    const char *non_remapped[] = {
        "",
        "System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist",
        "./System/foo",
        "../Library/foo",
        "/data/local/tmp/ioswine/app/Info.plist",
        "/proc",
        "/proc/self/fd/0",
        "/dev",
        "/dev/null",
        "/var/containers/Bundle/Application/UUID/Aidoku.app/Info.plist",
        "/SystemFoo/bar",
        "/Library2/baz",
        "/usr/share",
        "/etc/hosts"
    };
    for (size_t i = 0; i < sizeof(non_remapped) / sizeof(non_remapped[0]); i++) {
        out = lsk_path_remap(non_remapped[i], buf, sizeof(buf));
        CHECK(out == non_remapped[i]);
    }
    join2(expected, sizeof(expected), root, "/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist");
    out = lsk_path_remap(expected, buf, sizeof(buf));
    CHECK(out == expected);

    /* 7. Buffer overflow and exact boundary tests. */
    in = "/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist";
    size_t needed = strlen(root) + strlen(in) + 1;
    CHECK(lsk_path_remap(in, NULL, 128) == in);
    CHECK(lsk_path_remap(in, buf, 0) == in);
    CHECK(lsk_path_remap(in, buf, 4) == in);

    char small[LSK_PATH_MAX];
    memset(small, 0xAA, sizeof(small));
    out = lsk_path_remap(in, small, needed - 1);
    CHECK(out == in);
    for (size_t i = 0; i < sizeof(small); i++) {
        CHECK((unsigned char)small[i] == 0xAA);
    }

    memset(small, 0xAA, sizeof(small));
    out = lsk_path_remap(in, small, needed);
    CHECK(out == small);
    join2(expected, sizeof(expected), root, in);
    CHECK(strcmp(small, expected) == 0);
    CHECK((unsigned char)small[needed] == 0xAA);

    /* 8. IOSWINE_PATH_TRACE=1 logging for hits and prefix misses only. */
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    int saved_err = dup(2);
    CHECK(saved_err >= 0);
    CHECK(dup2(pipefd[1], 2) >= 0);
    close(pipefd[1]);

    setenv("IOSWINE_PATH_TRACE", "1", 1);
    (void)lsk_path_remap("/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist", buf, sizeof(buf));
    (void)lsk_path_remap("/System/Library/Missing.plist", buf, sizeof(buf));
    (void)lsk_path_remap("/proc/self/fd/0", buf, sizeof(buf));
    (void)lsk_path_remap("relative/path", buf, sizeof(buf));
    unsetenv("IOSWINE_PATH_TRACE");
    (void)lsk_path_remap("/Library/Preferences/com.apple.test.plist", buf, sizeof(buf));

    dup2(saved_err, 2);
    close(saved_err);

    char logbuf[4096];
    ssize_t logn = read(pipefd[0], logbuf, sizeof(logbuf) - 1);
    close(pipefd[0]);
    CHECK(logn > 0);
    logbuf[logn > 0 ? logn : 0] = '\0';

    char hit_line[1024];
    snprintf(hit_line, sizeof(hit_line),
             "[path] /System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist -> %s/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist\n",
             root);
    CHECK(strstr(logbuf, hit_line) != NULL);
    CHECK(strstr(logbuf, "[path] /System/Library/Missing.plist -> /System/Library/Missing.plist (miss)\n") != NULL);
    CHECK(strstr(logbuf, "/proc/self/fd/0") == NULL);
    CHECK(strstr(logbuf, "relative/path") == NULL);
    CHECK(strstr(logbuf, "com.apple.test.plist") == NULL);

    /* 9. Reverse unmapping for F_GETPATH. */
    join2(buf, sizeof(buf), root, "/System/Library/PrivateFrameworks/CoreEmoji.framework");
    lsk_path_unmap_inplace(buf);
    CHECK(strcmp(buf, "/System/Library/PrivateFrameworks/CoreEmoji.framework") == 0);

    join2(buf, sizeof(buf), root, "/Library");
    lsk_path_unmap_inplace(buf);
    CHECK(strcmp(buf, "/Library") == 0);

    strcpy(buf, "/data/local/tmp/app/Info.plist");
    lsk_path_unmap_inplace(buf);
    CHECK(strcmp(buf, "/data/local/tmp/app/Info.plist") == 0);

    lsk_path_unmap_inplace(NULL);

    rm_rf(root);

    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
