/*
 * thread_reg_test.c - Host unit test for libsystem_kernel/src/thread.c.
 *
 * Exercises:
 *   1. Live-thread registry add/remove via bsdthread_create, bsdthread_terminate,
 *      and workqueue/workloop spawn hooks (lsk_wq_thread_on_spawn / _wlh).
 *   2. "[thread] wq-create ..." and "[thread] create ..." trace gating on g_ipc_trace.
 *   3. lsk_thread_dump() formatting (including ulock wait info and wq flags) and
 *      the invariant that every dumped line is emitted in a single write() call.
 *   4. Table-full handling (MAX_THREAD_RECS = 128) and slot reuse.
 *   5. Concurrent lock-free add/remove + dump under host pthreads.
 *   6. lsk_pthread_sigmask Darwin->Linux translation and SIGQUIT/SIGUSR2 protection.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Declarations from libsystem_kernel/src/thread.c (compiled with -DTHREAD_HOST_TEST). */
int lsk_bsdthread_register(void *threadstart, void *wqthread, int pthsize, void *dummy_val,
                           void *targetconc_ptr, uint64_t dispatchqueue_offset, uint64_t tsd_offset)
    __asm__("___bsdthread_register");
int lsk_bsdthread_create(void *func, void *arg, void *stack, void *pthread, unsigned int flags)
    __asm__("___bsdthread_create");
void lsk_bsdthread_terminate(void *stackaddr, size_t size, uint32_t kport, uint32_t sem)
    __asm__("___bsdthread_terminate");
int lsk_pthread_sigmask(int how, const void *set, void *oset)
    __asm__("___pthread_sigmask");

void lsk_thread_reg_add(long tid, void *func, void *arg, void *pthread, uint32_t kport, uint32_t wq_flags);
void lsk_thread_reg_remove(uint32_t kport);
void lsk_thread_reg_reset(void);
void lsk_thread_dump(void);
void lsk_wq_thread_on_spawn(void *pthread, uint32_t kport, uint32_t flags);
void lsk_wq_thread_on_spawn_wlh(void *pthread, uint32_t kport, uint32_t flags, uint64_t wlh);
void *workq_get_wqthread(void);

struct ulock_dbg { long tid; uint64_t addr; uint64_t val; uint32_t op; uint32_t sleeping; };
extern struct ulock_dbg g_ulock_dbg[64];

/* ---- Stubs required by thread.c ---- */
int g_ipc_trace = 0;
static int g_last_errno = 0;
void set_errno(int err) { g_last_errno = err; }
int linux_to_darwin_errno(long linux_err) { return (int)(linux_err < 0 ? -linux_err : linux_err); }

static uint32_t g_next_kport = 0x1003;
uint32_t ipc_port_allocate_thread(void) {
    return __atomic_fetch_add(&g_next_kport, 4, __ATOMIC_RELAXED);
}

static long g_fake_tid = 2000;
long thread_test_gettid(void) {
    return g_fake_tid;
}

static long g_clone_ret = 3000;
long __iosw_raw_clone(unsigned long flags, void *newsp, int *parent_tid, unsigned long tls, int *child_tid) {
    (void)flags; (void)newsp; (void)parent_tid; (void)tls; (void)child_tid;
    return g_clone_ret;
}

static int g_sigmask_last_how = -1;
static uint64_t g_sigmask_cur = 0;
int thread_test_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oset) {
    if (oset) *oset = g_sigmask_cur;
    if (set) {
        g_sigmask_last_how = how;
        if (how == 0) g_sigmask_cur |= *set;
        else if (how == 1) g_sigmask_cur &= ~(*set);
        else if (how == 2) g_sigmask_cur = *set;
        else return 22;
    }
    return 0;
}

/* ---- Write capture for verifying exact single-write() per line ---- */
#define MAX_CAP_LINES 256
static char g_cap_lines[MAX_CAP_LINES][512];
static size_t g_cap_lens[MAX_CAP_LINES];
static int g_cap_count = 0;
static pthread_mutex_t g_cap_mu = PTHREAD_MUTEX_INITIALIZER;

