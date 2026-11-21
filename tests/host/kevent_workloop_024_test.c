/* kevent_workloop_024_test.c - Host tests for libdispatch
 * kevent/workloop traps.
 *
 * Trap (a) __dispatch_kq_poll+392 "BUG IN LIBDISPATCH: Invalid wlh":
 * libdispatch traps when __dispatch_kq_poll is called with wlh == 0. Every
 * kq_poll caller in the 23G90 libdispatch binary passes a wlh from one of
 * three sources: dsc->dc_data (wait_for_ownership, cancel_waiter: provably
 * never 0, a queue pointer or DISPATCH_WLH_ANON), ddi_wlh (the
 * kernel-provided *workloop_id), or a live queue pointer (poke, leave,
 * timer). The emulation side can hand libdispatch a zero handle through
 * workq_dispatch_kevent(0, ...) (no guard: the owned-check explicitly
 * skips 0 and workq_set_wlh_busy(0, 1) is a no-op success), through a
 * knote with wlh == 0 reaching kevent_get_next_ready (only possible if
 * kevent_id(0, ...) created one: lsk_kevent_id had no rejection), or
 * through the trampoline handing __pthread_wqthread WORKLOOP flags with
 * bound_wlh == 0. A WORKLOOP worker with *workloop_id == 0 falls back to
 * the kevent worker (queue.c:6878-6880), so the trap itself fires later,
 * when a leave/drain path reads the absent adopted wlh from TLS and
 * passes it to kq_poll. The exact device-side caller that passes the
 * zero is [guess]; the fix closes every boundary by which a zero handle
 * can reach libdispatch, and the qrun census is the arbiter.
 *
 * Trap (b) __dispatch_event_loop_wait_for_ownership.cold.4 via
 * _dispatch_event_loop_cancel_waiter: the sync-waiter ownership handoff
 * is never observed. Root cause: the emulation treated SYNC_DISCOVER as
 * a no-op probe (kevent.c, the old NOTE_WL_DISCOVER_OWNER comment) and
 * never learned the owner from the debounce value, although XNU's
 * filt_wlupdate does (3988-4024). A waiter that sent DISCOVER naming
 * the servicer then saw owner == 0 and, if no servicer was busy, took
 * ownership itself while dq_state still said the servicer held the
 * drain lock, so two threads drained one queue and the handoff the
 * waiter waited for never arrived. The fix learns the owner from the
 * fresh debounce value (live & ~3), forgets a servicer owner in the
 * WORKLOOP_RETURN/THREAD_RETURN backstop (XNU filt_wlupdate 4117-4123,
 * servicers are never owners), and compares owners with the low 2 bits
 * clear (dq_state stores the masked tid, sync changes carry raw TSD[3]).
 * A second, smaller fix: an EV_ERROR receipt for a stale workloop change
 * now carries the freshly loaded debounce value in ext[3] (XNU
 * filt_wlremember_last_update; libdispatch's poke_drain reads dq_state
 * from ext[3] on EV_ERROR, event_kevent.c:1788) instead of the stale
 * snapshot the caller built the change with.
 *
 * Gates (FAIL on the unpatched tree, PASS on the patched tree):
 *   test_discover_learns_owner, test_waiter_blocks_against_discovered_owner,
 *   test_backstop_forgets_servicer_owner, test_kevent_id_zero_rejected,
 *   test_get_next_ready_skips_zero_wlh, test_error_event_ext3_refresh.
 * Pins (PASS on both trees, guard the new behavior):
 *   test_reentrant_wait_against_discovered_owner,
 *   test_uncontended_wait_still_works.
 * The DISCOVER / ESTALE tests need a state word mmap'd inside the
 * debounce guest window (0x100000000, 0x7fffffffff); under ASan the
 * window is owned by the sanitizer runtime, so those tests print SKIP
 * and the rest still run (the plain variant always maps the word, so
 * the gates run there).
 *
 * Build: like workloop_race_test, REAL kevent.c + ipc.c on the host.
 *   cc -std=gnu11 -O1 -g -w -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DKEVENT_HOST_TEST=1 \
 *      -iquote <src> -iquote tests/host \
 *      -o kevent_workloop_024_test tests/host/kevent_workloop_024_test.c \
 *      <src>/kevent.c <src>/ipc.c -lpthread
 * Plain cc + ASan/UBSan on Linux host. No Apple code, no device.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>

#include "workq.h"
#include "ipc.h"

/* lsk_kevent_id has no header declaration (it is the _kevent_id syscall
 * entry in kevent.c); declared here for the zero-id gate test. */
