/*
 * svchost.h: in-process XPC service host for ioswine.
 *
 * A service registry for fake system services that speak XPC over Mach
 * messages. A service registers a name and a handler; the name is
 * published through the existing bootstrap_register2 path (launchd
 * shim, routine 0xcc), so bootstrap_look_up finds it with no launchd.c
 * changes; incoming messages are dispatched synchronously in the
 * sender's thread, as a kobject, exactly like launchd.c and bks.c do
 * today.
 *
 * Style: freestanding C, same as launchd.c / bks.c. No libc.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "xpcwire.h"

/* PORT_KOBJECT_SVCHOST is new in ipc.h (value 7, after
 * PORT_KOBJECT_BKS_DISPLAY 6). The guard keeps this header working
 * until ipc.h gains it; the ipc.c dispatch branch and
 * ipc_svchost_port_create() are required ipc.c additions. */
#ifndef PORT_KOBJECT_SVCHOST
#define PORT_KOBJECT_SVCHOST 7
#endif

/* Limits. */
#define SVCHOST_MAX_SERVICES    32
#define SVCHOST_NAME_MAX        128
/* Total reply Mach message, header included. Handlers must fit their
 * reply body in SVCHOST_MAX_REPLY_BODY bytes. The ipc.c kobject branch
 * needs a rep_buf of at least SVCHOST_MAX_REPLY. */
#define SVCHOST_MAX_REPLY       1024
#define SVCHOST_MAX_REPLY_BODY  (SVCHOST_MAX_REPLY - sizeof(mach_msg_header_t))

/* XPC reply message id. libxpc's xpc_pipe_routine wrapper requires the
 * reply id to be exactly this. */
#define SVCHOST_REPLY_MSGH_ID   0x20000000U
/* XPC request message id family (pipe routines and connections). */
#define SVCHOST_REQUEST_MSGH_ID 0x10000000U
/* Bootstrap port name (launchd kobject, ipc.c). */
#define SVCHOST_BOOTSTRAP_PORT  ((mach_port_name_t)0x203)

/* Handler return contract: 0 means "reply built in the builder";
 * any other value is put verbatim into the reply dictionary's "error"
 * key (int64) and no other reply keys are sent. A handler that fails
 * to build its reply must return non-zero, never 0 with a half-built
 * builder. The reader is passed non-const because xw_dict_find takes
 * a non-const xw_reader; the reader never mutates the buffer, it is
 * zero-copy read-only. */
typedef int (*svchost_handler_t)(xw_reader *msg, xw_builder *reply,
                                 void *ctx);

/* Handler error codes (values of the reply "error" key). */
#define SVCHOST_ERR_UNKNOWN_MESSAGE  1  /* not a message this service answers */
#define SVCHOST_ERR_BAD_MESSAGE      22 /* malformed XPC payload (EINVAL) */
#define SVCHOST_ERR_NO_MEMORY        12 /* reply did not fit (ENOMEM) */

/* The example service. */
#define SVCHOST_PING_SERVICE "com.apple.ioswine.ping"

/*
 * Required ipc.c addition (does not exist in ipc.h yet): create one
 * receive port owned by the service host, of kind PORT_KOBJECT_SVCHOST,
 * and return its name in the caller task's namespace (a RECEIVE entry,
 * like ipc_mach_reply_port but with the kobject kind set). Returns
 * MACH_PORT_NULL on failure (no free port or no free entry). The port
 * must be created by ipc.c because struct ipc_port is defined there
 * and svchost cannot set kobject_kind itself.
 */
extern mach_port_name_t ipc_svchost_port_create(void);

/* Initialise the registry (idempotent) and register the example ping
 * service. Call once at libsystem_kernel startup, after ipc_init().
 * Returns KERN_SUCCESS (0) or the first registration error. */
int svchost_init(void);

/*
 * Register a service: create its receive port, publish the name with
 * the launchd shim through a bootstrap_register2 message to the
 * bootstrap port, and record handler/ctx. On success *out_port (if
 * non-NULL) receives the service port name. Re-registering an existing
 * name updates handler/ctx and returns the existing port (the launchd
 * table keeps the same port, as its 0xcc path overwrites the slot).
 * Returns KERN_SUCCESS or a KERN_* code (KERN_NO_SPACE if the table is
 * full, KERN_RESOURCE_SHORTAGE if no port could be created, otherwise
 * the "error" value the launchd shim returned for the register).
 */
int svchost_register(const char *name, svchost_handler_t handler,
                     void *ctx, mach_port_name_t *out_port);

/* Convenience service_register(name, handler) spelling, for callers
 * that do not need a context or the port name. */
static inline int service_register(const char *name,
                                   svchost_handler_t handler) {
    return svchost_register(name, handler, NULL, NULL);
}

/* Look up a registered service's port name (in-process use, e.g. tests
 * or other lsk code). Returns MACH_PORT_NULL if the name is unknown. */
mach_port_name_t svchost_lookup_port(const char *name);

/* Register the example ping service on its own (svchost_init does this;
 * exposed so tests can re-register it after a table reset). */
int svchost_register_ping(void);

/*
 * Kobject entry point, same shape as launchd_process_msg /
 * bks_display_process_msg. Called by the ipc.c dispatch branch for
 * PORT_KOBJECT_SVCHOST. Parses the
 * incoming XPC message, runs the service handler, and builds the reply
 * in reply_buf (header + XPC dict body, msgh_id 0x20000000, reply on
 * hdr->msgh_local_port with MOVE_SEND_ONCE).
 *
 * Returns 1 if a reply was built (*out_reply_size set, 0 for a one-way
 * message with no reply port) or 0 only if no reply could be built at
 * all (reply_buf too small for even the minimal error dict). A return
 * of 1 with a reply port always means a reply dict was built: a
 * send-once reply is never silently dropped (dropping it would deliver
 * MACH_NOTIFY_SEND_ONCE to the client, which libxpc reads as
 * MIG_SERVER_DIED, -308).
 */
int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size);

/* The ping handler, exposed for tests. See svchost.c for the accepted
 * request shapes. */
int svchost_ping_handler(xw_reader *msg, xw_builder *reply,
                         void *ctx);
