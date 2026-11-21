/* POSIX named semaphores (in-process, futex-backed, Darwin ABI & errnos). */
#include "linux.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define DARWIN_ENOENT       2
#define DARWIN_EINTR        4
#define DARWIN_EEXIST       17
#define DARWIN_EINVAL       22
#define DARWIN_ENOSPC       28
#define DARWIN_EAGAIN       35
#define DARWIN_ENAMETOOLONG 63
#define DARWIN_ENOSYS       78
#define DARWIN_EOVERFLOW    84

#define DARWIN_O_CREAT      0x0200
#define DARWIN_O_EXCL       0x0800

#define DARWIN_SEM_FAILED   ((void *)(intptr_t)-1)
#define DARWIN_SEM_VAL_MAX  32767

#define MAX_NAMED_SEMS      256
#define SEM_NAME_MAX        255

struct named_sem {
    int32_t  val;
    uint32_t waiters;
    uint32_t open_count;
    uint8_t  in_use;
    uint8_t  unlinked;
    char     name[SEM_NAME_MAX + 1];
};

static struct named_sem g_named_sems[MAX_NAMED_SEMS];
static uint32_t g_sem_lock = 0;

static inline void sem_lock(void) {
    while (__atomic_exchange_n(&g_sem_lock, 1, __ATOMIC_ACQUIRE)) {
        sys4(SYS_futex, (long)&g_sem_lock, FUTEX_WAIT_PRIVATE, 1, 0);
    }
}

static inline void sem_unlock(void) {
    __atomic_store_n(&g_sem_lock, 0, __ATOMIC_RELEASE);
    sys3(SYS_futex, (long)&g_sem_lock, FUTEX_WAKE_PRIVATE, 1);
}

static int sem_streq(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *a == *b;
}

static int sem_normalize_name(const char *name, char *out, size_t cap, int *err_out) {
    if (!name) {
        *err_out = DARWIN_EINVAL;
        return 0;
    }
    if (name[0] == '/') name++;
    size_t len = 0;
    while (name[len]) {
        if (len >= cap - 1) {
            *err_out = DARWIN_ENAMETOOLONG;
            return 0;
        }
        out[len] = name[len];
        len++;
    }
    if (len == 0) {
        *err_out = DARWIN_EINVAL;
        return 0;
    }
    out[len] = '\0';
    return 1;
}

static struct named_sem *sem_from_handle(void *sem) {
    if (!sem || sem == DARWIN_SEM_FAILED) return NULL;
    uintptr_t p = (uintptr_t)sem;
    uintptr_t base = (uintptr_t)g_named_sems;
    uintptr_t end = base + sizeof(g_named_sems);
    if (p < base || p >= end) return NULL;
    if ((p - base) % sizeof(struct named_sem) != 0) return NULL;
    struct named_sem *s = (struct named_sem *)sem;
    if (!__atomic_load_n(&s->in_use, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s->open_count, __ATOMIC_ACQUIRE) == 0) {
        return NULL;
    }
    return s;
}

void *lsk___sem_open(const char *name, int oflag, int mode, unsigned int value) __asm__("___sem_open");
void *lsk___sem_open(const char *name, int oflag, int mode, unsigned int value) {
    (void)mode;
    char norm[SEM_NAME_MAX + 1];
    int err = 0;
    if (!sem_normalize_name(name, norm, sizeof(norm), &err)) {
        set_errno(err);
        return DARWIN_SEM_FAILED;
    }

    sem_lock();

    struct named_sem *found = NULL;
    struct named_sem *free_slot = NULL;
    for (int i = 0; i < MAX_NAMED_SEMS; i++) {
        if (!g_named_sems[i].in_use) {
            if (!free_slot) free_slot = &g_named_sems[i];
            continue;
        }
        if (!g_named_sems[i].unlinked && sem_streq(g_named_sems[i].name, norm)) {
            found = &g_named_sems[i];
            break;
        }
    }

    if (found) {
        if ((oflag & (DARWIN_O_CREAT | DARWIN_O_EXCL)) == (DARWIN_O_CREAT | DARWIN_O_EXCL)) {
            sem_unlock();
            set_errno(DARWIN_EEXIST);
            return DARWIN_SEM_FAILED;
        }
        __atomic_fetch_add(&found->open_count, 1, __ATOMIC_RELEASE);
        sem_unlock();
        return found;
    }

    if (!(oflag & DARWIN_O_CREAT)) {
        sem_unlock();
        set_errno(DARWIN_ENOENT);
        return DARWIN_SEM_FAILED;
    }

    if (value > (unsigned int)DARWIN_SEM_VAL_MAX) {
        sem_unlock();
        set_errno(DARWIN_EINVAL);
        return DARWIN_SEM_FAILED;
    }

    if (!free_slot) {
        sem_unlock();
        set_errno(DARWIN_ENOSPC);
        return DARWIN_SEM_FAILED;
    }

    for (size_t i = 0; i <= SEM_NAME_MAX; i++) {
        free_slot->name[i] = norm[i];
        if (norm[i] == '\0') break;
    }
    __atomic_store_n(&free_slot->val, (int32_t)value, __ATOMIC_RELEASE);
    __atomic_store_n(&free_slot->waiters, 0, __ATOMIC_RELEASE);
    free_slot->unlinked = 0;
    __atomic_store_n(&free_slot->open_count, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&free_slot->in_use, 1, __ATOMIC_RELEASE);

    sem_unlock();
    return free_slot;
}

