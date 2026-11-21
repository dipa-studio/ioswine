#include "workq.h"
#include "linux.h"
#include "errno.h"

#define MAX_KNOTES 256

#define SYS_eventfd2        19
#define SYS_epoll_create1   20
#define SYS_epoll_ctl       21
#define SYS_epoll_pwait     22

#define EPOLL_CTL_ADD       1
#define EPOLL_CTL_DEL       2
#define EPOLL_CTL_MOD       3

#define EPOLLIN             0x00000001
#define EPOLLOUT            0x00000004
#define EPOLLET             0x80000000

struct epoll_event {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
typedef struct mach_timebase_info *mach_timebase_info_t;

struct timespec {
    long tv_sec;
    long tv_nsec;
};

extern int lsk_mach_timebase_info(mach_timebase_info_t info) __asm__("_mach_timebase_info");
extern uint64_t lsk_mach_absolute_time(void) __asm__("_mach_absolute_time");
/* Defined in mach.c (read-only here): current thread's Mach port name, TSD[3], or 0x303 for the main thread when no TSD is set. */
extern uint32_t lsk_mach_thread_self(void) __asm__("_mach_thread_self");

struct knote {
    int in_use;
    uint64_t wlh;
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    int32_t qos;
    uint64_t udata;
    uint32_t fflags;
    int64_t data;
    uint64_t ext[4];

    int enabled;
    int active;
    /* EVFILT_MACHPORT only: size of the first queued message, remembered at notify time. XNU filt_machportprocess reports the first message size; the armed ext[0]/ext[1] are never overwritten with it. */
    uint32_t pending_msg_size;

