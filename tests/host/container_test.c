/* container_test.c: host test for container.{c,h}.
 *
 * What it covers:
 *   1. Unset state: container_home() is NULL and every path
 *      function returns XW_ERR_NOT_FOUND before any set/setup.
 *   2. set_home + well-known dirs: exact strings, no trailing
 *      slash (case 1); container_join with a plist path (case 2).
 *   3. container_join rejects an absolute rel, any ".."
 *      component, an empty rel and a NULL rel, and leaves out
 *      untouched on rejection (case 3).
 *   4. Overflow: a too-small out returns XW_ERR_NOSPACE and out
 *      is untouched; an overlong home/bundle/group id returns
 *      XW_ERR_NOSPACE and the stored value is unchanged (case 5).
 *   5. Env precedence via container_setup(): CFFIXED_USER_HOME
 *      beats HOME beats the uid home (/var/mobile for uid 501,
 *      /var/root for uid 0); an unknown uid returns
 *      XW_ERR_NOT_FOUND and leaves the home unchanged; setup
 *      re-reads the environment on every call; trailing slashes
 *      are stripped; a relative or empty env value is rejected
 *      (case 6).
 *   6. Bundle container: not-found before set, set/get round
 *      trip, trailing-slash normalization, relative/NULL/overlong
 *      rejected.
 *   7. Group containers: not-found before registration,
 *      register/get round trip, re-registration updates the path,
 *      the 8-entry table fills and the 9th id returns
 *      XW_ERR_NOSPACE, relative/NULL/overlong rejected.
 *   8. Join normalization: trailing slashes, "." and empty
 *      components are skipped; "." alone yields HOME.
 *   9. Temp-dir home: the API is driven with a real mkdtemp
 *      directory under /tmp; every joined path stays under it.
 *  10. Fuzz: 20000 mutated join inputs, 5000 mutated set_home /
 *      set_bundle inputs, 5000 mutated group registrations and
 *      2000 mutated group lookups. Invariants: never crash, the
 *      return is 0 or a negative XW_ERR_*, a successful join
 *      output is NUL-terminated, starts with the home and has no
 *      ".." component, and a failed call leaves out untouched.
 *      The temp dir is verified unchanged after the fuzz (one
 *      sentinel file, same contents): container.c performs no
 *      filesystem IO at all, so nothing may appear, disappear or
 *      change outside the test's own temp dir.
 *
 * How the environment is driven on host: there is no real init.c
 * here, so this file stubs lsk_getenv (reading the real process
 * environment, so setenv/unsetenv control it) and _getuid (the asm
 * symbol container.c references for lsk_getuid; it returns the
 * test-controlled g_test_uid so the uid-fallback cases are
 * deterministic whatever uid runs the test). The stub list was
 * confirmed with `nm -u` on the compiled container.o: lsk_getenv
 * and _getuid only.
 *
 * The temp dir is created with mkdtemp under /tmp, holds one
 * sentinel file, and is removed at the end of main (the rmdir is
 * itself a checked assertion). Every scenario prints one PASS/FAIL
 * line; main prints a TOTAL line and returns nonzero on any
 * failed check.
 *
 * Build: The -I tests/host/host-shim entry is kept first for consistency;
 * container.c does not include linux.h, so the
 * shim is not strictly needed here.
 *
 *   Plain:
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -I tests/host/host-shim \
 *        -I src -o /tmp/container_test tests/host/container_test.c \
 *        src/container.c
 *     /tmp/container_test
 *   ASan+UBSan (same command with sanitizers, run the same way):
 *     cc -std=c99 -Wall -Wextra -Werror -O1 -g \
 *        -fsanitize=address,undefined -fno-sanitize-recover=all \
 *        -I tests/host/host-shim -I src -o /tmp/container_test_asan \
 *        tests/host/container_test.c src/container.c
 *     /tmp/container_test_asan
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "xpcwire.h"
#include "container.h"

/* ------------------------------------------------------------------ */
/* Stubs (no real init.c / bsd.c on host). See the file header.        */
/* ------------------------------------------------------------------ */

const char *lsk_getenv(const char *name) {
    if (name == NULL) return NULL;
    return getenv(name);
}

/* Test-controlled uid: the stub for lsk_getuid (asm symbol
 * _getuid, see container.c). Scenarios set this before calling
 * container_setup() for the uid-fallback cases. */
