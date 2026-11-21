/*
 * harness_smoke.c - smoke test for the ipc.c host harness.
 *
 * Proves the harness works end to end on the host:
 *  1. round_trip: a receiver thread blocks in ipc_msg on a plain
 *     receive port; the main thread sends it a request carrying a
 *     send-once reply port; the receiver answers on the send-once
 *     right and the main thread receives the reply. Checks the
 *     message contents both ways, that the receiver really blocked
 *     (still not done 200 ms after it started waiting, with no
 *     message queued), and that the enqueue hook logged both
 *     enqueues with the right port and size.
 *  2. receive_timeout: receiving on an empty port with
 *     MACH_RCV_TIMEOUT returns MACH_RCV_TIMED_OUT after about the
 *     requested time instead of hanging.
 *
 * The port dance follows the device tests (tests/libsys/mach_test.c
 * in the work dir): allocate a RECEIVE right, then insert a SEND
 * right on the same name with MACH_MSG_TYPE_MAKE_SEND. The reply
 * port comes from ipc_mach_reply_port() and is carried as the
 * local port with MACH_MSG_TYPE_MAKE_SEND_ONCE, exactly like
 * libdispatch's send_and_wait_for_reply.
 *
 * Build/run: ./build.sh (plain cc, then ASan+UBSan).
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"
#include "hook.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_ERR(call, want) do { \
    int _rc = (call); \
    g_checks++; \
    if (_rc != (want)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s returned %d (0x%x), want %d (0x%x)\n", \
               __FILE__, __LINE__, #call, _rc, (unsigned)_rc, \
               (want), (unsigned)(want)); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Messages.                                                           */
/* ------------------------------------------------------------------ */

#define SMOKE_REQ_ID 0x1234u
#define SMOKE_REP_ID 0x1235u
#define SMOKE_REQ_MAGIC 0x51554552u /* "REQU" */
#define SMOKE_REP_MAGIC 0x52505245u /* "REPR" */

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[32];
} smoke_req_t;

typedef struct {
    mach_msg_header_t hdr;
    uint32_t magic;
    char text[32];
} smoke_rep_t;

/* ------------------------------------------------------------------ */
/* Round-trip test (two threads).                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    mach_port_name_t srv;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int waiting; /* set just before entering ipc_msg */
    int done;    /* set after the reply send returns */
    int rcv_rc;  /* ipc_msg receive return */
    int rep_rc;  /* ipc_msg reply-send return */
    uint32_t got_id;
    uint32_t got_magic;
    char got_text[32];
    mach_port_name_t got_local;  /* msgh_local_port seen by receiver */
    mach_port_name_t reply_dest; /* send-once name the reply went to */
} receiver_ctx_t;

static void *receiver_main(void *arg) {
    receiver_ctx_t *ctx = (receiver_ctx_t *)arg;
    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);

    pthread_mutex_lock(&ctx->mu);
    ctx->waiting = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mu);

    int rc = ipc_msg(buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(buf),
                     ctx->srv, 5000, 0, NULL, 0);
    ctx->rcv_rc = rc;
    if (rc == MACH_MSG_SUCCESS) {
        smoke_req_t *req = (smoke_req_t *)buf;
        ctx->got_id = req->hdr.msgh_id;
        ctx->got_magic = req->magic;
        memcpy(ctx->got_text, req->text, sizeof ctx->got_text);
        ctx->got_local = req->hdr.msgh_local_port;
        ctx->reply_dest = req->hdr.msgh_remote_port;

        smoke_rep_t rep;
        memset(&rep, 0, sizeof rep);
        rep.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep.hdr.msgh_size = sizeof rep;
        rep.hdr.msgh_remote_port = req->hdr.msgh_remote_port;
        rep.hdr.msgh_local_port = MACH_PORT_NULL;
        rep.hdr.msgh_voucher_port = MACH_PORT_NULL;
        rep.hdr.msgh_id = SMOKE_REP_ID;
        rep.magic = SMOKE_REP_MAGIC;
        memcpy(rep.text, "reply-ok", 9);
        ctx->rep_rc = ipc_msg(&rep, MACH_SEND_MSG, sizeof rep, 0, 0, 0, 0, NULL, 0);
    }

    pthread_mutex_lock(&ctx->mu);
    ctx->done = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mu);
    return NULL;
}

