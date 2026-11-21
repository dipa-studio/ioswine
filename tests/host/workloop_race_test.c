/* workloop_race_test.c - Drive the kq/wq state machine through the
 * device crash sequence.
 *
 * Device facts:
 *   [thread] wq-create tid=0x694d prio=0x644005 workloop id=0x50694c800
 *   wq_trampoline: slot=0 wlh=0x50694c800 flags=0x464005 nevents=1
 *   [lsk CRASH] sig=0x0000000000000004 pc=0x00000001be0214ec
 *     (__dispatch_workloop_worker_thread+272, libdispatch BUG trap)
 *
 * Flag decode (libsystem_kernel/src/workq.h): 0x644005 is the fresh
 * first-entry trampoline flags (WORKLOOP 0x400000 | TSD_BASE_SET
 * 0x200000 | NEWSPI 0x40000 | PRIO_QOS 0x4000 | qos 5); 0x464005 is
 * the reuse entry (same, with REUSE 0x20000 instead of
 * TSD_BASE_SET). The dispatch-time slot flags are 0x444005
 * (WORKLOOP | NEWSPI | PRIO_QOS | qos 5). The crashing entry was a
 * REUSE entry on slot 0: the slot had parked and been woken for a
 * second event.
 *
 * This test drives the REAL kevent.c + ipc.c (host build, like
 * kevent_pset_test) through that exact sequence with a workq stub
 * that faithfully models workq.c's busy table and flag computation
 * (workq.c itself is aarch64-only and cannot compile on the x86_64
 * host). It pins the exact flag values and the dispatch / busy /
 * ownership / return invariants. The libdispatch trap itself is
 * NOT reproducible on host (no libdispatch here): most of this
 * test is a conformance pin and regression guard, not a
 * red/green gate for the crash. One scenario IS a gate: the
 * stale-out_wlh regression (test_get_next_ready_no_stale_wlh)
 * FAILS on the unpatched tree and PASSES with the fix. Where the current code already satisfies an
 * invariant, the test says so (workq.c IDLE-path rollback, not host-testable).
 *
 * Plain cc + ASan/UBSan on Linux host. No Apple code, no device.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "workq.h"
#include "ipc.h"

#include "workq.h"
#include "ipc.h"

/* ------------------------------------------------------------------ */
/* Stubs for symbols the real kevent.c / ipc.c need.                  */
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
static int g_carender_inits;
void carender_init(void) {
    g_carender_inits++;
}

/* Kobject service handlers (launchd, BKS, svchost, fakeboard, IOKit,
 * hidsys, notifyd) live in other LSK files that this test does not
 * compile. The test never sends a message to a service port, so these
 * stubs are never called; they exist only to satisfy the linker. Each
 * returns 0 ("not handled"). */
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

/* ------------------------------------------------------------------ */
/* Faithful workq model: busy table + slot flags (workq.c)            */
/* ------------------------------------------------------------------ */

#define WLH_CRASH 0x50694c800ULL
#define WLH_OTHER 0x5014b8680ULL
#define QOS_CRASH 5
#define WAITER_TID 0x303u

/* workq.c flag computation for a workloop dispatch:
 * WORKLOOP | NEWSPI | PRIO_QOS | decode_qos(qos, 4). */
static uint32_t model_slot_flags(int qos) {
    uint32_t q = workq_decode_qos(qos, 4);
    return WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_NEWSPI |
           WQ_FLAG_THREAD_PRIO_QOS | q;
}
/* Trampoline adjustment (wq_run_task_trampoline): first entry adds
 * TSD_BASE_SET, a reuse adds REUSE. */
static uint32_t model_trampoline_flags(uint32_t slot_flags, int entered_before) {
    if (entered_before) return slot_flags | WQ_FLAG_THREAD_REUSE;
    return slot_flags | WQ_FLAG_THREAD_TSD_BASE_SET;
}

#define MAX_DISPATCHED 128
struct dispatched_ev {
    uint64_t wlh;
    int nevents;
    int qos;
    uint32_t slot_flags;
    uint64_t ident;
    int16_t filter;
    uint32_t fflags;
    int64_t data;
    uint64_t ext1;
};
static struct dispatched_ev g_dispatched[MAX_DISPATCHED];
static int g_dispatched_count;
static int g_dispatch_held; /* calls that returned 0 (busy/owned) */

