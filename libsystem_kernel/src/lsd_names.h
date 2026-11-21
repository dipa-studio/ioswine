/*
 * lsd_names.h: the ONE wire-string table for the fake LaunchServices
 * daemon.
 *
 * Every service name, request key, reply key, record key and record
 * value the lsd service sends or reads lives here and nowhere else,
 * so a wrong string can be corrected after the first device trace
 * without touching any logic. No logic lives in this file, only
 * #defines.
 *
 * Tag on each entry: [src: ...] means the string (or the value) was
 * read from that public source.
 * [guess] means inference, not verified; the string is our best
 * prediction of the wire form and must be checked against the first
 * device trace. Where a name is a
 * property name from a header but its use as a wire key is unproven,
 * both are noted.
 *
 * The request keys, operation codes and reply shape in this file are
 * PLACEHOLDERS: no
 * published trace gives the real LS wire format, and the raw-dict
 * vs NSXPC question is unresolved. They exist so the host test can
 * drive the service and the trace correction is a small diff: change
 * the values here, not the logic in lsd.c. Do not present this shape
 * as the real protocol in any doc or comment.
 *
 * No em dashes are used in this file.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Mach service names.                                                 */
/* ------------------------------------------------------------------ */

/* [src: https://newosxbook.com/ent.php?osVer=iOS18&exec=MobileSMS]
 * (MobileSMS on iOS 18 carries a mach-lookup exception for it). The
 * unified XPC interface; the ObjC client API (LSApplicationWorkspace,
 * LSApplicationProxy, LSBundleRecord) talks here. Registered. */
#define LSD_SVC_XPC "com.apple.lsd.xpc"
/* [src: https://chromium.googlesource.com/external/github.com/WebKit/webkit/+/120815f59a5740c4a4cbc6212673c5ff6f813c55%5E%21/]
 * (WebKit iOS WebContent sandbox profile allows mach-lookup for it).
 * Low-level read (map) of the LaunchServices database. Registered;
 * answers the same record queries as the xpc service. */
#define LSD_SVC_MAPDB "com.apple.lsd.mapdb"
/* [guess] extrapolated from the macOS launchd plist
 * [src: http://objective-see.org/blog/blog_0x12.html]. Low-level
 * write (modify) of the database. Registered; accepts registration
 * messages and answers success with no state change. */
#define LSD_SVC_MODIFYDB "com.apple.lsd.modifydb"

/* The other names from the macOS plist are deliberately NOT
 * registered: a lookup for one of them fails fast at the launchd
 * shim instead of hanging, and any request for one of those
 * functions arriving on the xpc service is logged verbatim and
 * answered with an error (see the LSD_OP_OPEN_URL / LSD_OP_ICONS /
 * LSD_OP_AD_ID operations below). lsd cannot observe a
 * bootstrap_look_up call directly, so the first device trace shows
 * whether anything looks these names up by whether the client asks
 * for the function on the xpc service. */
/* [guess] macOS plist names. */
#define LSD_SVC_OPEN "com.apple.lsd.open"
#define LSD_SVC_OPENURL "com.apple.lsd.openurl"
#define LSD_SVC_PLUGIN "com.apple.lsd.plugin"
#define LSD_SVC_ICONS "com.apple.lsd.icons"
#define LSD_SVC_AD_ID "com.apple.lsd.advertisingidentifiers"
/* macOS 11 additionally lists these two
 * [src: https://f5.pm/go-182437.html]; not registered either. */
#define LSD_SVC_DIAGNOSTICS "com.apple.lsd.diagnostics"
#define LSD_SVC_DISSEMINATION "com.apple.lsd.dissemination"

/* Number of services lsd_init registers, in the order xpc, mapdb,
 * modifydb. */
#define LSD_NUM_SERVICES 3

/* ------------------------------------------------------------------ */
/* Request keys (all [guess] placeholders, see the file header).       */
/* ------------------------------------------------------------------ */

/* Operation code key. */
#define LSD_KEY_OPERATION "operation"
/* Bundle identifier the request is about. */
#define LSD_KEY_BUNDLE_ID "bundleIdentifier"
/* URL scheme for a handler lookup (bare scheme, e.g. "aidoku"). */
#define LSD_KEY_SCHEME "scheme"
/* Full URL for a handler lookup (e.g. "aidoku://open"); the scheme
 * is the part before ':'. */
#define LSD_KEY_URL "URL"
/* Requested record keys (accepted, currently ignored: the fake
 * always returns the full minimal record). */
#define LSD_KEY_KEYS "keys"

/* ------------------------------------------------------------------ */
/* Reply keys (all [guess] placeholders, see the file header).         */
/* ------------------------------------------------------------------ */

/* The svchost handler key: every reply carries "error"
 * (0 on success). Same spelling as DAEMON_KEY_ERROR in
 * daemon_names.h; duplicated here to keep this header self-contained. */
