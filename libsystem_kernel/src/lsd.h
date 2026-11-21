/*
 * lsd.h: fake LaunchServices daemon for ioswine.
 *
 * Serves the running app's own bundle record (bundle id, bundle
 * URL, container URLs, the Info.plist values the app reads through
 * LS) over the three registered Mach services com.apple.lsd.xpc,
 * com.apple.lsd.mapdb and com.apple.lsd.modifydb, so that
 * UIApplicationMain and the first UIKit/Foundation calls that
 * resolve the app's own identity do not block. There is exactly one
 * app on the device as far as ioswine is concerned; everything else
 * (listings of other apps, URL-scheme handlers for other schemes,
 * icon fetching, database writes) answers empty or with an error,
 * and never hangs.
 *
 * The request keys, operation codes and reply shape are
 * PLACEHOLDERS (all values in lsd_names.h, marked [guess]): no
 * published trace gives the real LS wire format, and the raw-dict
 * vs NSXPC question is unresolved. The handler tries both parsings
 * (plain dict first, then a raw-byte scan for the bundle id) and
 * logs every incoming message verbatim with bsxpc_log_dict("[lsd]")
 * when tracing is enabled, so the first device trace corrects the
 * table with a small diff. Do not present the host-test-only shape
 * as the real protocol.
 *
 * The other Mach names from the macOS plist (open, openurl, plugin,
 * icons, advertisingidentifiers, diagnostics, dissemination) are
 * deliberately NOT registered: a lookup for one of them fails fast
 * at the launchd shim instead of hanging, and a request for one of
 * those functions arriving on the xpc service is logged verbatim
 * and answered with an error, never dropped.
 *
 * Style: freestanding C, same as svchost.c / fakeboard.c. No libc.
 * Handlers are pure dict transforms: no ipc_lock takers, no
 * blocking, no allocation. All state lives in static tables.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "svchost.h"
#include "csops.h"

/* lsd-specific error codes (values of the reply "error" key, in
 * addition to the SVCHOST_ERR_* codes in svchost.h). */
#define LSD_ERR_NO_HANDLER 2  /* no app handles this URL scheme [guess] */

/*
 * Register the three services (xpc, mapdb, modifydb) with
 * svchost_register. Idempotent; call once at startup, after
 * ipc_init() + svchost_init(). Returns KERN_SUCCESS (0) or the
 * registration error. The container paths (container_set_home /
 * container_set_bundle, called by the loader) must be set before
 * the first record reply, or the record omits the URL fields
 * rather than fabricating paths.
 */
int lsd_init(void);

/*
 * Kobject entry point, same shape as svchost_process_msg /
 * fakeboard_process_msg. Dispatches by destination port among the
 * three lsd ports, parses the incoming XPC message, runs the
 * handler, and builds the reply in reply_buf (header + XPC dict
 * body, msgh_id 0x20000000, reply on hdr->msgh_local_port with
 * MOVE_SEND_ONCE). Returns 1 if a reply was built (*out_reply_size
 * set, 0 for a one-way message with no reply port) or 0 only if no
 * reply could be built at all. A return of 1 with a reply port
 * always means a reply dict was built (never silently drop a
 * send-once reply).
 */
int lsd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                    uint8_t *reply_buf, uint32_t max_reply_size,
                    uint32_t *out_reply_size);

/*
 * svchost handler (registered by lsd_init for all three services).
 * ctx is a pointer to the service index (0 xpc, 1 mapdb,
 * 2 modifydb) or NULL. Follows the handler interface in svchost.h:
 * return 0 means the reply was built in the builder; any other
 * value goes verbatim into the reply dict's "error" key.
 */
int lsd_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx);

/*
 * Supply the running app's parsed code signature (from csops)
 * so the record can carry teamID and the entitlements dict.
 * Pass NULL to clear it; with no info set the record omits those
 * two fields. Additive; does not change the fixed service pattern.
 * The pointer must stay alive while lsd serves replies (the info
 * is read, not copied, on every record reply).
 */
void lsd_set_csops_info(const csops_info *info);
