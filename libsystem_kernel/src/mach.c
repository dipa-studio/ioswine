#include "linux.h"
#include "ipc.h"
#include <stdint.h>
#include <stddef.h>

extern uint32_t *iosw_ref_mach_task_self_;
extern uint32_t *iosw_ref_bootstrap_port;
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");

/* mach_task_self */
uint32_t lsk_mach_task_self(void) __asm__("_mach_task_self");
uint32_t lsk_mach_task_self(void) {
    return iosw_ref_mach_task_self_ ? *iosw_ref_mach_task_self_ : 0x103;
}

uint32_t lsk_task_self_trap(void) __asm__("_task_self_trap");
uint32_t lsk_task_self_trap(void) {
    return 0x103;
}

int lsk_mach_task_is_self(uint32_t task) __asm__("_mach_task_is_self");
int lsk_mach_task_is_self(uint32_t task) {
    uint32_t self = iosw_ref_mach_task_self_ ? *iosw_ref_mach_task_self_ : 0x103;
    return (task == self || task == 0x103);
}

int lsk_krpc_mach_task_is_self(uint32_t task, int *is_self) __asm__("__kernelrpc_mach_task_is_self");
int lsk_krpc_mach_task_is_self(uint32_t task, int *is_self) {
    if (is_self) *is_self = lsk_mach_task_is_self(task);
    return KERN_SUCCESS;
}

int lsk_task_restartable_ranges_register(uint32_t task, void *ranges, uint32_t count) __asm__("_task_restartable_ranges_register");
int lsk_task_restartable_ranges_register(uint32_t task, void *ranges, uint32_t count) {
    (void)task; (void)ranges; (void)count;
    return KERN_SUCCESS;
}

int lsk_task_restartable_ranges_synchronize(uint32_t task) __asm__("_task_restartable_ranges_synchronize");
int lsk_task_restartable_ranges_synchronize(uint32_t task) {
    (void)task;
    return KERN_SUCCESS;
}

uint32_t lsk_mach_thread_self(void) __asm__("_mach_thread_self");
uint32_t lsk_mach_thread_self(void) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t port = *(uint32_t *)(tsd + 24); /* TSD[3] */
        if (port != 0 && port != MACH_PORT_DEAD) return port;
    }
    return 0x303;
}

uint32_t lsk_thread_self_trap(void) __asm__("_thread_self_trap");
uint32_t lsk_thread_self_trap(void) {
    return lsk_mach_thread_self();
}

/* mach_host_self */
uint32_t lsk_mach_host_self(void) __asm__("_mach_host_self");
uint32_t lsk_mach_host_self(void) {
    return 0x104;
}

uint32_t lsk_host_self_trap(void) __asm__("_host_self_trap");
uint32_t lsk_host_self_trap(void) {
    return 0x104;
}

/* mach_reply_port */
uint32_t lsk_mach_reply_port(void) __asm__("_mach_reply_port");
uint32_t lsk_mach_reply_port(void) {
    return ipc_mach_reply_port();
}

/* thread_get_special_reply_port */
uint32_t lsk_thread_get_special_reply_port(void) __asm__("_thread_get_special_reply_port");
uint32_t lsk_thread_get_special_reply_port(void) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t *p = (uint32_t *)(tsd + 64); /* TSD[8] */
        /* M8: recheck the receive right (matching lsk_mig_get_reply_port):
         * a cached name whose port was destroyed must not be returned. */
        if (*p != 0 && *p != MACH_PORT_DEAD && ipc_port_has_receive_right(*p)) return *p;
        *p = ipc_mach_reply_port();
        return *p;
    }
    return ipc_mach_reply_port();
}

/* mig_get_special_reply_port */
uint32_t lsk_mig_get_special_reply_port(void) __asm__("_mig_get_special_reply_port");
uint32_t lsk_mig_get_special_reply_port(void) {
    return lsk_thread_get_special_reply_port();
}

/* mig_get_reply_port */
uint32_t lsk_mig_get_reply_port(void) __asm__("_mig_get_reply_port");
uint32_t lsk_mig_get_reply_port(void) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t *p = (uint32_t *)(tsd + 16); /* TSD[2] */
        if (*p != 0 && *p != MACH_PORT_DEAD && ipc_port_has_receive_right(*p)) return *p;
        *p = ipc_mach_reply_port();
        return *p;
    }
    return ipc_mach_reply_port();
}

/* mig_dealloc_reply_port */
void lsk_mig_dealloc_reply_port(uint32_t port) __asm__("_mig_dealloc_reply_port");
void lsk_mig_dealloc_reply_port(uint32_t port) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t *p = (uint32_t *)(tsd + 16); /* TSD[2] */
        if (*p != 0 && *p != MACH_PORT_DEAD) {
            ipc_port_destroy(lsk_mach_task_self(), *p);
            *p = 0;
        }
    }
    if (port != 0 && port != MACH_PORT_DEAD) {
        ipc_port_destroy(lsk_mach_task_self(), port);
    }
}

/* mig_put_reply_port */
void lsk_mig_put_reply_port(uint32_t port) __asm__("_mig_put_reply_port");
void lsk_mig_put_reply_port(uint32_t port) {
    (void)port;
}

/* mig_dealloc_special_reply_port */
void lsk_mig_dealloc_special_reply_port(uint32_t port) __asm__("_mig_dealloc_special_reply_port");
void lsk_mig_dealloc_special_reply_port(uint32_t port) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t *p = (uint32_t *)(tsd + 64); /* TSD[8] */
        if (*p != 0 && *p != MACH_PORT_DEAD) {
            ipc_port_destroy(lsk_mach_task_self(), *p);
            *p = 0;
        }
    }
    if (port != 0 && port != MACH_PORT_DEAD) {
        ipc_port_destroy(lsk_mach_task_self(), port);
    }
}

/* thread_destruct_special_reply_port */
int lsk_thread_destruct_special_reply_port(uint32_t port) __asm__("_thread_destruct_special_reply_port");
int lsk_thread_destruct_special_reply_port(uint32_t port) {
    lsk_mig_dealloc_special_reply_port(port);
    return KERN_SUCCESS;
}

/* host_info */
struct host_basic_info {
    int32_t  max_cpus;
    int32_t  avail_cpus;
    uint64_t memory_size;
    int32_t  cpu_type;
    int32_t  cpu_subtype;
    int32_t  cpu_threadtype;
    int32_t  physical_cpu;
    int32_t  physical_cpu_max;
    int32_t  logical_cpu;
    int32_t  logical_cpu_max;
    uint64_t max_mem;
};

struct host_priority_info {
    int32_t kernel_priority;
    int32_t system_priority;
    int32_t server_priority;
    int32_t user_priority;
    int32_t depress_priority;
    int32_t idle_priority;
    int32_t minimum_priority;
    int32_t maximum_priority;
};

