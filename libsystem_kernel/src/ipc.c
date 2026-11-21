#include "ipc.h"
#include "launchd.h"
#include "bks.h"
#include "hidsys.h"
#include "svchost.h"
#include "notifyd.h"
#include "linux.h"
#include <stdint.h>
#include <stddef.h>

/* memcpy, declared manually (freestanding; same pattern as
 * present.c). The OOL descriptor address copies are the only users. */
void *memcpy(void *dst, const void *src, size_t n);

extern uint64_t mach_absolute_time(void) __asm__("_mach_absolute_time");
struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
extern int mach_timebase_info(struct mach_timebase_info *info) __asm__("_mach_timebase_info");
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");

/* Futex-based Mutex */
typedef struct {
    uint32_t val;
} ipc_mutex_t;

static ipc_mutex_t g_ipc_lock = {0};
static uint32_t g_ipc_generation = 1;
int g_ipc_trace = 0;
static ipc_enqueue_hook_t g_enqueue_hook = NULL;
static volatile int g_svc_pump_pending;

/* Forward declaration for the trace helper (defined below). */
static void ipc_print_str(const char *s);

/* Deferred enqueue-hook queue (M7). The hook (kevent_machport_notify) takes
 * kq_lock then wq_lock, and wq_spawn_worker takes ipc_lock, so calling the
 * hook while holding ipc_lock is a self-deadlock (ipc_lock -> kq_lock ->
 * wq_lock -> ipc_lock), and the reverse order (kq_lock -> ipc_lock in
 * kevent_apply_changes) makes it an AB-BA inversion. Hook notifications are
 * therefore queued under ipc_lock and fired from ipc_unlock, after the lock
 * is released. The drain is iterative and reentrancy-guarded, so a hook
 * that re-enters ipc_unlock (via wq_spawn_worker -> ipc_port_allocate_thread)
 * cannot recurse. */
#define IPC_PENDING_HOOK_MAX 128
static struct {
    mach_port_name_t name;
    uint32_t size;
} g_pending_hooks[IPC_PENDING_HOOK_MAX];
static uint32_t g_pending_hook_head = 0;
static uint32_t g_pending_hook_tail = 0;
static uint32_t g_pending_hook_count = 0;
static uint32_t g_pending_hook_dropped = 0;
static int g_hook_draining = 0;

/* Forward declaration: defined below, after the lock primitives. */
static void ipc_drain_pending_hooks(void);

void ipc_set_enqueue_hook(ipc_enqueue_hook_t hook) {
    g_enqueue_hook = hook;
}

static inline void ipc_lock(void) {
    while (__atomic_exchange_n(&g_ipc_lock.val, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&g_ipc_lock.val, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

/* Plain release with no hook drain. Used by ipc_port_has_receive_right and
 * ipc_port_peek_size, which kevent_apply_changes calls while holding
 * kq_lock: draining there would take kq_lock again (non-recursive) and
 * deadlock. Skipping the drain there loses nothing: every enqueue is
 * followed by a draining ipc_unlock in the enqueueing thread. */
static inline void ipc_unlock_raw(void) {
    __atomic_store_n(&g_ipc_lock.val, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&g_ipc_lock.val, FUTEX_WAKE_PRIVATE, 1);
}

static inline void ipc_unlock(void) {
    ipc_unlock_raw();
    ipc_drain_pending_hooks();
}

/* Queue a hook notification for delivery after ipc_unlock. Call with
 * ipc_lock held. */
static void ipc_enqueue_hook_deferred(mach_port_name_t name, uint32_t size) {
    if (!g_enqueue_hook || name == MACH_PORT_NULL) return;
    if (g_pending_hook_count < IPC_PENDING_HOOK_MAX) {
        g_pending_hooks[g_pending_hook_tail].name = name;
        g_pending_hooks[g_pending_hook_tail].size = size;
        g_pending_hook_tail = (g_pending_hook_tail + 1) % IPC_PENDING_HOOK_MAX;
        g_pending_hook_count++;
    } else {
        /* Overflow: drop the notification. The futex wake (ipc_notify_all)
         * still fires, so futex-blocked receivers are unaffected; only a
         * knote-driven receiver could miss this one notify, and the next
         * message's notify wakes it. This needs >128 hook calls under a
         * single ipc_lock hold, which no send path does. */
        g_pending_hook_dropped++;
        if (g_ipc_trace) ipc_print_str("[ipc] hook queue full, dropped\n");
    }
}

static void ipc_drain_pending_hooks(void) {
    if (g_hook_draining) return;
    if (__atomic_load_n(&g_pending_hook_count, __ATOMIC_ACQUIRE) == 0) return;
    g_hook_draining = 1;
    for (;;) {
        if (__atomic_load_n(&g_pending_hook_count, __ATOMIC_ACQUIRE) == 0) break;
        /* Snapshot the queue under ipc_lock, then fire the hooks with no
         * lock held. */
        struct {
            mach_port_name_t name;
            uint32_t size;
        } snap[IPC_PENDING_HOOK_MAX];
        uint32_t n = 0;
        ipc_lock();
        while (g_pending_hook_count > 0 && n < IPC_PENDING_HOOK_MAX) {
            snap[n].name = g_pending_hooks[g_pending_hook_head].name;
            snap[n].size = g_pending_hooks[g_pending_hook_head].size;
            g_pending_hook_head = (g_pending_hook_head + 1) % IPC_PENDING_HOOK_MAX;
            g_pending_hook_count--;
            n++;
        }
        ipc_unlock_raw();
        for (uint32_t i = 0; i < n; i++) {
            if (g_enqueue_hook) g_enqueue_hook(snap[i].name, snap[i].size);
        }
    }
    g_hook_draining = 0;
}

static inline void ipc_notify_all(void) {
    __atomic_fetch_add(&g_ipc_generation, 1, __ATOMIC_SEQ_CST);
    sys3(SYS_futex, (long)&g_ipc_generation, FUTEX_WAKE_PRIVATE, 0x7fffffff);
}

/* Logging helpers */
static void ipc_print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(2, s, len);
}

static void ipc_print_uint(uint64_t val) {
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
    ipc_print_str(&buf[idx + 1]);
}

static void ipc_print_hex(uint64_t val) {
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
    ipc_print_str(&buf[idx + 1]);
}

static uint32_t g_logged_kobject_ids[64];
static uint32_t g_logged_kobject_count = 0;

static void ipc_log_kobject_id(uint32_t id) {
    for (uint32_t i = 0; i < g_logged_kobject_count; i++) {
        if (g_logged_kobject_ids[i] == id) return;
    }
    if (g_logged_kobject_count < 64) {
        g_logged_kobject_ids[g_logged_kobject_count++] = id;
    }
    ipc_print_str("[lsk] kobject msg id ");
    ipc_print_uint(id);
    ipc_print_str("\n");
}

/* Data Model */
#define MAX_ENTRIES 2048
#define MAX_PORTS   2048



struct ipc_kmsg {
    struct ipc_kmsg *next;
    uint32_t size;           /* size of message body as sent */
    uint32_t alloc_size;     /* >0 if allocated via sys_mmap */
    uint32_t seqno;          /* sequence number */
    struct ipc_port *reply_port; /* in-flight reply port */
    uint32_t reply_gen;      /* generation of reply_port at send time (M6) */
    uint8_t reply_type;      /* in-flight reply type: SEND or SEND_ONCE */
    uint8_t is_complex;
    uint8_t is_launchd;      /* 1 if message is from launchd (PID 1, UID 0) */
    uint8_t pad;
    uint8_t data[];          /* payload (header + descriptors + body) */
};

#define MAX_PSET_MEMBERS 64

struct ipc_port {
    uint32_t in_use;
    uint32_t has_receive_right;
    uint32_t gen;            /* liveness epoch (M6): bumped on alloc and on receive-right destroy */
    uint32_t kobject_kind;
    mach_port_name_t svc_alias; /* SVCHOST connection port: name of the service port it belongs to */
    
    struct ipc_kmsg *queue_head;
    struct ipc_kmsg *queue_tail;
    uint32_t msgcount;
    uint32_t qlimit;
    uint32_t seqno;
    uint32_t mscount;
    uint32_t srights;
    uint32_t sorights;
    
    uint64_t context;
    uint64_t guard;
    uint32_t guarded;
    uint32_t guard_flags;
    
    /* Port set */
    mach_port_name_t pset_name;
    mach_port_name_t pset_members[MAX_PSET_MEMBERS];
    uint32_t pset_member_count;
    
    /* mk_timer */
    uint64_t timer_deadline;
    uint32_t timer_armed;
    
    /* Notifications */
    mach_port_t nsrequest_port;
    uint32_t nsrequest_mscount;
    mach_port_t dnrequest_port;

    uint32_t is_reply_port;
    uint32_t is_special_reply_port;

    /* Named memory entry (PORT_KOBJECT_MEM_ENTRY) */
    int      mem_fd;
    uint64_t mem_size;
    uint64_t mem_offset;
    int      mem_prot;
    uint64_t mem_vaddr;
};

struct ipc_entry {
    struct ipc_port *port;
    uint32_t type;         /* MACH_PORT_TYPE_* */
    uint32_t urefs;        /* send urefs */
    uint32_t dead_urefs;   /* dead-name urefs */
    uint16_t generation;
    uint16_t in_use;
};

static struct ipc_entry g_entries[MAX_ENTRIES];
static struct ipc_port  g_ports[MAX_PORTS];
static struct ipc_entry g_host_entry;
static struct ipc_port  g_host_port;
static int g_ipc_inited = 0;

/* M6: in-flight port descriptors carry (slot index, generation) instead of a
 * raw struct ipc_port *, so a port destroyed while a message is queued
 * cannot deliver a name for a dead (or reused) port. The pointer is
 * reconstructed and validated at receive. The store helper is exported
 * (ipc.h) because launchd.c builds a descriptor in its reply buffer too. */
void ipc_desc_store_port(uint8_t *desc_ptr, struct ipc_port *port) {
    if (!port) {
        *(uint32_t *)desc_ptr = 0xffffffffu;
        *(uint32_t *)(desc_ptr + 4) = 0;
        return;
    }
    *(uint32_t *)desc_ptr = (uint32_t)(port - g_ports);
    *(uint32_t *)(desc_ptr + 4) = port->gen;
}

static struct ipc_port *ipc_desc_load_port(const uint8_t *desc_ptr) {
    uint32_t idx = *(const uint32_t *)desc_ptr;
    uint32_t gen = *(const uint32_t *)(desc_ptr + 4);
    if (idx >= MAX_PORTS) return NULL;
    struct ipc_port *p = &g_ports[idx];
    if (!p->in_use || p->gen != gen) return NULL;
    return p;
}

/* Static kmsg pool for small messages <= 1024 bytes */
#define KMSG_POOL_COUNT 256
#define KMSG_SLOT_SIZE  1024

static uint8_t g_kmsg_pool[KMSG_POOL_COUNT][sizeof(struct ipc_kmsg) + KMSG_SLOT_SIZE] __attribute__((aligned(16)));
static uint8_t g_kmsg_in_use[KMSG_POOL_COUNT];

static struct ipc_kmsg *kmsg_alloc(uint32_t size) {
    if (size <= KMSG_SLOT_SIZE) {
        for (int i = 0; i < KMSG_POOL_COUNT; i++) {
            if (!g_kmsg_in_use[i]) {
                g_kmsg_in_use[i] = 1;
                struct ipc_kmsg *k = (struct ipc_kmsg *)g_kmsg_pool[i];
                k->next = NULL;
                k->size = size;
                k->alloc_size = 0;
                k->seqno = 0;
                k->reply_port = NULL;
                k->reply_gen = 0;
                k->reply_type = 0;
                k->is_complex = 0;
                k->is_launchd = 0;
                return k;
            }
        }
    }
    /* Fallback to anonymous mmap */
    size_t total = sizeof(struct ipc_kmsg) + size;
    size_t rounded = (total + 4095) & ~4095;
    void *p = sys_mmap(0, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uintptr_t)p >= (uintptr_t)-4095) return NULL;
    struct ipc_kmsg *k = (struct ipc_kmsg *)p;
    k->next = NULL;
    k->size = size;
    k->alloc_size = (uint32_t)rounded;
    k->seqno = 0;
    k->reply_port = NULL;
    k->reply_gen = 0;
    k->reply_type = 0;
    k->is_complex = 0;
    k->is_launchd = 0;
    return k;
}

static void kmsg_free(struct ipc_kmsg *kmsg) {
    if (!kmsg) return;
    if (kmsg->alloc_size > 0) {
        sys_munmap(kmsg, kmsg->alloc_size);
        return;
    }
    uintptr_t addr = (uintptr_t)kmsg;
    uintptr_t pool_base = (uintptr_t)g_kmsg_pool;
    uintptr_t slot_total = sizeof(struct ipc_kmsg) + KMSG_SLOT_SIZE;
    if (addr >= pool_base && addr < pool_base + KMSG_POOL_COUNT * slot_total) {
        int idx = (int)((addr - pool_base) / slot_total);
        if (idx >= 0 && idx < KMSG_POOL_COUNT) {
            g_kmsg_in_use[idx] = 0;
        }
    }
}

static inline uint32_t ipc_get_trailer_size(uint64_t option) {
    uint32_t elem = (option >> 24) & 0xf;
    switch (elem) {
        case 0: return 8;
        case 1: return 12;
        case 2: return 20;
        case 3: return 52;
        case 4: return 60;
        default: return 68;
    }
}

static void ipc_fill_trailer(uint8_t *p, uint32_t trailer_size, uint32_t seqno, struct ipc_port *port, int is_launchd) {
    if (trailer_size < 8) return;
    uint32_t *w = (uint32_t *)p;
    w[0] = 0;             /* msgh_trailer_type = 0 */
    w[1] = trailer_size;  /* msgh_trailer_size */
    if (trailer_size >= 12) {
        w[2] = seqno;     /* msgh_seqno */
    }
    uint32_t uid = is_launchd ? 0 : 501;
    uint32_t pid = is_launchd ? 1 : (uint32_t)sys0(SYS_getpid);
    if (trailer_size >= 20) {
        w[3] = uid;       /* sender security token {uid, gid} */
        w[4] = uid;
    }
    if (trailer_size >= 52) {
        /* audit token */
        w[5] = uid;
        w[6] = uid;
        w[7] = uid;
        w[8] = uid;
        w[9] = uid;
        w[10] = pid;
        w[11] = 0;
        w[12] = 1;
    }
    if (trailer_size >= 60) {
        uint64_t *ctx = (uint64_t *)(p + 52);
        *ctx = port ? port->context : 0;
    }
    if (trailer_size >= 68) {
        uint64_t *flt = (uint64_t *)(p + 60);
        *flt = 0;
    }
}

/* Descriptor walk bounds. The descriptor count comes
 * from the message itself, so a corrupt count used to walk desc_ptr
 * past the end of the message, reading stale pool bytes as
 * descriptors and, for port descriptors, writing a pointer there,
 * and for OOL descriptors copying and deallocating a garbage address
 * with a garbage size. Every walk below stops at the end of the
 * message it walks. */
static int ipc_desc_fits(const uint8_t *p, const uint8_t *end, uint32_t need) {
    if (!p || (uintptr_t)p > (uintptr_t)end) return 0;
    return (uintptr_t)end - (uintptr_t)p >= (uintptr_t)need;
}

/* OOL range probe. The sender's OOL descriptor carries
 * the address and size; the old code trusted both, so an inflated
 * size copied past the source mapping into neighbouring live heap
 * and then deallocated the neighbouring live pages. msync with
 * MS_ASYNC only proves the pages are mapped; it touches no content
 * and does no I/O. The probe rounds to the 4 KiB host page, not
 * the 16 KiB guest page: a mapping can start on a 4 KiB boundary,
 * and rounding down to 16 KiB would report a valid range as
 * unmapped (same idiom as the vm_copy validator). A zero
 * length range is trivially mapped. The probe cannot tell a
 * PROT_NONE page from a readable one, so a fault inside the copy
 * loop on a no-access source is still possible; that is the
 * pre-existing behaviour, not something this probe makes worse. */
static int ipc_range_mapped(uint64_t addr, uint32_t len) {
    uint64_t start, end;
    if (len == 0) return 1;
    if (addr + (uint64_t)len < addr) return 0;
    start = addr & ~4095ULL;
    end = (addr + (uint64_t)len + 4095ULL) & ~4095ULL;
    return sys3(SYS_msync, (long)start, (long)(end - start), 1 /* MS_ASYNC */) == 0;
}

/* Descriptor table validation, run on the sender's
 * buffer before the message is copied or any right is consumed.
 * Returns 1 when the table is well formed: the count fits in the
 * message, every descriptor fits in the message, and every OOL
 * descriptor's range is mapped. Returns 0 otherwise. */
static int ipc_msg_desc_valid(const mach_msg_header_t *hdr, uint32_t size) {
    const uint8_t *msg = (const uint8_t *)hdr;
    const uint8_t *end = msg + size;
    const uint8_t *p;
    uint32_t count, i;
    if (!(hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 1;
    if (size < sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t)) return 0;
    count = ((const mach_msg_body_t *)(msg + sizeof(mach_msg_header_t)))->msgh_descriptor_count;
    p = msg + sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t);
    for (i = 0; i < count; i++) {
        uint8_t type;
        uint32_t need;
        if (!ipc_desc_fits(p, end, 12)) return 0;
        type = p[11];
        if (type == MACH_MSG_OOL_DESCRIPTOR || type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) need = 16;
        else if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) need = 24;
        else need = 12;
        if (!ipc_desc_fits(p, end, need)) return 0;
        if (need == 16) {
            uint64_t addr;
            uint32_t sz = *(const uint32_t *)(p + 12);
            /* The OOL address is read with memcpy
             * because the wire layout puts it at offset 28, which
             * is 4-aligned, so a direct 8-byte load is unaligned
             * by construction (UBSan flags it; arm64 tolerates it).
             * The size field at offset 12 is 4-aligned, so a
             * direct 4-byte load is fine. */
            memcpy(&addr, p, sizeof addr);
            if (addr && !ipc_range_mapped(addr, sz)) return 0;
        }
        p += need;
    }
    return 1;
}

