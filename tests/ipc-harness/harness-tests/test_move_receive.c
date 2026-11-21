/*
 * test_move_receive.c - scenario 2: MOVE_RECEIVE in messages.
 *
 * Covers carrying a port descriptor with MOVE_RECEIVE disposition:
 * the receiver must get the receive right and the sender must lose
 * it. Also covers the M6 stale-pointer hazard: a descriptor that
 * carries a port whose entry is destroyed while the message is in
 * flight. Base ipc.c stores a raw struct ipc_port * in the
 * descriptor (ipc.c:2191) and dereferences it at receive
 * (ipc.c:2370) with no generation or liveness check, and
 * ipc_port_destroy does not scan queued messages (ipc.c:942-1000),
 * so the receive hands out a live-looking right for the destroyed
 * port. The fix addresses this (generation validation, storing
 * the name instead of the pointer, or clearing queued pointers in
 * destroy); after the fix the delivered name must be NULL.
 *
 * Note on slot reuse: the M6 audit says a slot "can be REUSED
 * after destroy", but in the current code a struct ipc_port slot
 * is never freed (in_use is only cleared on allocation failure,
 * ipc.c:536/561/584, never in destroy/destruct), so the stale
 * pointer keeps pointing at the leaked old struct rather than at
 * a different port. The hazard demonstrated here is therefore
 * "a right for a destroyed port is delivered", not "a different
 * port's name is delivered".
 *
 * Build/run: ./build.sh (plain cc, then ASan+UBSan).
 * IPC_SRC=base (default) or patched selects the ipc.c under test.
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"

#define MOVE_REQ_ID 0x2101u
#define MOVE_REQ_MAGIC 0x4d4f5645u /* "MOVE" */
#define MOVE_PING_ID 0x2102u

typedef struct {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t desc;
    uint32_t payload;
} move_msg_t;

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[16];
} ping_msg_t;

/* Build a complex message to |dest| carrying |carried| with |disp|. */
static void build_carry(move_msg_t *m, mach_port_name_t dest,
                        mach_port_name_t carried, uint8_t disp,
                        uint32_t id) {
    memset(m, 0, sizeof *m);
    m->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
                     | MACH_MSGH_BITS_COMPLEX;
    m->hdr.msgh_size = sizeof *m;
    m->hdr.msgh_remote_port = dest;
    m->hdr.msgh_local_port = MACH_PORT_NULL;
    m->hdr.msgh_voucher_port = MACH_PORT_NULL;
    m->hdr.msgh_id = id;
    m->body.msgh_descriptor_count = 1;
    m->desc.name = carried;
    m->desc.pad1 = 0;
    m->desc.pad2 = 0;
    m->desc.disposition = disp;
    m->desc.type = MACH_MSG_PORT_DESCRIPTOR;
    m->payload = MOVE_REQ_MAGIC;
}

/* ------------------------------------------------------------------ */
/* MOVE_RECEIVE: receiver gets the receive right, sender loses it.    */
/* ------------------------------------------------------------------ */