    uint32_t timer_clock;  /* tidx / 3: 0 = wallclock ns, 1 = continuous ticks, 2 = absolute ticks */
    uint64_t deadline_raw; /* raw deadline in timer_clock units */
};

static struct knote g_knotes[MAX_KNOTES];
static int g_epoll_fd = -1;
static int g_event_fd = -1;
static struct mach_timebase_info g_tb = {0, 0};

struct kevent_lock {
    int val;
};
static struct kevent_lock g_kq_lock = {0};

static inline void kq_lock_acquire(void) {
    while (__atomic_exchange_n(&g_kq_lock.val, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&g_kq_lock.val, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

static inline void kq_lock_release(void) {
    __atomic_store_n(&g_kq_lock.val, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&g_kq_lock.val, FUTEX_WAKE_PRIVATE, 1);
}

static void kevent_wake_poller(void) {
    if (g_event_fd >= 0) {
        uint64_t val = 1;
        sys3(SYS_write, g_event_fd, (long)&val, 8);
    }
}

static struct knote *knote_find(uint64_t wlh, uint64_t ident, int16_t filter) {
    for (int i = 0; i < MAX_KNOTES; i++) {
        if (g_knotes[i].in_use && g_knotes[i].wlh == wlh &&
            g_knotes[i].ident == ident && g_knotes[i].filter == filter) {
            return &g_knotes[i];
        }
    }
    return NULL;
}

static struct knote *knote_alloc(void) {
    for (int i = 0; i < MAX_KNOTES; i++) {
        if (!g_knotes[i].in_use) {
            uint8_t *p = (uint8_t *)&g_knotes[i];
            for (size_t b = 0; b < sizeof(struct knote); b++) p[b] = 0;
            g_knotes[i].in_use = 1;
            g_knotes[i].enabled = 1;
            return &g_knotes[i];
        }
    }
    return NULL;
}

/* ---- Workloop owner state (XNU kqwl_owner, bsd/kern/kern_event.c) ----
 * The owner is the sync waiter that currently owns the workloop (a Mach port
 * name). Servicers are never owners. The owner field is the single record
 * that stops a thread request from being delivered while the workloop is
 * owned, and stops a workloop's events from being handed to a second thread.
 * All accesses happen with kq_lock held. */
#define MAX_WL_OWNERS 64
struct wl_owner_entry {
    uint64_t wlh;
    uint32_t owner;
};
static struct wl_owner_entry g_wl_owner[MAX_WL_OWNERS];

uint32_t kevent_wl_owner_locked(uint64_t wlh) {
    for (int i = 0; i < MAX_WL_OWNERS; i++) {
        if (g_wl_owner[i].wlh == wlh) return g_wl_owner[i].owner;
    }
    return 0;
}

static void wl_set_owner_locked(uint64_t wlh, uint32_t owner) {
    for (int i = 0; i < MAX_WL_OWNERS; i++) {
        if (g_wl_owner[i].wlh == wlh) {
            if (owner == 0) {
                g_wl_owner[i].wlh = 0;
                g_wl_owner[i].owner = 0;
            } else {
                g_wl_owner[i].owner = owner;
            }
            return;
        }
    }
    if (owner == 0) return;
    for (int i = 0; i < MAX_WL_OWNERS; i++) {
        if (g_wl_owner[i].wlh == 0) {
            g_wl_owner[i].wlh = wlh;
            g_wl_owner[i].owner = owner;
            return;
        }
    }
    /* Table full: fail open (leave the workloop unowned) rather than stall it forever. */
    if (g_ipc_trace) {
        kq_print_str("[kq] wl owner table full wlh=0x");
        kq_print_hex(wlh);
        kq_print_str("\n");
    }
}

/* Owner ports are (tid & ~3): dq_state stores the masked tid (the drain-lock
 * holder, per _dispatch_tid_self), while a sync change carries the raw
 * TSD[3]. Compare with the low 2 bits clear so a DISCOVER-learned owner and
 * a sync waiter's raw ident match for the same thread. */
static int wl_owner_is(uint64_t wlh, uint32_t tid) {
    uint32_t owner = kevent_wl_owner_locked(wlh);
    return owner != 0 && (owner & ~3U) == (tid & ~3U);
}

/* ---- Sync-waiter records (XNU workloop turnstile, filt_wlpost_register_wait) ----
 * A waiter that blocks in wl_apply_change waits on its record's futex. Records
 * are static and never freed, so a waker can never touch a dead record; only
 * the waiter itself clears its own record after waking. */
#define MAX_WL_WAITERS 32
struct wl_waiter {
    int in_use;
    uint64_t wlh;
    uint32_t tid;
    int futex;
    int woken;
};
static struct wl_waiter g_wl_waiters[MAX_WL_WAITERS];

static struct wl_waiter *wl_waiter_find_locked(uint64_t wlh, uint32_t tid) {
    for (int i = 0; i < MAX_WL_WAITERS; i++) {
        if (g_wl_waiters[i].in_use && g_wl_waiters[i].wlh == wlh && g_wl_waiters[i].tid == tid) return &g_wl_waiters[i];
    }
    return NULL;
}

static struct wl_waiter *wl_waiter_find_any_locked(uint64_t wlh) {
    for (int i = 0; i < MAX_WL_WAITERS; i++) {
        if (g_wl_waiters[i].in_use && g_wl_waiters[i].wlh == wlh && !g_wl_waiters[i].woken) return &g_wl_waiters[i];
    }
    return NULL;
}

static struct wl_waiter *wl_waiter_alloc_locked(uint64_t wlh, uint32_t tid) {
    struct wl_waiter *rec = wl_waiter_find_locked(wlh, tid);
    if (rec) return rec;
    for (int i = 0; i < MAX_WL_WAITERS; i++) {
        if (!g_wl_waiters[i].in_use) {
            g_wl_waiters[i].in_use = 1;
            g_wl_waiters[i].wlh = wlh;
            g_wl_waiters[i].tid = tid;
            g_wl_waiters[i].futex = 0;
            g_wl_waiters[i].woken = 0;
            return &g_wl_waiters[i];
        }
    }
    return NULL;
}

static void wl_wake_waiter_locked(uint64_t wlh, uint32_t tid) {
    struct wl_waiter *rec = wl_waiter_find_locked(wlh, tid);
    if (!rec || rec->woken) return;
    __atomic_store_n(&rec->woken, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&rec->futex, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&rec->futex, FUTEX_WAKE_PRIVATE, 1);
}

/* Wake the first blocked sync waiter for wlh and make it the owner. Returns 1 if one was woken. */
static int wl_wake_next_waiter_locked(uint64_t wlh) {
    struct wl_waiter *waiter = wl_waiter_find_any_locked(wlh);
    if (!waiter) return 0;
    wl_set_owner_locked(wlh, waiter->tid);
    struct knote *wkn = knote_find(wlh, waiter->tid, EVFILT_WORKLOOP);
    if (wkn) {
        wkn->active = 1;
        /* Overwrite, do not OR: the delivered event must carry SYNC_WAKE without SYNC_WAIT, or libdispatch's wait loop treats it as still waiting. */
        wkn->fflags = NOTE_WL_SYNC_WAKE;
    }
    wl_wake_waiter_locked(wlh, waiter->tid);
    return 1;
}

/* Build the delivered thread-request event from the knote, not from the change.
 * XNU filt_wlprocess (bsd/kern/kern_event.c:5304-5309): data = 0, fflags = the
 * knote's saved NOTE_WL_* bits, flags = the knote's saved flags (including
 * EV_CLEAR, set at attach), ext[] = the knote's remembered lane/debounce triple. */
static void wl_build_thread_request_event(struct knote *kn, struct kevent_qos_s *ev) {
    ev->ident = kn->ident;
    ev->filter = EVFILT_WORKLOOP;
    ev->flags = kn->flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH | EV_EOF);
    ev->qos = kn->qos;
    ev->udata = kn->udata;
    ev->fflags = kn->fflags;
    ev->xflags = 0;
    ev->data = 0;
    for (int k = 0; k < 4; k++) ev->ext[k] = kn->ext[k];
}

/* XNU filt_wlprocess (bsd/kern/kern_event.c:5244-5263): if the workloop is owned, re-activate the knote instead of delivering; otherwise deliver the thread-request event. Returns 1 if an event was dispatched. */
static int wl_try_deliver_thread_request(struct knote *kn) {
    if (kevent_wl_owner_locked(kn->wlh) != 0) {
        kn->active = 1;
        return 0;
    }
    struct kevent_qos_s ev;
    wl_build_thread_request_event(kn, &ev);
    if (workq_dispatch_kevent(kn->wlh, &ev, 1, kn->qos)) {
        kn->active = 0;
        /* XNU knote_fill_kevent_with_sdata: EV_CLEAR clears the knote's fflags on delivery. The next ASYNC change re-arms them. */
        if (kn->flags & EV_CLEAR) kn->fflags = 0;
        return 1;
    }
    kn->active = 1;
    return 0;
}

/* XNU filt_wlupdate END_OWNERSHIP / kqworkloop_end_processing: release ownership, then hand the workloop to the next waiter (which takes precedence) or to the servicer via the re-activated thread-request knote. */
static void wl_release_owner_locked(uint64_t wlh) {
    wl_set_owner_locked(wlh, 0);
    if (wl_wake_next_waiter_locked(wlh)) {
        kevent_wake_poller();
        return;
    }
    struct knote *kn = knote_find(wlh, wlh, EVFILT_WORKLOOP);
    if (kn && kn->active && (kn->fflags & NOTE_WL_THREAD_REQUEST)) {
        wl_try_deliver_thread_request(kn);
    }
    kevent_wake_poller();
}

/* Backstop for the servicer-return path (called from workq.c WORKLOOP_RETURN
 * and THREAD_RETURN): if a sync waiter is still blocked on wlh now that the
 * servicer is done, hand it ownership. In XNU the waiter is woken via the
 * workloop turnstile when the servicer finishes; libdispatch normally also
 * sends SYNC_WAKE, in which case this finds no blocked waiter and is a
 * no-op. */
void kevent_workloop_wake_waiter(uint64_t wlh) {
    if (wlh == 0 || wlh == DISPATCH_WLH_ANON) return;
    kq_lock_acquire();
    uint32_t owner = kevent_wl_owner_locked(wlh);
    if (owner != 0 && wl_owner_is(wlh, lsk_mach_thread_self())) {
        /* XNU filt_wlupdate (4117-4123) and kqueue_threadreq_bind: if the
         * recorded owner is the servicer itself (learned from a
         * SYNC_DISCOVER naming the drain-lock holder), the owner is
         * forgotten, because servicers are never owners. Without this,
         * the DISCOVER-learned owner would make the backstop below see a
         * non-zero owner and wake nobody, so a waiter blocked on this
         * workloop would never be handed ownership. */
        wl_set_owner_locked(wlh, 0);
        owner = 0;
    }
    if (owner == 0) {
        wl_wake_next_waiter_locked(wlh);
    }
    kq_lock_release();
}

/* XNU filt_wlupdate (bsd/kern/kern_event.c:3854): generic ext[] debounce compare.
 * Reads the uint64 at ext[1], compares (live & mask) vs (value & mask). Returns 0 if
 * fresh (and refreshes the knote's remembered ext[3] to the live value, per
 * filt_wlremember_last_update), ESTALE (70) if stale. No debounce address in the
 * change means no debounce (SYNC_WAIT, SYNC_END, handoff_end carry none).
 * An address outside the emulated guest window, or unaligned, is treated as stale
 * rather than faulting on a read. kn may be NULL (attach before any knote exists). */
static int wl_debounce_locked(const struct kevent_qos_s *ch, struct knote *kn, uint64_t *out_live) {
    uint64_t addr = ch->ext[EV_EXTIDX_WL_ADDR];
    uint64_t mask = ch->ext[EV_EXTIDX_WL_MASK];
    uint64_t expected = ch->ext[EV_EXTIDX_WL_VALUE];
    *out_live = 0;
    if (addr == 0) return 0;
    if ((addr & 7) != 0 || addr <= 0x100000000ULL || addr >= 0x7fffffffffULL) {
        return ESTALE;
    }
    uint64_t live = *(volatile uint64_t *)addr;
    *out_live = live;
    if ((live & mask) != (expected & mask)) return ESTALE;
    if (kn) {
        kn->ext[EV_EXTIDX_WL_ADDR] = addr;
        kn->ext[EV_EXTIDX_WL_MASK] = mask;
        kn->ext[EV_EXTIDX_WL_VALUE] = live;
    }
    return 0;
}

/* XNU filt_wlupdate END_OWNERSHIP: only the owner itself releases ownership. A servicer retargeting (RETARGET) must not drop a waiter's ownership. */
static void wl_apply_end_ownership_locked(uint64_t wlh, const struct kevent_qos_s *ch) {
    uint32_t owner = kevent_wl_owner_locked(wlh);
    if (owner == 0) return;
    if (wl_owner_is(wlh, (uint32_t)ch->ident) || wl_owner_is(wlh, lsk_mach_thread_self())) {
        wl_release_owner_locked(wlh);
    } else if (g_ipc_trace) {
        kq_print_str("[kq] END_OWNERSHIP ignored: owner=0x");
        kq_print_hex(owner);
        kq_print_str(" ident=0x");
        kq_print_hex(ch->ident);
        kq_print_str("\n");
    }
}

/* Build a delivered EVFILT_MACHPORT event from the knote.
 * XNU filt_machportprocess (osfmk/ipc/ipc_pset.c:1856-2258): the armed ext[0] (receive
 * buffer) and ext[2] are preserved, ext[3] is zeroed on sanitized copyout
 * (filt_machportsanitizedcopyout), data is the receiver port name, ext[1] is the first
 * message size. A true direct receive into ext[0] is impossible from the notify hook:
 * it runs under ipc_lock and there is no dequeue API callable from kevent.c (ipc.c is
 * out of scope), so the TOO_LARGE shape is delivered in both arming modes and
 * libdispatch performs the real receive itself via lsk_mach_msg. See CHANGES.md. */
static void machport_build_event(struct knote *kn, struct kevent_qos_s *ev, uint32_t msg_size) {
    ev->ident = kn->ident;
    ev->filter = EVFILT_MACHPORT;
    ev->flags = kn->flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH | EV_EOF);
    ev->qos = kn->qos;
    ev->udata = kn->udata;
    ev->fflags = MACH_RCV_TOO_LARGE;
    ev->xflags = 0;
    ev->data = (int64_t)kn->ident;
    ev->ext[0] = kn->ext[0];
    ev->ext[1] = msg_size;
    ev->ext[2] = kn->ext[2];
    ev->ext[3] = 0;
}

void kevent_workloop_clear_active(uint64_t wlh) {
    if (wlh == 0 || wlh == DISPATCH_WLH_ANON) return;
    kq_lock_acquire();
    for (int i = 0; i < MAX_KNOTES; i++) {
        if (g_knotes[i].in_use && g_knotes[i].wlh == wlh && g_knotes[i].filter == EVFILT_WORKLOOP) {
            g_knotes[i].active = 0;
        }
    }
    kq_lock_release();
}

/* Poller thread */
void poller_thread_main(void) {
    while (1) {
        uint64_t min_delta_ns = (uint64_t)-1;
        int has_timers = 0;
        int fired_any = 0;

        uint64_t now_ticks = lsk_mach_absolute_time();

        kq_lock_acquire();
        for (int i = 0; i < MAX_KNOTES; i++) {
            struct knote *kn = &g_knotes[i];
            if (!kn->in_use || !kn->enabled || kn->filter != EVFILT_TIMER) continue;

            has_timers = 1;
            uint64_t delta_ns = 0;
            int expired = 0;

            if (kn->active) {
                expired = 1;
            } else if (kn->deadline_raw > 1000000000000000000ULL) {
                /* Unix epoch realtime ns */
                struct timespec ts;
                sys2(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, (long)&ts);
                uint64_t current_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                if (current_ns >= kn->deadline_raw) {
                    expired = 1;
                } else {
                    delta_ns = kn->deadline_raw - current_ns;
                }
            } else {
                /* Mach ticks (used for tidx 0..8 when programmed from mach_absolute_time / dispatch_time) */
                if (now_ticks >= kn->deadline_raw) {
                    expired = 1;
                } else {
                    uint64_t delta_ticks = kn->deadline_raw - now_ticks;
                    delta_ns = (delta_ticks * g_tb.numer) / g_tb.denom;
                }
            }

            if (expired) {
                kn->active = 1;
                struct kevent_qos_s ev;
                ev.ident = kn->ident;
                ev.filter = -7;
                ev.flags = (kn->flags & EV_ONESHOT) ? EV_ONESHOT : 0;
                ev.qos = kn->qos;
                ev.udata = kn->udata;
                ev.fflags = 0;
                ev.xflags = 0;
                ev.data = 1;
                ev.ext[0] = 0;
                ev.ext[1] = kn->ext[1];
                ev.ext[2] = 0;
                ev.ext[3] = 0;

                if (g_ipc_trace) {
                    kq_print_str("[kq] timer fire: wlh=0x");
                    kq_print_hex(kn->wlh);
                    kq_print_str(" ident=0x");
                    kq_print_hex(kn->ident);
                    kq_print_str(" deadline=0x");
                    kq_print_hex(kn->deadline_raw);
                    kq_print_str("\n");
                }

                if (workq_dispatch_kevent(kn->wlh, &ev, 1, kn->qos)) {
                    kn->active = 0;
                    if (kn->flags & EV_ONESHOT) {
                        kn->in_use = 0;
                        kn->enabled = 0;
                    }
                    fired_any = 1;
                }
            } else {
                if (delta_ns < min_delta_ns) {
                    min_delta_ns = delta_ns;
                }
            }
        }
        kq_lock_release();

        if (fired_any) {
            continue;
        }

        int timeout_ms = -1;
        if (has_timers) {
            uint64_t ms = min_delta_ns / 1000000ULL;
            if (ms > 3600000) ms = 3600000;
            timeout_ms = (int)ms;
            if (timeout_ms == 0 && min_delta_ns > 0) timeout_ms = 1;
        }

        struct epoll_event events[8];
        int nfds = (int)sys6(SYS_epoll_pwait, g_epoll_fd, (long)events, 8, timeout_ms, 0, 8);
        if (nfds > 0) {
            for (int i = 0; i < nfds; i++) {
                if (events[i].data == (uint64_t)g_event_fd) {
                    uint64_t val = 0;
                    sys3(SYS_read, g_event_fd, (long)&val, 8);
                } else {
                    /* Knote index in data */
                    int idx = (int)events[i].data;
                    kq_lock_acquire();
                    if (idx >= 0 && idx < MAX_KNOTES && g_knotes[idx].in_use && g_knotes[idx].enabled) {
                        struct knote *kn = &g_knotes[idx];
                        kn->active = 1;
                        kn->data = 1; /* At least 1 byte available */

                        struct kevent_qos_s ev;
                        ev.ident = kn->ident;
                        ev.filter = kn->filter;
                        ev.flags = (kn->flags & EV_DISPATCH) ? EV_DISPATCH : 0;
                        ev.qos = kn->qos;
                        ev.udata = kn->udata;
                        ev.fflags = 0;
                        ev.xflags = 0;
                        ev.data = kn->data;
                        ev.ext[0] = 0; ev.ext[1] = 0; ev.ext[2] = 0; ev.ext[3] = 0;

                        if (g_ipc_trace) {
                            kq_print_str("[kq] epoll ready idx=");
                            kq_print_uint(idx);
                            kq_print_str(" fd=");
                            kq_print_uint(kn->ident);
                            kq_print_str(" wlh=0x");
                            kq_print_hex(kn->wlh);
                            kq_print_str("\n");
                        }

                        if (workq_dispatch_kevent(kn->wlh, &ev, 1, kn->qos)) {
                            kn->active = 0;
                            if (kn->flags & EV_DISPATCH) {
                                kn->enabled = 0;
                            }
                        }
                    }
                    kq_lock_release();
                }
            }
        }
    }
}

/* Poller thread starter.
 * Host-test guard: the clone trampoline is aarch64 asm, so it is
 * compiled out of host test builds (KEVENT_HOST_TEST). The poller is not
 * needed there: the test drives kevent_machport_notify directly. Device
 * behaviour is unchanged. */
#if defined(__aarch64__) && !defined(KEVENT_HOST_TEST)
__asm__(
".text\n"
".globl ___iosw_poller_clone\n"
".p2align 2\n"
"___iosw_poller_clone:\n"
"    mov  x2, #0\n"
"    mov  x3, #0\n"
"    mov  x4, #0\n"
"    mov  x8, #220\n" // SYS_clone
"    svc  #0\n"
"    cbz  x0, 1f\n"
"    ret\n"
"1:\n"
"    bl   _poller_thread_main\n"
"    mov  x8, #93\n"
"    mov  x0, #0\n"
"    svc  #0\n"
"    b    .\n"
);
extern long __iosw_poller_clone(unsigned long flags, void *newsp);
#endif

void kevent_init(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;

    lsk_mach_timebase_info(&g_tb);
    if (g_tb.denom == 0) {
        g_tb.numer = 1;
        g_tb.denom = 1;
    }
    if (g_ipc_trace) {
        kq_print_str("[kq] kevent_init: tb.num=");
        kq_print_uint(g_tb.numer);
        kq_print_str(" tb.den=");
        kq_print_uint(g_tb.denom);
        kq_print_str(" now_ticks=0x");
        kq_print_hex(lsk_mach_absolute_time());
        kq_print_str("\n");
    }
    for (int i = 0; i < MAX_KNOTES; i++) g_knotes[i].in_use = 0;

    g_epoll_fd = (int)sys1(SYS_epoll_create1, 0x80000 /* EPOLL_CLOEXEC */);
    g_event_fd = (int)sys2(SYS_eventfd2, 0, 0x80000 /* EFD_CLOEXEC */);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.pad = 0;
    ev.data = (uint64_t)g_event_fd;
    sys4(SYS_epoll_ctl, g_epoll_fd, EPOLL_CTL_ADD, g_event_fd, (long)&ev);

    /* Hook into IPC message delivery for EVFILT_MACHPORT */
    extern void ipc_set_enqueue_hook(void (*hook)(uint32_t name, uint32_t msgh_size));
    ipc_set_enqueue_hook(kevent_machport_notify);

    /* Spawn poller thread (64 KiB stack). Not spawned in host test builds
     * (KEVENT_HOST_TEST): the poller serves timer/fd knotes only, and the
     * host test drives the machport notify path directly. */
#if defined(__aarch64__) && !defined(KEVENT_HOST_TEST)
    void *stack = sys_mmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)stack > 0) {
        void *sp = (void *)((uintptr_t)stack + 0x10000 - 16);
        unsigned long clone_flags = 0x50f00; /* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM */
        __iosw_poller_clone(clone_flags, sp);
    }
#endif
}

