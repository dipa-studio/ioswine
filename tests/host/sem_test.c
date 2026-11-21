/*
 * sem_test.c -- host test for POSIX named semaphores in libsystem_kernel/src/sem.c.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Darwin constants */
#define DARWIN_ENOENT       2
#define DARWIN_EEXIST       17
#define DARWIN_EINVAL       22
#define DARWIN_EAGAIN       35
#define DARWIN_ENAMETOOLONG 63
#define DARWIN_ENOSYS       78
#define DARWIN_EOVERFLOW    84
#define DARWIN_O_CREAT      0x0200
#define DARWIN_O_EXCL       0x0800
#define DARWIN_SEM_FAILED   ((void *)(intptr_t)-1)

/* Track Darwin errno set by sem.c */
static __thread int g_darwin_errno = 0;
void set_errno(int err) { g_darwin_errno = err; }

extern void *lsk_sem_open(const char *name, int oflag, ...) __asm__("_sem_open");
extern void *lsk___sem_open(const char *name, int oflag, int mode, unsigned int value) __asm__("___sem_open");
extern int lsk_sem_close(void *sem) __asm__("_sem_close");
extern int lsk_sem_unlink(const char *name) __asm__("_sem_unlink");
extern int lsk_sem_wait(void *sem) __asm__("_sem_wait");
extern int lsk___sem_wait_nocancel(void *sem) __asm__("___sem_wait_nocancel");
extern int lsk_sem_wait_nocancel(void *sem) __asm__("_sem_wait$NOCANCEL");
extern int lsk_sem_trywait(void *sem) __asm__("_sem_trywait");
extern int lsk_sem_post(void *sem) __asm__("_sem_post");
extern int lsk_sem_init(void *sem, int pshared, unsigned int value) __asm__("_sem_init");
extern int lsk_sem_destroy(void *sem) __asm__("_sem_destroy");
extern int lsk_sem_getvalue(void *sem, int *sval) __asm__("_sem_getvalue");