int lsk_kevent_id(uint64_t id, const void *changelist, int nchanges,
                  void *eventlist, int nevents,
                  void *data_out, size_t *data_available, unsigned int flags) __asm__("_kevent_id");

/* ------------------------------------------------------------------ */
/* Stubs for symbols the real kevent.c / ipc.c need.                  */
/* ------------------------------------------------------------------ */

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

/* Thread-aware _mach_thread_self: the main thread is 0x303 (the stub
 * value the other host tests use); helper threads set their own tid
 * through set_self_tid(). kevent.c reaches this through the
 * _mach_thread_self asm alias. */
static pthread_key_t g_self_tid_key;
static int g_self_key_created;
uint32_t _mach_thread_self(void) {
    if (!g_self_key_created) return 0x303;
    void *v = pthread_getspecific(g_self_tid_key);
    if (!v) return 0x303;
    return (uint32_t)(uintptr_t)v;
}
static void set_self_tid(uint32_t tid) {
    if (!g_self_key_created) {
        pthread_key_create(&g_self_tid_key, NULL);
        g_self_key_created = 1;
    }
    pthread_setspecific(g_self_tid_key, (void *)(uintptr_t)tid);
}
static void clear_self_tid(void) {
    if (g_self_key_created) pthread_setspecific(g_self_tid_key, NULL);
}

int _mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target;
    (void)flags;
    void *p = malloc(size ? size : 1);
    if (!p) return 6; /* KERN_RESOURCE_SHORTAGE */
    *address = (uint64_t)p;
    return 0;
}
int _mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    (void)target;
    (void)size;
    free((void *)address);
    return 0;
}
static int g_snapshot_inits;
void snapshot_init(void) {
    g_snapshot_inits++;
}
/* ipc.c calls carender_init() from the ipc lazy arm, so this test
 * needs a counting stub. No check on the counter: this test sends
 * no MACH_SEND_MSG, so the arm never fires here. */
static int g_carender_inits;
void carender_init(void) {
    g_carender_inits++;
}

/* Kobject service handlers live in other LSK files that this test does
 * not compile. The test never sends a message to a service port, so
 * these stubs are never called; they exist only to satisfy the linker.
 * Each returns 0 ("not handled"). */
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

/* Recording set_errno: lsk_kevent_id(0) must fail with EINVAL (22). */
static int g_last_errno = -1;
void set_errno(int err) {
    g_last_errno = err;
}

/* ------------------------------------------------------------------ */
/* Faithful workq model: busy table + owner check + dispatch record   */
/* (workq.c itself is aarch64-only and cannot compile on the x86_64   */
/* host). Models the real workq.c, including the zero-handle          */
/* guard: a dispatch for wlh 0 is refused and counted, never          */
/* recorded as a worker.                                              */
/* ------------------------------------------------------------------ */

#define WLH_A 0x50694c800ULL
#define WLH_B 0x5014b8680ULL
#define WLH_C 0x5014b8700ULL
#define WLH_D 0x5014b8780ULL
#define WLH_E 0x5014b8800ULL
#define WLH_F 0x5014b8880ULL
#define QOS 5
#define SERVICER_TID 0x500u
#define WAITER_TID 0x404u

#define MAX_DISPATCHED 128
struct dispatched_ev {
    uint64_t wlh;
    int nevents;
    int qos;
};
static struct dispatched_ev g_dispatched[MAX_DISPATCHED];
static int g_dispatched_count;
static int g_dispatch_held;
static int g_zero_dispatch_attempts;

#define MAX_BUSY_MODEL 64
static uint64_t g_busy_model[MAX_BUSY_MODEL];
static int g_busy_model_count;

static int model_busy(uint64_t wlh) {
    for (int i = 0; i < g_busy_model_count; i++) {
        if (g_busy_model[i] == wlh) return 1;
    }
    return 0;
}