/* XNU filt_machportprocess on a port set (osfmk/ipc/ipc_pset.c): a knote
 * attached to a port SET fires when a message arrives on ANY member of the
 * set. The enqueue hook is called with the destination (member) name, so an
 * exact-ident match alone never fires a set knote. Look the member's set up
 * once per notify via ipc_port_get_pset_name (ipc.c; takes ipc_lock and
 * releases it raw, so it is safe to call with kq_lock held: the hook drain
 * is skipped and no AB-BA inversion is possible) and match set knotes
 * against it as well as exact-port knotes. The delivered event keeps the
 * knote's own ident (the set name), so data is the set name and ext[1] is
 * the first message size, exactly as for a port knote. */
static void kevent_machport_notify_locked(uint32_t port, uint32_t msg_size) {
    extern uint32_t ipc_port_get_pset_name(uint32_t name);
    uint32_t pset = ipc_port_get_pset_name(port);
    for (int i = 0; i < MAX_KNOTES; i++) {
        struct knote *kn = &g_knotes[i];
        if (kn->in_use && kn->enabled && kn->filter == EVFILT_MACHPORT &&
            (kn->ident == port || (pset != 0 && kn->ident == pset))) {
            /* XNU filt_machportprocess: remember the first message size separately; never clobber the armed ext[0]/ext[1]. A new size is stored when no event is pending, or when the stored size is 0 (no valid size yet). */
            if (!kn->active || kn->pending_msg_size == 0) {
                kn->pending_msg_size = msg_size;
            }
            kn->active = 1;
            struct kevent_qos_s ev;
            machport_build_event(kn, &ev, kn->pending_msg_size);

            if (g_ipc_trace) {
                kq_print_str("[kq] machport ready port=0x");
                kq_print_hex(port);
                kq_print_str(" sz=");
                kq_print_uint(msg_size);
                kq_print_str(" wlh=0x");
                kq_print_hex(kn->wlh);
                kq_print_str("\n");
            }

            if (workq_dispatch_kevent(kn->wlh, &ev, 1, kn->qos)) {
                kn->active = 0;
                kn->pending_msg_size = 0;
                if (kn->flags & EV_DISPATCH) {
                    kn->enabled = 0;
                }
            }
            /* XNU filt_machportprocess: no break; every matching knote fires. */
        }
    }
}

