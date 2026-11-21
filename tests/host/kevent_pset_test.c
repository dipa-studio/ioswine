/* kevent_pset_test: EVFILT_MACHPORT knotes on port sets.
 *
 * The defect: kevent_machport_notify matched knotes only by exact port
 * ident, so a knote registered for a port SET never fired when a message
 * arrived on a member port (XNU fires it: filt_machportprocess on a set).
 * The same gap existed at attach time: ipc_port_peek_size returned 0 for
 * a set, so a knote registered after a message was already queued never
 * fired level-triggered either.
 *
 * The test drives the REAL kevent.c + ipc.c (host build, see the build
 * command below) through the public path: allocate ports and a set with
 * ipc.c, register knotes with kevent_apply_changes, send plain messages
 * with ipc_msg, and observe workq_dispatch_kevent calls (stubbed here to
 * record delivered events). It FAILS on the unpatched tree (tests 1, 4, 6
 * and the fuzz loop) and PASSES on the patched tree.
 *
 * Build example:
 *   cc -std=gnu11 -O1 -g -w -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -DKEVENT_HOST_TEST=1 \
 *      -Ilibsystem_kernel/src -Itests/host/host-shim \
 *      -o kevent_pset_test tests/host/kevent_pset_test.c \
 *      libsystem_kernel/src/kevent.c libsystem_kernel/src/ipc.c -lpthread
 *
 * KEVENT_HOST_TEST compiles out the aarch64 poller-clone trampoline in
 * kevent.c (the poller serves timer/fd knotes only; the test drives the
 * machport notify path directly). The host-shim linux.h maps the aarch64
 * syscall numbers kevent.c/ipc.c use onto x86_64 syscall() numbers.
 *
 * No Apple code, no device needed.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "workq.h"
#include "ipc.h"

/* ------------------------------------------------------------------ */
/* Stubs for symbols the real kevent.c / ipc.c need.                  */
/* ------------------------------------------------------------------ */

/* Mach time: a counter that advances, so timeouts and deadlines behave. */
static uint64_t g_now_ticks = 1000000;
uint64_t _mach_absolute_time(void) {
    g_now_ticks += 1000;
    return g_now_ticks;
}
int _mach_timebase_info(void *info) {
    uint32_t *p = (uint32_t *)info;
    p[0] = 1;
    p[1] = 1;
    return 0;
}
uint32_t _mach_thread_self(void) {
    return 0x303; /* main thread */
}
int _mach_vm_allocate(uint32_t target, uint64_t *address, uint64_t size, int flags) {
    (void)target;
    (void)flags;
    void *p = malloc(size ? size : 1);
    if (!p) return 6; /* KERN_RESOURCE_SHORTAGE */
    *address = (uint64_t)p;
    return 0;
}
int _mach_vm_deallocate(uint32_t target, uint64_t address, uint64_t size) {
    (void)target;
    (void)size;
    free((void *)address);
    return 0;
}
static int g_snapshot_inits;
void snapshot_init(void) {
    g_snapshot_inits++;
}
static int g_carender_inits;
void carender_init(void) {
    g_carender_inits++;
}

/* Kobject service handlers (launchd, BKS, svchost, fakeboard, IOKit,
 * hidsys, notifyd) live in other LSK files that this test does not
 * compile. The test never sends a message to a service port, so these
 * stubs are never called; they exist only to satisfy the linker. Each
 * returns 0 ("not handled"). */
int launchd_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int svchost_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t max_reply_size,
                        uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size,
                      uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int hidsys_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size) {
    (void)hdr; (void)send_size; (void)reply_buf; (void)max_reply_size;
    (void)out_reply_size;
    return 0;
}
int hidsys_is_enabled(void) {
    return 0;
}
int notifyd_process_msg(const mach_msg_header_t *msg, uint32_t send_size,
                        uint8_t *reply_buf, uint32_t reply_cap,
                        uint32_t *out_reply_size) {
    (void)msg; (void)send_size; (void)reply_buf; (void)reply_cap;
    (void)out_reply_size;
    return 0;
}
void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint) {
    (void)service_port;
    (void)endpoint;
}
int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                           uint32_t *out_msgh_id) {
    (void)buf; (void)cap; (void)out_len; (void)out_msgh_id;
    return 0;
}
void set_errno(int err) {
    (void)err;
}