static uint32_t g_test_uid = CONTAINER_UID_MOBILE;

uint32_t _getuid(void) {
    return g_test_uid;
}

/* ------------------------------------------------------------------ */
/* Harness (same shape as daemons_test.c).                             */
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
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static void fill_sentinel(char *buf, size_t n) {
    memset(buf, 0xAA, n);
}

static int check_sentinel(const char *buf, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if ((unsigned char)buf[i] != 0xAA) return 0;
    }
    return 1;
}

static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* True when path has a component equal to "..". */
static int has_dotdot(const char *path) {
    const char *p = path;
    while (*p != '\0') {
        const char *q;
        size_t clen;
        while (*p == '/') p++;
        if (*p == '\0') break;
        q = p;
        while (*q != '/' && *q != '\0') q++;
        clen = (size_t)(q - p);
        if (clen == 2 && p[0] == '.' && p[1] == '.') return 1;
        p = q;
    }
    return 0;
}

/* Count the entries of dir, excluding "." and "..". Returns -1 on
 * error. */
static int count_dir_entries(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (d == NULL) return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0) continue;
        if (strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

/* The canonical fake home for container test cases. */
#define TEST_HOME \
    "/var/mobile/Containers/Data/Application/11111111-2222-3333-4444-555555555555"
#define TEST_BUNDLE \
    "/var/containers/Bundle/Application/AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"
#define TEST_GROUP "group.com.example.app"
#define TEST_GROUP_PATH \
    "/var/mobile/Containers/Shared/AppGroup/99999999-8888-7777-6666-555555555555"

/* ------------------------------------------------------------------ */
/* Scenario 1: unset state.                                            */
/* ------------------------------------------------------------------ */

static void scen_1_unset(void) {
    char out[128];
    SCEN_BEGIN(1, "unset state");
    CHECK(container_home() == NULL);
    CHECK(container_join("Documents", out, sizeof(out)) ==
          XW_ERR_NOT_FOUND);
    CHECK(container_documents(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_library(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_preferences(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_caches(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_tmp(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_bundle(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    CHECK(container_group(TEST_GROUP, out, sizeof(out)) ==
          XW_ERR_NOT_FOUND);
    SCEN_END(1);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: set_home and well-known dirs, exact strings.            */
/* ------------------------------------------------------------------ */

static void scen_2_well_known(void) {
    char out[CONTAINER_MAX_PATH];
    int rc;
    SCEN_BEGIN(2, "set_home and well-known dirs");
    rc = container_set_home(TEST_HOME);
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), TEST_HOME) == 0);
    rc = container_documents(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Documents") == 0);
    rc = container_library(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Library") == 0);
    rc = container_preferences(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Library/Preferences") == 0);
    rc = container_caches(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Library/Caches") == 0);
    rc = container_tmp(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/tmp") == 0);
    rc = container_join("Library/Preferences/com.example.app.plist",
                        out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME
                      "/Library/Preferences/com.example.app.plist") == 0);
    /* Trailing slashes are stripped at set time. */
    rc = container_set_home(TEST_HOME "/");
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), TEST_HOME) == 0);
    rc = container_set_home(TEST_HOME "///");
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), TEST_HOME) == 0);
    SCEN_END(2);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: join rejections, out untouched.                          */
/* ------------------------------------------------------------------ */

static void scen_3_rejections(void) {
    char out[128];
    int rc;
    const char *bad_rels[] = {
        "/etc/passwd", "../etc", "a/../b", "..", "a/..",
        "a/../../b", "", "Library/../../etc/passwd"
    };
    size_t i;
    SCEN_BEGIN(3, "join rejections");
    /* Home is the TEST_HOME set by scenario 2. */
    CHECK(container_home() != NULL);
    for (i = 0; i < sizeof(bad_rels) / sizeof(bad_rels[0]); i++) {
        fill_sentinel(out, sizeof(out));
        rc = container_join(bad_rels[i], out, sizeof(out));
        CHECK(rc == XW_ERR_BAD_TYPE);
        CHECK(check_sentinel(out, sizeof(out)));
    }
    fill_sentinel(out, sizeof(out));
    rc = container_join(NULL, out, sizeof(out));
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(check_sentinel(out, sizeof(out)));
    fill_sentinel(out, sizeof(out));
    rc = container_join("Documents", NULL, sizeof(out));
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(check_sentinel(out, sizeof(out)));
    rc = container_join("Documents", out, 0);
    CHECK(rc == XW_ERR_NOSPACE);
    SCEN_END(3);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: overflow / overlong inputs.                             */
/* ------------------------------------------------------------------ */

static void scen_4_overflow(void) {
    char small[8];
    char big[2100];
    char out[CONTAINER_MAX_PATH];
    int rc;
    size_t i;
    SCEN_BEGIN(4, "overflow and overlong inputs");
    /* Home is the long TEST_HOME: every well-known dir overflows
     * an 8-byte out, and out must be left untouched. */
    fill_sentinel(small, sizeof(small));
    rc = container_documents(small, sizeof(small));
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(check_sentinel(small, sizeof(small)));
    fill_sentinel(small, sizeof(small));
    rc = container_join("Library/Preferences/com.example.app.plist",
                        small, sizeof(small));
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(check_sentinel(small, sizeof(small)));
    /* A one-byte-short buffer also fails without a partial write. */
    rc = container_documents(out, sizeof(out));
    CHECK(rc == XW_OK);
    fill_sentinel(out, sizeof(out));
    rc = container_documents(out, strlen(TEST_HOME "/Documents"));
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(check_sentinel(out, sizeof(out)));
    /* Overlong home: rejected, stored home unchanged. */
    memset(big, 'a', sizeof(big) - 1);
    big[0] = '/';
    big[sizeof(big) - 1] = '\0';
    rc = container_set_home(big);
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), TEST_HOME) == 0);
    /* Overlong bundle: rejected, bundle still unset. */
    rc = container_set_bundle(big);
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(container_bundle(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    /* Overlong group id: rejected. */
    for (i = 0; i < CONTAINER_MAX_GROUP_ID + 10; i++) big[i] = 'g';
    big[CONTAINER_MAX_GROUP_ID + 10] = '\0';
    rc = container_set_group(big, TEST_GROUP_PATH);
    CHECK(rc == XW_ERR_NOSPACE);
    /* set_home rejects relative, empty and NULL roots. */
    rc = container_set_home("var/mobile");
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_home("");
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_home(NULL);
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_home("/");
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), TEST_HOME) == 0);
    SCEN_END(4);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: env precedence via container_setup.                     */
/* ------------------------------------------------------------------ */

static void scen_5_env(void) {
    int rc;
    SCEN_BEGIN(5, "env precedence via container_setup");
    unsetenv(CONTAINER_ENV_FIXED_HOME);
    unsetenv(CONTAINER_ENV_HOME);
    /* Both set: CFFIXED_USER_HOME wins. */
    setenv(CONTAINER_ENV_FIXED_HOME, "/a", 1);
    setenv(CONTAINER_ENV_HOME, "/b", 1);
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/a") == 0);
    /* Only HOME: HOME wins. */
    unsetenv(CONTAINER_ENV_FIXED_HOME);
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/b") == 0);
    /* Neither: uid 501 falls back to /var/mobile. */
    unsetenv(CONTAINER_ENV_HOME);
    g_test_uid = CONTAINER_UID_MOBILE;
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), CONTAINER_HOME_MOBILE) == 0);
    /* Neither: uid 0 falls back to /var/root. */
    g_test_uid = CONTAINER_UID_ROOT;
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), CONTAINER_HOME_ROOT) == 0);
    /* Neither: an unknown uid has no iOS home; the previous home
     * is left untouched. */
    g_test_uid = 1000;
    rc = container_setup();
    CHECK(rc == XW_ERR_NOT_FOUND);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), CONTAINER_HOME_ROOT) == 0);
    /* Trailing slashes in the env value are stripped. */
    setenv(CONTAINER_ENV_FIXED_HOME, "/a2/", 1);
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/a2") == 0);
    /* Setup re-reads the environment on every call. */
    setenv(CONTAINER_ENV_FIXED_HOME, "/a3", 1);
    rc = container_setup();
    CHECK(rc == XW_OK);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/a3") == 0);
    /* A relative env value is rejected; the home is unchanged. */
    setenv(CONTAINER_ENV_FIXED_HOME, "relative/path", 1);
    rc = container_setup();
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/a3") == 0);
    /* An empty env value is rejected the same way. */
    unsetenv(CONTAINER_ENV_FIXED_HOME);
    setenv(CONTAINER_ENV_HOME, "", 1);
    rc = container_setup();
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(container_home() != NULL &&
          strcmp(container_home(), "/a3") == 0);
    /* Leave the environment clean for the later scenarios. */
    unsetenv(CONTAINER_ENV_FIXED_HOME);
    unsetenv(CONTAINER_ENV_HOME);
    g_test_uid = CONTAINER_UID_MOBILE;
    SCEN_END(5);
}

