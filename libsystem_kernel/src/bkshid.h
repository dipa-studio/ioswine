/*
 * bkshid.h: in-process fake for the BackBoardServices HID event
 * delivery service.
 *
 * BKSHIDEventDeliveryManager (BackBoardServices) is the app-process
 * client through which UIKit tells backboardd how HID events should
 * be delivered and deferred for its windows. It is built on a
 * BoardServices BSServiceConnection, so it speaks BSXPC (XPC
 * dictionaries over a Mach service), not the raw MIG protocol of
 * the display service (bks.c). The endpoint is
 * "com.apple.backboard.hid-services.xpc" [guess, strong]. The
 * BSXPC instance / interface name is [open]:
 * this file never asserts one; the handler reads it from the
 * incoming bsxpc_context on connect and logs it.
 *
 * What this fake is for: with the manager live and its connection
 * activated, -[_UIEventDeferringManager _setLocalRecord:
 * forEnvironment:] can save records during key-window evaluation
 * instead of throwing "Invalid state. Saving records without a
 * transaction". The fake cannot open the client's transaction for
 * it; it makes the client's transaction open succeed by answering
 * every message benignly and never dropping a reply. It is about
 * key-window evaluation, not touch delivery: touches still enter
 * through -[UIEventFetcher _receiveHIDEventInternal:], and the raw
 * com.apple.backboard.hid.services event port is intentionally left
 * unanswered.
 *
 * Transport: the service registers with svchost_register like
 * fakeboard does and rides the existing PORT_KOBJECT_SVCHOST branch
 * in ipc.c. No new kobject kind, no ipc.c branch, no launchd.c
 * change. Registration happens from the same init path as
 * fakeboard_init (init.c, IOSWINE_FAKEBOARD=1 block).
 *
 * Style: freestanding C11, same as fakeboard.c / bks.c. No libc, no
 * malloc, no static function-pointer tables (dispatch is an
 * if-chain on the bsxpc_SEL string).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "svchost.h"

/* Mach service name the delivery manager connects to [guess, strong]. */
#define BKSHID_SERVICE_NAME "com.apple.backboard.hid-services.xpc"

/* Env kill-switch for A/B debugging (mirrors hidsys_is_enabled):
 * IOSWINE_BKSHID=0 leaves the service unregistered. Default is on. */
int bkshid_is_enabled(void);

/* Register the service with svchost. Idempotent. Returns
 * KERN_SUCCESS, or the svchost_register error. */
int bkshid_init(void);

/* The svchost handler. Dispatches on bsxpc_SEL with an if-chain.
 * Returns 0 after building a benign success reply for every
 * parseable message (never a bare {"error": ...} dict: an
 * error-shaped reply where the client expects a payload makes the
 * client throw). Returns SVCHOST_ERR_BAD_MESSAGE only
 * for input bsxpc_message_parse rejects (no CID exists to echo),
 * SVCHOST_ERR_NO_MEMORY only when the reply does not fit. */
int bkshid_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx);

/* Server-side state the deferring path needs.
 * last_sel is the most recent bsxpc_SEL seen, NUL terminated (empty
 * when none arrived yet). The buffer is copied out; the caller owns
 * it. */
typedef struct {
    int connected;           /* a connect handshake was answered */
    int txn_open;            /* a transaction-begin-like selector arrived
                              * without a matching end [guess heuristic] */
    uint32_t connects;       /* connect handshakes answered */
    uint32_t messages;       /* parseable messages handled */
    uint32_t record_saves;   /* record-like selectors seen [guess] */
    char last_sel[128];
} bkshid_state_t;

void bkshid_get_state(bkshid_state_t *out);

#ifdef BKSHID_HOST_TEST
/* Host test only: reset all state (but not the registration flag). */
void bkshid_test_reset(void);
#endif
