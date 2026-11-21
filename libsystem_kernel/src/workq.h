#pragma once
#include <stdint.h>
#include <stddef.h>
#include "linux.h"

#define MAX_WQ_THREADS 16

extern int g_ipc_trace;

static inline void kq_print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys3(SYS_write, 2, (long)s, len);
}

static inline void kq_print_uint(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        while (val > 0) {
            buf[idx--] = '0' + (val % 10);
            val /= 10;
        }
    }
    kq_print_str(&buf[idx + 1]);
}

static inline void kq_print_int(int64_t val) {
    if (val < 0) {
        kq_print_str("-");
        kq_print_uint((uint64_t)-val);
    } else {
        kq_print_uint((uint64_t)val);
    }
}

static inline void kq_print_hex(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        const char hex[] = "0123456789abcdef";
        while (val > 0) {
            buf[idx--] = hex[val & 0xf];
            val >>= 4;
        }
    }
    kq_print_str(&buf[idx + 1]);
}

static inline uint32_t workq_decode_qos(int qos, uint32_t fallback) {
    if ((qos & 0xff) >= 1 && (qos & 0xff) <= 6) {
        return (uint32_t)(qos & 0xff);
    }
    if (qos & 0x3f00) {
        return (uint32_t)(__builtin_ctz((qos & 0x3f00) >> 8) + 1);
    }
    return fallback;
}

/* 72-byte kevent_qos_s */
struct kevent_qos_s {
    uint64_t ident;       /* 0x00 */
    int16_t  filter;      /* 0x08 */
    uint16_t flags;       /* 0x0a */
    int32_t  qos;         /* 0x0c */
    uint64_t udata;       /* 0x10 */
    uint32_t fflags;      /* 0x18 */
    uint32_t xflags;      /* 0x1c */
    int64_t  data;        /* 0x20 */
    uint64_t ext[4];      /* 0x28 (ext[0]..ext[3]) */
};

/* EVFILT filters */
#define EVFILT_READ            (-1)
#define EVFILT_WRITE           (-2)
#define EVFILT_AIO             (-3)
#define EVFILT_VNODE           (-4)
#define EVFILT_PROC            (-5)
#define EVFILT_SIGNAL          (-6)
#define EVFILT_TIMER           (-7)
#define EVFILT_MACHPORT        (-8)
#define EVFILT_FS              (-9)
#define EVFILT_USER            (-10)
#define EVFILT_WORKLOOP        (-17)

/* EV_* flags */
#define EV_ADD                 0x0001
#define EV_DELETE              0x0002
#define EV_ENABLE              0x0004
#define EV_DISABLE             0x0008
#define EV_ONESHOT             0x0010
#define EV_CLEAR               0x0020
#define EV_RECEIPT             0x0040
#define EV_DISPATCH            0x0080
#define EV_UDATA_SPECIFIC      0x0100
#define EV_VANISHED            0x0200
#define EV_ERROR               0x4000
#define EV_EOF                 0x8000

/* KEVENT_FLAG_* */
#define KEVENT_FLAG_IMMEDIATE              0x0001
#define KEVENT_FLAG_ERROR_EVENTS           0x0002
#define KEVENT_FLAG_WORKQ                  0x0020
#define KEVENT_FLAG_WORKLOOP               0x0400
#define KEVENT_FLAG_PARKING                0x0800
#define KEVENT_FLAG_STACK_DATA             0x1000
#define KEVENT_FLAG_DYNAMIC_KQ_MUST_EXIST  0x4000

/* NOTE_WL_* flags (XNU bsd/sys/event_private.h:501-523) */
#define NOTE_WL_THREAD_REQUEST     0x00000001
#define NOTE_WL_SYNC_WAIT          0x00000004
#define NOTE_WL_SYNC_WAKE          0x00000008
#define NOTE_WL_UPDATE_QOS         0x00000010
#define NOTE_WL_END_OWNERSHIP      0x00000020
#define NOTE_WL_DISCOVER_OWNER     0x00000080
#define NOTE_WL_IGNORE_ESTALE      0x00000100
#define NOTE_WL_SYNC_IPC           0x80000000

