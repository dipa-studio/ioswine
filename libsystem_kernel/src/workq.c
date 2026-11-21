#include "workq.h"
#include "linux.h"
#include "errno.h"

enum wq_slot_state {
    WQ_SLOT_FREE = 0,
    WQ_SLOT_IDLE,
    WQ_SLOT_RUNNING
};

struct wq_slot {
    int state;
    int futex;
    int tid;
    uint32_t kport;
    void *self;
    void *stackbottom;
    int has_entered;

    /* Current / next task */
    uint64_t bound_wlh;
    uint32_t flags;
    int nevents;
    struct kevent_qos_s events[16]; /* M3: was 14; kevent can return up to 16 deferred events */
};

static struct wq_slot g_wq_slots[MAX_WQ_THREADS];

#define MAX_BUSY_WLH 64
static uint64_t g_busy_wlh[MAX_BUSY_WLH];
static int g_busy_wlh_count = 0;

#define WQ_PENDING_MAX 16
/* M10: pending reqthreads batches, each with its own flags. The old single
 * counter + single flags slot lost all but the last batch's flags when two
 * QUEUE_REQTHREADS calls raced with no free slot. FIFO; on overflow the
 * new batch merges into the tail entry (reqs added, tail flags kept). */
struct wq_pending_entry {
    int reqs;
    uint32_t flags;
};
static struct wq_pending_entry g_pending_workq[WQ_PENDING_MAX];
static int g_pending_workq_head = 0;
static int g_pending_workq_tail = 0;
static int g_pending_workq_count = 0;

struct wq_lock {
    int val;
};
static struct wq_lock g_wq_lock = {0};