long (*g_thread_test_write_hook)(int fd, const void *buf, size_t len) = NULL;

static long capture_write(int fd, const void *buf, size_t len) {
    assert(fd == 2);
    pthread_mutex_lock(&g_cap_mu);
    if (g_cap_count < MAX_CAP_LINES) {
        size_t copy = len < 511 ? len : 511;
        memcpy(g_cap_lines[g_cap_count], buf, copy);
        g_cap_lines[g_cap_count][copy] = '\0';
        g_cap_lens[g_cap_count] = len;
        g_cap_count++;
    }
    pthread_mutex_unlock(&g_cap_mu);
    return (long)len;
}

static void cap_reset(void) {
    pthread_mutex_lock(&g_cap_mu);
    g_cap_count = 0;
    pthread_mutex_unlock(&g_cap_mu);
}

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void test_empty_dump(void) {
    lsk_thread_reg_reset();
    memset(g_ulock_dbg, 0, sizeof(g_ulock_dbg));
    g_thread_test_write_hook = capture_write;
    cap_reset();

    lsk_thread_dump();
    CHECK(g_cap_count == 1);
    CHECK(strcmp(g_cap_lines[0], "[lsk THREADS] live threads:\n") == 0);
}

static void test_bsdthread_and_wq_create(void) {
    lsk_thread_reg_reset();
    memset(g_ulock_dbg, 0, sizeof(g_ulock_dbg));
    g_thread_test_write_hook = capture_write;

    void *fake_threadstart = (void *)0x180012340ULL;
    void *fake_wqthread = (void *)0x180056780ULL;
    int feat = lsk_bsdthread_register(fake_threadstart, fake_wqthread, 4096, NULL, NULL, 0, 224);
    CHECK(feat == 0x402000de);
    CHECK(workq_get_wqthread() == fake_wqthread);

    /* 1. bsdthread_create with g_ipc_trace = 1 */
    uint8_t pth_buf[1024] __attribute__((aligned(16)));
    uint8_t stk_buf[4096] __attribute__((aligned(16)));
    memset(pth_buf, 0, sizeof(pth_buf));
    *(uint64_t *)(pth_buf + 176) = (uint64_t)(stk_buf + sizeof(stk_buf));
    *(uint64_t *)(pth_buf + 184) = (uint64_t)stk_buf;

    g_ipc_trace = 1;
    g_clone_ret = 0x1111;
    cap_reset();
    int rc = lsk_bsdthread_create((void *)0x100004000ULL, (void *)0xdeadbeefULL,
                                  stk_buf + sizeof(stk_buf), pth_buf, 0);
    CHECK(rc == 0);
    CHECK(g_cap_count == 1);
    CHECK(strstr(g_cap_lines[0], "[thread] create func=0x100004000 arg=0xdeadbeef") != NULL);
    CHECK(strstr(g_cap_lines[0], "tid=1111\n") != NULL);

    /* 2. bsdthread_create with g_ipc_trace = 0 still registers without logging */
    g_ipc_trace = 0;
    g_clone_ret = 0x1112;
    cap_reset();
    rc = lsk_bsdthread_create((void *)0x100005000ULL, (void *)0xcafebabeULL,
                              stk_buf + sizeof(stk_buf), pth_buf, 0);
    CHECK(rc == 0);
    CHECK(g_cap_count == 0);

    /* 3. Workqueue/workloop spawns: kevent, workloop, plain workq */
    g_ipc_trace = 1;
    uint8_t wq_mem[0x600] __attribute__((aligned(16)));
    memset(wq_mem, 0, sizeof(wq_mem));
    void *wq_pth = wq_mem + 0x500;

    /* 3a. kevent (uses self - 0x488 via lsk_wq_thread_on_spawn) */
    *(uint64_t *)((uintptr_t)wq_pth - 0x488) = (uint64_t)-4; /* DISPATCH_WLH_ANON */
    g_fake_tid = 0x2221;
    cap_reset();
    lsk_wq_thread_on_spawn(wq_pth, 0x2003, 0x002c0006u /* TSD_BASE_SET | KEVENT | NEWSPI | PRIO_QOS | 6 */);
    CHECK(g_cap_count == 1);
    CHECK(strstr(g_cap_lines[0], "[thread] wq-create tid=0x2221 prio=0x2c0006 kevent id=0xfffffffffffffffc ") != NULL);
    CHECK(strstr(g_cap_lines[0], "kport=0x2003\n") != NULL);

    /* 3b. workloop */
    *(uint64_t *)((uintptr_t)wq_pth - 0x488) = 0x123456789abcdef0ULL;
    g_fake_tid = 0x2222;
    cap_reset();
    lsk_wq_thread_on_spawn(wq_pth, 0x2007, 0x00640004u /* TSD_BASE_SET | WORKLOOP | NEWSPI | PRIO_QOS | 4 */);
    CHECK(g_cap_count == 1);
    CHECK(strstr(g_cap_lines[0], "[thread] wq-create tid=0x2222 prio=0x640004 workloop id=0x123456789abcdef0 ") != NULL);

    /* 3c. plain reqthreads workq */
    *(uint64_t *)((uintptr_t)wq_pth - 0x488) = 0;
    g_fake_tid = 0x2223;
    cap_reset();
    lsk_wq_thread_on_spawn(wq_pth, 0x200b, 0x00240004u);
    CHECK(g_cap_count == 1);
    CHECK(strstr(g_cap_lines[0], "[thread] wq-create tid=0x2223 prio=0x240004 workq id=0x0 ") != NULL);

    /* 3d. Trace disabled: still registers, emits no wq-create line */
    g_ipc_trace = 0;
    g_fake_tid = 0x2224;
    cap_reset();
    lsk_wq_thread_on_spawn_wlh(wq_pth, 0x200f, 0x00640004u, 0x7777ULL);
    CHECK(g_cap_count == 0);

    /* Attach ulock wait state to tid 0x2222 */
    g_ulock_dbg[0x2222 & 63].tid = 0x2222;
    g_ulock_dbg[0x2222 & 63].op = 0x101;
    g_ulock_dbg[0x2222 & 63].addr = 0x1800aabb0ULL;
    g_ulock_dbg[0x2222 & 63].sleeping = 1;

    /* Dump all 6 threads and verify each line is one write() */
    cap_reset();
    lsk_thread_dump();
    CHECK(g_cap_count == 7);
    for (int i = 0; i < g_cap_count; i++) {
        CHECK(g_cap_lens[i] > 0);
        CHECK(g_cap_lines[i][g_cap_lens[i] - 1] == '\n');
        /* Exactly one newline per write() */
        char *first_nl = strchr(g_cap_lines[i], '\n');
        CHECK(first_nl == &g_cap_lines[i][g_cap_lens[i] - 1]);
    }
    CHECK(strcmp(g_cap_lines[0], "[lsk THREADS] live threads:\n") == 0);
    CHECK(strstr(g_cap_lines[1], "[lsk THREAD] tid=1111 func=0x100004000 ") != NULL);
    CHECK(strstr(g_cap_lines[3], "[lsk THREAD] tid=2221 func=0x180056780 arg=0xfffffffffffffffc ") != NULL);
    CHECK(strstr(g_cap_lines[3], " wq=0x2c0006") != NULL);
    CHECK(strstr(g_cap_lines[4], "[lsk THREAD] tid=2222 func=0x180056780 arg=0x123456789abcdef0 ") != NULL);
    CHECK(strstr(g_cap_lines[4], " ulock op=0x101 addr=0x1800aabb0\n") != NULL);
    CHECK(strstr(g_cap_lines[6], "[lsk THREAD] tid=2224 func=0x180056780 arg=0x7777 ") != NULL);

    /* Remove 0x2003 via lsk_bsdthread_terminate and 0x200f via lsk_thread_reg_remove */
    g_ipc_trace = 1;
    cap_reset();
    lsk_bsdthread_terminate(NULL, 0, 0x2003, 0);
    CHECK(g_cap_count == 1);
    CHECK(strcmp(g_cap_lines[0], "[thread] exit kport=0x2003\n") == 0);

    lsk_thread_reg_remove(0x200f);
    lsk_thread_reg_remove(0x99999); /* non-existent: safe no-op */

    cap_reset();
    lsk_thread_dump();
    CHECK(g_cap_count == 5);
    for (int i = 1; i < g_cap_count; i++) {
        CHECK(strstr(g_cap_lines[i], "kport=0x2003") == NULL);
        CHECK(strstr(g_cap_lines[i], "kport=0x200f") == NULL);
    }
}