int lsk_host_info(uint32_t host, int flavor, void *info, uint32_t *cnt) __asm__("_host_info");
int lsk_host_info(uint32_t host, int flavor, void *info, uint32_t *cnt) {
    (void)host;
    if (flavor == 1 && info && cnt && *cnt >= 12) { /* HOST_BASIC_INFO */
        struct host_basic_info *b = (struct host_basic_info *)info;
        b->max_cpus = 8;
        b->avail_cpus = 8;
        b->memory_size = 8ULL * 1024 * 1024 * 1024;
        b->cpu_type = 0x0100000c; /* CPU_TYPE_ARM64 */
        b->cpu_subtype = 2;       /* CPU_SUBTYPE_ARM64E */
        b->cpu_threadtype = 1;
        b->physical_cpu = 8;
        b->physical_cpu_max = 8;
        b->logical_cpu = 8;
        b->logical_cpu_max = 8;
        b->max_mem = 8ULL * 1024 * 1024 * 1024;
        *cnt = 12;
        return KERN_SUCCESS;
    }
    if (flavor == 5 && info && cnt && *cnt >= 8) { /* HOST_PRIORITY_INFO */
        struct host_priority_info *p = (struct host_priority_info *)info;
        p->kernel_priority = 80;
        p->system_priority = 80;
        p->server_priority = 32;
        p->user_priority = 31;
        p->depress_priority = 0;
        p->idle_priority = 0;
        p->minimum_priority = 0;
        p->maximum_priority = 63;
        *cnt = 8;
        return KERN_SUCCESS;
    }
    return KERN_SUCCESS;
}

/* host_page_size */
int lsk_host_page_size(uint32_t host, size_t *out_page_size) __asm__("_host_page_size");
int lsk_host_page_size(uint32_t host, size_t *out_page_size) {
    (void)host;
    if (out_page_size) *out_page_size = 16384;
    return KERN_SUCCESS;
}

int lsk_host_page_size_2(uint32_t host, size_t *out_page_size) __asm__("__host_page_size");
int lsk_host_page_size_2(uint32_t host, size_t *out_page_size) {
    return lsk_host_page_size(host, out_page_size);
}

/* host_check_multiuser_mode */
int lsk_host_check_multiuser_mode(uint32_t host, uint32_t *mode) __asm__("_host_check_multiuser_mode");
int lsk_host_check_multiuser_mode(uint32_t host, uint32_t *mode) {
    (void)host;
    if (mode) *mode = 0;
    return KERN_SUCCESS;
}

/* host_get_multiuser_config_flags */
int lsk_host_get_multiuser_config_flags(uint32_t host, uint32_t *flags) __asm__("_host_get_multiuser_config_flags");
int lsk_host_get_multiuser_config_flags(uint32_t host, uint32_t *flags) {
    (void)host;
    if (flags) *flags = 0;
    return KERN_SUCCESS;
}

/* task_info */
int lsk_task_info(uint32_t task, int flavor, void *info, uint32_t *cnt) __asm__("_task_info");
int lsk_task_info(uint32_t task, int flavor, void *info, uint32_t *cnt) {
    (void)task;
    if (flavor == 15 && info && cnt && *cnt >= 8) { /* TASK_AUDIT_TOKEN */
        uint32_t *tok = (uint32_t *)info;
        tok[0] = (uint32_t)sys0(SYS_getuid);
        tok[1] = (uint32_t)sys0(SYS_geteuid);
        tok[2] = (uint32_t)sys0(SYS_getegid);
        tok[3] = (uint32_t)sys0(SYS_getuid);
        tok[4] = (uint32_t)sys0(SYS_getgid);
        tok[5] = (uint32_t)sys0(SYS_getpid);
        tok[6] = 0;
        tok[7] = 1; /* pidversion / execcnt */
        *cnt = 8;
        return KERN_SUCCESS;
    }
    return KERN_SUCCESS;
}

/* task_get_special_port */
int lsk_task_get_special_port(uint32_t task, int which_port, uint32_t *port) __asm__("_task_get_special_port");
int lsk_task_get_special_port(uint32_t task, int which_port, uint32_t *port) {
    (void)task;
    if (!port) return KERN_INVALID_ARGUMENT;
    /* XNU task_special_ports.h: 1 KERNEL, 2 HOST, 3 NAME, 4 BOOTSTRAP, 5 INSPECT, 6 READ, 9 ACCESS,
     * 10 DEBUG_CONTROL, 11 RESOURCE_NOTIFY. An ordinary app only has the bootstrap port; every other
     * special port is MACH_PORT_NULL. Returning the bootstrap port for 10 (DEBUG_CONTROL) made libdispatch
     * (0x1be034958) open a receive channel on 0x203 -> EINVAL in EVFILT_MACHPORT -> libxpc crash. */
    switch (which_port) {
    case 1: case 3: *port = 0x103; break;      /* task self / name */
    case 4: *port = 0x203; break;              /* bootstrap */
    default: *port = 0; break;
    }
    return KERN_SUCCESS;
}

int lsk_task_set_special_port(uint32_t task, int which_port, uint32_t port) __asm__("_task_set_special_port");
int lsk_task_set_special_port(uint32_t task, int which_port, uint32_t port) {
    (void)task;
    (void)which_port;
    (void)port;
    return KERN_SUCCESS;
}

/* mach_port_* management */
int lsk_mach_port_construct(uint32_t task, void *options, uint64_t context, uint32_t *name) __asm__("_mach_port_construct");
int lsk_mach_port_construct(uint32_t task, void *options, uint64_t context, uint32_t *name) {
    return ipc_port_construct(task, (mach_port_options_t *)options, context, name);
}

int lsk_krpc_mach_port_construct_trap(uint32_t task, void *options, uint64_t context, uint32_t *name) __asm__("__kernelrpc_mach_port_construct_trap");
int lsk_krpc_mach_port_construct_trap(uint32_t task, void *options, uint64_t context, uint32_t *name) {
    return ipc_port_construct(task, (mach_port_options_t *)options, context, name);
}

int lsk_krpc_mach_port_construct(uint32_t task, void *options, uint64_t context, uint32_t *name) __asm__("__kernelrpc_mach_port_construct");
int lsk_krpc_mach_port_construct(uint32_t task, void *options, uint64_t context, uint32_t *name) {
    return ipc_port_construct(task, (mach_port_options_t *)options, context, name);
}

int lsk_mach_port_destruct(uint32_t task, uint32_t name, int delta, uint64_t guard) __asm__("_mach_port_destruct");
int lsk_mach_port_destruct(uint32_t task, uint32_t name, int delta, uint64_t guard) {
    return ipc_port_destruct(task, name, delta, guard);
}

int lsk_krpc_mach_port_destruct_trap(uint32_t task, uint32_t name, int delta, uint64_t guard) __asm__("__kernelrpc_mach_port_destruct_trap");
int lsk_krpc_mach_port_destruct_trap(uint32_t task, uint32_t name, int delta, uint64_t guard) {
    return ipc_port_destruct(task, name, delta, guard);
}