static int model_set_busy(uint64_t wlh, int busy) {
    if (wlh == 0) return 1;
    if (busy) {
        if (model_busy(wlh)) return 1;
        if (g_busy_model_count < MAX_BUSY_MODEL) {
            g_busy_model[g_busy_model_count++] = wlh;
            return 1;
        }
        return 0;
    }
    for (int i = 0; i < g_busy_model_count; i++) {
        if (g_busy_model[i] == wlh) {
            g_busy_model[i] = g_busy_model[--g_busy_model_count];
            break;
        }
    }
    return 1;
}

int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos) {
    if (wlh == 0) {
        g_zero_dispatch_attempts++;
        return 0;
    }
    if (wlh != DISPATCH_WLH_ANON && kevent_wl_owner_locked(wlh) != 0) {
        g_dispatch_held++;
        return 0;
    }
    if (model_busy(wlh)) {
        g_dispatch_held++;
        return 0;
    }
    if (!model_set_busy(wlh, 1)) {
        g_dispatch_held++;
        return 0;
    }
    if (g_dispatched_count < MAX_DISPATCHED) {
        struct dispatched_ev *d = &g_dispatched[g_dispatched_count++];
        d->wlh = wlh;
        d->nevents = nevents;
        d->qos = qos;
    }
    (void)ev;
    return 1;
}
void workq_lock(void) {}
void workq_unlock(void) {}
int workq_is_wlh_busy(uint64_t wlh) {
    return model_busy(wlh);
}
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    return model_set_busy(wlh, busy);
}

/* Servicer return for wlh, as lsk_workq_kernreturn does on
 * WQOPS_THREAD_WORKLOOP_RETURN: clear busy, then wake a blocked
 * sync waiter if one is still waiting (kevent_workloop_wake_waiter). */
static void servicer_return(uint64_t wlh) {
    workq_set_wlh_busy(wlh, 0);
    kevent_workloop_wake_waiter(wlh);
}

/* The model's workloop state word: the host stand-in for the guest
 * workloop struct the device's ext[1] points at. It is mmap'd at a
 * fixed address inside wl_debounce_locked's guest window
 * (0x100000000, 0x7fffffffff): a plain .bss address does NOT work,
 * because this host's PIE base sits above the window ceiling and the
 * debounce rejects it as ESTALE. */
static uint64_t *g_wl_state_ptr;
static uint64_t wl_state_addr(void) { return (uint64_t)g_wl_state_ptr; }
static int wl_addr_in_window(uint64_t a) {
    return a > 0x100000000ULL && a < 0x7fffffffffULL && (a & 7) == 0;
}
static int alloc_wl_state(void) {
    static const uint64_t hints[] = {
        0x200000000ULL, 0x300000000ULL, 0x400000000ULL,
        0x500000000ULL, 0x600000000ULL,
    };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        void *p = mmap((void *)hints[i], 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            g_wl_state_ptr = (uint64_t *)p;
            *g_wl_state_ptr = 0;
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        void *p = mmap((void *)hints[i], 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && wl_addr_in_window((uint64_t)p)) {
            g_wl_state_ptr = (uint64_t *)p;
            *g_wl_state_ptr = 0;
            return 1;
        }
        if (p != MAP_FAILED) munmap(p, 4096);
    }
    {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && wl_addr_in_window((uint64_t)p)) {
            g_wl_state_ptr = (uint64_t *)p;
            *g_wl_state_ptr = 0;
            return 1;
        }
        if (p != MAP_FAILED) munmap(p, 4096);
    }
    g_wl_state_ptr = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;
static int g_skips;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("FAIL: %s\n", what);
    }
}

static void clear_dispatched(void) {
    g_dispatched_count = 0;
    g_dispatch_held = 0;
    g_zero_dispatch_attempts = 0;
}

/* A SYNC_DISCOVER change exactly as libdispatch builds it in
 * _dispatch_event_loop_wait_for_ownership (23G90): ident is the
 * drain-lock owner port ((dq_state & ~3) | 3), fflags carry SYNC_WAKE |
 * DISCOVER_OWNER | IGNORE_ESTALE, ext[1] is &dq_state, ext[2] is the
 * debounce mask, ext[3] is the dq_state snapshot. */