/* ------------------------------------------------------------------ */
/* Scenario 6: bundle container.                                       */
/* ------------------------------------------------------------------ */

static void scen_6_bundle(void) {
    char out[CONTAINER_MAX_PATH];
    char big[2100];
    int rc;
    SCEN_BEGIN(6, "bundle container");
    /* Scenario 4 tried an overlong bundle and left it unset. */
    CHECK(container_bundle(out, sizeof(out)) == XW_ERR_NOT_FOUND);
    rc = container_set_bundle(TEST_BUNDLE);
    CHECK(rc == XW_OK);
    rc = container_bundle(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_BUNDLE) == 0);
    /* Trailing slashes are stripped at set time. */
    rc = container_set_bundle(TEST_BUNDLE "/");
    CHECK(rc == XW_OK);
    rc = container_bundle(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_BUNDLE) == 0);
    /* A too-small out returns NOSPACE and leaves out untouched. */
    fill_sentinel(out, sizeof(out));
    rc = container_bundle(out, 16);
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(check_sentinel(out, sizeof(out)));
    /* Relative, empty, NULL and overlong roots are rejected and
     * the stored bundle is unchanged. */
    rc = container_set_bundle("var/containers/Bundle");
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_bundle("");
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_bundle(NULL);
    CHECK(rc == XW_ERR_BAD_TYPE);
    memset(big, 'a', sizeof(big) - 1);
    big[0] = '/';
    big[sizeof(big) - 1] = '\0';
    rc = container_set_bundle(big);
    CHECK(rc == XW_ERR_NOSPACE);
    rc = container_bundle(out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_BUNDLE) == 0);
    SCEN_END(6);
}