void kevent_machport_notify(uint32_t port, uint32_t msg_size) {
    kq_lock_acquire();
    kevent_machport_notify_locked(port, msg_size);
    kq_lock_release();
}

/* ---- EVFILT_WORKLOOP change handling (XNU filt_wlattach / filt_wlupdate / filt_wlprocess) ----
 * Called with kq_lock held; returns an errno (0 on success). A SYNC_WAIT that blocks
 * releases kq_lock while blocked and re-acquires it before returning. */
static int wl_apply_change(uint64_t wlh, const struct kevent_qos_s *ch) {
    uint32_t fflags = ch->fflags;
    int is_thread_request = (fflags & NOTE_WL_THREAD_REQUEST) != 0;
    int is_sync_wait = (fflags & NOTE_WL_SYNC_WAIT) != 0;
    int is_sync_wake = (fflags & NOTE_WL_SYNC_WAKE) != 0;
    int is_sync_ipc = (fflags & NOTE_WL_SYNC_IPC) != 0;
    int is_add = (ch->flags & EV_ADD) != 0;
    int is_delete = (ch->flags & EV_DELETE) != 0;

    /* Plain delete (EV_DELETE without EV_ADD): includes SYNC_END. */
    if (is_delete && !is_add) {
        struct knote *kn = knote_find(wlh, ch->ident, EVFILT_WORKLOOP);
        if (!kn) return 2; /* ENOENT: XNU kevent_register, except soft delete. */
        /* Debounce first when the change carries a triple; a stale delete is rejected with ESTALE unless IGNORE_ESTALE pretends success without deleting. */
        uint64_t live = 0;
        if (wl_debounce_locked(ch, NULL, &live) == ESTALE) {
            if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
            if (fflags & NOTE_WL_IGNORE_ESTALE) return 0;
            return ESTALE;
        }
        /* XNU filt_wlupdate (4071-4091): deleting an unwoken SYNC_WAIT knote wakes it. The waiter wakes with no event; libdispatch's wait loop treats zero returned events as no longer waiting and re-checks dq_state. */
        if (kn->ident != wlh) wl_wake_waiter_locked(wlh, (uint32_t)kn->ident);
        if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
        kn->in_use = 0;
        kn->enabled = 0;
        kn->active = 0;
        return 0;
    }

    /* Soft delete (EV_ADD|EV_DELETE): LEAVE and RETARGET. XNU kevent_register pretends success even if the knote does not exist. */
    if (is_add && is_delete) {
        struct knote *kn = knote_find(wlh, ch->ident, EVFILT_WORKLOOP);
        if (!kn) {
            if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
            return 0;
        }
        uint64_t live = 0;
        if (wl_debounce_locked(ch, NULL, &live) == ESTALE) {
            if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
            if (fflags & NOTE_WL_IGNORE_ESTALE) return 0;
            return ESTALE;
        }
        if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
        kn->in_use = 0;
        kn->enabled = 0;
        kn->active = 0;
        return 0;
    }

    /* Touch (EV_ENABLE/EV_DISABLE without ADD or DELETE). */
    if (!is_add && !is_delete) {
        struct knote *kn = knote_find(wlh, ch->ident, EVFILT_WORKLOOP);
        if (!kn) return 2; /* ENOENT */
        if (ch->flags & EV_DISABLE) kn->enabled = 0;
        if (ch->flags & EV_ENABLE) {
            kn->enabled = 1;
            /* Re-attempt delivery of an active thread-request knote (EV_DISPATCH re-enable on touch). */
            if (kn->ident == wlh && kn->active && (kn->fflags & NOTE_WL_THREAD_REQUEST)) {
                wl_try_deliver_thread_request(kn);
            }
        }
        return 0;
    }

    /* Add (EV_ADD without EV_DELETE). */
    if (is_thread_request) {
        /* XNU filt_wlattach: a thread-request ident must equal the dynamic id. */
        if (ch->ident != wlh) return 22; /* EINVAL */
    } else if (is_sync_wait || is_sync_wake) {
        /* XNU filt_wlattach: a sync waiter ident must not be the dynamic id. */
        if (ch->ident == wlh) return 22; /* EINVAL */
    } else if (!is_sync_ipc) {
        /* UPDATE_QOS-only or bare add: targets the thread-request knote. */
        if (ch->ident != wlh) return 22; /* EINVAL */
    }

    /* Debounce BEFORE mutating the knote: a stale update with IGNORE_ESTALE must leave it untouched. */
    uint64_t live = 0;
    if (wl_debounce_locked(ch, NULL, &live) == ESTALE) {
        if (fflags & NOTE_WL_END_OWNERSHIP) wl_apply_end_ownership_locked(wlh, ch);
        if (fflags & NOTE_WL_IGNORE_ESTALE) return 0;
        return ESTALE;
    }
    /* XNU filt_wlupdate (3988-4024): NOTE_WL_DISCOVER_OWNER learns the owner
     * from the freshly loaded debounce value (the drain-lock holder's port,
     * live & ~3). The debounce above already returned early on stale, so a
     * stale DISCOVER with IGNORE_ESTALE is a no-op here, exactly as XNU
     * requires (the operation does not happen). Learn only when the workloop
     * is currently unowned: a DISCOVER must not steal ownership from a
     * waiter that already holds it. Without this, a sync waiter that sent
     * DISCOVER naming the servicer would see owner == 0 and take ownership
     * itself while the servicer still drains (the drain lock in dq_state
     * says otherwise), so two threads drain one queue and the handoff the
     * waiter waits for never arrives. */
    if ((fflags & NOTE_WL_DISCOVER_OWNER) != 0 && live != 0) {
        uint32_t discovered = (uint32_t)(live & ~3ULL);
        if (discovered != 0 && kevent_wl_owner_locked(wlh) == 0) {
            wl_set_owner_locked(wlh, discovered);
        }
    }

    struct knote *kn = knote_find(wlh, ch->ident, EVFILT_WORKLOOP);
    if (!kn) {
        kn = knote_alloc();
        if (!kn) return 12; /* ENOMEM */
        kn->wlh = wlh;
        kn->ident = ch->ident;
        kn->filter = EVFILT_WORKLOOP;
        kn->enabled = 1;
    }
    kn->flags = ch->flags;
    kn->flags |= EV_CLEAR; /* XNU filt_wlattach sets EV_CLEAR at attach. */
    kn->qos = ch->qos;
    kn->udata = ch->udata;
    kn->fflags = ch->fflags;
    kn->data = 0; /* XNU filt_wlprocess delivers data = 0. */
    for (int k = 0; k < 4; k++) kn->ext[k] = ch->ext[k];
    /* Refresh the remembered debounce value to the live one (filt_wlremember_last_update). */
    if (ch->ext[EV_EXTIDX_WL_ADDR] != 0) kn->ext[EV_EXTIDX_WL_VALUE] = live;
    if (ch->flags & EV_DISABLE) kn->enabled = 0;
    if (ch->flags & EV_ENABLE) kn->enabled = 1;

    if (is_sync_ipc) {
        /* XNU filt_wlupdate_sync_ipc: the sync-IPC turnstile push has no in-process equivalent (no turnstile to push on). The knote is recorded so handoff_end finds it; the change is otherwise a no-op after debounce. */
        return 0;
    }

    if (is_sync_wake) {
        /* SYNC_DISCOVER carries IGNORE_ESTALE: the knote activation is a
         * probe result for the waiter itself, not a wake. The ownership
         * part of DISCOVER was handled above (owner learning from the
         * debounce value); this branch must not wake anyone and must not
         * take ownership. */
        if (fflags & NOTE_WL_IGNORE_ESTALE) {
            kn->active = 1;
            return 0;
        }
        /* SYNC_WAKE / SYNC_FAKE: a handoff from the drainer. Wake the waiter, make it the owner, and activate its knote so its own collect returns the wake event. Never block. */
        wl_wake_waiter_locked(wlh, (uint32_t)ch->ident);
        wl_set_owner_locked(wlh, (uint32_t)ch->ident);
        kn->active = 1;
        return 0;
    }

    if (is_sync_wait) {
        /* XNU kevent_register_wait_prepare + filt_wlpost_register_wait: block the waiter on the workloop turnstile. Only the waiter itself blocks (ident == this thread's Mach port), so a deferred changelist applied by a returning servicer never blocks here. */
        if (ch->ident != lsk_mach_thread_self()) {
            /* Not the waiter (or the port could not be determined): do not block. Leave the knote active so the waiter's own collect can return it. */
            kn->active = 1;
            return 0;
        }
        uint32_t owner = kevent_wl_owner_locked(wlh);
        if (wl_owner_is(wlh, (uint32_t)ch->ident)) {
            /* Already the owner (re-entrant sync). The masked compare
             * matters here: a DISCOVER-learned owner is the masked tid
             * from dq_state, while ident is the raw TSD[3]. */
            kn->active = 1;
            return 0;
        }
        if (owner == 0) {
            /* Uncontended: take ownership directly if no servicer is draining. */
            workq_lock();
            int busy = workq_is_wlh_busy(wlh);
            workq_unlock();
            if (!busy) {
                wl_set_owner_locked(wlh, (uint32_t)ch->ident);
                kn->active = 1;
                return 0;
            }
        }
        /* Contended: block until woken. */
        struct wl_waiter *rec = wl_waiter_alloc_locked(wlh, (uint32_t)ch->ident);
        if (!rec) {
            /* No waiter slot: do not block forever with nowhere to record the wake. Take ownership directly. */
            wl_set_owner_locked(wlh, (uint32_t)ch->ident);
            kn->active = 1;
            return 0;
        }
        /* Re-check after allocating: a SYNC_WAKE may have made this thread the owner already (defense in depth; the waiter holds kq_lock throughout, so no waker can interleave here). */
        if (kevent_wl_owner_locked(wlh) == (uint32_t)ch->ident || rec->woken) {
            rec->in_use = 0;
            wl_set_owner_locked(wlh, (uint32_t)ch->ident);
            kn->active = 1;
            return 0;
        }
        /* Release kq_lock while blocked so the waker can take it. The knote pointer must not be used after re-acquiring. */
        kq_lock_release();
        while (__atomic_load_n(&rec->woken, __ATOMIC_ACQUIRE) == 0) {
            sys4(SYS_futex, (long)&rec->futex, FUTEX_WAIT_PRIVATE, 0, 0);
        }
        kq_lock_acquire();
        rec->in_use = 0;
        /* The waker set this thread as owner and activated the knote; the collect phase returns the wake event. */
        return 0;
    }

    /* Thread request (or a plain UPDATE_QOS on the thread-request knote): activate and deliver if unowned. */
    kn->active = 1;
    if (kn->fflags & NOTE_WL_THREAD_REQUEST) {
        wl_try_deliver_thread_request(kn);
    }
    return 0;
}

