/*
 * test_special_reply.c - scenario 4: special reply port (TSD[8]).
 *
 * Gets the special reply port, destroys it, and verifies the next
 * get returns a fresh valid port. This is the M8 case: mach.c's
 * lsk_thread_get_special_reply_port caches the name in TSD[8] and
 * returns it without re-validating the receive right
 * (mach.c:80-96), and lsk_mach_port_destroy clears TSD[2] only
 * (mach.c:376-389), so against the BASE logic the second get
 * returns the stale (destroyed) name. The fix addresses both
 * halves. The TSD[8] logic runs here through tsd8_port.{c,h}, a
 * host port of those two mach.c functions (the real code uses
 * `mrs tpidr_el0` and cannot run on the x86_64 host); see that
 * file for the exact scope of the port.
 *
 * Build/run: ./build.sh (plain cc, then ASan+UBSan).
 * IPC_SRC=base (default) or patched selects the ipc.c under test
 * and the TSD8_PATCHED logic (0 for base, 1 for patched).
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "tsd8_port.h"

static void test_special_reply(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("special_reply_port");
    mach_port_t task = ipc_task_self();

    tsd8_init();

    mach_port_name_t p1 = tsd8_get_special_reply_port();
    CHECK(p1 != MACH_PORT_NULL);
    CHECK(p1 != MACH_PORT_DEAD);
    CHECK(ipc_port_has_receive_right(p1) == 1);

    /* The second get returns the cached name: TSD[8] caching works. */
    mach_port_name_t p1b = tsd8_get_special_reply_port();
    CHECK(p1b == p1);

    /* Destroy it through the mach.c destroy path (which clears
     * TSD[2] only, not TSD[8], in the base logic). */
    CHECK_ERR(tsd8_destroy_port(p1), KERN_SUCCESS);
    CHECK(ipc_port_has_receive_right(p1) == 0);
    {
        int ptype = 0;
        CHECK_ERR(ipc_port_type(task, p1, &ptype), KERN_INVALID_NAME);
    }

    /* The next get must return a fresh valid port, not the stale
     * name. BASE: returns p1 again (stale), so both checks FAIL. */
    mach_port_name_t p2 = tsd8_get_special_reply_port();
    CHECK(p2 != MACH_PORT_NULL);
    CHECK(p2 != MACH_PORT_DEAD);
    CHECK(p2 != p1);
    CHECK(ipc_port_has_receive_right(p2) == 1);
    if (p2 == p1) {
        printf("special_reply_port: NOTE base returned the stale "
               "destroyed name 0x%x\n", (unsigned)p1);
    }

    /* Clean up only if p2 is a live, distinct port. */
    if (p2 != MACH_PORT_NULL && p2 != p1) {
        CHECK_ERR(ipc_port_destroy(task, p2), KERN_SUCCESS);
    }
    SCENARIO_END("special_reply_port", f0);
}

int main(void) {
    printf("ipc special reply port test\n");
    ipc_init();
    harness_hook_install();
    test_special_reply();
    return test_finish("special_reply");
}
