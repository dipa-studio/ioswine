#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"

#define LAUNCHD_ROUTINE_REGISTER    0xcc
#define LAUNCHD_ROUTINE_CHECKIN     0xce
#define LAUNCHD_ROUTINE_LOOKUP      0xcf
#define LAUNCHD_ROUTINE_XPC_LOOKUP  0x324
#define LAUNCHD_ROUTINE_DOMAIN_INFO 0x388

/* Service entry for table hook (future services can register real ports here) */
typedef struct {
    const char *name;
    struct ipc_port *(*get_port)(void);
} launchd_service_t;

/* Handle an incoming Mach message addressed to the launchd kobject port.
 * Returns 1 if a reply was constructed in reply_buf (reply_size filled),
 * or 0 if no reply is needed or an error occurred.
 */
int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size);
