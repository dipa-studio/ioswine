/*
 * notifyd.h: the notifyd MIG server for ioswine.
 *
 * notifyd is the Darwin notification center daemon. It is a MIG
 * server on the Mach service name com.apple.system.notification_center
 * (subsystem notify_ipc 1000, routines 1002-1032), NOT an XPC or
 * svchost service. The ipc.c patch (notifyd-ipc.patch) routes
 * messages for its port to notifyd_process_msg; this file is the
 * server itself.
 *
 * Sources: the Apple open-source Libnotify drop (notify_ipc.defs,
 * libnotify.c, notifyd.c, notify_internal.h, service.h, notify.conf.iPhone).
 * Every claim in the comments is [src:] (verified against that
 * drop) or [guess] (inferred, unconfirmed until the first device
 * trace). Wire strings and protocol numbers live in
 * notifyd_names.h, never as literals here.
 *
 * What this delivery is NOT (sane defaults, all documented):
 * - Shared memory is OUT OF SCOPE (G2). register_check_2 always
 *   takes the server-check fallback: size -1, slot -1, and the
 *   client is registered as PLAIN. That is exactly what the real
 *   server does when nslots == 0, so the client behaviour is
 *   identical from the caller's side.
 * - There is no notifyd_svchost_handler. The standard XPC
 *   handler contract is replaced by the MIG exception for this
 *   service: the entry point is notifyd_process_msg, and the
 *   service registers with launchd directly from notifyd_init.
 * - Entitlements are not enforced (G7). The fake has no
 *   entitlement source, so claim_root_access never promotes a
 *   uid, restricted-post names are always denied, and the
 *   filtered-variant sandbox check always allows.
 * - XPC event delivery is not wired (G6). No MIG routine can
 *   create an XPC_EVENT client (only notifyd_matching_register
 *   can, via the publisher), so the XPC_EVENT fanout case is
 *   unreachable over MIG and is a documented no-op.
 * - Path monitoring is a stub (G8). monitor_file_2 stores the
 *   path on the client; nothing watches it.
 * - Proc death watch is not needed (G5). All clients share the
 *   app's pid, so registrations live and die with the process.
 *   The proc table exists for suspend_pid/resume_pid and for
 *   common-port ownership only.
 * - Reply msgh_id = request id + 100 is [guess] (the standard
 *   MIG rule) until the first device trace.
 * - The register_check_2 fallback returns the REAL name_id, not
 *   0. The real server leaves *name_id at its zero initializer
 *   on the nslots == 0 path, but the client stores nid in both
 *   branches and uses it for regen/post_3/get_state_2/set_state_2,
 *   so 0 would address the wrong name. Documented deviation.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include "ipc.h"
#include "notifyd_names.h"

/* ------------------------------------------------------------------ */
/* Limits.                                                            */
/* ------------------------------------------------------------------ */

/* Largest reply this server builds. The biggest reply is
 * get_state_3 / set_state_3 (header 24 + NDR 8 + RetCode 4 +
 * name_id 8 + state 8, tail-padded to 8 = 56 bytes); 256 leaves
 * ample room and is the cap the ipc.c branch allocates for. */
#define NOTIFYD_MAX_REPLY 256

/* Table sizes. These are the fake's own choices (the real daemon
 * grows its tables dynamically); they are generous for one app
 * process, which is the only client the fake ever serves. */
#define NOTIFYD_MAX_NAMES 64
#define NOTIFYD_MAX_CLIENTS 128
#define NOTIFYD_MAX_PROCS 32
#define NOTIFYD_MAX_GEN_PORTS 64
#define NOTIFYD_MAX_FILEPORTS 16

/* Longest name the server accepts, including the NUL terminator.
 * [src: notify_ipc.defs] (notify_name = c_string[*:512]). */
#define NOTIFYD_NAME_MAX 512

/* ------------------------------------------------------------------ */
/* API.                                                               */
/* ------------------------------------------------------------------ */

/* Initialise the service: create its Mach port (via
 * ipc_notifyd_port_create from notifyd-ipc.patch), register the
 * service name with launchd, and pre-seed the name table from
 * notify.conf.iPhone. Called from daemons_init()
 * (after cfprefsd, before MobileGestalt). Idempotent: a
 * second call returns KERN_SUCCESS without re-registering.
 * Returns KERN_SUCCESS, or a Mach error code on failure. */
int notifyd_init(void);

/* Look up the service port created by notifyd_init, or
 * MACH_PORT_NULL when notifyd_init has not run. */
mach_port_name_t notifyd_lookup_port(void);

/* The kobject entry point. The ipc.c NOTIFYD branch calls this
 * with the request message (msg) and its size (send_size,
 * including any audit trailer the branch appended), plus a reply
 * buffer (reply_buf, reply_cap). Same reply contract as
 * svchost_process_msg: returns 1 with a reply built (the reply
 * size is stored in *out_reply_size), and never silently drops a
 * send-once reply. For a simpleroutine, or when the request has
 * no reply port, the reply size is 0. Returns 0 only when the
 * message is too short to read a header from at all.
 *
 * Runs under ipc_lock (the branch holds it), so this function
 * must not call any lock-taking ipc_* function. It may call the
 * lock-held helpers from notifyd-ipc.patch
 * (ipc_notifyd_gen_port_locked, ipc_notifyd_deliver_locked) and
 * the raw syscalls in linux.h. */
int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap,
                        uint32_t *out_reply_size);

/* Register an fd for a fileport name, for register_file_descriptor_2.
 * The bsd.c fileport_makeport wiring calls this when it creates a
 * fileport; until then the table is empty and unmapped fds are
 * refused silently. Returns 1 on success, 0 when the table is full.
 * This is an internal test hook, not a MIG routine. */
int notifyd_fileport_register(mach_port_name_t port_name, int fd);

/* ------------------------------------------------------------------ */
/* Test hooks (host test only; never called on target).                */
/* ------------------------------------------------------------------ */

/* Reset all tables and re-run the pre-seed, so scenarios start
 * from a clean state. */
void notifyd_test_reset(void);

/* Number of live names / clients. */
int notifyd_test_name_count(void);
int notifyd_test_client_count(void);

/* Find a name by string. Returns 1 when found and fills the
 * out parameters (any of them may be NULL). */
int notifyd_test_find_name(const char *name, uint64_t *name_id_out,
                           uint64_t *state_out, uint32_t *val_out,
                           uint32_t *postcount_out);

/* Find a client by (pid, token). Returns 1 when found and fills
 * the out parameters (any of them may be NULL). type is a
 * NOTIFY_TYPE_* value. */
int notifyd_test_client_info(uint32_t pid, int token, int *type_out,
                             int *suspended_out, int *pending_out,
                             uint64_t *name_id_out);

/* The common port of a proc, or MACH_PORT_NULL when the proc has
 * none (or does not exist). */
mach_port_name_t notifyd_test_proc_common_port(uint32_t pid);
