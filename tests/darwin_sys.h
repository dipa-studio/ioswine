/* tests/darwin_sys.h - Darwin raw syscall helpers for arm64 Mach-O tests */
#pragma once

#include <stdint.h>
#include <stddef.h>

static inline long dsys3(long num, long a, long b, long c) {
    register long x16 __asm__("x16") = num;
    register long x0  __asm__("x0")  = a;
    register long x1  __asm__("x1")  = b;
    register long x2  __asm__("x2")  = c;
    long err;
    __asm__ volatile(
        "svc #0x80\n\t"
        "cset %w[err], cs"
        : "+r"(x0), [err] "=&r"(err)
        : "r"(x16), "r"(x1), "r"(x2)
        : "memory", "cc"
    );
    return err ? -x0 : x0;
}

static inline long dsys_write(int fd, const void *buf, size_t len) {
    return dsys3(4, (long)fd, (long)buf, (long)len);
}

__attribute__((noreturn)) static inline void dsys_exit(int code) {
    register long x16 __asm__("x16") = 1;
    register long x0  __asm__("x0")  = (long)code;
    for (;;) {
        __asm__ volatile(
            "svc #0x80"
            : "+r"(x0)
            : "r"(x16)
            : "memory", "cc"
        );
    }
}
