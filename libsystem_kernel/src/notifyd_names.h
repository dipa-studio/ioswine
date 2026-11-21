/*
 * notifyd_names.h: the wire-string table for notifyd,
 * plus the numeric protocol constants from the
 * notify_ipc.defs subsystem (routine ids, status codes, client
 * types, access bits). No notifyd logic file carries a wire string
 * or a protocol number as a literal; everything lives here, so a
 * wrong string or number can be corrected after the first device
 * trace without touching any logic.
 *
 * Sources: the Apple open-source Libnotify drop (notify_ipc.defs,
 * libnotify.c, notifyd.c, notify_internal.h, service.h,
 * com.apple.notifyd.plist, notify.conf.iPhone). Every entry is
 * tagged [src:] (verified against that drop) or [guess] (inferred,
 * unconfirmed until the first device trace).
 *
 * No em dashes are used in this file.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Wire strings.                                                      */
/* ------------------------------------------------------------------ */

/* [src: libnotify.h] (NOTIFY_SERVICE_NAME). The Mach service name
 * notifyd registers with launchd. com.apple.notifyd is only the
 * launchd job label (see NOTIFYD_JOB_LABEL below); it is never
 * registered as a service name. */
#define NOTIFYD_SERVICE_NAME "com.apple.system.notification_center"

/* [src: libnotify.c] (NOTIFY_SHM_ID). The shared-memory segment id
 * for the register_check memory path. Out of scope for this delivery
 * (the server-check fallback is forced); listed here so the id is
 * recorded for when the page is wired up. */
#define NOTIFYD_SHM_ID "apple.shm.notification_center"

/* [src: notifyd.c] (notifyd_init). The name whose 64-bit state is
 * ((uint64_t)server_pid << 32) | NOTIFY_IPC_VERSION. */
#define NOTIFYD_IPC_VERSION_NAME "com.apple.system.notify.ipc_version"

/* [src: service.h] (SERVICE_PREFIX). Prefix for per-service
 * registrations made by the daemon itself. */
#define NOTIFYD_SERVICE_PREFIX "com.apple.system.notify.service."

/* [src: notifyd.c] (XPC_EVENT_PUBLISHER_NAME). The XPC event
 * publisher name used by notifyd_matching_register. XPC event
 * delivery is not wired in this delivery (G6); the name is
 * recorded here for when it is wired. */
#define NOTIFYD_XPC_PUBLISHER_NAME "com.apple.notifyd.matching"

/* [src: libnotify.h] (NOTIFY_KEY_NOTIFICATION / NOTIFY_KEY_STATE).
 * Keys of the XPC event dict notifyd publishes for XPC_EVENT
 * clients. Unused in this delivery (no publisher wired, G6). */
#define NOTIFYD_XPC_KEY_NOTIFICATION "Notification"
#define NOTIFYD_XPC_KEY_STATE "_State"

/* [src: com.apple.notifyd.plist] (Label). The launchd job label.
 * NOT a service name. Listed here so nobody registers it. */
#define NOTIFYD_JOB_LABEL "com.apple.notifyd"

/* [src: notify_client.c]. The entitlement the client-side code
 * checks for loopback (same-process) registrations. The fake has
 * no entitlement source (G7), so this is recorded, not enforced. */
#define NOTIFYD_LOOPBACK_ENTITLEMENT \
    "com.apple.developer.web-browser-engine.restrict.notifyd"

/* [src: notify_internal.h]. Prefix of names whose posts require
 * the matching restrict-post entitlement. The fake has no
 * entitlement source (G7), so posts to these names are always
 * denied. */
#define NOTIFYD_RESTRICT_PREFIX "com.apple.private.restrict-post."

/* [src: notify_internal.h]. Prefix of per-user names; only the
 * named uid (or a subtree name) may read or write them. */
#define NOTIFYD_USER_UID_PREFIX "user.uid."

/* Trace prefix for bsxpc_log_dict ("[svcname]" prefix). */
#define NOTIFYD_TRACE_PREFIX "[notifyd]"

/* Trace dict keys (built by notifyd.c, logged via
 * bsxpc_log_dict when IOSWINE_SVC_TRACE=1). These keys are
 * diagnostic only; they never appear on the wire. */
#define NOTIFYD_TRACE_KEY_ID "id"
#define NOTIFYD_TRACE_KEY_NAME "name"
#define NOTIFYD_TRACE_KEY_TOKEN "token"
#define NOTIFYD_TRACE_KEY_SIZE "size"
#define NOTIFYD_TRACE_KEY_STATUS "status"
#define NOTIFYD_TRACE_KEY_RESULT "result"