static struct kevent_qos_s discover_change(uint64_t wlh, uint64_t dq_state) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = (dq_state & ~3ULL) | 3ULL;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_SYNC_WAKE | NOTE_WL_DISCOVER_OWNER | NOTE_WL_IGNORE_ESTALE;
    ch.udata = wlh;
    ch.ext[0] = 0;
    ch.ext[1] = wl_state_addr();
    ch.ext[2] = ~0ULL; /* full mask: any dq_state change is stale */
    ch.ext[3] = dq_state;
    return ch;
}

static struct kevent_qos_s sync_wait_change(uint32_t tid) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = tid;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_SYNC_WAIT;
    return ch;
}

static struct kevent_qos_s sync_end_change(uint32_t tid) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = tid;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    ch.fflags = NOTE_WL_SYNC_WAKE | NOTE_WL_END_OWNERSHIP;
    return ch;
}

static struct kevent_qos_s thread_request_change(uint64_t wlh) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = wlh;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_THREAD_REQUEST;
    ch.udata = wlh;
    ch.ext[0] = 0;
    ch.ext[1] = wl_state_addr();
    ch.ext[2] = 0;
    ch.ext[3] = 0;
    return ch;
}

/* Gate 1 (trap b): DISCOVER learns the owner from the fresh debounce
 * value. Unpatched, the owner stays 0: the waiter then takes ownership
 * itself while dq_state says the servicer holds the drain lock. */
static void test_discover_learns_owner(void) {
    if (g_wl_state_ptr == NULL) {
        g_skips++;
        printf("SKIP test_discover_learns_owner: no in-window state word (ASan owns the debounce window)\n");
        return;
    }
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    clear_dispatched();
    *g_wl_state_ptr = SERVICER_TID; /* dq_state whose owner is the servicer */
    ch = discover_change(WLH_A, SERVICER_TID);
    n = kevent_apply_changes(WLH_A, &ch, 1, ev, 1, 0);
    check(n == 0, "DISCOVER attach returns 0 events");
    check(kevent_wl_owner_locked(WLH_A) == SERVICER_TID,
          "DISCOVER learns the drain-lock owner from dq_state");
    check(g_dispatched_count == 0, "DISCOVER dispatches nothing");
    check(g_zero_dispatch_attempts == 0, "DISCOVER asks for no zero dispatch");
    /* Cleanup: delete the DISCOVER knote (ident is the owner port) with
     * END_OWNERSHIP, which also releases the learned owner. */
    memset(&ch, 0, sizeof(ch));
    ch.ident = (SERVICER_TID & ~3U) | 3U;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    ch.fflags = NOTE_WL_END_OWNERSHIP;
    n = kevent_apply_changes(WLH_A, &ch, 1, ev, 1, 0);
    check(n == 0, "DISCOVER knote delete returns 0 events");
    check(kevent_wl_owner_locked(WLH_A) == 0, "owner released after DISCOVER knote delete");
}

/* Gate 2 (trap b): the full handoff. A waiter that sent DISCOVER naming
 * the servicer must block (not take ownership), and the servicer's
 * return (the backstop) must hand it ownership. Unpatched, the waiter
 * takes ownership immediately (owner == 0, nothing busy) and the
 * still-blocked check fails. */
static _Atomic int g_waiter_entered;
static _Atomic int g_waiter_done;
static int g_waiter_apply_ret;

static void *waiter_thread(void *arg) {
    uint64_t wlh = (uint64_t)(uintptr_t)arg;
    struct kevent_qos_s ch = sync_wait_change(WAITER_TID);
    struct kevent_qos_s ev[1];
    set_self_tid(WAITER_TID);
    atomic_store(&g_waiter_entered, 1);
    /* flags=3 (IMMEDIATE|ERROR_EVENTS), as libdispatch's wait poll: no
     * collect, so the call only returns when the waiter is woken. */
    g_waiter_apply_ret = kevent_apply_changes(wlh, &ch, 1, ev, 1, 3);
    atomic_store(&g_waiter_done, 1);
    return NULL;
}

static int wait_for_atomic(_Atomic int *flag, int want, int timeout_ms) {
    int waited = 0;
    while (atomic_load(flag) != want && waited < timeout_ms) {
        usleep(10000);
        waited += 10;
    }
    return atomic_load(flag) == want;
}

