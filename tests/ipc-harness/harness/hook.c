/*
 * hook.c - mock g_enqueue_hook for the ipc.c host harness.
 * See hook.h for details.
 *
 * No em dashes are used in this file.
 */
#define _POSIX_C_SOURCE 200809L
#include "hook.h"

static harness_hook_call_t g_calls[HARNESS_HOOK_MAX];
static size_t g_count = 0;
static size_t g_dropped = 0;

static void harness_hook_fn(mach_port_name_t name, uint32_t size) {
    size_t idx = __atomic_fetch_add(&g_count, 1, __ATOMIC_RELAXED);
    if (idx < HARNESS_HOOK_MAX) {
        g_calls[idx].port = name;
        g_calls[idx].size = size;
    } else {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
    }
}

void harness_hook_install(void) {
    harness_hook_clear();
    ipc_set_enqueue_hook(harness_hook_fn);
}

void harness_hook_uninstall(void) {
    ipc_set_enqueue_hook(NULL);
}

void harness_hook_clear(void) {
    __atomic_store_n(&g_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dropped, 0, __ATOMIC_RELAXED);
}

size_t harness_hook_count(void) {
    size_t c = __atomic_load_n(&g_count, __ATOMIC_RELAXED);
    return c < HARNESS_HOOK_MAX ? c : HARNESS_HOOK_MAX;
}

size_t harness_hook_dropped(void) {
    return __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
}

harness_hook_call_t harness_hook_get(size_t i) {
    harness_hook_call_t empty = { 0, 0 };
    if (i >= harness_hook_count()) return empty;
    return g_calls[i];
}

int harness_hook_find(mach_port_name_t port, uint32_t size) {
    size_t n = harness_hook_count();
    for (size_t i = 0; i < n; i++) {
        if (g_calls[i].port == port && g_calls[i].size == size) {
            return (int)i;
        }
    }
    return -1;
}