static inline void wq_lock_acquire(struct wq_lock *l) {
    while (__atomic_exchange_n(&l->val, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&l->val, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

static inline void wq_lock_release(struct wq_lock *l) {
    __atomic_store_n(&l->val, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&l->val, FUTEX_WAKE_PRIVATE, 1);
}

void workq_lock(void) {
    wq_lock_acquire(&g_wq_lock);
}

void workq_unlock(void) {
    wq_lock_release(&g_wq_lock);
}

static void *lsk_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *lsk_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

int workq_is_wlh_busy(uint64_t wlh) {
    for (int i = 0; i < g_busy_wlh_count; i++) {
        if (g_busy_wlh[i] == wlh) return 1;
    }
    return 0;
}

/* XNU kqueue_threadreq_initiate / E9: a request is never silently dropped. Returns 1 if the wlh is now recorded busy (or already was), 0 if the busy table is full and the entry was dropped; callers must not dispatch when it returns 0. */
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    if (wlh == 0) return 1;
    if (busy) {
        if (workq_is_wlh_busy(wlh)) return 1;
        if (g_busy_wlh_count < MAX_BUSY_WLH) {
            g_busy_wlh[g_busy_wlh_count++] = wlh;
            return 1;
        }
        if (g_ipc_trace) {
            kq_print_str("[kq] busy table full wlh=0x");
            kq_print_hex(wlh);
            kq_print_str("\n");
        }
        return 0;
    } else {
        for (int i = 0; i < g_busy_wlh_count; i++) {
            if (g_busy_wlh[i] == wlh) {
                g_busy_wlh[i] = g_busy_wlh[--g_busy_wlh_count];
                break;
            }
        }
        return 1;
    }
}

/* Assembly entry points */
__asm__(
".text\n"
".globl ___iosw_wq_clone\n"
".p2align 2\n"
"___iosw_wq_clone:\n"
"    mov  x8, #220\n" // SYS_clone
"    svc  #0\n"
"    cbz  x0, 1f\n"
"    ret\n"
"1:\n"
"    // Child running on clone stack\n"
"    ldr  x0, [sp, #0]\n"
"    bl   _wq_thread_entry\n"
"    mov  x8, #93\n" // SYS_exit
"    mov  x0, #0\n"
"    svc  #0\n"
"    b    .\n"
"\n"
".globl ___iosw_wq_trampoline\n"
".p2align 2\n"
"___iosw_wq_trampoline:\n"
"    // x0 = self, x1 = kport, x2 = stackbottom, x3 = keventlist\n"
"    // w4 = flags, w5 = nevents, x6 = g_wqthread, x7 = new_sp\n"
"    mov  sp, x7\n"
"    br   x6\n"
);

extern long __iosw_wq_clone(unsigned long flags, void *newsp, int *parent_tid, unsigned long tls, int *child_tid);
extern void __iosw_wq_trampoline(void *self, uint32_t kport, void *stackbottom, void *keventlist,
                                 uint32_t flags, int nevents, void *g_wqthread, void *new_sp);

void wq_thread_entry(struct wq_slot *slot);

static void wq_run_task_trampoline(struct wq_slot *slot) {
    /* M3: the stack scratch area below self shifts down by 0x80 (two more
     * 72-byte kevents): keventlist self-0x480, wlh_ptr self-0x488,
     * new_sp self-0x490. The relative spacing is unchanged. */
    void *new_sp = (void *)((uintptr_t)slot->self - 0x490);
    void *keventlist = NULL;

    if (slot->bound_wlh != 0) {
        uint64_t *wlh_ptr = (uint64_t *)((uintptr_t)slot->self - 0x488);
        *wlh_ptr = slot->bound_wlh;
        keventlist = (void *)((uintptr_t)slot->self - 0x480);
        if (slot->nevents > 0) {
            /* Defense in depth: never copy more than the 16-event buffer. */
            int ncopy = slot->nevents > 16 ? 16 : slot->nevents;
            lsk_memcpy(keventlist, slot->events, ncopy * sizeof(struct kevent_qos_s));
        }
    } else {
        uint64_t *wlh_ptr = (uint64_t *)((uintptr_t)slot->self - 0x488);
        *wlh_ptr = 0;
    }

    uint32_t flags = slot->flags;
    if (slot->bound_wlh == 0) {
        /* Defense in depth: a zero handle must never reach
         * __pthread_wqthread as a workloop or kevent task (libdispatch
         * traps on __dispatch_kq_poll with wlh 0). A plain workqueue
         * thread ignores the keventlist, so stripping the bits only
         * drops an invalid workloop task. */
        flags &= ~(WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_KEVENT);
    }
    if (slot->has_entered) {
        flags |= WQ_FLAG_THREAD_REUSE;
    } else {
        flags |= WQ_FLAG_THREAD_TSD_BASE_SET;
        flags &= ~WQ_FLAG_THREAD_REUSE;
        slot->has_entered = 1;
    }

    if (g_ipc_trace) {
        kq_print_str("[kq] wq_trampoline: slot=");
        kq_print_uint((uint64_t)(slot - g_wq_slots));
        kq_print_str(" self=0x");
        kq_print_hex((uint64_t)slot->self);
        kq_print_str(" wlh=0x");
        kq_print_hex(slot->bound_wlh);
        kq_print_str(" flags=0x");
        kq_print_hex(flags);
        kq_print_str(" nevents=");
        kq_print_int(slot->nevents);
        kq_print_str("\n");
    }

    void *wq_fn = workq_get_wqthread();
    __iosw_wq_trampoline(slot->self, slot->kport, slot->stackbottom,
                         keventlist, flags, slot->nevents,
                         wq_fn, new_sp);
    /* Never returns */
}

static int wq_spawn_worker(struct wq_slot *slot) {
    /* Guard 16K + Stack 512K + pthread_t 16K = 544K (0x88000) */
    void *base = sys_mmap(0, 0x88000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)base <= 0) {
        return -1;
    }

    /* Guard page */
    sys_mprotect(base, 0x4000, PROT_NONE);

    slot->stackbottom = (void *)((uintptr_t)base + 0x4000);
    slot->self = (void *)((uintptr_t)base + 0x84000);
    slot->state = WQ_SLOT_RUNNING;
    slot->has_entered = 0;

    extern uint32_t ipc_port_allocate_thread(void);
    slot->kport = ipc_port_allocate_thread();

    /* Zero pthread_t and initialize TSD */
    lsk_memset(slot->self, 0, 0x4000);
    uint64_t tsd_base = (uint64_t)slot->self + 0xe0;
    *(uint64_t *)tsd_base = (uint64_t)slot->self; /* slot 0: self */
    *(uint32_t *)(tsd_base + 24) = slot->kport;   /* slot 3: mach_thread_self */

    /* M3: clone_sp shifts with the enlarged keventlist (self-0x4A0), so the
     * clone's initial stack does not overlap it. */
    void *clone_sp = (void *)((uintptr_t)slot->self - 0x4A0);
    *(uint64_t *)clone_sp = (uint64_t)slot;

    unsigned long clone_flags = 0x3d0f00; /* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS */
    int *tid_loc = (int *)((uintptr_t)slot->self + 216);

    long ret = __iosw_wq_clone(clone_flags, clone_sp, tid_loc, tsd_base, tid_loc);
    if (ret < 0) {
        slot->state = WQ_SLOT_FREE;
        return -1;
    }
    return 0;
}

void wq_thread_entry(struct wq_slot *slot) {
    slot->tid = (int)sys0(SYS_gettid);
    uint64_t tsd_base = (uint64_t)slot->self + 0xe0;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tsd_base));

    wq_run_task_trampoline(slot);
}