static void test_waiter_blocks_against_discovered_owner(void) {
    if (g_wl_state_ptr == NULL) {
        g_skips++;
        printf("SKIP test_waiter_blocks_against_discovered_owner: no in-window state word (ASan owns the debounce window)\n");
        return;
    }
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    pthread_t th;
    clear_dispatched();
    *g_wl_state_ptr = SERVICER_TID;
    ch = discover_change(WLH_B, SERVICER_TID);
    n = kevent_apply_changes(WLH_B, &ch, 1, ev, 1, 0);
    check(n == 0, "handoff: DISCOVER attach returns 0 events");
    check(kevent_wl_owner_locked(WLH_B) == SERVICER_TID,
          "handoff: DISCOVER learned the servicer owner");
    atomic_store(&g_waiter_entered, 0);
    atomic_store(&g_waiter_done, 0);
    if (pthread_create(&th, NULL, waiter_thread, (void *)(uintptr_t)WLH_B) != 0) {
        check(0, "handoff: waiter thread created");
        return;
    }
    check(wait_for_atomic(&g_waiter_entered, 1, 2000),
          "handoff: waiter entered the sync wait");
    usleep(100000);
    check(atomic_load(&g_waiter_done) == 0,
          "handoff: waiter is still blocked after DISCOVER named the servicer");
    check(kevent_wl_owner_locked(WLH_B) == SERVICER_TID,
          "handoff: waiter did not steal ownership while blocked");
    /* The servicer returns: the backstop must forget the servicer owner
     * and hand ownership to the waiter. Run it as the servicer (self is
     * the servicer tid) so the servicer-forget rule applies. */
    set_self_tid(SERVICER_TID);
    servicer_return(WLH_B);
    clear_self_tid();
    check(wait_for_atomic(&g_waiter_done, 1, 2000),
          "handoff: waiter was woken by the servicer return");
    check(kevent_wl_owner_locked(WLH_B) == WAITER_TID,
          "handoff: waiter owns the workloop after the wake");
    pthread_join(th, NULL);
    /* Waiter leaves: SYNC_END releases ownership. */
    ch = sync_end_change(WAITER_TID);
    n = kevent_apply_changes(WLH_B, &ch, 1, ev, 1, 0);
    check(n == 0, "handoff: SYNC_END returns 0 events");
    check(kevent_wl_owner_locked(WLH_B) == 0,
          "handoff: ownership released after SYNC_END");
    /* Cleanup: delete the DISCOVER knote. */
    memset(&ch, 0, sizeof(ch));
    ch.ident = (SERVICER_TID & ~3U) | 3U;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    n = kevent_apply_changes(WLH_B, &ch, 1, ev, 1, 0);
    check(n == 0, "handoff: DISCOVER knote delete returns 0 events");
}

/* Gate 3 (trap b): the backstop forgets a servicer owner. A DISCOVER
 * that names the calling thread itself (the servicer leaving) must not
 * leave a stale owner behind: the backstop clears it, because
 * servicers are never owners. Unpatched, the owner stays set and no
 * waiter is ever woken. */
static void test_backstop_forgets_servicer_owner(void) {
    if (g_wl_state_ptr == NULL) {
        g_skips++;
        printf("SKIP test_backstop_forgets_servicer_owner: no in-window state word (ASan owns the debounce window)\n");
        return;
    }
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    /* Main thread is 0x303; dq_state owner 0x300 is the same thread
     * with the low 2 bits clear (masked compare). flags=3, as
     * libdispatch's polls: no collect, so the DISCOVER knote event
     * (whose ident is this thread) is not returned here. */
    *g_wl_state_ptr = 0x300;
    ch = discover_change(WLH_C, 0x300);
    n = kevent_apply_changes(WLH_C, &ch, 1, ev, 1, 3);
    check(n == 0, "servicer-owner: DISCOVER attach returns 0 events");
    check(kevent_wl_owner_locked(WLH_C) == 0x300,
          "servicer-owner: DISCOVER learned the servicer owner");
    kevent_workloop_wake_waiter(WLH_C);
    check(kevent_wl_owner_locked(WLH_C) == 0,
          "servicer-owner: backstop forgets the servicer owner");
    memset(&ch, 0, sizeof(ch));
    ch.ident = (0x300 & ~3U) | 3U;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    n = kevent_apply_changes(WLH_C, &ch, 1, ev, 1, 0);
    check(n == 0, "servicer-owner: DISCOVER knote delete returns 0 events");
}

