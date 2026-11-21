/* loader/start.S - Freestanding ELF entry point */

.text
.global _start
.type _start, %function

_start:
    /* Clear frame pointer (x29) and link register (x30) */
    mov     x29, xzr
    mov     x30, xzr

    /* On entry, sp points to argc (Linux initial stack).
       Pass original sp in x0 to loader_main(long *sp). */
    mov     x0, sp

    /* Call loader_main; sp is 16-byte aligned. */
    bl      loader_main

    /* If loader_main ever returns, exit_group(127) via svc #0 */
    mov     x0, #127
    mov     x8, #94     /* SYS_exit_group */
    svc     #0

1:
    b       1b

.size _start, . - _start

/* Non-executable stack note */
.section .note.GNU-stack,"",@progbits
