/* Freestanding runtime for the loader (implemented in loader/rt.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

/* Formatted output to a fd. Supports %s %c %d %i %u %x %lx %ld %lu %llx %p %zu %zx %% and
   width/zero-pad for numbers (e.g. %016lx, %8d). Returns bytes written. Async-signal-safe. */
int lvdprintf(int fd, const char *fmt, va_list ap);
int ldprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* stderr with "[loader] " prefix when LOG is enabled (env IOSWINE_LOG=1 sets rt_log_enabled). */
extern int rt_log_enabled;
#define LOG(...) do { if (rt_log_enabled) ldprintf(2, "[loader] " __VA_ARGS__); } while (0)
/* Print "[loader] FATAL: <msg>\n" to stderr and exit_group(127). */
__attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...);
