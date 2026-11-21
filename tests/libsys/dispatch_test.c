#include <stdint.h>
#include <stddef.h>

/* libSystem declarations */
int printf(const char *fmt, ...);
int fflush(void *stream);
int write(int fd, const void *buf, size_t count);
int read(int fd, void *buf, size_t count);
int pipe(int fildes[2]);
int close(int fd);
void exit(int status);

/* libdispatch prototypes */
typedef struct dispatch_queue_s *dispatch_queue_t;
typedef struct dispatch_semaphore_s *dispatch_semaphore_t;
typedef struct dispatch_group_s *dispatch_group_t;
typedef struct dispatch_source_s *dispatch_source_t;
typedef const struct dispatch_source_type_s *dispatch_source_type_t;
typedef uint64_t dispatch_time_t;
typedef void (*dispatch_function_t)(void *);

extern struct dispatch_queue_s _dispatch_main_q;
extern const struct dispatch_source_type_s _dispatch_source_type_timer;
extern const struct dispatch_source_type_s _dispatch_source_type_read;

dispatch_queue_t dispatch_get_global_queue(intptr_t identifier, uintptr_t flags);
dispatch_queue_t dispatch_queue_create(const char *label, void *attr);
dispatch_semaphore_t dispatch_semaphore_create(intptr_t value);
intptr_t dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout);
intptr_t dispatch_semaphore_signal(dispatch_semaphore_t dsema);
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
void dispatch_async_f(dispatch_queue_t queue, void *context, dispatch_function_t work);
void dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void *ctx, dispatch_function_t work);
dispatch_group_t dispatch_group_create(void);
void dispatch_group_async_f(dispatch_group_t group, dispatch_queue_t queue, void *ctx, dispatch_function_t work);
intptr_t dispatch_group_wait(dispatch_group_t group, dispatch_time_t timeout);
dispatch_source_t dispatch_source_create(dispatch_source_type_t type, uintptr_t handle, uintptr_t mask, dispatch_queue_t q);
void dispatch_source_set_timer(dispatch_source_t source, dispatch_time_t start, uint64_t interval, uint64_t leeway);
void dispatch_source_set_event_handler_f(dispatch_source_t source, dispatch_function_t handler);
void dispatch_source_cancel(dispatch_source_t source);
void dispatch_set_context(void *object, void *context);
void dispatch_resume(dispatch_source_t object);

/* CoreFoundation prototypes */
typedef void *CFRunLoopRef;
typedef void *CFRunLoopTimerRef;
typedef const void *CFStringRef;
typedef const void *CFAllocatorRef;
extern const CFStringRef kCFRunLoopDefaultMode;
extern const CFAllocatorRef kCFAllocatorDefault;

typedef struct {
    intptr_t version;
    void *info;
    const void *(*retain)(const void *info);
    void (*release)(const void *info);
    void *(*copyDescription)(const void *info);
} CFRunLoopTimerContext;

CFRunLoopRef CFRunLoopGetCurrent(void);
void CFRunLoopRun(void);
void CFRunLoopStop(CFRunLoopRef rl);
double CFAbsoluteTimeGetCurrent(void);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, double fireDate, double interval,
                                       uint64_t flags, int64_t order,
                                       void (*callout)(CFRunLoopTimerRef, void *),
                                       CFRunLoopTimerContext *context);
void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer, CFStringRef mode);

/* Helper signal callback */
static void signal_cb(void *ctx) {
    dispatch_semaphore_signal((dispatch_semaphore_t)ctx);
}

/* Check 1: dispatch_async_f on global queue */
static void test_check1(void) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_async_f(q, sem, signal_cb);

    intptr_t res = dispatch_semaphore_wait(sem, dispatch_time(0, 5000000000ULL));
    if (res != 0) {
        printf("FAIL 1 dispatch_async_global (timeout)\n");
        exit(1);
    }
    printf("ok 1 dispatch_async_global\n");
}

/* Check 2: dispatch_after_f 50 ms on global queue */
static void test_check2(void) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_after_f(dispatch_time(0, 50000000ULL), q, sem, signal_cb);

    intptr_t res = dispatch_semaphore_wait(sem, dispatch_time(0, 5000000000ULL));
    if (res != 0) {
        printf("FAIL 2 dispatch_after (timeout)\n");
        exit(1);
    }
    printf("ok 2 dispatch_after\n");
}

/* Check 3: dispatch_group_async_f of 100 blocks */
static void group_inc_cb(void *ctx) {
    int *counter = (int *)ctx;
    __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
}

static void test_check3(void) {
    int counter = 0;
    dispatch_group_t grp = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);

    for (int i = 0; i < 100; i++) {
        dispatch_group_async_f(grp, q, &counter, group_inc_cb);
    }

    intptr_t res = dispatch_group_wait(grp, dispatch_time(0, 5000000000ULL));
    if (res != 0 || counter != 100) {
        printf("FAIL 3 dispatch_group_100 (res=%ld count=%d)\n", (long)res, counter);
        exit(1);
    }
    printf("ok 3 dispatch_group_100\n");
}

/* Check 4: dispatch_source_create(&_dispatch_source_type_timer, ...) on serial queue fires 3x */
struct check4_ctx {
    int count;
    dispatch_semaphore_t sem;
    dispatch_source_t src;
};