int lsk_krpc_mach_port_destruct(uint32_t task, uint32_t name, int delta, uint64_t guard) __asm__("__kernelrpc_mach_port_destruct");
int lsk_krpc_mach_port_destruct(uint32_t task, uint32_t name, int delta, uint64_t guard) {
    return ipc_port_destruct(task, name, delta, guard);
}

int lsk_mach_port_allocate(uint32_t task, int right, uint32_t *name) __asm__("_mach_port_allocate");
int lsk_mach_port_allocate(uint32_t task, int right, uint32_t *name) {
    return ipc_port_allocate(task, right, name);
}

int lsk_krpc_mach_port_allocate(uint32_t task, int right, uint32_t *name) __asm__("__kernelrpc_mach_port_allocate");
int lsk_krpc_mach_port_allocate(uint32_t task, int right, uint32_t *name) {
    return ipc_port_allocate(task, right, name);
}

int lsk_krpc_mach_port_allocate_trap(uint32_t task, int right, uint32_t *name) __asm__("__kernelrpc_mach_port_allocate_trap");
int lsk_krpc_mach_port_allocate_trap(uint32_t task, int right, uint32_t *name) {
    return ipc_port_allocate(task, right, name);
}

int lsk_mach_port_allocate_name(uint32_t task, int right, uint32_t name) __asm__("_mach_port_allocate_name");
int lsk_mach_port_allocate_name(uint32_t task, int right, uint32_t name) {
    return ipc_port_allocate_name(task, right, name);
}

int lsk_krpc_mach_port_allocate_name(uint32_t task, int right, uint32_t name) __asm__("__kernelrpc_mach_port_allocate_name");
int lsk_krpc_mach_port_allocate_name(uint32_t task, int right, uint32_t name) {
    return ipc_port_allocate_name(task, right, name);
}

int lsk_mach_port_allocate_full(uint32_t task, int right, uint32_t proto, void *qos, uint32_t *name) __asm__("_mach_port_allocate_full");
int lsk_mach_port_allocate_full(uint32_t task, int right, uint32_t proto, void *qos, uint32_t *name) {
    return ipc_port_allocate_full(task, right, proto, qos, name);
}

int lsk_krpc_mach_port_allocate_full(uint32_t task, int right, uint32_t proto, void *qos, uint32_t *name) __asm__("__kernelrpc_mach_port_allocate_full");
int lsk_krpc_mach_port_allocate_full(uint32_t task, int right, uint32_t proto, void *qos, uint32_t *name) {
    return ipc_port_allocate_full(task, right, proto, qos, name);
}

int lsk_mach_port_deallocate(uint32_t task, uint32_t name) __asm__("_mach_port_deallocate");
int lsk_mach_port_deallocate(uint32_t task, uint32_t name) {
    return ipc_port_deallocate(task, name);
}

int lsk_krpc_mach_port_deallocate(uint32_t task, uint32_t name) __asm__("__kernelrpc_mach_port_deallocate");
int lsk_krpc_mach_port_deallocate(uint32_t task, uint32_t name) {
    return ipc_port_deallocate(task, name);
}

int lsk_krpc_mach_port_deallocate_trap(uint32_t task, uint32_t name) __asm__("__kernelrpc_mach_port_deallocate_trap");
int lsk_krpc_mach_port_deallocate_trap(uint32_t task, uint32_t name) {
    return ipc_port_deallocate(task, name);
}

int lsk_mach_port_destroy(uint32_t task, uint32_t name) __asm__("_mach_port_destroy");
int lsk_mach_port_destroy(uint32_t task, uint32_t name) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        uint32_t *p = (uint32_t *)(tsd + 16); /* TSD[2] */
        if (*p == name) *p = 0;
        /* M8: TSD[8] is the thread special reply port cache; clear it too,
         * or a later get_special_reply_port returns the destroyed name. */
        uint32_t *p8 = (uint32_t *)(tsd + 64); /* TSD[8] */
        if (*p8 == name) *p8 = 0;
    }
    return ipc_port_destroy(task, name);
}

int lsk_krpc_mach_port_destroy(uint32_t task, uint32_t name) __asm__("__kernelrpc_mach_port_destroy");
int lsk_krpc_mach_port_destroy(uint32_t task, uint32_t name) {
    return lsk_mach_port_destroy(task, name);
}

int lsk_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t poly, int right) __asm__("_mach_port_insert_right");
int lsk_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t poly, int right) {
    return ipc_port_insert_right(task, name, poly, right);
}

int lsk_krpc_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t poly, int right) __asm__("__kernelrpc_mach_port_insert_right");
int lsk_krpc_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t poly, int right) {
    return ipc_port_insert_right(task, name, poly, right);
}

int lsk_krpc_mach_port_insert_right_trap(uint32_t task, uint32_t name, uint32_t poly, int right) __asm__("__kernelrpc_mach_port_insert_right_trap");
int lsk_krpc_mach_port_insert_right_trap(uint32_t task, uint32_t name, uint32_t poly, int right) {
    return ipc_port_insert_right(task, name, poly, right);
}

int lsk_mach_port_extract_right(uint32_t task, uint32_t name, int right, uint32_t *poly, int *acquired) __asm__("_mach_port_extract_right");
int lsk_mach_port_extract_right(uint32_t task, uint32_t name, int right, uint32_t *poly, int *acquired) {
    return ipc_port_extract_right(task, name, right, poly, acquired);
}

int lsk_krpc_mach_port_extract_right(uint32_t task, uint32_t name, int right, uint32_t *poly, int *acquired) __asm__("__kernelrpc_mach_port_extract_right");
int lsk_krpc_mach_port_extract_right(uint32_t task, uint32_t name, int right, uint32_t *poly, int *acquired) {
    return ipc_port_extract_right(task, name, right, poly, acquired);
}

int lsk_mach_port_mod_refs(uint32_t task, uint32_t name, int right, int delta) __asm__("_mach_port_mod_refs");
int lsk_mach_port_mod_refs(uint32_t task, uint32_t name, int right, int delta) {
    return ipc_port_mod_refs(task, name, right, delta);
}

int lsk_krpc_mach_port_mod_refs(uint32_t task, uint32_t name, int right, int delta) __asm__("__kernelrpc_mach_port_mod_refs");
int lsk_krpc_mach_port_mod_refs(uint32_t task, uint32_t name, int right, int delta) {
    return ipc_port_mod_refs(task, name, right, delta);
}

int lsk_krpc_mach_port_mod_refs_trap(uint32_t task, uint32_t name, int right, int delta) __asm__("__kernelrpc_mach_port_mod_refs_trap");
int lsk_krpc_mach_port_mod_refs_trap(uint32_t task, uint32_t name, int right, int delta) {
    return ipc_port_mod_refs(task, name, right, delta);
}

int lsk_mach_port_type(uint32_t task, uint32_t name, int *ptype) __asm__("_mach_port_type");
int lsk_mach_port_type(uint32_t task, uint32_t name, int *ptype) {
    return ipc_port_type(task, name, ptype);
}

