/*
 * mem_entry_test.c -- host test for mach_make_memory_entry_64 / mach_make_memory_entry
 * and shared mach_vm_map / vm_map.
 *
 * Links the real libsystem_kernel/src/vm.c + ipc.c under tests/host/host-shim/linux.h.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* vm.c entry points */
extern int mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) __asm__("_mach_vm_allocate");
extern int mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) __asm__("_mach_vm_deallocate");
extern int mach_vm_map(uint32_t target, uint64_t *address, uint64_t size, uint64_t mask, int flags,
                       uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("_mach_vm_map");
extern int vm_map(uint32_t target, uintptr_t *address, size_t size, uintptr_t mask, int flags,
                  uint32_t object, uint64_t offset, int copy, int cur_prot, int max_prot, int inherit) __asm__("_vm_map");
extern int mach_make_memory_entry_64(uint32_t target_task, uint64_t *size, uint64_t offset,
                                     int permission, uint32_t *object_handle, uint32_t parent_entry) __asm__("_mach_make_memory_entry_64");
extern int mach_make_memory_entry(uint32_t target_task, uintptr_t *size, uintptr_t offset,
                                  int permission, uint32_t *object_handle, uint32_t parent_entry) __asm__("_mach_make_memory_entry");

/* Stubs needed by ipc.c */
uint64_t _mach_absolute_time(void) { return 1000000ULL; }
int _mach_timebase_info(void *info) {
    uint32_t *p = (uint32_t *)info;
    p[0] = 1;
    p[1] = 1;
    return 0;
}
uint32_t _mach_thread_self(void) { return 0x303; }
void snapshot_init(void) {}
int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size, uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size; (void)out_reply_size;
    return 0;
}
int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size, uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size; (void)out_reply_size;
    return 0;
}
int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size, uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size; (void)out_reply_size;
    return 0;
}
int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size, uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size; (void)out_reply_size;
    return 0;
}
int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size, uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size; (void)out_reply_size;
    return 0;
}
int hidsys_is_enabled(void) { return 0; }
int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap, uint32_t *out_reply_size) {
    (void)msg; (void)send_size; (void)reply_buf; (void)reply_cap; (void)out_reply_size;
    return 0;
}
void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint) {
    (void)service_port; (void)endpoint;
}
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len, uint32_t *out_msgh_id) {
    (void)buf; (void)cap; (void)out_len; (void)out_msgh_id;
    return 0;
}
void set_errno(int err) { (void)err; }
int linux_to_darwin_errno(long linux_err) { return linux_err < 0 ? (int)(-linux_err) : 0; }

#define VM_FLAGS_ANYWHERE     0x0001
#define VM_PROT_READ          0x01
#define VM_PROT_WRITE         0x02
#define VM_PROT_IS_MASK       0x40
#define MAP_MEM_NAMED_CREATE  0x000020
#define MAP_MEM_PURGABLE      0x000040
#define MAP_MEM_VM_COPY       0x200000
#define MAP_MEM_VM_SHARE      0x400000

static int count_open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d) != NULL) n++;
    closedir(d);
    return n;
}

static void test_named_create_and_share(void) {
    uint64_t sz = 4096;
    uint32_t entry = 0;
    int kr = mach_make_memory_entry_64(0, &sz, 0,
                                       VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_NAMED_CREATE,
                                       &entry, 0);
    CHECK(kr == KERN_SUCCESS);
    CHECK(entry != MACH_PORT_NULL);
    CHECK(sz == 16384);

    uint64_t a1 = 0, a2 = 0, a3 = 0;
    kr = mach_vm_map(0x103, &a1, 16384, 0, VM_FLAGS_ANYWHERE, entry, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr == KERN_SUCCESS && a1 != 0);

    kr = mach_vm_map(0x103, &a2, 16384, 0, VM_FLAGS_ANYWHERE, entry, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr == KERN_SUCCESS && a2 != 0 && a2 != a1);

    /* Shared writes in both directions */
    *(volatile uint32_t *)a1 = 0xdeadbeefU;
    CHECK(*(volatile uint32_t *)a2 == 0xdeadbeefU);
    *(volatile uint32_t *)(a2 + 4096) = 0xcafebabeU;
    CHECK(*(volatile uint32_t *)(a1 + 4096) == 0xcafebabeU);

    /* copy=1 (MAP_PRIVATE) sees initial data but isolates subsequent writes */
    kr = mach_vm_map(0x103, &a3, 16384, 0, VM_FLAGS_ANYWHERE, entry, 0, 1,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr == KERN_SUCCESS && a3 != 0);
    CHECK(*(volatile uint32_t *)a3 == 0xdeadbeefU);
    *(volatile uint32_t *)a3 = 0x11112222U;
    CHECK(*(volatile uint32_t *)a1 == 0xdeadbeefU);

    CHECK(mach_vm_deallocate(0x103, a1, 16384) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, a2, 16384) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, a3, 16384) == KERN_SUCCESS);
    CHECK(ipc_port_deallocate(0x103, entry) == KERN_SUCCESS);

    /* Mapping after deallocate must fail */
    uint64_t a4 = 0;
    kr = mach_vm_map(0x103, &a4, 16384, 0, VM_FLAGS_ANYWHERE, entry, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr != KERN_SUCCESS);
}