/* Workloop ext[] indices (XNU bsd/sys/event_private.h:528-541) */
#define EV_EXTIDX_WL_LANE          0
#define EV_EXTIDX_WL_ADDR          1
#define EV_EXTIDX_WL_MASK          2
#define EV_EXTIDX_WL_VALUE         3

/* MACH_RCV_* option bits and receive results (XNU osfmk/mach/message.h) */
#define MACH_RCV_MSG               0x00000002
#define MACH_RCV_LARGE             0x00000004
#define MACH_RCV_LARGE_IDENTITY    0x00000008
#define MACH_RCV_TOO_LARGE         0x10004004

/* Errno values used by the workloop paths (errno.h here defines no named constants) */
#define ESTALE                     70

/* WQOPS */
#define WQOPS_THREAD_RETURN                0x04
#define WQOPS_QUEUE_REQTHREADS             0x20
#define WQOPS_QUEUE_REQTHREADS2            0x30
#define WQOPS_THREAD_KEVENT_RETURN         0x40
#define WQOPS_SET_EVENT_MANAGER_PRIORITY   0x80
#define WQOPS_THREAD_WORKLOOP_RETURN       0x100
#define WQOPS_SHOULD_NARROW                0x200
#define WQOPS_SETUP_DISPATCH               0x400

/* WQ_FLAG_THREAD_* */
#define WQ_FLAG_THREAD_PRIO_QOS            0x00004000
#define WQ_FLAG_THREAD_PRIO_SCHED          0x00008000
#define WQ_FLAG_THREAD_OVERCOMMIT          0x00010000
#define WQ_FLAG_THREAD_REUSE               0x00020000
#define WQ_FLAG_THREAD_NEWSPI              0x00040000
#define WQ_FLAG_THREAD_KEVENT              0x00080000
#define WQ_FLAG_THREAD_EVENT_MANAGER       0x00100000
#define WQ_FLAG_THREAD_TSD_BASE_SET        0x00200000
#define WQ_FLAG_THREAD_WORKLOOP            0x00400000
#define WQ_FLAG_THREAD_DEFAULT_QOS         0x00800000
#define WQ_FLAG_THREAD_COOPERATIVE         0x01000000

#define DISPATCH_WLH_ANON                  ((uint64_t)-4)

void *workq_get_wqthread(void);
void workq_init(void);
void workq_lock(void);
void workq_unlock(void);
void workq_reqthreads(int numthreads, uint32_t prio, int cooperative);
int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos);
int workq_is_wlh_busy(uint64_t wlh);
/* Returns 1 if the wlh is now recorded busy (or already was), 0 if the busy table is full and the entry was dropped. Callers must not dispatch when it returns 0. */
int workq_set_wlh_busy(uint64_t wlh, int busy);
void workq_dump_state(void);

void kevent_init(void);
int kevent_apply_changes(uint64_t wlh, const struct kevent_qos_s *changes, int nchanges,
                         struct kevent_qos_s *eventlist, int nevents, unsigned int flags);
int kevent_collect_events(uint64_t wlh, struct kevent_qos_s *events, int max_events);
int kevent_get_next_ready(uint64_t *out_wlh, struct kevent_qos_s *out_ev, int max_events, int *out_qos);
void kevent_machport_notify(uint32_t port, uint32_t msg_size);
void kevent_workloop_clear_active(uint64_t wlh);
/* Per-workloop owner (kqwl_owner equivalent). Call with kq_lock held; the read itself is a single aligned 32-bit load, so the dispatch gate in workq.c may also call it while holding only wq_lock. Returns the owning thread's Mach port name, or 0 when unowned. */
uint32_t kevent_wl_owner_locked(uint64_t wlh);
/* Servicer-return backstop (called from workq.c WORKLOOP_RETURN): if a sync waiter is still blocked on wlh now that the servicer is done, hand it ownership. */
void kevent_workloop_wake_waiter(uint64_t wlh);