int lsk_krpc_mach_port_type(uint32_t task, uint32_t name, int *ptype) __asm__("__kernelrpc_mach_port_type");
int lsk_krpc_mach_port_type(uint32_t task, uint32_t name, int *ptype) {
    return ipc_port_type(task, name, ptype);
}

int lsk_krpc_mach_port_type_trap(uint32_t task, uint32_t name, int *ptype) __asm__("__kernelrpc_mach_port_type_trap");
int lsk_krpc_mach_port_type_trap(uint32_t task, uint32_t name, int *ptype) {
    return ipc_port_type(task, name, ptype);
}

int lsk_mach_port_names(uint32_t task, uint32_t **names, uint32_t *names_cnt, uint32_t **types, uint32_t *types_cnt) __asm__("_mach_port_names");
int lsk_mach_port_names(uint32_t task, uint32_t **names, uint32_t *names_cnt, uint32_t **types, uint32_t *types_cnt) {
    return ipc_port_names(task, names, names_cnt, types, types_cnt);
}

int lsk_krpc_mach_port_names(uint32_t task, uint32_t **names, uint32_t *names_cnt, uint32_t **types, uint32_t *types_cnt) __asm__("__kernelrpc_mach_port_names");
int lsk_krpc_mach_port_names(uint32_t task, uint32_t **names, uint32_t *names_cnt, uint32_t **types, uint32_t *types_cnt) {
    return ipc_port_names(task, names, names_cnt, types, types_cnt);
}

int lsk_mach_port_get_refs(uint32_t task, uint32_t name, int right, uint32_t *refs) __asm__("_mach_port_get_refs");
int lsk_mach_port_get_refs(uint32_t task, uint32_t name, int right, uint32_t *refs) {
    return ipc_port_get_refs(task, name, right, refs);
}

int lsk_krpc_mach_port_get_refs(uint32_t task, uint32_t name, int right, uint32_t *refs) __asm__("__kernelrpc_mach_port_get_refs");
int lsk_krpc_mach_port_get_refs(uint32_t task, uint32_t name, int right, uint32_t *refs) {
    return ipc_port_get_refs(task, name, right, refs);
}

int lsk_mach_port_get_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) __asm__("_mach_port_get_attributes");
int lsk_mach_port_get_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) {
    return ipc_port_get_attributes(task, name, flavor, info, cnt);
}

int lsk_krpc_mach_port_get_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) __asm__("__kernelrpc_mach_port_get_attributes");
int lsk_krpc_mach_port_get_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) {
    return ipc_port_get_attributes(task, name, flavor, info, cnt);
}

int lsk_krpc_mach_port_get_attributes_trap(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) __asm__("__kernelrpc_mach_port_get_attributes_trap");
int lsk_krpc_mach_port_get_attributes_trap(uint32_t task, uint32_t name, int flavor, void *info, uint32_t *cnt) {
    return ipc_port_get_attributes(task, name, flavor, info, cnt);
}

int lsk_mach_port_set_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t cnt) __asm__("_mach_port_set_attributes");
int lsk_mach_port_set_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t cnt) {
    return ipc_port_set_attributes(task, name, flavor, info, cnt);
}

int lsk_krpc_mach_port_set_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t cnt) __asm__("__kernelrpc_mach_port_set_attributes");
int lsk_krpc_mach_port_set_attributes(uint32_t task, uint32_t name, int flavor, void *info, uint32_t cnt) {
    return ipc_port_set_attributes(task, name, flavor, info, cnt);
}

int lsk_mach_port_get_context(uint32_t task, uint32_t name, uint64_t *context) __asm__("_mach_port_get_context");
int lsk_mach_port_get_context(uint32_t task, uint32_t name, uint64_t *context) {
    return ipc_port_get_context(task, name, context);
}

int lsk_krpc_mach_port_get_context(uint32_t task, uint32_t name, uint64_t *context) __asm__("__kernelrpc_mach_port_get_context");
int lsk_krpc_mach_port_get_context(uint32_t task, uint32_t name, uint64_t *context) {
    return ipc_port_get_context(task, name, context);
}

int lsk_mach_port_set_context(uint32_t task, uint32_t name, uint64_t context) __asm__("_mach_port_set_context");
int lsk_mach_port_set_context(uint32_t task, uint32_t name, uint64_t context) {
    return ipc_port_set_context(task, name, context);
}

int lsk_krpc_mach_port_set_context(uint32_t task, uint32_t name, uint64_t context) __asm__("__kernelrpc_mach_port_set_context");
int lsk_krpc_mach_port_set_context(uint32_t task, uint32_t name, uint64_t context) {
    return ipc_port_set_context(task, name, context);
}

int lsk_mach_port_guard(uint32_t task, uint32_t name, uint64_t guard, int strict) __asm__("_mach_port_guard");
int lsk_mach_port_guard(uint32_t task, uint32_t name, uint64_t guard, int strict) {
    return ipc_port_guard(task, name, guard, strict);
}

int lsk_krpc_mach_port_guard(uint32_t task, uint32_t name, uint64_t guard, int strict) __asm__("__kernelrpc_mach_port_guard");
int lsk_krpc_mach_port_guard(uint32_t task, uint32_t name, uint64_t guard, int strict) {
    return ipc_port_guard(task, name, guard, strict);
}

int lsk_krpc_mach_port_guard_trap(uint32_t task, uint32_t name, uint64_t guard, int strict) __asm__("__kernelrpc_mach_port_guard_trap");
int lsk_krpc_mach_port_guard_trap(uint32_t task, uint32_t name, uint64_t guard, int strict) {
    return ipc_port_guard(task, name, guard, strict);
}

int lsk_mach_port_unguard(uint32_t task, uint32_t name, uint64_t guard) __asm__("_mach_port_unguard");
int lsk_mach_port_unguard(uint32_t task, uint32_t name, uint64_t guard) {
    return ipc_port_unguard(task, name, guard);
}

int lsk_krpc_mach_port_unguard(uint32_t task, uint32_t name, uint64_t guard) __asm__("__kernelrpc_mach_port_unguard");
int lsk_krpc_mach_port_unguard(uint32_t task, uint32_t name, uint64_t guard) {
    return ipc_port_unguard(task, name, guard);
}

int lsk_krpc_mach_port_unguard_trap(uint32_t task, uint32_t name, uint64_t guard) __asm__("__kernelrpc_mach_port_unguard_trap");
int lsk_krpc_mach_port_unguard_trap(uint32_t task, uint32_t name, uint64_t guard) {
    return ipc_port_unguard(task, name, guard);
}

int lsk_mach_port_guard_with_flags(uint32_t task, uint32_t name, uint64_t guard, uint64_t flags) __asm__("_mach_port_guard_with_flags");
int lsk_mach_port_guard_with_flags(uint32_t task, uint32_t name, uint64_t guard, uint64_t flags) {
    return ipc_port_guard_with_flags(task, name, guard, flags);
}

