/*
 * test_notifications.c - scenario 1: notifications.
 *
 * Covers dead-name, no-senders (deferred and immediate), send-once
 * and port-destroyed notifications. For each, the test verifies two
 * things: (a) the notification message is enqueued on the notify
 * port (the enqueue code exists in base ipc.c), and (b) the mock
 * enqueue hook fired for it (hook log in harness/hook.h).
 *
 * Against the BASE ipc.c the (a) checks pass and the (b) checks
 * FAIL: the audit confirms no hook call at any notification site
 * (ipc.c:282-309 send-once, 830-855 dead-name in deallocate,
 * 902-935 no-senders in deallocate, 960-990 dead-name in destroy,
 * 1040 no-senders in mod_refs, 1558-1589 immediate no-senders).
 * That is the expected-before-patch result; adding the hook at those sites
 * allows (b) to pass.
 *
 * Port-destroyed is different: base ipc.c has no delivery site for
 * MACH_NOTIFY_PORT_DESTROYED at all (registration is accepted but
 * stores nothing), and it is not in the patch list. The test
 * documents that no notification arrives.
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

#define NOTIFY_REQ_ID 0x2001u
#define NOTIFY_REQ_MAGIC 0x4e4f5446u /* "NOTF" */

/* ------------------------------------------------------------------ */
/* dead-name: register, destroy the port, notification must arrive.   */
/* ------------------------------------------------------------------ */

static void test_dead_name(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("dead_name");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n = MACH_PORT_NULL;   /* notify port */
    mach_port_name_t s = MACH_PORT_NULL;   /* service port */
    mach_port_t prev = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);
    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);

    CHECK_ERR(ipc_port_request_notification(task, s, MACH_NOTIFY_DEAD_NAME,
                                            0, n, MACH_MSG_TYPE_COPY_SEND,
                                            &prev),
              KERN_SUCCESS);
    CHECK(prev == MACH_PORT_NULL);

    /* Clear the hook log so only the destroy is observed. */
    harness_hook_clear();

    /* Destroying the receive right delivers the dead-name
     * notification to the notify port. */
    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);

    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, n, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        mach_dead_name_notification_t *notif = (mach_dead_name_notification_t *)buf;
        CHECK(notif->not_header.msgh_id == MACH_NOTIFY_DEAD_NAME);
        CHECK(notif->not_header.msgh_size == sizeof(mach_dead_name_notification_t));
        CHECK(notif->not_port == s);
        /* Notifications get a NULL local port (ipc.c:2345-2351). */
        CHECK(notif->not_header.msgh_local_port == MACH_PORT_NULL);
    }

    /* The hook must have fired for the notification enqueue.
     * BASE: no hook at the destroy site, so this FAILS. */
    CHECK(harness_hook_find(n, (uint32_t)sizeof(mach_dead_name_notification_t)) >= 0);

    CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);
    SCENARIO_END("dead_name", f0);
}

/* ------------------------------------------------------------------ */
/* no-senders, deferred: register, drop the last send right.          */
/* ------------------------------------------------------------------ */

static void test_no_senders_deferred(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("no_senders_deferred");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n = MACH_PORT_NULL;
    mach_port_name_t s = MACH_PORT_NULL;
    mach_port_t prev = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);
    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);

    /* Pin the mscount so the notification's not_count is exact:
     * the MAKE_SEND insert already bumped it to 1, so set it to a
     * distinctive value instead of assuming 0. */
    CHECK_ERR(ipc_port_set_mscount(task, s, 7), KERN_SUCCESS);

    CHECK_ERR(ipc_port_request_notification(task, s, MACH_NOTIFY_NO_SENDERS,
                                            0, n, MACH_MSG_TYPE_COPY_SEND,
                                            &prev),
              KERN_SUCCESS);
    CHECK(prev == MACH_PORT_NULL);

    harness_hook_clear();

    /* Drop the only send reference: urefs 1 -> 0, srights 1 -> 0,
     * so the no-senders notification is delivered to n. The
     * receive right stays (the entry keeps RECEIVE). */
    CHECK_ERR(ipc_port_deallocate(task, s), KERN_SUCCESS);
    CHECK(ipc_port_has_receive_right(s) == 1);

    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, n, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        mach_no_senders_notification_t *notif = (mach_no_senders_notification_t *)buf;
        CHECK(notif->not_header.msgh_id == MACH_NOTIFY_NO_SENDERS);
        CHECK(notif->not_header.msgh_size == sizeof(mach_no_senders_notification_t));
        CHECK(notif->not_count == 7);
        CHECK(notif->not_header.msgh_local_port == MACH_PORT_NULL);
    }

    /* BASE: no hook at the deallocate site, so this FAILS. */
    CHECK(harness_hook_find(n, (uint32_t)sizeof(mach_no_senders_notification_t)) >= 0);

    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);
    SCENARIO_END("no_senders_deferred", f0);
}

/* ------------------------------------------------------------------ */
/* no-senders, immediate: register on a port with no send rights.     */
/* ------------------------------------------------------------------ */

