/*
 * fakeboard.h: in-process fake FrontBoard workspace service for ioswine.
 *
 * The fake answers the workspace service endpoints and drives the scene
 * lifecycle from the server side: it initiates the scene-create callout,
 * then pushes settings updates and activation, because the client (UIKit)
 * never creates a scene on its own. Scene creation is a SERVER-INITIATED
 * callout (FBSWorkspaceScenesClient
 * createSceneWithIdentity:... on iOS 17 /
 * createWithSceneID:groupID:... on newer); waiting for a create
 * request deadlocks the app at the CFRunLoop wait.
 *
 * Dispatch is keyed by bsxpc_SEL (via bsxpc_message_parse).
 * The real envelope keys are "bsxpc" (kind, "connect" for
 * the handshake), "bsxpc_context" (parameters dict), "bsxpc_CID"
 * (uint64 call id), "bsxpc_CIDr" (bool, true for a reply) and
 * "bsxpc_SEL" (the ObjC selector). Replies are completion replies:
 * same CID, CIDr=true, plus the result fields. The callout envelope
 * uses the same real keys (bsxpc / bsxpc_SEL / bsxpc_CID).
 *
 * THE "connect" REPLY RULE. The incoming message's msgh_id is the
 * reply-expectation signal: 0x40000000 means a reply is expected,
 * 0x10000000 means the message is one-way and NO reply may be sent.
 * Sending a reply to a one-way "connect" crashes libxpc in its
 * async-reply hook (the crash follows fakeboard's reply to "connect"
 * arriving on reply port knote 0x2103). fakeboard_process_msg enforces
 * the rule because it sees the Mach header; the svchost adapter handler
 * cannot (it never sees the header). Any other msgh_id falls back to the
 * rule: reply when a reply port is present.
 *
 * Design decisions (all marked [guess] where unverified):
 *
 * Endpoint discovery. The fake learns the client's callout endpoint
 * in this priority order:
 *   1. A mach_send port value carried in the handshake / first message
 *      on a workspace service port (any key, preferring "endpoint",
 *      "connection", "clientEndpoint"). This is the persistent send
 *      right a real XPC connection gives the server, and it is visible
 *      to both entry points through the reader's descriptors. [guess]
 *   2. The message's msgh_local_port (reply port) as a one-shot
 *      channel. This is visible only in the fakeboard_process_msg
 *      path (the svchost adapter handler never sees the raw header),
 *      and the endpoint is cleared after one callout is delivered
 *      through it. [guess]
 *   3. Otherwise no endpoint is known and callouts stay queued;
 *      fakeboard_next_callout returns 0 until one arrives.
 * The endpoint scan now also looks inside bsxpc_context, since the
 * parameters moved there from the message root.
 *
 * Trigger. The first create callout is queued when the client sends a
 * scene-create request (a message whose bsxpc_SEL is a scene-create
 * selector, or a dict carrying identity/parameters keys). The handler
 * replies to the request itself with an acknowledgement dict {"error":
 * 0, "sceneIdentifier": ...}. fakeboard_scene_create(bundle_id) is the
 * manual trigger with the same effect. State machine:
 *   NONE -> CREATING (on request / scene_create) -> FOREGROUND (on
 *   update + activate) -> BACKGROUND (on background) -> INVALIDATED
 *   (on invalidate). A create callout that cannot be delivered within
 *   the FRONTBOARD 0x8BADF00D watchdog (about 12-20 s of wall-clock
 *   time in scene creation) kills the app, so the runtime must
 *   drain the callout queue promptly.
 *
 * Execution model. Synchronous kobject dispatch, like launchd.c /
 * bks.c / svchost.c. Handlers are pure
 * dict transforms: no ipc_lock takers, no blocking, no IPC. The
 * callout queue is drained (by the host simulator or the event loop)
 * through fakeboard_next_callout; nothing here sends a
 * Mach message itself except fakeboard_init's registrations, which go
 * through svchost_register like any other service.
 *
 * Wire strings. Every service name, selector, key and display value
 * comes from fakeboard_names.h. fakeboard.c contains no wire-string
 * literals (checked by grep); correcting a string after verification
 * means editing that one header.
 *
 * Style: freestanding C, same as launchd.c / bks.c / svchost.c. No
 * libc. No em dashes in comments.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "xpcwire.h"
#include "svchost.h"
#include "bsxpc.h"
#include "fakeboard_names.h"

/* XPC message id meaning "a reply is expected" (measured on device
 * 2026-11-12). XW_MSGH_ID_XPC (0x10000000)
 * is the one-way message id. Neither xpcwire.h nor ipc.h defines
 * the 0x40000000 constant yet; the guard avoids a redefinition error here. */
