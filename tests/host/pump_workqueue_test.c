/*
 * pump_workqueue_test.c - Thread/workqueue tests.
 *
 * Two parts:
 *   Part 1 drives the REAL libsystem_kernel/src/thread.c
 *   (THREAD_HOST_TEST), the registry built for the
 *   workqueue/workloop spawn path:
 *     - lsk_wq_thread_on_spawn_wlh registers workqueue threads and
 *       logs "[thread] wq-create ..." when tracing is on.
 *     - lsk_thread_reg_add/remove + lsk_thread_dump stay consistent
 *       under concurrent add/remove/dump from host pthreads.
 *     - Table-full (128 records) fails silent, never crashes.
 *   Part 2 models the contract the display-link pump replacement
 * must satisfy, in test-local code: commits arriving
 *   from producer threads while the consumer renders must be neither
 *   lost nor applied twice, the consumer must never deadlock, and
 *   damage rects must coalesce and clip to the frame. The real
 *   CA-commit path is device-only; this test pins the queue contract
 *   it must meet, so a future host implementation of the pump can be
 *   dropped in behind the same assertions.
 *
 * Plain cc + ASan/UBSan on a Linux host. No device, no Apple code.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Declarations from libsystem_kernel/src/thread.c (THREAD_HOST_TEST). */
void lsk_thread_reg_add(long tid, void *func, void *arg, void *pthread, uint32_t kport, uint32_t wq_flags);
void lsk_thread_reg_remove(uint32_t kport);
void lsk_thread_reg_reset(void);
void lsk_thread_dump(void);
void lsk_wq_thread_on_spawn_wlh(void *pthread, uint32_t kport, uint32_t flags, uint64_t wlh);

struct ulock_dbg { long tid; uint64_t addr; uint64_t val; uint32_t op; uint32_t sleeping; };
extern struct ulock_dbg g_ulock_dbg[64];

#define WQ_FLAG_THREAD_KEVENT   0x00080000u
#define WQ_FLAG_THREAD_WORKLOOP 0x00400000u
#define WQ_DISPATCH_WLH_ANON    ((uint64_t)-4)

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Stubs required by thread.c (same surface as thread_reg_test.c)     */
/* ------------------------------------------------------------------ */

int g_ipc_trace = 0;
static int g_last_errno = 0;
void set_errno(int err) { g_last_errno = err; (void)g_last_errno; }
int linux_to_darwin_errno(long linux_err) { return (int)(linux_err < 0 ? -linux_err : linux_err); }

static uint32_t g_next_kport = 0x1003;
uint32_t ipc_port_allocate_thread(void) {
    return __atomic_fetch_add(&g_next_kport, 4, __ATOMIC_RELAXED);
}

long thread_test_gettid(void) {
    return (long)syscall(SYS_gettid);
}

long __iosw_raw_clone(unsigned long flags, void *newsp, int *parent_tid,
                      unsigned long tls, int *child_tid) {
    (void)flags; (void)newsp; (void)parent_tid; (void)tls; (void)child_tid;
    return 3000;
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

/* Capture fd 2 output (lsk_thread_dump and the wq-create trace write
 * there) via thread.c's host-test write hook. The hook must be
 * thread-safe: the concurrent test dumps from worker threads. */
static char g_cap_buf[65536];
static size_t g_cap_len = 0;
static pthread_mutex_t g_cap_mu = PTHREAD_MUTEX_INITIALIZER;
long (*g_thread_test_write_hook)(int fd, const void *buf, size_t len) = NULL;

static long capture_write(int fd, const void *buf, size_t len) {
    if (fd != 2) return (long)write(fd, buf, len);
    pthread_mutex_lock(&g_cap_mu);
    size_t room = sizeof(g_cap_buf) - 1 - g_cap_len;
    size_t take = len < room ? len : room;
    if (take > 0) {
        memcpy(g_cap_buf + g_cap_len, buf, take);
        g_cap_len += take;
        g_cap_buf[g_cap_len] = '\0';
    }
    pthread_mutex_unlock(&g_cap_mu);
    return (long)len;
}

static void cap_reset(void) {
    pthread_mutex_lock(&g_cap_mu);
    g_cap_len = 0;
    g_cap_buf[0] = '\0';
    pthread_mutex_unlock(&g_cap_mu);
}

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    size_t ln = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += ln; }
    return n;
}

/* ------------------------------------------------------------------ */
/* Part 1: real thread registry                                        */
/* ------------------------------------------------------------------ */