static void test_no_senders_immediate(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("no_senders_immediate");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n = MACH_PORT_NULL;
    mach_port_name_t s = MACH_PORT_NULL; /* receive-only: srights == 0 */
    mach_port_t prev = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);
    CHECK_ERR(make_recv_port(task, &s), KERN_SUCCESS);
    CHECK(ipc_port_has_receive_right(s) == 1);
    CHECK_ERR(ipc_port_set_mscount(task, s, 5), KERN_SUCCESS);

    harness_hook_clear();

    /* srights is already 0, so registration delivers immediately. */
    CHECK_ERR(ipc_port_request_notification(task, s, MACH_NOTIFY_NO_SENDERS,
                                            0, n, MACH_MSG_TYPE_COPY_SEND,
                                            &prev),
              KERN_SUCCESS);

    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, n, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        mach_no_senders_notification_t *notif = (mach_no_senders_notification_t *)buf;
        CHECK(notif->not_header.msgh_id == MACH_NOTIFY_NO_SENDERS);
        CHECK(notif->not_header.msgh_size == sizeof(mach_no_senders_notification_t));
        CHECK(notif->not_count == 5);
    }

    /* BASE: no hook at the immediate-delivery site, so this FAILS. */
    CHECK(harness_hook_find(n, (uint32_t)sizeof(mach_no_senders_notification_t)) >= 0);

    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);
    SCENARIO_END("no_senders_immediate", f0);
}

/* ------------------------------------------------------------------ */
/* send-once: carry a send-once reply port, deallocate the right.     */
/* ------------------------------------------------------------------ */

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[32];
} notify_req_t;

static void test_send_once(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("send_once");
    mach_port_t task = ipc_task_self();
    mach_port_name_t s = MACH_PORT_NULL;   /* service port */
    mach_port_name_t r = MACH_PORT_NULL;   /* reply port (notify dest) */

    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);
    r = ipc_mach_reply_port();
    CHECK(r != MACH_PORT_NULL);

    /* Send a request carrying the reply port as send-once. */
    notify_req_t req;
    memset(&req, 0, sizeof req);
    req.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                       MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.hdr.msgh_size = sizeof req;
    req.hdr.msgh_remote_port = s;
    req.hdr.msgh_local_port = r;
    req.hdr.msgh_voucher_port = MACH_PORT_NULL;
    req.hdr.msgh_id = NOTIFY_REQ_ID;
    req.magic = NOTIFY_REQ_MAGIC;
    memcpy(req.text, "send-once", 10);
    CHECK_ERR(send_only(&req, sizeof req), MACH_MSG_SUCCESS);

    /* Receive the request on s (single-threaded: the send already
     * enqueued it). The remote port is the send-once right. */
    uint8_t rbuf[1024];
    memset(rbuf, 0, sizeof rbuf);
    int rc = recv_timeout(rbuf, sizeof rbuf, s, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    mach_port_name_t so = MACH_PORT_NULL;
    if (rc == MACH_MSG_SUCCESS) {
        notify_req_t *got = (notify_req_t *)rbuf;
        CHECK(got->hdr.msgh_id == NOTIFY_REQ_ID);
        CHECK(got->magic == NOTIFY_REQ_MAGIC);
        so = got->hdr.msgh_remote_port;
        CHECK(so != MACH_PORT_NULL);
        CHECK(so != r);
        int ptype = 0;
        CHECK_ERR(ipc_port_type(task, so, &ptype), KERN_SUCCESS);
        CHECK((ptype & MACH_PORT_TYPE_SEND_ONCE) != 0);
    }

    harness_hook_clear();

    /* Deallocating the send-once right notifies the reply port. */
    CHECK_ERR(ipc_port_deallocate(task, so), KERN_SUCCESS);

    uint8_t nbuf[512];
    memset(nbuf, 0, sizeof nbuf);
    rc = recv_timeout(nbuf, sizeof nbuf, r, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        mach_msg_header_t *notif = (mach_msg_header_t *)nbuf;
        CHECK(notif->msgh_id == MACH_NOTIFY_SEND_ONCE);
        CHECK(notif->msgh_size == sizeof(mach_msg_header_t));
        CHECK(notif->msgh_local_port == MACH_PORT_NULL);
    }

    /* BASE: no hook at the send-once notify site, so this FAILS. */
    CHECK(harness_hook_find(r, (uint32_t)sizeof(mach_msg_header_t)) >= 0);

    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, r), KERN_SUCCESS);
    SCENARIO_END("send_once", f0);
}

/* ------------------------------------------------------------------ */
/* port-destroyed: registration is accepted but nothing is delivered. */
/* ------------------------------------------------------------------ */

static void test_port_destroyed(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("port_destroyed");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n = MACH_PORT_NULL;
    mach_port_name_t s = MACH_PORT_NULL;
    mach_port_t prev = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);
    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);

    /* Registration returns success but stores nothing (the else
     * branch in ipc_port_request_notification). */
    CHECK_ERR(ipc_port_request_notification(task, s, MACH_NOTIFY_PORT_DESTROYED,
                                            0, n, MACH_MSG_TYPE_COPY_SEND,
                                            &prev),
              KERN_SUCCESS);
    CHECK(prev == MACH_PORT_NULL);

    harness_hook_clear();
    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);

    /* No delivery site exists for PORT_DESTROYED, so nothing
     * arrives. This documents the current behavior; it is not in
     * the patch list, so the patched ipc.c is expected to behave
     * the same way. */
    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    int rc = recv_timeout(buf, sizeof buf, n, 200);
    CHECK_ERR(rc, MACH_RCV_TIMED_OUT);
    printf("port_destroyed: NOTE no delivery site in base ipc.c; "
           "not in the patch list\n");

    CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);
    SCENARIO_END("port_destroyed", f0);
}

int main(void) {
    printf("ipc notifications test\n");
    ipc_init();
    harness_hook_install();
    test_dead_name();
    test_no_senders_deferred();
    test_no_senders_immediate();
    test_send_once();
    test_port_destroyed();
    return test_finish("notifications");
}