#define MAX_BUSY_MODEL 64
static uint64_t g_busy_model[MAX_BUSY_MODEL];
static int g_busy_model_count;
static int g_entered_wlh; /* wlh that has entered the trampoline before */

static int model_busy(uint64_t wlh) {
    for (int i = 0; i < g_busy_model_count; i++) {
        if (g_busy_model[i] == wlh) return 1;
    }
    return 0;
}

/* workq_set_wlh_busy semantics (workq.c): returns 1 if the wlh is
 * now recorded busy (or already was), 0 if the busy table is full
 * and the entry was dropped. */
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
    if (wlh != DISPATCH_WLH_ANON && wlh != 0 && model_busy(wlh)) {
        g_dispatch_held++;
        return 0;
    }
    /* Real workq.c claims the busy entry before waking/spawning;
     * a full table means the request is held, never dispatched. */
    if (!model_set_busy(wlh, 1)) {
        g_dispatch_held++;
        return 0;
    }
    if (g_dispatched_count < MAX_DISPATCHED) {
        struct dispatched_ev *d = &g_dispatched[g_dispatched_count++];
        d->wlh = wlh;
        d->nevents = nevents;
        d->qos = qos;
        d->slot_flags = model_slot_flags(qos);
        d->ident = nevents > 0 ? ev[0].ident : 0;
        d->filter = nevents > 0 ? ev[0].filter : 0;
        d->fflags = nevents > 0 ? ev[0].fflags : 0;
        d->data = nevents > 0 ? ev[0].data : 0;
        d->ext1 = nevents > 0 ? ev[0].ext[1] : 0;
    }
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

/* The model's workloop state word: the host stand-in for the
 * guest workloop struct the device's ext[1] points at. It is
 * mmap'd at a fixed address inside wl_debounce_locked's guest
 * window (0x100000000, 0x7fffffffff): a plain .bss address does
 * NOT work, because this host's PIE base (0x6324...) sits above
 * the window ceiling and the debounce rejects it as ESTALE. */
#include <sys/mman.h>
static uint64_t *g_wl_state_ptr;
/* 0 when no in-window state word could be mapped (ASan owns the
 * whole debounce window: see alloc_wl_state). thread_request_change
 * then passes ext[1] = 0, which means "no debounce" to
 * wl_debounce_locked; the delivered event's ext[1] is 0 and the
 * carry-through invariant is still pinned, just with a zero
 * address. The plain variant always maps the state word, so the
 * full debounce-read path is covered there. */
static uint64_t wl_state_addr(void) { return (uint64_t)g_wl_state_ptr; }
static int wl_addr_in_window(uint64_t a) {
    return a > 0x100000000ULL && a < 0x7fffffffffULL && (a & 7) == 0;
}
static int alloc_wl_state(void) {
    /* Try fixed addresses inside the debounce guest window, then
     * the same addresses as hints (the kernel may place the mapping
     * nearby), then any address the kernel picks. The fixed
     * 0x200000000 works under plain cc but ASan already owns that
     * range, so a single address is not enough. */
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
    /* No in-window address is mappable: under ASan the whole
     * debounce window is owned by the sanitizer runtime (a 1 GiB
     * sweep finds no free address; a raw syscall fails too). Fall
     * back to ext[1] = 0 (no debounce) rather than failing the
     * test: the state machine is still fully exercised, only the
     * debounce-read path is skipped in this variant. */
    g_wl_state_ptr = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

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
}

static struct kevent_qos_s thread_request_change(uint64_t wlh, int qos) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof(ch));
    ch.ident = wlh;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = qos;
    ch.fflags = NOTE_WL_THREAD_REQUEST;
    ch.ext[0] = 0; /* lane */
    ch.ext[1] = wl_state_addr(); /* workloop state-word address
                              (EV_EXTIDX_WL_ADDR); on the device
                              this is the guest wlh */
    ch.ext[2] = 0; /* mask (0: debounce always fresh) */
    ch.ext[3] = 0; /* value */
    return ch;
}

