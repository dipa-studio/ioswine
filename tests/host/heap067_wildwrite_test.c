/* heap067_wildwrite_test: ipc.c descriptor walks, OOL ranges, trailer
 * size, and the write-watch harness.
 *
 * Tests for main-thread wild writes where dictionary storage changes
 * underneath during execution. The audit ruled out the signal
 * handlers, the 50th/200th-send hooks, the snapshot solarium
 * prepare/restore, the kevent keventlist copy, and the vm_copy path,
 * and found three real latent ipc.c defects. This test reproduces
 * them. It drives ipc.c + kevent.c + vm.c through the public path.
 * It fails on the unpatched base and passes on the patched tree.
 *
 * W1 descriptor count, send path, forked child: a complex message
 * with msgh_descriptor_count = 10000 and only 2048 bytes of body.
 * Base: the send walk reads desc_ptr off the end of the mmap'd
 * kmsg (rounded to 4096) and the child dies with SIGSEGV (plain) or
 * errors out after reading garbage descriptors from adjacent ASan
 * mappings (ASan). Fixed: the pre-pass rejects the message with
 * MACH_SEND_MSG_TOO_SMALL before the message is copied or any right
 * is consumed.
 *
 * W2 trailer alignment, in-process: a valid 41-byte message received
 * into a buffer with rcv_size = 49 (= 41 + 8, the old formula).
 * Base: the trailer is written at the 4-aligned offset 44, so it
 * overwrites the 0xC3 canary at bytes 49..51. Fixed: the buffer must
 * hold the aligned size plus the trailer (52), so the receive
 * returns MACH_RCV_TOO_LARGE with the canary intact, and a second
 * receive with rcv_size = 64 succeeds and the trailer reads back
 * as type 0, size 8. The second receive uses MACH_RCV_TIMEOUT with
 * timeout 0: on the base tree the first receive already consumed
 * the message, so a blocking second receive would wait forever.
 *
 * W3 OOL inflated size, forked child: a valid OOL descriptor whose
 * size field is 1 MiB while the source region is one 4096-byte
 * page. Base: the send walk copies past the source mapping and the
 * child dies with SIGSEGV. Fixed: the pre-pass range probe rejects
 * the message with MACH_SEND_MSG_TOO_SMALL and the source region
 * is still mapped with its contents intact.
 *
 * W3r good OOL, regression: a valid OOL descriptor (size = 4096,
 * deallocate = 1) sends, the receiver gets the bytes, and the
 * sender's source region is deallocated. Passes on base and fixed.
 *
 * W5 destroy, in-process: ipc_msg_destroy on a message whose OOL
 * descriptor has deallocate = 0 and points at a live region. Base:
 * the destroy deallocates the region anyway (XNU only deallocates
 * when the flag is set), so the live region is gone. Fixed: the
 * region is still mapped with its contents intact. A second part
 * checks that deallocate = 1 is still freed, and that destroying
 * the same message twice does not crash.
 *
 * W4 write-watch harness, in-process: a model dictionary storage
 * (two pages, first page filled with a sentinel, mprotect PROT_READ
 * so any write faults) is watched while the modeled valid paths
 * run: a valid complex send/receive with two port descriptors and
 * a valid OOL descriptor, a valid plain send/receive, and a real
 * vm_copy read from the guarded page. A SIGSEGV handler records
 * the faulting address. No fault and an intact sentinel is the
 * expected (negative) result for the modeled paths: none of them
 * writes into the guarded storage.
 *
 * Build example:
 *   cc -std=gnu11 -O1 -g -w -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DKEVENT_HOST_TEST=1 \
 *      -iquote libsystem_kernel/src -iquote tests/host \
 *      -o heap067_wildwrite_test tests/host/heap067_wildwrite_test.c \
 *      libsystem_kernel/src/ipc.c libsystem_kernel/src/kevent.c libsystem_kernel/src/vm.c -lpthread
 * The same command without -fsanitize builds the plain variant.
 * KEVENT_HOST_TEST compiles out the aarch64 poller-clone trampoline
 * in kevent.c. The host-shim linux.h maps the aarch64 syscall
 * numbers ipc.c / kevent.c / vm.c use onto x86_64 syscall() numbers,
 * plus the msync dispatch (227->26) that ipc.c's OOL range probe
 * needs. The _mach_vm_allocate / _mach_vm_deallocate malloc stubs
 * from kevent_pset_test.c are deliberately NOT used here: the real
 * vm.c is compiled, so OOL regions are real mmaps in the hint
 * range with the 2 MiB gaps that make the W3 over-read fault.
 *
 * No Apple code, no device needed.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "workq.h"
#include "ipc.h"

extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
extern int vm_copy(uint32_t target_task, uint64_t source_address, uint64_t size, uint64_t dest_address) __asm__("_vm_copy");
void ipc_msg_destroy(void *msg);

/* ------------------------------------------------------------------ */
/* Stubs for symbols the real kevent.c / ipc.c / vm.c need.           */
/* Copied from kevent_pset_test.c, except the _mach_vm_allocate /      */
/* _mach_vm_deallocate malloc stubs are dropped (the real vm.c is     */
/* compiled) and linux_to_darwin_errno is stubbed (errno.c cannot     */
/* compile on x86_64: it uses inline "mrs tpidr_el0" asm).            */
/* ------------------------------------------------------------------ */

