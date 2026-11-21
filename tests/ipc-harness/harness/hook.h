/*
 * hook.h - mock g_enqueue_hook for the ipc.c host harness.
 *
 * ipc.c calls the installed hook (if any) on every enqueue: normal
 * sends, kobject replies, timer fires. The mock records (port name,
 * msgh_size) into a fixed log the tests can inspect. The hook runs
 * under ipc_lock, so it never calls back into ipc.c, never locks
 * and never allocates; it only appends to a fixed array with an
 * atomic index. Calls past the log capacity are dropped and
 * counted (see harness_hook_dropped).
 *
 * Host-only code. Nothing in this file ships inside the
 * freestanding dylib.
 *
 * No em dashes are used in this file.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"

#define HARNESS_HOOK_MAX 4096

typedef struct {
    mach_port_name_t port;
    uint32_t size;
} harness_hook_call_t;

/* Install the mock hook and clear the log. */
void harness_hook_install(void);
/* Remove the hook (ipc_set_enqueue_hook(NULL)). The log is kept. */
void harness_hook_uninstall(void);
/* Clear the log (count and dropped counter go back to zero). */
void harness_hook_clear(void);
/* Number of calls recorded (capped at HARNESS_HOOK_MAX). */
size_t harness_hook_count(void);
/* Calls dropped because the log was full. */
size_t harness_hook_dropped(void);
/* The i-th recorded call, or {0, 0} if i is out of range. */
harness_hook_call_t harness_hook_get(size_t i);
/* Index of the first recorded call with this port and size, or -1. */
int harness_hook_find(mach_port_name_t port, uint32_t size);