/* Bootstrap register (launchd routine 0xcc) dict keys, as built
 * by notifyd_init (same shape as svchost's sh_bootstrap_register).
 * [src: svchost.c]. */
#define NOTIFYD_BOOTSTRAP_KEY_TYPE "type"
#define NOTIFYD_BOOTSTRAP_KEY_HANDLE "handle"
#define NOTIFYD_BOOTSTRAP_KEY_NAME "name"
#define NOTIFYD_BOOTSTRAP_KEY_PORT "port"
#define NOTIFYD_BOOTSTRAP_KEY_FLAGS "flags"

/* Pre-seed names from the shipped config, notify.conf.iPhone
 * (notifyd_init). [src: notify.conf.iPhone]. */
#define NOTIFYD_CFG_SYSTEM_PREFIX "com.apple.system."
#define NOTIFYD_CFG_CLOCK_SET "com.apple.system.clock_set"
#define NOTIFYD_CFG_TIMEZONE "com.apple.system.timezone"
#define NOTIFYD_CFG_TIMEZONE_PATH "/var/db/timezone/localtime"
#define NOTIFYD_CFG_BATTERYSAVERMODE "com.apple.system.batterysavermode"
#define NOTIFYD_CFG_BATTERYSAVERMODE_DISC \
    "com.apple.system.batterysavermode.discretionary"
#define NOTIFYD_CFG_LOWPOWERMODE "com.apple.system.lowpowermode"
#define NOTIFYD_CFG_LOWPOWERMODE_DISC \
    "com.apple.system.lowpowermode.discretionary"

/* ------------------------------------------------------------------ */
/* Numeric protocol constants (notify_ipc.defs).                       */
/* ------------------------------------------------------------------ */

/* [src: notify_ipc.defs] (subsystem notify_ipc 1000). */
#define NOTIFYD_SUBSYSTEM_BASE 1000

/* Routine ids. [src: notify_ipc.defs, declaration order]. Ids 1000
 * and 1001 are skips in the .defs (no routines). */
#define NOTIFYD_CHECK 1002
#define NOTIFYD_GET_STATE 1003
#define NOTIFYD_SUSPEND 1004
#define NOTIFYD_RESUME 1005
#define NOTIFYD_SUSPEND_PID 1006
#define NOTIFYD_RESUME_PID 1007
#define NOTIFYD_POST_2 1008
#define NOTIFYD_POST_3 1009
#define NOTIFYD_POST_4 1010
#define NOTIFYD_REGISTER_PLAIN_2 1011
#define NOTIFYD_REGISTER_CHECK_2 1012
#define NOTIFYD_REGISTER_SIGNAL_2 1013
#define NOTIFYD_REGISTER_FILE_DESCRIPTOR_2 1014
#define NOTIFYD_REGISTER_MACH_PORT_2 1015
#define NOTIFYD_CANCEL_2 1016
#define NOTIFYD_GET_STATE_2 1017
#define NOTIFYD_GET_STATE_3 1018
#define NOTIFYD_SET_STATE_2 1019
#define NOTIFYD_SET_STATE_3 1020
#define NOTIFYD_MONITOR_FILE_2 1021
#define NOTIFYD_REGENERATE 1022
#define NOTIFYD_CHECKIN 1023
#define NOTIFYD_DUMP 1024
#define NOTIFYD_GENERATE_COMMON_PORT 1025
#define NOTIFYD_REGISTER_COMMON_PORT 1026
#define NOTIFYD_REGISTER_MACH_PORT_3 1027
#define NOTIFYD_FILTERED_CHECKIN 1028
#define NOTIFYD_FILTERED_POST 1029
#define NOTIFYD_FILTERED_REGENERATE 1030
#define NOTIFYD_FILTERED_SET_STATE_2 1031
#define NOTIFYD_FILTERED_SET_STATE_3 1032
#define NOTIFYD_ROUTINE_FIRST 1002
#define NOTIFYD_ROUTINE_LAST 1032

/* [guess] The standard MIG rule: the
 * reply msgh_id is the request id plus 100. Unconfirmed until the
 * first device trace. */
#define NOTIFYD_REPLY_ID_OFFSET 100

/* [src: notify_internal.h] (NOTIFY_IPC_VERSION). The protocol
 * version the client and server agree on (checkin). */
#define NOTIFYD_IPC_VERSION 3
#define NOTIFYD_IPC_VERSION_MIN 3

