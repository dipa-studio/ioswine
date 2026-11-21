/*
 * container.h: app container layout and path translation for ioswine.
 *
 * The data container root is HOME. Every path the app builds goes
 * through container_join and stays under HOME; the bundle container
 * is a separate, read-only root; group containers are keyed by group
 * id from the com.apple.security.application-groups entitlement.
 *
 * Resolution order: CFFIXED_USER_HOME first,
 * then HOME, then the uid home (/var/mobile for uid 501, /var/root
 * for uid 0). Both env vars are set to the data container path at
 * launch. container_setup() performs this resolution; the launcher
 * calls it once at startup, after setting HOME, CFFIXED_USER_HOME
 * and TMPDIR in the environment. container_home() returns NULL
 * until container_setup() or container_set_home() has run.
 *
 * Design decisions:
 *
 * 1. container_join is the sandbox-enforcement point: an absolute rel
 *    and any ".." component are rejected with XW_ERR_BAD_TYPE. "."
 *    and empty components (from "//") are skipped, so the result is
 *    normalized, and the result never has a trailing slash. A rel of
 *    "." yields HOME itself.
 * 2. container_tmp() always returns <HOME>/tmp and never reads
 *    TMPDIR. TMPDIR is <container>/tmp/ at launch anyway,
 *    so reading it could only let a path escape HOME.
 * 3. The bundle root is set by the launcher via
 *    container_set_bundle() (derived from argv[0] / the dyld image
 *    path, never from HOME). There is no env var for it.
 *    container_bundle() returns XW_ERR_NOT_FOUND until it is set.
 * 4. Group containers are pre-registered by the launcher via the
 *    additive container_set_group() (the launcher resolves a group
 *    id to a path with its own metadata-plist scan of
 *    Shared/AppGroup/). container.h does
 *    no scan. container_group() returns XW_ERR_NOT_FOUND for an
 *    unregistered group id.
 * 5. No mkdir anywhere in this file. container.h is pure path
 *    translation. On real iOS the installer creates the container
 *    directories; in ioswine the launcher plays installd's role and
 *    owns the fake filesystem tree, and any writing component creates a
 *    missing parent on first write without letting an absolute iOS path
 *    reach the Android mkdirat before the fake root is in place.
 * 6. Stored roots are normalized: trailing '/' characters are
 *    stripped at set time, so every path function returns an exact
 *    string with no trailing slash.
 * 7. A failed container_setup() (unknown uid, overlong or relative
 *    env value) leaves the previous home untouched.
 * 8. container.h has no service and no wire strings, so it owns no
 *    *_names.h file. The well-known relative paths live in the
 *    CONTAINER_REL_* defines below (the single table); no path
 *    literal appears in container.c.
 *
 * Dependencies: lsk_getenv (declared in ipc.h, defined in init.c)
 * and lsk_getuid (defined in bsd.c as asm symbol _getuid; declared
 * here in container.c following the ipc.c/kevent.c asm-label
 * convention). No other libsystem_kernel symbol is used.
 *
 * All functions return 0 or a negative XW_ERR_* (xpcwire.h):
 * XW_ERR_NOSPACE when out is too small (out is left untouched, no
 * truncated path is ever written), XW_ERR_NOT_FOUND when the home,
 * bundle or group is not set/registered, XW_ERR_BAD_TYPE for a
 * malformed argument (NULL, empty, relative root, absolute or ".."
 * rel). container_home() returns NULL until set.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"

/* Limits. Container paths are long (two UUIDs, about 90 chars
 * before the subpath); 1024 leaves ample room for any subpath
 *. */
#define CONTAINER_MAX_PATH 1024     /* static home/bundle/group buffers */
#define CONTAINER_MAX_GROUP_ID 128  /* longest group id we register */
#define CONTAINER_MAX_GROUPS 8      /* pre-registered group containers */

/* Uids with a known home. */
#define CONTAINER_UID_MOBILE 501
#define CONTAINER_UID_ROOT 0

/* Environment variable names, in resolution order. */
#define CONTAINER_ENV_FIXED_HOME "CFFIXED_USER_HOME"
#define CONTAINER_ENV_HOME "HOME"

/* Uid-home fallback paths. */
#define CONTAINER_HOME_MOBILE "/var/mobile"
#define CONTAINER_HOME_ROOT "/var/root"

/* Well-known relative paths inside the data container
 *. Single table: no path literal
 * appears in container.c. */
#define CONTAINER_REL_DOCUMENTS "Documents"
#define CONTAINER_REL_LIBRARY "Library"
#define CONTAINER_REL_PREFERENCES "Library/Preferences"
#define CONTAINER_REL_CACHES "Library/Caches"
#define CONTAINER_REL_APP_SUPPORT "Library/Application Support"
#define CONTAINER_REL_TMP "tmp"

/* Set the data container root explicitly, overriding any value
 * from container_setup(). Copies into a static buffer; the
 * launcher or the test harness calls this. The path must be
 * absolute, non-empty and fit in CONTAINER_MAX_PATH; trailing '/'
 * characters are stripped. */
int container_set_home(const char *home);

/* The data container root, or NULL until container_setup() or
 * container_set_home() has run. Do not free or modify the
 * returned string. */
const char *container_home(void);

/* Resolve the home from the environment in the sourced order:
 * CFFIXED_USER_HOME, then HOME, then the uid home
 * (CONTAINER_UID_MOBILE -> /var/mobile, CONTAINER_UID_ROOT ->
 * /var/root). Any other uid has no iOS home and returns
 * XW_ERR_NOT_FOUND. Re-reads the environment on every call, so
 * calling it again after the environment changes re-resolves. A
 * failed call leaves the previous home untouched. The launcher
 * calls this once at startup; it is NOT called from
 * daemons_init() (container has no service). */
int container_setup(void);

/* <HOME>/<rel> into out. rel uses '/' separators, no leading '/'.
 * An absolute rel, an empty rel, and any ".." component are
 * rejected with XW_ERR_BAD_TYPE; "." and empty components are
 * skipped. Returns XW_ERR_NOT_FOUND when the home is not set. */
int container_join(const char *rel, char *out, size_t cap);

/* Well-known directories under HOME (all via container_join). */
int container_documents(char *out, size_t cap);    /* Documents */
int container_library(char *out, size_t cap);      /* Library */
int container_preferences(char *out, size_t cap);  /* Library/Preferences */
int container_caches(char *out, size_t cap);       /* Library/Caches */
int container_tmp(char *out, size_t cap);          /* tmp (TMPDIR ignored) */

/* Bundle container root.
 * The launcher sets it from the executable path; the bundle
 * container is read-only and separate from the data container.
 * Same absolute-path and trailing-slash rules as
 * container_set_home(). */
int container_set_bundle(const char *bundle_path);
int container_bundle(char *out, size_t cap);

/* Group container for group_id (additive, from container.md
 * section 5-7). Groups are pre-registered by the launcher with
 * container_set_group(); container_group() looks the id up there
 * and returns XW_ERR_NOT_FOUND for an unregistered id. The group
 * container is outside HOME, so it is never built by joining a
 * group id onto HOME. */
int container_set_group(const char *group_id, const char *path);
int container_group(const char *group_id, char *out, size_t cap);
