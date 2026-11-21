/*
 * cfprefsd.h: fake CFPreferences daemon (XPC service) for ioswine.
 *
 * Registers both Mach service names with one handler (svchost):
 * com.apple.cfprefsd.agent (per-user) and com.apple.cfprefsd.daemon
 * (privileged). Storage is one bplist00 file per domain under
 * container_preferences(), written temp-file plus rename, never in
 * place. Every claim is [src: ...] or [guess].
 *
 * Operation dispatch (one place, cfprefsd.c): only ops 1 and 5 are
 * verified in public sources. Op 8 (set-multiple) and ops 4 / 20
 * (no-reply handlers, treated here as synchronize no-ops) are
 * [guess]. Get, copy-values, copy-multiple, synchronize, list-keys
 * and list-applications have NO known operation number yet
 *: CFP_OP_GET and friends are -1 until the first
 * device trace supplies the numbers, and the dispatcher answers
 * SVCHOST_ERR_UNKNOWN_MESSAGE (1) for them. The placeholder reply
 * shape for a future get is: the requested values inline in the
 * reply dict when the reply fits SVCHOST_MAX_REPLY_BODY, else
 * SVCHOST_ERR_NO_MEMORY (12), never truncated; the fd fallback is
 * unresolved until the trace shows the reply key.
 *
 * Sane defaults (correct from the first device trace):
 * - A set reply is an empty dict [guess].
 * - The Value of an op 1 set is stored as-is as a bplist DATA node
 *   [guess: the client library serializes the plist value to data
 *   before sending; the daemon stores the bytes as-is].
 * - A set for a domain whose file does not exist yet, with the
 *   Preferences directory present, succeeds without an access
 *   token (there is nothing to protect yet; the file is created
 *   fresh). A set for an existing domain file always requires a
 *   valid CFPreferencesAccessToken fd (F_GETFL O_RDWR and F_GETPATH
 *   equal to the plist path); without it the set fails with
 *   SVCHOST_ERR_BAD_MESSAGE (22).
 * - A missing Preferences (or ByHost) directory fails the set with
 *   22: the fake never creates directories.
 * - A domain string containing '/' is rejected with 22: the domain
 *   is a file stem, never a path (the DEF CON sample abusing the
 *   domain as a path is exploit behavior, not normal client use).
 * - The ByHost host identifier is the fixed placeholder CFP_HOST_ID
 *   [guess]; the fake has no host-UUID source.
 * - CFPreferencesUser "kCFPreferencesAnyUser" and
 *   "kCFPreferencesCurrentUser" both map to container_preferences()
 *   in the fake (one container); the agent/daemon role does not
 *   change storage [guess].
 * - Domain plists are capped at 1 MiB (CFP_MAX_PLIST_BYTES, the
 *   real daemon's clone threshold). A
 *   larger existing file, or a serialized dict that would exceed
 *   the cap, fails the set with SVCHOST_ERR_NO_MEMORY (12).
 * - The access-token fd is read from the Mach message descriptor
 *   as a host fd number. That is the host-test semantics; on target
 *   the descriptor carries a fileport port, and converting it needs
 *   an ipc.c fileport_makefd step that does not exist yet. Until
 *   then a set with a token fails closed with 22 on target (fcntl
 *   on a port name fails), and only the no-file first-write path
 *   above can succeed there. This is a known limitation, not a
 *   silent bypass.
 * - The F_GETPATH comparison strips a trailing " (deleted)":
 *   Linux /proc readlink appends it when the file has been replaced
 *   since the fd was opened, which the temp-file plus rename does
 *   between the sub-messages of a multi. Darwin's F_GETPATH has no
 *   such suffix, so the strip keeps per-sub-message validation
 *   working the way the real daemon's does.
 * - A failed write may leave the "<name>.cfptmp" temp file behind;
 *   the next write to the same domain overwrites it.
 *
 * File I/O goes through the CFP_* macros, which default to the
 * plain Darwin names bsd.c provides on target (openat, read, write,
 * close, renameat, fcntl, with Darwin flag values). The host test
 * redirects them with -D to its own translators
 * (cfp_host_openat etc.), which translate the Darwin flags to Linux
 * flags and call libc. See the test header comment.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"
#include "svchost.h"
#include "cfprefs_names.h"

/* Operation numbers.
 * Op 1 serves both read domain (when Key is absent) and set a single value
 * (when Key is present) [src: 0x183565818, 0x183569874, 0x18357d12c].
 * Op 5 is a multi-message batch answering with a "CFPreferencesMessages"
 * array matching request count and order [src: 0x183570fd8, 0x183571220,
 * 0x18357127c, 0x18357cd8c].
 * 8, 4 and 20 are [guess]; the rest are unknown (-1) until the first
 * device trace. */