/* Status codes. [src: notify.h] (notify_status_t). */
#define NOTIFY_STATUS_OK 0
#define NOTIFY_STATUS_INVALID_NAME 1
#define NOTIFY_STATUS_INVALID_TOKEN 2
#define NOTIFY_STATUS_INVALID_REQUEST 6
#define NOTIFY_STATUS_NOT_AUTHORIZED 7
#define NOTIFY_STATUS_DUPLICATE_TOKEN 8
#define NOTIFY_STATUS_INVALID_TIME 9
#define NOTIFY_STATUS_NEW_CLIENT_FAILED 48
#define NOTIFY_STATUS_DUP_CLIENT 56
#define NOTIFY_STATUS_INVALID_PORT_INTERNAL 59
#define NOTIFY_STATUS_CLIENT_NOT_FOUND 50
#define NOTIFY_STATUS_NO_NID 60
#define NOTIFY_STATUS_TYPE_ISSUE 52
#define NOTIFY_STATUS_SERVICE_UNAVAILABLE 1000000

/* Client types. [src: notify_internal.h] (notify_type_t). */
#define NOTIFY_TYPE_NONE 0
#define NOTIFY_TYPE_MEMORY 1
#define NOTIFY_TYPE_PLAIN 2
#define NOTIFY_TYPE_SIGNAL 3
#define NOTIFY_TYPE_FILE 4
#define NOTIFY_TYPE_PORT 5
#define NOTIFY_TYPE_COMMON_PORT 6
#define NOTIFY_TYPE_XPC_EVENT 7

/* Access bits and shifts. [src: notify_internal.h]
 * (NOTIFY_ACCESS_DEFAULT, NOTIFY_ACCESS_READ/WRITE,
 * NOTIFY_ACCESS_USER/GROUP/OTHER_SHIFT). */
#define NOTIFYD_ACCESS_DEFAULT 0x333
#define NOTIFYD_ACCESS_READ 1
#define NOTIFYD_ACCESS_WRITE 2
#define NOTIFYD_ACCESS_USER_SHIFT 0
#define NOTIFYD_ACCESS_GROUP_SHIFT 4
#define NOTIFYD_ACCESS_OTHER_SHIFT 8

/* Access masks for the pre-seeded reserve lines, parsed from the
 * six-character mode strings in notify.conf.iPhone.
 * "rwr-r-"  -> user RW (3), group R (1), other R (1) = 0x113.
 * "rwrwr-"  -> user RW (3), group RW (3), other R (1) = 0x133.
 * [src: notify.conf.iPhone]. */
#define NOTIFYD_ACCESS_SYSTEM_PREFIX 0x113
#define NOTIFYD_ACCESS_CLOCK_SET 0x133

/* [src: notify_internal.h] (NOTIFY_SLOT_NONE). The slot value a
 * plain client gets (no shared-memory slot). */
#define NOTIFYD_SLOT_NONE 0xffffffffU

/* [src: notifyd.c] (NOTIFYD_PATH_MAX). Longest monitor path the
 * server accepts (including the NUL). */
#define NOTIFYD_PATH_MAX 1024

/* Queue limits (the server port is created with qlimit 32;
 * generated ports with qlimit 16). These are internal choices,
 * recorded here so the ipc patch and notifyd.c agree. */
#define NOTIFYD_SERVER_QLIMIT 32
#define NOTIFYD_GEN_PORT_QLIMIT 16

/* NDR record: the only representation the fake accepts.
 * [src: notify_ipc.defs] (the .defs NDR is int_rep 1, char_rep 0,
 * float_rep 0). Anything else gets MIG_BAD_ARGUMENTS. */
#define NOTIFYD_NDR_INT_REP 1
#define NOTIFYD_NDR_CHAR_REP 0
#define NOTIFYD_NDR_FLOAT_REP 0

/* MIG error codes (the reply's RetCode). [src: mach/mig_errors.h]. */
#define NOTIFYD_MIG_BAD_ARGUMENTS (-300)
#define NOTIFYD_MIG_BAD_ID (-301)
#define NOTIFYD_MIG_TYPE_ERROR (-302)

/* Launchd bootstrap port and request-id base for the register
 * message notifyd_init sends. [src: svchost.h] (SVCHOST_BOOTSTRAP
 * PORT 0x203, SVCHOST_REQUEST_MSGH_ID 0x10000000); the values are
 * the same here because both services register with the same
 * launchd bootstrap port. */
#define NOTIFYD_BOOTSTRAP_PORT 0x203U
#define NOTIFYD_REQUEST_MSGH_ID 0x10000000U
