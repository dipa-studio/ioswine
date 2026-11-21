/* loader/rt.c - Freestanding runtime for the loader */

#include "loader/rt.h"
#include "loader/sys.h"

int rt_log_enabled = 0;

/* Memory operations using byte loops with asm barriers to prevent
   the compiler from generating calls to library builtins. */

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dst = (unsigned char *)d;
    const unsigned char *src = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
        __asm__ volatile("" : : : "memory");
    }
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dst = (unsigned char *)d;
    const unsigned char *src = (const unsigned char *)s;
    if (dst == src || n == 0)
        return d;
    if (dst < src) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
            __asm__ volatile("" : : : "memory");
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            dst[i - 1] = src[i - 1];
            __asm__ volatile("" : : : "memory");
        }
    }
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dst = (unsigned char *)d;
    unsigned char val = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        dst[i] = val;
        __asm__ volatile("" : : : "memory");
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p1 = (const unsigned char *)a;
    const unsigned char *p2 = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i])
            return p1[i] < p2[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s != ch) {
        if (!*s)
            return NULL;
        s++;
    }
    return (char *)s;
}

/* Formatted output without heap allocation, using raw Linux write syscall */

struct fmt_buf {
    int fd;
    size_t count;
    size_t len;
    char buf[512];
};

static void buf_flush(struct fmt_buf *b) {
    if (b->len == 0)
        return;
    const char *p = b->buf;
    size_t rem = b->len;
    while (rem > 0) {
        long res = sys3(SYS_write, b->fd, (long)p, rem);
        if (res <= 0)
            break;
        p += res;
        rem -= (size_t)res;
        b->count += (size_t)res;
    }
    b->len = 0;
}

static void buf_putc(struct fmt_buf *b, char c) {
    if (b->len >= sizeof(b->buf)) {
        buf_flush(b);
    }
    b->buf[b->len++] = c;
}

static void buf_write(struct fmt_buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf_putc(b, s[i]);
    }
}

static void format_num(struct fmt_buf *b, uint64_t uval, int is_negative, int base, int width, int zero_pad) {
    char digits[64];
    size_t dlen = 0;
    const char *chars = "0123456789abcdef";

    if (uval == 0) {
        digits[dlen++] = '0';
    } else {
        while (uval > 0) {
            digits[dlen++] = chars[uval % base];
            uval /= base;
        }
    }

    size_t total_len = dlen + (is_negative ? 1 : 0);
    size_t pad_len = 0;
    if ((size_t)width > total_len) {
        pad_len = (size_t)width - total_len;
    }

    if (!zero_pad) {
        for (size_t i = 0; i < pad_len; i++) {
            buf_putc(b, ' ');
        }
    }

    if (is_negative) {
        buf_putc(b, '-');
    }

    if (zero_pad) {
        for (size_t i = 0; i < pad_len; i++) {
            buf_putc(b, '0');
        }
    }

    for (size_t i = dlen; i > 0; i--) {
        buf_putc(b, digits[i - 1]);
    }
}

int lvdprintf(int fd, const char *fmt, va_list ap) {
    struct fmt_buf b;
    b.fd = fd;
    b.count = 0;
    b.len = 0;

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            buf_putc(&b, fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0')
            break;
        if (fmt[i] == '%') {
            buf_putc(&b, '%');
            continue;
        }

        int zero_pad = 0;
        if (fmt[i] == '0') {
            zero_pad = 1;
            i++;
        }

        int width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }

        int is_long = 0;
        int is_long_long = 0;
        int is_size_t = 0;

        if (fmt[i] == 'l') {
            i++;
            if (fmt[i] == 'l') {
                is_long_long = 1;
                i++;
            } else {
                is_long = 1;
            }
        } else if (fmt[i] == 'z') {
            is_size_t = 1;
            i++;
        }

        char spec = fmt[i];
        if (spec == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            size_t slen = strlen(str);
            if (!zero_pad && width > (int)slen) {
                for (int p = 0; p < width - (int)slen; p++)
                    buf_putc(&b, ' ');
            }
            buf_write(&b, str, slen);
        } else if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            buf_putc(&b, c);
        } else if (spec == 'd' || spec == 'i') {
            int64_t val;
            if (is_long || is_long_long || is_size_t) {
                val = va_arg(ap, long);
            } else {
                val = va_arg(ap, int);
            }
            int is_negative = 0;
            uint64_t uval;
            if (val < 0) {
                is_negative = 1;
                uval = (uint64_t)(-(val + 1)) + 1;
            } else {
                uval = (uint64_t)val;
            }
            format_num(&b, uval, is_negative, 10, width, zero_pad);
        } else if (spec == 'u') {
            uint64_t uval;
            if (is_long || is_long_long || is_size_t) {
                uval = va_arg(ap, unsigned long);
            } else {
                uval = va_arg(ap, unsigned int);
            }
            format_num(&b, uval, 0, 10, width, zero_pad);
        } else if (spec == 'x') {
            uint64_t uval;
            if (is_long || is_long_long || is_size_t) {
                uval = va_arg(ap, unsigned long);
            } else {
                uval = va_arg(ap, unsigned int);
            }
            format_num(&b, uval, 0, 16, width, zero_pad);
        } else if (spec == 'p') {
            uintptr_t uval = (uintptr_t)va_arg(ap, void *);
            buf_write(&b, "0x", 2);
            format_num(&b, uval, 0, 16, 0, 0);
        } else {
            buf_putc(&b, '%');
            buf_putc(&b, spec);
        }
    }

    buf_flush(&b);
    return (int)b.count;
}

int ldprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = lvdprintf(fd, fmt, ap);
    va_end(ap);
    return ret;
}

__attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...) {
    ldprintf(2, "[loader] FATAL: ");

    va_list ap;
    va_start(ap, fmt);
    lvdprintf(2, fmt, ap);
    va_end(ap);

    size_t flen = strlen(fmt);
    if (flen == 0 || fmt[flen - 1] != '\n') {
        ldprintf(2, "\n");
    }

    sys1(SYS_exit_group, 127);
    for (;;) { }
}