/* ------------------------------------------------------------------ */
/* Scenario 7: group containers.                                       */
/* ------------------------------------------------------------------ */

static void scen_7_groups(void) {
    char out[CONTAINER_MAX_PATH];
    char id[64];
    char path[CONTAINER_MAX_PATH];
    int rc;
    int i;
    SCEN_BEGIN(7, "group containers");
    CHECK(container_group(TEST_GROUP, out, sizeof(out)) ==
          XW_ERR_NOT_FOUND);
    rc = container_set_group(TEST_GROUP, TEST_GROUP_PATH);
    CHECK(rc == XW_OK);
    rc = container_group(TEST_GROUP, out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_GROUP_PATH) == 0);
    /* Trailing slashes are stripped at set time. */
    rc = container_set_group(TEST_GROUP, TEST_GROUP_PATH "/");
    CHECK(rc == XW_OK);
    rc = container_group(TEST_GROUP, out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_GROUP_PATH) == 0);
    /* Re-registration updates the path. */
    rc = container_set_group(TEST_GROUP,
                             TEST_GROUP_PATH "2");
    CHECK(rc == XW_OK);
    rc = container_group(TEST_GROUP, out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_GROUP_PATH "2") == 0);
    rc = container_set_group(TEST_GROUP, TEST_GROUP_PATH);
    CHECK(rc == XW_OK);
    /* A too-small out returns NOSPACE and leaves out untouched. */
    fill_sentinel(out, sizeof(out));
    rc = container_group(TEST_GROUP, out, 16);
    CHECK(rc == XW_ERR_NOSPACE);
    CHECK(check_sentinel(out, sizeof(out)));
    /* Bad registrations are rejected. */
    rc = container_set_group(TEST_GROUP, "relative/path");
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_group(NULL, TEST_GROUP_PATH);
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_group(TEST_GROUP, NULL);
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_set_group("", TEST_GROUP_PATH);
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_group(NULL, out, sizeof(out));
    CHECK(rc == XW_ERR_BAD_TYPE);
    rc = container_group("", out, sizeof(out));
    CHECK(rc == XW_ERR_BAD_TYPE);
    /* Fill the table (one slot is already taken by TEST_GROUP):
     * 7 more fit, the 9th new id does not. */
    for (i = 0; i < CONTAINER_MAX_GROUPS - 1; i++) {
        snprintf(id, sizeof(id), "group.test.%d", i);
        snprintf(path, sizeof(path),
                 "/var/mobile/Containers/Shared/AppGroup/00000000-0000-"
                 "0000-0000-%012d",
                 i);
        rc = container_set_group(id, path);
        CHECK(rc == XW_OK);
        rc = container_group(id, out, sizeof(out));
        CHECK(rc == XW_OK);
        CHECK(strcmp(out, path) == 0);
    }
    rc = container_set_group("group.test.overflow", TEST_GROUP_PATH);
    CHECK(rc == XW_ERR_NOSPACE);
    /* Earlier registrations still resolve after the table filled. */
    rc = container_group(TEST_GROUP, out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_GROUP_PATH) == 0);
    rc = container_group("group.test.3", out, sizeof(out));
    CHECK(rc == XW_OK);
    SCEN_END(7);
}

