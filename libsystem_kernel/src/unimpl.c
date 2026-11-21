#include "linux.h"
#include "errno.h"
extern const char *lsk_getenv(const char *name);
#include <stddef.h>
#include <stdint.h>

static size_t mystrlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void print_hex(uint64_t val) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int nib = (val >> (i * 4)) & 0xf;
        buf[2 + (15 - i)] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[18] = '\0';
    sys_write(2, buf, 18);
}

__attribute__((visibility("hidden")))
long __iosw_unimplemented(long x0, const char *name) {
    (void)x0;
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    static const char prefix[] = "[lsk] unimplemented: ";
    sys_write(2, prefix, sizeof(prefix) - 1);
    if (name) {
        sys_write(2, name, mystrlen(name));
    } else {
        static const char unknown[] = "(unknown)";
        sys_write(2, unknown, sizeof(unknown) - 1);
    }
    static const char from[] = " (caller: ";
    sys_write(2, from, sizeof(from) - 1);
    print_hex(lr);
    sys_write(2, ")\n", 2);
    {
        /* Soft by default: return -1 / ENOSYS to the caller (stubs tail-branch here). */
        const char *f = lsk_getenv("IOSWINE_UNIMPL_FATAL");
        if (f && f[0] == '1') { sys_exit_group(134); __builtin_unreachable(); }
    }
    set_errno(78 /* Darwin ENOSYS */);
    return -1;
}