/* Forward declaration: defined below (port allocation section). Used by
 * the send-once notification hook (M5). */
static mach_port_name_t ipc_port_get_name_for_recv(struct ipc_port *port);

/* Mach IPC Send-Once Notification (MACH_NOTIFY_SEND_ONCE 0x47) */
static void ipc_send_once_notify_port_locked(struct ipc_port *rp) {
    if (!rp || !rp->has_receive_right) return;
    struct ipc_kmsg *notif = kmsg_alloc(sizeof(mach_msg_header_t));
    if (!notif) return;
    mach_msg_header_t *nhdr = (mach_msg_header_t *)notif->data;
    nhdr->msgh_bits = MACH_MSGH_BITS(0, 0);
    nhdr->msgh_size = sizeof(mach_msg_header_t);
    nhdr->msgh_remote_port = MACH_PORT_NULL;
    nhdr->msgh_local_port = MACH_PORT_NULL;
    nhdr->msgh_voucher_port = MACH_PORT_NULL;
    nhdr->msgh_id = MACH_NOTIFY_SEND_ONCE; /* 0x47 */

    notif->reply_port = NULL;
    notif->reply_type = 0;
    notif->is_complex = 0;
    rp->seqno++;
    notif->seqno = rp->seqno;

    if (rp->queue_tail) {
        rp->queue_tail->next = notif;
        rp->queue_tail = notif;
    } else {
        rp->queue_head = notif;
        rp->queue_tail = notif;
    }
    rp->msgcount++;
    /* M5: a knote-driven receiver learns about the notification. The
     * notification port's name comes from its receive entry. */
    ipc_enqueue_hook_deferred(ipc_port_get_name_for_recv(rp), sizeof(mach_msg_header_t));
    if (rp->sorights > 0) rp->sorights--;
}

/* Initialization */
void ipc_init(void) {
    if (g_ipc_inited) return;
    ipc_lock();
    if (g_ipc_inited) {
        ipc_unlock();
        return;
    }

    for (int i = 0; i < MAX_ENTRIES; i++) {
        g_entries[i].generation = 3;
        g_entries[i].in_use = 0;
    }
    for (int i = 0; i < MAX_PORTS; i++) {
        g_ports[i].in_use = 0;
    }

    /* Index 1: 0x103 = mach_task_self_ */
    g_entries[1].in_use = 1;
    g_entries[1].generation = 3;
    g_entries[1].port = &g_ports[1];
    g_entries[1].type = MACH_PORT_TYPE_SEND;
    g_entries[1].urefs = 0xffff;
    g_ports[1].in_use = 1;
    g_ports[1].kobject_kind = PORT_KOBJECT_TASK;
    g_ports[1].has_receive_right = 1;
    g_ports[1].qlimit = MACH_PORT_QLIMIT_LARGE;

    /* Index 2: 0x203 = bootstrap_port */
    g_entries[2].in_use = 1;
    g_entries[2].generation = 3;
    g_entries[2].port = &g_ports[2];
    g_entries[2].type = MACH_PORT_TYPE_SEND;
    g_entries[2].urefs = 0xffff;
    g_ports[2].in_use = 1;
    g_ports[2].kobject_kind = PORT_KOBJECT_LAUNCHD;
    g_ports[2].has_receive_right = 0;
    g_ports[2].qlimit = MACH_PORT_QLIMIT_LARGE;

    /* Index 3: 0x303 = mach_thread_self for main thread */
    g_entries[3].in_use = 1;
    g_entries[3].generation = 3;
    g_entries[3].port = &g_ports[3];
    g_entries[3].type = MACH_PORT_TYPE_SEND;
    g_entries[3].urefs = 0xffff;
    g_ports[3].in_use = 1;
    g_ports[3].kobject_kind = PORT_KOBJECT_THREAD;
    g_ports[3].has_receive_right = 1;
    g_ports[3].qlimit = MACH_PORT_QLIMIT_LARGE;

    /* Host port 0x104 */
    g_host_entry.in_use = 1;
    g_host_entry.generation = 4;
    g_host_entry.port = &g_host_port;
    g_host_entry.type = MACH_PORT_TYPE_SEND;
    g_host_entry.urefs = 0xffff;
    g_host_port.in_use = 1;
    g_host_port.kobject_kind = PORT_KOBJECT_HOST;
    g_host_port.has_receive_right = 1;
    g_host_port.qlimit = MACH_PORT_QLIMIT_LARGE;

    /* Port 4: BackBoard display service kobject */
    g_ports[4].in_use = 1;
    g_ports[4].kobject_kind = PORT_KOBJECT_BKS_DISPLAY;
    g_ports[4].has_receive_right = 1;
    g_ports[4].qlimit = MACH_PORT_QLIMIT_LARGE;
    g_ports[4].srights = 1;

    /* Port 5/6: fake com.apple.iohideventsystem service & connection kobjects */
    g_ports[5].in_use = 1;
    g_ports[5].kobject_kind = PORT_KOBJECT_HIDSYS;
    g_ports[5].has_receive_right = 1;
    g_ports[5].qlimit = MACH_PORT_QLIMIT_LARGE;
    g_ports[5].srights = 1;

    g_ports[6].in_use = 1;
    g_ports[6].kobject_kind = PORT_KOBJECT_HIDSYS;
    g_ports[6].has_receive_right = 1;
    g_ports[6].qlimit = MACH_PORT_QLIMIT_LARGE;
    g_ports[6].srights = 1;

    g_ipc_inited = 1;
    ipc_unlock();
}

struct ipc_port *ipc_get_bks_display_port(void) {
    return &g_ports[4];
}

struct ipc_port *ipc_get_hidsys_port(void) {
    if (!hidsys_is_enabled()) return NULL;
    return &g_ports[5];
}

struct ipc_port *ipc_get_hidsys_conn_port(void) {
    if (!hidsys_is_enabled()) return NULL;
    return &g_ports[6];
}

void ipc_port_retain_send(struct ipc_port *port) {
    if (port) port->srights++;
}

void ipc_set_trace(int trace) {
    g_ipc_trace = trace;
}

mach_port_t ipc_task_self(void) {
    if (!g_ipc_inited) ipc_init();
    return 0x103;
}

/* Port and Entry Allocation */
static struct ipc_port *ipc_port_new(uint32_t kobject_kind) {
    for (int i = 4; i < MAX_PORTS; i++) {
        if (!g_ports[i].in_use) {
            struct ipc_port *p = &g_ports[i];
            p->in_use = 1;
            p->gen++; /* new liveness epoch (M6) */
            p->has_receive_right = 1;
            p->kobject_kind = kobject_kind;
            p->svc_alias = MACH_PORT_NULL;
            p->queue_head = NULL;
            p->queue_tail = NULL;
            p->msgcount = 0;
            p->qlimit = MACH_PORT_QLIMIT_DEFAULT;
            p->seqno = 0;
            p->mscount = 0;
            p->srights = 0;
            p->sorights = 0;
            p->context = 0;
            p->guard = 0;
            p->guarded = 0;
            p->guard_flags = 0;
            p->pset_name = 0;
            p->pset_member_count = 0;
            p->timer_deadline = 0;
            p->timer_armed = 0;
            p->nsrequest_port = 0;
            p->nsrequest_mscount = 0;
            p->dnrequest_port = 0;
            p->is_reply_port = 0;
            p->is_special_reply_port = 0;
            p->mem_fd = -1;
            p->mem_size = 0;
            p->mem_offset = 0;
            p->mem_prot = 0;
            p->mem_vaddr = 0;
            return p;
        }
    }
    return NULL;
}

static struct ipc_entry *ipc_entry_new(mach_port_name_t *out_name) {
    for (int i = 4; i < MAX_ENTRIES; i++) {
        if (!g_entries[i].in_use) {
            g_entries[i].in_use = 1;
            uint16_t gen = g_entries[i].generation;
            if ((gen & 3) != 3) gen = 3;
            g_entries[i].generation = gen;
            g_entries[i].port = NULL;
            g_entries[i].type = MACH_PORT_TYPE_NONE;
            g_entries[i].urefs = 0;
            g_entries[i].dead_urefs = 0;
            *out_name = (mach_port_name_t)((i << 8) | (gen & 0xff));
            return &g_entries[i];
        }
    }
    return NULL;
}

static struct ipc_entry *ipc_entry_lookup(mach_port_name_t name) {
    if (name == 0 || name == MACH_PORT_DEAD) return NULL;
    if (name == 0x104) return &g_host_entry;
    uint32_t idx = name >> 8;
    uint32_t gen = name & 0xff;
    if (idx < MAX_ENTRIES && g_entries[idx].in_use && g_entries[idx].generation == gen) {
        return &g_entries[idx];
    }
    return NULL;
}

struct ipc_port *ipc_port_from_name(mach_port_name_t name) {
    struct ipc_entry *e = ipc_entry_lookup(name);
    return e ? e->port : NULL;
}

static void ipc_entry_free(struct ipc_entry *e) {
    if (!e || e == &g_host_entry) return;
    e->in_use = 0;
    e->port = NULL;
    e->type = MACH_PORT_TYPE_NONE;
    e->urefs = 0;
    e->dead_urefs = 0;
    e->generation += 4;
    if ((e->generation & 0xff) > 0xfb || (e->generation & 3) != 3) e->generation = 3;
}

static mach_port_name_t ipc_port_get_name_for_recv(struct ipc_port *port) {
    for (int i = 1; i < MAX_ENTRIES; i++) {
        if (g_entries[i].in_use && g_entries[i].port == port && (g_entries[i].type & MACH_PORT_TYPE_RECEIVE)) {
            return (mach_port_name_t)((i << 8) | g_entries[i].generation);
        }
    }
    for (int i = 1; i < MAX_ENTRIES; i++) {
        if (g_entries[i].in_use && g_entries[i].port == port) {
            return (mach_port_name_t)((i << 8) | g_entries[i].generation);
        }
    }
    return MACH_PORT_NULL;
}

static mach_port_name_t ipc_right_copyout(struct ipc_port *port, uint8_t right_type) {
    if (!port) return MACH_PORT_NULL;
    /* Find existing entry */
    for (int i = 1; i < MAX_ENTRIES; i++) {
        if (g_entries[i].in_use && g_entries[i].port == port) {
            if (right_type == MACH_MSG_TYPE_PORT_SEND) {
                g_entries[i].type |= MACH_PORT_TYPE_SEND;
                g_entries[i].urefs++;
                return (mach_port_name_t)((i << 8) | g_entries[i].generation);
            }
        }
    }
    /* Allocate new entry */
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) return MACH_PORT_NULL;
    e->port = port;
    if (right_type == MACH_MSG_TYPE_PORT_SEND) {
        e->type = MACH_PORT_TYPE_SEND;
        e->urefs = 1;
    } else if (right_type == MACH_MSG_TYPE_PORT_SEND_ONCE) {
        e->type = MACH_PORT_TYPE_SEND_ONCE;
        e->urefs = 1;
    } else if (right_type == MACH_MSG_TYPE_PORT_RECEIVE) {
        e->type = MACH_PORT_TYPE_RECEIVE;
        e->urefs = 0;
    }
    return name;
}

mach_port_t ipc_port_allocate_thread(void) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_port *port = ipc_port_new(PORT_KOBJECT_THREAD);
    if (!port) {
        ipc_unlock();
        return 0x303;
    }
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        port->in_use = 0;
        ipc_unlock();
        return 0x303;
    }
    e->port = port;
    e->type = MACH_PORT_TYPE_SEND;
    e->urefs = 1;
    port->srights = 1;
    ipc_unlock();
    return name;
}

int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size, uint8_t *rep_buf, uint32_t rep_cap, uint32_t *rep_size);

mach_port_name_t ipc_svchost_port_create(void) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_port *port = ipc_port_new(PORT_KOBJECT_SVCHOST);
    if (!port) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        port->in_use = 0;
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    e->port = port;
    e->type = MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND;
    e->urefs = 1;
    port->srights = 1;
    ipc_unlock();
    return name;
}

mach_port_name_t ipc_iokit_port_create(void) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_port *port = ipc_port_new(PORT_KOBJECT_IOKIT);
    if (!port) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        port->in_use = 0;
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    e->port = port;
    e->type = MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND;
    e->urefs = 1;
    port->srights = 1;
    ipc_unlock();
    return name;
}


mach_port_name_t ipc_notifyd_port_create(void) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_port *port = ipc_port_new(PORT_KOBJECT_NOTIFYD);
    if (!port) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    port->qlimit = NOTIFYD_SERVER_QLIMIT;
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        port->in_use = 0;
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    e->port = port;
    e->type = MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND;
    e->urefs = 1;
    port->srights = 1;
    ipc_unlock();
    return name;
}

mach_port_name_t ipc_notifyd_gen_port_locked(uint32_t qlimit,
                                            mach_port_name_t *send_name_out) {
    struct ipc_port *port = ipc_port_new(PORT_KOBJECT_NONE);
    mach_port_name_t recv_name = 0;
    mach_port_name_t send_name = 0;
    struct ipc_entry *re;
    struct ipc_entry *se;
    if (!port) return MACH_PORT_NULL;
    port->qlimit = qlimit;
    re = ipc_entry_new(&recv_name);
    if (!re) {
        port->in_use = 0;
        return MACH_PORT_NULL;
    }
    se = ipc_entry_new(&send_name);
    if (!se) {
        ipc_entry_free(re);
        port->in_use = 0;
        return MACH_PORT_NULL;
    }
    re->port = port;
    re->type = MACH_PORT_TYPE_RECEIVE;
    re->urefs = 1;
    se->port = port;
    se->type = MACH_PORT_TYPE_SEND;
    se->urefs = 1;
    port->srights = 1;
    if (send_name_out) *send_name_out = send_name;
    return recv_name;
}

int ipc_notifyd_deliver_locked(mach_port_name_t dest, uint32_t msgh_id) {
    struct ipc_entry *e = ipc_entry_lookup(dest);
    struct ipc_port *p;
    struct ipc_kmsg *kmsg;
    mach_msg_header_t *h;
    if (!e || !e->port) return 0;
    p = e->port;
    if (!p->has_receive_right) return 0;
    kmsg = kmsg_alloc(sizeof(mach_msg_header_t));
    if (!kmsg) return 0;
    h = (mach_msg_header_t *)kmsg->data;
    h->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0);
    h->msgh_size = sizeof(mach_msg_header_t);
    h->msgh_remote_port = dest;
    h->msgh_local_port = MACH_PORT_NULL;
    h->msgh_voucher_port = MACH_PORT_NULL;
    h->msgh_id = msgh_id;
    p->seqno++;
    kmsg->seqno = p->seqno;
    if (p->queue_tail) {
        p->queue_tail->next = kmsg;
        p->queue_tail = kmsg;
    } else {
        p->queue_head = kmsg;
        p->queue_tail = kmsg;
    }
    p->msgcount++;
    ipc_notify_all();
    return 1;
}

