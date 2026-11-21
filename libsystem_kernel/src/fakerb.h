/*
 * fakerb.h: fake RunningBoard service for ioswine.
 *
 * Answers the RBSConnection side of the launch handshake for the app
 * process itself: process handle for self, the connection handshake
 * (handshakeWithRequest:), assertion acquire and invalidate,
 * process-state monitor subscribe and unsubscribe. There is no real
 * runningboardd; the app process is the only process, so every
 * answer describes the app process (pid and bundle id set by the
 * test hooks below, or by the integration layer at startup).
 *
 * Dispatch is keyed by bsxpc_SEL (see bsxpc_message_parse in bsxpc.h),
 * and every reply echoes the request CID with CIDr=true when
 * the request carried one (see bsxpc_reply_build). The "connect"
 * reply rule applies here too: a message whose msgh_id is
 * 0x10000000 is one-way and gets NO reply, even when it carries a
 * reply port (replying to a one-way message crashes libxpc in its
 * async-reply hook). A message with msgh_id
 * 0x40000000 expects a reply, and gets one. The rule is enforced in
 * fakerb_process_msg (the kobject entry, which sees the header).
 * The svchost adapter path (svchost_process_msg) never sees a
 * msgh_id, so it cannot enforce the rule: there, every message with
 * a reply port gets a reply.
 *
 * Wire strings: the envelope keys and selector spellings live in
 * fakeboard_names.h; the defines below are only short aliases to it,
 * so a corrected string is a one-line change in the table. The RBS
 * selectors are [guess], except "handshakeWithRequest:" which is measured
 * (device 2026-11-12: Aidoku sends RBS handshakeWithRequest: to the fake).
 *
 * Style: freestanding C, same as launchd.c / bks.c / svchost.c. No
 * libc. The dispatch runs synchronously in the sender's thread as a
 * kobject, so handlers are pure dict
 * transforms: no ipc_lock takers, no blocking, no allocation. All
 * state lives in static tables in fakerb.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "xpcwire.h"
#include "fakeboard_names.h"

/* 0x40000000: the incoming msgh_id that means a reply is expected.
 * xpcwire.h defines XW_MSGH_ID_XPC (0x10000000, one-way) and
 * XW_MSGH_ID_XPC_REPLY (0x20000000, a reply); the reply-expected
 * value is defined here guarded. */
#ifndef XW_MSGH_ID_XPC_REPLY_EXPECTED
#define XW_MSGH_ID_XPC_REPLY_EXPECTED 0x40000000U
#endif

/* ------------------------------------------------------------------ */
/* Wire strings. ALL [guess] unless noted. They live in              */
/* fakeboard_names.h (the ONE table); the defines below are only     */
/* short aliases to it, so a corrected string is a one-line change   */
/* in the table.                                                     */
/* ------------------------------------------------------------------ */

/* BSXPC message envelope keys, aliased to the table. The
 * envelope is bsxpc / bsxpc_context / bsxpc_CID / bsxpc_CIDr /
 * bsxpc_SEL, measured on device 2026-11-12. */
#define FAKERB_KEY_BSXPC   FB_BSXPC_KEY_BSXPC
#define FAKERB_KEY_CONTEXT FB_BSXPC_KEY_CONTEXT
#define FAKERB_KEY_CID     FB_BSXPC_KEY_CID
#define FAKERB_KEY_CIDR    FB_BSXPC_KEY_CIDR
#define FAKERB_KEY_SEL     FB_BSXPC_KEY_SEL

/* Reply keys. "error" and "pid" are aliased to the table ("error" is
 * measured, not guessed). The
 * handle/states/state/identifier keys live in fakeboard_names.h
 * (FAKERB_KEY_HANDLE etc.). */
#define FAKERB_KEY_ERROR      FB_ENV_ERROR
#define FAKERB_KEY_PID        FB_ENV_PID

/* RBSProcessHandle property names, used as the wire keys of the
 * handle dict (key spellings are [guess]). Aliased to fakeboard_names.h. */
