/* tests/host/rt_selftest.c - Host-side selftest for loader runtime */

#include "loader/sys.h"
#include "loader/rt.h"

__attribute__((noreturn)) void loader_main(long *sp) {
    (void)sp;

    ldprintf(1, "rt: hello\n");
    ldprintf(1, "rt: d=%d u=%u x=%x lx=%lx p=%p s=%s c=%c pct=%%\n",
             -42, 42, 0x2a, 0xdeadbeefcafeUL, (void *)0x1234, "abc", 'Z');
    ldprintf(1, "rt: pad=[%04x] [%4d] [%016lx]\n", 0xff, 7, 0x10UL);
    ldprintf(1, "rt: strlen=%zu strcmp=%d memcmp=%d\n",
             strlen("hello"), strcmp("a", "a"), memcmp("abc", "abc", 3));

    sys1(SYS_exit_group, 0);
    for (;;) { }
}