void workq_init(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    lsk_memset(g_wq_slots, 0, sizeof(g_wq_slots));
    lsk_memset(g_busy_wlh, 0, sizeof(g_busy_wlh));
}

int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos) {
    wq_lock_acquire(&g_wq_lock);

    /* A zero handle is not a workloop. Dispatching it would build a
     * WORKLOOP worker with bound_wlh 0, and the trampoline would hand
     * __pthread_wqthread wlh 0; libdispatch then calls __dispatch_kq_poll
     * with wlh 0 and traps (BUG IN LIBDISPATCH: Invalid wlh). Drop it:
     * there is no workloop to drain. */
    if (wlh == 0) {
        wq_lock_release(&g_wq_lock);
        return 0;
    }

    /* XNU filt_wlprocess: an owned workloop is drained by its owner, never by a new servicer. The event stays active on its knote for the owner to collect. */
    if (wlh != DISPATCH_WLH_ANON && wlh != 0 && kevent_wl_owner_locked(wlh) != 0) {
        if (g_ipc_trace) {
            kq_print_str("[kq] dispatch_kevent: wlh=0x");
            kq_print_hex(wlh);
            kq_print_str(" OWNED\n");
        }
        wq_lock_release(&g_wq_lock);
        return 0;
    }

    if (workq_is_wlh_busy(wlh)) {
        if (g_ipc_trace) {
            kq_print_str("[kq] dispatch_kevent: wlh=0x");
            kq_print_hex(wlh);
            kq_print_str(" BUSY\n");
        }
        wq_lock_release(&g_wq_lock);
        return 0;
    }

    struct wq_slot *slot = NULL;
    for (int i = 0; i < MAX_WQ_THREADS; i++) {
        if (g_wq_slots[i].state == WQ_SLOT_IDLE) {
            slot = &g_wq_slots[i];
            break;
        }
    }

    if (!slot) {
        for (int i = 0; i < MAX_WQ_THREADS; i++) {
            if (g_wq_slots[i].state == WQ_SLOT_FREE) {
                slot = &g_wq_slots[i];
                break;
            }
        }
        if (slot) {
            if (!workq_set_wlh_busy(wlh, 1)) {
                wq_lock_release(&g_wq_lock);
                return 0;
            }
            slot->bound_wlh = wlh;
            /* M3: clamp to the 16-event buffer; kevent never returns more. */
            slot->nevents = nevents > 16 ? 16 : nevents;
            for (int k = 0; k < slot->nevents; k++) {
                slot->events[k] = ev[k];
            }
            if (wlh == DISPATCH_WLH_ANON) {
                slot->flags = WQ_FLAG_THREAD_KEVENT | WQ_FLAG_THREAD_EVENT_MANAGER |
                              WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS |
                              workq_decode_qos(qos, 6);
            } else {
                slot->flags = WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_NEWSPI |
                              WQ_FLAG_THREAD_PRIO_QOS |
                              workq_decode_qos(qos, 4);
            }
            if (g_ipc_trace) {
                kq_print_str("[kq] dispatch_kevent: spawn worker wlh=0x");
                kq_print_hex(wlh);
                kq_print_str(" nevents=");
                kq_print_uint(nevents);
                kq_print_str(" flags=0x");
                kq_print_hex(slot->flags);
                kq_print_str("\n");
            }
            int r = wq_spawn_worker(slot);
            wq_lock_release(&g_wq_lock);
            return r == 0 ? 1 : -1;
        }
        if (g_ipc_trace) {
            kq_print_str("[kq] dispatch_kevent: NO SLOTS for wlh=0x");
            kq_print_hex(wlh);
            kq_print_str("\n");
        }
        wq_lock_release(&g_wq_lock);
        return 0;
    }

    if (!workq_set_wlh_busy(wlh, 1)) {
        wq_lock_release(&g_wq_lock);
        return 0;
    }
    slot->state = WQ_SLOT_RUNNING;
    slot->bound_wlh = wlh;
    /* M3: clamp to the 16-event buffer; kevent never returns more. */
    slot->nevents = nevents > 16 ? 16 : nevents;
    for (int k = 0; k < slot->nevents; k++) {
        slot->events[k] = ev[k];
    }
    if (wlh == DISPATCH_WLH_ANON) {
        slot->flags = WQ_FLAG_THREAD_KEVENT | WQ_FLAG_THREAD_EVENT_MANAGER |
                      WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS |
                      workq_decode_qos(qos, 6);
    } else {
        slot->flags = WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_NEWSPI |
                      WQ_FLAG_THREAD_PRIO_QOS |
                      workq_decode_qos(qos, 4);
    }

    if (g_ipc_trace) {
        kq_print_str("[kq] dispatch_kevent: wake idle slot wlh=0x");
        kq_print_hex(wlh);
        kq_print_str(" nevents=");
        kq_print_uint(nevents);
        kq_print_str(" flags=0x");
        kq_print_hex(slot->flags);
        kq_print_str("\n");
    }

    __atomic_store_n(&slot->futex, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&slot->futex, FUTEX_WAKE_PRIVATE, 1);
    wq_lock_release(&g_wq_lock);
    return 1;
}