static void test_existing_range_xpc_shmem_pattern(void) {
    uint64_t src = 0;
    int kr = mach_vm_allocate(0x103, &src, 32768, VM_FLAGS_ANYWHERE);
    CHECK(kr == KERN_SUCCESS && src != 0);
    strcpy((char *)src, "page0-hello");
    strcpy((char *)(src + 16384), "page1-world");

    /* Exact __xpc_shmem_create_with_prot flags: prot | MAP_MEM_VM_SHARE | MAP_MEM_PURGABLE */
    uint64_t sz = 32768;
    uint32_t entry = 0;
    kr = mach_make_memory_entry_64(0x103, &sz, src,
                                   VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_VM_SHARE | MAP_MEM_PURGABLE,
                                   &entry, 0);
    CHECK(kr == KERN_SUCCESS && entry != MACH_PORT_NULL && sz == 32768);

    /* Exact _xpc_shmem_map flags: cur_prot = 0x43, max_prot = 0x43 */
    uintptr_t dst = 0;
    kr = vm_map(0x103, &dst, 32768, 0, VM_FLAGS_ANYWHERE, entry, 0, 0,
                VM_PROT_READ | VM_PROT_WRITE | VM_PROT_IS_MASK,
                VM_PROT_READ | VM_PROT_WRITE | VM_PROT_IS_MASK, 2);
    CHECK(kr == KERN_SUCCESS && dst != 0 && (uint64_t)dst != src);
    CHECK(strcmp((const char *)dst, "page0-hello") == 0);
    CHECK(strcmp((const char *)(dst + 16384), "page1-world") == 0);

    /* Mutate src -> visible in dst; mutate dst -> visible in src */
    strcpy((char *)src, "from-src");
    CHECK(strcmp((const char *)dst, "from-src") == 0);
    strcpy((char *)(dst + 16384), "from-dst");
    CHECK(strcmp((const char *)(src + 16384), "from-dst") == 0);

    /* Parent entry subrange at offset 16384 */
    uint64_t sub_sz = 16384;
    uint32_t sub_entry = 0;
    kr = mach_make_memory_entry_64(0x103, &sub_sz, 16384, VM_PROT_READ | VM_PROT_WRITE,
                                   &sub_entry, entry);
    CHECK(kr == KERN_SUCCESS && sub_entry != MACH_PORT_NULL && sub_sz == 16384);

    uint64_t sub_dst = 0;
    kr = mach_vm_map(0x103, &sub_dst, 16384, 0, VM_FLAGS_ANYWHERE, sub_entry, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr == KERN_SUCCESS && sub_dst != 0);
    CHECK(strcmp((const char *)sub_dst, "from-dst") == 0);
    strcpy((char *)sub_dst, "via-sub");
    CHECK(strcmp((const char *)(src + 16384), "via-sub") == 0);

    /* Create a second entry on the same live range -> still shares */
    uintptr_t sz2 = 16384;
    uint32_t entry2 = 0;
    kr = mach_make_memory_entry(0x103, &sz2, (uintptr_t)(src + 16384),
                                VM_PROT_READ, &entry2, 0);
    CHECK(kr == KERN_SUCCESS && entry2 != MACH_PORT_NULL);

    /* Without VM_PROT_IS_MASK, requesting WRITE on a READ-only entry fails */
    uint64_t ro_bad = 0;
    kr = mach_vm_map(0x103, &ro_bad, 16384, 0, VM_FLAGS_ANYWHERE, entry2, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2);
    CHECK(kr == KERN_PROTECTION_FAILURE);

    /* With VM_PROT_IS_MASK (0x43), it clamps to READ-only and succeeds */
    uint64_t ro_ok = 0;
    kr = mach_vm_map(0x103, &ro_ok, 16384, 0, VM_FLAGS_ANYWHERE, entry2, 0, 0,
                     VM_PROT_READ | VM_PROT_WRITE | VM_PROT_IS_MASK,
                     VM_PROT_READ | VM_PROT_WRITE | VM_PROT_IS_MASK, 2);
    CHECK(kr == KERN_SUCCESS && ro_ok != 0);
    CHECK(strcmp((const char *)ro_ok, "via-sub") == 0);

    CHECK(mach_vm_deallocate(0x103, ro_ok, 16384) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, sub_dst, 16384) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, (uint64_t)dst, 32768) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, src, 32768) == KERN_SUCCESS);
    CHECK(ipc_port_deallocate(0x103, entry2) == KERN_SUCCESS);
    CHECK(ipc_port_deallocate(0x103, sub_entry) == KERN_SUCCESS);
    CHECK(ipc_port_deallocate(0x103, entry) == KERN_SUCCESS);
}