static void test_capacity_and_reuse(void) {
    lsk_thread_reg_reset();
    memset(g_ulock_dbg, 0, sizeof(g_ulock_dbg));
    g_thread_test_write_hook = capture_write;
    g_ipc_trace = 0;

    for (uint32_t i = 0; i < 128; i++) {
        lsk_thread_reg_add((long)(4000 + i), (void *)0x1000, (void *)(uintptr_t)i,
                           (void *)0x2000, 0x5000 + i, 0);
    }

    /* 129th fails silently in registry, still prints wq-create when traced */
    g_ipc_trace = 1;
    g_fake_tid = 9999;
    cap_reset();
    lsk_wq_thread_on_spawn_wlh((void *)0x3000, 0x6000, 0x640004u, 0xabcdULL);
    CHECK(g_cap_count == 1);
    CHECK(strstr(g_cap_lines[0], "[thread] wq-create tid=0x270f ") != NULL);

    cap_reset();
    lsk_thread_dump();
    CHECK(g_cap_count == 129); /* header + 128 entries */

    /* Remove one and verify reuse */
    lsk_thread_reg_remove(0x5005);
    lsk_wq_thread_on_spawn_wlh((void *)0x3000, 0x6001, 0x640004u, 0xbeefULL);

    cap_reset();
    lsk_thread_dump();
    CHECK(g_cap_count == 129);
    int found_beef = 0, found_5005 = 0;
    for (int i = 1; i < g_cap_count; i++) {
        if (strstr(g_cap_lines[i], "arg=0xbeef") && strstr(g_cap_lines[i], "kport=0x6001")) found_beef = 1;
        if (strstr(g_cap_lines[i], "kport=0x5005")) found_5005 = 1;
    }
    CHECK(found_beef == 1);
    CHECK(found_5005 == 0);
}