static void test_registry_basic(void) {
    lsk_thread_reg_reset();
    g_ipc_trace = 0;
    long tid = thread_test_gettid();
    char want[64];
    snprintf(want, sizeof(want), "tid=%lx", (unsigned long)tid);
    lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)0x1000, 0x2003, WQ_FLAG_THREAD_KEVENT, 0x5555);
    lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)0x2000, 0x2007, WQ_FLAG_THREAD_WORKLOOP, 0x6666);
    cap_reset();
    lsk_thread_dump();
    const char *out = g_cap_buf;
    CHECK(strstr(out, "[lsk THREADS] live threads:") != NULL);
    CHECK(strstr(out, want) != NULL);
    CHECK(strstr(out, "kport=0x2003") != NULL);
    CHECK(strstr(out, "kport=0x2007") != NULL);
    lsk_thread_reg_remove(0x2003);
    cap_reset();
    lsk_thread_dump();
    out = g_cap_buf;
    CHECK(strstr(out, "kport=0x2003") == NULL);
    CHECK(strstr(out, "kport=0x2007") != NULL);
    /* Removing a kport that is not there is a no-op, not a crash. */
    lsk_thread_reg_remove(0x9999);
    lsk_thread_reg_reset();
}

static void test_registry_trace_line(void) {
    lsk_thread_reg_reset();
    g_ipc_trace = 1;
    cap_reset();
    lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)0x3000, 0x2011,
                               WQ_FLAG_THREAD_KEVENT, WQ_DISPATCH_WLH_ANON);
    lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)0x4000, 0x2015,
                               WQ_FLAG_THREAD_WORKLOOP, 0x7777);
    lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)0x5000, 0x2019, 0, 0);
    const char *out = g_cap_buf;
    CHECK(count_substr(out, "[thread] wq-create") == 3);
    CHECK(strstr(out, "kevent") != NULL);
    CHECK(strstr(out, "workloop") != NULL);
    CHECK(strstr(out, "workq") != NULL);
    g_ipc_trace = 0;
    lsk_thread_reg_reset();
}

static void test_registry_table_full(void) {
    lsk_thread_reg_reset();
    for (int i = 0; i < 128; i++) {
        lsk_thread_reg_add(1000 + i, (void *)(uintptr_t)(0x1000 + i),
                           NULL, NULL, 0x3000 + (uint32_t)i * 4, 0);
    }
    /* 129th add: table full, fails silent, no crash. */
    lsk_thread_reg_add(9999, (void *)(uintptr_t)0x9999, NULL, NULL, 0x9999, 0);
    cap_reset();
    lsk_thread_dump();
    const char *out = g_cap_buf;
    CHECK(count_substr(out, "[lsk THREAD]") == 128);
    CHECK(strstr(out, "kport=0x9999") == NULL);
    /* Slot reuse after removal. */
    lsk_thread_reg_remove(0x3000);
    lsk_thread_reg_add(9999, (void *)(uintptr_t)0x9999, NULL, NULL, 0x9999, 0);
    cap_reset();
    lsk_thread_dump();
    out = g_cap_buf;
    CHECK(count_substr(out, "[lsk THREAD]") == 128);
    CHECK(strstr(out, "kport=0x9999") != NULL);
    lsk_thread_reg_reset();
}

typedef struct {
    int id;
    int iters;
} worker_arg_t;

static void *registry_worker(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;
    for (int i = 0; i < a->iters; i++) {
        uint32_t kport = 0x4000 + (uint32_t)(a->id * 100000 + i * 4);
        lsk_wq_thread_on_spawn_wlh((void *)(uintptr_t)(0x10000 + i),
                                   kport, WQ_FLAG_THREAD_KEVENT, 0);
        if ((i % 7) == 0) lsk_thread_dump();
        lsk_thread_reg_remove(kport);
    }
    return NULL;
}