int lsk_krpc_mach_port_guard_with_flags(uint32_t task, uint32_t name, uint64_t guard, uint64_t flags) __asm__("__kernelrpc_mach_port_guard_with_flags");
int lsk_krpc_mach_port_guard_with_flags(uint32_t task, uint32_t name, uint64_t guard, uint64_t flags) {
    return ipc_port_guard_with_flags(task, name, guard, flags);
}

int lsk_mach_port_swap_guard(uint32_t task, uint32_t name, uint64_t old_guard, uint64_t new_guard) __asm__("_mach_port_swap_guard");
int lsk_mach_port_swap_guard(uint32_t task, uint32_t name, uint64_t old_guard, uint64_t new_guard) {
    return ipc_port_swap_guard(task, name, old_guard, new_guard);
}

int lsk_krpc_mach_port_swap_guard(uint32_t task, uint32_t name, uint64_t old_guard, uint64_t new_guard) __asm__("__kernelrpc_mach_port_swap_guard");
int lsk_krpc_mach_port_swap_guard(uint32_t task, uint32_t name, uint64_t old_guard, uint64_t new_guard) {
    return ipc_port_swap_guard(task, name, old_guard, new_guard);
}

int lsk_mach_port_insert_member(uint32_t task, uint32_t name, uint32_t pset) __asm__("_mach_port_insert_member");
int lsk_mach_port_insert_member(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_insert_member(task, name, pset);
}

int lsk_krpc_mach_port_insert_member(uint32_t task, uint32_t name, uint32_t pset) __asm__("__kernelrpc_mach_port_insert_member");
int lsk_krpc_mach_port_insert_member(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_insert_member(task, name, pset);
}

int lsk_krpc_mach_port_insert_member_trap(uint32_t task, uint32_t name, uint32_t pset) __asm__("__kernelrpc_mach_port_insert_member_trap");
int lsk_krpc_mach_port_insert_member_trap(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_insert_member(task, name, pset);
}

int lsk_mach_port_extract_member(uint32_t task, uint32_t name, uint32_t pset) __asm__("_mach_port_extract_member");
int lsk_mach_port_extract_member(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_extract_member(task, name, pset);
}

int lsk_krpc_mach_port_extract_member(uint32_t task, uint32_t name, uint32_t pset) __asm__("__kernelrpc_mach_port_extract_member");
int lsk_krpc_mach_port_extract_member(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_extract_member(task, name, pset);
}

int lsk_krpc_mach_port_extract_member_trap(uint32_t task, uint32_t name, uint32_t pset) __asm__("__kernelrpc_mach_port_extract_member_trap");
int lsk_krpc_mach_port_extract_member_trap(uint32_t task, uint32_t name, uint32_t pset) {
    return ipc_port_extract_member(task, name, pset);
}

int lsk_mach_port_move_member(uint32_t task, uint32_t member, uint32_t after) __asm__("_mach_port_move_member");
int lsk_mach_port_move_member(uint32_t task, uint32_t member, uint32_t after) {
    return ipc_port_move_member(task, member, after);
}

int lsk_krpc_mach_port_move_member(uint32_t task, uint32_t member, uint32_t after) __asm__("__kernelrpc_mach_port_move_member");
int lsk_krpc_mach_port_move_member(uint32_t task, uint32_t member, uint32_t after) {
    return ipc_port_move_member(task, member, after);
}

int lsk_krpc_mach_port_move_member_trap(uint32_t task, uint32_t member, uint32_t after) __asm__("__kernelrpc_mach_port_move_member_trap");
int lsk_krpc_mach_port_move_member_trap(uint32_t task, uint32_t member, uint32_t after) {
    return ipc_port_move_member(task, member, after);
}

int lsk_mach_port_request_notification(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) __asm__("_mach_port_request_notification");
int lsk_mach_port_request_notification(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) {
    return ipc_port_request_notification(task, name, id, sync, notify, poly, previous);
}

int lsk_krpc_mach_port_request_notification(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) __asm__("__kernelrpc_mach_port_request_notification");
int lsk_krpc_mach_port_request_notification(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) {
    return ipc_port_request_notification(task, name, id, sync, notify, poly, previous);
}

int lsk_krpc_mach_port_request_notification_trap(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) __asm__("__kernelrpc_mach_port_request_notification_trap");
int lsk_krpc_mach_port_request_notification_trap(uint32_t task, uint32_t name, int id, uint32_t sync, uint32_t notify, int poly, uint32_t *previous) {
    return ipc_port_request_notification(task, name, id, sync, notify, poly, previous);
}

int lsk_mach_port_set_mscount(uint32_t task, uint32_t name, uint32_t mscount) __asm__("_mach_port_set_mscount");
int lsk_mach_port_set_mscount(uint32_t task, uint32_t name, uint32_t mscount) {
    return ipc_port_set_mscount(task, name, mscount);
}

int lsk_krpc_mach_port_set_mscount(uint32_t task, uint32_t name, uint32_t mscount) __asm__("__kernelrpc_mach_port_set_mscount");
int lsk_krpc_mach_port_set_mscount(uint32_t task, uint32_t name, uint32_t mscount) {
    return ipc_port_set_mscount(task, name, mscount);
}

int lsk_mach_port_kernel_object(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) __asm__("_mach_port_kernel_object");
int lsk_mach_port_kernel_object(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) {
    return ipc_port_kernel_object(task, name, kobject_type, kobject_addr);
}

int lsk_krpc_mach_port_kernel_object(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) __asm__("__kernelrpc_mach_port_kernel_object");
int lsk_krpc_mach_port_kernel_object(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) {
    return ipc_port_kernel_object(task, name, kobject_type, kobject_addr);
}

int lsk_mach_port_kobject(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) __asm__("_mach_port_kobject");
int lsk_mach_port_kobject(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) {
    return ipc_port_kernel_object(task, name, kobject_type, kobject_addr);
}

int lsk_krpc_mach_port_kobject(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) __asm__("__kernelrpc_mach_port_kobject");
int lsk_krpc_mach_port_kobject(uint32_t task, uint32_t name, uint32_t *kobject_type, uint64_t *kobject_addr) {
    return ipc_port_kernel_object(task, name, kobject_type, kobject_addr);
}

int lsk_mach_port_is_connection_for_service(uint32_t task, uint32_t name, void *sp, uint64_t *filter_policy) __asm__("_mach_port_is_connection_for_service");
int lsk_mach_port_is_connection_for_service(uint32_t task, uint32_t name, void *sp, uint64_t *filter_policy) {
    (void)task; (void)name; (void)sp; (void)filter_policy;
    return KERN_FAILURE;
}

int lsk_krpc_mach_port_is_connection_for_service(uint32_t task, uint32_t name, void *sp, uint64_t *filter_policy) __asm__("__kernelrpc_mach_port_is_connection_for_service");
int lsk_krpc_mach_port_is_connection_for_service(uint32_t task, uint32_t name, void *sp, uint64_t *filter_policy) {
    (void)task; (void)name; (void)sp; (void)filter_policy;
    return KERN_FAILURE;
}

