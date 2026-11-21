/* Seccomp Darwin-syscall trap (ARCHITECTURE.md §5). */
#pragma once
#include <stdint.h>
#define SIGSYS_MAX_RANGES 8
/* Syscalls with pc in [lo, hi) run natively. Range must not cross a 4 GiB boundary. */
void sigsys_allow_range(uint64_t lo, uint64_t hi);
/* Install SIGSYS handler + seccomp filter. Call once, right before handing off to Mach-O code. */
void sigsys_install(void);
