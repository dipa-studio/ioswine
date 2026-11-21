/*
 * test_sync_send.c - scenario 6: sync send+wait with a second thread.
 *
 * A sender thread does a single ipc_msg with MACH_SEND_MSG |
 * MACH_RCV_MSG (a sync send) and blocks for the reply; a second
 * thread receives the request and replies after a delay. Verifies
 * that the sender unblocks with the reply, and that it does NOT
 * return MACH_RCV_PORT_DIED spuriously while the reply is in
 * flight (the M1 scenario). The blocking-receive fix is already in
 * the tree (ipc.c:2449-2452, the sorights-based early return was
 * removed), so this is expected to PASS against the BASE ipc.c; if
 * it does not, that is a real finding, not an expected failure.
 *
 * Every blocking call carries a timeout so a broken wake-up fails
 * the test instead of hanging the run.
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
#include <pthread.h>
#include "common.h"

#define SYNC_REQ_ID 0x2301u
#define SYNC_REP_ID 0x2302u
#define SYNC_REQ_MAGIC 0x53594e43u /* "SYNC" */
#define SYNC_REP_MAGIC 0x5253504eu /* "RSPN" */
#define REPLY_DELAY_MS 300
#define STILL_BLOCKED_CHECK_MS 150

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[32];
} sync_req_t;

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[32];
} sync_rep_t;

typedef struct {
    mach_port_name_t srv;
    mach_port_name_t reply;
    pthread_mutex_t mu;
    int done;
    int rc;
    uint32_t got_id;
    uint32_t got_magic;
    char got_text[32];
} sender_ctx_t;

typedef struct {
    mach_port_name_t srv;
    pthread_mutex_t mu;
    int done;
    int rcv_rc;
    int rep_rc;
    uint32_t got_id;
} receiver_ctx_t;

static void *sender_main(void *arg) {
    sender_ctx_t *ctx = (sender_ctx_t *)arg;

    sync_req_t req;
    memset(&req, 0, sizeof req);
    req.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                       MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.hdr.msgh_size = sizeof req;
    req.hdr.msgh_remote_port = ctx->srv;
    req.hdr.msgh_local_port = ctx->reply;
    req.hdr.msgh_voucher_port = MACH_PORT_NULL;
    req.hdr.msgh_id = SYNC_REQ_ID;
    req.magic = SYNC_REQ_MAGIC;
    memcpy(req.text, "sync-request", 13);

    /* One call: send the request, then wait for the reply on the
     * reply port. The reply lands in rbuf (via the scatter
     * parameter) so the request buffer is not clobbered. */
    uint8_t rbuf[1024];
    memset(rbuf, 0, sizeof rbuf);
    int rc = ipc_msg(&req, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                     sizeof req, 0, ctx->reply, 5000, 0,
                     rbuf, sizeof rbuf);

    pthread_mutex_lock(&ctx->mu);
    ctx->rc = rc;
    if (rc == MACH_MSG_SUCCESS) {
        sync_rep_t *rep = (sync_rep_t *)rbuf;
        ctx->got_id = rep->hdr.msgh_id;
        ctx->got_magic = rep->magic;
        memcpy(ctx->got_text, rep->text, sizeof ctx->got_text);
    }
    ctx->done = 1;
    pthread_mutex_unlock(&ctx->mu);
    return NULL;
}

static void *receiver_main(void *arg) {
    receiver_ctx_t *ctx = (receiver_ctx_t *)arg;
    uint8_t buf[1024];
    memset(buf, 0, sizeof buf);

    int rc = recv_timeout(buf, sizeof buf, ctx->srv, 5000);
    ctx->rcv_rc = rc;
    if (rc == MACH_MSG_SUCCESS) {
        sync_req_t *req = (sync_req_t *)buf;
        ctx->got_id = req->hdr.msgh_id;

        /* Delay before replying: the sender must stay blocked
         * while the reply is in flight (the M1 check). */
        test_msleep(REPLY_DELAY_MS);

        sync_rep_t rep;
        memset(&rep, 0, sizeof rep);
        rep.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep.hdr.msgh_size = sizeof rep;
        rep.hdr.msgh_remote_port = req->hdr.msgh_remote_port;
        rep.hdr.msgh_local_port = MACH_PORT_NULL;
        rep.hdr.msgh_voucher_port = MACH_PORT_NULL;
        rep.hdr.msgh_id = SYNC_REP_ID;
        rep.magic = SYNC_REP_MAGIC;
        memcpy(rep.text, "sync-reply", 11);
        ctx->rep_rc = send_only(&rep, sizeof rep);
    }

    pthread_mutex_lock(&ctx->mu);
    ctx->done = 1;
    pthread_mutex_unlock(&ctx->mu);
    return NULL;
}

