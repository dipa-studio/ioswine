/* tests/hello.c - Minimal Mach-O test program */

#include "darwin_sys.h"

int main(int argc, char **argv, char **envp, char **apple) {
    (void)argv;
    (void)envp;

    /* Write "hello\n" */
    dsys_write(1, "hello\n", 6);

    /* Write "argc=<n>\n" formatting <n> by hand */
    char buf[32];
    buf[0] = 'a';
    buf[1] = 'r';
    buf[2] = 'g';
    buf[3] = 'c';
    buf[4] = '=';
    int pos = 5;

    if (argc == 0) {
        buf[pos++] = '0';
    } else {
        int val = argc;
        if (val < 0) {
            buf[pos++] = '-';
            val = -val;
        }
        char digits[16];
        int dpos = 0;
        while (val > 0) {
            digits[dpos++] = (char)('0' + (val % 10));
            val /= 10;
        }
        for (int i = dpos - 1; i >= 0; i--) {
            buf[pos++] = digits[i];
        }
    }
    buf[pos++] = '\n';
    dsys_write(1, buf, (size_t)pos);

    /* Write "apple0=<apple[0]>\n" */
    dsys_write(1, "apple0=", 7);
    if (apple && apple[0]) {
        size_t len = 0;
        while (apple[0][len])
            len++;
        dsys_write(1, apple[0], len);
    }
    dsys_write(1, "\n", 1);

    return 0;
}