/* Workqueue stubs: record every dispatched kevent, deliver it. */
#define MAX_DELIVERED 64
struct delivered_ev {
    uint64_t ident;
    uint64_t data;
    uint64_t ext1;
    int16_t filter;
};
static struct delivered_ev g_delivered[MAX_DELIVERED];
static int g_delivered_count;
int workq_dispatch_kevent(uint64_t wlh, const struct kevent_qos_s *ev, int nevents, int qos) {
    (void)wlh;
    (void)qos;
    for (int i = 0; i < nevents && g_delivered_count < MAX_DELIVERED; i++) {
        g_delivered[g_delivered_count].ident = ev[i].ident;
        g_delivered[g_delivered_count].data = (uint64_t)ev[i].data;
        g_delivered[g_delivered_count].ext1 = ev[i].ext[1];
        g_delivered[g_delivered_count].filter = ev[i].filter;
        g_delivered_count++;
    }
    return 1;
}
void workq_lock(void) {}
void workq_unlock(void) {}
int workq_is_wlh_busy(uint64_t wlh) {
    (void)wlh;
    return 0;
}
int workq_set_wlh_busy(uint64_t wlh, int busy) {
    (void)wlh;
    (void)busy;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Small harness.                                                     */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;
static uint64_t g_wlh = 0x500000000ULL;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("FAIL: %s\n", what);
    }
}

static void clear_delivered(void) {
    g_delivered_count = 0;
}

/* True when a delivered event matches (ident, data, ext1). Consumes the
 * match so a duplicate delivery is not counted twice. */
static int delivered_take(uint64_t ident, uint64_t data, uint64_t ext1) {
    for (int i = 0; i < g_delivered_count; i++) {
        if (g_delivered[i].ident == ident && g_delivered[i].data == data &&
            g_delivered[i].ext1 == ext1) {
            g_delivered[i] = g_delivered[--g_delivered_count];
            return 1;
        }
    }
    return 0;
}

static int send_plain(mach_port_name_t dest, uint32_t id, uint32_t size) {
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    mach_msg_header_t *hdr = (mach_msg_header_t *)buf;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    hdr->msgh_size = size;
    hdr->msgh_remote_port = dest;
    hdr->msgh_local_port = 0;
    hdr->msgh_voucher_port = 0;
    hdr->msgh_id = id;
    return ipc_msg(buf, MACH_SEND_MSG, size, 0, 0, 0, 0, NULL, 0);
}

static int drain_one(mach_port_name_t port) {
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    ((mach_msg_header_t *)buf)->msgh_size = sizeof buf;
    return ipc_msg(buf, MACH_RCV_MSG, 0, sizeof buf, port, 0, 0, NULL, 0);
}

static int knote_add(uint64_t ident) {
    struct kevent_qos_s ch;
    struct kevent_qos_s ev[1];
    memset(&ch, 0, sizeof ch);
    ch.ident = ident;
    ch.filter = EVFILT_MACHPORT;
    ch.flags = EV_ADD | EV_ENABLE;
    ch.qos = 4;
    /* KEVENT_FLAG_ERROR_EVENTS: a failed attach comes back as one error
     * event with data = errno; success returns 0 events. */
    int n = kevent_apply_changes(g_wlh, &ch, 1, ev, 1, KEVENT_FLAG_ERROR_EVENTS);
    if (n != 0) {
        printf("knote_add ident=0x%llx failed err=%lld\n",
               (unsigned long long)ident, (long long)ev[0].data);
        return -1;
    }
    return 0;
}

static void knote_del(uint64_t ident) {
    struct kevent_qos_s ch;
    memset(&ch, 0, sizeof ch);
    ch.ident = ident;
    ch.filter = EVFILT_MACHPORT;
    ch.flags = EV_DELETE;
    kevent_apply_changes(g_wlh, &ch, 1, NULL, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Fixed scenarios.                                                   */
/* ------------------------------------------------------------------ */

static mach_port_name_t g_member1, g_member2, g_setA, g_setB;

static void setup_world(void) {
    int r;
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &g_member1);
    assert(r == 0);
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &g_member2);
    assert(r == 0);
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_PORT_SET, &g_setA);
    assert(r == 0);
    r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_PORT_SET, &g_setB);
    assert(r == 0);
    /* Send rights for both members, so plain messages can be sent to them. */
    r = ipc_port_insert_right(0x103, g_member1, g_member1, MACH_MSG_TYPE_MAKE_SEND);
    assert(r == 0);
    r = ipc_port_insert_right(0x103, g_member2, g_member2, MACH_MSG_TYPE_MAKE_SEND);
    assert(r == 0);
    r = ipc_port_insert_member(0x103, g_member1, g_setA);
    assert(r == 0);
    r = ipc_port_insert_member(0x103, g_member2, g_setB);
    assert(r == 0);
}