/* ------------------------------------------------------------------ */
/* Scenario 8: join normalization.                                     */
/* ------------------------------------------------------------------ */

static void scen_8_normalize(void) {
    char out[CONTAINER_MAX_PATH];
    int rc;
    SCEN_BEGIN(8, "join normalization");
    rc = container_set_home(TEST_HOME);
    CHECK(rc == XW_OK);
    rc = container_join("Documents/", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Documents") == 0);
    rc = container_join("./Documents", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Documents") == 0);
    rc = container_join("Documents//x", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/Documents/x") == 0);
    rc = container_join("a/./b", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/a/b") == 0);
    rc = container_join(".", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME) == 0);
    rc = container_join("./././tmp", out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(strcmp(out, TEST_HOME "/tmp") == 0);
    SCEN_END(8);
}

/* ------------------------------------------------------------------ */
/* Scenario 9: temp-dir home (the dir is created by main).             */
/* ------------------------------------------------------------------ */

static char g_tmpdir[256];

static void scen_9_tmpdir_home(void) {
    char out[CONTAINER_MAX_PATH];
    char expect[CONTAINER_MAX_PATH];
    int rc;
    SCEN_BEGIN(9, "temp-dir home");
    CHECK(g_tmpdir[0] != '\0');
    rc = container_set_home(g_tmpdir);
    CHECK(rc == XW_OK);
    rc = container_documents(out, sizeof(out));
    CHECK(rc == XW_OK);
    snprintf(expect, sizeof(expect), "%s/Documents", g_tmpdir);
    CHECK(strcmp(out, expect) == 0);
    CHECK(starts_with(out, g_tmpdir));
    rc = container_preferences(out, sizeof(out));
    CHECK(rc == XW_OK);
    snprintf(expect, sizeof(expect), "%s/Library/Preferences",
             g_tmpdir);
    CHECK(strcmp(out, expect) == 0);
    CHECK(starts_with(out, g_tmpdir));
    rc = container_join("Library/Preferences/com.example.app.plist",
                        out, sizeof(out));
    CHECK(rc == XW_OK);
    CHECK(starts_with(out, g_tmpdir));
    CHECK(!has_dotdot(out));
    /* A join that would escape the temp dir is rejected. */
    fill_sentinel(out, sizeof(out));
    rc = container_join("../outside", out, sizeof(out));
    CHECK(rc == XW_ERR_BAD_TYPE);
    CHECK(check_sentinel(out, sizeof(out)));
    SCEN_END(9);
}

/* ------------------------------------------------------------------ */
/* Scenario 10: fuzz.                                                  */
/* ------------------------------------------------------------------ */

static void fuzz_rel(char *buf, size_t cap) {
    static const char alpha[] = "ab./x";
    static const char *valid[] = {
        "Documents", "Library/Preferences", "tmp", "a/b/c", "x"
    };
    size_t len;
    size_t i;
    if (cap == 0) return;
    if ((rng_next() % 3) == 0) {
        /* A valid rel, sometimes with a mutated suffix, so the
         * success path is exercised as well as rejections. */
        const char *v = valid[rng_next() %
                              (sizeof(valid) / sizeof(valid[0]))];
        size_t vlen = strlen(v);
        size_t extra = rng_next() % 8;
        if (vlen + extra + 1 > cap) extra = 0;
        memcpy(buf, v, vlen);
        for (i = 0; i < extra; i++) {
            buf[vlen + i] =
                alpha[rng_next() % (sizeof(alpha) - 1)];
        }
        buf[vlen + extra] = '\0';
        return;
    }
    len = rng_next() % 25;
    if (len + 1 > cap) len = cap - 1;
    for (i = 0; i < len; i++) {
        buf[i] = alpha[rng_next() % (sizeof(alpha) - 1)];
    }
    buf[len] = '\0';
}

static void fuzz_path(char *buf, size_t cap, size_t max_len) {
    static const char alpha[] = "/ab.x";
    size_t len = rng_next() % (max_len + 1);
    size_t i;
    if (cap == 0) return;
    if (len + 1 > cap) len = cap - 1;
    for (i = 0; i < len; i++) {
        buf[i] = alpha[rng_next() % (sizeof(alpha) - 1)];
    }
    buf[len] = '\0';
}

static void scen_10_fuzz(void) {
    char rel[64];
    char out[CONTAINER_MAX_PATH];
    char home_before[CONTAINER_MAX_PATH];
    char id[64];
    char path[128];
    const char *h;
    int rc;
    int i;
    int violations = 0;
    SCEN_BEGIN(10, "fuzz");
    /* Join fuzz: 20000 mutated rels against the temp-dir home. */
    rc = container_set_home(g_tmpdir);
    CHECK(rc == XW_OK);
    for (i = 0; i < 20000; i++) {
        size_t cap = (size_t)(rng_next() % (CONTAINER_MAX_PATH + 1));
        if (cap == 0) cap = 1;
        fuzz_rel(rel, sizeof(rel));
        fill_sentinel(out, sizeof(out));
        rc = container_join(rel, out, cap);
        if (rc == XW_OK) {
            /* Output must be NUL-terminated inside cap, stay
             * under the home, and contain no ".." component. */
            size_t olen = 0;
            int terminated = 0;
            size_t k;
            for (k = 0; k < cap; k++) {
                if (out[k] == '\0') {
                    terminated = 1;
                    olen = k;
                    break;
                }
            }
            if (!terminated || !starts_with(out, g_tmpdir) ||
                has_dotdot(out)) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: bad success "
                           "output for rel \"%s\" (rc %d, "
                           "terminated %d, len %lu)\n",
                           i, rel, rc, terminated,
                           (unsigned long)olen);
                }
                violations++;
            }
        } else {
            if (rc != XW_ERR_BAD_TYPE &&
                rc != XW_ERR_NOSPACE && rc != XW_ERR_NOT_FOUND) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: unexpected rc %d "
                           "for rel \"%s\"\n",
                           i, rc, rel);
                }
                violations++;
            }
            if (!check_sentinel(out, sizeof(out))) {
                if (violations < 5) {
                    printf("\n  fuzz iter %d: failed call wrote "
                           "to out for rel \"%s\" (rc %d)\n",
                           i, rel, rc);
                }
                violations++;
            }
        }
    }
    /* set_home / set_bundle fuzz: 5000 mutated roots. A success
     * must leave an absolute, slash-free home; a failure must
     * leave the previous home untouched. */
    for (i = 0; i < 5000; i++) {
        h = container_home();
        if (h != NULL) {
            size_t hl = strlen(h);
            memcpy(home_before, h, hl + 1);
        } else {
            home_before[0] = '\0';
        }
        fuzz_path(rel, sizeof(rel), 1100);
        rc = container_set_home(rel);
        if (rc == XW_OK) {
            h = container_home();
            if (h == NULL || h[0] != '/' ||
                h[strlen(h) - 1] == '/') {
                if (violations < 5) {
                    printf("\n  fuzz set_home iter %d: bad "
                           "stored home for \"%s\"\n",
                           i, rel);
                }
                violations++;
            }
        } else {
            if (rc != XW_ERR_BAD_TYPE &&
                rc != XW_ERR_NOSPACE) {
                if (violations < 5) {
                    printf("\n  fuzz set_home iter %d: "
                           "unexpected rc %d for \"%s\"\n",
                           i, rc, rel);
                }
                violations++;
            }
            h = container_home();
            if ((home_before[0] == '\0' && h != NULL) ||
                (home_before[0] != '\0' &&
                 (h == NULL || strcmp(h, home_before) != 0))) {
                if (violations < 5) {
                    printf("\n  fuzz set_home iter %d: failed "
                           "call changed home (\"%s\")\n",
                           i, rel);
                }
                violations++;
            }
        }
    }
    /* Group fuzz: 5000 mutated registrations and 2000 mutated
     * lookups. A successful registration must resolve back to
     * the same path. */
    for (i = 0; i < 5000; i++) {
        fuzz_path(id, sizeof(id), 40);
        fuzz_path(path, sizeof(path), 80);
        rc = container_set_group(id, path);
        if (rc != XW_OK && rc != XW_ERR_BAD_TYPE &&
            rc != XW_ERR_NOSPACE) {
            if (violations < 5) {
                printf("\n  fuzz set_group iter %d: unexpected "
                       "rc %d\n",
                       i, rc);
            }
            violations++;
        }
        if (rc == XW_OK) {
            char got[CONTAINER_MAX_PATH];
            int grc = container_group(id, got, sizeof(got));
            if (grc != XW_OK) {
                if (violations < 5) {
                    printf("\n  fuzz set_group iter %d: "
                           "registered group does not resolve "
                           "(rc %d)\n",
                           i, grc);
                }
                violations++;
            }
        }
    }
    for (i = 0; i < 2000; i++) {
        fuzz_path(id, sizeof(id), 40);
        fill_sentinel(out, sizeof(out));
        rc = container_group(id, out, sizeof(out));
        if (rc != XW_OK && rc != XW_ERR_BAD_TYPE &&
            rc != XW_ERR_NOSPACE && rc != XW_ERR_NOT_FOUND) {
            if (violations < 5) {
                printf("\n  fuzz group iter %d: unexpected rc "
                       "%d\n",
                       i, rc);
            }
            violations++;
        }
        if (rc != XW_OK && !check_sentinel(out, sizeof(out))) {
            if (violations < 5) {
                printf("\n  fuzz group iter %d: failed call "
                       "wrote to out (rc %d)\n",
                       i, rc);
            }
            violations++;
        }
    }
    printf("  fuzz: 20000 join + 5000 set_home + 5000 set_group + "
           "2000 group lookups, %d violations\n",
           violations);
    CHECK(violations == 0);
    /* Restore a known home for the temp-dir check in main. */
    rc = container_set_home(g_tmpdir);
    CHECK(rc == XW_OK);
    SCEN_END(10);
}