/* mach_msg / mach_msg_trap */
int lsk_mach_msg_trap(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                      uint32_t rcv_name, uint32_t timeout, uint32_t notify) __asm__("_mach_msg_trap");
int lsk_mach_msg_trap(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                      uint32_t rcv_name, uint32_t timeout, uint32_t notify) {
    return ipc_msg(msg, (uint32_t)option, send_size, rcv_size, rcv_name, timeout, notify, NULL, 0);
}

int lsk_mach_msg_overwrite_trap(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                                uint32_t rcv_name, uint32_t timeout, uint32_t notify, void *scatter, uint32_t scatter_size) __asm__("_mach_msg_overwrite_trap");
int lsk_mach_msg_overwrite_trap(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                                uint32_t rcv_name, uint32_t timeout, uint32_t notify, void *scatter, uint32_t scatter_size) {
    return ipc_msg(msg, (uint32_t)option, send_size, rcv_size, rcv_name, timeout, notify, scatter, scatter_size);
}

int lsk_mach_msg2_trap(void *data, uint64_t options, uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
                       uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority, uint64_t timeout) __asm__("_mach_msg2_trap");
int lsk_mach_msg2_trap(void *data, uint64_t options, uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
                       uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority, uint64_t timeout) {
    return ipc_msg2(data, options, msgh_bits_and_send_size, msgh_remote_and_local_port,
                    msgh_voucher_and_id, desc_count_and_rcv_name, rcv_size_and_priority, timeout);
}

int lsk_mach_msg2_internal(void *data, uint64_t options, uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
                          uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority, uint64_t timeout) __asm__("_mach_msg2_internal");
int lsk_mach_msg2_internal(void *data, uint64_t options, uint64_t msgh_bits_and_send_size, uint64_t msgh_remote_and_local_port,
                          uint64_t msgh_voucher_and_id, uint64_t desc_count_and_rcv_name, uint64_t rcv_size_and_priority, uint64_t timeout) {
    return ipc_msg2(data, options, msgh_bits_and_send_size, msgh_remote_and_local_port,
                    msgh_voucher_and_id, desc_count_and_rcv_name, rcv_size_and_priority, timeout);
}

int lsk_mach_msg(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                 uint32_t rcv_name, uint32_t timeout, uint32_t notify) __asm__("_mach_msg");
int lsk_mach_msg(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                 uint32_t rcv_name, uint32_t timeout, uint32_t notify) {
    return ipc_msg(msg, (uint32_t)option, send_size, rcv_size, rcv_name, timeout, notify, NULL, 0);
}

int lsk_mach_msg_overwrite(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                           uint32_t rcv_name, uint32_t timeout, uint32_t notify, void *scatter, uint32_t scatter_size) __asm__("_mach_msg_overwrite");
int lsk_mach_msg_overwrite(void *msg, int option, uint32_t send_size, uint32_t rcv_size,
                           uint32_t rcv_name, uint32_t timeout, uint32_t notify, void *scatter, uint32_t scatter_size) {
    return ipc_msg(msg, (uint32_t)option, send_size, rcv_size, rcv_name, timeout, notify, scatter, scatter_size);
}

int lsk_mach_msg_send(void *msg) __asm__("_mach_msg_send");
int lsk_mach_msg_send(void *msg) {
    if (!msg) return MACH_SEND_INVALID_HEADER;
    mach_msg_header_t *h = (mach_msg_header_t *)msg;
    return ipc_msg(msg, MACH_SEND_MSG, h->msgh_size, 0, 0, 0, 0, NULL, 0);
}

int lsk_mach_msg_receive(void *msg) __asm__("_mach_msg_receive");
int lsk_mach_msg_receive(void *msg) {
    if (!msg) return MACH_RCV_INVALID_NAME;
    mach_msg_header_t *h = (mach_msg_header_t *)msg;
    return ipc_msg(msg, MACH_RCV_MSG, 0, h->msgh_size, h->msgh_local_port, 0, 0, NULL, 0);
}

void lsk_mach_msg_destroy(void *msg) __asm__("_mach_msg_destroy");
void lsk_mach_msg_destroy(void *msg) {
    ipc_msg_destroy(msg);
}

int lsk_mach_msg_server_once(int (*demux)(void *, void *), uint32_t max_size, uint32_t rcv_name, int options) __asm__("_mach_msg_server_once");
int lsk_mach_msg_server_once(int (*demux)(void *, void *), uint32_t max_size, uint32_t rcv_name, int options) {
    if (!demux || max_size < sizeof(mach_msg_header_t)) return KERN_INVALID_ARGUMENT;
    uint64_t buf_addr = 0;
    size_t alloc_sz = (max_size * 2 + 16383) & ~16383;
    mach_vm_allocate(0x103, &buf_addr, alloc_sz, 1 /* ANYWHERE */);
    if (!buf_addr) return KERN_RESOURCE_SHORTAGE;

    void *msg_buf = (void *)buf_addr;
    void *rep_buf = (void *)(buf_addr + max_size);

    int kr = ipc_msg(msg_buf, MACH_RCV_MSG | (uint32_t)options, 0, max_size, rcv_name, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, NULL, 0);
    if (kr == MACH_MSG_SUCCESS) {
        int handled = demux(msg_buf, rep_buf);
        if (handled) {
            mach_msg_header_t *rh = (mach_msg_header_t *)rep_buf;
            ipc_msg(rep_buf, MACH_SEND_MSG | (uint32_t)options, rh->msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, NULL, 0);
        }
    }

    mach_vm_deallocate(0x103, buf_addr, alloc_sz);
    return kr;
}

int lsk_mach_msg_server(int (*demux)(void *, void *), uint32_t max_size, uint32_t rcv_name, int options) __asm__("_mach_msg_server");
int lsk_mach_msg_server(int (*demux)(void *, void *), uint32_t max_size, uint32_t rcv_name, int options) {
    while (1) {
        int kr = lsk_mach_msg_server_once(demux, max_size, rcv_name, options);
        if (kr != MACH_MSG_SUCCESS && kr != MACH_RCV_TOO_LARGE) {
            return kr;
        }
    }
}

/* mk_timer */
uint32_t lsk_mk_timer_create(void) __asm__("_mk_timer_create");
uint32_t lsk_mk_timer_create(void) {
    return ipc_mk_timer_create();
}

int lsk_mk_timer_destroy(uint32_t name) __asm__("_mk_timer_destroy");
int lsk_mk_timer_destroy(uint32_t name) {
    return ipc_mk_timer_destroy(name);
}

int lsk_mk_timer_arm(uint32_t name, uint64_t expire_time) __asm__("_mk_timer_arm");
int lsk_mk_timer_arm(uint32_t name, uint64_t expire_time) {
    return ipc_mk_timer_arm(name, expire_time);
}

int lsk_mk_timer_arm_leeway(uint32_t name, uint64_t mk_timer_flags, uint64_t expire_time, uint64_t mk_leeway) __asm__("_mk_timer_arm_leeway");
int lsk_mk_timer_arm_leeway(uint32_t name, uint64_t mk_timer_flags, uint64_t expire_time, uint64_t mk_leeway) {
    return ipc_mk_timer_arm_leeway(name, mk_timer_flags, expire_time, mk_leeway);
}