static void test_set_knote_fires(void) {
    /* Test 1: knote on the set, message to a member. This is the defect:
     * before the fix the set knote never fires. */
    clear_delivered();
    check(knote_add(g_setA) == 0, "t1: register set knote");
    check(send_plain(g_member1, 40231, 36) == 0, "t1: send to member");
    check(g_delivered_count == 1, "t1: exactly one event delivered");
    if (g_delivered_count == 1) {
        check(g_delivered[0].ident == g_setA, "t1: event ident is the set");
        check(g_delivered[0].data == g_setA, "t1: event data is the set");
        check(g_delivered[0].ext1 == 36, "t1: event ext[1] is the message size");
        check(g_delivered[0].filter == EVFILT_MACHPORT, "t1: event filter is MACHPORT");
    }
    check(drain_one(g_member1) == 0, "t1: drain member");
    knote_del(g_setA);
}

static void test_port_knote_still_fires(void) {
    /* Test 2: no regression on the exact-port path. */
    clear_delivered();
    check(knote_add(g_member1) == 0, "t2: register port knote");
    check(send_plain(g_member1, 40231, 36) == 0, "t2: send to member");
    check(g_delivered_count == 1, "t2: exactly one event delivered");
    if (g_delivered_count == 1) {
        check(g_delivered[0].ident == g_member1, "t2: event ident is the port");
        check(g_delivered[0].data == g_member1, "t2: event data is the port");
        check(g_delivered[0].ext1 == 36, "t2: event ext[1] is the message size");
    }
    check(drain_one(g_member1) == 0, "t2: drain member");
    knote_del(g_member1);
}

static void test_no_false_positive(void) {
    /* Test 3: a message to a port that is NOT a member of the set must not
     * fire the set knote. */
    clear_delivered();
    check(knote_add(g_setA) == 0, "t3: register set knote");
    check(send_plain(g_member2, 40231, 36) == 0, "t3: send to non-member");
    check(g_delivered_count == 0, "t3: set knote does not fire for non-member");
    check(drain_one(g_member2) == 0, "t3: drain non-member");
    knote_del(g_setA);
}

static void test_attach_time_peek(void) {
    /* Test 4: message queued BEFORE the knote is registered on the set
     * must fire level-triggered at registration (ipc_port_peek_size). */
    check(send_plain(g_member1, 40231, 36) == 0, "t4: send to member first");
    clear_delivered();
    check(knote_add(g_setA) == 0, "t4: register set knote");
    check(g_delivered_count == 1, "t4: set knote fires at attach for queued message");
    if (g_delivered_count == 1) {
        check(g_delivered[0].ident == g_setA, "t4: event ident is the set");
        check(g_delivered[0].ext1 == 36, "t4: event ext[1] is the message size");
    }
    check(drain_one(g_member1) == 0, "t4: drain member");
    knote_del(g_setA);
}

static void test_extract_member(void) {
    /* Test 5: after extracting the member from the set, the set knote
     * must stop firing for it. */
    check(ipc_port_extract_member(0x103, g_member1, g_setA) == 0, "t5: extract member");
    clear_delivered();
    check(knote_add(g_setA) == 0, "t5: register set knote");
    check(send_plain(g_member1, 40231, 36) == 0, "t5: send to ex-member");
    check(g_delivered_count == 0, "t5: set knote does not fire for ex-member");
    check(drain_one(g_member1) == 0, "t5: drain ex-member");
    knote_del(g_setA);
    check(ipc_port_insert_member(0x103, g_member1, g_setA) == 0, "t5: re-insert member");
}

static void test_second_member(void) {
    /* Test 6: a second member added to the set also fires the set knote. */
    check(ipc_port_insert_member(0x103, g_member2, g_setA) == 0, "t6: insert 2nd member");
    clear_delivered();
    check(knote_add(g_setA) == 0, "t6: register set knote");
    check(send_plain(g_member2, 40231, 48) == 0, "t6: send to 2nd member");
    check(g_delivered_count == 1, "t6: exactly one event delivered");
    if (g_delivered_count == 1) {
        check(g_delivered[0].ident == g_setA, "t6: event ident is the set");
        check(g_delivered[0].ext1 == 48, "t6: event ext[1] is the message size");
    }
    check(drain_one(g_member2) == 0, "t6: drain 2nd member");
    knote_del(g_setA);
    check(ipc_port_extract_member(0x103, g_member2, g_setA) == 0, "t6: extract 2nd member");
}

/* ------------------------------------------------------------------ */
/* Fuzz loop: random membership, knotes and sends vs a model.         */
/* ------------------------------------------------------------------ */

