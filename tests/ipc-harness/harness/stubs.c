/*
 * stubs.c - host-only stubs for the symbols ipc.c needs from the
 * rest of libsystem_kernel.
 *
 * These are NOT the real implementations. They exist only so ipc.c
 * links on the host for the smoke test.
 *
 * - launchd_process_msg / bks_display_process_msg /
 *   svchost_process_msg / iokit_process_msg: return 0, meaning
 *   "not handled". ipc.c's kobject dispatch then sends no reply
 *   for that message (it still returns MACH_MSG_SUCCESS and fires
 *   the enqueue hook with the reply name and a zero reply size).
 *   So: sends to kobject ports (bootstrap 0x203, BKS display,
 *   service ports, IOKit ports) succeed in the harness but produce
 *   no reply. Do not wait on a reply port for one of those sends
 *   without a timeout. Service tests replace these stubs
 *   with the real dispatch files (launchd.c, bks.c, svchost.c,
 *   iokit.c) or with richer scripted stubs.
 * - mach_absolute_time: CLOCK_MONOTONIC nanoseconds. ipc.c pairs
 *   it with mach_timebase_info {1, 1}, so ticks are nanoseconds
 *   and the receive-timeout math in ipc_rcv_locked works as-is.
 * - mach_timebase_info: {numer = 1, denom = 1}. The struct layout
 *   must match the one ipc.c defines locally (two uint32_t); it
 *   is only ever used through a pointer.
 * - mach_vm_allocate / mach_vm_deallocate: libc mmap/munmap. The
 *   target argument (ipc.c passes 0x103) and the flags are
 *   ignored. Used for OOL message copies and for
 *   ipc_port_names buffers.
 *
 * The four mach_* functions are declared in ipc.c with asm labels
 * ("_mach_absolute_time" and friends), so the definitions here
 * carry the same asm labels or the link fails.
 *
 * Host-only code: libc and pthreads are allowed here. Nothing in
 * this file ships inside the freestanding dylib.
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/mman.h>
#include "ipc.h"

/* ------------------------------------------------------------------ */
/* Kobject dispatch stubs: not handled.                                 */
/* ------------------------------------------------------------------ */

int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}

int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}

int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}

int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size,
                      uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mach time / VM stubs.                                               */
/* ------------------------------------------------------------------ */

/* Layout must match the struct ipc.c defines locally. */
struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};

/* The four mach_* functions are declared in ipc.c with asm labels
 * ("_mach_absolute_time" and friends), so the definitions here
 * repeat the asm label on a separate declaration first: the label
 * attaches to the function and the plain definition below emits
 * the symbol under that name. (An asm label written directly on a
 * definition is rejected by gcc.) */
uint64_t mach_absolute_time(void) __asm__("_mach_absolute_time");
uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int mach_timebase_info(struct mach_timebase_info *info) __asm__("_mach_timebase_info");
int mach_timebase_info(struct mach_timebase_info *info) {
    if (!info) return -1;
    info->numer = 1;
    info->denom = 1;
    return 0;
}

int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target; (void)flags;
    if (!address) return -1;
    if (size == 0) {
        *address = 0;
        return 0;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    *address = (uint64_t)(uintptr_t)p;
    return 0;
}

int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    (void)target;
    if (!address || size == 0) return 0;
    return munmap((void *)(uintptr_t)address, size) == 0 ? 0 : -1;
}

/* Stubs for hooks added to ipc.c (2026-11-13). */
#include <stdint.h>
struct mach_msg_header_stub;
int notifyd_process_msg(const void *hdr, uint32_t send_size, uint8_t *rep, uint32_t cap, uint32_t *out) {
    (void)hdr; (void)send_size; (void)rep; (void)cap; if (out) *out = 0; return 0;
}
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len, uint32_t *out_msgh_id) {
    (void)buf; (void)cap; (void)out_len; (void)out_msgh_id; return 0;
}