void *lsk_sem_open(const char *name, int oflag, ...) __asm__("_sem_open");
void *lsk_sem_open(const char *name, int oflag, ...) {
    int mode = 0;
    unsigned int value = 0;
    if (oflag & DARWIN_O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, int);
        value = va_arg(ap, unsigned int);
        va_end(ap);
    }
    return lsk___sem_open(name, oflag, mode, value);
}

int lsk_sem_close(void *sem) __asm__("_sem_close");
int lsk_sem_close(void *sem) {
    sem_lock();
    struct named_sem *s = sem_from_handle(sem);
    if (!s) {
        sem_unlock();
        set_errno(DARWIN_EINVAL);
        return -1;
    }
    uint32_t rem = __atomic_sub_fetch(&s->open_count, 1, __ATOMIC_ACQ_REL);
    if (rem == 0 && s->unlinked) {
        __atomic_store_n(&s->in_use, 0, __ATOMIC_RELEASE);
    }
    sem_unlock();
    return 0;
}

int lsk_sem_unlink(const char *name) __asm__("_sem_unlink");
int lsk_sem_unlink(const char *name) {
    char norm[SEM_NAME_MAX + 1];
    int err = 0;
    if (!sem_normalize_name(name, norm, sizeof(norm), &err)) {
        set_errno(err);
        return -1;
    }

    sem_lock();
    struct named_sem *found = NULL;
    for (int i = 0; i < MAX_NAMED_SEMS; i++) {
        if (g_named_sems[i].in_use && !g_named_sems[i].unlinked &&
            sem_streq(g_named_sems[i].name, norm)) {
            found = &g_named_sems[i];
            break;
        }
    }
    if (!found) {
        sem_unlock();
        set_errno(DARWIN_ENOENT);
        return -1;
    }
    found->unlinked = 1;
    if (__atomic_load_n(&found->open_count, __ATOMIC_ACQUIRE) == 0) {
        __atomic_store_n(&found->in_use, 0, __ATOMIC_RELEASE);
    }
    sem_unlock();
    return 0;
}

int lsk_sem_wait(void *sem) __asm__("_sem_wait");
int lsk_sem_wait(void *sem) {
    struct named_sem *s = sem_from_handle(sem);
    if (!s) {
        set_errno(DARWIN_EINVAL);
        return -1;
    }
    for (;;) {
        int32_t v = __atomic_load_n(&s->val, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(&s->val, &v, v - 1, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return 0;
            }
            continue;
        }
        __atomic_fetch_add(&s->waiters, 1, __ATOMIC_SEQ_CST);
        v = __atomic_load_n(&s->val, __ATOMIC_SEQ_CST);
        if (v == 0) {
            long r = sys4(SYS_futex, (long)&s->val, FUTEX_WAIT_PRIVATE, 0, 0);
            if (r == -4 /* -EINTR */) {
                __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_RELAXED);
                set_errno(DARWIN_EINTR);
                return -1;
            }
        }
        __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_RELAXED);
    }
}

int lsk___sem_wait_nocancel(void *sem) __asm__("___sem_wait_nocancel");
int lsk___sem_wait_nocancel(void *sem) {
    return lsk_sem_wait(sem);
}

int lsk_sem_wait_nocancel(void *sem) __asm__("_sem_wait$NOCANCEL");
int lsk_sem_wait_nocancel(void *sem) {
    return lsk_sem_wait(sem);
}

int lsk_sem_trywait(void *sem) __asm__("_sem_trywait");
int lsk_sem_trywait(void *sem) {
    struct named_sem *s = sem_from_handle(sem);
    if (!s) {
        set_errno(DARWIN_EINVAL);
        return -1;
    }
    for (;;) {
        int32_t v = __atomic_load_n(&s->val, __ATOMIC_ACQUIRE);
        if (v <= 0) {
            set_errno(DARWIN_EAGAIN);
            return -1;
        }
        if (__atomic_compare_exchange_n(&s->val, &v, v - 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
}

int lsk_sem_post(void *sem) __asm__("_sem_post");
int lsk_sem_post(void *sem) {
    struct named_sem *s = sem_from_handle(sem);
    if (!s) {
        set_errno(DARWIN_EINVAL);
        return -1;
    }
    int32_t v = __atomic_load_n(&s->val, __ATOMIC_RELAXED);
    for (;;) {
        if (v >= DARWIN_SEM_VAL_MAX) {
            set_errno(DARWIN_EOVERFLOW);
            return -1;
        }
        if (__atomic_compare_exchange_n(&s->val, &v, v + 1, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            break;
        }
    }
    if (__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST) > 0) {
        sys3(SYS_futex, (long)&s->val, FUTEX_WAKE_PRIVATE, 1);
    }
    return 0;
}

/* Darwin does not implement unnamed POSIX semaphores or sem_getvalue (returns ENOSYS). */
int lsk_sem_init(void *sem, int pshared, unsigned int value) __asm__("_sem_init");
int lsk_sem_init(void *sem, int pshared, unsigned int value) {
    (void)sem; (void)pshared; (void)value;
    set_errno(DARWIN_ENOSYS);
    return -1;
}

int lsk_sem_destroy(void *sem) __asm__("_sem_destroy");
int lsk_sem_destroy(void *sem) {
    (void)sem;
    set_errno(DARWIN_ENOSYS);
    return -1;
}

int lsk_sem_getvalue(void *sem, int *sval) __asm__("_sem_getvalue");
int lsk_sem_getvalue(void *sem, int *sval) {
    (void)sem; (void)sval;
    set_errno(DARWIN_ENOSYS);
    return -1;
}