int kevent_apply_changes(uint64_t wlh, const struct kevent_qos_s *changes, int nchanges,
                         struct kevent_qos_s *eventlist, int nevents, unsigned int flags) {
    kevent_init();
    kq_lock_acquire();

    if (g_ipc_trace) {
        kq_print_str("[kq] apply wlh=0x");
        kq_print_hex(wlh);
        kq_print_str(" nchanges=");
        kq_print_int(nchanges);
        kq_print_str(" nevents=");
        kq_print_int(nevents);
        kq_print_str(" flags=0x");
        kq_print_hex(flags);
        {
            uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
            for (int f = 0; f < 10 && fp && ((uintptr_t)fp > 0x1000) && (((uintptr_t)fp & 7) == 0); f++) {
                kq_print_str(" lr=0x");
                kq_print_hex(fp[1] & 0xffffffffffffULL);
                fp = (uint64_t *)fp[0];
            }
        }
        kq_print_str("\n");
        for (int i = 0; i < nchanges; i++) {
            const struct kevent_qos_s *ch = &changes[i];
            kq_print_str("[kq]   ch[");
            kq_print_uint(i);
            kq_print_str("] ident=0x");
            kq_print_hex(ch->ident);
            kq_print_str(" filt=");
            kq_print_int(ch->filter);
            kq_print_str(" flags=0x");
            kq_print_hex(ch->flags);
            kq_print_str(" fflags=0x");
            kq_print_hex(ch->fflags);
            kq_print_str(" data=0x");
            kq_print_hex((uint64_t)ch->data);
            kq_print_str(" udata=0x");
            kq_print_hex(ch->udata);
            kq_print_str("\n");
        }
    }

    int err_events_count = 0;

    for (int i = 0; i < nchanges; i++) {
        const struct kevent_qos_s *ch = &changes[i];
        int err = 0;

        if (ch->filter == EVFILT_WORKLOOP) {
            /* XNU filt_wlattach / filt_wlupdate / filt_wlprocess: all workloop changes go through the dedicated path. */
            err = wl_apply_change(wlh, ch);
        } else if ((ch->flags & EV_DELETE) && !(ch->flags & EV_ADD)) {
            struct knote *kn = knote_find(wlh, ch->ident, ch->filter);
            if (kn) {
                if (kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE) {
                    sys4(SYS_epoll_ctl, g_epoll_fd, EPOLL_CTL_DEL, (int)kn->ident, 0);
                }
                kn->in_use = 0;
                kn->enabled = 0;
                kn->active = 0;
                if (kn->filter == EVFILT_TIMER) {
                    kevent_wake_poller();
                }
            } else {
                err = 2; /* ENOENT */
            }
        } else if ((ch->flags & EV_ADD) || (ch->filter == EVFILT_USER && (ch->fflags & 0x01000000))) {
            if (ch->filter == EVFILT_MACHPORT && (ch->flags & EV_ADD)) {
                extern int ipc_port_has_receive_right(uint32_t name);
                if (!ipc_port_has_receive_right((uint32_t)ch->ident)) {
                    err = 22; /* EINVAL: match XNU filt_machportattach */
                }
            }
            struct knote *kn = NULL;
            if (err == 0) {
                kn = knote_find(wlh, ch->ident, ch->filter);
                if (!kn) {
                    kn = knote_alloc();
                    if (!kn) {
                        err = 12; /* ENOMEM */
                    } else {
                        kn->wlh = wlh;
                        kn->ident = ch->ident;
                        kn->filter = ch->filter;
                        kn->enabled = 1;
                    }
                }
            }
            if (kn) {
                kn->flags = ch->flags;
                kn->qos = ch->qos;
                kn->udata = ch->udata;
                kn->fflags = ch->fflags;
                kn->data = ch->data;
                for (int k = 0; k < 4; k++) kn->ext[k] = ch->ext[k];

                if (ch->flags & EV_DISABLE) kn->enabled = 0;
                if (ch->flags & EV_ENABLE) kn->enabled = 1;

                if (kn->filter == EVFILT_USER) {
                    if (ch->fflags & 0x01000000 /* NOTE_TRIGGER */) {
                        kn->active = 1;
                        struct kevent_qos_s ev = *ch;
                        ev.flags = 0;
                        ev.data = 1;
                        if (workq_dispatch_kevent(wlh, &ev, 1, kn->qos)) {
                            if (kn->flags & EV_CLEAR) kn->active = 0;
                        }
                    }
                } else if (kn->filter == EVFILT_TIMER) {
                    uint32_t tidx = (uint32_t)(ch->ident & 0xff);
                    kn->timer_clock = tidx / 3;
                    uint64_t deadline = (uint64_t)ch->data;
                    if (!(ch->fflags & 0x08 /* NOTE_ABSOLUTE */)) {
                        deadline += lsk_mach_absolute_time();
                    }
                    kn->deadline_raw = deadline;
                    kn->active = 0;
                    if (g_ipc_trace) {
                        kq_print_str("[kq]   timer arm: wlh=0x");
                        kq_print_hex(wlh);
                        kq_print_str(" ident=0x");
                        kq_print_hex(ch->ident);
                        kq_print_str(" tidx=");
                        kq_print_uint(tidx);
                        kq_print_str(" clock=");
                        kq_print_uint(kn->timer_clock);
                        kq_print_str(" deadline=0x");
                        kq_print_hex(deadline);
                        kq_print_str("\n");
                    }
                    kevent_wake_poller();
                } else if (kn->filter == EVFILT_READ || kn->filter == EVFILT_WRITE) {
                    int fd = (int)ch->ident;
                    struct epoll_event ev;
                    ev.events = (kn->filter == EVFILT_READ ? EPOLLIN : EPOLLOUT) | EPOLLET;
                    ev.pad = 0;
                    ev.data = (uint64_t)(kn - g_knotes);
                    int r = (int)sys4(SYS_epoll_ctl, g_epoll_fd, EPOLL_CTL_ADD, fd, (long)&ev);
                    if (r < 0) {
                        sys4(SYS_epoll_ctl, g_epoll_fd, EPOLL_CTL_MOD, fd, (long)&ev);
                    }
                } else if (kn->filter == EVFILT_MACHPORT) {
                    extern uint32_t ipc_port_peek_size(uint32_t name);
                    uint32_t sz = ipc_port_peek_size((uint32_t)ch->ident);
                    if (sz > 0) {
                        kevent_machport_notify_locked((uint32_t)ch->ident, sz);
                    }
                }
            }
        } else if (ch->flags & EV_ENABLE) {
            struct knote *kn = knote_find(wlh, ch->ident, ch->filter);
            if (kn) {
                kn->enabled = 1;
                if (kn->filter == EVFILT_MACHPORT) {
                    extern uint32_t ipc_port_peek_size(uint32_t name);
                    uint32_t sz = ipc_port_peek_size((uint32_t)ch->ident);
                    if (sz > 0) kevent_machport_notify_locked((uint32_t)ch->ident, sz);
                }
            }
        } else if (ch->flags & EV_DISABLE) {
            struct knote *kn = knote_find(wlh, ch->ident, ch->filter);
            if (kn) kn->enabled = 0;
        }

        if ((ch->flags & EV_RECEIPT) || (err != 0 && (flags & KEVENT_FLAG_ERROR_EVENTS))) {
            if (eventlist && err_events_count < nevents) {
                eventlist[err_events_count] = *ch;
                eventlist[err_events_count].flags = EV_ERROR;
                eventlist[err_events_count].data = err;
                if (err == ESTALE && ch->filter == EVFILT_WORKLOOP &&
                    ch->ext[EV_EXTIDX_WL_ADDR] != 0) {
                    /* XNU filt_wlupdate writes the freshly loaded debounce
                     * value into ext[WL_VALUE] before returning ESTALE
                     * (filt_wlremember_last_update), and libdispatch's
                     * poke_drain reads dq_state from ext[3] on an EV_ERROR
                     * thread-request event (event_kevent.c:1788). Emitting
                     * the raw change would hand it the stale snapshot the
                     * caller built the change with. Refresh it to live. */
                    uint64_t live2 = 0;
                    wl_debounce_locked(ch, NULL, &live2);
                    if (live2 != 0) {
                        eventlist[err_events_count].ext[EV_EXTIDX_WL_VALUE] = live2;
                    }
                }
                err_events_count++;
            }
        }
    }

    if (flags & KEVENT_FLAG_ERROR_EVENTS) {
        kq_lock_release();
        return err_events_count;
    }

    int out_count = 0;
    if (eventlist && nevents > 0) {
        out_count = kevent_collect_events(wlh, eventlist, nevents);
    }
    kq_lock_release();
    return out_count;
}