static void test_move_receive(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("move_receive");
    mach_port_t task = ipc_task_self();
    mach_port_name_t p = MACH_PORT_NULL; /* the port being moved */
    mach_port_name_t q = MACH_PORT_NULL; /* the destination port */

    CHECK_ERR(make_service_port(task, &p), KERN_SUCCESS);
    CHECK_ERR(make_service_port(task, &q), KERN_SUCCESS);
    CHECK(ipc_port_has_receive_right(p) == 1);

    move_msg_t m;
    build_carry(&m, q, p, MACH_MSG_TYPE_MOVE_RECEIVE, MOVE_REQ_ID);
    CHECK_ERR(send_only(&m, sizeof m), MACH_MSG_SUCCESS);

    /* The sender lost the receive right at send time. The entry
     * keeps its SEND right, so the name still resolves. */
    CHECK(ipc_port_has_receive_right(p) == 0);
    {
        int ptype = 0;
        CHECK_ERR(ipc_port_type(task, p, &ptype), KERN_SUCCESS);
        CHECK((ptype & MACH_PORT_TYPE_RECEIVE) == 0);
        CHECK((ptype & MACH_PORT_TYPE_SEND) != 0);
    }

    /* The receiver gets a NEW name for the receive right
     * (ipc_right_copyout allocates a fresh entry for RECEIVE). */
    uint8_t buf[1024];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, q, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    mach_port_name_t delivered = MACH_PORT_NULL;
    uint8_t delivered_disp = 0;
    if (rc == MACH_MSG_SUCCESS) {
        move_msg_t *got = (move_msg_t *)buf;
        CHECK(got->hdr.msgh_id == MOVE_REQ_ID);
        CHECK(got->payload == MOVE_REQ_MAGIC);
        CHECK((got->hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0);
        delivered = got->desc.name;
        delivered_disp = got->desc.disposition;
        CHECK(delivered != MACH_PORT_NULL);
        CHECK(delivered != p);
        CHECK(delivered_disp == MACH_MSG_TYPE_PORT_RECEIVE);
        CHECK(got->desc.type == MACH_MSG_PORT_DESCRIPTOR);
    }

    /* The delivered name really holds the receive right: give it
     * a SEND right, send a ping to it, receive the ping on it. */
    if (delivered != MACH_PORT_NULL) {
        CHECK(ipc_port_has_receive_right(delivered) == 1);
        CHECK_ERR(ipc_port_insert_right(task, delivered, delivered,
                                        MACH_MSG_TYPE_MAKE_SEND),
                  KERN_SUCCESS);
        ping_msg_t ping;
        memset(&ping, 0, sizeof ping);
        ping.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        ping.hdr.msgh_size = sizeof ping;
        ping.hdr.msgh_remote_port = delivered;
        ping.hdr.msgh_local_port = MACH_PORT_NULL;
        ping.hdr.msgh_voucher_port = MACH_PORT_NULL;
        ping.hdr.msgh_id = MOVE_PING_ID;
        ping.magic = MOVE_REQ_MAGIC;
        memcpy(ping.text, "ping", 5);
        CHECK_ERR(send_only(&ping, sizeof ping), MACH_MSG_SUCCESS);

        uint8_t pbuf[512];
        memset(pbuf, 0, sizeof pbuf);
        rc = recv_timeout(pbuf, sizeof pbuf, delivered, 1000);
        CHECK_ERR(rc, MACH_MSG_SUCCESS);
        if (rc == MACH_MSG_SUCCESS) {
            ping_msg_t *got = (ping_msg_t *)pbuf;
            CHECK(got->hdr.msgh_id == MOVE_PING_ID);
            CHECK(got->magic == MOVE_REQ_MAGIC);
        }
        CHECK_ERR(ipc_port_destroy(task, delivered), KERN_SUCCESS);
    }

    CHECK_ERR(ipc_port_destroy(task, p), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, q), KERN_SUCCESS);
    SCENARIO_END("move_receive", f0);
}

/* ------------------------------------------------------------------ */
/* M6 stale pointer: destroy the carried port while in flight.        */
/* ------------------------------------------------------------------ */

static void test_stale_pointer(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("stale_pointer_after_destroy");
    mach_port_t task = ipc_task_self();
    mach_port_name_t p = MACH_PORT_NULL; /* carried, then destroyed */
    mach_port_name_t q = MACH_PORT_NULL; /* destination */

    CHECK_ERR(make_service_port(task, &p), KERN_SUCCESS);
    CHECK_ERR(make_service_port(task, &q), KERN_SUCCESS);

    /* Carry p as COPY_SEND so the sender keeps its rights; the
     * queued message holds a raw pointer to p's struct. */
    move_msg_t m;
    build_carry(&m, q, p, MACH_MSG_TYPE_COPY_SEND, MOVE_REQ_ID);
    CHECK_ERR(send_only(&m, sizeof m), MACH_MSG_SUCCESS);

    /* Destroy p while the message is still queued on q. Destroy
     * does not scan queued messages, so the descriptor pointer
     * still points at p's (leaked, never freed) struct. */
    CHECK_ERR(ipc_port_destroy(task, p), KERN_SUCCESS);
    CHECK(ipc_port_has_receive_right(p) == 0);
    {
        int ptype = 0;
        CHECK_ERR(ipc_port_type(task, p, &ptype), KERN_INVALID_NAME);
    }

    uint8_t buf[1024];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, q, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        move_msg_t *got = (move_msg_t *)buf;
        CHECK(got->hdr.msgh_id == MOVE_REQ_ID);
        /* Correct behavior (patch item 5): no right is delivered
         * for a destroyed port, so the name must be NULL. BASE
         * delivers a fresh SEND right for the destroyed port. */
        CHECK(got->desc.name == MACH_PORT_NULL);
        if (got->desc.name != MACH_PORT_NULL) {
            printf("stale_pointer_after_destroy: NOTE base delivered "
                   "name 0x%x (disp %u) for the destroyed port\n",
                   (unsigned)got->desc.name,
                   (unsigned)got->desc.disposition);
        }
    }

    CHECK_ERR(ipc_port_destroy(task, q), KERN_SUCCESS);
    SCENARIO_END("stale_pointer_after_destroy", f0);
}

int main(void) {
    printf("ipc MOVE_RECEIVE test\n");
    ipc_init();
    harness_hook_install();
    test_move_receive();
    test_stale_pointer();
    return test_finish("move_receive");
}