static volatile int g_mt_stop = 0;
static void *mt_worker(void *arg) {
    uint32_t base = (uint32_t)(uintptr_t)arg;
    for (int iter = 0; iter < 500; iter++) {
        uint32_t kp = 0x8000u + base * 16u + (uint32_t)(iter % 4);
        lsk_thread_reg_add((long)(7000 + base * 16 + (iter % 4)),
                           (void *)0x180010000ULL, (void *)(uintptr_t)iter,
                           (void *)0x200000000ULL, kp, 0x640004u);
        if ((iter & 3) == 3) {
            for (uint32_t k = 0; k < 4; k++) {
                lsk_thread_reg_remove(0x8000u + base * 16u + k);
            }
        }
    }
    return NULL;
}

static long mt_check_write(int fd, const void *buf, size_t len) {
    assert(fd == 2);
    const char *s = (const char *)buf;
    assert(len > 0 && s[len - 1] == '\n');
    return (long)len;
}

static void test_concurrent_reg_and_dump(void) {
    lsk_thread_reg_reset();
    memset(g_ulock_dbg, 0, sizeof(g_ulock_dbg));
    g_ipc_trace = 0;
    g_thread_test_write_hook = mt_check_write;

    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        assert(pthread_create(&th[i], NULL, mt_worker, (void *)(uintptr_t)i) == 0);
    }
    for (int d = 0; d < 200; d++) {
        lsk_thread_dump();
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    g_thread_test_write_hook = capture_write;
    cap_reset();
    lsk_thread_dump();
    CHECK(g_cap_count >= 1);
}