#define CFP_OP_READ          1  /* read domain plist (when Key is absent) [src: 0x183565818] */
#define CFP_OP_SET           1  /* set a single value (Key + Value data) [src: 0x183565818] */
#define CFP_OP_MULTI         5  /* CFPreferencesMessages array [src: 0x183570fd8] */
#define CFP_OP_SET_MULTIPLE  8  /* [guess] inline key/value pairs */
#define CFP_OP_NO_REPLY_A    4  /* [guess] no-reply op, sync no-op here */
#define CFP_OP_NO_REPLY_B    20 /* [guess] no-reply op, sync no-op here */
#define CFP_OP_GET           (-1) /* [guess] unknown number (get) */
#define CFP_OP_COPY_VALUES   (-1) /* [guess] unknown number */
#define CFP_OP_COPY_MULTIPLE (-1) /* [guess] unknown number */
#define CFP_OP_SYNCHRONIZE   (-1) /* [guess] unknown number */
#define CFP_OP_COPY_KEY_LIST (-1) /* [guess] unknown number */
#define CFP_OP_COPY_APP_LIST (-1) /* [guess] unknown number */

/* Limits. */
#define CFP_MAX_PLIST_BYTES   0x100000u   /* 1 MiB domain file cap */
#define CFP_ARENA_BYTES       (0x100000u + 0x10000u) /* parse arena: 1 MiB + 64 KiB */
#define CFP_MAX_PATH          1024        /* longest plist path built here */
#define CFP_MAX_DOMAIN        256         /* longest domain string accepted */
#define CFP_MAX_KEY           256         /* longest preference key accepted */
#define CFP_SHMEM_SIZE        0x4000u     /* 16 KiB shared generation page */
#define CFP_SHMEM_SLOTS       (CFP_SHMEM_SIZE / 4u) /* 4096 uint32_t slots */
#define CFP_MAX_SHMEM_DOMAINS 512u        /* static slot-mapping table size */

/* Register both service names with svchost_register (one handler).
 * Idempotent; returns KERN_SUCCESS or the registration error. Call
 * once at startup, after ipc_init() + svchost_init(). */
int cfprefsd_init(void);

/* Name of the cfprefsd shared-memory region (NUL-terminated), or "" if
 * shmem has not been initialized yet. */
const char *cfprefsd_shm_name(void);

/* File descriptor for the cfprefsd shared-memory region when memfd-backed
 * (or -1 when name does not match or /dev/shm backing is used). */
int cfprefsd_shm_fd(const char *name);

/* Kobject entry point, same shape as svchost_process_msg. Parses the
 * incoming XPC message, runs the cfprefsd handler, and builds the
 * reply in reply_buf (header + XPC dict body, msgh_id 0x20000000).
 * Returns 1 if a reply was built (*out_reply_size set, 0 for a
 * one-way message with no reply port) or 0 only if no reply could
 * be built at all. */
int cfprefsd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                         uint8_t *reply_buf, uint32_t max_reply_size,
                         uint32_t *out_reply_size);

/* The svchost handler (registered for both service names). Return 0
 * means the reply was built in the builder; any other value goes
 * verbatim into the reply dict's "error" key. Logs the incoming
 * message verbatim with bsxpc_log_dict("[cfprefsd]") when tracing
 * is enabled. */
int cfprefsd_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx);
