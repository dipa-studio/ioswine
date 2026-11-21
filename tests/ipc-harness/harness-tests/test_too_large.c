/*
 * test_too_large.c - scenario 5: MACH_RCV_TOO_LARGE / MACH_RCV_LARGE.
 *
 * Documents the current (BASE) receive semantics when the buffer is
 * too small for the queued message plus its trailer:
 *   - without MACH_RCV_LARGE: ipc_rcv_locked returns
 *     MACH_RCV_TOO_LARGE, sets msgh_size to the needed size, and
 *     dequeues AND DESTROYS the message.
 *   - with MACH_RCV_LARGE: it returns MACH_RCV_TOO_LARGE and sets
 *     msgh_size the same way, but the message stays queued, so a
 *     later receive with a large enough buffer still gets it.
 * Both are expected to PASS against the BASE ipc.c; the test pins
 * the behavior down. Whether XNU consumes the message under
 * MACH_RCV_LARGE (partial copy) instead of preserving it is not
 * verified here; if the implementation
 * changes it, this test must be updated to match.
 *
 * The trailer is 8 bytes for a plain receive (ipc_get_trailer_size
 * returns 8 when option bits 24-27 are 0), so a 64-byte message
 * needs 72 bytes and a 32-byte buffer is too small.
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

#define BIG_MSG_ID 0x2201u
#define BIG_MSG_MAGIC 0x42494721u /* "BIG!" */
#define BIG_MSG_SIZE 64u
#define BIG_MSG_NEEDED (BIG_MSG_SIZE + 8u) /* message + 8-byte trailer */
#define SMALL_RCV_SIZE 32u

typedef struct {
    mach_msg_header_t hdr; /* 24 */
    uint32_t magic;        /* 4 */
    char text[36];         /* 36: total 64 */
} big_msg_t;

static void build_big(big_msg_t *m, mach_port_name_t dest, uint32_t id) {
    memset(m, 0, sizeof *m);
    m->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m->hdr.msgh_size = sizeof *m;
    m->hdr.msgh_remote_port = dest;
    m->hdr.msgh_local_port = MACH_PORT_NULL;
    m->hdr.msgh_voucher_port = MACH_PORT_NULL;
    m->hdr.msgh_id = id;
    m->magic = BIG_MSG_MAGIC;
    memcpy(m->text, "too-large-probe", 16);
}

/* ------------------------------------------------------------------ */
/* Without LARGE: TOO_LARGE destroys the message.                     */
/* ------------------------------------------------------------------ */

static void test_too_large_no_large(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("too_large_without_large");
    mach_port_t task = ipc_task_self();
    mach_port_name_t s = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);

    big_msg_t m;
    build_big(&m, s, BIG_MSG_ID);
    CHECK(sizeof m == BIG_MSG_SIZE);
    CHECK_ERR(send_only(&m, sizeof m), MACH_MSG_SUCCESS);

    uint8_t small[SMALL_RCV_SIZE];
    memset(small, 0, sizeof small);
    int rc = ipc_msg(small, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                     SMALL_RCV_SIZE, s, 1000, 0, NULL, 0);
    CHECK_ERR(rc, MACH_RCV_TOO_LARGE);
    /* The needed size is reported in the header. */
    CHECK(((mach_msg_header_t *)small)->msgh_size == BIG_MSG_NEEDED);

    /* The message was destroyed: a later receive times out. */
    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    rc = recv_timeout(buf, sizeof buf, s, 200);
    CHECK_ERR(rc, MACH_RCV_TIMED_OUT);

    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);
    SCENARIO_END("too_large_without_large", f0);
}

/* ------------------------------------------------------------------ */
/* With LARGE: TOO_LARGE preserves the message.                       */
/* ------------------------------------------------------------------ */

static void test_too_large_with_large(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("too_large_with_large");
    mach_port_t task = ipc_task_self();
    mach_port_name_t s = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &s), KERN_SUCCESS);

    big_msg_t m;
    build_big(&m, s, BIG_MSG_ID);
    CHECK_ERR(send_only(&m, sizeof m), MACH_MSG_SUCCESS);

    uint8_t small[SMALL_RCV_SIZE];
    memset(small, 0, sizeof small);
    int rc = ipc_msg(small, MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE,
                     0, SMALL_RCV_SIZE, s, 1000, 0, NULL, 0);
    CHECK_ERR(rc, MACH_RCV_TOO_LARGE);
    CHECK(((mach_msg_header_t *)small)->msgh_size == BIG_MSG_NEEDED);

    /* The message was preserved: a later receive with a big buffer
     * gets the whole message. */
    uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    rc = recv_timeout(buf, sizeof buf, s, 1000);
    CHECK_ERR(rc, MACH_MSG_SUCCESS);
    if (rc == MACH_MSG_SUCCESS) {
        big_msg_t *got = (big_msg_t *)buf;
        CHECK(got->hdr.msgh_id == BIG_MSG_ID);
        CHECK(got->magic == BIG_MSG_MAGIC);
        CHECK(memcmp(got->text, "too-large-probe", 16) == 0);
    }

    CHECK_ERR(ipc_port_destroy(task, s), KERN_SUCCESS);
    SCENARIO_END("too_large_with_large", f0);
}

int main(void) {
    printf("ipc MACH_RCV_TOO_LARGE test\n");
    ipc_init();
    harness_hook_install();
    test_too_large_no_large();
    test_too_large_with_large();
    return test_finish("too_large");
}