mach_port_t ipc_mach_reply_port(void) {
    mach_port_name_t port = 0;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_new(&port);
    if (!e) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    struct ipc_port *p = ipc_port_new(PORT_KOBJECT_NONE);
    if (!p) {
        ipc_entry_free(e);
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    p->is_reply_port = 1;
    p->is_special_reply_port = 0;
    e->port = p;
    e->type = MACH_PORT_TYPE_RECEIVE;
    ipc_unlock();
    return port;
}

mach_port_t ipc_mach_special_reply_port(void) {
    mach_port_name_t port = 0;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_new(&port);
    if (!e) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    struct ipc_port *p = ipc_port_new(PORT_KOBJECT_NONE);
    if (!p) {
        ipc_entry_free(e);
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    p->is_reply_port = 0;
    p->is_special_reply_port = 1;
    e->port = p;
    e->type = MACH_PORT_TYPE_RECEIVE;
    ipc_unlock();
    return port;
}

/* Port Operations */
int ipc_port_allocate(mach_port_t task, int right, mach_port_name_t *name) {
    (void)task;
    if (!name) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    mach_port_name_t new_name = 0;
    struct ipc_entry *e = ipc_entry_new(&new_name);
    if (!e) {
        ipc_unlock();
        return KERN_RESOURCE_SHORTAGE;
    }

    if (right == MACH_PORT_RIGHT_RECEIVE) {
        struct ipc_port *p = ipc_port_new(PORT_KOBJECT_NONE);
        if (!p) {
            ipc_entry_free(e);
            ipc_unlock();
            return KERN_RESOURCE_SHORTAGE;
        }
        e->port = p;
        e->type = MACH_PORT_TYPE_RECEIVE;
    } else if (right == MACH_PORT_RIGHT_PORT_SET) {
        e->port = NULL;
        e->type = MACH_PORT_TYPE_PORT_SET;
    } else if (right == MACH_PORT_RIGHT_DEAD_NAME) {
        e->port = NULL;
        e->type = MACH_PORT_TYPE_DEAD_NAME;
        e->dead_urefs = 1;
    } else {
        ipc_entry_free(e);
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }

    *name = new_name;
    if (g_ipc_trace) {
        ipc_print_str("[ipc] allocate right=");
        ipc_print_uint((uint32_t)right);
        ipc_print_str(" port=0x");
        ipc_print_hex(new_name);
        ipc_print_str("\n");
    }
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_allocate_name(mach_port_t task, int right, mach_port_name_t name) {
    (void)task;
    if (name == 0 || name == MACH_PORT_DEAD) return KERN_INVALID_NAME;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *existing = ipc_entry_lookup(name);
    if (existing) {
        ipc_unlock();
        return KERN_NAME_EXISTS;
    }

    uint32_t idx = name >> 8;
    uint32_t gen = name & 0xff;
    if (idx >= MAX_ENTRIES || g_entries[idx].in_use) {
        ipc_unlock();
        return KERN_RESOURCE_SHORTAGE;
    }

    struct ipc_entry *e = &g_entries[idx];
    e->in_use = 1;
    e->generation = gen;

    if (right == MACH_PORT_RIGHT_RECEIVE) {
        struct ipc_port *p = ipc_port_new(PORT_KOBJECT_NONE);
        if (!p) {
            e->in_use = 0;
            ipc_unlock();
            return KERN_RESOURCE_SHORTAGE;
        }
        e->port = p;
        e->type = MACH_PORT_TYPE_RECEIVE;
    } else if (right == MACH_PORT_RIGHT_PORT_SET) {
        e->port = NULL;
        e->type = MACH_PORT_TYPE_PORT_SET;
    } else if (right == MACH_PORT_RIGHT_DEAD_NAME) {
        e->port = NULL;
        e->type = MACH_PORT_TYPE_DEAD_NAME;
        e->dead_urefs = 1;
    } else {
        e->in_use = 0;
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_allocate_full(mach_port_t task, int right, mach_port_t proto, void *qos, mach_port_name_t *name) {
    (void)proto; (void)qos;
    return ipc_port_allocate(task, right, name);
}

int ipc_port_construct(mach_port_t task, mach_port_options_t *options, uint64_t context, mach_port_name_t *name) {
    (void)task;
    if (!name) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    mach_port_name_t new_name = 0;
    struct ipc_entry *e = ipc_entry_new(&new_name);
    if (!e) {
        ipc_unlock();
        return KERN_RESOURCE_SHORTAGE;
    }
    struct ipc_port *p = ipc_port_new(PORT_KOBJECT_NONE);
    if (!p) {
        ipc_entry_free(e);
        ipc_unlock();
        return KERN_RESOURCE_SHORTAGE;
    }

    e->port = p;
    e->type = MACH_PORT_TYPE_RECEIVE;
    p->context = context;

    if (options) {
        if (options->flags & MPO_QLIMIT) {
            p->qlimit = options->mpl.mpl_qlimit;
        }
        if (options->flags & MPO_CONTEXT_AS_GUARD) {
            p->guard = context;
            p->guarded = 1;
        }
        if (options->flags & MPO_INSERT_SEND_RIGHT) {
            e->type |= MACH_PORT_TYPE_SEND;
            e->urefs = 1;
            p->srights = 1;
        }
        if (options->flags & MPO_REPLY_PORT) {
            p->is_reply_port = 1;
        }
    }

    *name = new_name;
    if (g_ipc_trace) {
        ipc_print_str("[ipc] construct port=0x");
        ipc_print_hex(new_name);
        ipc_print_str(" flags=0x");
        ipc_print_hex(options ? options->flags : 0);
        ipc_print_str("\n");
    }
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_destruct(mach_port_t task, mach_port_name_t name, int delta, uint64_t guard) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !(e->type & MACH_PORT_TYPE_RECEIVE)) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *p = e->port;
    if (p && p->guarded && p->guard != guard) {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }

    if (delta > 0 && (e->type & MACH_PORT_TYPE_SEND)) {
        if (e->urefs > (uint32_t)delta) e->urefs -= delta;
        else {
            e->urefs = 0;
            e->type &= ~MACH_PORT_TYPE_SEND;
            if (p) p->srights = 0;
        }
    }

    /* Destroy receive right */
    e->type &= ~MACH_PORT_TYPE_RECEIVE;
    if (p) {
        p->has_receive_right = 0;
        p->gen++; /* new liveness epoch (M6): invalidates in-flight descriptors/reply ports */
        /* Deliver dead name notification if requested */
        if (p->dnrequest_port) {
            struct ipc_entry *dne = ipc_entry_lookup(p->dnrequest_port);
            if (dne && dne->port) {
                struct ipc_kmsg *kmsg = kmsg_alloc(sizeof(mach_dead_name_notification_t));
                if (kmsg) {
                    mach_dead_name_notification_t *n = (mach_dead_name_notification_t *)kmsg->data;
                    n->not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                    n->not_header.msgh_size = sizeof(mach_dead_name_notification_t);
                    n->not_header.msgh_remote_port = p->dnrequest_port;
                    n->not_header.msgh_local_port = MACH_PORT_NULL;
                    n->not_header.msgh_voucher_port = MACH_PORT_NULL;
                    n->not_header.msgh_id = MACH_NOTIFY_DEAD_NAME;
                    n->not_port = name;
                    if (dne->port->queue_tail) {
                        dne->port->queue_tail->next = kmsg;
                        dne->port->queue_tail = kmsg;
                    } else {
                        dne->port->queue_head = kmsg;
                        dne->port->queue_tail = kmsg;
                    }
                    dne->port->msgcount++;
                    /* M5: a knote-driven receiver learns about the notification. */
                    ipc_enqueue_hook_deferred(p->dnrequest_port, sizeof(mach_dead_name_notification_t));
                }
            }
            p->dnrequest_port = 0;
        }
    }

    if (e->type == MACH_PORT_TYPE_NONE) {
        ipc_entry_free(e);
    }
    ipc_notify_all();
    ipc_unlock();
    return KERN_SUCCESS;
}

static void ipc_port_mem_entry_release_locked(struct ipc_port *p) {
    if (!p || p->kobject_kind != PORT_KOBJECT_MEM_ENTRY) return;
    if (p->srights > 0 || p->sorights > 0) return;
    if (p->mem_fd >= 0) {
        sys1(SYS_close, p->mem_fd);
        p->mem_fd = -1;
    }
    p->mem_size = 0;
    p->mem_offset = 0;
    p->mem_prot = 0;
    p->mem_vaddr = 0;
    p->in_use = 0;
    p->gen++;
}

int ipc_port_deallocate(mach_port_t task, mach_port_name_t name) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    if (e->type & MACH_PORT_TYPE_DEAD_NAME) {
        if (e->dead_urefs > 0) e->dead_urefs--;
        if (e->dead_urefs == 0) {
            e->type &= ~MACH_PORT_TYPE_DEAD_NAME;
            if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
        }
        ipc_unlock();
        return KERN_SUCCESS;
    }

    if (e->type & MACH_PORT_TYPE_SEND_ONCE) {
        e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
        if (e->port) {
            if (e->port->sorights > 0) e->port->sorights--;
            if (e->port->has_receive_right) {
                ipc_send_once_notify_port_locked(e->port);
                ipc_notify_all();
            }
        }
        if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
        ipc_unlock();
        return KERN_SUCCESS;
    }

    if (e->type & MACH_PORT_TYPE_SEND) {
        if (e->urefs > 0) e->urefs--;
        if (e->urefs == 0) {
            e->type &= ~MACH_PORT_TYPE_SEND;
            struct ipc_port *p = e->port;
            if (p && p->srights > 0) {
                p->srights--;
                if (p->srights == 0 && p->nsrequest_port != 0) {
                    /* Deliver no-senders notification */
                    struct ipc_entry *nse = ipc_entry_lookup(p->nsrequest_port);
                    if (nse && nse->port) {
                        struct ipc_kmsg *kmsg = kmsg_alloc(sizeof(mach_no_senders_notification_t));
                        if (kmsg) {
                            mach_no_senders_notification_t *n = (mach_no_senders_notification_t *)kmsg->data;
                            n->not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                            n->not_header.msgh_size = sizeof(mach_no_senders_notification_t);
                            n->not_header.msgh_remote_port = p->nsrequest_port;
                            n->not_header.msgh_local_port = MACH_PORT_NULL;
                            n->not_header.msgh_voucher_port = MACH_PORT_NULL;
                            n->not_header.msgh_id = MACH_NOTIFY_NO_SENDERS;
                            n->not_count = p->mscount;
                            if (nse->port->queue_tail) {
                                nse->port->queue_tail->next = kmsg;
                                nse->port->queue_tail = kmsg;
                            } else {
                                nse->port->queue_head = kmsg;
                                nse->port->queue_tail = kmsg;
                            }
                            nse->port->msgcount++;
                            /* M5: a knote-driven receiver learns about the notification. */
                            ipc_enqueue_hook_deferred(p->nsrequest_port, sizeof(mach_no_senders_notification_t));
                        }
                    }
                    p->nsrequest_port = 0;
                    ipc_notify_all();
                }
                ipc_port_mem_entry_release_locked(p);
            }
            if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
        }
        ipc_unlock();
        return KERN_SUCCESS;
    }

    ipc_unlock();
    return KERN_INVALID_RIGHT;
}

int ipc_port_destroy(mach_port_t task, mach_port_name_t name) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *p = e->port;
    if (p && p->kobject_kind == PORT_KOBJECT_MEM_ENTRY) {
        if ((e->type & MACH_PORT_TYPE_SEND) && p->srights > 0) {
            p->srights--;
        }
        ipc_port_mem_entry_release_locked(p);
        ipc_entry_free(e);
        ipc_notify_all();
        ipc_unlock();
        return KERN_SUCCESS;
    }
    if (p) {
        p->has_receive_right = 0;
        p->gen++; /* new liveness epoch (M6): invalidates in-flight descriptors/reply ports */
        /* Deliver dead name notification if requested */
        if (p->dnrequest_port) {
            struct ipc_entry *dne = ipc_entry_lookup(p->dnrequest_port);
            if (dne && dne->port) {
                struct ipc_kmsg *kmsg = kmsg_alloc(sizeof(mach_dead_name_notification_t));
                if (kmsg) {
                    mach_dead_name_notification_t *n = (mach_dead_name_notification_t *)kmsg->data;
                    n->not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                    n->not_header.msgh_size = sizeof(mach_dead_name_notification_t);
                    n->not_header.msgh_remote_port = p->dnrequest_port;
                    n->not_header.msgh_local_port = MACH_PORT_NULL;
                    n->not_header.msgh_voucher_port = MACH_PORT_NULL;
                    n->not_header.msgh_id = MACH_NOTIFY_DEAD_NAME;
                    n->not_port = name;
                    if (dne->port->queue_tail) {
                        dne->port->queue_tail->next = kmsg;
                        dne->port->queue_tail = kmsg;
                    } else {
                        dne->port->queue_head = kmsg;
                        dne->port->queue_tail = kmsg;
                    }
                    dne->port->msgcount++;
                    /* M5: a knote-driven receiver learns about the notification. */
                    ipc_enqueue_hook_deferred(p->dnrequest_port, sizeof(mach_dead_name_notification_t));
                }
            }
            p->dnrequest_port = 0;
        }
        /* Drain message queue */
        struct ipc_kmsg *cur = p->queue_head;
        while (cur) {
            struct ipc_kmsg *nxt = cur->next;
            /* M6: only notify a reply port that is still the same live port
             * recorded at send time. */
            if (cur->reply_port && cur->reply_port->in_use &&
                cur->reply_port->gen == cur->reply_gen &&
                cur->reply_type == MACH_MSG_TYPE_PORT_SEND_ONCE) {
                ipc_send_once_notify_port_locked(cur->reply_port);
            }
            kmsg_free(cur);
            cur = nxt;
        }
        p->queue_head = NULL;
        p->queue_tail = NULL;
        p->msgcount = 0;
    }

    ipc_entry_free(e);
    ipc_notify_all();
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_mod_refs(mach_port_t task, mach_port_name_t name, int right, int delta) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    if (right == MACH_PORT_RIGHT_SEND) {
        if (!(e->type & MACH_PORT_TYPE_SEND)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        int new_refs = (int)e->urefs + delta;
        if (new_refs < 0) {
            ipc_unlock();
            return KERN_INVALID_VALUE;
        }
        e->urefs = (uint32_t)new_refs;
        if (e->urefs == 0) {
            e->type &= ~MACH_PORT_TYPE_SEND;
            if (e->port && e->port->srights > 0) {
                e->port->srights--;
                if (e->port->srights == 0 && e->port->nsrequest_port != 0) {
                    /* Deliver no-senders notification */
                    struct ipc_entry *nse = ipc_entry_lookup(e->port->nsrequest_port);
                    if (nse && nse->port) {
                        struct ipc_kmsg *kmsg = kmsg_alloc(sizeof(mach_no_senders_notification_t));
                        if (kmsg) {
                            mach_no_senders_notification_t *n = (mach_no_senders_notification_t *)kmsg->data;
                            n->not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                            n->not_header.msgh_size = sizeof(mach_no_senders_notification_t);
                            n->not_header.msgh_remote_port = e->port->nsrequest_port;
                            n->not_header.msgh_local_port = MACH_PORT_NULL;
                            n->not_header.msgh_voucher_port = MACH_PORT_NULL;
                            n->not_header.msgh_id = MACH_NOTIFY_NO_SENDERS;
                            n->not_count = e->port->mscount;
                            if (nse->port->queue_tail) {
                                nse->port->queue_tail->next = kmsg;
                                nse->port->queue_tail = kmsg;
                            } else {
                                nse->port->queue_head = kmsg;
                                nse->port->queue_tail = kmsg;
                            }
                            nse->port->msgcount++;
                            /* M5: a knote-driven receiver learns about the notification. */
                            ipc_enqueue_hook_deferred(e->port->nsrequest_port, sizeof(mach_no_senders_notification_t));
                        }
                    }
                    e->port->nsrequest_port = 0;
                    ipc_notify_all();
                }
                ipc_port_mem_entry_release_locked(e->port);
            }
            if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
        }
        ipc_unlock();
        return KERN_SUCCESS;
    }

    if (right == MACH_PORT_RIGHT_RECEIVE) {
        if (!(e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        if (delta == -1) {
            e->type &= ~MACH_PORT_TYPE_RECEIVE;
            if (e->port) {
                e->port->has_receive_right = 0;
                /* M6: new liveness epoch (same as destruct/destroy): a
                 * queued descriptor or reply port for this port must not
                 * validate after the receive right is gone. */
                e->port->gen++;
            }
            if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
            ipc_notify_all();
            ipc_unlock();
            return KERN_SUCCESS;
        }
        ipc_unlock();
        return KERN_INVALID_VALUE;
    }

    if (right == MACH_PORT_RIGHT_DEAD_NAME) {
        int new_refs = (int)e->dead_urefs + delta;
        if (new_refs < 0) {
            ipc_unlock();
            return KERN_INVALID_VALUE;
        }
        e->dead_urefs = (uint32_t)new_refs;
        if (e->dead_urefs == 0) {
            e->type &= ~MACH_PORT_TYPE_DEAD_NAME;
            if (e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(e);
        }
        ipc_unlock();
        return KERN_SUCCESS;
    }

    ipc_unlock();
    return KERN_INVALID_RIGHT;
}

int ipc_port_insert_right(mach_port_t task, mach_port_name_t name, mach_port_t poly, int polyPoly) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *poly_e = ipc_entry_lookup(poly);
    if (!poly_e || !poly_e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *port = poly_e->port;

    struct ipc_entry *dest_e = ipc_entry_lookup(name);
    if (!dest_e) {
        /* Create entry with that name if available */
        uint32_t idx = name >> 8;
        uint32_t gen = name & 0xff;
        if (idx < MAX_ENTRIES && !g_entries[idx].in_use) {
            dest_e = &g_entries[idx];
            dest_e->in_use = 1;
            dest_e->generation = gen;
            dest_e->port = port;
            dest_e->type = MACH_PORT_TYPE_NONE;
            dest_e->urefs = 0;
            dest_e->dead_urefs = 0;
        } else {
            ipc_unlock();
            return KERN_INVALID_NAME;
        }
    }

    if (polyPoly == MACH_MSG_TYPE_MAKE_SEND) {
        if (!(poly_e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        dest_e->port = port;
        dest_e->type |= MACH_PORT_TYPE_SEND;
        dest_e->urefs++;
        port->srights++;
        port->mscount++;
    } else if (polyPoly == MACH_MSG_TYPE_COPY_SEND) {
        if (!(poly_e->type & MACH_PORT_TYPE_SEND)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        dest_e->port = port;
        dest_e->type |= MACH_PORT_TYPE_SEND;
        dest_e->urefs++;
        port->srights++;
    } else if (polyPoly == MACH_MSG_TYPE_MOVE_RECEIVE) {
        if (!(poly_e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        poly_e->type &= ~MACH_PORT_TYPE_RECEIVE;
        dest_e->port = port;
        dest_e->type |= MACH_PORT_TYPE_RECEIVE;
        if (poly_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(poly_e);
    } else if (polyPoly == MACH_MSG_TYPE_MAKE_SEND_ONCE) {
        if (!(poly_e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        dest_e->port = port;
        dest_e->type |= MACH_PORT_TYPE_SEND_ONCE;
        dest_e->urefs = 1;
        port->sorights++;
    } else {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_extract_right(mach_port_t task, mach_port_name_t name, int desired_right, mach_port_t *poly, int *acquired) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    if (desired_right == MACH_MSG_TYPE_MAKE_SEND) {
        if (!(e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        if (e->port) {
            e->port->srights++;
            e->port->mscount++;
        }
        if (poly) *poly = name;
        if (acquired) *acquired = MACH_MSG_TYPE_PORT_SEND;
        ipc_unlock();
        return KERN_SUCCESS;
    }

    if (poly) *poly = name;
    if (acquired) *acquired = desired_right;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_type(mach_port_t task, mach_port_name_t name, int *ptype) {
    (void)task;
    if (!ptype) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    *ptype = (int)e->type;
    ipc_unlock();
    return KERN_SUCCESS;
}

/* Port set of a port: the name of the port set the named port is
 * a member of, or MACH_PORT_NULL when it is in no set. Used by the kevent
 * machport notify to fire knotes registered on a port set. Same locking
 * discipline as ipc_port_has_receive_right / ipc_port_peek_size: takes
 * ipc_lock and releases it raw, because kevent_apply_changes calls it while
 * holding kq_lock and a hook drain there would take kq_lock again. */
uint32_t ipc_port_get_pset_name(mach_port_name_t name) {
    if (!g_ipc_inited) ipc_init();
    /* Lock-free read: taking ipc_lock here (under kq_lock) deadlocked on
     * device against a thread holding ipc_lock and waiting for kq_lock.
     * Entries live in a static table; a stale read only delays a wakeup. */
    struct ipc_entry *e = ipc_entry_lookup(name);
    uint32_t pset = 0;
    if (e && e->port) {
        pset = __atomic_load_n(&e->port->pset_name, __ATOMIC_ACQUIRE);
    }
    return pset;
}

int ipc_port_has_receive_right(mach_port_name_t name) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    int has = 0;
    if (e && (e->type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_PORT_SET))) {
        has = 1;
    }
    /* Raw unlock (no hook drain): kevent_apply_changes calls this while
     * holding kq_lock, and draining would take kq_lock again. */
    ipc_unlock_raw();
    return has;
}

int ipc_port_names(mach_port_t task, mach_port_name_t **names, uint32_t *names_cnt, mach_port_type_t **types, uint32_t *types_cnt) {
    if (!names || !names_cnt || !types || !types_cnt) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    uint32_t count = 0;
    for (int i = 1; i < MAX_ENTRIES; i++) {
        if (g_entries[i].in_use) count++;
    }

    uint64_t names_addr = 0;
    uint64_t types_addr = 0;
    size_t names_sz = count * sizeof(mach_port_name_t);
    size_t types_sz = count * sizeof(mach_port_type_t);
    if (names_sz == 0) names_sz = 16384;
    if (types_sz == 0) types_sz = 16384;

    ipc_unlock();
    mach_vm_allocate(task, &names_addr, names_sz, 1 /* ANYWHERE */);
    mach_vm_allocate(task, &types_addr, types_sz, 1 /* ANYWHERE */);
    ipc_lock();

    mach_port_name_t *out_n = (mach_port_name_t *)names_addr;
    mach_port_type_t *out_t = (mach_port_type_t *)types_addr;
    uint32_t idx = 0;
    for (int i = 1; i < MAX_ENTRIES && idx < count; i++) {
        if (g_entries[i].in_use) {
            out_n[idx] = (mach_port_name_t)((i << 8) | g_entries[i].generation);
            out_t[idx] = g_entries[i].type;
            idx++;
        }
    }

    *names = out_n;
    *names_cnt = idx;
    *types = out_t;
    *types_cnt = idx;

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_get_refs(mach_port_t task, mach_port_name_t name, int right, uint32_t *refs) {
    (void)task;
    if (!refs) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    if (right == MACH_PORT_RIGHT_SEND) {
        *refs = (e->type & MACH_PORT_TYPE_SEND) ? e->urefs : 0;
    } else if (right == MACH_PORT_RIGHT_RECEIVE) {
        *refs = (e->type & MACH_PORT_TYPE_RECEIVE) ? 1 : 0;
    } else if (right == MACH_PORT_RIGHT_SEND_ONCE) {
        *refs = (e->type & MACH_PORT_TYPE_SEND_ONCE) ? 1 : 0;
    } else if (right == MACH_PORT_RIGHT_PORT_SET) {
        *refs = (e->type & MACH_PORT_TYPE_PORT_SET) ? 1 : 0;
    } else if (right == MACH_PORT_RIGHT_DEAD_NAME) {
        *refs = (e->type & MACH_PORT_TYPE_DEAD_NAME) ? e->dead_urefs : 0;
    } else {
        *refs = 0;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_get_attributes(mach_port_t task, mach_port_name_t name, int flavor, void *info, uint32_t *cnt) {
    (void)task;
    if (!info || !cnt) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *p = e->port;

    if (flavor == MACH_PORT_LIMITS_INFO) {
        if (*cnt < 1) { ipc_unlock(); return KERN_INVALID_ARGUMENT; }
        mach_port_limits_t *lim = (mach_port_limits_t *)info;
        lim->mpl_qlimit = p->qlimit;
        *cnt = 1;
    } else if (flavor == MACH_PORT_RECEIVE_STATUS) {
        if (!(e->type & MACH_PORT_TYPE_RECEIVE)) {
            ipc_unlock();
            return KERN_INVALID_RIGHT;
        }
        if (*cnt < sizeof(mach_port_status_t) / sizeof(uint32_t)) {
            ipc_unlock(); return KERN_INVALID_ARGUMENT;
        }
        mach_port_status_t *s = (mach_port_status_t *)info;
        s->mps_pset = p->pset_name;
        s->mps_seqno = p->seqno;
        s->mps_qlimit = p->qlimit;
        s->mps_msgcount = p->msgcount;
        s->mps_mscount = p->mscount;
        s->mps_srights = p->srights;
        s->mps_sorights = p->sorights;
        s->mps_nsrequest = p->nsrequest_port ? 1 : 0;
        s->mps_flags = 0;
        *cnt = sizeof(mach_port_status_t) / sizeof(uint32_t);
    } else if (flavor == MACH_PORT_GUARD_INFO) {
        uint64_t *g = (uint64_t *)info;
        *g = p->guard;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_set_attributes(mach_port_t task, mach_port_name_t name, int flavor, void *info, uint32_t cnt) {
    (void)task; (void)cnt;
    if (!info) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *p = e->port;

    if (flavor == MACH_PORT_LIMITS_INFO) {
        mach_port_limits_t *lim = (mach_port_limits_t *)info;
        p->qlimit = lim->mpl_qlimit;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_set_context(mach_port_t task, mach_port_name_t name, uint64_t context) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    e->port->context = context;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_get_context(mach_port_t task, mach_port_name_t name, uint64_t *context) {
    (void)task;
    if (!context) return KERN_INVALID_ARGUMENT;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    *context = e->port->context;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_guard(mach_port_t task, mach_port_name_t name, uint64_t guard, int strict) {
    (void)task; (void)strict;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (e->port->guarded) {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }
    e->port->guard = guard;
    e->port->guarded = 1;
    e->port->context = guard; /* XNU: the guard lives in ip_context, so it is also the trailer msgh_context
                               * (libxpc finds the connection of an async reply that way, 0x197fa287c). */
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_unguard(mach_port_t task, mach_port_name_t name, uint64_t guard) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (e->port->guarded && e->port->guard != guard) {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }
    e->port->guard = 0;
    e->port->guarded = 0;
    e->port->context = 0;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_guard_with_flags(mach_port_t task, mach_port_name_t name, uint64_t guard, uint64_t flags) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (e->port->guarded) {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }
    e->port->guard = guard;
    e->port->context = guard; /* XNU: guard == ip_context */
    e->port->guarded = 1;
    e->port->guard_flags = (uint32_t)flags;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_swap_guard(mach_port_t task, mach_port_name_t name, uint64_t old_guard, uint64_t new_guard) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (e->port->guarded && e->port->guard != old_guard) {
        ipc_unlock();
        return KERN_INVALID_ARGUMENT;
    }
    e->port->guard = new_guard;
    e->port->guarded = 1;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_insert_member(mach_port_t task, mach_port_name_t name, mach_port_name_t pset) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *me = ipc_entry_lookup(name);
    struct ipc_entry *se = ipc_entry_lookup(pset);
    if (!me || !(me->type & MACH_PORT_TYPE_RECEIVE)) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (!se || !(se->type & MACH_PORT_TYPE_PORT_SET)) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    struct ipc_port *member_p = me->port;
    if (member_p->pset_name == pset) {
        if (g_ipc_trace) {
            ipc_print_str("[ipc] insert_member member=0x");
            ipc_print_hex(name);
            ipc_print_str(" pset=0x");
            ipc_print_hex(pset);
            ipc_print_str(" already in set\n");
        }
        ipc_unlock();
        return KERN_ALREADY_IN_SET;
    }

    member_p->pset_name = pset;
    if (g_ipc_trace) {
        ipc_print_str("[ipc] insert_member member=0x");
        ipc_print_hex(name);
        ipc_print_str(" pset=0x");
        ipc_print_hex(pset);
        ipc_print_str("\n");
    }
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_extract_member(mach_port_t task, mach_port_name_t name, mach_port_name_t pset) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *me = ipc_entry_lookup(name);
    struct ipc_entry *se = ipc_entry_lookup(pset);
    if (!me || !(me->type & MACH_PORT_TYPE_RECEIVE)) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (!se || !(se->type & MACH_PORT_TYPE_PORT_SET)) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }

    if (me->port->pset_name != pset) {
        ipc_unlock();
        return KERN_NOT_IN_SET;
    }

    me->port->pset_name = 0;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_move_member(mach_port_t task, mach_port_name_t member, mach_port_name_t after) {
    if (after == MACH_PORT_NULL) {
        ipc_lock();
        struct ipc_entry *me = ipc_entry_lookup(member);
        if (me && me->port) me->port->pset_name = 0;
        if (g_ipc_trace) {
            ipc_print_str("[ipc] move_member member=0x");
            ipc_print_hex(member);
            ipc_print_str(" pset=0 (removed)\n");
        }
        ipc_unlock();
        return KERN_SUCCESS;
    }
    return ipc_port_insert_member(task, member, after);
}

int ipc_port_request_notification(mach_port_t task, mach_port_name_t name, int id, uint32_t sync, mach_port_t notify, int poly, mach_port_t *previous) {
    (void)task; (void)poly;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();

    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    struct ipc_port *p = e->port;

    if (id == MACH_NOTIFY_NO_SENDERS) {
        if (previous) *previous = p->nsrequest_port;
        p->nsrequest_port = notify;
        p->nsrequest_mscount = sync;
        if (p->srights == 0 && notify != 0) {
            /* Immediate notification */
            struct ipc_entry *nse = ipc_entry_lookup(notify);
            if (nse && nse->port) {
                struct ipc_kmsg *kmsg = kmsg_alloc(sizeof(mach_no_senders_notification_t));
                if (kmsg) {
                    mach_no_senders_notification_t *n = (mach_no_senders_notification_t *)kmsg->data;
                    n->not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                    n->not_header.msgh_size = sizeof(mach_no_senders_notification_t);
                    n->not_header.msgh_remote_port = notify;
                    n->not_header.msgh_local_port = MACH_PORT_NULL;
                    n->not_header.msgh_voucher_port = MACH_PORT_NULL;
                    n->not_header.msgh_id = MACH_NOTIFY_NO_SENDERS;
                    n->not_count = p->mscount;
                    if (nse->port->queue_tail) {
                        nse->port->queue_tail->next = kmsg;
                        nse->port->queue_tail = kmsg;
                    } else {
                        nse->port->queue_head = kmsg;
                        nse->port->queue_tail = kmsg;
                    }
                    nse->port->msgcount++;
                    /* M5: a knote-driven receiver learns about the notification. */
                    ipc_enqueue_hook_deferred(notify, sizeof(mach_no_senders_notification_t));
                }
            }
            p->nsrequest_port = 0;
            ipc_notify_all();
        }
    } else if (id == MACH_NOTIFY_DEAD_NAME) {
        if (previous) *previous = p->dnrequest_port;
        p->dnrequest_port = notify;
    } else {
        if (previous) *previous = 0;
    }

    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_set_mscount(mach_port_t task, mach_port_name_t name, uint32_t mscount) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    e->port->mscount = mscount;
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_port_kernel_object(mach_port_t task, mach_port_name_t name, uint32_t *kobject_type, uint64_t *kobject_addr) {
    (void)task;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    if (kobject_type) *kobject_type = e->port->kobject_kind;
    if (kobject_addr) *kobject_addr = 0;
    ipc_unlock();
    return KERN_SUCCESS;
}

/* mk_timer implementation */
mach_port_name_t ipc_mk_timer_create(void) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    mach_port_name_t name = 0;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    struct ipc_port *p = ipc_port_new(PORT_KOBJECT_TIMER);
    if (!p) {
        ipc_entry_free(e);
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    e->port = p;
    e->type = MACH_PORT_TYPE_RECEIVE;
    ipc_unlock();
    return name;
}

int ipc_mk_timer_destroy(mach_port_name_t name) {
    return ipc_port_destroy(ipc_task_self(), name);
}

int ipc_mk_timer_arm(mach_port_name_t name, uint64_t deadline) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port || e->port->kobject_kind != PORT_KOBJECT_TIMER) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    e->port->timer_deadline = deadline;
    e->port->timer_armed = 1;
    ipc_notify_all();
    ipc_unlock();
    return KERN_SUCCESS;
}

int ipc_mk_timer_arm_leeway(mach_port_name_t name, uint64_t flags, uint64_t deadline, uint64_t leeway) {
    (void)flags; (void)leeway;
    return ipc_mk_timer_arm(name, deadline);
}

int ipc_mk_timer_cancel(mach_port_name_t name, uint64_t *result_time) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port || e->port->kobject_kind != PORT_KOBJECT_TIMER) {
        ipc_unlock();
        return KERN_INVALID_NAME;
    }
    e->port->timer_armed = 0;
    /* Drain unconsumed timer expiry messages */
    struct ipc_kmsg *prev = NULL;
    struct ipc_kmsg *cur = e->port->queue_head;
    while (cur) {
        mach_msg_header_t *h = (mach_msg_header_t *)cur->data;
        if (h->msgh_id == 0) {
            struct ipc_kmsg *del = cur;
            if (prev) prev->next = cur->next;
            else e->port->queue_head = cur->next;
            if (e->port->queue_tail == del) e->port->queue_tail = prev;
            cur = cur->next;
            kmsg_free(del);
            e->port->msgcount--;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
    if (result_time) *result_time = mach_absolute_time();
    ipc_unlock();
    return KERN_SUCCESS;
}

static void ipc_fire_timer_locked(struct ipc_port *p, mach_port_name_t port_name) {
    p->timer_armed = 0;
    struct ipc_kmsg *kmsg = kmsg_alloc(24);
    if (!kmsg) return;
    mach_msg_header_t *h = (mach_msg_header_t *)kmsg->data;
    h->msgh_bits = MACH_MSGH_BITS(0, 0);
    h->msgh_size = 24;
    h->msgh_remote_port = MACH_PORT_NULL;
    h->msgh_local_port = port_name;
    h->msgh_voucher_port = MACH_PORT_NULL;
    h->msgh_id = 0;

    if (p->queue_tail) {
        p->queue_tail->next = kmsg;
        p->queue_tail = kmsg;
    } else {
        p->queue_head = kmsg;
        p->queue_tail = kmsg;
    }
    p->msgcount++;
    /* M7: deferred out of ipc_lock (the hook takes kq_lock then wq_lock). */
    ipc_enqueue_hook_deferred(port_name, 24);
}

/* Check deadlines of a port or port set */
static void ipc_check_timers_locked(struct ipc_entry *e, mach_port_name_t name, uint64_t now) {
    if (e->type & MACH_PORT_TYPE_RECEIVE) {
        struct ipc_port *p = e->port;
        if (p && p->kobject_kind == PORT_KOBJECT_TIMER && p->timer_armed && now >= p->timer_deadline) {
            ipc_fire_timer_locked(p, name);
        }
    } else if (e->type & MACH_PORT_TYPE_PORT_SET) {
        for (int i = 1; i < MAX_ENTRIES; i++) {
            if (g_entries[i].in_use && (g_entries[i].type & MACH_PORT_TYPE_RECEIVE) && g_entries[i].port) {
                struct ipc_port *p = g_entries[i].port;
                if (p->pset_name == name && p->kobject_kind == PORT_KOBJECT_TIMER && p->timer_armed && now >= p->timer_deadline) {
                    mach_port_name_t mname = (mach_port_name_t)((i << 8) | g_entries[i].generation);
                    ipc_fire_timer_locked(p, mname);
                }
            }
        }
    }
}

static uint64_t ipc_earliest_timer_locked(struct ipc_entry *e, mach_port_name_t name) {
    uint64_t earliest = 0;
    if (e->type & MACH_PORT_TYPE_RECEIVE) {
        struct ipc_port *p = e->port;
        if (p && p->kobject_kind == PORT_KOBJECT_TIMER && p->timer_armed) {
            earliest = p->timer_deadline;
        }
    } else if (e->type & MACH_PORT_TYPE_PORT_SET) {
        for (int i = 1; i < MAX_ENTRIES; i++) {
            if (g_entries[i].in_use && (g_entries[i].type & MACH_PORT_TYPE_RECEIVE) && g_entries[i].port) {
                struct ipc_port *p = g_entries[i].port;
                if (p->pset_name == name && p->kobject_kind == PORT_KOBJECT_TIMER && p->timer_armed) {
                    if (earliest == 0 || p->timer_deadline < earliest) {
                        earliest = p->timer_deadline;
                    }
                }
            }
        }
    }
    return earliest;
}

/* Core Messaging */
static int ipc_send_locked(mach_msg_header_t *hdr, uint32_t send_size, uint64_t option) {
    (void)option;
    if (send_size < sizeof(mach_msg_header_t)) {
        return MACH_SEND_MSG_TOO_SMALL;
    }

    mach_port_name_t dest_name = hdr->msgh_remote_port;
    if (dest_name == 0) {
        return MACH_SEND_INVALID_DEST;
    }

    /* Validate destination rights in table */
    struct ipc_entry *dest_e = NULL;
    struct ipc_port *dest_p = NULL;
    int dest_is_dead = (dest_name == 0 || dest_name == MACH_PORT_DEAD);

    if (!dest_is_dead) {
        dest_e = ipc_entry_lookup(dest_name);
        if (!dest_e) {
            dest_is_dead = 1;
        } else if (dest_e->type & MACH_PORT_TYPE_DEAD_NAME) {
            dest_is_dead = 1;
        } else {
            dest_p = dest_e->port;
            if (!dest_p || (!dest_p->has_receive_right && dest_p->kobject_kind == PORT_KOBJECT_NONE)) {
                dest_is_dead = 1;
            }
        }
    }

    uint32_t remote_disp = MACH_MSGH_BITS_REMOTE(hdr->msgh_bits);
    if (remote_disp == MACH_MSG_TYPE_COPY_SEND || remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
        if (!dest_is_dead && !(dest_e->type & MACH_PORT_TYPE_SEND)) return MACH_SEND_INVALID_RIGHT;
        if (dest_e && !(dest_e->type & (MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_DEAD_NAME))) return MACH_SEND_INVALID_RIGHT;
    } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
        if (!dest_is_dead && !(dest_e->type & MACH_PORT_TYPE_SEND_ONCE)) return MACH_SEND_INVALID_RIGHT;
        if (dest_e && !(dest_e->type & (MACH_PORT_TYPE_SEND_ONCE | MACH_PORT_TYPE_DEAD_NAME))) return MACH_SEND_INVALID_RIGHT;
    } else if (remote_disp == MACH_MSG_TYPE_MAKE_SEND || remote_disp == MACH_MSG_TYPE_MAKE_SEND_ONCE) {
        if (dest_is_dead || !dest_e || !(dest_e->type & MACH_PORT_TYPE_RECEIVE)) return MACH_SEND_INVALID_RIGHT;
    } else {
        return MACH_SEND_INVALID_RIGHT;
    }

    /* Reply port validation */
    mach_port_name_t reply_name = hdr->msgh_local_port;
    struct ipc_entry *reply_e = NULL;
    uint32_t local_disp = MACH_MSGH_BITS_LOCAL(hdr->msgh_bits);
    if (reply_name != MACH_PORT_NULL) {
        reply_e = ipc_entry_lookup(reply_name);
        if (!reply_e) return MACH_SEND_INVALID_REPLY;
        if (local_disp == MACH_MSG_TYPE_MAKE_SEND_ONCE || local_disp == MACH_MSG_TYPE_MAKE_SEND) {
            if (!(reply_e->type & MACH_PORT_TYPE_RECEIVE)) return MACH_SEND_INVALID_REPLY;
        } else if (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
            if (!(reply_e->type & MACH_PORT_TYPE_SEND_ONCE)) return MACH_SEND_INVALID_REPLY;
        } else if (local_disp == MACH_MSG_TYPE_MOVE_SEND || local_disp == MACH_MSG_TYPE_COPY_SEND) {
            if (!(reply_e->type & MACH_PORT_TYPE_SEND)) return MACH_SEND_INVALID_REPLY;
        } else {
            return MACH_SEND_INVALID_REPLY;
        }
    }

    /* If destination is dead or has no receiver, the message cannot be delivered.
     * Under Mach IPC semantics, any send-once right carried as the reply port
     * must produce a MACH_NOTIFY_SEND_ONCE (0x47) notification to the reply port.
     */
    if (dest_is_dead) {
        /* Consume moved destination right */
        if (dest_e) {
            if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
                if (dest_e->urefs > 0) dest_e->urefs--;
                if (dest_e->urefs == 0) {
                    dest_e->type &= ~MACH_PORT_TYPE_SEND;
                    if (dest_p && dest_p->srights > 0) dest_p->srights--;
                    if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
                }
            } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (dest_p && dest_p->sorights > 0) dest_p->sorights--;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
        }
        /* Deliver MACH_NOTIFY_SEND_ONCE to reply port if reply right was send-once */
        if (reply_name != MACH_PORT_NULL && reply_e && reply_e->port) {
            if (!reply_e->port->is_special_reply_port) reply_e->port->is_reply_port = 1;
            if (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
                ipc_send_once_notify_port_locked(reply_e->port);
                ipc_notify_all();
            } else if (local_disp == MACH_MSG_TYPE_MAKE_SEND_ONCE) {
                ipc_send_once_notify_port_locked(reply_e->port);
                ipc_notify_all();
            }
        }
        return MACH_SEND_INVALID_DEST;
    }

    /* Handle kobject ports */
    if (dest_p->kobject_kind != PORT_KOBJECT_NONE) {
        if (dest_p->kobject_kind == PORT_KOBJECT_LAUNCHD) {
            uint8_t rep_buf[512];
            uint32_t rep_size = 0;
            int reply_enqueued = 0; /* M4: only fire the hook if a reply was actually enqueued */
            if (launchd_process_msg(hdr, send_size, rep_buf, sizeof(rep_buf), &rep_size)) {
                if (reply_name != MACH_PORT_NULL && reply_e && reply_e->port) {
                    struct ipc_kmsg *rep_kmsg = kmsg_alloc(rep_size);
                    if (rep_kmsg) {
                        for (uint32_t i = 0; i < rep_size; i++) rep_kmsg->data[i] = rep_buf[i];
                        rep_kmsg->size = rep_size;
                        rep_kmsg->reply_port = NULL;
                        /* M7: preserve the reply header's remote disposition
                         * (the receive-side rebuild re-derives msgh_bits from
                         * this), instead of forcing PORT_NULL. */
                        rep_kmsg->reply_type = (uint8_t)MACH_MSGH_BITS_REMOTE(((const mach_msg_header_t *)rep_buf)->msgh_bits);
                        rep_kmsg->is_complex = (((const mach_msg_header_t *)rep_buf)->msgh_bits & MACH_MSGH_BITS_COMPLEX) ? 1 : 0;
                        rep_kmsg->is_launchd = 1;

                        struct ipc_port *rp = reply_e->port;
                        if (!rp->is_special_reply_port) rp->is_reply_port = 1;
                        rp->seqno++;
                        rep_kmsg->seqno = rp->seqno;
                        if (rp->queue_tail) {
                            rp->queue_tail->next = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        } else {
                            rp->queue_head = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        }
                        rp->msgcount++;
                        reply_enqueued = 1;
                    }
                }
            }
            if (reply_e && (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE)) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
            }
            if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
                dest_e->urefs--;
                if (dest_e->urefs == 0) {
                    dest_e->type &= ~MACH_PORT_TYPE_SEND;
                    if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
                }
            } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
            ipc_notify_all();
            /* M4: never fire the hook with rep_size == 0 (no reply enqueued). */
            if (reply_enqueued && rep_size > 0) ipc_enqueue_hook_deferred(reply_name, rep_size);
            return MACH_MSG_SUCCESS;
        }

        if (dest_p->kobject_kind == PORT_KOBJECT_BKS_DISPLAY) {
            uint8_t rep_buf[256];
            uint32_t rep_size = 0;
            int reply_enqueued = 0; /* M4: only fire the hook if a reply was actually enqueued */
            if (bks_display_process_msg(hdr, send_size, rep_buf, sizeof(rep_buf), &rep_size)) {
                if (reply_name != MACH_PORT_NULL && reply_e && reply_e->port) {
                    struct ipc_kmsg *rep_kmsg = kmsg_alloc(rep_size);
                    if (rep_kmsg) {
                        for (uint32_t i = 0; i < rep_size; i++) rep_kmsg->data[i] = rep_buf[i];
                        rep_kmsg->size = rep_size;
                        rep_kmsg->reply_port = NULL;
                        /* M7: preserve the reply header's remote disposition
                         * (the receive-side rebuild re-derives msgh_bits from
                         * this), instead of forcing PORT_NULL. */
                        rep_kmsg->reply_type = (uint8_t)MACH_MSGH_BITS_REMOTE(((const mach_msg_header_t *)rep_buf)->msgh_bits);
                        rep_kmsg->is_complex = 0;
                        rep_kmsg->is_launchd = 0;

                        struct ipc_port *rp = reply_e->port;
                        if (!rp->is_special_reply_port) rp->is_reply_port = 1;
                        rp->seqno++;
                        rep_kmsg->seqno = rp->seqno;
                        if (rp->queue_tail) {
                            rp->queue_tail->next = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        } else {
                            rp->queue_head = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        }
                        rp->msgcount++;
                        reply_enqueued = 1;
                    }
                }
            }
            if (reply_e && (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE)) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
            }
            if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
                dest_e->urefs--;
                if (dest_e->urefs == 0) {
                    dest_e->type &= ~MACH_PORT_TYPE_SEND;
                    if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
                }
            } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
            ipc_notify_all();
            /* M4: never fire the hook with rep_size == 0 (no reply enqueued). */
            if (reply_enqueued && rep_size > 0) ipc_enqueue_hook_deferred(reply_name, rep_size);
            return MACH_MSG_SUCCESS;
        }

        if (dest_p->kobject_kind == PORT_KOBJECT_SVCHOST ||
            dest_p->kobject_kind == PORT_KOBJECT_IOKIT ||
            dest_p->kobject_kind == PORT_KOBJECT_HIDSYS) {
            static uint8_t rep_buf[8192]; /* max(SVCHOST_MAX_REPLY, IOKIT_MAX_REPLY); used under ipc_lock */
            uint32_t rep_size = 0;
            int reply_enqueued = 0; /* M4: only fire the hook if a reply was actually enqueued */
            mach_msg_header_t *mh = (mach_msg_header_t *)hdr;
            mach_port_name_t saved_remote = mh->msgh_remote_port;
            /* libxpc connection ports (iOS 17+): the first message to a mach service is "w00t"
             * (0x77303074) carrying MOVE_RECEIVE of the client's connection port + MOVE_SEND of its
             * reply endpoint. All later messages go to the connection port, so it becomes a SVCHOST
             * kobject aliasing the service port (dispatch below rewrites the remote name). */
            if (dest_p->kobject_kind == PORT_KOBJECT_SVCHOST && hdr->msgh_id == 0x77303074 &&
                (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) && send_size >= 52 &&
                *(const uint32_t *)((const uint8_t *)hdr + 24) >= 1 &&
                ((const uint8_t *)hdr)[24 + 4 + 10] == MACH_MSG_TYPE_MOVE_RECEIVE) {
                mach_port_name_t cname = *(const uint32_t *)((const uint8_t *)hdr + 28);
                struct ipc_entry *ce = ipc_entry_lookup(cname);
                if (ce && ce->port && (ce->type & MACH_PORT_TYPE_RECEIVE)) {
                    ce->port->kobject_kind = PORT_KOBJECT_SVCHOST;
                    ce->port->svc_alias = dest_p->svc_alias ? dest_p->svc_alias : saved_remote;
                    if (g_ipc_trace) {
                        ipc_print_str("[ipc] w00t: connection port 0x"); ipc_print_hex(cname);
                        ipc_print_str(" -> service 0x"); ipc_print_hex(ce->port->svc_alias); ipc_print_str("\n");
                    }
                    if (g_ipc_trace) { ipc_print_str("[ipc] w00t: ndesc="); ipc_print_uint(*(const uint32_t *)((const uint8_t *)hdr + 24));
                        ipc_print_str(" size="); ipc_print_uint(send_size); ipc_print_str(" local=0x"); ipc_print_hex(hdr->msgh_local_port); ipc_print_str("\n"); }
                    if (*(const uint32_t *)((const uint8_t *)hdr + 24) >= 2 && send_size >= 52) {
                        extern void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint);
                        mach_port_name_t ep = *(const uint32_t *)((const uint8_t *)hdr + 40);
                        if (g_ipc_trace) { ipc_print_str("[ipc] w00t: endpoint 0x"); ipc_print_hex(ep); ipc_print_str("\n"); }
                        fakeboard_note_w00t(ce->port->svc_alias, ep);
                    }
                }
            }
            if (dest_p->svc_alias) mh->msgh_remote_port = dest_p->svc_alias;
            /* w00t carries no XPC payload and gets no reply on XNU: consume it here. */
            int is_w00t = (dest_p->kobject_kind == PORT_KOBJECT_SVCHOST && hdr->msgh_id == 0x77303074);
            int handled = is_w00t ? 0
                : (dest_p->kobject_kind == PORT_KOBJECT_IOKIT)
                    ? iokit_process_msg(hdr, send_size, rep_buf, sizeof(rep_buf), &rep_size)
                : (dest_p->kobject_kind == PORT_KOBJECT_HIDSYS)
                    ? hidsys_process_msg(hdr, send_size, rep_buf, sizeof(rep_buf), &rep_size)
                    : svchost_process_msg(hdr, send_size, rep_buf, 1024, &rep_size);
            mh->msgh_remote_port = saved_remote;
            if (dest_p->kobject_kind == PORT_KOBJECT_SVCHOST) g_svc_pump_pending = 1;
            if (handled && rep_size > 0) {
                if (reply_name != MACH_PORT_NULL && reply_e && reply_e->port) {
                    struct ipc_kmsg *rep_kmsg = kmsg_alloc(rep_size);
                    if (rep_kmsg) {
                        for (uint32_t i = 0; i < rep_size; i++) rep_kmsg->data[i] = rep_buf[i];
                        rep_kmsg->size = rep_size;
                        rep_kmsg->reply_port = NULL;
                        /* M7: preserve the reply header's remote disposition
                         * (the receive-side rebuild re-derives msgh_bits from
                         * this), instead of forcing PORT_NULL. */
                        rep_kmsg->reply_type = (uint8_t)MACH_MSGH_BITS_REMOTE(((const mach_msg_header_t *)rep_buf)->msgh_bits);
                        rep_kmsg->is_complex = (((const mach_msg_header_t *)rep_buf)->msgh_bits & MACH_MSGH_BITS_COMPLEX) ? 1 : 0;
                        rep_kmsg->is_launchd = 1;

                        struct ipc_port *rp = reply_e->port;
                        if (!rp->is_special_reply_port) rp->is_reply_port = 1;
                        rp->seqno++;
                        rep_kmsg->seqno = rp->seqno;
                        if (rp->queue_tail) {
                            rp->queue_tail->next = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        } else {
                            rp->queue_head = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        }
                        rp->msgcount++;
                        reply_enqueued = 1;
                    }
                }
            }
            if (reply_e && (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE)) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
            }
            if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
                dest_e->urefs--;
                if (dest_e->urefs == 0) {
                    dest_e->type &= ~MACH_PORT_TYPE_SEND;
                    if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
                }
            } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
            ipc_notify_all();
            /* M4: never fire the hook with rep_size == 0 (no reply enqueued). */
            if (reply_enqueued && rep_size > 0) ipc_enqueue_hook_deferred(reply_name, rep_size);
            return MACH_MSG_SUCCESS;
        }

        if (dest_p->kobject_kind == PORT_KOBJECT_NOTIFYD) {
            /* notifyd (S2). The request is copied to a
             * stack buffer and a 52-byte audit trailer is appended
             * (is_launchd 0, so the trailer carries the server's
             * own pid, matching checkin's server_pid). The copy
             * is needed because notifyd reads the audit token from
             * the trailer, and kobject dispatch runs at send time,
             * before any trailer exists. The largest legitimate
             * request (regenerate with a 512-byte name and a
             * 1024-byte path) is about 1.7 KB, so a 2048-byte
             * buffer covers it; a bigger message falls back to
             * the untrailered call and gets MIG_BAD_ARGUMENTS.
             * Request descriptors still hold names here (the
             * branch runs before the send-path translation), so
             * notifyd validates ports by name. */
            uint8_t rep_buf[NOTIFYD_MAX_REPLY];
            uint8_t req_copy[2048];
            uint32_t rep_size = 0;
            const mach_msg_header_t *req_hdr = hdr;
            uint32_t req_size = send_size;
            if (hdr->msgh_size + 52 <= sizeof(req_copy)) {
                uint32_t toff = (hdr->msgh_size + 3u) & ~3u;
                uint32_t i;
                for (i = 0; i < hdr->msgh_size; i++)
                    req_copy[i] = ((const uint8_t *)hdr)[i];
                ipc_fill_trailer(req_copy + toff, 52, dest_p->seqno,
                                 dest_p, 0);
                req_hdr = (const mach_msg_header_t *)req_copy;
                req_size = toff + 52;
            }
            if (notifyd_process_msg(req_hdr, req_size, rep_buf,
                                    sizeof(rep_buf), &rep_size)) {
                if (rep_size > 0 && reply_name != MACH_PORT_NULL &&
                    reply_e && reply_e->port) {
                    struct ipc_kmsg *rep_kmsg = kmsg_alloc(rep_size);
                    if (rep_kmsg) {
                        uint32_t i;
                        for (i = 0; i < rep_size; i++)
                            rep_kmsg->data[i] = rep_buf[i];
                        rep_kmsg->size = rep_size;
                        rep_kmsg->reply_port = NULL;
                        rep_kmsg->reply_type = 0;
                        rep_kmsg->is_complex = (((const mach_msg_header_t *)rep_buf)->msgh_bits & MACH_MSGH_BITS_COMPLEX) ? 1 : 0;
                        /* is_launchd 0: the reply trailer carries
                         * the server's own pid, not forged pid 1. */
                        rep_kmsg->is_launchd = 0;
                        if (rep_kmsg->is_complex) {
                            /* Translate port descriptors in the
                             * reply (name -> carried port pointer,
                             * with the send-path disposition
                             * semantics), so the receive path can
                             * copy the right out to the client.
                             * Only move_receive occurs in notifyd
                             * replies (generate_common_port and
                             * register_mach_port_3). */
                            mach_msg_body_t *rbody = (mach_msg_body_t *)(rep_kmsg->data + sizeof(mach_msg_header_t));
                            uint32_t rcount = rbody->msgh_descriptor_count;
                            uint8_t *rdesc = (uint8_t *)(rbody + 1);
                            /* The count comes from our own
                             * reply, so this guard never fires on good
                             * traffic; it keeps a bad count from walking
                             * rdesc off the end of the reply buffer and
                             * writing a port pointer past it. */
                            const uint8_t *rmsg_end = rep_kmsg->data + rep_size;
                            uint32_t ri;
                            for (ri = 0; ri < rcount; ri++) {
                                if (!ipc_desc_fits(rdesc, rmsg_end, 12)) break;
                                uint8_t rtype = rdesc[11];
                                if (rtype == MACH_MSG_PORT_DESCRIPTOR) {
                                    mach_port_name_t rname = *(mach_port_name_t *)rdesc;
                                    uint8_t rdisp = rdesc[10];
                                    struct ipc_entry *re = ipc_entry_lookup(rname);
                                    struct ipc_port *rport = re ? re->port : NULL;
                                    if (rdisp == MACH_MSG_TYPE_MOVE_RECEIVE) {
                                        if (re && (re->type & MACH_PORT_TYPE_RECEIVE)) {
                                            re->type &= ~MACH_PORT_TYPE_RECEIVE;
                                            if (rport) rport->has_receive_right = 0;
                                            if (re->type == MACH_PORT_TYPE_NONE) ipc_entry_free(re);
                                        }
                                    } else if (rdisp == MACH_MSG_TYPE_MAKE_SEND) {
                                        if (rport) {
                                            rport->srights++;
                                            rport->mscount++;
                                        }
                                    } else if (rdisp == MACH_MSG_TYPE_COPY_SEND) {
                                        if (rport) rport->srights++;
                                    } else if (rdisp == MACH_MSG_TYPE_MOVE_SEND) {
                                        if (re && (re->type & MACH_PORT_TYPE_SEND)) {
                                            re->urefs--;
                                            if (re->urefs == 0) {
                                                re->type &= ~MACH_PORT_TYPE_SEND;
                                                if (re->type == MACH_PORT_TYPE_NONE) ipc_entry_free(re);
                                            }
                                        }
                                    }
                                    *(struct ipc_port **)rdesc = rport;
                                    rdesc += 12;
                                } else if (rtype == MACH_MSG_OOL_DESCRIPTOR || rtype == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) {
                                    rdesc += 16;
                                } else if (rtype == MACH_MSG_OOL_PORTS_DESCRIPTOR) {
                                    rdesc += 24;
                                } else {
                                    rdesc += 12;
                                }
                            }
                        }
                        struct ipc_port *rp = reply_e->port;
                        if (!rp->is_special_reply_port) rp->is_reply_port = 1;
                        rp->seqno++;
                        rep_kmsg->seqno = rp->seqno;
                        if (rp->queue_tail) {
                            rp->queue_tail->next = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        } else {
                            rp->queue_head = rep_kmsg;
                            rp->queue_tail = rep_kmsg;
                        }
                        rp->msgcount++;
                    }
                }
            }
            if (reply_e && (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE)) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
            }
            if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
                dest_e->urefs--;
                if (dest_e->urefs == 0) {
                    dest_e->type &= ~MACH_PORT_TYPE_SEND;
                    if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
                }
            } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
            ipc_notify_all();
            if (g_enqueue_hook && reply_name != MACH_PORT_NULL && rep_size > 0) {
                g_enqueue_hook(reply_name, rep_size);
            }
            return MACH_MSG_SUCCESS;
        }

        ipc_log_kobject_id(hdr->msgh_id);
        if (reply_name != MACH_PORT_NULL && reply_e && reply_e->port) {
            struct ipc_kmsg *rep_kmsg = kmsg_alloc(sizeof(mig_reply_error_t));
            if (rep_kmsg) {
                mig_reply_error_t *rep = (mig_reply_error_t *)rep_kmsg->data;
                rep->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, 0);
                rep->Head.msgh_size = sizeof(mig_reply_error_t);
                rep->Head.msgh_remote_port = reply_name;
                rep->Head.msgh_local_port = MACH_PORT_NULL;
                rep->Head.msgh_voucher_port = MACH_PORT_NULL;
                rep->Head.msgh_id = hdr->msgh_id + 100;
                rep->NDR.mig_vers = 0;
                rep->NDR.if_vers = 0;
                rep->NDR.reserved1 = 0;
                rep->NDR.mig_encoding = 0;
                rep->NDR.int_rep = 1;
                rep->NDR.char_rep = 0;
                rep->NDR.float_rep = 0;
                rep->NDR.reserved2 = 0;
                rep->RetCode = -301; /* MIG_BAD_ID */
                /* M7: the reply header's remote disposition is PORT_SEND_ONCE;
                 * carry it so the receive-side rebuild preserves it. */
                rep_kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND_ONCE;

                struct ipc_port *rp = reply_e->port;
                if (rp->queue_tail) {
                    rp->queue_tail->next = rep_kmsg;
                    rp->queue_tail = rep_kmsg;
                } else {
                    rp->queue_head = rep_kmsg;
                    rp->queue_tail = rep_kmsg;
                }
                rp->msgcount++;
                /* MIG_BAD_ID reply hook: the reply port has a new message. */
                ipc_enqueue_hook_deferred(reply_name, sizeof(mig_reply_error_t));
            }
        }
        /* Consume destination right if moved */
        if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
            dest_e->urefs--;
            if (dest_e->urefs == 0) {
                dest_e->type &= ~MACH_PORT_TYPE_SEND;
                if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
            }
        } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
            dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
            if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
        }
        ipc_notify_all();
        return MACH_MSG_SUCCESS;
    }

    /* Validate the descriptor table (count against the
     * message size, OOL ranges against the mappings) before copying
     * the message or consuming any rights. A corrupt count used to
     * walk desc_ptr off the end of the message; an inflated OOL size
     * used to copy past the source mapping and deallocate
     * neighbouring live pages. */
    if (!ipc_msg_desc_valid(hdr, send_size)) {
        return MACH_SEND_MSG_TOO_SMALL;
    }

    /* Allocate kmsg */
    struct ipc_kmsg *kmsg = kmsg_alloc(send_size);
    if (!kmsg) return MACH_SEND_NO_BUFFER;

    uint8_t *src_bytes = (uint8_t *)hdr;
    for (uint32_t i = 0; i < send_size; i++) {
        kmsg->data[i] = src_bytes[i];
    }
    mach_msg_header_t *khdr = (mach_msg_header_t *)kmsg->data;
    khdr->msgh_size = send_size;

    /* In-flight reply port */
    if (reply_name != MACH_PORT_NULL && reply_e) {
        kmsg->reply_port = reply_e->port;
        /* M6: record the reply port's generation so the receive side (and
         * the destroy drain) only notify the same live port. */
        kmsg->reply_gen = reply_e->port ? reply_e->port->gen : 0;
        if (reply_e->port && !reply_e->port->is_special_reply_port) reply_e->port->is_reply_port = 1;
        if (local_disp == MACH_MSG_TYPE_MAKE_SEND_ONCE) {
            kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND_ONCE;
            reply_e->port->sorights++;
        } else if (local_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
            kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND_ONCE;
            reply_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
            if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
        } else if (local_disp == MACH_MSG_TYPE_MAKE_SEND) {
            kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND;
            reply_e->port->srights++;
            reply_e->port->mscount++;
        } else if (local_disp == MACH_MSG_TYPE_COPY_SEND) {
            kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND;
            reply_e->port->srights++;
        } else if (local_disp == MACH_MSG_TYPE_MOVE_SEND) {
            kmsg->reply_type = MACH_MSG_TYPE_PORT_SEND;
            reply_e->urefs--;
            if (reply_e->urefs == 0) {
                reply_e->type &= ~MACH_PORT_TYPE_SEND;
                if (reply_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(reply_e);
            }
        }
    } else {
        kmsg->reply_port = NULL;
        kmsg->reply_type = 0;
    }

    /* Consume destination right if moved */
    if (remote_disp == MACH_MSG_TYPE_MOVE_SEND) {
        dest_e->urefs--;
        if (dest_e->urefs == 0) {
            dest_e->type &= ~MACH_PORT_TYPE_SEND;
            dest_p->srights--;
            if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
        }
    } else if (remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
        dest_e->type &= ~MACH_PORT_TYPE_SEND_ONCE;
        dest_p->sorights--;
        if (dest_e->type == MACH_PORT_TYPE_NONE) ipc_entry_free(dest_e);
    }

    /* Complex messages & descriptors */
    if (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        kmsg->is_complex = 1;
        mach_msg_body_t *body = (mach_msg_body_t *)(kmsg->data + sizeof(mach_msg_header_t));
        uint32_t count = body->msgh_descriptor_count;
        uint8_t *desc_ptr = (uint8_t *)(body + 1);
        /* Defense in depth. The pre-pass above already
         * validated this table, so this guard never fires on good
         * traffic; it keeps the walk from running off the end of
         * the message if the table ever changes under it. */
        const uint8_t *msg_end = kmsg->data + send_size;

        for (uint32_t i = 0; i < count; i++) {
            if (!ipc_desc_fits(desc_ptr, msg_end, 12)) {
                kmsg_free(kmsg);
                return MACH_SEND_MSG_TOO_SMALL;
            }
            uint8_t type = desc_ptr[11];
            if (type == MACH_MSG_PORT_DESCRIPTOR || type == MACH_MSG_GUARDED_PORT_DESCRIPTOR) {
                mach_port_t port_name = *(mach_port_t *)desc_ptr;
                uint8_t disp = desc_ptr[10];
                struct ipc_entry *pe = ipc_entry_lookup(port_name);
                struct ipc_port *carried_port = pe ? pe->port : NULL;

                if (disp == MACH_MSG_TYPE_MOVE_RECEIVE) {
                    if (pe && (pe->type & MACH_PORT_TYPE_RECEIVE)) {
                        pe->type &= ~MACH_PORT_TYPE_RECEIVE;
                        if (carried_port) carried_port->has_receive_right = 0;
                        if (pe->type == MACH_PORT_TYPE_NONE) ipc_entry_free(pe);
                    }
                } else if (disp == MACH_MSG_TYPE_MAKE_SEND) {
                    if (carried_port) {
                        carried_port->srights++;
                        carried_port->mscount++;
                    }
                } else if (disp == MACH_MSG_TYPE_COPY_SEND) {
                    if (carried_port) carried_port->srights++;
                } else if (disp == MACH_MSG_TYPE_MOVE_SEND) {
                    if (pe && (pe->type & MACH_PORT_TYPE_SEND)) {
                        pe->urefs--;
                        if (pe->urefs == 0) {
                            pe->type &= ~MACH_PORT_TYPE_SEND;
                            if (pe->type == MACH_PORT_TYPE_NONE) ipc_entry_free(pe);
                        }
                    }
                }
                /* Store (slot index, generation) in the descriptor (M6),
                 * not a raw pointer: the port may be destroyed while this
                 * message is queued. */
                ipc_desc_store_port(desc_ptr, carried_port);
                desc_ptr += 12;
            } else if (type == MACH_MSG_OOL_DESCRIPTOR || type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) {
                /* The size field lives at offset 12, so
                 * a 12-byte descriptor is not enough to read it. */
                if (!ipc_desc_fits(desc_ptr, msg_end, 16)) {
                    kmsg_free(kmsg);
                    return MACH_SEND_MSG_TOO_SMALL;
                }
                uint64_t addr;
                uint8_t deallocate = desc_ptr[8];
                uint32_t sz = *(uint32_t *)(desc_ptr + 12);
                /* Memcpy, not a direct 8-byte load: the
                 * OOL address sits at offset 28, 4-aligned. */
                memcpy(&addr, desc_ptr, sizeof addr);

                uint64_t in_flight_addr = 0;
                ipc_unlock();
                mach_vm_allocate(0x103, &in_flight_addr, sz, 1 /* ANYWHERE */);
                if (in_flight_addr && addr) {
                    uint8_t *dst = (uint8_t *)in_flight_addr;
                    uint8_t *src = (uint8_t *)addr;
                    for (uint32_t b = 0; b < sz; b++) dst[b] = src[b];
                }
                if (deallocate && addr) {
                    mach_vm_deallocate(0x103, addr, sz);
                }
                ipc_lock();
                /* Memcpy, not a direct 8-byte store:
                 * the OOL address sits at offset 28, 4-aligned. */
                memcpy(desc_ptr, &in_flight_addr, sizeof in_flight_addr);
                desc_ptr += 16;
            } else if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) {
                desc_ptr += 24;
            } else {
                desc_ptr += 12;
            }
        }
    }

    /* Enqueue to dest_p */
    if (dest_p->queue_tail) {
        dest_p->queue_tail->next = kmsg;
        dest_p->queue_tail = kmsg;
    } else {
        dest_p->queue_head = kmsg;
        dest_p->queue_tail = kmsg;
    }
    dest_p->msgcount++;
    dest_p->seqno++;
    kmsg->seqno = dest_p->seqno;

    if (g_ipc_trace) {
        ipc_print_str("[ipc] send id=");
        ipc_print_uint(hdr->msgh_id);
        ipc_print_str(" dest=0x");
        ipc_print_hex(dest_name);
        ipc_print_str(" size=");
        ipc_print_uint(send_size);
        ipc_print_str("\n");
    }

    ipc_notify_all();
    /* M7: deferred out of ipc_lock (the hook takes kq_lock then wq_lock). */
    ipc_enqueue_hook_deferred(dest_name, send_size);
    return MACH_MSG_SUCCESS;
}

uint32_t ipc_port_peek_size(mach_port_name_t name) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    uint32_t sz = 0;
    if (e && e->port && e->port->queue_head) {
        sz = e->port->queue_head->size;
    } else if (e && (e->type & MACH_PORT_TYPE_PORT_SET)) {
        /* Port set: a set has no queue of its own, so report the first
         * member port with a queued message. Without this, a knote attached
         * to the set never fires level-triggered for a message that was
         * already queued when the knote was registered (the same
         * defect as the notify path, at attach time). */
        for (int i = 1; i < MAX_ENTRIES; i++) {
            if (g_entries[i].in_use && (g_entries[i].type & MACH_PORT_TYPE_RECEIVE) &&
                g_entries[i].port && g_entries[i].port->pset_name == name &&
                g_entries[i].port->queue_head) {
                sz = g_entries[i].port->queue_head->size;
                break;
            }
        }
    }
    /* Raw unlock (no hook drain): kevent_apply_changes calls this while
     * holding kq_lock, and draining would take kq_lock again. */
    ipc_unlock_raw();
    return sz;
}

static int ipc_rcv_locked(void *msg, uint64_t option, uint32_t rcv_size, mach_port_name_t rcv_name, uint32_t timeout) {
    struct ipc_entry *rcv_entry = ipc_entry_lookup(rcv_name);
    if (!rcv_entry || !(rcv_entry->type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_PORT_SET))) {
        return MACH_RCV_INVALID_NAME;
    }

    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    uint64_t start_time = mach_absolute_time();
    uint64_t caller_deadline = 0;
    if (option & MACH_RCV_TIMEOUT) {
        uint64_t timeout_ns = (uint64_t)timeout * 1000000ULL;
        uint64_t timeout_ticks = (timeout_ns * tb.denom) / tb.numer;
        caller_deadline = start_time + timeout_ticks;
    }

    while (1) {
        struct ipc_entry *cur_entry = ipc_entry_lookup(rcv_name);
        if (!cur_entry || !(cur_entry->type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_PORT_SET))) {
            return MACH_RCV_PORT_DIED;
        }
        rcv_entry = cur_entry;

        uint64_t now = mach_absolute_time();
        ipc_check_timers_locked(rcv_entry, rcv_name, now);

        struct ipc_port *recv_port = NULL;
        struct ipc_kmsg *kmsg = NULL;
        mach_port_name_t member_name = 0;

        if (rcv_entry->type & MACH_PORT_TYPE_RECEIVE) {
            recv_port = rcv_entry->port;
            if (!recv_port || !recv_port->has_receive_right) {
                return MACH_RCV_PORT_DIED;
            }
            kmsg = recv_port->queue_head;
            member_name = rcv_name;
        } else {
            /* Port set */
            for (int i = 1; i < MAX_ENTRIES; i++) {
                if (g_entries[i].in_use && (g_entries[i].type & MACH_PORT_TYPE_RECEIVE) && g_entries[i].port) {
                    struct ipc_port *p = g_entries[i].port;
                    if (p->pset_name == rcv_name && p->queue_head) {
                        recv_port = p;
                        kmsg = p->queue_head;
                        member_name = (mach_port_name_t)((i << 8) | g_entries[i].generation);
                        break;
                    }
                }
            }
        }

        if (kmsg) {
            uint32_t trailer_size = ipc_get_trailer_size(option);
            /* The trailer is written at the 4-aligned
             * offset ((kmsg->size + 3) & ~3), so the buffer must hold
             * the aligned size plus the trailer, not the raw size
             * plus the trailer. With a non-4-aligned message and an
             * exactly-fitting buffer the old code wrote up to 3 bytes
             * past the receiver's buffer. */
            uint32_t needed_size = ((kmsg->size + 3) & ~3) + trailer_size;

            if (rcv_size < needed_size) {
                if (msg) ((mach_msg_header_t *)msg)->msgh_size = needed_size;
                if (option & MACH_RCV_LARGE) {
                    return MACH_RCV_TOO_LARGE;
                } else {
                    /* Dequeue and destroy message */
                    recv_port->queue_head = kmsg->next;
                    if (!recv_port->queue_head) recv_port->queue_tail = NULL;
                    recv_port->msgcount--;
                    kmsg_free(kmsg);
                    return MACH_RCV_TOO_LARGE;
                }
            }

            /* Dequeue message */
            recv_port->queue_head = kmsg->next;
            if (!recv_port->queue_head) recv_port->queue_tail = NULL;
            recv_port->msgcount--;

            /* Validate the queued message's descriptor
             * table before copying it out. Every message in the queue
             * passed the send-side pre-pass, and our own replies are
             * built by our code, so this never fires on good traffic;
             * it keeps a corrupt queued message from being delivered
             * with a half-rewritten descriptor table. */
            if (!ipc_msg_desc_valid((const mach_msg_header_t *)kmsg->data, kmsg->size)) {
                kmsg_free(kmsg);
                return MACH_RCV_INVALID_TYPE;
            }

            /* Copy body to msg */
            uint8_t *dst = (uint8_t *)msg;
            for (uint32_t i = 0; i < kmsg->size; i++) {
                dst[i] = kmsg->data[i];
            }

            mach_msg_header_t *out_hdr = (mach_msg_header_t *)msg;
            /* XNU: msgh_local_port = name of the receiving port. Kernel notifications
             * get NULL here. */
            if (out_hdr->msgh_id != MACH_NOTIFY_SEND_ONCE &&
                out_hdr->msgh_id != MACH_NOTIFY_DEAD_NAME &&
                out_hdr->msgh_id != MACH_NOTIFY_NO_SENDERS) {
                out_hdr->msgh_local_port = member_name;
            } else {
                out_hdr->msgh_local_port = MACH_PORT_NULL;
            }
            /* M6: only copy out a reply port that is still the same live
             * port recorded at send time; a destroyed port delivers NULL. */
            if (kmsg->reply_port && kmsg->reply_port->in_use && kmsg->reply_port->gen == kmsg->reply_gen) {
                out_hdr->msgh_remote_port = ipc_right_copyout(kmsg->reply_port, kmsg->reply_type);
            } else {
                out_hdr->msgh_remote_port = MACH_PORT_NULL;
            }
            out_hdr->msgh_voucher_port = MACH_PORT_NULL;
            out_hdr->msgh_bits = MACH_MSGH_BITS(kmsg->reply_type, 0);

            /* Rewrite descriptors for receiver */
            if (kmsg->is_complex) {
                out_hdr->msgh_bits |= MACH_MSGH_BITS_COMPLEX;
                mach_msg_body_t *body = (mach_msg_body_t *)(dst + sizeof(mach_msg_header_t));
                uint32_t count = body->msgh_descriptor_count;
                uint8_t *desc_ptr = (uint8_t *)(body + 1);
                /* Defense in depth. The pre-copy
                 * validation above already checked this table, so
                 * this guard never fires on good traffic. */
                const uint8_t *msg_end = dst + kmsg->size;

                for (uint32_t i = 0; i < count; i++) {
                    if (!ipc_desc_fits(desc_ptr, msg_end, 12)) {
                        kmsg_free(kmsg);
                        return MACH_RCV_INVALID_TYPE;
                    }
                    uint8_t type = desc_ptr[11];
                    if (type == MACH_MSG_PORT_DESCRIPTOR || type == MACH_MSG_GUARDED_PORT_DESCRIPTOR) {
                        /* M6: validate (slot index, generation); a destroyed
                         * port delivers MACH_PORT_NULL (all branches below
                         * already guard on cp). */
                        struct ipc_port *cp = ipc_desc_load_port(desc_ptr);
                        uint8_t disp = desc_ptr[10];
                        mach_port_name_t delivered_name = MACH_PORT_NULL;
                        uint8_t delivered_disp = MACH_MSG_TYPE_PORT_SEND;

                        if (disp == MACH_MSG_TYPE_MOVE_RECEIVE || disp == MACH_MSG_TYPE_PORT_RECEIVE) {
                            if (cp) {
                                cp->has_receive_right = 1;
                                delivered_name = ipc_right_copyout(cp, MACH_MSG_TYPE_PORT_RECEIVE);
                            }
                            delivered_disp = MACH_MSG_TYPE_PORT_RECEIVE;
                        } else if (disp == MACH_MSG_TYPE_MAKE_SEND_ONCE || disp == MACH_MSG_TYPE_MOVE_SEND_ONCE || disp == MACH_MSG_TYPE_PORT_SEND_ONCE) {
                            if (cp) delivered_name = ipc_right_copyout(cp, MACH_MSG_TYPE_PORT_SEND_ONCE);
                            delivered_disp = MACH_MSG_TYPE_PORT_SEND_ONCE;
                        } else {
                            if (cp) delivered_name = ipc_right_copyout(cp, MACH_MSG_TYPE_PORT_SEND);
                            delivered_disp = MACH_MSG_TYPE_PORT_SEND;
                        }
                        *(mach_port_t *)desc_ptr = delivered_name;
                        *(uint32_t *)(desc_ptr + 4) = 0;
                        desc_ptr[10] = delivered_disp;
                        desc_ptr += 12;
                    } else if (type == MACH_MSG_OOL_DESCRIPTOR || type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) {
                        /* The size field lives at offset
                         * 12, so a 12-byte descriptor is not enough
                         * to read it. */
                        if (!ipc_desc_fits(desc_ptr, msg_end, 16)) {
                            kmsg_free(kmsg);
                            return MACH_RCV_INVALID_TYPE;
                        }
                        uint64_t in_flight_addr;
                        uint32_t sz = *(uint32_t *)(desc_ptr + 12);
                        /* Memcpy, not a direct 8-byte
                         * load: the OOL address sits at offset 28,
                         * 4-aligned. */
                        memcpy(&in_flight_addr, desc_ptr, sizeof in_flight_addr);
                        uint64_t rcv_addr = 0;

                        ipc_unlock();
                        mach_vm_allocate(0x103, &rcv_addr, sz, 1 /* ANYWHERE */);
                        if (rcv_addr && in_flight_addr) {
                            uint8_t *d = (uint8_t *)rcv_addr;
                            uint8_t *s = (uint8_t *)in_flight_addr;
                            for (uint32_t b = 0; b < sz; b++) d[b] = s[b];
                        }
                        if (in_flight_addr) {
                            mach_vm_deallocate(0x103, in_flight_addr, sz);
                        }
                        ipc_lock();
                        /* Memcpy, not a direct 8-byte
                         * store: the OOL address sits at offset 28,
                         * 4-aligned. */
                        memcpy(desc_ptr, &rcv_addr, sizeof rcv_addr);
                        desc_ptr += 16;
                    } else if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) {
                        desc_ptr += 24;
                    } else {
                        desc_ptr += 12;
                    }
                }
            }

            /* Append trailer aligned to 4 */
            uint8_t *trailer_ptr = dst + ((kmsg->size + 3) & ~3);
            ipc_fill_trailer(trailer_ptr, trailer_size, kmsg->seqno, recv_port, kmsg->is_launchd);
            out_hdr->msgh_size = kmsg->size;

            if (g_ipc_trace) {
                ipc_print_str("[ipc] rcv ctx=0x");
                ipc_print_hex(recv_port ? recv_port->context : 0);
                ipc_print_str(" tsz=");
                ipc_print_uint(trailer_size);
                ipc_print_str(" id=");
                ipc_print_uint(out_hdr->msgh_id);
                ipc_print_str(" local=0x");
                ipc_print_hex(member_name);
                ipc_print_str(" size=");
                ipc_print_uint(kmsg->size);
                ipc_print_str("\n");
            }

            kmsg_free(kmsg);
            return MACH_MSG_SUCCESS;
        }

        /* Check if receive port died */
        struct ipc_entry *cur_e = ipc_entry_lookup(rcv_name);
        if (!cur_e || !(cur_e->type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_PORT_SET))) {
            return MACH_RCV_PORT_DIED;
        }
        rcv_entry = cur_e;

        if (rcv_entry->type & MACH_PORT_TYPE_RECEIVE) {
            if (!rcv_entry->port || !rcv_entry->port->has_receive_right) {
                return MACH_RCV_PORT_DIED;
            }
            /* XNU: a receive on a reply port with no outstanding send(-once) rights just blocks (the right
             * may still be in flight inside a queued request). Returning MACH_RCV_PORT_DIED here let
             * libdispatch's send_and_wait_for_reply return early while its stack dwr was still queued
             * (stale stack dmsg crash in libxpc's handler). */
        }

        /* Timeout check */
        if (option & MACH_RCV_TIMEOUT) {
            if (now >= caller_deadline) {
                return MACH_RCV_TIMED_OUT;
            }
        }

        /* Calculate futex sleep deadline */
        uint64_t earliest_timer = ipc_earliest_timer_locked(rcv_entry, rcv_name);
        uint64_t wait_until = 0;

        if (option & MACH_RCV_TIMEOUT) wait_until = caller_deadline;
        if (earliest_timer > 0 && (wait_until == 0 || earliest_timer < wait_until)) {
            wait_until = earliest_timer;
        }

        struct {
            long tv_sec;
            long tv_nsec;
        } ts;
        void *ts_ptr = NULL;

        if (wait_until > 0) {
            if (wait_until <= now) {
                /* Loop directly */
                continue;
            }
            uint64_t delta_ticks = wait_until - now;
            uint64_t delta_ns = (delta_ticks * tb.numer) / tb.denom;
            ts.tv_sec = (long)(delta_ns / 1000000000ULL);
            ts.tv_nsec = (long)(delta_ns % 1000000000ULL);
            ts_ptr = &ts;
        }

        uint32_t saved_gen = __atomic_load_n(&g_ipc_generation, __ATOMIC_SEQ_CST);
        if (g_ipc_trace) {
            uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
            ipc_print_str("[ipc] rcv WAIT port=0x");
            ipc_print_hex(rcv_name);
            ipc_print_str(" opt=0x");
            ipc_print_hex((uint32_t)option);
            ipc_print_str(" to=");
            ipc_print_uint(timeout);
            for (int f = 0; f < 8 && fp && ((uintptr_t)fp > 0x1000) && (((uintptr_t)fp & 7) == 0); f++) {
                ipc_print_str(" lr");
                ipc_print_uint(f);
                ipc_print_str("=0x");
                ipc_print_hex(fp[1] & 0xffffffffffffULL);
                fp = (uint64_t *)fp[0];
            }
            ipc_print_str("\n");
        }
        ipc_unlock();
        sys4(SYS_futex, (long)&g_ipc_generation, FUTEX_WAIT_PRIVATE, saved_gen, (long)ts_ptr);
        ipc_lock();

        /* Re-validate entry after waking up */
        rcv_entry = ipc_entry_lookup(rcv_name);
        if (!rcv_entry || !(rcv_entry->type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_PORT_SET))) {
            return MACH_RCV_PORT_DIED;
        }
    }
}

/* Server-initiated callouts of in-process services (fakeboard scene create, settings updates): queued by the
 * service under ipc_lock, delivered here with ordinary sends after the lock is dropped. */
static void ipc_svc_pump(void) {
    extern int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len, uint32_t *out_msgh_id);
    static uint8_t cbuf[8192];
    for (int n = 0; n < 16; n++) {
        uint32_t len = 0, id = 0;
        if (fakeboard_next_callout(cbuf, sizeof(cbuf), &len, &id) != 1) break;
        mach_msg_header_t *h = (mach_msg_header_t *)cbuf;
        if (g_ipc_trace) {
            ipc_print_str("[ipc] svc callout -> 0x"); ipc_print_hex(h->msgh_remote_port);
            ipc_print_str(" id=0x"); ipc_print_hex(id); ipc_print_str(" len="); ipc_print_uint(len); ipc_print_str("\n");
        }
        ipc_lock();
        int r = ipc_send_locked(h, len, MACH_SEND_MSG);
        ipc_unlock();
        if (r != MACH_MSG_SUCCESS && g_ipc_trace) {
            ipc_print_str("[ipc] svc callout send FAIL 0x"); ipc_print_hex((uint32_t)r); ipc_print_str("\n");
        }
    }
}

int ipc_msg(void *msg, uint64_t option, uint32_t send_size, uint32_t rcv_size,
            mach_port_name_t rcv_name, uint32_t timeout, mach_port_name_t notify,
            void *scatter, uint32_t scatter_size) {
    (void)notify; (void)scatter; (void)scatter_size;
    if (!g_ipc_inited) ipc_init();

    int send_ret = MACH_MSG_SUCCESS;
    if (option & MACH_SEND_MSG) {
        if (!msg) return MACH_SEND_INVALID_HEADER;
        mach_msg_header_t *shdr = (mach_msg_header_t *)msg;
        if (send_size == 0) send_size = shdr->msgh_size;
        uint32_t sid = shdr->msgh_id;
        uint32_t sdest = shdr->msgh_remote_port;
        uint32_t slocal = shdr->msgh_local_port;
        ipc_lock();
        send_ret = ipc_send_locked(shdr, send_size, option);
        if (g_ipc_trace && send_ret != MACH_MSG_SUCCESS) {
            uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
            uint64_t caller_lr = (fp && fp[0]) ? ((uint64_t *)fp[0])[1] : 0;
            ipc_print_str("[ipc] send id=");
            ipc_print_uint(sid);
            ipc_print_str(" dest=0x");
            ipc_print_hex(sdest);
            ipc_print_str(" local=0x");
            ipc_print_hex(slocal);
            ipc_print_str(" size=");
            ipc_print_uint(send_size);
            ipc_print_str(" kr=0x");
            ipc_print_hex((uint32_t)send_ret);
            ipc_print_str(" lr=0x");
            ipc_print_hex(caller_lr);
            ipc_print_str("\n");
        }
        ipc_unlock();
        g_svc_pump_pending = 0; ipc_svc_pump(); /* cheap when the fakeboard queue is empty */
        {   /* IOSWINE_SNAPSHOT: arm the window snapshot pump once libdispatch is live (first svc send). */
            static int snap_sends;
            if (snap_sends < 201) ++snap_sends;
            /* __libkernel_init_late is never called by the iOS 26 libSystem: run it here once
             * libdispatch is live (snapshot pump, carender, CADisplayLink hooks). The
             * CADisplayLink install is retried later in case QuartzCore was not up yet. */
            if (snap_sends == 50) {
#if defined(__APPLE__)
                /* One init_late mechanism: this path runs __libkernel_init_late here, which arms
                 * snapshot_init, carender_init (IOSWINE_CARENDER=1) and the
                 * CADisplayLink hooks. No separate carender_init() call. */
                extern void __libkernel_init_late(const void *arg);
                __libkernel_init_late(0);
#else
                /* Host tests have no init_late call, so arm carender here
                 * alongside snapshot_init (same once-only send-50 site).
                 * carender_init has its own once-guard; the two paths
                 * never both run. */
                extern void snapshot_init(void); snapshot_init();
                extern void carender_init(void); carender_init();
#endif
            }
#if defined(__APPLE__)
            /* TextInputUI loads lazily (first keyboard focus): keep retrying the TUIKB hooks */
            static unsigned tuikb_sends;
            if (++tuikb_sends > 200 && tuikb_sends % 50 == 0) {
                extern int tuikb_lock_install_hooks(void);
                extern int tuikb_validate_install_hooks(void);
                tuikb_lock_install_hooks();
                tuikb_validate_install_hooks();
            }
#endif
            if (snap_sends == 200) {
#if defined(__APPLE__)
                extern int cadisplay_link_install_hooks(void);
                cadisplay_link_install_hooks();
                extern int tuikb_lock_install_hooks(void);
                tuikb_lock_install_hooks();
                extern int mainscreen_install_hooks(void);
                mainscreen_install_hooks();
#else           /* host tests link ipc.c without cadisplay.c */
                extern int cadisplay_link_install_hooks(void) __attribute__((weak));
                if (cadisplay_link_install_hooks) cadisplay_link_install_hooks();
                extern int tuikb_lock_install_hooks(void) __attribute__((weak));
                if (tuikb_lock_install_hooks) tuikb_lock_install_hooks();
                extern int mainscreen_install_hooks(void) __attribute__((weak));
                if (mainscreen_install_hooks) mainscreen_install_hooks();
#endif
            }
        }
        if (g_ipc_trace && send_ret == MACH_MSG_SUCCESS) {
            const mach_msg_header_t *sh = (const mach_msg_header_t *)msg;
            ipc_print_str("[ipc] send ok remote=0x"); ipc_print_hex(sh->msgh_remote_port);
            ipc_print_str(" local=0x"); ipc_print_hex(sh->msgh_local_port);
            ipc_print_str(" id=0x"); ipc_print_hex((uint32_t)sh->msgh_id);
            ipc_print_str(" size="); ipc_print_uint(sh->msgh_size);
            ipc_print_str("\n");
        }
        if (send_ret != MACH_MSG_SUCCESS) {
            if (g_ipc_trace) {
                uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
                ipc_print_str("[ipc] send FAIL ret=0x"); ipc_print_hex((uint32_t)send_ret);
                ipc_print_str(" remote=0x"); ipc_print_hex(((mach_msg_header_t *)msg)->msgh_remote_port);
                ipc_print_str(" id="); ipc_print_uint((uint32_t)((mach_msg_header_t *)msg)->msgh_id);
                for (int f = 0; f < 6 && fp && ((uintptr_t)fp > 0x1000) && (((uintptr_t)fp & 7) == 0); f++) {
                    ipc_print_str(" lr=0x"); ipc_print_hex(fp[1] & 0xffffffffffffULL); fp = (uint64_t *)fp[0];
                }
                ipc_print_str("\n");
            }
            return send_ret;
        }
    }

    if (option & MACH_RCV_MSG) {
        if (!msg) return MACH_RCV_HEADER_ERROR;
        void *rcv_buf = scatter ? scatter : msg;
        uint32_t eff_rcv_size = (scatter && scatter_size) ? scatter_size : rcv_size;
        if (rcv_name == MACH_PORT_NULL) rcv_name = ((mach_msg_header_t *)msg)->msgh_local_port;
        if (eff_rcv_size == 0) eff_rcv_size = ((mach_msg_header_t *)msg)->msgh_size;
        ipc_lock();
        int rcv_ret = ipc_rcv_locked(rcv_buf, option, eff_rcv_size, rcv_name, timeout);
        ipc_unlock();
        if (g_ipc_trace && rcv_ret != MACH_MSG_SUCCESS) {
            uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
            ipc_print_str("[ipc] rcv FAIL port=0x"); ipc_print_hex(rcv_name);
            ipc_print_str(" ret=0x"); ipc_print_hex((uint32_t)rcv_ret);
            ipc_print_str(" opt=0x"); ipc_print_hex((uint32_t)option);
            for (int f = 0; f < 6 && fp && ((uintptr_t)fp > 0x1000) && (((uintptr_t)fp & 7) == 0); f++) {
                ipc_print_str(" lr=0x"); ipc_print_hex(fp[1] & 0xffffffffffffULL); fp = (uint64_t *)fp[0];
            }
            ipc_print_str("\n");
        }
        return rcv_ret;
    }

    return MACH_MSG_SUCCESS;
}

int ipc_msg2(void *data, uint64_t options, uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
             uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority, uint64_t timeout) {
    void *msg_buf = data;
    void *rcv_buf = NULL;
    uint32_t send_size = (uint32_t)(msgh_bits_and_send_size >> 32);
    uint32_t rcv_size = (uint32_t)rcv_size_and_priority;
    mach_port_name_t rcv_name = (mach_port_name_t)(desc_count_and_rcv_name >> 32);

    /* MACH_MSG2_VECTOR = 0x100000000ULL */
    if ((options & 0x100000000ULL) && data) {
        struct {
            uint64_t msgv_data;
            uint64_t msgv_rcv_addr;
            uint32_t msgv_send_size;
            uint32_t msgv_rcv_size;
        } *vec = data;
        msg_buf = (void *)vec->msgv_data;
        if (vec->msgv_rcv_addr) rcv_buf = (void *)vec->msgv_rcv_addr;
        send_size = vec->msgv_send_size;
        rcv_size = vec->msgv_rcv_size;
    }

    if (msg_buf && (options & MACH_SEND_MSG)) {
        mach_msg_header_t *h = (mach_msg_header_t *)msg_buf;
        if ((uint32_t)msgh_bits_and_send_size != 0)
            h->msgh_bits = (uint32_t)msgh_bits_and_send_size;
        if (send_size != 0)
            h->msgh_size = send_size;
        else
            send_size = h->msgh_size;
        if (msgh_remote_and_local_port != 0) {
            h->msgh_remote_port = (mach_port_t)msgh_remote_and_local_port;
            h->msgh_local_port = (mach_port_t)(msgh_remote_and_local_port >> 32);
        }
        if (msgh_voucher_and_id != 0) {
            h->msgh_voucher_port = (mach_port_name_t)msgh_voucher_and_id;
            h->msgh_id = (mach_msg_id_t)(msgh_voucher_and_id >> 32);
        }
    }
    if (rcv_name == 0 && msg_buf) {
        mach_msg_header_t *h = (mach_msg_header_t *)msg_buf;
        rcv_name = h->msgh_local_port;
    }

    return ipc_msg(msg_buf, options, send_size, rcv_size, rcv_name, (uint32_t)timeout, 0, rcv_buf, rcv_size);
}

void ipc_msg_destroy(void *msg) {
    if (!msg) return;
    mach_msg_header_t *h = (mach_msg_header_t *)msg;
    uint32_t remote_type = MACH_MSGH_BITS_REMOTE(h->msgh_bits);
    if (h->msgh_remote_port != MACH_PORT_NULL) {
        if (remote_type == MACH_MSG_TYPE_PORT_SEND || remote_type == MACH_MSG_TYPE_PORT_SEND_ONCE) {
            ipc_port_deallocate(ipc_task_self(), h->msgh_remote_port);
        }
    }
    if (h->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        mach_msg_body_t *body = (mach_msg_body_t *)((uint8_t *)msg + sizeof(mach_msg_header_t));
        uint32_t count = body->msgh_descriptor_count;
        uint8_t *desc_ptr = (uint8_t *)(body + 1);
        /* Bound the walk at the end of the message,
         * like the send and receive walks. A corrupt count used to
         * walk desc_ptr off the end of the buffer. */
        const uint8_t *msg_end = (const uint8_t *)msg + h->msgh_size;

        for (uint32_t i = 0; i < count; i++) {
            if (!ipc_desc_fits(desc_ptr, msg_end, 12)) break;
            uint8_t type = desc_ptr[11];
            if (type == MACH_MSG_PORT_DESCRIPTOR || type == MACH_MSG_GUARDED_PORT_DESCRIPTOR) {
                mach_port_name_t name = *(mach_port_name_t *)desc_ptr;
                uint8_t disp = desc_ptr[10];
                if (name != MACH_PORT_NULL) {
                    if (disp == MACH_MSG_TYPE_PORT_RECEIVE) {
                        ipc_port_mod_refs(ipc_task_self(), name, MACH_PORT_RIGHT_RECEIVE, -1);
                    } else if (disp == MACH_MSG_TYPE_PORT_SEND || disp == MACH_MSG_TYPE_PORT_SEND_ONCE) {
                        ipc_port_deallocate(ipc_task_self(), name);
                    }
                }
                desc_ptr += 12;
            } else if (type == MACH_MSG_OOL_DESCRIPTOR || type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR) {
                uint8_t deallocate;
                uint64_t addr;
                uint32_t sz;
                /* The size field lives at offset 12. */
                if (!ipc_desc_fits(desc_ptr, msg_end, 16)) break;
                deallocate = desc_ptr[8];
                /* Memcpy, not a direct 8-byte load: the
                 * OOL address sits at offset 28, 4-aligned. */
                memcpy(&addr, desc_ptr, sizeof addr);
                sz = *(uint32_t *)(desc_ptr + 12);
                /* XNU only deallocates an OOL region
                 * when the descriptor's deallocate flag is set. The
                 * old code deallocated unconditionally, so destroying
                 * a failed send unmapped a live buffer the sender
                 * still owned. And only deallocate a range that is
                 * still mapped: a second destroy of the same message
                 * used to unmap whatever had been mapped over the
                 * freed range since. A skipped deallocate leaks the
                 * sender's buffer, which is the safe side. */
                if (deallocate && addr && ipc_range_mapped(addr, sz)) {
                    mach_vm_deallocate(0x103, addr, sz);
                }
                desc_ptr += 16;
            } else if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) {
                desc_ptr += 24;
            } else {
                desc_ptr += 12;
            }
        }
    }
}

mach_port_name_t ipc_mem_entry_create(int fd, uint64_t size, uint64_t offset, int prot, uint64_t vaddr) {
    if (fd < 0 || size == 0) return MACH_PORT_NULL;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_port *p = ipc_port_new(PORT_KOBJECT_MEM_ENTRY);
    if (!p) {
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    p->has_receive_right = 0;
    p->mem_fd = fd;
    p->mem_size = size;
    p->mem_offset = offset;
    p->mem_prot = prot;
    p->mem_vaddr = vaddr;

    mach_port_name_t name = MACH_PORT_NULL;
    struct ipc_entry *e = ipc_entry_new(&name);
    if (!e) {
        p->mem_fd = -1;
        p->in_use = 0;
        ipc_unlock();
        return MACH_PORT_NULL;
    }
    e->port = p;
    e->type = MACH_PORT_TYPE_SEND;
    e->urefs = 1;
    p->srights = 1;
    ipc_unlock();
    return name;
}

int ipc_mem_entry_lookup(mach_port_name_t name, int *out_dup_fd, uint64_t *out_size, uint64_t *out_offset, int *out_prot) {
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    struct ipc_entry *e = ipc_entry_lookup(name);
    if (!e || !e->port || !e->port->in_use || e->port->kobject_kind != PORT_KOBJECT_MEM_ENTRY || e->port->mem_fd < 0) {
        ipc_unlock();
        return 0;
    }
    struct ipc_port *p = e->port;
    int dup_fd = (int)sys1(SYS_dup, p->mem_fd);
    if (dup_fd < 0) {
        ipc_unlock();
        return 0;
    }
    if (out_dup_fd) *out_dup_fd = dup_fd;
    else sys1(SYS_close, dup_fd);
    if (out_size) *out_size = p->mem_size;
    if (out_offset) *out_offset = p->mem_offset;
    if (out_prot) *out_prot = p->mem_prot;
    ipc_unlock();
    return 1;
}

int ipc_mem_entry_find_by_vaddr(uint64_t vaddr, uint64_t size, int *out_dup_fd, uint64_t *out_offset, int *out_prot) {
    if (vaddr == 0 || size == 0) return 0;
    if (!g_ipc_inited) ipc_init();
    ipc_lock();
    for (int i = 4; i < MAX_PORTS; i++) {
        struct ipc_port *p = &g_ports[i];
        if (!p->in_use || p->kobject_kind != PORT_KOBJECT_MEM_ENTRY || p->mem_fd < 0 || p->mem_vaddr == 0) {
            continue;
        }
        if (vaddr >= p->mem_vaddr && vaddr < p->mem_vaddr + p->mem_size &&
            size <= (p->mem_vaddr + p->mem_size - vaddr)) {
            int dup_fd = (int)sys1(SYS_dup, p->mem_fd);
            if (dup_fd < 0) {
                ipc_unlock();
                return 0;
            }
            if (out_dup_fd) *out_dup_fd = dup_fd;
            else sys1(SYS_close, dup_fd);
            if (out_offset) *out_offset = p->mem_offset + (vaddr - p->mem_vaddr);
            if (out_prot) *out_prot = p->mem_prot;
            ipc_unlock();
            return 1;
        }
    }
    ipc_unlock();
    return 0;
}