static void test_flag_decode(void) {
    /* The device's exact values, from the flag constants. */
    check(workq_decode_qos(5, 4) == 5, "decode_qos(5,4)==5");
    check(model_slot_flags(5) == 0x444005u, "slot flags 0x444005");
    check(model_trampoline_flags(0x444005u, 0) == 0x644005u,
          "fresh trampoline flags 0x644005 (wq-create line)");
    check(model_trampoline_flags(0x444005u, 1) == 0x464005u,
          "reuse trampoline flags 0x464005 (crash line)");
    check((0x464005u & WQ_FLAG_THREAD_REUSE) != 0, "crash flags carry REUSE");
    check((0x464005u & WQ_FLAG_THREAD_TSD_BASE_SET) == 0, "crash flags lack TSD_BASE_SET");
    check((0x644005u & WQ_FLAG_THREAD_TSD_BASE_SET) != 0, "wq-create flags carry TSD_BASE_SET");
    check((0x644005u & WQ_FLAG_THREAD_REUSE) == 0, "wq-create flags lack REUSE");
    if (g_wl_state_ptr != NULL) {
        check(wl_addr_in_window(wl_state_addr()),
              "state word inside the debounce guest window");
    } else {
        /* ASan fallback: no state word, ext[1] is 0 (no debounce).
         * The carry-through invariant below still applies. */
        check(wl_state_addr() == 0, "ASan fallback: ext[1] is 0");
    }
}

static void test_thread_request_dispatch(void) {
    struct kevent_qos_s ch = thread_request_change(WLH_CRASH, QOS_CRASH);
    struct kevent_qos_s ev[1];
    int n;
    clear_dispatched();
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    check(n == 0, "thread-request attach returns 0 events");
    check(g_dispatched_count == 1, "one dispatch for the thread request");
    check(g_dispatch_held == 0, "nothing held");
    if (g_dispatched_count == 1) {
        struct dispatched_ev *d = &g_dispatched[0];
        check(d->wlh == WLH_CRASH, "dispatch wlh is the crash workloop");
        check(d->nevents == 1, "dispatch nevents=1");
        check(d->qos == QOS_CRASH, "dispatch qos=5");
        check(d->slot_flags == 0x444005u, "dispatch slot flags 0x444005");
        check(d->ident == WLH_CRASH, "delivered event ident==wlh");
        check(d->filter == EVFILT_WORKLOOP, "delivered event is EVFILT_WORKLOOP");
        check(d->fflags == NOTE_WL_THREAD_REQUEST, "delivered event carries THREAD_REQUEST");
        check(d->data == 0, "delivered event data==0 (XNU filt_wlprocess)");
        check(d->ext1 == wl_state_addr(), "delivered event ext[1] is the state-word address");
        check(model_trampoline_flags(d->slot_flags, 0) == 0x644005u,
              "fresh entry matches the device wq-create line");
    }
    check(model_busy(WLH_CRASH) == 1, "workloop busy after dispatch");
}