static void test_round_trip(void) {
    printf("--- round_trip ---\n");
    mach_port_t task = ipc_task_self();
    mach_port_name_t srv = MACH_PORT_NULL;
    mach_port_name_t reply = MACH_PORT_NULL;

    ipc_init();
    harness_hook_install();

    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &srv), KERN_SUCCESS);
    CHECK(srv != MACH_PORT_NULL);
    CHECK_ERR(ipc_port_insert_right(task, srv, srv, MACH_MSG_TYPE_MAKE_SEND), KERN_SUCCESS);

    reply = ipc_mach_reply_port();
    CHECK(reply != MACH_PORT_NULL);

    receiver_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.srv = srv;
    pthread_mutex_init(&ctx.mu, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    pthread_t th;
    CHECK(pthread_create(&th, NULL, receiver_main, &ctx) == 0);

    /* Wait until the receiver is inside (or about to enter) ipc_msg. */
    pthread_mutex_lock(&ctx.mu);
    while (!ctx.waiting) pthread_cond_wait(&ctx.cond, &ctx.mu);
    pthread_mutex_unlock(&ctx.mu);

    /* Prove the receiver blocks with no message: 200 ms later it
     * must still not be done. */
    struct timespec nap = { 0, 200 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    pthread_mutex_lock(&ctx.mu);
    int still_blocked = !ctx.done;
    pthread_mutex_unlock(&ctx.mu);
    CHECK(still_blocked);

    /* Send the request, carrying the reply port as send-once. */
    smoke_req_t req;
    memset(&req, 0, sizeof req);
    req.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.hdr.msgh_size = sizeof req;
    req.hdr.msgh_remote_port = srv;
    req.hdr.msgh_local_port = reply;
    req.hdr.msgh_voucher_port = MACH_PORT_NULL;
    req.hdr.msgh_id = SMOKE_REQ_ID;
    req.magic = SMOKE_REQ_MAGIC;
    memcpy(req.text, "hello-ipc", 10);
    CHECK_ERR(ipc_msg(&req, MACH_SEND_MSG, sizeof req, 0, 0, 0, 0, NULL, 0),
              MACH_MSG_SUCCESS);

    /* Receive the reply on the reply port. */
    uint8_t rbuf[4096];
    memset(rbuf, 0, sizeof rbuf);
    int rc = ipc_msg(rbuf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(rbuf),
                     reply, 5000, 0, NULL, 0);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        smoke_rep_t *rep = (smoke_rep_t *)rbuf;
        CHECK(rep->hdr.msgh_id == SMOKE_REP_ID);
        CHECK(rep->magic == SMOKE_REP_MAGIC);
        CHECK(memcmp(rep->text, "reply-ok", 9) == 0);
        CHECK(rep->hdr.msgh_local_port == reply);
    }

    pthread_join(th, NULL);
    CHECK(ctx.rcv_rc == MACH_MSG_SUCCESS);
    CHECK(ctx.rep_rc == MACH_MSG_SUCCESS);
    CHECK(ctx.got_id == SMOKE_REQ_ID);
    CHECK(ctx.got_magic == SMOKE_REQ_MAGIC);
    CHECK(memcmp(ctx.got_text, "hello-ipc", 10) == 0);
    CHECK(ctx.got_local == srv);
    CHECK(ctx.reply_dest != MACH_PORT_NULL);

    /* Hook log: one enqueue for the request (dest srv), one for the
     * reply (dest is the send-once name the receiver used). */
    CHECK(harness_hook_count() >= 2);
    CHECK(harness_hook_find(srv, sizeof(req)) >= 0);
    CHECK(harness_hook_find(ctx.reply_dest, sizeof(smoke_rep_t)) >= 0);
    CHECK(harness_hook_dropped() == 0);

    CHECK_ERR(ipc_port_destroy(task, srv), KERN_SUCCESS);
    CHECK_ERR(ipc_port_destroy(task, reply), KERN_SUCCESS);
    pthread_mutex_destroy(&ctx.mu);
    pthread_cond_destroy(&ctx.cond);
}

/* ------------------------------------------------------------------ */
/* Receive-timeout test (proves blocking returns, not hangs).          */
/* ------------------------------------------------------------------ */

static void test_receive_timeout(void) {
    printf("--- receive_timeout ---\n");
    mach_port_t task = ipc_task_self();
    mach_port_name_t empty = MACH_PORT_NULL;
    CHECK_ERR(ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &empty), KERN_SUCCESS);
    CHECK(empty != MACH_PORT_NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t buf[256];
    int rc = ipc_msg(buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(buf),
                     empty, 100, 0, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK_ERR(rc, MACH_RCV_TIMED_OUT);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("timeout test: elapsed %ld ms (want about 100)\n", elapsed_ms);
    CHECK(elapsed_ms >= 50);
    CHECK(elapsed_ms < 5000);

    CHECK_ERR(ipc_port_destroy(task, empty), KERN_SUCCESS);
}

int main(void) {
    printf("ipc harness smoke test\n");
    test_round_trip();
    test_receive_timeout();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
