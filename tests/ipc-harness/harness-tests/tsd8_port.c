/*
 * tsd8_port.c - host port of the mach.c TSD[8] special-reply-port
 * logic. See tsd8_port.h for details and the aarch64 limitation.
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "tsd8_port.h"

#ifndef TSD8_PATCHED
#define TSD8_PATCHED 0
#endif

/* Slot offsets match mach.c: TSD[2] at +16, TSD[8] at +64. */
#define TSD_SIZE 128
#define TSD_OFF_REPLY 16
#define TSD_OFF_SPECIAL 64

static pthread_key_t g_tsd_key;
static pthread_once_t g_tsd_once = PTHREAD_ONCE_INIT;

static void tsd8_make_key(void) {
    pthread_key_create(&g_tsd_key, free);
}

static uint32_t *tsd8_base(void) {
    pthread_once(&g_tsd_once, tsd8_make_key);
    void *b = pthread_getspecific(g_tsd_key);
    if (!b) {
        b = calloc(1, TSD_SIZE);
        pthread_setspecific(g_tsd_key, b);
    }
    return (uint32_t *)b;
}

void tsd8_init(void) {
    (void)tsd8_base();
}

mach_port_name_t tsd8_get_special_reply_port(void) {
    uint32_t *base = tsd8_base();
    uint32_t *p = (uint32_t *)((uint8_t *)base + TSD_OFF_SPECIAL);
#if TSD8_PATCHED
    /* Patch item 6: re-validate the cached name, matching
     * lsk_mig_get_reply_port (mach.c:108). */
    if (*p != 0 && *p != MACH_PORT_DEAD && ipc_port_has_receive_right(*p))
        return *p;
#else
    /* Base mach.c logic (mach.c:85-86): return the cached name if
     * nonzero and not DEAD, with no receive-right recheck. */
    if (*p != 0 && *p != MACH_PORT_DEAD)
        return *p;
#endif
    *p = ipc_mach_reply_port();
    return *p;
}

int tsd8_destroy_port(mach_port_name_t name) {
    uint32_t *base = tsd8_base();
    /* mach.c:381 clears TSD[2] when it holds the destroyed name. */
    uint32_t *p2 = (uint32_t *)((uint8_t *)base + TSD_OFF_REPLY);
    if (*p2 == name)
        *p2 = 0;
#if TSD8_PATCHED
    /* Patch item 6: clear TSD[8] as well. */
    uint32_t *p8 = (uint32_t *)((uint8_t *)base + TSD_OFF_SPECIAL);
    if (*p8 == name)
        *p8 = 0;
#endif
    return ipc_port_destroy(ipc_task_self(), name);
}