#ifndef XW_MSGH_ID_XPC_REPLY_EXPECTED
#define XW_MSGH_ID_XPC_REPLY_EXPECTED 0x40000000U
#endif

/* Scene states, returned by fakeboard_scene_state. */
#define FB_SCENE_NONE 0
#define FB_SCENE_CREATING 1
#define FB_SCENE_FOREGROUND 2
#define FB_SCENE_BACKGROUND 3
#define FB_SCENE_INVALIDATED 4

/* Callout queue capacity (queued outbound callouts) and the largest
 * single callout body the queue holds (Mach header excluded). The
 * create callout with full settings is well under 1 KB; 2048 leaves
 * room for longer identity strings. */
#define FB_CALLOUT_QUEUE_LEN 8
#define FB_CALLOUT_MAX_BODY 7168
/* Largest reply body fakeboard_process_msg builds (header excluded).
 * Replies are small ack/error dicts; 1024 matches SVCHOST_MAX_REPLY. */
#define FB_MAX_REPLY_BODY 1024

/*
 * Register the workspace services with svchost_register (workspace
 * service, coupler, systemappservices, system-service) and arm the
 * state machine. Idempotent. Returns KERN_SUCCESS (0) or the first
 * registration error. The self pid defaults to the Linux pid of this
 * process (getpid syscall) until fakeboard_set_self_pid overrides it.
 */
int fakeboard_init(void);

/*
 * Kobject entry point, same shape as launchd_process_msg /
 * svchost_process_msg. Called by the ipc.c dispatch branch for a
 * fakeboard service port (future integration), or directly by the
 * host simulator. Dispatches by destination port (hdr->msgh_remote_port
 * matched against the ports fakeboard_init registered), logs every
 * incoming dict verbatim via bsxpc_log_dict with prefix "[fakeboard]"
 * when trace is on, learns the client endpoint (rules 1 and 2 above),
 * and builds the reply in reply_buf (header + XPC dict body, msgh_id
 * 0x20000000, reply on hdr->msgh_local_port with MOVE_SEND_ONCE).
 *
 * THE "connect" REPLY RULE (see the file header): the reply is built
 * only when the incoming msgh_id says a reply is expected
 * (0x40000000); a message with msgh_id 0x10000000 is one-way and
 * gets NO reply even when it carries a reply port (return 1 with
 * *out_reply_size 0). Any other msgh_id falls back to the reply-port
 * rule. The dispatch itself still runs for a one-way message, so its
 * side effects (endpoint learning, handshake marking) happen.
 *
 * Returns 1 if a reply was built (*out_reply_size set, 0 for a
 * one-way message with no reply port, or for a one-way message by
 * msgh_id) or 0 only if no reply could be built at all (reply_buf
 * too small for even the minimal error dict). A return of 1 with a
 * reply port always means a reply dict was built: a send-once reply
 * is never silently dropped (dropping it would deliver
 * MACH_NOTIFY_SEND_ONCE to the client, which libxpc reads as
 * MIG_SERVER_DIED, -308). The one exception is the msgh_id rule
 * above: a one-way message deliberately gets no reply.
 */
