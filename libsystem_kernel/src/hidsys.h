#ifndef HIDSYS_H
#define HIDSYS_H

#include "ipc.h"
#include <stdint.h>

/* Kobject kind for com.apple.iohideventsystem service and connection ports.
 * Follows ipc.h PORT_KOBJECT_NOTIFYD (9). */
#define PORT_KOBJECT_HIDSYS                      10

/* IOHIDEventSystem MIG subsystem IDs (IOKit __iohideventsystem_subsystem 70000..70028) */
#define HIDSYS_MIG_BASE                          70000
#define HIDSYS_MIG_END                           70029
#define HIDSYS_MIG_REPLY_OFFSET                  100

#define HIDSYS_MIG_OPEN                          70000
#define HIDSYS_MIG_COPY_MATCHING_SERVICES        70001
#define HIDSYS_MIG_RELEASE_NOTIFICATION          70002 /* simpleroutine */
#define HIDSYS_MIG_CLEAR_SERVICE_CACHE           70003
#define HIDSYS_MIG_DO_CLIENT_REFRESH             70004
#define HIDSYS_MIG_DISPATCH_EVENT                70005 /* simpleroutine */
#define HIDSYS_MIG_SERVICE_CONFORMS_TO           70006
#define HIDSYS_MIG_COPY_EVENT_FOR_SERVICE        70007
#define HIDSYS_MIG_COPY_MATCHING_EVENT_FOR_SVC   70008
#define HIDSYS_MIG_SET_ELEMENT_VALUE_FOR_SVC     70009
#define HIDSYS_MIG_SET_PROPERTIES_FOR_SVC        70010
#define HIDSYS_MIG_COPY_PROPERTY_FOR_SVC         70011
#define HIDSYS_MIG_COPY_PROPERTIES_FOR_SVC       70012
#define HIDSYS_MIG_SET_PROPERTIES                70013
#define HIDSYS_MIG_COPY_PROPERTY                 70014
#define HIDSYS_MIG_QUEUE_CREATE                  70015
#define HIDSYS_MIG_QUEUE_START                   70016
#define HIDSYS_MIG_QUEUE_STOP                    70017
#define HIDSYS_MIG_REGISTER_RECORD_CHANGED       70018 /* simpleroutine */
#define HIDSYS_MIG_UNREGISTER_RECORD_CHANGED     70019 /* simpleroutine */
#define HIDSYS_MIG_REGISTER_RECORD_REMOVED       70020
#define HIDSYS_MIG_REG_PROPERTY_CHANGED_NOTIF    70021
#define HIDSYS_MIG_UNREG_PROPERTY_CHANGED_NOTIF  70022
#define HIDSYS_MIG_REG_EVENT_FILTER_NOTIF        70023
#define HIDSYS_MIG_UNREG_EVENT_FILTER_NOTIF      70024
#define HIDSYS_MIG_REG_SERVICES_CHANGED_NOTIF    70025
#define HIDSYS_MIG_CREATE_VIRTUAL_SERVICE        70026
#define HIDSYS_MIG_UNREG_SERVICES_CHANGED_NOTIF  70027
#define HIDSYS_MIG_DISPATCH_EVENT_VIRTUAL_SVC    70028 /* simpleroutine */

#define HIDSYS_MAX_REPLY                         256
#define HIDSYS_MIG_BAD_ID                        (-303)
#define HIDSYS_MIG_BAD_ARGUMENTS                 (-304)
#define HIDSYS_KERN_NOT_SUPPORTED                46
#define HIDSYS_IORETURN_NOT_FOUND                ((int32_t)0xe00002f0U)

/* Port accessors implemented in ipc.c (must not acquire ipc_lock) */
struct ipc_port *ipc_get_hidsys_port(void);
struct ipc_port *ipc_get_hidsys_conn_port(void);

/* Returns 1 if fake com.apple.iohideventsystem is enabled (default ON unless IOSWINE_HIDSYS=0) */
int hidsys_is_enabled(void);

/* Process an incoming Mach message sent to the iohideventsystem kobject port.
 * Called under ipc_lock; must NOT acquire ipc_lock. */
int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size);

#endif