void workq_reqthreads(int numthreads, uint32_t prio, int cooperative) {
    wq_lock_acquire(&g_wq_lock);

    int qos = 4;
    if (prio & 0x3f00) {
        qos = __builtin_ctz((prio & 0x3f00) >> 8) + 1;
    }

    uint32_t base_flags = WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS | qos;
    if (prio & 0x80000000) base_flags |= WQ_FLAG_THREAD_OVERCOMMIT;
    if (cooperative) base_flags |= WQ_FLAG_THREAD_COOPERATIVE;

    if (g_ipc_trace) {
        kq_print_str("[kq] reqthreads: num=");
        kq_print_uint(numthreads);
        kq_print_str(" prio=0x");
        kq_print_hex(prio);
        kq_print_str(" qos=");
        kq_print_uint(qos);
        kq_print_str("\n");
    }

    while (numthreads > 0) {
        struct wq_slot *slot = NULL;
        for (int i = 0; i < MAX_WQ_THREADS; i++) {
            if (g_wq_slots[i].state == WQ_SLOT_IDLE) {
                slot = &g_wq_slots[i];
                break;
            }
        }
        if (slot) {
            slot->state = WQ_SLOT_RUNNING;
            slot->bound_wlh = 0;
            slot->nevents = 0;
            slot->flags = base_flags;
            __atomic_store_n(&slot->futex, 1, __ATOMIC_RELEASE);
            sys3(SYS_futex, (long)&slot->futex, FUTEX_WAKE_PRIVATE, 1);
            numthreads--;
            continue;
        }

        for (int i = 0; i < MAX_WQ_THREADS; i++) {
            if (g_wq_slots[i].state == WQ_SLOT_FREE) {
                slot = &g_wq_slots[i];
                break;
            }
        }
        if (slot) {
            slot->bound_wlh = 0;
            slot->nevents = 0;
            slot->flags = base_flags;
            wq_spawn_worker(slot);
            numthreads--;
            continue;
        }

        /* M10: queue this batch with its own flags (see globals above). */
        if (g_pending_workq_count < WQ_PENDING_MAX) {
            int idx = g_pending_workq_tail;
            g_pending_workq[idx].reqs = numthreads;
            g_pending_workq[idx].flags = base_flags;
            g_pending_workq_tail = (g_pending_workq_tail + 1) % WQ_PENDING_MAX;
            g_pending_workq_count++;
        } else {
            int idx = (g_pending_workq_tail + WQ_PENDING_MAX - 1) % WQ_PENDING_MAX;
            g_pending_workq[idx].reqs += numthreads;
        }
        break;
    }

    wq_lock_release(&g_wq_lock);
}