static void test_busy_hold_and_reuse(void) {
    struct kevent_qos_s ch = thread_request_change(WLH_CRASH, QOS_CRASH);
    struct kevent_qos_s ev[1];
    struct kevent_qos_s out[16];
    int out_qos = 0;
    uint64_t out_wlh = 0;
    int n;
    /* Second request while busy: held, not double-delivered. */
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    check(n == 0, "second thread-request attach returns 0 events");
    check(g_dispatched_count == 1, "no second dispatch while busy");
    check(g_dispatch_held == 1, "second request held (busy)");
    /* Servicer returns: busy clears, no waiter to wake. */
    servicer_return(WLH_CRASH);
    check(model_busy(WLH_CRASH) == 0, "workloop free after servicer return");
    /* The returning servicer's get_next_ready re-delivers the held
     * request as an immediate reuse (the device crash entry). It
     * claims the workloop busy again: the servicer owns it until
     * it returns. */
    n = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
    check(n == 1, "get_next_ready delivers the held request");
    check(out_wlh == WLH_CRASH, "reuse wlh is the crash workloop");
    check(out_qos == QOS_CRASH, "reuse qos=5");
    check(out[0].filter == EVFILT_WORKLOOP, "reuse event is EVFILT_WORKLOOP");
    check(out[0].fflags == NOTE_WL_THREAD_REQUEST, "reuse event carries THREAD_REQUEST");
    check(out[0].ext[1] == wl_state_addr(), "reuse event ext[1] is the state-word address");
    check(g_dispatched_count == 1, "get_next_ready does not dispatch itself");
    check(model_busy(WLH_CRASH) == 1, "workloop busy again after get_next_ready");
    servicer_return(WLH_CRASH);
    /* Re-delivery through the dispatch path (a fresh change, as
     * libdispatch sends after the return) is a reuse entry. */
    clear_dispatched();
    g_entered_wlh = 1;
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    check(n == 0, "re-attach after return returns 0 events");
    check(g_dispatched_count == 1, "reuse request dispatches");
    if (g_dispatched_count == 1) {
        check(model_trampoline_flags(g_dispatched[0].slot_flags, g_entered_wlh) == 0x464005u,
              "reuse entry matches the device trampoline line");
    }
    /* EV_CLEAR: the delivered knote's fflags were cleared, so a
     * further collect delivers nothing (no duplicate delivery). */
    {
        struct kevent_qos_s out2[16];
        int n2 = kevent_collect_events(WLH_CRASH, out2, 16);
        check(n2 == 0, "no duplicate delivery after EV_CLEAR");
    }
    servicer_return(WLH_CRASH);
}

static void test_owned_hold_and_release(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n;
    clear_dispatched();
    /* Sync waiter 0x303 (this thread, per the _mach_thread_self
     * stub) takes ownership of the unowned, unbusy workloop. */
    memset(&ch, 0, sizeof(ch));
    ch.ident = WAITER_TID;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS_CRASH;
    ch.fflags = NOTE_WL_SYNC_WAIT;
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    /* The waiter takes ownership directly (uncontended) and its own
     * collect returns the sync-wait event: that is how the waiter
     * learns it owns the workloop. */
    check(n == 1, "sync-wait attach collects its own wait event");
    check(ev[0].filter == EVFILT_WORKLOOP, "wait event is EVFILT_WORKLOOP");
    check(ev[0].ident == WAITER_TID, "wait event ident is the waiter");
    check(ev[0].fflags == NOTE_WL_SYNC_WAIT, "wait event carries SYNC_WAIT");
    check(kevent_wl_owner_locked(WLH_CRASH) == WAITER_TID, "waiter owns the workloop");
    /* Thread request while owned: held, never dispatched. */
    ch = thread_request_change(WLH_CRASH, QOS_CRASH);
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    check(n == 0, "thread-request attach while owned returns 0 events");
    check(g_dispatched_count == 0, "no dispatch while owned");
    /* A servicer return must NOT hand an owned workloop to a
     * servicer: get_next_ready skips owned workloops. */
    {
        struct kevent_qos_s out[16];
        uint64_t out_wlh = 0;
        int out_qos = 0;
        int n2 = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
        check(n2 == 0, "owned workloop is never handed to a servicer");
    }
    /* END_OWNERSHIP from the owner (delete change, ident == owner):
     * ownership releases and the held thread request is delivered. */
    memset(&ch, 0, sizeof(ch));
    ch.ident = WAITER_TID;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    ch.fflags = NOTE_WL_END_OWNERSHIP;
    n = kevent_apply_changes(WLH_CRASH, &ch, 1, ev, 1, 0);
    check(n == 0, "end-ownership delete returns 0 events");
    check(kevent_wl_owner_locked(WLH_CRASH) == 0, "ownership released");
    check(g_dispatched_count == 1, "held thread request delivered on release");
    if (g_dispatched_count == 1) {
        check(g_dispatched[0].wlh == WLH_CRASH, "released delivery wlh");
        check(g_dispatched[0].nevents == 1, "released delivery nevents=1");
    }
    servicer_return(WLH_CRASH);
}