/* Gate 4 (trap a): kevent_id(0) is rejected, and no wlh==0 knote can
 * be dispatched. Unpatched, lsk_kevent_id(0) applies the change and
 * get_next_ready hands the park loop a WORKLOOP task with wlh 0. */
static void test_kevent_id_zero_rejected(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    struct kevent_qos_s out[16];
    uint64_t out_wlh = 0xDEADBEEF;
    int out_qos = 0;
    int n, r;
    clear_dispatched();
    memset(&ch, 0, sizeof(ch));
    ch.ident = 0;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_THREAD_REQUEST;
    g_last_errno = -1;
    r = lsk_kevent_id(0, &ch, 1, ev, 1, NULL, NULL, 3);
    check(r == -1, "kevent_id(0) returns -1");
    check(g_last_errno == 22, "kevent_id(0) sets EINVAL");
    n = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
    check(n == 0, "kevent_id(0) created no dispatchable work");
    check(out_wlh == 0, "kevent_id(0) left no stale wlh");
    check(g_zero_dispatch_attempts == 0, "kevent_id(0) asked for no zero dispatch");
}

/* Gate 5 (trap a): even if a wlh==0 knote somehow reaches the table
 * (bypassing the kevent_id guard through the internal entry),
 * kevent_get_next_ready must never hand it to a worker. Unpatched,
 * it returns the zero-handle thread-request event. */
static void test_get_next_ready_skips_zero_wlh(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    struct kevent_qos_s out[16];
    uint64_t out_wlh = 0xDEADBEEF;
    int out_qos = 0;
    int n;
    clear_dispatched();
    memset(&ch, 0, sizeof(ch));
    ch.ident = 0;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_THREAD_REQUEST;
    n = kevent_apply_changes(0, &ch, 1, ev, 1, 0);
    check(n == 0, "zero-wlh attach returns 0 events");
    /* The attach tried to deliver the thread request through
     * workq_dispatch_kevent(0, ...); the workq guard refused it, so no
     * worker was spawned for wlh 0 (the stub models the real guard). */
    check(g_dispatched_count == 0, "zero knote dispatched no worker");
    n = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
    check(n == 0, "get_next_ready never dispatches a zero-handle knote");
    check(out_wlh == 0, "get_next_ready leaves no zero wlh behind");
    check(g_dispatched_count == 0, "zero knote asked for no worker");
    /* Leave the poison knote in place: the skip guard makes it invisible
     * to later scans, and this test runs last. */
}

/* Gate 6 (trap b, second fix): an EV_ERROR receipt for a stale workloop
 * change carries the freshly loaded debounce value in ext[3], not the
 * stale snapshot the caller built the change with. Unpatched, ext[3]
 * is the stale snapshot. */
static void test_error_event_ext3_refresh(void) {
    if (g_wl_state_ptr == NULL) {
        g_skips++;
        printf("SKIP test_error_event_ext3_refresh: no in-window state word (ASan owns the debounce window)\n");
        return;
    }
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    uint64_t live = 0x1122334455667788ULL;
    uint64_t stale = live ^ 0xFFFFULL;
    *g_wl_state_ptr = live;
    memset(&ch, 0, sizeof(ch));
    ch.ident = WLH_D;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS;
    ch.fflags = NOTE_WL_THREAD_REQUEST | NOTE_WL_UPDATE_QOS;
    ch.udata = WLH_D;
    ch.ext[0] = 0;
    ch.ext[1] = wl_state_addr();
    ch.ext[2] = ~0ULL; /* full mask: snapshot mismatch is ESTALE */
    ch.ext[3] = stale;
    n = kevent_apply_changes(WLH_D, &ch, 1, ev, 1, 3);
    check(n == 1, "stale thread-request returns 1 error event");
    if (n == 1) {
        check((ev[0].flags & EV_ERROR) != 0, "error event carries EV_ERROR");
        check(ev[0].data == ESTALE, "error event data is ESTALE");
        check(ev[0].ext[3] == live,
              "error event ext[3] is the fresh debounce value, not the stale snapshot");
    }
}

