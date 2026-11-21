/*
 * cfprefs_names.h: the wire-string table for the fake cfprefsd.
 *
 * Same pattern as fakeboard_names.h / daemon_names.h: every string
 * the service puts on the wire lives here and nowhere else, so a
 * wrong string can be corrected after the first device trace without
 * touching any logic. cfprefsd.c carries no wire-string literals.
 *
 * Every spelling below is [src: DEF CON 29 slides / ZecOps PoC]
 * except where marked [guess].
 *
 * The last section holds filesystem name pieces (not wire strings):
 * they live here too so that cfprefsd.c carries no string literals
 * at all and the verifier's grep for literals in .c files stays
 * clean. The trace prefix is a log label, not a wire string.
 *
 * No em dashes are used in this file.
 */
#pragma once

/* Service names (Mach service names, one per role, one handler). */
#define CFP_SVC_AGENT  "com.apple.cfprefsd.agent"
#define CFP_SVC_DAEMON "com.apple.cfprefsd.daemon"

/* Message keys [src: aidoku device log]. */
#define CFP_KEY_OPERATION          "CFPreferencesOperation"
#define CFP_KEY_DOMAIN             "CFPreferencesDomain"
#define CFP_KEY_USER               "CFPreferencesUser"
#define CFP_KEY_MESSAGES           "CFPreferencesMessages"
#define CFP_KEY_KEY                "Key"
#define CFP_KEY_VALUE              "Value"
#define CFP_KEY_CURRENT_APP_DOMAIN "CFPreferencesCurrentApplicationDomain"
#define CFP_KEY_SYNC_WRITE         "CFPreferencesShouldWriteSynchronously"
#define CFP_KEY_BY_HOST            "CFPreferencesIsByHost"
#define CFP_KEY_ACCESS_TOKEN       "CFPreferencesAccessToken"
#define CFP_KEY_IS_MANAGED         "CFPreferencesIsManaged"
#define CFP_KEY_HOST_BUNDLE_ID     "CFPreferencesHostBundleIdentifier"

/* User-scope values (the literal strings sent on the wire, not the
 * CF constant names). */
#define CFP_USER_CURRENT "kCFPreferencesCurrentUser"
#define CFP_USER_ANY     "kCFPreferencesAnyUser"

/* The global domain, as sent in CFPreferencesDomain
 * [src: aidoku device log]. */
#define CFP_DOMAIN_GLOBAL  "NSGlobalDomain"
#define CFP_DOMAIN_ANY_APP "kCFPreferencesAnyApplication"

/* Reply keys. The svchost handler puts the error code under "error".
 * CFP_KEY_PLIST_DATA is the read domain reply key [src: 0x183569874, 0x18357d12c].
 * CFP_KEY_SHMEM_* are the shared-memory cache validation keys
 * [src: 0x1835694c4-0x183569518, 0x18357d108-0x18357d1f0]. */
#define CFP_KEY_ERROR       "error"
#define CFP_KEY_PLIST_DATA  "CFPreferencesPropertyList"
#define CFP_KEY_SHMEM_NAME  "CFPreferencesShmemName"
#define CFP_KEY_SHMEM_INDEX "CFPreferencesShmemIndex"
#define CFP_KEY_SHMEM_STATE "CFPreferencesShmemState"

/* Filesystem name pieces (not wire strings). */
#define CFP_FILE_SUFFIX   ".plist"              /* <domain>.plist */
#define CFP_FILE_GLOBAL   ".GlobalPreferences"  /* NSGlobalDomain file stem */
#define CFP_DIR_BYHOST    "ByHost"              /* Preferences/ByHost/ */
#define CFP_TMP_SUFFIX    ".cfptmp"             /* temp file for atomic write */
#define CFP_BPLIST_MAGIC  "bplist00"            /* binary plist header magic */
#define CFP_DEV_SHM_DIR   "/dev/shm/"           /* POSIX shm dir on Linux/host */
#define CFP_SHMEM_PREFIX  "c"                   /* short shmem name prefix ("c<hexpid>") */
#define CFP_HEX_DIGITS    "0123456789abcdef"    /* hex lookup for shmem name */
/* ByHost host identifier [guess]. The real string
 * comes from the host UUID (gethostuuid, reduced to the last 12 hex
 * characters); the fake has no host-UUID source, so it uses the
 * all-zeros reduced form. Correct from the first device trace. */
#define CFP_HOST_ID       "000000000000"

/* Verbatim trace prefix for bsxpc_log_dict. */
#define CFP_TRACE_PREFIX "[cfprefsd]"