static void test_busy_table_full(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    int n, i;
    clear_dispatched();
    /* Fill the 64-entry busy table with other workloops. */
    for (i = 0; i < MAX_BUSY_MODEL; i++) {
        uint64_t w = 0x600000000ULL + (uint64_t)i;
        check(workq_set_wlh_busy(w, 1) == 1, "busy table fill");
    }
    /* A thread request for a new workloop cannot dispatch (table
     * full) but the request must not be lost: the knote stays
     * active for the owner/servicer to collect later. */
    ch = thread_request_change(WLH_OTHER, QOS_CRASH);
    n = kevent_apply_changes(WLH_OTHER, &ch, 1, ev, 1, 0);
    check(n == 0, "thread-request attach with full busy table returns 0 events");
    check(g_dispatched_count == 0, "no dispatch when the busy table is full");
    check(g_dispatch_held == 1, "request held (busy table full)");
    /* Free one entry: the held request is now collectable through
     * get_next_ready. */
    check(workq_set_wlh_busy(0x600000000ULL, 0) == 1, "free one busy entry");
    {
        struct kevent_qos_s out[16];
        uint64_t out_wlh = 0;
        int out_qos = 0;
        int n2 = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
        check(n2 == 1, "held request delivered after a busy entry frees");
        check(out_wlh == WLH_OTHER, "delivered wlh is the held one");
    }
    for (i = 1; i < MAX_BUSY_MODEL; i++) {
        workq_set_wlh_busy(0x600000000ULL + (uint64_t)i, 0);
    }
    servicer_return(WLH_OTHER);
}

/* Regression test (fix: reset *out_wlh on every no-work
 * exit of kevent_get_next_ready, and require nev > 0 in the park
 * loop). Unpatched, get_next_ready sets *out_wlh right after the
 * busy claim and the count==0 path releases the claim and keeps
 * scanning WITHOUT resetting it, so a no-work return hands the
 * park loop a stale wlh: it dispatches a WORKLOOP task with
 * nevents==0 into __dispatch_workloop_worker_thread, whose
 * dispatch_assume BUG trap fires (SIGILL, the device crash).
 * This scenario hits that path with a waiter-only workloop: a
 * SYNC_WAIT from a foreign tid leaves an active knote but takes
 * no ownership, so get_next_ready claims the wlh, finds nothing
 * deliverable (workloop knotes never go to a servicer), and
 * returns 0. FAILS on the unpatched tree (out_wlh stays
 * WLH_OTHER), PASSES with the fix. */
static void test_get_next_ready_no_stale_wlh(void) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    struct kevent_qos_s out[16];
    uint64_t out_wlh = 0xDEADBEEF;
    int out_qos = 0;
    int n;
    memset(&ch, 0, sizeof(ch));
    ch.ident = 0x404; /* foreign tid: not this thread (0x303) */
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = QOS_CRASH;
    ch.fflags = NOTE_WL_SYNC_WAIT;
    n = kevent_apply_changes(WLH_OTHER, &ch, 1, ev, 1, 0);
    check(n == 0, "foreign sync-wait attach returns 0 events");
    check(kevent_wl_owner_locked(WLH_OTHER) == 0, "foreign waiter takes no ownership");
    n = kevent_get_next_ready(&out_wlh, out, 16, &out_qos);
    check(n == 0, "get_next_ready finds no work for a waiter-only workloop");
    check(out_wlh == 0, "get_next_ready leaves no stale wlh on no-work return");
    memset(&ch, 0, sizeof(ch));
    ch.ident = 0x404;
    ch.filter = EVFILT_WORKLOOP;
    ch.flags = EV_DELETE;
    n = kevent_apply_changes(WLH_OTHER, &ch, 1, ev, 1, 0);
    check(n == 0, "waiter knote delete returns 0 events");
}

int main(void) {
    if (!alloc_wl_state()) {
        fprintf(stderr, "workloop_race_test: no in-window state word (ASan owns the debounce window); running with ext[1] = 0\n");
    }
    kevent_init();
    test_flag_decode();
    test_thread_request_dispatch();
    test_busy_hold_and_reuse();
    test_owned_hold_and_release();
    test_busy_table_full();
    test_get_next_ready_no_stale_wlh();
    printf("workloop_race_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