int lsk_mk_timer_cancel(uint32_t name, uint64_t *result_time) __asm__("_mk_timer_cancel");
int lsk_mk_timer_cancel(uint32_t name, uint64_t *result_time) {
    return ipc_mk_timer_cancel(name, result_time);
}

/* Semaphores */
struct mach_sem {
    uint32_t id;
    int val;
};
#define MAX_MACH_SEMS 128
static struct mach_sem g_mach_sems[MAX_MACH_SEMS];
static uint32_t g_sem_counter = 0x5001;

static struct mach_sem *get_mach_sem(uint32_t id) {
    if (id == 0) return 0;
    for (int i = 0; i < MAX_MACH_SEMS; i++) {
        if (g_mach_sems[i].id == id) return &g_mach_sems[i];
    }
    return 0;
}

int lsk_semaphore_create(uint32_t task, uint32_t *semaphore, int policy, int value) __asm__("_semaphore_create");
int lsk_semaphore_create(uint32_t task, uint32_t *semaphore, int policy, int value) {
    (void)task; (void)policy;
    if (!semaphore) return 4; /* KERN_INVALID_ARGUMENT */
    for (int i = 0; i < MAX_MACH_SEMS; i++) {
        if (g_mach_sems[i].id == 0) {
            g_mach_sems[i].id = g_sem_counter++;
            g_mach_sems[i].val = value;
            *semaphore = g_mach_sems[i].id;
            return KERN_SUCCESS;
        }
    }
    return 3; /* KERN_RESOURCE_SHORTAGE */
}

int lsk_semaphore_destroy(uint32_t task, uint32_t semaphore) __asm__("_semaphore_destroy");
int lsk_semaphore_destroy(uint32_t task, uint32_t semaphore) {
    (void)task;
    struct mach_sem *s = get_mach_sem(semaphore);
    if (s) {
        s->id = 0;
        s->val = 0;
    }
    return KERN_SUCCESS;
}

int lsk_semaphore_signal(uint32_t semaphore) __asm__("_semaphore_signal");
int lsk_semaphore_signal(uint32_t semaphore) {
    struct mach_sem *s = get_mach_sem(semaphore);
    if (!s) return 4; /* KERN_INVALID_ARGUMENT */
    __atomic_fetch_add(&s->val, 1, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&s->val, FUTEX_WAKE_PRIVATE, 1);
    return KERN_SUCCESS;
}

int lsk_semaphore_signal_all(uint32_t semaphore) __asm__("_semaphore_signal_all");
int lsk_semaphore_signal_all(uint32_t semaphore) {
    struct mach_sem *s = get_mach_sem(semaphore);
    if (!s) return 4;
    __atomic_fetch_add(&s->val, 100, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&s->val, FUTEX_WAKE_PRIVATE, 0x7fffffff);
    return KERN_SUCCESS;
}

int lsk_semaphore_wait(uint32_t semaphore) __asm__("_semaphore_wait");
int lsk_semaphore_wait(uint32_t semaphore) {
    struct mach_sem *s = get_mach_sem(semaphore);
    if (!s) return 4;
    while (1) {
        int v = __atomic_load_n(&s->val, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(&s->val, &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return KERN_SUCCESS;
            }
            continue;
        }
        sys4(SYS_futex, (long)&s->val, FUTEX_WAIT_PRIVATE, v, 0);
    }
}

int lsk_semaphore_timedwait(uint32_t semaphore, uint64_t wait_time) __asm__("_semaphore_timedwait");
int lsk_semaphore_timedwait(uint32_t semaphore, uint64_t wait_time) {
    struct mach_sem *s = get_mach_sem(semaphore);
    if (!s) return 4;
    uint32_t sec = (uint32_t)(wait_time & 0xffffffff);
    uint32_t nsec = (uint32_t)(wait_time >> 32);
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;

    while (1) {
        int v = __atomic_load_n(&s->val, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(&s->val, &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return KERN_SUCCESS;
            }
            continue;
        }
        long ret = sys4(SYS_futex, (long)&s->val, FUTEX_WAIT_PRIVATE, v, (long)&ts);
        if (ret == -110 /* -ETIMEDOUT */) {
            return 49; /* KERN_OPERATION_TIMED_OUT */
        }
    }
}

int lsk_semaphore_wait_signal(uint32_t wait_sem, uint32_t sig_sem) __asm__("_semaphore_wait_signal");
int lsk_semaphore_wait_signal(uint32_t wait_sem, uint32_t sig_sem) {
    lsk_semaphore_signal(sig_sem);
    return lsk_semaphore_wait(wait_sem);
}

int lsk_semaphore_timedwait_signal(uint32_t wait_sem, uint32_t sig_sem, uint64_t wait_time) __asm__("_semaphore_timedwait_signal");
int lsk_semaphore_timedwait_signal(uint32_t wait_sem, uint32_t sig_sem, uint64_t wait_time) {
    lsk_semaphore_signal(sig_sem);
    return lsk_semaphore_timedwait(wait_sem, wait_time);
}

/* Vouchers */
const uintptr_t *g_voucher_table = 0;

int lsk_voucher_mach_msg_set(void *msg, uint32_t voucher) __asm__("_voucher_mach_msg_set");
int lsk_voucher_mach_msg_set(void *msg, uint32_t voucher) {
    const uintptr_t *tbl = g_voucher_table;
    if (tbl && tbl[1]) {
        uintptr_t fn = tbl[1];
        register void *a0 __asm__("x0") = msg;
        register uint32_t a1 __asm__("x1") = voucher;
        register int res __asm__("w0");
        __asm__ volatile ("braaz %3" : "=r"(res) : "r"(a0), "r"(a1), "r"(fn));
        return res;
    }
    return 0;
}

void lsk_voucher_mach_msg_clear(void *msg) __asm__("_voucher_mach_msg_clear");
void lsk_voucher_mach_msg_clear(void *msg) {
    const uintptr_t *tbl = g_voucher_table;
    if (tbl && tbl[2]) {
        uintptr_t fn = tbl[2];
        register void *a0 __asm__("x0") = msg;
        __asm__ volatile ("braaz %1" : : "r"(a0), "r"(fn));
    }
}

uint32_t lsk_voucher_mach_msg_get(void *msg) __asm__("_voucher_mach_msg_get");
uint32_t lsk_voucher_mach_msg_get(void *msg) {
    (void)msg;
    return 0;
}

int lsk_mach_voucher_deallocate(uint32_t task, uint32_t name) __asm__("_mach_voucher_deallocate");
int lsk_mach_voucher_deallocate(uint32_t task, uint32_t name) {
    (void)task; (void)name;
    return KERN_SUCCESS;
}