int kevent_collect_events(uint64_t wlh, struct kevent_qos_s *events, int max_events) {
    int count = 0;
    for (int i = 0; i < MAX_KNOTES && count < max_events; i++) {
        struct knote *kn = &g_knotes[i];
        if (!kn->in_use || !kn->enabled || !kn->active || kn->wlh != wlh) continue;
        if (kn->filter == EVFILT_WORKLOOP) {
            /* XNU filt_wlprocess / kevent_id EXDEV: a thread-request event goes only to the bound servicer via dispatch or the park loop, never via a generic collect. Leave it active. */
            if (kn->ident == kn->wlh) continue;
            /* Sync-waiter knote: only the waiter itself may collect it. Anyone else leaves it active. */
            if (kn->ident != lsk_mach_thread_self()) continue;
            events[count].ident = kn->ident;
            events[count].filter = EVFILT_WORKLOOP;
            events[count].flags = (kn->flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH | EV_EOF));
            events[count].qos = kn->qos;
            events[count].udata = kn->udata;
            events[count].fflags = kn->fflags;
            events[count].xflags = 0;
            events[count].data = 0;
            for (int k = 0; k < 4; k++) events[count].ext[k] = kn->ext[k];
            count++;
            kn->active = 0;
            if (kn->flags & EV_CLEAR) kn->fflags = 0;
            continue;
        }
        if (kn->filter == EVFILT_MACHPORT) {
            /* A machport knote with no remembered size has nothing valid to deliver yet; leave it active for the next notify. */
            if (kn->pending_msg_size == 0) continue;
            machport_build_event(kn, &events[count], kn->pending_msg_size);
            count++;
            kn->active = 0;
            kn->pending_msg_size = 0;
            if (kn->flags & EV_DISPATCH) {
                kn->enabled = 0;
            }
            if (kn->flags & EV_ONESHOT) {
                kn->in_use = 0;
                kn->enabled = 0;
            }
            continue;
        }
        events[count].ident = kn->ident;
        events[count].filter = kn->filter;
        events[count].flags = (kn->flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH | EV_EOF));
        events[count].qos = kn->qos;
        events[count].udata = kn->udata;
        events[count].fflags = kn->fflags;
        events[count].xflags = 0;
        events[count].data = kn->data ? kn->data : 1;
        for (int k = 0; k < 4; k++) events[count].ext[k] = kn->ext[k];
        count++;
        kn->active = 0;
        if (kn->flags & EV_DISPATCH) {
            kn->enabled = 0;
        }
        if (kn->flags & EV_ONESHOT) {
            kn->in_use = 0;
            kn->enabled = 0;
        }
    }
    return count;
}

