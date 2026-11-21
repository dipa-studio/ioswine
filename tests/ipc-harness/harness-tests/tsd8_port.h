/*
 * tsd8_port.h - host port of the mach.c TSD[8] special-reply-port
 * logic.
 *
 * The real functions live in libsystem_kernel/src/mach.c and read
 * the thread's Darwin TSD base from tpidr_el0
 * (lsk_thread_get_special_reply_port, mach.c:80-96, and the
 * TSD-clearing half of lsk_mach_port_destroy, mach.c:376-389).
 * That code cannot run on the x86_64 host: the `mrs tpidr_el0`
 * inline asm is aarch64-only. This file ports the two functions
 * verbatim except for the TSD base source, which is a pthread TLS
 * block instead of tpidr_el0. Slot offsets match mach.c exactly:
 * TSD[2] at +16, TSD[8] at +64.
 *
 * TSD8_PATCHED selects the logic: 0 reproduces the current mach.c
 * (return the cached name if nonzero and not DEAD, no receive-right
 * recheck; destroy clears TSD[2] only), 1 implements patch item 6
 * (M8: re-validate the cached name with ipc_port_has_receive_right
 * on get, and clear TSD[8] on destroy). build.sh passes
 * -DTSD8_PATCHED=0 for IPC_SRC=base and =1 for IPC_SRC=patched.
 *
 * The patched mode validates this host port of the intended fix,
 * not actual mach.c; the real mach.c fix must be
 * verified by compiling mach.c for aarch64 and on the device.
 *
 * Host-only code. Nothing here ships inside the freestanding dylib.
 *
 * No em dashes are used in this file.
 */
#pragma once
#include <stdint.h>
#include "ipc.h"

/* Initialise the host TSD (pthread TLS). Call once before use. */
void tsd8_init(void);
/* Host port of lsk_thread_get_special_reply_port (mach.c:80-96). */
mach_port_name_t tsd8_get_special_reply_port(void);
/* Host port of the TSD-clearing half of lsk_mach_port_destroy
 * (mach.c:376-389): destroys the port and clears the cached TSD
 * slot(s). Returns the ipc_port_destroy result. */
int tsd8_destroy_port(mach_port_name_t name);