#define FAKERB_HANDLE_KEY_PID           FB_BSXPC_KEY_HANDLE_PID
#define FAKERB_HANDLE_KEY_BUNDLE_ID     FB_BSXPC_KEY_HANDLE_BUNDLE_ID
#define FAKERB_HANDLE_KEY_AUDIT_TOKEN   FB_BSXPC_KEY_HANDLE_AUDIT_TOKEN
#define FAKERB_HANDLE_KEY_IS_RUNNING    FB_BSXPC_KEY_HANDLE_IS_RUNNING
#define FAKERB_HANDLE_KEY_IS_FOREGROUND FB_BSXPC_KEY_HANDLE_IS_FOREGROUND
#define FAKERB_HANDLE_KEY_IS_VALID      FB_BSXPC_KEY_HANDLE_IS_VALID
#define FAKERB_HANDLE_KEY_IS_APPLICATION FB_BSXPC_KEY_HANDLE_IS_APPLICATION

/* Limits (not wire strings, so they stay here). */
#define FAKERB_MAX_ASSERTIONS 32
#define FAKERB_MAX_MONITORS   8
#define FAKERB_BUNDLE_ID_MAX  256

/* ------------------------------------------------------------------ */
/* Public API. The host simulator codes against exactly this.          */
/* ------------------------------------------------------------------ */

/*
 * Register com.apple.runningboard with svchost_register, so
 * bootstrap_look_up finds it. Idempotent. Returns KERN_SUCCESS (0)
 * or the registration error. Call once at libsystem_kernel startup,
 * after ipc_init() and svchost_init(). Does not touch the self pid
 * or bundle id; set those with the hooks below before the first
 * client message arrives (a handle query with pid still 0 answers
 * {"error": 1}).
 */
int fakerb_init(void);

/*
 * Kobject entry point, same shape as svchost_process_msg /
 * launchd_process_msg. Called by the ipc.c dispatch branch for the
 * RunningBoard service port (or directly by the host simulator with
 * a crafted Mach message). Parses the incoming XPC message, logs it
 * verbatim with bsxpc_log_dict under the "[fakerb]" prefix when
 * tracing is on (IOSWINE_SVC_TRACE=1), runs the bsxpc_SEL dispatch,
 * and builds the reply in reply_buf (header + XPC dict body, msgh_id
 * 0x20000000, reply on hdr->msgh_local_port with MOVE_SEND_ONCE).
 *
 * The "connect" reply rule: when hdr->msgh_id is 0x10000000 the
 * message is one-way and NO reply is built or sent, even when it
 * carries a reply port; the function returns 1 with
 * *out_reply_size = 0. When msgh_id is 0x40000000 (or anything
 * else) and a reply port exists, a reply is built.
 *
 * Returns 1 if a reply was built (*out_reply_size set, 0 for a
 * one-way message with no reply port, or for a message suppressed
 * by the msgh_id rule) or 0 only if no reply could be built at all
 * (reply_buf too small for even the minimal error dict). A return
 * of 1 with a reply port always means a reply dict was built,
 * unless the msgh_id rule suppressed it: a send-once reply is
 * never silently dropped (dropping it would deliver
 * MACH_NOTIFY_SEND_ONCE to the client, which libxpc reads as
 * MIG_SERVER_DIED, -308).
 */
int fakerb_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size);

/* Test hooks. Must be called before the first client message (and
 * not concurrently with dispatch; the dispatch runs under ipc_lock
 * and these do not take any lock). The bundle id is copied into a
 * static buffer, so the caller need not keep the string alive.
 * fakerb_set_self_bundle_id with a NULL or empty string is ignored.
 * Defaults: pid 0 until set, bundle id FAKERB_DEFAULT_BUNDLE_ID. */
void fakerb_set_self_pid(uint32_t pid);
void fakerb_set_self_bundle_id(const char *bundle_id);

/* Assertion table inspection (host simulator / tests). Count of
 * currently held assertions; 1 if the assertion id is currently
 * held, 0 otherwise. */
int fakerb_assertion_count(void);
int fakerb_assertion_valid(uint32_t id);