static void test_registry_concurrent(void) {
    lsk_thread_reg_reset();
    g_ipc_trace = 0;
    const int nthreads = 8;
    const int iters = 500;
    pthread_t th[nthreads];
    worker_arg_t args[nthreads];
    for (int i = 0; i < nthreads; i++) {
        args[i].id = i;
        args[i].iters = iters;
        CHECK(pthread_create(&th[i], NULL, registry_worker, &args[i]) == 0);
    }
    for (int i = 0; i < nthreads; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    /* Every worker removed everything it added: the dump is empty. */
    cap_reset();
    lsk_thread_dump();
    const char *out = g_cap_buf;
    CHECK(strstr(out, "[lsk THREADS] live threads:") != NULL);
    CHECK(count_substr(out, "[lsk THREAD]") == 0);
    lsk_thread_reg_reset();
}

/* ------------------------------------------------------------------ */
/* Part 2: display-link pump contract (test-local model)               */
/*                                                                     */
/* The pump replacement must satisfy this contract: a bounded         */
/* commit queue; producers (CA commits) enqueue while the consumer    */
/* (display link) renders; no commit is lost or applied twice; the    */
/* consumer drains the queue before exiting; damage rects coalesce   */
/* into one union rect clipped to the frame.                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t x, y;
    uint32_t w, h;
} dmg_rect_t;

#define COMMIT_QUEUE_LEN 4
#define FRAME_W 720
#define FRAME_H 1600

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    dmg_rect_t rects[COMMIT_QUEUE_LEN];
    int head, len;
    int producers_done;
    uint64_t enqueued;
    uint64_t rendered;
    uint64_t coalesced;
    uint64_t frames;
} commit_queue_t;

static void cq_init(commit_queue_t *q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->head = 0;
    q->len = 0;
    q->producers_done = 0;
    q->enqueued = 0;
    q->rendered = 0;
    q->coalesced = 0;
    q->frames = 0;
}

static void cq_destroy(commit_queue_t *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* Clip a rect to the frame; 0 = non-empty after clip, -1 = empty. */
static int clip_rect(const dmg_rect_t *in, dmg_rect_t *out) {
    int64_t x0 = in->x, y0 = in->y;
    int64_t x1 = (int64_t)in->x + (int64_t)in->w;
    int64_t y1 = (int64_t)in->y + (int64_t)in->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FRAME_W) x1 = FRAME_W;
    if (y1 > FRAME_H) y1 = FRAME_H;
    if (x1 <= x0 || y1 <= y0) return -1;
    out->x = (int32_t)x0;
    out->y = (int32_t)y0;
    out->w = (uint32_t)(x1 - x0);
    out->h = (uint32_t)(y1 - y0);
    return 0;
}

/* Union of two rects (both already clipped and non-empty). */
static void union_rect(const dmg_rect_t *a, const dmg_rect_t *b, dmg_rect_t *out) {
    int32_t x0 = a->x < b->x ? a->x : b->x;
    int32_t y0 = a->y < b->y ? a->y : b->y;
    int32_t x1 = (a->x + (int32_t)a->w) > (b->x + (int32_t)b->w)
                     ? (a->x + (int32_t)a->w) : (b->x + (int32_t)b->w);
    int32_t y1 = (a->y + (int32_t)a->h) > (b->y + (int32_t)b->h)
                     ? (a->y + (int32_t)a->h) : (b->y + (int32_t)b->h);
    out->x = x0;
    out->y = y0;
    out->w = (uint32_t)(x1 - x0);
    out->h = (uint32_t)(y1 - y0);
}

static void cq_enqueue(commit_queue_t *q, dmg_rect_t r) {
    pthread_mutex_lock(&q->mu);
    while (q->len == COMMIT_QUEUE_LEN) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    int idx = (q->head + q->len) % COMMIT_QUEUE_LEN;
    q->rects[idx] = r;
    q->len++;
    q->enqueued++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

/* Consumer: drain the queue into one coalesced damage rect per frame.
 * Returns 0 when all producers are done and the queue is empty. */
static void *cq_consumer(void *arg) {
    commit_queue_t *q = (commit_queue_t *)arg;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (q->len == 0 && !q->producers_done) {
            pthread_cond_wait(&q->not_empty, &q->mu);
        }
        if (q->len == 0 && q->producers_done) {
            pthread_mutex_unlock(&q->mu);
            return NULL;
        }
        /* Coalesce everything currently queued into one frame. */
        dmg_rect_t acc;
        int have = 0;
        while (q->len > 0) {
            dmg_rect_t r = q->rects[q->head];
            q->head = (q->head + 1) % COMMIT_QUEUE_LEN;
            q->len--;
            q->rendered++;
            dmg_rect_t c;
            if (clip_rect(&r, &c) != 0) continue;
            if (!have) { acc = c; have = 1; }
            else union_rect(&acc, &c, &acc);
            q->coalesced++;
        }
        pthread_cond_broadcast(&q->not_full);
        pthread_mutex_unlock(&q->mu);
        if (have) {
            /* A rendered frame's damage must stay inside the frame. */
            assert(acc.x >= 0 && acc.y >= 0);
            assert(acc.x + (int32_t)acc.w <= FRAME_W);
            assert(acc.y + (int32_t)acc.h <= FRAME_H);
            __atomic_fetch_add(&q->frames, 1, __ATOMIC_RELAXED);
        }
    }
}