int kevent_get_next_ready(uint64_t *out_wlh, struct kevent_qos_s *out_ev, int max_events, int *out_qos) {
    kq_lock_acquire();
    workq_lock();
    for (int i = 0; i < MAX_KNOTES; i++) {
        struct knote *kn = &g_knotes[i];
        if (!kn->in_use || !kn->enabled || !kn->active) continue;
        /* A zero handle is never a real workloop: no knote should carry
         * it (lsk_kevent_id rejects id 0), and dispatching one would hand
         * __pthread_wqthread a WORKLOOP task with wlh 0, which libdispatch
         * traps on in __dispatch_kq_poll (Invalid wlh). Skip it rather
         * than dispatching poison. */
        if (kn->wlh == 0) continue;
        /* XNU filt_wlprocess / kevent_id EXDEV: an owned workloop is drained by its owner, never by a servicer. */
        if (kevent_wl_owner_locked(kn->wlh) != 0) continue;
        if (workq_is_wlh_busy(kn->wlh)) continue;
        uint64_t wlh = kn->wlh;
        /* Claim the wlh before collecting, so a full busy table cannot strand already-consumed events. */
        if (!workq_set_wlh_busy(wlh, 1)) continue;
        *out_wlh = wlh;
        int count = 0;
        int qos = 4;

        /* Thread-request event first (XNU kqueue_process delivers it to the bound servicer). This is the fix for the Round 3 hang: the event must actually reach the worker. */
        struct knote *trk = knote_find(wlh, wlh, EVFILT_WORKLOOP);
        if (trk && trk->in_use && trk->enabled && trk->active && (trk->fflags & NOTE_WL_THREAD_REQUEST)) {
            if (count < max_events) {
                wl_build_thread_request_event(trk, &out_ev[count]);
                if (trk->qos) qos = trk->qos;
                count++;
                trk->active = 0;
                if (trk->flags & EV_CLEAR) trk->fflags = 0;
            }
        }

        for (int j = 0; j < MAX_KNOTES && count < max_events; j++) {
            struct knote *k = &g_knotes[j];
            if (!k->in_use || !k->enabled || !k->active || k->wlh != wlh) continue;
            /* Workloop knotes: the thread request was delivered above; sync-waiter knotes never go to a servicer. */
            if (k->filter == EVFILT_WORKLOOP) continue;
            if (k->filter == EVFILT_MACHPORT) {
                if (k->pending_msg_size == 0) continue;
                machport_build_event(k, &out_ev[count], k->pending_msg_size);
                if (k->qos) qos = k->qos;
                count++;
                k->active = 0;
                k->pending_msg_size = 0;
                if (k->flags & EV_DISPATCH) {
                    k->enabled = 0;
                }
                if (k->flags & EV_ONESHOT) {
                    k->in_use = 0;
                    k->enabled = 0;
                }
                continue;
            }

            out_ev[count].ident = k->ident;
            out_ev[count].filter = k->filter;
            out_ev[count].flags = (k->flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH | EV_EOF));
            out_ev[count].qos = k->qos;
            out_ev[count].udata = k->udata;
            out_ev[count].fflags = k->fflags;
            out_ev[count].xflags = 0;
            out_ev[count].data = k->data ? k->data : 1;
            for (int m = 0; m < 4; m++) out_ev[count].ext[m] = k->ext[m];
            if (k->qos) qos = k->qos;

            k->active = 0;
            if (k->flags & EV_DISPATCH) {
                k->enabled = 0;
            }
            if (k->flags & EV_ONESHOT) {
                k->in_use = 0;
                k->enabled = 0;
            }
            count++;
        }
        if (count == 0) {
            /* Nothing deliverable (e.g. only a sync-waiter knote was active): release the claim and keep scanning. Reset *out_wlh: the caller dispatches on *out_wlh != 0, so leaving the claimed wlh behind would hand __pthread_wqthread a WORKLOOP task with nevents == 0, which libdispatch's __dispatch_workloop_worker_thread assumes never happens (its dispatch_assume BUG trap fires on an empty list). */
            workq_set_wlh_busy(wlh, 0);
            *out_wlh = 0;
            continue;
        }
        workq_unlock();
        kq_lock_release();
        if (out_qos) *out_qos = qos;
        return count;
    }
    workq_unlock();
    kq_lock_release();
    /* No work found: report no workloop, never a stale one. */
    *out_wlh = 0;
    return 0;
}