uint64_t lsk_voucher_mach_msg_adopt(void *msg) __asm__("_voucher_mach_msg_adopt");
uint64_t lsk_voucher_mach_msg_adopt(void *msg) {
    const uintptr_t *tbl = g_voucher_table;
    if (tbl && tbl[3]) {
        uintptr_t fn = tbl[3];
        register void *a0 __asm__("x0") = msg;
        register uint64_t res __asm__("x0");
        __asm__ volatile ("braaz %2" : "=r"(res) : "r"(a0), "r"(fn));
        return res;
    }
    /* No voucher table: return VOUCHER_NULL (0), not -1. -1 is a valid
     * voucher value in some contexts and traps callers that treat it as
     * a real voucher; 0 is the documented "no voucher" value. */
    return 0;
}

void lsk_voucher_mach_msg_revert(uint64_t voucher) __asm__("_voucher_mach_msg_revert");
void lsk_voucher_mach_msg_revert(uint64_t voucher) {
    const uintptr_t *tbl = g_voucher_table;
    if (tbl && tbl[4]) {
        uintptr_t fn = tbl[4];
        register uint64_t a0 __asm__("x0") = voucher;
        __asm__ volatile ("braaz %1" : : "r"(a0), "r"(fn));
    }
}

int lsk_mach_generate_activity_id(uint32_t target, int count, uint64_t *activity_id) __asm__("_mach_generate_activity_id");
int lsk_mach_generate_activity_id(uint32_t target, int count, uint64_t *activity_id) {
    (void)target;
    static uint64_t g_act_id = 1000;
    if (activity_id && count > 0) {
        for (int i = 0; i < count; i++) {
            activity_id[i] = g_act_id++;
        }
    }
    return KERN_SUCCESS;
}

/* host_get_special_port: unprivileged tasks get no host special ports. libdispatch
 * (_dispatch_mach_host_port_init, 0x1be0325c0) treats KERN_INVALID_ARGUMENT (4) as a normal outcome. */
int lsk_host_get_special_port(uint32_t host, int node, int which, uint32_t *port) __asm__("_host_get_special_port");
int lsk_host_get_special_port(uint32_t host, int node, int which, uint32_t *port) {
    (void)host; (void)node; (void)which;
    if (port) *port = 0;
    return 4; /* KERN_INVALID_ARGUMENT */
}

/* host_get_io_main: with IOSWINE_IOKIT=1 hand out our in-process IOKit master port (iokit.c / iosurface.c);
 * otherwise KERN_INVALID_ARGUMENT as before (IOKit clients then see "no IOKit"). */
extern const char *lsk_getenv(const char *name);
extern int iosurface_init(void);
extern uint32_t iokit_master_port(void);
int lsk_host_get_io_main(uint32_t host, uint32_t *main_port) __asm__("_host_get_io_main");
int lsk_host_get_io_main(uint32_t host, uint32_t *main_port) {
    (void)host;
    if (!main_port) return 4;
    *main_port = 0;
    if (!lsk_getenv("IOSWINE_IOKIT")) return 4; /* KERN_INVALID_ARGUMENT */
    if (iosurface_init() != 0) return 5;        /* KERN_FAILURE */
    *main_port = iokit_master_port();
    return *main_port ? 0 : 5;
}

int lsk_mig_strncpy_zerofill(char *dest, const char *src, int len) __asm__("_mig_strncpy_zerofill");
int lsk_mig_strncpy_zerofill(char *dest, const char *src, int len) {
    if (!dest || len <= 0) return 0;
    if (len == 1) {
        dest[0] = '\0';
        return (src && src[0] == '\0') ? 1 : 1;
    }
    int copied_len = 0;
    int hit_null = (src == NULL);
    for (int i = 0; i < len - 1; i++) {
        if (!hit_null) {
            char c = src[i];
            dest[i] = c;
            if (c == '\0') {
                hit_null = 1;
                copied_len = i + 1;
            }
        } else {
            dest[i] = '\0';
        }
    }
    dest[len - 1] = '\0';
    return hit_null ? copied_len : len;
}

int lsk_mig_strncpy(char *dest, const char *src, int len) __asm__("_mig_strncpy");
int lsk_mig_strncpy(char *dest, const char *src, int len) {
    if (!dest || len <= 0) return 0;
    for (int i = 0; i < len; i++) {
        if (!src) {
            dest[i] = '\0';
            return i + 1;
        }
        char c = src[i];
        dest[i] = c;
        if (c == '\0') return i + 1;
    }
    dest[len - 1] = '\0';
    return len;
}

/* host_create_mach_voucher: libdispatch (_voucher_init path, 0x1be034004) crashes on any error, so hand out a
 * real port name (receive + send right, never receives anything). Voucher attributes are ignored. */
int lsk_host_create_mach_voucher(uint32_t host, void *recipes, uint32_t size, uint32_t *voucher) __asm__("_host_create_mach_voucher");
int lsk_host_create_mach_voucher(uint32_t host, void *recipes, uint32_t size, uint32_t *voucher) {
    (void)host; (void)recipes; (void)size;
    mach_port_name_t n = 0;
    int kr = ipc_port_allocate(ipc_task_self(), 1 /* MACH_PORT_RIGHT_RECEIVE */, &n);
    if (kr) return kr;
    kr = ipc_port_insert_right(ipc_task_self(), n, n, 20 /* MACH_MSG_TYPE_MAKE_SEND */);
    if (kr) return kr;
    if (voucher) *voucher = n;
    return 0;
}

/* task_name_for_pid: our own pid -> the task self name (0x103); other pids do not exist for us. */
int lsk_task_name_for_pid(uint32_t target_tport, int pid, uint32_t *tn) __asm__("_task_name_for_pid");
int lsk_task_name_for_pid(uint32_t target_tport, int pid, uint32_t *tn) {
    (void)target_tport;
    if (!tn) return 4;
    if (pid == (int)sys0(SYS_getpid)) { *tn = 0x103; return 0; }
    *tn = 0;
    return 5; /* KERN_FAILURE */
}

/* mach_error_string / mach_error_type: minimal (callers only log the text). */
const char *lsk_mach_error_string(int err) __asm__("_mach_error_string");
const char *lsk_mach_error_string(int err) {
    switch (err) {
    case 0: return "(os/kern) successful";
    case 4: return "(os/kern) invalid argument";
    case 5: return "(os/kern) failure";
    case 15: return "(os/kern) invalid name";
    default: return "(ioswine) mach error";
    }
}
const char *lsk_mach_error_type(int err) __asm__("_mach_error_type");
const char *lsk_mach_error_type(int err) { (void)err; return "(os/kern)"; }

/* host_request_notification(host, type, port): libdispatch asks for calendar-change
 * notifications (HOST_NOTIFY_CALENDAR_CHANGE). Nothing ever changes the wall clock under us,
 * so accept and never notify. */
int lsk_host_request_notification(uint32_t host, int type, uint32_t port) __asm__("_host_request_notification");
int lsk_host_request_notification(uint32_t host, int type, uint32_t port) {
    (void)host; (void)type; (void)port;
    return 0;
}