static int wait_done(pthread_mutex_t *mu, int *done, long timeout_ms) {
    long waited = 0;
    while (waited < timeout_ms) {
        pthread_mutex_lock(mu);
        int d = *done;
        pthread_mutex_unlock(mu);
        if (d) return 1;
        test_msleep(50);
        waited += 50;
    }
    return 0;
}

static void test_sync_send_wait(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("sync_send_wait");
    mach_port_t task = ipc_task_self();
    mach_port_name_t srv = MACH_PORT_NULL;
    mach_port_name_t reply = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &srv), KERN_SUCCESS);
    reply = ipc_mach_reply_port();
    CHECK(reply != MACH_PORT_NULL);

    sender_ctx_t sctx;
    memset(&sctx, 0, sizeof sctx);
    sctx.srv = srv;
    sctx.reply = reply;
    pthread_mutex_init(&sctx.mu, NULL);

    receiver_ctx_t rctx;
    memset(&rctx, 0, sizeof rctx);
    rctx.srv = srv;
    pthread_mutex_init(&rctx.mu, NULL);

    pthread_t sth, rth;
    CHECK(pthread_create(&rth, NULL, receiver_main, &rctx) == 0);
    CHECK(pthread_create(&sth, NULL, sender_main, &sctx) == 0);

    /* The receiver replies about 300 ms after it gets the request.
     * At 150 ms the sender must still be blocked (it must not have
     * returned MACH_RCV_PORT_DIED while the reply is in flight). */
    test_msleep(STILL_BLOCKED_CHECK_MS);
    pthread_mutex_lock(&sctx.mu);
    int still_blocked = !sctx.done;
    pthread_mutex_unlock(&sctx.mu);
    CHECK(still_blocked);
    if (!still_blocked) {
        printf("sync_send_wait: NOTE sender returned early (rc=0x%x)\n",
               (unsigned)sctx.rc);
    }

    /* Bounded wait for both threads (their own timeouts bound them
     * to about 5 s, so this cannot hang the run). */
    CHECK(wait_done(&sctx.mu, &sctx.done, 8000) == 1);
    CHECK(wait_done(&rctx.mu, &rctx.done, 8000) == 1);
    pthread_join(sth, NULL);
    pthread_join(rth, NULL);

    CHECK(rctx.rcv_rc == MACH_MSG_SUCCESS);
    CHECK(rctx.got_id == SYNC_REQ_ID);
    CHECK(rctx.rep_rc == MACH_MSG_SUCCESS);

    /* The sender must have unblocked with the reply, not with a
     * spurious PORT_DIED. */
    CHECK(sctx.rc == MACH_MSG_SUCCESS);
    CHECK(sctx.rc != MACH_RCV_PORT_DIED);
    CHECK(sctx.got_id == SYNC_REP_ID);
    CHECK(sctx.got_magic == SYNC_REP_MAGIC);
    CHECK(memcmp(sctx.got_text, "sync-reply", 11) == 0);

    CHECK_ERR(ipc_port_destroy(task, srv), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, reply), KERN_SUCCESS);
    pthread_mutex_destroy(&sctx.mu);
    pthread_mutex_destroy(&rctx.mu);
    SCENARIO_END("sync_send_wait", f0);
}

int main(void) {
    printf("ipc sync send+wait test\n");
    ipc_init();
    harness_hook_install();
    test_sync_send_wait();
    return test_finish("sync_send");
}