static void test_pthread_sigmask(void) {
    g_sigmask_cur = 0;
    g_sigmask_last_how = -1;

    /* Invalid how returns EINVAL (22) */
    uint32_t set = 0xffffffffu;
    uint32_t oset = 0;
    CHECK(lsk_pthread_sigmask(99, &set, &oset) == 22);

    /* Darwin SIG_BLOCK=1 translates to Linux 0 and strips SIGQUIT(3), SIGUSR2(12), SIGSYS(31) */
    CHECK(lsk_pthread_sigmask(1, &set, &oset) == 0);
    CHECK(g_sigmask_last_how == 0);
    CHECK((g_sigmask_cur & (1ULL << (3 - 1))) == 0);
    CHECK((g_sigmask_cur & (1ULL << (12 - 1))) == 0);
    CHECK((g_sigmask_cur & (1ULL << (31 - 1))) == 0);
    CHECK((g_sigmask_cur & (1ULL << (15 - 1))) != 0); /* SIGTERM still allowed */

    /* Darwin SIG_UNBLOCK=2 translates to Linux 1 */
    uint32_t unb = (1u << (15 - 1));
    CHECK(lsk_pthread_sigmask(2, &unb, &oset) == 0);
    CHECK(g_sigmask_last_how == 1);
    CHECK((oset & (1u << (15 - 1))) != 0);
    CHECK((g_sigmask_cur & (1ULL << (15 - 1))) == 0);

    /* Darwin SIG_SETMASK=3 translates to Linux 2 and protects SIGQUIT/SIGUSR2/SIGSYS */
    CHECK(lsk_pthread_sigmask(3, &set, NULL) == 0);
    CHECK(g_sigmask_last_how == 2);
    CHECK((g_sigmask_cur & (1ULL << (3 - 1))) == 0);
    CHECK((g_sigmask_cur & (1ULL << (12 - 1))) == 0);
    CHECK((g_sigmask_cur & (1ULL << (31 - 1))) == 0);
}

static void test_real_pipe_write(void) {
    /* Exercise real sys_write(2, ...) path with g_thread_test_write_hook == NULL */
    lsk_thread_reg_reset();
    lsk_thread_reg_add(0x42, (void *)0x1111, (void *)0x2222, (void *)0x3333, 0x403, 0);

    int pfd[2];
    assert(pipe(pfd) == 0);
    int saved_stderr = dup(2);
    assert(saved_stderr >= 0);
    assert(dup2(pfd[1], 2) == 2);
    close(pfd[1]);

    g_thread_test_write_hook = NULL;
    lsk_thread_dump();

    assert(dup2(saved_stderr, 2) == 2);
    close(saved_stderr);

    char rbuf[512];
    ssize_t n = read(pfd[0], rbuf, sizeof(rbuf) - 1);
    close(pfd[0]);
    assert(n > 0);
    rbuf[n] = '\0';
    CHECK(strstr(rbuf, "[lsk THREADS] live threads:\n") != NULL);
    CHECK(strstr(rbuf, "[lsk THREAD] tid=42 func=0x1111 arg=0x2222 pthread=0x3333 kport=0x403\n") != NULL);
}

int main(void) {
    test_empty_dump();
    test_bsdthread_and_wq_create();
    test_capacity_and_reuse();
    test_concurrent_reg_and_dump();
    test_pthread_sigmask();
    test_real_pipe_write();
    printf("TOTAL: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
