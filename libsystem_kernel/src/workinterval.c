/*
 * workinterval.c - succeed-everything work_interval API.
 *
 * CoreAnimation / UIKit create work intervals for frame scheduling and throw
 * "Failed to create work interval" when work_interval_create fails. Linux has
 * no equivalent, so every call succeeds and hands out opaque handles from a
 * static pool. Freestanding: no libc, no malloc.
 */
#include <stdint.h>
#include <stddef.h>

#define WI_POOL 64
#define WI_SLOT 256

static uint8_t g_wi_pool[WI_POOL][WI_SLOT] __attribute__((aligned(16)));
static uint32_t g_wi_next;

static void *wi_alloc(void) {
    uint32_t i = __atomic_fetch_add(&g_wi_next, 1, __ATOMIC_RELAXED) % WI_POOL;
    uint8_t *p = g_wi_pool[i];
    for (size_t k = 0; k < WI_SLOT; k++) p[k] = 0;
    *(uint64_t *)p = 0x10000u + i; /* fake work interval id */
    return p;
}

int __work_interval_ctl(uint32_t op, uint64_t id, void *arg, size_t len) {
    (void)op; (void)id; (void)arg; (void)len;
    return 0;
}

int work_interval_create(void **out, uint32_t flags) {
    (void)flags;
    if (!out) return 22;
    *out = wi_alloc();
    return 0;
}

int work_interval_copy_port(void *wi, uint32_t *port) {
    (void)wi;
    if (port) *port = 0;
    return 0;
}

int work_interval_destroy(void *wi) { (void)wi; return 0; }
int work_interval_get_flags_from_port(uint32_t port, uint32_t *flags) {
    (void)port;
    if (flags) *flags = 0;
    return 0;
}
int work_interval_join(void *wi) { (void)wi; return 0; }
int work_interval_join_port(uint32_t port) { (void)port; return 0; }
int work_interval_leave(void) { return 0; }
int work_interval_notify(void *wi, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    (void)wi; (void)a; (void)b; (void)c; (void)d; (void)e;
    return 0;
}
int work_interval_notify_simple(void *wi, uint64_t a, uint64_t b, uint64_t c) {
    (void)wi; (void)a; (void)b; (void)c;
    return 0;
}

void *work_interval_instance_alloc(void *wi) { (void)wi; return wi_alloc(); }
void work_interval_instance_free(void *wii) { (void)wii; }
void work_interval_instance_clear(void *wii) { (void)wii; }
int work_interval_instance_start(void *wii) { (void)wii; return 0; }
int work_interval_instance_update(void *wii) { (void)wii; return 0; }
int work_interval_instance_finish(void *wii) { (void)wii; return 0; }
void work_interval_instance_set_start(void *wii, uint64_t v) { (void)wii; (void)v; }
void work_interval_instance_set_deadline(void *wii, uint64_t v) { (void)wii; (void)v; }
void work_interval_instance_set_finish(void *wii, uint64_t v) { (void)wii; (void)v; }
void work_interval_instance_set_complexity(void *wii, uint32_t v) { (void)wii; (void)v; }
uint64_t work_interval_instance_get_id(void *wii) { return wii ? *(uint64_t *)wii : 0; }
uint64_t work_interval_instance_get_start(void *wii) { (void)wii; return 0; }
uint64_t work_interval_instance_get_deadline(void *wii) { (void)wii; return 0; }
uint64_t work_interval_instance_get_finish(void *wii) { (void)wii; return 0; }
uint32_t work_interval_instance_get_complexity(void *wii) { (void)wii; return 0; }
int work_interval_instance_get_telemetry_data(void *wii, void *out, size_t len) {
    (void)wii;
    uint8_t *p = (uint8_t *)out;
    for (size_t k = 0; p && k < len; k++) p[k] = 0;
    return 0;
}
