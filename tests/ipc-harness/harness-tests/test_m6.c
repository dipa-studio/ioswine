/* test_m6.c - M6 regression: a right for a DESTROYED port must not be delivered.
 *
 * Replicates the demonstrated hazard: a message carrying a
 * port is queued, the port is destroyed, then the message is received. The
 * base code delivered a live SEND right for the destroyed port (name
 * 0x40b, disp 17). The fix must deliver MACH_PORT_NULL instead.
 *
 * Three destroy paths are covered: ipc_port_destroy, ipc_port_mod_refs
 * (RECEIVE, -1) - the mod_refs path is the gap where mod_refs
 * cleared has_receive_right with no generation bump - and a
 * positive case where the port stays alive and the right is delivered.
 *
 * Regression test adopted into
 * harness-tests/ (built and run by build.sh, same as the other tests).
 * No em dashes are used in this file.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ipc.h"
#include "linux.h"

static int checks = 0, failures = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECK_ERR(call, want) do { int _r = (call); checks++; if (_r != (want)) { failures++; printf("FAIL line %d: %s -> %d, want %d\n", __LINE__, #call, _r, (int)(want)); } } while (0)

typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t desc;
} carry_msg_t;

static void send_carry(mach_port_name_t dest, mach_port_name_t carried) {
    carry_msg_t m;
    memset(&m, 0, sizeof m);
    m.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    m.hdr.msgh_size = sizeof m;
    m.hdr.msgh_remote_port = dest;
    m.hdr.msgh_local_port = MACH_PORT_NULL;
    m.hdr.msgh_voucher_port = MACH_PORT_NULL;
    m.hdr.msgh_id = 0x7777;
    m.body.msgh_descriptor_count = 1;
    m.desc.name = carried;
    m.desc.pad1 = 0;
    m.desc.pad2 = 0;
    m.desc.disposition = MACH_MSG_TYPE_MAKE_SEND;
    m.desc.type = MACH_MSG_PORT_DESCRIPTOR;
    int rc = ipc_msg(&m, MACH_SEND_MSG, sizeof m, 0, 0, 0, 0, NULL, 0);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
}

static mach_port_name_t recv_carry(mach_port_name_t dest, uint8_t *out_disp) {
    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);
    int rc = ipc_msg(buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof buf, dest, 5000, 0, NULL, 0);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc != MACH_MSG_SUCCESS) return (mach_port_name_t)0xdead;
    carry_msg_t *m = (carry_msg_t *)buf;
    if (out_disp) *out_disp = m->desc.disposition;
    return m->desc.name;
}

static void test_destroy_port(void) {
    printf("--- m6 destroy via ipc_port_destroy ---\n");
    mach_port_t task = ipc_task_self();
    mach_port_name_t p = MACH_PORT_NULL, d = MACH_PORT_NULL;
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p), KERN_SUCCESS);
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &d), KERN_SUCCESS);
    CHECK_ERR(ipc_port_insert_right(task, d, d, MACH_MSG_TYPE_MAKE_SEND), KERN_SUCCESS);
    send_carry(d, p);
    /* Destroy the carried port while the message is queued. */
    CHECK_ERR(ipc_port_destroy(task, p), KERN_SUCCESS);
    uint8_t disp = 0;
    mach_port_name_t got = recv_carry(d, &disp);
    printf("destroy: delivered name=0x%x disp=%u (want NULL/17)\n", got, disp);
    CHECK(got == MACH_PORT_NULL);
    CHECK(disp == MACH_MSG_TYPE_PORT_SEND);
    CHECK_ERR(ipc_port_destroy(task, d), KERN_SUCCESS);
}

static void test_destroy_modrefs(void) {
    printf("--- m6 destroy via mod_refs RECEIVE -1 ---\n");
    mach_port_t task = ipc_task_self();
    mach_port_name_t p = MACH_PORT_NULL, d = MACH_PORT_NULL;
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p), KERN_SUCCESS);
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &d), KERN_SUCCESS);
    CHECK_ERR(ipc_port_insert_right(task, d, d, MACH_MSG_TYPE_MAKE_SEND), KERN_SUCCESS);
    send_carry(d, p);
    /* Destroy the carried port's receive right via mod_refs. */
    CHECK_ERR(ipc_port_mod_refs(task, p, MACH_PORT_RIGHT_RECEIVE, -1), KERN_SUCCESS);
    uint8_t disp = 0;
    mach_port_name_t got = recv_carry(d, &disp);
    printf("modrefs: delivered name=0x%x disp=%u (want NULL/17)\n", got, disp);
    CHECK(got == MACH_PORT_NULL);
    CHECK(disp == MACH_MSG_TYPE_PORT_SEND);
    CHECK_ERR(ipc_port_destroy(task, d), KERN_SUCCESS);
}

static void test_alive_positive(void) {
    printf("--- m6 positive: port alive, right delivered ---\n");
    mach_port_t task = ipc_task_self();
    mach_port_name_t p = MACH_PORT_NULL, d = MACH_PORT_NULL;
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p), KERN_SUCCESS);
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &d), KERN_SUCCESS);
    CHECK_ERR(ipc_port_insert_right(task, d, d, MACH_MSG_TYPE_MAKE_SEND), KERN_SUCCESS);
    send_carry(d, p);
    uint8_t disp = 0;
    mach_port_name_t got = recv_carry(d, &disp);
    printf("alive: delivered name=0x%x disp=%u (want non-NULL/17)\n", got, disp);
    CHECK(got != MACH_PORT_NULL);
    CHECK(disp == MACH_MSG_TYPE_PORT_SEND);
    /* The delivered send right is usable: deallocate it. */
    CHECK_ERR(ipc_port_deallocate(task, got), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, p), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, d), KERN_SUCCESS);
}

int main(void) {
    printf("m6 regression test\n");
    test_destroy_port();
    test_destroy_modrefs();
    test_alive_positive();
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