/* Mach time: a counter that advances, so timeouts and deadlines behave. */
static uint64_t g_now_ticks = 1000000;
uint64_t _mach_absolute_time(void) {
    g_now_ticks += 1000;
    return g_now_ticks;
}
int _mach_timebase_info(void *info) {
    uint32_t *p = (uint32_t *)info;
    p[0] = 1;
    p[1] = 1;
    return 0;
}
uint32_t _mach_thread_self(void) {
    return 0x303; /* main thread */
}
static int g_snapshot_inits;
void snapshot_init(void) {
    g_snapshot_inits++;
}
static int g_carender_inits;
void carender_init(void) {
    g_carender_inits++;
}

/* Kobject service handlers live in other LSK files that this test
 * does not compile. The test never sends a message to a service port,
 * so these stubs are never called; they exist only to satisfy the
 * linker. Each returns 0 ("not handled"). */
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
int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int hidsys_is_enabled(void) {
    return 0;
}
int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap,
                        uint32_t *out_reply_size) {
    (void)msg; (void)send_size; (void)reply_buf; (void)reply_cap;
    (void)out_reply_size;
    return 0;
}
void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint) {
    (void)service_port;
    (void)endpoint;
}
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                           uint32_t *out_msgh_id) {
    (void)buf; (void)cap; (void)out_len; (void)out_msgh_id;
    return 0;
}
void set_errno(int err) {
    (void)err;
}
int linux_to_darwin_errno(long linux_err) {
    if (linux_err >= 0) return 0;
    return (int)-linux_err;
}

/* Workqueue stubs: no kevent is ever delivered in this test. */
int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos) {
    (void)wlh; (void)ev; (void)nevents; (void)qos;
    return 1;
}
void workq_lock(void) {}
void workq_unlock(void) {}
int workq_is_wlh_busy(uint64_t wlh) {
    (void)wlh;
    return 0;
}
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    (void)wlh;
    (void)busy;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Small harness.                                                     */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("FAIL: %s\n", what);
        /* Flush: a later scenario may fault the parent (that is the
         * point of a crash-repro test), and buffered output would be
         * lost with it. */
        fflush(stdout);
    }
}

/* Returns 1 when the range [addr, addr+len) is mapped, 0 otherwise.
 * libc mincore: -1 with ENOMEM when any page in the range is
 * unmapped. Used only to observe whether a region survived. */
static int range_mapped(uint64_t addr, size_t len) {
    unsigned char vec[4];
    if (len == 0) return 1;
    if (mincore((void *)(uintptr_t)addr, len, vec) == 0) return 1;
    return 0;
}

static mach_port_name_t g_dest;

static void setup_world(void) {
    mach_port_name_t name = 0;
    int r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &name);
    if (r != 0) {
        printf("setup: ipc_port_allocate failed %d\n", r);
        exit(2);
    }
    r = ipc_port_insert_right(0x103, name, name, MACH_MSG_TYPE_MAKE_SEND);
    if (r != 0) {
        printf("setup: ipc_port_insert_right failed %d\n", r);
        exit(2);
    }
    g_dest = name;
}