static struct wq_slot *wq_get_current_slot(void) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    void *self = (void *)(tsd - 0xe0);
    for (int i = 0; i < MAX_WQ_THREADS; i++) {
        if (g_wq_slots[i].state != WQ_SLOT_FREE && g_wq_slots[i].self == self) {
            return &g_wq_slots[i];
        }
    }
    return NULL;
}

int lsk_workq_open(void) __asm__("___workq_open");
int lsk_workq_open(void) {
    workq_init();
    kevent_init();
    return 0;
}

int lsk_workq_kernreturn(int options, void *item, int affinity, int prio) __asm__("___workq_kernreturn");
int lsk_workq_kernreturn(int options, void *item, int affinity, int prio) {
    if (g_ipc_trace) {
        kq_print_str("[kq] kernreturn: opt=0x");
        kq_print_hex(options);
        kq_print_str(" item=0x");
        kq_print_hex((uint64_t)item);
        kq_print_str(" aff=");
        kq_print_int(affinity);
        kq_print_str(" prio=0x");
        kq_print_hex((uint32_t)prio);
        kq_print_str("\n");
    }

    if (options == WQOPS_SETUP_DISPATCH) {
        workq_init();
        kevent_init();
        return 0;
    }
    if (options == WQOPS_SET_EVENT_MANAGER_PRIORITY) {
        return 0;
    }
    if (options == WQOPS_SHOULD_NARROW) {
        return 0;
    }
    if (options == WQOPS_QUEUE_REQTHREADS) {
        workq_reqthreads(affinity, (uint32_t)prio, 0);
        return 0;
    }
    if (options == WQOPS_QUEUE_REQTHREADS2) {
        workq_reqthreads(affinity, (uint32_t)prio, 1);
        return 0;
    }

    /* Thread returns: 0x04, 0x40, 0x100 */
    struct wq_slot *slot = wq_get_current_slot();
    if (!slot) {
        return 0;
    }

    if (options == WQOPS_THREAD_KEVENT_RETURN) {
        if (affinity > 0 && item != NULL) {
            kevent_apply_changes(DISPATCH_WLH_ANON, (const struct kevent_qos_s *)item, affinity, NULL, 0, 0);
        }
        wq_lock_acquire(&g_wq_lock);
        workq_set_wlh_busy(DISPATCH_WLH_ANON, 0);
        slot->bound_wlh = 0;
        wq_lock_release(&g_wq_lock);
    } else if (options == WQOPS_THREAD_WORKLOOP_RETURN) {
        uint64_t bound_wlh = slot->bound_wlh;
        if (affinity > 0 && item != NULL) {
            kevent_apply_changes(bound_wlh, (const struct kevent_qos_s *)item, affinity, NULL, 0, 0);
        }
        /* XNU workq_thread_return / kqworkloop_end_processing: the return unbinds the servicer (busy clear below). It does NOT clear the workloop's active state or its owner from dq_state; ownership is released only via NOTE_WL_END_OWNERSHIP in the deferred changelist above, and servicers are never owners. */
        wq_lock_acquire(&g_wq_lock);
        workq_set_wlh_busy(bound_wlh, 0);
        slot->bound_wlh = 0;
        wq_lock_release(&g_wq_lock);
        /* If a sync waiter is still blocked on this workloop now that the servicer is done, hand it ownership (backstop; libdispatch normally sends SYNC_WAKE, in which case this is a no-op). */
        kevent_workloop_wake_waiter(bound_wlh);
    } else {
        /* WQOPS_THREAD_RETURN (0x04) */
        uint64_t bound_wlh = slot->bound_wlh;
        wq_lock_acquire(&g_wq_lock);
        if (slot->bound_wlh != 0) {
            workq_set_wlh_busy(slot->bound_wlh, 0);
            slot->bound_wlh = 0;
        }
        wq_lock_release(&g_wq_lock);
        /* A servicer that returns via the plain path still releases its
         * waiters: without this, a sync waiter blocked on the bound
         * workloop would never be handed ownership (the WORKLOOP_RETURN
         * backstop above never runs for this return). Plain threads have
         * bound_wlh 0, so the backstop is a no-op for them. */
        if (bound_wlh != 0) kevent_workloop_wake_waiter(bound_wlh);
    }

    /* Loop: check for next work or park */
    while (1) {
        /* 1. Check if any kevent work is ready */
        uint64_t next_wlh = 0;
        int next_qos = 4;
        int nev = kevent_get_next_ready(&next_wlh, slot->events, 16, &next_qos);
        /* Defense in depth with kevent_get_next_ready (which now guarantees out_wlh == 0 when it returns 0): never hand __pthread_wqthread a workloop entry with zero events. libdispatch's __dispatch_workloop_worker_thread assumes nevents >= 1. */
        if (next_wlh != 0 && nev > 0) {
            wq_lock_acquire(&g_wq_lock);
            slot->state = WQ_SLOT_RUNNING;
            slot->bound_wlh = next_wlh;
            slot->nevents = nev;
            if (next_wlh == DISPATCH_WLH_ANON) {
                slot->flags = WQ_FLAG_THREAD_KEVENT | WQ_FLAG_THREAD_EVENT_MANAGER |
                              WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_PRIO_QOS |
                              workq_decode_qos(next_qos, 6);
            } else {
                slot->flags = WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_NEWSPI |
                              WQ_FLAG_THREAD_PRIO_QOS |
                              workq_decode_qos(next_qos, 4);
            }
            wq_lock_release(&g_wq_lock);

            if (g_ipc_trace) {
                kq_print_str("[kq] kernreturn: immediate reuse wlh=0x");
                kq_print_hex(next_wlh);
                kq_print_str(" nevents=");
                kq_print_int(nev);
                kq_print_str("\n");
            }
            wq_run_task_trampoline(slot);
        }

        wq_lock_acquire(&g_wq_lock);

        /* 2. Check if pending workq requests exist (M10: per-batch flags) */
        if (g_pending_workq_count > 0) {
            struct wq_pending_entry *pe = &g_pending_workq[g_pending_workq_head];
            uint32_t pflags = pe->flags;
            pe->reqs--;
            if (pe->reqs <= 0) {
                g_pending_workq_head = (g_pending_workq_head + 1) % WQ_PENDING_MAX;
                g_pending_workq_count--;
            }
            slot->state = WQ_SLOT_RUNNING;
            slot->bound_wlh = 0;
            slot->nevents = 0;
            slot->flags = pflags;
            wq_lock_release(&g_wq_lock);
            wq_run_task_trampoline(slot);
        }

        /* 3. Park */
        slot->state = WQ_SLOT_IDLE;
        slot->bound_wlh = 0;
        slot->nevents = 0;
        slot->flags = 0;
        __atomic_store_n(&slot->futex, 0, __ATOMIC_RELEASE);
        wq_lock_release(&g_wq_lock);

        while (__atomic_load_n(&slot->futex, __ATOMIC_ACQUIRE) == 0) {
            sys4(SYS_futex, (long)&slot->futex, FUTEX_WAIT_PRIVATE, 0, 0);
        }
        __atomic_store_n(&slot->futex, 0, __ATOMIC_RELEASE);

        /* Woken up with assigned task in slot (already set to RUNNING by waker) */
        wq_run_task_trampoline(slot);
    }
    return 0;
}

void workq_dump_state(void) {
    kq_print_str("[workq slots]\n");
    for (int i = 0; i < MAX_WQ_THREADS; i++) {
        if (g_wq_slots[i].state != WQ_SLOT_FREE) {
            kq_print_str("  slot ");
            kq_print_int(i);
            kq_print_str(" state=");
            kq_print_int(g_wq_slots[i].state);
            kq_print_str(" tid=");
            kq_print_int(g_wq_slots[i].tid);
            kq_print_str(" wlh=0x");
            kq_print_hex(g_wq_slots[i].bound_wlh);
            kq_print_str(" flags=0x");
            kq_print_hex(g_wq_slots[i].flags);
            kq_print_str("\n");
        }
    }
}
