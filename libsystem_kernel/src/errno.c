#include "errno.h"
#include <stdint.h>
#include <stddef.h>

extern int32_t *iosw_ref_errno;

void set_errno(int err) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        int **errno_ptr = (int **)(tsd + 8); /* TSD slot 1 */
        if (*errno_ptr) {
            **errno_ptr = err;
        }
    }
    if (iosw_ref_errno) {
        *iosw_ref_errno = err;
    }
}

int linux_to_darwin_errno(long linux_err) {
    if (linux_err >= 0) return 0;
    long err = -linux_err;
    switch (err) {
        case 11:  return 35; /* EAGAIN / EWOULDBLOCK */
        case 35:  return 11; /* EDEADLK */
        case 36:  return 63; /* ENAMETOOLONG */
        case 37:  return 77; /* ENOLCK */
        case 38:  return 78; /* ENOSYS */
        case 39:  return 66; /* ENOTEMPTY */
        case 40:  return 62; /* ELOOP */
        case 42:  return 91; /* ENOMSG */
        case 61:  return 96; /* ENODATA */
        case 62:  return 101;/* ETIME */
        case 75:  return 84; /* EOVERFLOW */
        case 88:  return 38; /* ENOTSOCK */
        case 89:  return 39; /* EDESTADDRREQ */
        case 90:  return 40; /* EMSGSIZE */
        case 91:  return 41; /* EPROTOTYPE */
        case 92:  return 42; /* ENOPROTOOPT */
        case 93:  return 43; /* EPROTONOSUPPORT */
        case 94:  return 44; /* ESOCKTNOSUPPORT */
        case 95:  return 45; /* ENOTSUP / EOPNOTSUPP */
        case 96:  return 46; /* EPFNOSUPPORT */
        case 97:  return 47; /* EAFNOSUPPORT */
        case 98:  return 48; /* EADDRINUSE */
        case 99:  return 49; /* EADDRNOTAVAIL */
        case 100: return 50; /* ENETDOWN */
        case 101: return 51; /* ENETUNREACH */
        case 102: return 52; /* ENETRESET */
        case 103: return 53; /* ECONNABORTED */
        case 104: return 54; /* ECONNRESET */
        case 105: return 55; /* ENOBUFS */
        case 106: return 56; /* EISCONN */
        case 107: return 57; /* ENOTCONN */
        case 108: return 58; /* ESHUTDOWN */
        case 109: return 59; /* ETOOMANYREFS */
        case 110: return 60; /* ETIMEDOUT */
        case 111: return 61; /* ECONNREFUSED */
        case 112: return 64; /* EHOSTDOWN */
        case 113: return 65; /* EHOSTUNREACH */
        case 114: return 37; /* EALREADY */
        case 115: return 36; /* EINPROGRESS */
        case 125: return 89; /* ECANCELED */
        default:  return (int)err;
    }
}

int *lsk_error(void) __asm__("___error");
int *lsk_error(void) {
    uint64_t tsd = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tsd));
    if (tsd) {
        int **errno_ptr = (int **)(tsd + 8);
        if (*errno_ptr) return *errno_ptr;
    }
    return iosw_ref_errno;
}

long lsk_cerror(int err) __asm__("_cerror");
long lsk_cerror(int err) {
    set_errno(err);
    return -1;
}

long lsk_cerror_nocancel(int err) __asm__("_cerror_nocancel");
long lsk_cerror_nocancel(int err) {
    set_errno(err);
    return -1;
}