int fakeboard_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                          uint8_t *reply_buf, uint32_t max_reply_size,
                          uint32_t *out_reply_size);

/*
 * Drain one queued outbound callout. Fills buf with a complete Mach
 * message (24-byte header + XPC body), writes the total length to
 * *out_len and the msgh_id to *out_msgh_id. The header's remote port
 * is the client's callout endpoint and the local port is this fake's
 * workspace service port (so the client can reply to the callout).
 * Returns 1 when a callout was written, 0 when the queue is empty or
 * no client endpoint is known yet (callouts stay queued), or a
 * negative XW_ERR_* when buf is too small for the next callout.
 * A one-shot endpoint (learned from a reply port) is cleared after
 * the callout it delivered is dequeued.
 */
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                           uint32_t *out_msgh_id);

/*
 * Build and queue the SERVER-INITIATED scene-create callout for
 * bundle_id: identity "sceneID:<bundle>-default", specification
 * UIApplicationSceneSpecification, initial settings and client
 * settings for 1440x3200 @ scale 4. Moves state to CREATING.
 * Returns 0, KERN_ALREADY_IN_SET when a scene is already creating or
 * foreground, or a negative XW_ERR_* when the callout does not fit
 * the queue. The bundle id is remembered for later requests that do
 * not carry one.
 */
int fakeboard_scene_create(const char *bundle_id);

/*
 * Queue a settings update callout: foreground YES, frame
 * {{0, 0}, {360, 800}}, displayConfiguration 1440x3200 @ scale 4,
 * interfaceOrientation portrait. Requires a scene in CREATING or
 * FOREGROUND, else KERN_INVALID_VALUE. Does not change state on its
 * own; foreground is reached by fakeboard_scene_activate.
 */
int fakeboard_scene_update_settings(void);

/*
 * Queue the activation callout (BSAction kind "activate", target the
 * scene identity). Moves CREATING to FOREGROUND. Idempotent when
 * already FOREGROUND. Requires a scene in CREATING or FOREGROUND,
 * else KERN_INVALID_VALUE.
 */
int fakeboard_scene_activate(void);

/*
 * Queue a settings update callout with foreground NO and move
 * FOREGROUND to BACKGROUND. Requires FOREGROUND, else
 * KERN_INVALID_VALUE. Provided for the
 * state machine to be complete.
 */
int fakeboard_scene_background(void);

/*
 * Queue the invalidation callout (BSAction kind "destroy", target the
 * scene identity) and move state to INVALIDATED. Requires a scene
 * (any state except NONE), else KERN_INVALID_VALUE. After this, a new
 * scene can be created with fakeboard_scene_create.
 */
int fakeboard_scene_invalidate(void);

/* Current scene state: FB_SCENE_NONE / CREATING / FOREGROUND /
 * BACKGROUND / INVALIDATED. */
int fakeboard_scene_state(void);

/* 1 when fakeboard_init ran (a scene can still arrive), else 0.
 * The mainscreen hook gates its scene wait on this:
 * with fakeboard off no scene will ever come, so the hook must
 * call through immediately instead of waiting out the budget. */
int fakeboard_is_inited(void);

/*
 * Test hook: override the pid the process handle and audit token use
 * (default is the Linux pid of this process). The simulator sets this
 * to its fake pid.
 */
void fakeboard_set_self_pid(uint32_t pid);

/* The svchost adapter handler. Registered per service by
 * fakeboard_init; the host simulator may also call it directly. It
 * shares the dispatch core with fakeboard_process_msg but never sees
 * the raw Mach header, so it can only learn the endpoint from a
 * mach_send port value in the message (endpoint rule 1) and it
 * CANNOT enforce the "connect" msgh_id rule: every message that
 * reaches it gets a reply when svchost_process_msg sees a reply
 * port. The ctx is a per-service record owned by fakeboard.c; NULL
 * ctx means "workspace service". 0 means the reply was built in the
 * builder, any other value goes verbatim into the reply's "error" key. */
int fakeboard_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx);