int lsk_kevent_qos(int fd, const void *changelist, int nchanges,
                   void *eventlist, int nevents,
                   void *data_out, size_t *data_available, unsigned int flags) __asm__("_kevent_qos");
int lsk_kevent_qos(int fd, const void *changelist, int nchanges,
                   void *eventlist, int nevents,
                   void *data_out, size_t *data_available, unsigned int flags) {
    (void)data_out;
    if (data_available) *data_available = 0;

    if (fd != -1) {
        set_errno(38 /* ENOSYS */);
        return -1;
    }

    return kevent_apply_changes(DISPATCH_WLH_ANON,
                                (const struct kevent_qos_s *)changelist, nchanges,
                                (struct kevent_qos_s *)eventlist, nevents, flags);
}

int lsk_kevent_id(uint64_t id, const void *changelist, int nchanges,
                  void *eventlist, int nevents,
                  void *data_out, size_t *data_available, unsigned int flags) __asm__("_kevent_id");
int lsk_kevent_id(uint64_t id, const void *changelist, int nchanges,
                  void *eventlist, int nevents,
                  void *data_out, size_t *data_available, unsigned int flags) {
    (void)data_out;
    if (data_available) *data_available = 0;

    if (id == 0) {
        /* XNU kevent_id requires a valid dynamic workloop id (an id of 0
         * is not a workloop). Accepting 0 would create knotes with wlh 0,
         * which kevent_get_next_ready would hand to __pthread_wqthread as
         * a WORKLOOP task with wlh 0; libdispatch then calls
         * __dispatch_kq_poll with wlh 0 and traps (BUG IN LIBDISPATCH:
         * Invalid wlh). Reject it here, at the boundary. */
        set_errno(22 /* EINVAL */);
        return -1;
    }

    return kevent_apply_changes(id,
                                (const struct kevent_qos_s *)changelist, nchanges,
                                (struct kevent_qos_s *)eventlist, nevents, flags);
}