typedef struct {
    commit_queue_t *q;
    int id;
    int n;
} producer_arg_t;

static void *cq_producer(void *arg) {
    producer_arg_t *a = (producer_arg_t *)arg;
    for (int i = 0; i < a->n; i++) {
        dmg_rect_t r;
        r.x = (a->id * 137 + i * 61) % FRAME_W;
        r.y = (a->id * 211 + i * 97) % FRAME_H;
        r.w = 10 + (uint32_t)((i * 7) % 300);
        r.h = 10 + (uint32_t)((i * 11) % 200);
        cq_enqueue(a->q, r);
    }
    return NULL;
}

static void test_pump_commit_queue(void) {
    commit_queue_t q;
    cq_init(&q);
    pthread_t cons;
    CHECK(pthread_create(&cons, NULL, cq_consumer, &q) == 0);
    const int nprod = 4;
    const int ncommits = 200;
    pthread_t prod[nprod];
    producer_arg_t pargs[nprod];
    for (int i = 0; i < nprod; i++) {
        pargs[i].q = &q;
        pargs[i].id = i;
        pargs[i].n = ncommits;
        CHECK(pthread_create(&prod[i], NULL, cq_producer, &pargs[i]) == 0);
    }
    for (int i = 0; i < nprod; i++) {
        CHECK(pthread_join(prod[i], NULL) == 0);
    }
    pthread_mutex_lock(&q.mu);
    q.producers_done = 1;
    pthread_cond_signal(&q.not_empty);
    pthread_mutex_unlock(&q.mu);
    CHECK(pthread_join(cons, NULL) == 0);
    /* No commit lost, none applied twice, all drained before exit. */
    CHECK(q.enqueued == (uint64_t)(nprod * ncommits));
    CHECK(q.rendered == q.enqueued);
    CHECK(q.frames >= 1);
    CHECK(q.frames <= q.enqueued);
    CHECK(q.len == 0);
    cq_destroy(&q);
}

static void test_damage_clip_union(void) {
    dmg_rect_t out;
    dmg_rect_t in;
    /* Fully outside: empty. */
    in.x = 900; in.y = 0; in.w = 10; in.h = 10;
    CHECK(clip_rect(&in, &out) == -1);
    /* Negative origin clips to 0. */
    in.x = -50; in.y = -20; in.w = 100; in.h = 100;
    CHECK(clip_rect(&in, &out) == 0);
    CHECK(out.x == 0 && out.y == 0 && out.w == 50 && out.h == 80);
    /* Past the far edge clips to the frame. */
    in.x = 700; in.y = 1500; in.w = 100; in.h = 200;
    CHECK(clip_rect(&in, &out) == 0);
    CHECK(out.x == 700 && out.y == 1500 && out.w == 20 && out.h == 100);
    /* Zero-size rect is empty. */
    in.x = 10; in.y = 10; in.w = 0; in.h = 5;
    CHECK(clip_rect(&in, &out) == -1);
    /* Union of two disjoint rects spans both. */
    dmg_rect_t a = { .x = 0, .y = 0, .w = 10, .h = 10 };
    dmg_rect_t b = { .x = 100, .y = 200, .w = 20, .h = 30 };
    union_rect(&a, &b, &out);
    CHECK(out.x == 0 && out.y == 0 && out.w == 120 && out.h == 230);
    /* Union of overlapping rects is the bounding box, not the sum. */
    dmg_rect_t c = { .x = 5, .y = 5, .w = 10, .h = 10 };
    union_rect(&a, &c, &out);
    CHECK(out.x == 0 && out.y == 0 && out.w == 15 && out.h == 15);
}

int main(void) {
    /* Unbuffered stdout: CHECK failures must survive a sanitizer abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    g_thread_test_write_hook = capture_write;
    test_registry_basic();
    test_registry_trace_line();
    test_registry_table_full();
    test_registry_concurrent();
    test_pump_commit_queue();
    test_damage_clip_union();
    printf("pump_workqueue_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