#define LSD_KEY_ERROR "error"
/* List replies (allApplications, plug-ins) carry the array here. */
#define LSD_KEY_RECORDS "records"
/* openApplicationWithBundleID answer: false. */
#define LSD_KEY_SUCCESS "success"
/* Nested entitlements dictionary in a record reply. */
#define LSD_KEY_ENTITLEMENTS "entitlements"
/* Signer team identifier in a record reply, read from the
 * entitlements plist key below when csops info is available. */
#define LSD_KEY_TEAM_ID "teamID"

/* Entitlements plist key holding the team identifier. Property name
 * from the entitlements format; its presence in this app's
 * entitlements is [guess]. */
#define LSD_ENT_TEAM_ID "com.apple.developer.team-identifier"

/* ------------------------------------------------------------------ */
/* Operation codes (all [guess] placeholders, see the file header).    */
/* One dispatch table, one place: lsd.c switches on these values.      */
/* ------------------------------------------------------------------ */

#define LSD_OP_RECORD 1             /* own-bundle record lookup */
#define LSD_OP_ALL_APPLICATIONS 2   /* allApplications listing */
#define LSD_OP_HANDLER_FOR_SCHEME 3 /* URL-scheme handler lookup */
#define LSD_OP_PLUGINS 4            /* plug-in records for a bundle */
#define LSD_OP_REGISTER 5           /* registerItemInfo (accepted, ignored) */
#define LSD_OP_OPEN 6               /* openApplicationWithBundleID */
#define LSD_OP_OBSERVE_DB 7         /* database-change observation */
#define LSD_OP_OPEN_URL 8           /* openurl function: logged, error only */
#define LSD_OP_ICONS 9              /* icons function: logged, error only */
#define LSD_OP_AD_ID 10             /* advertisingidentifiers: logged, error */

/* ------------------------------------------------------------------ */
/* Minimal bundle record: the running app (Aidoku 0.9-4).              */
/* Values marked [src] are sourced; the rest are [guess] or read from */
/* the app's own Info.plist at startup in a later revision.           */
/* ------------------------------------------------------------------ */

/* [src: https://raw.githubusercontent.com/Aidoku/Aidoku/altstore/apps.json] */
#define LSD_APP_BUNDLE_ID "app.aidoku.Aidoku"
/* Display name / CFBundleName / CFBundleExecutable. Read from the
 * app's Info.plist at startup in a later revision [guess]. */
#define LSD_APP_NAME "Aidoku"
/* [src: Aidoku AltStore source] (version "0.9.4"). The build number
 * (CFBundleVersion) is unknown; it is the short version string
 * until the loader reads Info.plist [guess]. */
#define LSD_APP_SHORT_VERSION "0.9.4"
#define LSD_APP_VERSION "0.9.4"
/* [src: https://github.com/arutkayb/aidoku-lingo/commit/9997efae090aa28d15d0691232b857e3c5e815d5]
 * (URL scheme "aidoku" in the rebrand commit). */
#define LSD_APP_URL_SCHEME "aidoku"

/* Record field keys. The key spellings are the LSBundleRecord /
 * LSApplicationRecord property names
 * [src: https://www.research.macpaw.com/articles/installed-applications-on-ios/];
 * their use as wire keys is [guess].
 *
 * Reply budget trim (2026-11-12, measured): the full record
 * does NOT fit in 1000 bytes of body, so only the keys below
 * are sent. Dropped: CFBundleName,
 * CFBundleExecutable, MinimumOSVersion, applicationType,
 * isPlaceholder, LSApplicationHasRegistered,
 * LSApplicationReadyToBeFrontableKey, UIDeviceFamily. A dropped key
 * reads as nil/false on the client; see lsd.c for the measured
 * sizes. */
#define LSD_REC_BUNDLE_URL "bundleURL"
#define LSD_REC_DATA_CONTAINER_URL "dataContainerURL"
#define LSD_REC_BUNDLE_CONTAINER_URL "bundleContainerURL"
#define LSD_REC_LOCALIZED_NAME "localizedName"
#define LSD_REC_CF_BUNDLE_SHORT_VERSION "CFBundleShortVersionString"
#define LSD_REC_CF_BUNDLE_VERSION "CFBundleVersion"
#define LSD_REC_IS_INSTALLED "isInstalled"
#define LSD_REC_IS_LAUNCHABLE "isLaunchable"
/* [src: Apple Launch Services Concepts] (CFBundleURLTypes /
 * CFBundleDocumentTypes are the Info.plist keys LS reads). */
#define LSD_REC_URL_TYPES "CFBundleURLTypes"
#define LSD_REC_URL_NAME "CFBundleURLName"
#define LSD_REC_URL_SCHEMES "CFBundleURLSchemes"

/* URL construction. The bundle URL is "file://" + bundle container
 * root + "/Aidoku.app/"; the container URLs are "file://" + root +
 * "/". The container roots come from container.h at reply time, so
 * the two containers keep whatever UUIDs container.c assigns (they
 * must differ). */
#define LSD_URL_FILE_PREFIX "file://"
#define LSD_URL_SLASH "/"
#define LSD_URL_APP_DIR_SUFFIX "/Aidoku.app/"

/* Verbatim trace prefix (bsxpc_log_dict) for incoming messages. */
#define LSD_TRACE_PREFIX "[lsd]"
