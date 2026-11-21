/*
 * common.h - shared helpers for the ipc-xnu scenario tests.
 *
 * Each test binary is a single translation unit that includes this
 * file and defines its own main. The counters below are static so
 * each binary has its own copy. The CHECK macros count a check on
 * every use and a failure (with a printed line) when the condition
 * is false. SCENARIO_BEGIN / SCENARIO_END bracket one scenario and
 * print "<name>: PASS" or "<name>: FAIL (N checks failed)".
 *
 * Port helpers follow the device-test dance (tests/libsys/mach_test.c
 * in the reference tree): allocate a RECEIVE right, then insert a
 * SEND right on the same name with MACH_MSG_TYPE_MAKE_SEND.
 *
 * Host-only code. Nothing here ships inside the freestanding dylib.
 *
 * No em dashes are used in this file.
 */
#pragma once
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

/* Bracket one scenario; prints "<name>: PASS" or "<name>: FAIL (N)". */
#define SCENARIO_BEGIN(_name) do { \
    printf("--- %s ---\n", (_name)); \
} while (0)

#define SCENARIO_END(_name, _fail0) do { \
    int _nf = g_failures - (_fail0); \
    if (_nf == 0) { \
        printf("%s: PASS\n", (_name)); \
    } else { \
        printf("%s: FAIL (%d checks failed in this scenario)\n", \
               (_name), _nf); \
    } \
} while (0)

static inline void test_msleep(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000 * 1000;
    nanosleep(&ts, NULL);
}

/* Allocate a RECEIVE right and insert a SEND right on the same name.
 * Returns KERN_SUCCESS or the failing kern return; on success *name
 * holds the port name. */
static inline int make_service_port(mach_port_t task, mach_port_name_t *name) {
    int kr = ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
    if (kr != KERN_SUCCESS) return kr;
    kr = ipc_port_insert_right(task, *name, *name, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        ipc_port_destroy(task, *name);
        *name = MACH_PORT_NULL;
    }
    return kr;
}

/* Allocate a plain RECEIVE-only port (no SEND right). Useful for
 * notification destinations and for ports that must start with
 * srights == 0 (immediate no-senders). */
static inline int make_recv_port(mach_port_t task, mach_port_name_t *name) {
    return ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
}

/* Receive one message with a timeout. Returns the ipc_msg return
 * code (MACH_MSG_SUCCESS, MACH_RCV_TIMED_OUT, ...). */
static inline int recv_timeout(void *buf, uint32_t buf_size,
                               mach_port_name_t port, uint32_t timeout_ms) {
    return ipc_msg(buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, buf_size,
                   port, timeout_ms, 0, NULL, 0);
}

/* Send one message (no receive). Returns the ipc_msg return code. */
static inline int send_only(void *msg, uint32_t size) {
    return ipc_msg(msg, MACH_SEND_MSG, size, 0, 0, 0, 0, NULL, 0);
}

/* Print the standard footer and return the process exit code:
 * 0 when every check passed, 1 otherwise. */
static inline int test_finish(const char *title) {
    printf("%s: %d checks, %d failures\n", title, g_checks, g_failures);
    if (g_failures) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