/* Pin: a re-entrant sync wait against a DISCOVER-learned owner returns
 * immediately (masked compare: the learned owner is the masked tid,
 * the change carries the raw tid). Passes on both trees: unpatched,
 * the waiter takes ownership directly instead, with the same
 * observables. It guards the masked compare against regressions. */
static void test_reentrant_wait_against_discovered_owner(void) {
    if (g_wl_state_ptr == NULL) {
        g_skips++;
        printf("SKIP test_reentrant_wait_against_discovered_owner: no in-window state word (ASan owns the debounce window)\n");
        return;
    }
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    *g_wl_state_ptr = WAITER_TID;
    ch = discover_change(WLH_E, WAITER_TID);
    n = kevent_apply_changes(WLH_E, &ch, 1, ev, 1, 0);
    check(n == 0, "re-entrant: DISCOVER attach returns 0 events");
    check(kevent_wl_owner_locked(WLH_E) == WAITER_TID,
          "re-entrant: DISCOVER learned the waiter owner");
    set_self_tid(WAITER_TID);
    ch = sync_wait_change(WAITER_TID);
    n = kevent_apply_changes(WLH_E, &ch, 1, ev, 1, 0);
    check(n == 1, "re-entrant: sync wait returns its own wait event at once");
    if (n == 1) {
        check(ev[0].ident == WAITER_TID, "re-entrant: wait event ident is the waiter");
        check(ev[0].fflags == NOTE_WL_SYNC_WAIT, "re-entrant: wait event carries SYNC_WAIT");
    }
    check(kevent_wl_owner_locked(WLH_E) == WAITER_TID,
          "re-entrant: waiter still owns the workloop");
    clear_self_tid();
    ch = sync_end_change(WAITER_TID);
    n = kevent_apply_changes(WLH_E, &ch, 1, ev, 1, 0);
    check(n == 0, "re-entrant: SYNC_END returns 0 events");
    check(kevent_wl_owner_locked(WLH_E) == 0,
          "re-entrant: ownership released after SYNC_END");
    memset(&ch, 0, sizeof(ch));
    ch.ident = (WAITER_TID & ~3U) | 3U;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    n = kevent_apply_changes(WLH_E, &ch, 1, ev, 1, 0);
    check(n == 0, "re-entrant: DISCOVER knote delete returns 0 events");
}

/* Pin: the uncontended sync-wait path still works (a waiter takes
 * ownership of a free, unbusy workloop directly). Guards the masked
 * compare and the DISCOVER learning against breaking the common case. */
static void test_uncontended_wait_still_works(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    set_self_tid(0x303);
    ch = sync_wait_change(0x303);
    n = kevent_apply_changes(WLH_F, &ch, 1, ev, 1, 0);
    check(n == 1, "uncontended: sync wait returns its own wait event");
    check(kevent_wl_owner_locked(WLH_F) == 0x303,
          "uncontended: waiter owns the workloop");
    ch = sync_end_change(0x303);
    n = kevent_apply_changes(WLH_F, &ch, 1, ev, 1, 0);
    check(n == 0, "uncontended: SYNC_END returns 0 events");
    check(kevent_wl_owner_locked(WLH_F) == 0,
          "uncontended: ownership released after SYNC_END");
    clear_self_tid();
}

int main(void) {
    /* Create the TLS key on the main thread before any helper starts,
     * so set_self_tid never races pthread_key_create. */
    set_self_tid(0x303);
    clear_self_tid();
    if (!alloc_wl_state()) {
        fprintf(stderr, "kevent_workloop_024_test: no in-window state word (ASan owns the debounce window); DISCOVER/ESTALE gates SKIP, the rest still run\n");
    }
    kevent_init();
    test_discover_learns_owner();
    test_waiter_blocks_against_discovered_owner();
    test_backstop_forgets_servicer_owner();
    test_error_event_ext3_refresh();
    test_reentrant_wait_against_discovered_owner();
    test_uncontended_wait_still_works();
    test_kevent_id_zero_rejected();
    test_get_next_ready_skips_zero_wlh();
    printf("kevent_workloop_024_test: %d checks, %d failures, %d skips\n",
           g_checks, g_failures, g_skips);
    return g_failures ? 1 : 0;
}
