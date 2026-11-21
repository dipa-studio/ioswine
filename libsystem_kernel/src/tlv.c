/*
 * tlv.c - Thread-local variables (__thread / C++ thread_local) for images our
 * loader maps itself (main executable, embedded frameworks).
 *
 * The loader binds every TLV descriptor thunk (__tlv_bootstrap) to
 * lsk_tlv_get_addr and points the descriptor key at a record
 * {template start, init size, total size}. Like Apple's tlv_get_addr, the
 * thunk takes the descriptor in x0, returns the variable address in x0 and
 * preserves every other register.
 *
 * Per-thread blocks live in an open-addressed table keyed by
 * (tpidr_el0, record): tpidr_el0 is the iOS TSD base, unique per thread.
 * Blocks are never freed (threads that exit leak their block).
 */
#include <stdint.h>
#include <stddef.h>
#include "linux.h"

struct tlv_info { uint64_t start, init_size, total_size; };
struct tlv_desc { uint64_t thunk; struct tlv_info *key; uint64_t offset; };

#define TLV_SLOTS 8192
static struct { uint64_t tsd; const struct tlv_info *info; uint8_t *block; } g_tlv[TLV_SLOTS];
static volatile int g_tlv_lock;

static uint8_t *tlv_alloc(const struct tlv_info *ti) {
    size_t sz = ti->total_size ? ti->total_size : 16;
    sz = (sz + 4095) & ~(size_t)4095;
    uint8_t *b = (uint8_t *)sys6(SYS_mmap, 0, (long)sz, 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1, 0);
    if ((long)b < 0 && (long)b > -4096) return 0;
    const uint8_t *src = (const uint8_t *)ti->start;
    for (uint64_t i = 0; i < ti->init_size; i++) b[i] = src[i];
    return b; /* zerofill part is already zero */
}

uint64_t lsk_tlv_lookup(const struct tlv_desc *d) {
    uint64_t tsd;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    const struct tlv_info *ti = d->key;
    uint64_t h = ((tsd >> 4) ^ ((uint64_t)ti >> 3)) * 0x9e3779b97f4a7c15ull;
    uint32_t i = (uint32_t)(h >> 51) & (TLV_SLOTS - 1);
    /* Lock-free reads of published slots; insertion under a spinlock. */
    for (uint32_t n = 0; n < TLV_SLOTS; n++, i = (i + 1) & (TLV_SLOTS - 1)) {
        uint64_t t = __atomic_load_n(&g_tlv[i].tsd, __ATOMIC_ACQUIRE);
        if (t == tsd && g_tlv[i].info == ti) return (uint64_t)g_tlv[i].block + d->offset;
        if (t == 0) break;
    }
    while (__atomic_exchange_n(&g_tlv_lock, 1, __ATOMIC_ACQUIRE)) { }
    i = (uint32_t)(h >> 51) & (TLV_SLOTS - 1);
    for (uint32_t n = 0; n < TLV_SLOTS; n++, i = (i + 1) & (TLV_SLOTS - 1)) {
        if (g_tlv[i].tsd == tsd && g_tlv[i].info == ti) break;
        if (g_tlv[i].tsd == 0) {
            g_tlv[i].info = ti;
            g_tlv[i].block = tlv_alloc(ti);
            __atomic_store_n(&g_tlv[i].tsd, tsd, __ATOMIC_RELEASE);
            break;
        }
    }
    uint8_t *b = g_tlv[i].block;
    __atomic_store_n(&g_tlv_lock, 0, __ATOMIC_RELEASE);
    return (uint64_t)b + d->offset;
}

/* Thunk: x0 = descriptor; returns address in x0; preserves all other registers. */
__asm__(
    ".text\n"
    ".globl _lsk_tlv_get_addr\n"
    ".p2align 2\n"
    "_lsk_tlv_get_addr:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  sub sp, sp, #(18*8 + 32*16)\n"
    "  stp x1, x2, [sp, #0]\n"
    "  stp x3, x4, [sp, #16]\n"
    "  stp x5, x6, [sp, #32]\n"
    "  stp x7, x8, [sp, #48]\n"
    "  stp x9, x10, [sp, #64]\n"
    "  stp x11, x12, [sp, #80]\n"
    "  stp x13, x14, [sp, #96]\n"
    "  stp x15, x16, [sp, #112]\n"
    "  stp x17, x18, [sp, #128]\n"
    "  add x9, sp, #144\n"
    "  stp q0, q1, [x9, #0]\n"
    "  stp q2, q3, [x9, #32]\n"
    "  stp q4, q5, [x9, #64]\n"
    "  stp q6, q7, [x9, #96]\n"
    "  stp q8, q9, [x9, #128]\n"
    "  stp q10, q11, [x9, #160]\n"
    "  stp q12, q13, [x9, #192]\n"
    "  stp q14, q15, [x9, #224]\n"
    "  stp q16, q17, [x9, #256]\n"
    "  stp q18, q19, [x9, #288]\n"
    "  stp q20, q21, [x9, #320]\n"
    "  stp q22, q23, [x9, #352]\n"
    "  stp q24, q25, [x9, #384]\n"
    "  stp q26, q27, [x9, #416]\n"
    "  stp q28, q29, [x9, #448]\n"
    "  stp q30, q31, [x9, #480]\n"
    "  bl _lsk_tlv_lookup\n"
    "  add x9, sp, #144\n"
    "  ldp q0, q1, [x9, #0]\n"
    "  ldp q2, q3, [x9, #32]\n"
    "  ldp q4, q5, [x9, #64]\n"
    "  ldp q6, q7, [x9, #96]\n"
    "  ldp q8, q9, [x9, #128]\n"
    "  ldp q10, q11, [x9, #160]\n"
    "  ldp q12, q13, [x9, #192]\n"
    "  ldp q14, q15, [x9, #224]\n"
    "  ldp q16, q17, [x9, #256]\n"
    "  ldp q18, q19, [x9, #288]\n"
    "  ldp q20, q21, [x9, #320]\n"
    "  ldp q22, q23, [x9, #352]\n"
    "  ldp q24, q25, [x9, #384]\n"
    "  ldp q26, q27, [x9, #416]\n"
    "  ldp q28, q29, [x9, #448]\n"
    "  ldp q30, q31, [x9, #480]\n"
    "  ldp x1, x2, [sp, #0]\n"
    "  ldp x3, x4, [sp, #16]\n"
    "  ldp x5, x6, [sp, #32]\n"
    "  ldp x7, x8, [sp, #48]\n"
    "  ldp x9, x10, [sp, #64]\n"
    "  ldp x11, x12, [sp, #80]\n"
    "  ldp x13, x14, [sp, #96]\n"
    "  ldp x15, x16, [sp, #112]\n"
    "  ldp x17, x18, [sp, #128]\n"
    "  mov sp, x29\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
);