static int send_buf(uint8_t *buf, uint32_t size) {
    return ipc_msg(buf, MACH_SEND_MSG, size, 0, 0, 0, 0, NULL, 0);
}

static int recv_buf(uint8_t *buf, uint32_t rcv_size, uint64_t option) {
    ((mach_msg_header_t *)buf)->msgh_size = rcv_size;
    return ipc_msg(buf, option, 0, rcv_size, g_dest, 0, 0, NULL, 0);
}

/* Drain every queued message on g_dest. Returns the number drained. */
static int drain_all(void) {
    int n = 0;
    uint8_t buf[8192];
    while (1) {
        int r = recv_buf(buf, sizeof buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT);
        if (r != 0) break;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* W2: trailer alignment.                                             */
/* ------------------------------------------------------------------ */

static void test_w2_trailer(void) {
    uint8_t sendb[64];
    uint8_t recvb[64];
    int r;

    memset(sendb, 0, sizeof sendb);
    mach_msg_header_t *hdr = (mach_msg_header_t *)sendb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    hdr->msgh_size = 41;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x2222;
    for (int i = 0; i < 17; i++) sendb[24 + i] = (uint8_t)(0xA0 + i);

    r = send_buf(sendb, 41);
    check(r == 0, "w2: send of 41-byte message succeeds");

    /* Receive with rcv_size = 49 = 41 + 8 (the old formula). The
     * canary at bytes 49..51 sits where the trailer lands when the
     * buffer only fits the old size. */
    memset(recvb, 0, sizeof recvb);
    recvb[49] = 0xC3; recvb[50] = 0xC3; recvb[51] = 0xC3;
    r = recv_buf(recvb, 49, MACH_RCV_MSG | MACH_RCV_LARGE);
    check(r == MACH_RCV_TOO_LARGE, "w2: exact-fit receive is too large");
    check(recvb[49] == 0xC3 && recvb[50] == 0xC3 && recvb[51] == 0xC3,
          "w2: canary past the old size is intact");

    /* The message stays queued (MACH_RCV_LARGE). Re-receive it with
     * room for the aligned size plus the trailer. The re-receive
     * uses MACH_RCV_TIMEOUT with timeout 0 (recv_buf passes 0): on
     * the base tree the exact-fit receive above already consumed
     * the message, so a blocking receive here would wait forever. */
    memset(recvb, 0, sizeof recvb);
    r = recv_buf(recvb, 64, MACH_RCV_MSG | MACH_RCV_TIMEOUT);
    check(r == 0, "w2: re-receive with 64 bytes succeeds");
    if (r != 0) return;
    check(((mach_msg_header_t *)recvb)->msgh_size == 41, "w2: received size is 41");
    check(recvb[24] == 0xA0 && recvb[40] == 0xB0, "w2: payload intact");
    /* Trailer at the 4-aligned offset 44: type 0, size 8. */
    check(*(uint32_t *)(recvb + 44) == 0 && *(uint32_t *)(recvb + 48) == 8,
          "w2: trailer reads back as type 0 size 8");
}

/* ------------------------------------------------------------------ */
/* W3r: good OOL, regression. W5: destroy.                            */
/* ------------------------------------------------------------------ */

static void test_w3r_good_ool(void) {
    uint64_t src = 0;
    int r = mach_vm_allocate(0x103, &src, 4096, 1);
    check(r == 0, "w3r: ool source allocates");
    if (r != 0) return;
    memset((void *)(uintptr_t)src, 0x5A, 4096);

    uint8_t msgb[64];
    memset(msgb, 0, sizeof msgb);
    mach_msg_header_t *hdr = (mach_msg_header_t *)msgb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = 44;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x3333;
    mach_msg_body_t *body = (mach_msg_body_t *)(msgb + 24);
    body->msgh_descriptor_count = 1;
    /* Build the descriptor in an aligned local, then memcpy it in:
     * the wire layout puts it at offset 28, 4-aligned, so writing
     * its 8-byte address field through a struct pointer would be
     * an unaligned access (UBSan flags it). */
    mach_msg_ool_descriptor_t ool;
    memset(&ool, 0, sizeof ool);
    ool.address = (void *)(uintptr_t)src;
    ool.deallocate = 1;
    ool.copy = 0;
    ool.pad1 = 0;
    ool.type = MACH_MSG_OOL_DESCRIPTOR;
    ool.size = 4096;
    memcpy(msgb + 28, &ool, sizeof ool);

    r = send_buf(msgb, 44);
    check(r == 0, "w3r: good ool send succeeds");
    /* The send consumed the sender's region (deallocate = 1). */
    check(!range_mapped(src, 4096), "w3r: sender source deallocated by send");

    uint8_t recvb[8192];
    memset(recvb, 0, sizeof recvb);
    r = recv_buf(recvb, sizeof recvb, MACH_RCV_MSG);
    check(r == 0, "w3r: good ool receive succeeds");
    if (r != 0) return;
    mach_msg_ool_descriptor_t rool;
    memcpy(&rool, recvb + 28, sizeof rool);
    check(rool.type == MACH_MSG_OOL_DESCRIPTOR, "w3r: received descriptor is ool");
    check(rool.size == 4096, "w3r: received ool size is 4096");
    uint8_t *data = (uint8_t *)(uintptr_t)rool.address;
    int ok = 1;
    for (int i = 0; i < 4096; i++) if (data[i] != 0x5A) { ok = 0; break; }
    check(ok, "w3r: received ool bytes match the source pattern");
}

static void build_ool_msg(uint8_t *msgb, uint64_t addr, uint8_t deallocate, uint32_t sz) {
    memset(msgb, 0, 64);
    mach_msg_header_t *hdr = (mach_msg_header_t *)msgb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = 44;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x5555;
    mach_msg_body_t *body = (mach_msg_body_t *)(msgb + 24);
    body->msgh_descriptor_count = 1;
    /* Aligned local, memcpy in: the wire offset 28 is 4-aligned. */
    mach_msg_ool_descriptor_t ool;
    memset(&ool, 0, sizeof ool);
    ool.address = (void *)(uintptr_t)addr;
    ool.deallocate = deallocate;
    ool.copy = 0;
    ool.pad1 = 0;
    ool.type = MACH_MSG_OOL_DESCRIPTOR;
    ool.size = sz;
    memcpy(msgb + 28, &ool, sizeof ool);
}

static void test_w5_destroy(void) {
    uint64_t region = 0;
    int r = mach_vm_allocate(0x103, &region, 4096, 1);
    check(r == 0, "w5: region allocates");
    if (r != 0) return;
    memset((void *)(uintptr_t)region, 0x6B, 4096);

    /* deallocate = 0: the sender still owns the region, so destroying
     * the unsent message must not free it. The contents are read only
     * when the region is still mapped: on the base tree the destroy
     * frees it, and reading a freed region faults the parent. */
    uint8_t msgb[64];
    build_ool_msg(msgb, region, 0, 4096);
    ipc_msg_destroy(msgb);
    check(range_mapped(region, 4096), "w5: destroy with deallocate=0 keeps the region");
    if (range_mapped(region, 4096)) {
        check(*(uint8_t *)(uintptr_t)region == 0x6B, "w5: region contents intact after destroy");
    } else {
        check(0, "w5: region contents intact after destroy");
    }

    /* deallocate = 1: the region is consumed, so destroying the
     * unsent message frees it. */
    build_ool_msg(msgb, region, 1, 4096);
    ipc_msg_destroy(msgb);
    check(!range_mapped(region, 4096), "w5: destroy with deallocate=1 frees the region");

    /* Destroying the same message twice must not crash and must not
     * unmap anything else. */
    ipc_msg_destroy(msgb);
    check(!range_mapped(region, 4096), "w5: second destroy leaves the region freed");
}

/* ------------------------------------------------------------------ */
/* W1 and W3: forked children. The child resets SIGSEGV to the       */
/* default first (so a fault kills it instead of being caught) and    */
/* exits with _exit, never exit. Exit code 42 means the clean        */
/* reject the fixed tree returns; anything else is a failure.        */
/* ------------------------------------------------------------------ */

static int run_child(int (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        return -1;
    }
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        _exit(fn());
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* W1: count = 10000, body 2048 bytes. Base: the send walk reads off
 * the end of the mmap'd kmsg and the child dies with SIGSEGV (139).
 * Fixed: MACH_SEND_MSG_TOO_SMALL before any copy. */
static int child_w1(void) {
    static uint8_t msgb[2048];
    memset(msgb, 0, sizeof msgb);
    mach_msg_header_t *hdr = (mach_msg_header_t *)msgb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = 2048;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x1111;
    mach_msg_body_t *body = (mach_msg_body_t *)(msgb + 24);
    body->msgh_descriptor_count = 10000;
    int r = send_buf(msgb, 2048);
    if (r == MACH_SEND_MSG_TOO_SMALL) return 42;
    return 43;
}

/* W3: OOL size 1 MiB, source one 4096-byte page. Base: the send walk
 * copies past the source mapping and the child dies with SIGSEGV
 * (139). Fixed: MACH_SEND_MSG_TOO_SMALL and the source is intact. */
static int child_w3(void) {
    uint64_t src = 0;
    int r = mach_vm_allocate(0x103, &src, 4096, 1);
    if (r != 0) return 44;
    memset((void *)(uintptr_t)src, 0x5A, 4096);
    uint8_t msgb[64];
    build_ool_msg(msgb, src, 1, 0x100000);
    r = send_buf(msgb, 44);
    if (r != MACH_SEND_MSG_TOO_SMALL) return 43;
    if (!range_mapped(src, 4096)) return 43;
    if (*(uint8_t *)(uintptr_t)src != 0x5A || ((uint8_t *)(uintptr_t)src)[4095] != 0x5A) return 43;
    return 42;
}

/* ------------------------------------------------------------------ */
/* W4: write-watch harness.                                           */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_watch_faulted;
static volatile uintptr_t g_watch_fault_addr;
static sigjmp_buf g_watch_jmp;

static void watch_segv_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    g_watch_faulted = 1;
    g_watch_fault_addr = (uintptr_t)info->si_addr;
    siglongjmp(g_watch_jmp, 1);
}

static uint8_t *g_watch_page;
static uint32_t g_watch_sentinel = 0xD1C7D1C7u;

static int watch_pattern_intact(void) {
    uint32_t *p = (uint32_t *)g_watch_page;
    for (int i = 0; i < 1024; i++) if (p[i] != g_watch_sentinel) return 0;
    return 1;
}

/* Run one modeled path with the guard armed. Returns 1 when the path
 * completed without faulting and the sentinel is intact. */
static int watch_run(const char *name, void (*fn)(void)) {
    g_watch_faulted = 0;
    g_watch_fault_addr = 0;
    if (sigsetjmp(g_watch_jmp, 1) == 0) {
        fn();
    }
    if (g_watch_faulted) {
        printf("watch: fault at %p during %s\n", (void *)g_watch_fault_addr, name);
        return 0;
    }
    if (!watch_pattern_intact()) {
        printf("watch: sentinel changed during %s\n", name);
        return 0;
    }
    return 1;
}

static void watch_path_complex_ool(void) {
    /* Valid complex send with two port descriptors and one valid OOL
     * descriptor, then a receive. */
    uint64_t src = 0;
    if (mach_vm_allocate(0x103, &src, 4096, 1) != 0) return;
    memset((void *)(uintptr_t)src, 0x3C, 4096);
    uint8_t msgb[128];
    memset(msgb, 0, sizeof msgb);
    mach_msg_header_t *hdr = (mach_msg_header_t *)msgb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    hdr->msgh_size = 24 + 4 + 12 + 12 + 16;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x4444;
    mach_msg_body_t *body = (mach_msg_body_t *)(msgb + 24);
    body->msgh_descriptor_count = 3;
    mach_msg_port_descriptor_t *p1 = (mach_msg_port_descriptor_t *)(msgb + 28);
    p1->name = g_dest;
    p1->pad1 = 0;
    p1->pad2 = 0;
    p1->disposition = MACH_MSG_TYPE_COPY_SEND;
    p1->type = MACH_MSG_PORT_DESCRIPTOR;
    mach_msg_port_descriptor_t *p2 = (mach_msg_port_descriptor_t *)(msgb + 40);
    p2->name = g_dest;
    p2->pad1 = 0;
    p2->pad2 = 0;
    p2->disposition = MACH_MSG_TYPE_COPY_SEND;
    p2->type = MACH_MSG_PORT_DESCRIPTOR;
    /* Aligned local, memcpy in: the wire offset 52 is 4-aligned. */
    mach_msg_ool_descriptor_t ool;
    memset(&ool, 0, sizeof ool);
    ool.address = (void *)(uintptr_t)src;
    ool.deallocate = 1;
    ool.copy = 0;
    ool.pad1 = 0;
    ool.type = MACH_MSG_OOL_DESCRIPTOR;
    ool.size = 4096;
    memcpy(msgb + 52, &ool, sizeof ool);
    int r = send_buf(msgb, 24 + 4 + 12 + 12 + 16);
    if (r != 0) return;
    uint8_t recvb[8192];
    memset(recvb, 0, sizeof recvb);
    recv_buf(recvb, sizeof recvb, MACH_RCV_MSG);
}

static void watch_path_plain(void) {
    uint8_t sendb[64];
    memset(sendb, 0, sizeof sendb);
    mach_msg_header_t *hdr = (mach_msg_header_t *)sendb;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    hdr->msgh_size = 40;
    hdr->msgh_remote_port = g_dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = 0x4445;
    int r = send_buf(sendb, 40);
    if (r != 0) return;
    uint8_t recvb[256];
    memset(recvb, 0, sizeof recvb);
    recv_buf(recvb, sizeof recvb, MACH_RCV_MSG);
}

static void watch_path_vm_copy(void) {
    uint8_t scratch[64];
    memset(scratch, 0, sizeof scratch);
    /* Read 64 bytes from the guarded page (PROT_READ allows reads)
     * into a scratch buffer. A write into the guard would fault. */
    vm_copy(0x103, (uint64_t)(uintptr_t)g_watch_page, 64, (uint64_t)(uintptr_t)scratch);
}

static void test_w4_watch(void) {
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = watch_segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &old_sa) != 0) {
        check(0, "w4: sigaction installs");
        return;
    }

    g_watch_page = (uint8_t *)mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(g_watch_page != MAP_FAILED, "w4: guard pages mmap");
    if (g_watch_page == MAP_FAILED) {
        sigaction(SIGSEGV, &old_sa, NULL);
        return;
    }
    uint32_t *p = (uint32_t *)g_watch_page;
    for (int i = 0; i < 1024; i++) p[i] = g_watch_sentinel;
    if (mprotect(g_watch_page, 4096, PROT_READ) != 0) {
        check(0, "w4: guard page mprotect to read-only");
        munmap(g_watch_page, 8192);
        sigaction(SIGSEGV, &old_sa, NULL);
        return;
    }

    check(watch_run("complex ool", watch_path_complex_ool),
          "w4: valid complex ool send/receive does not touch the guard");
    check(watch_run("plain", watch_path_plain),
          "w4: valid plain send/receive does not touch the guard");
    check(watch_run("vm_copy read", watch_path_vm_copy),
          "w4: vm_copy read from the guard does not touch the guard");

    mprotect(g_watch_page, 4096, PROT_READ | PROT_WRITE);
    munmap(g_watch_page, 8192);
    sigaction(SIGSEGV, &old_sa, NULL);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    setup_world();

    test_w2_trailer();
    test_w3r_good_ool();
    test_w5_destroy();

    int w1 = run_child(child_w1);
    check(w1 == 42, "w1: corrupt descriptor count is rejected cleanly");
    if (w1 != 42) printf("w1: child returned %d (139 = SIGSEGV on base)\n", w1);
    drain_all();

    int w3 = run_child(child_w3);
    check(w3 == 42, "w3: inflated ool size is rejected cleanly, source intact");
    if (w3 != 42) printf("w3: child returned %d (139 = SIGSEGV on base)\n", w3);
    drain_all();

    test_w4_watch();

    printf("heap067_wildwrite: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