static void timer_cb(void *arg) {
    struct check4_ctx *c = (struct check4_ctx *)arg;
    if (c) {
        c->count++;
        if (c->count == 3) {
            dispatch_source_cancel(c->src);
            dispatch_semaphore_signal(c->sem);
        }
    }
}

static void test_check4(void) {
    struct check4_ctx c;
    c.count = 0;
    c.sem = dispatch_semaphore_create(0);
    dispatch_queue_t sq = dispatch_queue_create("test.timer.queue", NULL);
    c.src = dispatch_source_create(&_dispatch_source_type_timer, 0, 0, sq);

    dispatch_set_context(c.src, &c);
    dispatch_source_set_event_handler_f(c.src, timer_cb);
    dispatch_source_set_timer(c.src, dispatch_time(0, 20000000ULL), 20000000ULL, 0);
    dispatch_resume(c.src);

    intptr_t res = dispatch_semaphore_wait(c.sem, dispatch_time(0, 5000000000ULL));
    if (res != 0 || c.count < 3) {
        printf("FAIL 4 dispatch_source_timer_serial (res=%ld count=%d)\n", (long)res, c.count);
        exit(1);
    }
    printf("ok 4 dispatch_source_timer_serial\n");
}

/* Check 5: Main queue + CFRunLoopRun with CFRunLoopTimer (3 fires -> CFRunLoopStop) */
struct check5_ctx {
    int main_count;
    CFRunLoopRef rl;
};

static void main_q_cb(void *arg) {
    struct check5_ctx *c = (struct check5_ctx *)arg;
    c->main_count++;
    if (c->main_count == 3) {
        CFRunLoopStop(c->rl);
    }
}

static void cf_timer_callout(CFRunLoopTimerRef t, void *info) {
    (void)t;
    struct check5_ctx *c = (struct check5_ctx *)info;
    dispatch_async_f(&_dispatch_main_q, c, main_q_cb);
}

static void cf_watchdog_cb(void *arg) {
    CFRunLoopRef rl = (CFRunLoopRef)arg;
    CFRunLoopStop(rl);
}

static void test_check5(void) {
    struct check5_ctx c;
    c.main_count = 0;
    c.rl = CFRunLoopGetCurrent();

    CFRunLoopTimerContext tc;
    tc.version = 0;
    tc.info = &c;
    tc.retain = NULL;
    tc.release = NULL;
    tc.copyDescription = NULL;

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
                                                   CFAbsoluteTimeGetCurrent() + 0.02,
                                                   0.02, 0, 0,
                                                   cf_timer_callout, &tc);
    CFRunLoopAddTimer(c.rl, timer, kCFRunLoopDefaultMode);

    /* 5s watchdog on global queue */
    dispatch_after_f(dispatch_time(0, 5000000000ULL),
                     dispatch_get_global_queue(0, 0),
                     c.rl, cf_watchdog_cb);

    CFRunLoopRun();

    if (c.main_count != 3) {
        printf("FAIL 5 cfrunloop_timer_main_queue (count=%d)\n", c.main_count);
        exit(1);
    }
    printf("ok 5 cfrunloop_timer_main_queue\n");
}

/* Check 6: dispatch_source_create(&_dispatch_source_type_read, pipe_rfd, ...) */
struct check6_ctx {
    int rfd;
    dispatch_semaphore_t sem;
    dispatch_source_t src;
    int ok;
};

static void pipe_read_cb(void *arg) {
    struct check6_ctx *c = (struct check6_ctx *)arg;
    char buf[16];
    int n = read(c->rfd, buf, sizeof(buf));
    if (n == 2 && buf[0] == 'o' && buf[1] == 'k') {
        c->ok = 1;
    }
    dispatch_source_cancel(c->src);
    dispatch_semaphore_signal(c->sem);
}

static void test_check6(void) {
    int fds[2];
    if (pipe(fds) < 0) {
        printf("FAIL 6 dispatch_source_read_pipe (pipe failed)\n");
        exit(1);
    }

    struct check6_ctx c;
    c.rfd = fds[0];
    c.sem = dispatch_semaphore_create(0);
    c.ok = 0;

    dispatch_queue_t sq = dispatch_queue_create("test.pipe.queue", NULL);
    c.src = dispatch_source_create(&_dispatch_source_type_read, (uintptr_t)fds[0], 0, sq);
    dispatch_set_context(c.src, &c);

    dispatch_source_set_event_handler_f(c.src, pipe_read_cb);
    dispatch_resume(c.src);

    write(fds[1], "ok", 2);

    intptr_t res = dispatch_semaphore_wait(c.sem, dispatch_time(0, 5000000000ULL));
    close(fds[0]);
    close(fds[1]);

    if (res != 0 || c.ok != 1) {
        printf("FAIL 6 dispatch_source_read_pipe (res=%ld ok=%d)\n", (long)res, c.ok);
        exit(1);
    }
    printf("ok 6 dispatch_source_read_pipe\n");
}

int main(void) {
    test_check1();
    test_check2();
    test_check3();
    test_check4();
    test_check5();
    test_check6();

    printf("dispatch ok\n");
    return 0;
}
