/*
 * test_port_names.c - scenario 3: port name reuse / uniqueness.
 *
 * Verifies that destroying a port and allocating a new one yields a
 * different name (the generation bump, ipc.c:477, enforced on lookup
 * at ipc.c:459) and that the old name no longer resolves. Also
 * verifies that a run of allocate/destroy cycles never repeats a
 * name. This is expected to PASS against the BASE ipc.c: the
 * generation bump is confirmed present by the audit.
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

#define NAME_CYCLES 50

/* ------------------------------------------------------------------ */
/* Destroy then allocate: new name differs, old name is dead.         */
/* ------------------------------------------------------------------ */

static void test_name_reuse(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("name_reuse");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n1 = MACH_PORT_NULL;
    mach_port_name_t n2 = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n1), KERN_SUCCESS);
    CHECK(n1 != MACH_PORT_NULL);
    CHECK(ipc_port_has_receive_right(n1) == 1);

    CHECK_ERR(ipc_port_destroy(task, n1), KERN_SUCCESS);

    /* The old name no longer resolves. */
    {
        int ptype = 0;
        CHECK_ERR(ipc_port_type(task, n1, &ptype), KERN_INVALID_NAME);
    }
    CHECK(ipc_port_has_receive_right(n1) == 0);

    CHECK_ERR(make_service_port(task, &n2), KERN_SUCCESS);
    CHECK(n2 != MACH_PORT_NULL);
    /* Generation bump: same slot may be reused, but the name (which
     * carries the generation in its low byte) must differ. */
    CHECK(n2 != n1);
    CHECK(ipc_port_has_receive_right(n2) == 1);

    CHECK_ERR(ipc_port_destroy(task, n2), KERN_SUCCESS);
    SCENARIO_END("name_reuse", f0);
}

/* ------------------------------------------------------------------ */
/* Many allocate/destroy cycles: every name is unique.                 */
/* ------------------------------------------------------------------ */

static void test_name_uniqueness(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("name_uniqueness");
    mach_port_t task = ipc_task_self();
    mach_port_name_t seen[NAME_CYCLES];

    for (int i = 0; i < NAME_CYCLES; i++) {
        mach_port_name_t n = MACH_PORT_NULL;
        CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);
        CHECK(n != MACH_PORT_NULL);
        seen[i] = n;
        /* No repeat of any earlier name in this run. */
        for (int j = 0; j < i; j++) {
            CHECK(seen[j] != n);
        }
        CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);
        /* Destroyed names stay dead even after later allocations. */
        {
            int ptype = 0;
            CHECK_ERR(ipc_port_type(task, n, &ptype), KERN_INVALID_NAME);
        }
    }

    SCENARIO_END("name_uniqueness", f0);
}

/* ------------------------------------------------------------------ */
/* ipc_port_names lists live ports and drops destroyed ones.          */
/* ------------------------------------------------------------------ */

static void test_port_names_list(void) {
    int f0 = g_failures;
    SCENARIO_BEGIN("port_names_list");
    mach_port_t task = ipc_task_self();
    mach_port_name_t n = MACH_PORT_NULL;

    CHECK_ERR(make_service_port(task, &n), KERN_SUCCESS);

    mach_port_name_t *names = NULL;
    uint32_t names_cnt = 0;
    mach_port_type_t *types = NULL;
    uint32_t types_cnt = 0;
    CHECK_ERR(ipc_port_names(task, &names, &names_cnt, &types, &types_cnt),
              KERN_SUCCESS);
    {
        int found = 0;
        for (uint32_t i = 0; i < names_cnt; i++) {
            if (names[i] == n) found = 1;
        }
        CHECK(found == 1);
    }

    CHECK_ERR(ipc_port_destroy(task, n), KERN_SUCCESS);

    names = NULL;
    names_cnt = 0;
    types = NULL;
    types_cnt = 0;
    CHECK_ERR(ipc_port_names(task, &names, &names_cnt, &types, &types_cnt),
              KERN_SUCCESS);
    {
        int found = 0;
        for (uint32_t i = 0; i < names_cnt; i++) {
            if (names[i] == n) found = 1;
        }
        CHECK(found == 0);
    }

    /* The buffers come from mach_vm_allocate (mmap); the short-lived
     * test process reclaims them on exit, so they are not freed here. */
    SCENARIO_END("port_names_list", f0);
}

int main(void) {
    printf("ipc port names test\n");
    ipc_init();
    harness_hook_install();
    test_name_reuse();
    test_name_uniqueness();
    test_port_names_list();
    return test_finish("port_names");
}