static void test_ipc_transfer_and_errors(void) {
    /* Error cases */
    uint64_t sz = 0;
    uint32_t port = 0;
    CHECK(mach_make_memory_entry_64(0x103, &sz, 0, MAP_MEM_NAMED_CREATE, &port, 0) == KERN_INVALID_ARGUMENT);
    sz = 16384;
    CHECK(mach_make_memory_entry_64(0x103, &sz, 0x10000000ULL, VM_PROT_READ, &port, 0) == KERN_INVALID_ADDRESS);
    uint64_t bogus = 0;
    CHECK(mach_vm_map(0x103, &bogus, 16384, 0, VM_FLAGS_ANYWHERE, 0xdeadbeefU, 0, 0,
                      VM_PROT_READ, VM_PROT_READ, 2) == KERN_INVALID_ARGUMENT);

    /* Transfer memory entry over Mach IPC message descriptor */
    mach_port_name_t qport = 0;
    CHECK(ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &qport) == KERN_SUCCESS);
    CHECK(ipc_port_insert_right(0x103, qport, qport, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);

    sz = 16384;
    CHECK(mach_make_memory_entry_64(0, &sz, 0,
                                    VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_NAMED_CREATE,
                                    &port, 0) == KERN_SUCCESS);
    uint64_t v1 = 0;
    CHECK(mach_vm_map(0x103, &v1, 16384, 0, VM_FLAGS_ANYWHERE, port, 0, 0,
                      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2) == KERN_SUCCESS);
    *(volatile uint64_t *)v1 = 0x123456789abcdef0ULL;

    struct {
        mach_msg_header_t hdr;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t desc;
    } smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    smsg.hdr.msgh_size = sizeof(smsg);
    smsg.hdr.msgh_remote_port = qport;
    smsg.hdr.msgh_id = 42;
    smsg.body.msgh_descriptor_count = 1;
    smsg.desc.name = port;
    smsg.desc.disposition = MACH_MSG_TYPE_COPY_SEND;
    smsg.desc.type = MACH_MSG_PORT_DESCRIPTOR;
    CHECK(ipc_msg(&smsg, MACH_SEND_MSG, sizeof(smsg), 0, 0, 0, 0, NULL, 0) == MACH_MSG_SUCCESS);

    /* Deallocate sender's handle before receiving; in-flight descriptor keeps entry alive */
    CHECK(ipc_port_deallocate(0x103, port) == KERN_SUCCESS);

    uint8_t rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    CHECK(ipc_msg(rbuf, MACH_RCV_MSG, 0, sizeof(rbuf), qport, 0, 0, NULL, 0) == MACH_MSG_SUCCESS);
    mach_msg_port_descriptor_t *rdesc = (mach_msg_port_descriptor_t *)(rbuf + sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t));
    CHECK(rdesc->name != MACH_PORT_NULL);

    uint64_t v2 = 0;
    CHECK(mach_vm_map(0x103, &v2, 16384, 0, VM_FLAGS_ANYWHERE, rdesc->name, 0, 0,
                      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 2) == KERN_SUCCESS);
    CHECK(*(volatile uint64_t *)v2 == 0x123456789abcdef0ULL);

    CHECK(mach_vm_deallocate(0x103, v1, 16384) == KERN_SUCCESS);
    CHECK(mach_vm_deallocate(0x103, v2, 16384) == KERN_SUCCESS);
    CHECK(ipc_port_deallocate(0x103, rdesc->name) == KERN_SUCCESS);
    CHECK(ipc_port_destroy(0x103, qport) == KERN_SUCCESS);

    /* FD / port leak check */
    int fd_before = count_open_fds();
    for (int i = 0; i < 64; i++) {
        uint64_t lsz = 16384;
        uint32_t lport = 0;
        CHECK(mach_make_memory_entry_64(0, &lsz, 0,
                                        VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_NAMED_CREATE,
                                        &lport, 0) == KERN_SUCCESS);
        CHECK(ipc_port_deallocate(0x103, lport) == KERN_SUCCESS);
    }
    int fd_after = count_open_fds();
    CHECK(fd_before == fd_after);
}

int main(void) {
    ipc_init();
    test_named_create_and_share();
    test_existing_range_xpc_shmem_pattern();
    test_ipc_transfer_and_errors();
    printf("mem_entry_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