static uint32_t g_rng = 0x016016u;
static uint32_t rng_next(void) {
    /* xorshift32, fixed seed: deterministic across runs. */
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void fuzz_loop(void) {
    /* Fixed world, reused across iterations: 4 ports, 2 sets. Queues are
     * drained at the end of every iteration, so no iteration starts with a
     * queued message and the attach-time peek never fires mid-fuzz. */
    mach_port_name_t ports[4];
    mach_port_name_t sets[2];
    int r;
    for (int i = 0; i < 4; i++) {
        r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_RECEIVE, &ports[i]);
        assert(r == 0);
        r = ipc_port_insert_right(0x103, ports[i], ports[i], MACH_MSG_TYPE_MAKE_SEND);
        assert(r == 0);
    }
    for (int i = 0; i < 2; i++) {
        r = ipc_port_allocate(0x103, MACH_PORT_RIGHT_PORT_SET, &sets[i]);
        assert(r == 0);
    }

    const int iters = 1000;
    /* Failures from the fixed scenarios above must not cut the fuzz short:
     * break only on a NEW fuzz failure. */
    int base_failures = g_failures;
    int done = 0;
    for (int it = 0; it < iters; it++) {
        /* Random membership: each port in no set, set 0, or set 1. */
        int member_of[4];
        for (int i = 0; i < 4; i++) {
            member_of[i] = (int)(rng_next() % 3) - 1; /* -1, 0, 1 */
            for (int s = 0; s < 2; s++) {
                if (member_of[i] == s) {
                    ipc_port_insert_member(0x103, ports[i], sets[s]);
                } else {
                    ipc_port_extract_member(0x103, ports[i], sets[s]);
                }
            }
        }
        /* Random knotes on ports and sets. */
        uint64_t idents[6];
        int registered[6];
        for (int i = 0; i < 4; i++) idents[i] = ports[i];
        for (int i = 0; i < 2; i++) idents[4 + i] = sets[i];
        for (int i = 0; i < 6; i++) {
            registered[i] = (rng_next() & 1) != 0;
            if (registered[i]) {
                if (knote_add(idents[i]) != 0) registered[i] = 0;
            }
        }
        /* One random send. */
        int dest = (int)(rng_next() % 4);
        uint32_t size = 24 + (rng_next() % 41); /* 24..64 */
        uint32_t id = 40000 + (rng_next() % 1000);
        clear_delivered();
        r = send_plain(ports[dest], id, size);
        if (r != 0) {
            printf("fuzz iter %d: send failed r=%d\n", it, r);
            g_failures++;
            break;
        }
        /* Model: a registered knote fires iff its ident is the destination
         * port, or its ident is a set the destination is a member of. */
        int expected = 0;
        for (int i = 0; i < 6; i++) {
            if (!registered[i]) continue;
            int fires = 0;
            if (idents[i] == ports[dest]) {
                fires = 1;
            } else {
                for (int s = 0; s < 2; s++) {
                    if (idents[i] == sets[s] && member_of[dest] == s) fires = 1;
                }
            }
            if (fires) {
                expected++;
                if (!delivered_take(idents[i], idents[i], size)) {
                    printf("fuzz iter %d: missing event ident=0x%llx dest=0x%llx size=%u\n",
                           it, (unsigned long long)idents[i],
                           (unsigned long long)ports[dest], size);
                    g_failures++;
                }
            }
        }
        if (g_delivered_count != 0) {
            printf("fuzz iter %d: %d unexpected extra events\n", it, g_delivered_count);
            g_failures++;
        }
        if (expected != 0) {
            /* delivered_take consumed the expected events; any leftovers
             * were reported above. */
        }
        r = drain_one(ports[dest]);
        if (r != 0) {
            printf("fuzz iter %d: drain failed r=%d\n", it, r);
            g_failures++;
            break;
        }
        for (int i = 0; i < 6; i++) {
            if (registered[i]) knote_del(idents[i]);
        }
        for (int i = 0; i < 4; i++) {
            for (int s = 0; s < 2; s++) {
                ipc_port_extract_member(0x103, ports[i], sets[s]);
            }
        }
        done = it + 1;
        if (g_failures > base_failures) break;
    }
    printf("fuzz: %d/%d iterations, rng seed 0x016016\n", done, iters);
}

int main(void) {
    setup_world();
    test_set_knote_fires();
    test_port_knote_still_fires();
    test_no_false_positive();
    test_attach_time_peek();
    test_extract_member();
    test_second_member();
    fuzz_loop();
    /* The ipc lazy arm fires exactly once, at the 50th
     * MACH_SEND_MSG send, and arms both snapshot_init and
     * carender_init. The fuzz loop alone sends 1000 messages. */
    check(g_snapshot_inits == 1, "snapshot_init armed exactly once");
    check(g_carender_inits == 1, "carender_init armed exactly once");
    printf("kevent_pset_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS kevent_pset_test\n");
        return 0;
    }
    printf("FAIL kevent_pset_test\n");
    return 1;
}