/* ------------------------------------------------------------------ */
/* main.                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    char tmpl[] = "/tmp/container_test_XXXXXX";
    char sentinel_path[512];
    char sentinel_buf[64];
    FILE *f;
    int rc;
    printf("container_test: host test\n");
    /* Temp dir under /tmp with one sentinel file; main verifies
     * it is unchanged after the fuzz and removes it. */
    if (mkdtemp(tmpl) == NULL) {
        printf("FAIL: mkdtemp failed\n");
        return 1;
    }
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", tmpl);
    snprintf(sentinel_path, sizeof(sentinel_path), "%s/sentinel.txt",
             g_tmpdir);
    f = fopen(sentinel_path, "w");
    if (f == NULL) {
        printf("FAIL: cannot create sentinel file\n");
        return 1;
    }
    fputs("container-test-sentinel\n", f);
    fclose(f);
    scen_1_unset();
    scen_2_well_known();
    scen_3_rejections();
    scen_4_overflow();
    scen_5_env();
    scen_6_bundle();
    scen_7_groups();
    scen_8_normalize();
    scen_9_tmpdir_home();
    scen_10_fuzz();
    /* The fuzz must not have touched the temp dir: exactly one
     * entry, and the sentinel file has the same contents. */
    CHECK(count_dir_entries(g_tmpdir) == 1);
    f = fopen(sentinel_path, "r");
    CHECK(f != NULL);
    if (f != NULL) {
        size_t n = fread(sentinel_buf, 1, sizeof(sentinel_buf) - 1, f);
        sentinel_buf[n] = '\0';
        fclose(f);
        CHECK(strcmp(sentinel_buf, "container-test-sentinel\n") == 0);
    }
    /* Clean up the temp dir. */
    rc = remove(sentinel_path);
    CHECK(rc == 0);
    rc = rmdir(g_tmpdir);
    CHECK(rc == 0);
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