static void test_errors_and_basics(void) {
    g_darwin_errno = 0;
    CHECK(lsk_sem_open(NULL, DARWIN_O_CREAT, 0644, 1) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_EINVAL);

    CHECK(lsk_sem_open("", DARWIN_O_CREAT, 0644, 1) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_EINVAL);

    CHECK(lsk_sem_open("/", DARWIN_O_CREAT, 0644, 1) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_EINVAL);

    char longname[300];
    memset(longname, 'a', sizeof(longname));
    longname[0] = '/';
    longname[299] = '\0';
    CHECK(lsk_sem_open(longname, DARWIN_O_CREAT, 0644, 1) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_ENAMETOOLONG);

    CHECK(lsk_sem_open("/missing", 0) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_ENOENT);

    CHECK(lsk_sem_open("/badval", DARWIN_O_CREAT, 0644, 32768) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_EINVAL);

    /* Create with initial value 2 */
    void *s1 = lsk_sem_open("/alpha", DARWIN_O_CREAT | DARWIN_O_EXCL, 0644, 2);
    CHECK(s1 != DARWIN_SEM_FAILED && s1 != NULL);

    /* O_CREAT | O_EXCL fails with EEXIST when already present */
    CHECK(lsk_sem_open("/alpha", DARWIN_O_CREAT | DARWIN_O_EXCL, 0644, 1) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_EEXIST);

    /* Reopen with/without leading slash */
    void *s2 = lsk_sem_open("alpha", 0);
    CHECK(s2 == s1);

    CHECK(lsk_sem_trywait(s1) == 0);
    CHECK(lsk_sem_wait_nocancel(s2) == 0);

    /* Now empty -> trywait fails with Darwin EAGAIN (35) */
    g_darwin_errno = 0;
    CHECK(lsk_sem_trywait(s1) == -1);
    CHECK(g_darwin_errno == DARWIN_EAGAIN);

    CHECK(lsk_sem_post(s1) == 0);
    CHECK(lsk___sem_wait_nocancel(s2) == 0);

    /* Unlink while open: name disappears, handle still works, new name is separate */
    CHECK(lsk_sem_unlink("/alpha") == 0);
    CHECK(lsk_sem_unlink("/alpha") == -1);
    CHECK(g_darwin_errno == DARWIN_ENOENT);
    CHECK(lsk_sem_open("/alpha", 0) == DARWIN_SEM_FAILED);
    CHECK(g_darwin_errno == DARWIN_ENOENT);

    void *s_new = lsk___sem_open("/alpha", DARWIN_O_CREAT, 0644, 1);
    CHECK(s_new != DARWIN_SEM_FAILED && s_new != s1);
    CHECK(lsk_sem_post(s1) == 0);
    CHECK(lsk_sem_wait(s1) == 0);
    CHECK(lsk_sem_close(s1) == 0);
    CHECK(lsk_sem_close(s2) == 0);

    /* Close s_new without unlink -> persists value across reopen */
    CHECK(lsk_sem_wait(s_new) == 0);
    CHECK(lsk_sem_post(s_new) == 0);
    CHECK(lsk_sem_close(s_new) == 0);

    void *s_reopen = lsk_sem_open("/alpha", 0);
    CHECK(s_reopen != DARWIN_SEM_FAILED);
    CHECK(lsk_sem_trywait(s_reopen) == 0);
    CHECK(lsk_sem_trywait(s_reopen) == -1 && g_darwin_errno == DARWIN_EAGAIN);
    CHECK(lsk_sem_close(s_reopen) == 0);
    CHECK(lsk_sem_unlink("/alpha") == 0);

    /* Overflow at SEM_VALUE_MAX (32767) */
    void *smax = lsk_sem_open("/max", DARWIN_O_CREAT, 0644, 32767);
    CHECK(smax != DARWIN_SEM_FAILED);
    CHECK(lsk_sem_post(smax) == -1);
    CHECK(g_darwin_errno == DARWIN_EOVERFLOW);
    CHECK(lsk_sem_close(smax) == 0);
    CHECK(lsk_sem_unlink("/max") == 0);

    /* Unnamed POSIX semaphores return ENOSYS on Darwin */
    int dummy = 0;
    CHECK(lsk_sem_init(&dummy, 0, 1) == -1 && g_darwin_errno == DARWIN_ENOSYS);
    CHECK(lsk_sem_destroy(&dummy) == -1 && g_darwin_errno == DARWIN_ENOSYS);
    CHECK(lsk_sem_getvalue(&dummy, &dummy) == -1 && g_darwin_errno == DARWIN_ENOSYS);
}

#define NTHREADS 4
#define ITERS_PER_THREAD 2500

static void *g_mt_sem = NULL;
static int g_consumed = 0;

static void *producer_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        if (lsk_sem_post(g_mt_sem) != 0) return (void *)1;
    }
    return NULL;
}

static void *consumer_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        if (lsk_sem_wait(g_mt_sem) != 0) return (void *)1;
        __atomic_fetch_add(&g_consumed, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void test_multithreaded(void) {
    g_mt_sem = lsk_sem_open("/mt_sem", DARWIN_O_CREAT | DARWIN_O_EXCL, 0644, 0);
    CHECK(g_mt_sem != DARWIN_SEM_FAILED);
    g_consumed = 0;

    pthread_t prods[NTHREADS], cons[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&cons[i], NULL, consumer_fn, NULL) == 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&prods[i], NULL, producer_fn, NULL) == 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        pthread_join(prods[i], &ret);
        CHECK(ret == NULL);
        pthread_join(cons[i], &ret);
        CHECK(ret == NULL);
    }
    CHECK(g_consumed == NTHREADS * ITERS_PER_THREAD);
    CHECK(lsk_sem_trywait(g_mt_sem) == -1 && g_darwin_errno == DARWIN_EAGAIN);
    CHECK(lsk_sem_close(g_mt_sem) == 0);
    CHECK(lsk_sem_unlink("/mt_sem") == 0);
}

int main(void) {
    test_errors_and_basics();
    test_multithreaded();
    printf("sem_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
