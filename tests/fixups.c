/* tests/fixups.c - Rebase and chained fixups test */

#include "darwin_sys.h"

static int add1(int x) {
    return x + 1;
}

static const char *const strs[3] __attribute__((used)) = {
    "first",
    "second",
    "third"
};

int (*fp)(int) = add1;

int g_val0 = 100;
int g_val1 = 200;

struct {
    const char *name;
    int *ptr;
} tab[2] = {
    { "alpha", &g_val0 },
    { "beta",  &g_val1 }
};

static int streq(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Check strs (rebases in __DATA_CONST) */
    const char *volatile *s = (const char *volatile *)strs;
    if (!streq(s[0], "first") || !streq(s[1], "second") || !streq(s[2], "third")) {
        dsys_write(1, "fixups BAD strs\n", 16);
        return 1;
    }

    /* Check fp (rebase in __DATA) */
    int (*volatile f)(int) = fp;
    if (f(41) != 42) {
        dsys_write(1, "fixups BAD fp\n", 14);
        return 1;
    }

    /* Check tab (rebases pointing to string and int in __DATA) */
    if (!streq(tab[0].name, "alpha") || !tab[0].ptr || *tab[0].ptr != 100 ||
        !streq(tab[1].name, "beta")  || !tab[1].ptr || *tab[1].ptr != 200) {
        dsys_write(1, "fixups BAD tab\n", 15);
        return 1;
    }

    dsys_write(1, "fixups ok\n", 10);
    return 0;
}
