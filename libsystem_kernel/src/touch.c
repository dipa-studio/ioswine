/*
 * touch.c - Presenter touch reader and UIKit IOHIDEvent injector.
 *
 * Activated when IOSWINE_TOUCH=1 or IOSWINE_TOUCH_SELFTEST=1 (and IOSWINE_SNAPSHOT=1).
 * Reads ISWS_MSG_TOUCH events from the @ioswine-surface connection (via
 * present.c), normalizes client-pixel coordinates to [0,1], posts them via
 * dispatch_async_f to _dispatch_main_q, and in touch_inject() builds an
 * IOHIDEvent digitizer collection (Hand parent + Finger children) and
 * delivers it to UIKit.
 *
 * Delivery routes:
 *   D (default): the combined path. -[UIEventFetcher
 *      _receiveHIDEventInternal:] is called directly on the main queue
 *      (synchronously, no fetch-thread hop), then the dispatcher's
 *      main-runloop source is signalled and the main runloop woken, then
 *      drainEventsIntoEnvironment: and processEventQueue(env, 4) are
 *      called directly. If no -[UIApplication sendEvent:] delivery is
 *      observed for this event, route C runs as a fallback for the same
 *      phase. IOSWINE_TOUCH_INTERNAL=1 selects this same path (the old
 *      route B name is kept as an alias so old devq lines still work).
 *   A (IOSWINE_TOUCH_QUEUE=1): -[UIEventFetcher _receiveHIDEvent:] -
 *      the thread-safe wrapper: it retains the event and performs the
 *      block onto the com.apple.uikit.eventfetch-thread, which runs
 *      _receiveHIDEventInternal:, signals the dispatcher, and the main
 *      runloop source drains into the environment. Kept only for A/B
 *      comparison. DEAD ON DEVICE: every event logged
 *      "queued via _receiveHIDEvent:" and "queue filtered=0 raw=-" and
 *      nothing ever reached sendEvent:, because the fetch-thread block
 *      never runs in this environment (no HID source feeds that
 *      thread's runloop) and the drain never happens.
 *   C (IOSWINE_TOUCH_DIRECT=1, fallback): synthesize a UITouchesEvent
 *      from UITouch objects and call -[UIApplication sendEvent:] directly,
 *      bypassing the HID layer and the _UIEventDeferringManager entirely.
 *      Also the fallback of route D when the HID chain drops an event.
 *      The touch state (phase, location, timestamp, tap
 *      count) is written through the setters when they respond, and
 *      straight into the UITouch ivars when they do not, so UIControl
 *      begin/continue/endTracking: runs and fires the action itself.
 *      The old interim TouchUpInside hack is behind
 *      IOSWINE_TOUCH_TAP_HACK=1 (default off): with real tracking it
 *      would double-fire every tap.
 *
 * Coordinate rule (the attempt-2 bug): IOHIDEvent digitizer coordinates
 * are NORMALIZED [0,1]; UIKit multiplies by the screen size when it builds
 * a UITouch. touch.c used to pass window points (107, 791) to the event
 * constructors, which put the touch far outside every view. The event
 * constructors now receive normalized coordinates; the "[touch] down x=.."
 * log line still prints window points for humans.
 *
 * Diagnostic logging with IOSWINE_TOUCH_TRACE=1. Trace hooks (installed
 * only in trace mode) wrap -[UIEventFetcher drainEventsIntoEnvironment:],
 * -[UIEventDispatcher eventFetcherDidReceiveEvents:], -[UIApplication
 * sendEvent:] and -[UIWindow sendEvent:] and log each stage, plus the
 * _incomingHIDEventsFiltered queue count, so a device run names the exact
 * stage where an event stops.
 * Self-test with IOSWINE_TOUCH_SELFTEST=1 (after frame 20, tap Browse tab).
 *
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc beyond
 * <stdint.h>/<stddef.h>, no malloc, no __thread, no static fn-ptr tables).
 */
#include "touch.h"
#include "present.h"
#include "keyboard.h"

#ifndef TOUCH_HOST_TEST
#include "linux.h"
extern const char *lsk_getenv(const char *name);
extern uint64_t mach_absolute_time(void);
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define TOUCH_DEFAULT_WIN_W     360.0
#define TOUCH_DEFAULT_WIN_H     800.0
#define TOUCH_RING_LEN          256u
#define TOUCH_POLL_TIMEOUT_MS   50

/* IOKit / IOHIDFamily constants */
#define TOUCH_TRANSDUCER_FINGER 2u
#define TOUCH_TRANSDUCER_HAND   3u

/* IOHIDDigitizerEventMask bits */
#define TOUCH_MASK_RANGE        0x1u
#define TOUCH_MASK_TOUCH        0x2u
#define TOUCH_MASK_POSITION     0x4u
#define TOUCH_MASK_CANCEL       0x80u

/* Options and field IDs */
#define TOUCH_OPT_COLLECTION    0x00002u
#define TOUCH_OPT_DISP_INTEG    0x80000u
#define TOUCH_FIELD_CONTEXT_ID  0xb0005u /* [guess] context id field */
#define TOUCH_FIELD_DISP_INTEG  0xb0019u /* [guess] display integrated field */
/* Sender ID used only when IOSWINE_TOUCH_SENDER_ID=magic. It is a
 * per-device IORegistry entry ID copied from vphoned docs, not this
 * device's digitizer ID; the default sender ID is the key window's
 * _contextId instead (see touch_choose_sender_id). */
#define TOUCH_SENDER_ID_MAGIC   0x8000000817319a7eULL

/* UITouchPhase values (UITouchPhaseBegan=0, Moved=1, Stationary=2,
 * Ended=3, Cancelled=4). */
#define TOUCH_PHASE_BEGAN       0
#define TOUCH_PHASE_MOVED       1
#define TOUCH_PHASE_ENDED       3
#define TOUCH_PHASE_CANCELLED   4

/* Pinned 23G90 cache addresses.
 * TOUCH_PROCESS_EVENT_QUEUE_ADDR is the 052 address for
 * -[UIEventEnvironment processEventQueue:]; the combined route calls it
 * directly after the drain, which is the step route B was missing. */
#define TOUCH_RECEIVE_INTERNAL_ADDR 0x1891353b0ULL
#define TOUCH_DRAIN_EVENTS_ADDR     0x18a4c3b70ULL
#define TOUCH_PROCESS_EVENT_QUEUE_ADDR 0x18921f274ULL

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache (same as snapshot.c) */
#define TOUCH_GAPIS_ADDR        0x1ec954040ULL
#define TOUCH_GAPIS_OBJ_DISC    0x63faULL
#define TOUCH_SLOT_DLOPEN       13ULL
#define TOUCH_DIV_DLOPEN        0xbf31ULL
#define TOUCH_SLOT_DLSYM        16ULL
#define TOUCH_DIV_DLSYM         0x839dULL

/* ------------------------------------------------------------------ */
/* Runtime state (zero-initialized in .bss)                           */
/* ------------------------------------------------------------------ */

#ifdef TOUCH_HOST_TEST
static const touch_test_ops_t *g_test_ops;
#endif

typedef void (*send_event_fn)(void *self, void *cmd, void *event);

typedef struct {
    int dispatch_resolved;
    void *dispatch_main_q;
    void (*dispatch_async_f)(void *queue, void *ctx, void (*work)(void *));
    int (*pthread_create)(void *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);

    int inject_resolved;
    void *(*objc_getClass)(const char *name);
    void *(*sel_registerName)(const char *name);
    void *objc_msgSend;
    void *(*objc_autoreleasePoolPush)(void);
    void (*objc_autoreleasePoolPop)(void *pool);

    void *(*object_getClass)(void *obj);
    void *(*class_getInstanceMethod)(void *cls, void *sel);
    void *(*method_setImplementation)(void *m, void *imp);
    void *(*class_getInstanceVariable)(void *cls, const char *name);
    long (*ivar_getOffset)(void *iv);
    void *(*object_setIvar)(void *obj, void *iv, void *value);

    void (*receive_internal)(void *fetcher, void *event);
    void (*drain_events)(void *fetcher, void *environment);
    /* Direct -[UIEventEnvironment processEventQueue:] (limit 4, as in
     * 052). Called by the combined route after the drain; this is the
     * step that actually dispatches queued events to sendEvent:. */
    void (*process_event_queue)(void *environment, uint32_t limit);

    /* BackBoardServices digitizer-info setter (WebKit HIDEventGenerator /
     * Chromium shape): stamps the context id onto the event the way
     * backboardd does. Restored after a guessed IOHID field (0xb0005) left the event
     * without a valid context id. Signature per WebKit/Chromium: last arg is float. */
    void (*BKSHIDEventSetDigitizerInfo)(
        void *event, uint32_t contextID,
        uint8_t systemGesturesPossible, uint8_t systemGestureStateChange,
        void *displayUUID, double initialTouchTimestamp, float maximumForce);

    void (*CFRunLoopSourceSignal)(void *source);
    void (*CFRunLoopWakeUp)(void *rl);
    void *(*CFRunLoopGetMain)(void);

    void *(*IOHIDEventCreateDigitizerEvent)(
        void *allocator, uint64_t timestamp, uint32_t transducerType,
        uint32_t index, uint32_t identity, uint32_t eventMask, uint32_t buttonMask,
        double x, double y, double z, double tipPressure, double twist,
        uint32_t range, uint32_t touch, uint32_t options);
    void *(*IOHIDEventCreateDigitizerFingerEvent)(
        void *allocator, uint64_t timestamp, uint32_t index, uint32_t identity,
        uint32_t eventMask, double x, double y, double z,
        double tipPressure, double twist,
        uint32_t range, uint32_t touch, uint32_t options);
    void (*IOHIDEventAppendEvent)(void *parent, void *child, uint32_t options);
    void (*IOHIDEventSetIntegerValue)(void *event, uint32_t field, int64_t value);
    void (*IOHIDEventSetSenderID)(void *event, uint64_t senderID);
    void (*CFRelease)(void *cf);

    void *sel_respondsToSelector;
    void *sel_sharedApplication;
    void *sel_eventFetcher;
    void *sel_receiveHIDEvent;
    void *sel_keyWindow;
    void *sel_windows;
    void *sel_count;
    void *sel_objectAtIndex;
    void *sel_connectedScenes;
    void *sel_allObjects;
    void *sel_rootViewController;
    void *sel_tabBarController;
    void *sel_selectedIndex;
    void *sel_viewControllers;
    void *sel_childViewControllers;
    void *sel_allTouches;
    void *sel_sendEvent;
    void *sel_contextId;
    void *sel_layer;
    void *sel_context;
    void *sel_layerContextId;
    void *sel_window;
    void *sel_view;
    void *sel_hitTest;
    void *sel_setWindow;
    void *sel_setView;
    void *sel_setPhase;
    void *sel_setTimestamp;
    void *sel_setTapCount;
    void *sel_setLocationInWindow;
    void *sel_setHidEvent;
    void *sel_setIsFirstTouchForView;
    void *sel_setInitialTouchTimestamp;
    void *sel_setTouchesPriv;
    void *sel_setTouches;
    void *sel_setWithObject;
    void *sel_alloc;
    void *sel_init;
    void *sel_drainEvents;
    void *sel_dispatcherReceived;
    void *sel_setEventTimestamp;
} touch_syms_t;

typedef struct {
    volatile int in_use;
    uint32_t action;
    uint32_t pointer_id;
    uint32_t pointer_count;
    double norm_x;
    double norm_y;
    int64_t timestamp_ns;
} touch_slot_t;

typedef struct {
    uint8_t active;
    double wx;
    double wy;
    double nx; /* normalized [0,1] x, the value the HID event carries */
    double ny; /* normalized [0,1] y, the value the HID event carries */
} touch_finger_t;

static touch_syms_t g_tsyms;
static touch_slot_t g_slots[TOUCH_RING_LEN];
static touch_finger_t g_fingers[ISWS_MAX_POINTERS];
static uint32_t g_slot_next;
static int g_started;
static int g_logged_missing_fetcher;
static int g_logged_missing_syms;

static int g_touch_trace;
static int g_touch_selftest;
static int g_touch_direct;
static int g_touch_tap_hack; /* IOSWINE_TOUCH_TAP_HACK=1: interim TouchUpInside hack, default off */
static int g_touch_internal;
static int g_touch_queue; /* IOSWINE_TOUCH_QUEUE=1: old route A, for A/B only */
static int g_sender_override;
static uint64_t g_sender_override_value;
/* Context-id override: IOSWINE_TOUCH_CTX_ID forces the
 * context id stamped on the HID event (decimal or 0x-hex, parsed by
 * touch_parse_u64); IOSWINE_TOUCH_CTX_FIELD=1 also writes the guessed
 * 0xb0005 field (default off, see touch_resolve_context_id). */
static int g_ctx_override;
static uint64_t g_ctx_override_value;
static int g_ctx_field;
static int g_ctx_logged;
static uint32_t g_ctx_last_logged;
static int g_sendevent_hooked;
static int g_pipeline_hooks_installed;
static int g_selftest_done;
static send_event_fn g_sendEvent_orig;

/* Deliveries of -[UIApplication sendEvent:] observed through the
 * hook, counted whether or not tracing is on (the hook is installed
 * whenever tracing or the selftest is on). The selftest reports this
 * count next to the tab index: it is the end-to-end proof that a
 * synthesized tap reached sendEvent:, independent of which tab the
 * app shows). */
static uint64_t g_sendevent_count;

/* MOVE log sampling (F10): the first MOVE after a DOWN logs, then
 * every 16th MOVE. DOWN/UP/CANCEL always log. */
static uint32_t g_move_log_n;

/* Drop counters (F9). Read on host via touch_test_drop_counts();
 * on device they are visible only through the trace-gated ring-full
 * line in enqueue_touch_for_main(). */
static uint64_t g_drop_ring_full;
static uint64_t g_drop_invalid;
static uint64_t g_drop_stale;
static uint64_t g_cancel_all_sent;

/* Last accepted wire timestamp per pointer, for the stale-event
 * guard (F8). The wire timestamp is Linux CLOCK_MONOTONIC, the wrong
 * epoch for an IOHIDEvent, so touch_inject() still uses
 * mach_absolute_time() for the event itself; the wire timestamp is
 * only used to order events from the same pointer. */
static int64_t g_last_wire_ts[ISWS_MAX_POINTERS];
static uint8_t g_wire_ts_set[ISWS_MAX_POINTERS];

/* Reader-thread disconnect tracking (F7). */
static int g_reader_was_connected;

/* Orig IMPs for the trace hooks (chained, never dropped). */
typedef void (*drain_fn)(void *self, void *cmd, void *env);
typedef void (*dispatcher_received_fn)(void *self, void *cmd, void *arg);
static drain_fn g_drain_orig;
static dispatcher_received_fn g_dispatcher_received_orig;
static send_event_fn g_window_sendEvent_orig;

/* Direct-path UITouch objects, one per pointer id, created on DOWN and
 * reused for MOVE/UP so UIKit sees one continuous touch. The object is
 * owned by the UITouchesEvent's NSSet; the table holds a borrowed
 * pointer and is cleared on UP/CANCEL. */
static void *g_direct_touches[ISWS_MAX_POINTERS];
static uint64_t g_direct_touch_count;
/* The view the touch began on, one per pointer id.
 * UIKit assigns a touch its view at Began and keeps it for the whole
 * gesture: a drag-out still delivers Moved/Ended to the control that
 * began tracking, so the control can un-highlight and cancel. The
 * injector hit-tests every phase (the hit-test count is pinned by
 * test_direct_route), but the view it writes into the touch must be
 * the Began view, not the current hit-test result: on a drag-out the
 * current point hit-tests the window, and writing that into the
 * touch would hand the MOVE/UP to the window, leaving the control
 * tracking forever (highlight stuck, endTracking: never runs). */
static void *g_direct_views[ISWS_MAX_POINTERS];
/* Last location set for each direct-path pointer, in window points.
 * The ivar fallback for _previousLocationInWindow needs it: with
 * resetPrevious=NO the previous location is the old current one, and
 * the touch object is the only other place it lives. */
static double g_direct_prev_x[ISWS_MAX_POINTERS];
static double g_direct_prev_y[ISWS_MAX_POINTERS];

static volatile int g_win_lock;
static int g_win_size_set;
static double g_win_w;
static double g_win_h;

/* ------------------------------------------------------------------ */
/* Freestanding helpers                                               */
/* ------------------------------------------------------------------ */

static size_t touch_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

#ifdef TOUCH_HOST_TEST
static void touch_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}
#endif

static int is_finite_f32(float v) {
    union {
        float f;
        uint32_t u;
    } x;
    x.f = v;
    return ((x.u >> 23) & 0xffu) != 0xffu;
}

static int is_finite_f64(double v) {
    union {
        double d;
        uint64_t u;
    } x;
    x.d = v;
    return ((x.u >> 52) & 0x7ffu) != 0x7ffu;
}

static const char *touch_getenv(const char *name) {
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->getenv_fn) return g_test_ops->getenv_fn(name);
    return NULL;
#else
    return lsk_getenv(name);
#endif
}

static void touch_log_write(const char *buf, size_t len) {
    if (!buf || len == 0) return;
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->log_fn) {
        g_test_ops->log_fn(buf, len);
    }
#else
    sys_write(2, buf, len);
#endif
}

static void touch_log_str(const char *s) {
    touch_log_write(s, touch_strlen(s));
}

static size_t fmt_u64(char *out, uint64_t val) {
    char tmp[24];
    size_t n = 0;
    if (val == 0) {
        out[0] = '0';
        return 1;
    }
    while (val > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

/* Format a double as milli-units (x1000, rounded) with a sign, e.g.
 * "-1234" for -1.234. Used by trace lines that print window points. */
static size_t fmt_milli(char *out, double v) {
    size_t n = 0;
    uint64_t mag;
    if (v < 0.0) {
        out[n++] = '-';
        mag = (uint64_t)(-v * 1000.0 + 0.5);
    } else {
        mag = (uint64_t)(v * 1000.0 + 0.5);
    }
    return n + fmt_u64(out + n, mag);
}

static void touch_log_event(uint32_t action, uint32_t pointer_id,
                            double wx, double wy) {
    char buf[96];
    size_t pos = 0;
    const char *prefix = "[touch] ";
    const char *act = "unknown";
    int64_t ix = (int64_t)(wx + (wx >= 0.0 ? 0.5 : -0.5));
    int64_t iy = (int64_t)(wy + (wy >= 0.0 ? 0.5 : -0.5));

    /* MOVE log sampling (F10): log the first MOVE after a DOWN, then
     * every 16th MOVE. A drag at 60 Hz used to emit 60 lines a
     * second. DOWN/UP/CANCEL always log. */
    if (action == ISWS_TOUCH_MOVE) {
        if ((g_move_log_n % 16u) != 0) {
            g_move_log_n++;
            return;
        }
        g_move_log_n++;
    } else if (action == ISWS_TOUCH_DOWN) {
        g_move_log_n = 0;
    }

    if (action == ISWS_TOUCH_DOWN) act = "down";
    else if (action == ISWS_TOUCH_MOVE) act = "move";
    else if (action == ISWS_TOUCH_UP) act = "up";
    else if (action == ISWS_TOUCH_CANCEL) act = "cancel";

    for (size_t i = 0; prefix[i]; i++) buf[pos++] = prefix[i];
    for (size_t i = 0; act[i]; i++) buf[pos++] = act[i];

    {
        const char *xs = " x=";
        for (size_t i = 0; xs[i]; i++) buf[pos++] = xs[i];
    }
    if (ix < 0) {
        buf[pos++] = '-';
        ix = -ix;
    }
    pos += fmt_u64(buf + pos, (uint64_t)ix);

    {
        const char *ys = " y=";
        for (size_t i = 0; ys[i]; i++) buf[pos++] = ys[i];
    }
    if (iy < 0) {
        buf[pos++] = '-';
        iy = -iy;
    }
    pos += fmt_u64(buf + pos, (uint64_t)iy);

    {
        const char *ids = " id=";
        for (size_t i = 0; ids[i]; i++) buf[pos++] = ids[i];
    }
    pos += fmt_u64(buf + pos, (uint64_t)pointer_id);
    buf[pos++] = '\n';
    touch_log_write(buf, pos);
}

/* Trace-gated detail line for one injected phase: normalized coords
 * in milli-units (0..1000), pointer id, and the wire timestamp that
 * arrived with the message. Emitted once per injected event, next to
 * the stage lines ([touch] fetcher thread present?, internal receive,
 * source signalled, sendEvent: reached). Together with the base
 * [touch] down/move/up/cancel line, which stays byte-identical, this
 * gives one diagnosable line per phase and per pipeline stage. */
static void touch_log_detail(uint32_t action, uint32_t pointer_id,
                             double norm_x, double norm_y,
                             int64_t wire_ts) {
    char buf[128];
    size_t pos = 0;
    const char *act = "unknown";
    uint64_t nx_milli = (uint64_t)(norm_x * 1000.0 + 0.5);
    uint64_t ny_milli = (uint64_t)(norm_y * 1000.0 + 0.5);
    if (action == ISWS_TOUCH_DOWN) act = "down";
    else if (action == ISWS_TOUCH_MOVE) act = "move";
    else if (action == ISWS_TOUCH_UP) act = "up";
    else if (action == ISWS_TOUCH_CANCEL) act = "cancel";

    {
        const char *p = "[touch] hid-built ";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    }
    for (size_t i = 0; act[i]; i++) buf[pos++] = act[i];
    {
        const char *p = " nx=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    }
    pos += fmt_u64(buf + pos, nx_milli);
    {
        const char *p = " ny=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    }
    pos += fmt_u64(buf + pos, ny_milli);
    {
        const char *p = " id=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    }
    pos += fmt_u64(buf + pos, (uint64_t)pointer_id);
    {
        const char *p = " wire_ts=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    }
    if (wire_ts < 0) {
        uint64_t mag = (uint64_t)(-(wire_ts + 1)) + 1u;
        buf[pos++] = '-';
        pos += fmt_u64(buf + pos, mag);
    } else {
        pos += fmt_u64(buf + pos, (uint64_t)wire_ts);
    }
    buf[pos++] = '\n';
    touch_log_write(buf, pos);
}

static uint64_t touch_mach_abs_time(void) {
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->mach_abs_time) return g_test_ops->mach_abs_time();
    return 1000000ULL;
#else
    return mach_absolute_time();
#endif
}

#ifndef TOUCH_HOST_TEST
static void sleep_ms_raw(int ms) {
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    sys2(SYS_nanosleep, (long)&ts, 0);
}
#else
extern int usleep(unsigned int usec);
static void sleep_ms_raw(int ms) {
    if (ms > 0) usleep((unsigned int)ms * 1000);
}
#endif

static void win_lock_acquire(void) {
    while (__atomic_exchange_n(&g_win_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&g_win_lock, __ATOMIC_RELAXED) != 0) {
        }
    }
}

static void win_lock_release(void) {
    __atomic_store_n(&g_win_lock, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* DSC dlsym / dlopen + arm64e PAC signing                            */
/* ------------------------------------------------------------------ */

static void *lsk_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(TOUCH_HOST_TEST)
    uint64_t v = (uint64_t)p;
    __asm__ volatile(
        "xpaci %0\n"
        "paciza %0\n"
        : "+r"(v)
    );
    return (void *)v;
#else
    return p;
#endif
}

#if defined(__aarch64__) && !defined(TOUCH_HOST_TEST)
static __attribute__((noinline)) void *gapis_call2(uint64_t slot, uint64_t div,
                                                    uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)TOUCH_GAPIS_ADDR;
    if (!obj) return NULL;
    uint64_t vt = *(volatile uint64_t *)obj;
    uint64_t obj_div = (obj & 0x0000ffffffffffffULL) | (TOUCH_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    uint64_t slot_addr = vt + slot * 8ULL;
    uint64_t fn = *(volatile uint64_t *)slot_addr;
    uint64_t fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

    register uint64_t x0 __asm__("x0") = obj;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x16 __asm__("x16") = fn;
    register uint64_t x17 __asm__("x17") = fn_div;
    __asm__ volatile(
        "blraa x16, x17"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "+r"(x17)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "cc", "memory"
    );
    return (void *)x0;
}
#endif

static void *touch_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->dlsym_default) {
        return g_test_ops->dlsym_default(name);
    }
    return NULL;
#elif defined(__aarch64__)
    return gapis_call2(TOUCH_SLOT_DLSYM, TOUCH_DIV_DLSYM,
                       (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static void *touch_dlopen(const char *path) {
    if (!path) return NULL;
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->dlopen_fn) {
        return g_test_ops->dlopen_fn(path);
    }
    return NULL;
#elif defined(__aarch64__)
    return gapis_call2(TOUCH_SLOT_DLOPEN, TOUCH_DIV_DLOPEN,
                       (uint64_t)path, 1 /* RTLD_LAZY */);
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Symbol resolution                                                  */
/* ------------------------------------------------------------------ */

static int ensure_dispatch_syms(void) {
    if (g_tsyms.dispatch_resolved) {
        return (g_tsyms.dispatch_main_q && g_tsyms.dispatch_async_f &&
                g_tsyms.pthread_create) ? 0 : -1;
    }
    g_tsyms.dispatch_resolved = 1;
    g_tsyms.dispatch_main_q = touch_dlsym("_dispatch_main_q");
    g_tsyms.dispatch_async_f =
        (void (*)(void *, void *, void (*)(void *)))lsk_sign_fn(touch_dlsym("dispatch_async_f"));
    g_tsyms.pthread_create =
        (int (*)(void *, const void *, void *(*)(void *), void *))
        lsk_sign_fn(touch_dlsym("pthread_create"));
    return (g_tsyms.dispatch_main_q && g_tsyms.dispatch_async_f &&
            g_tsyms.pthread_create) ? 0 : -1;
}

static int ensure_inject_syms(void) {
    if (g_tsyms.inject_resolved) {
        return (g_tsyms.objc_getClass && g_tsyms.sel_registerName && g_tsyms.objc_msgSend &&
                g_tsyms.IOHIDEventCreateDigitizerEvent &&
                g_tsyms.IOHIDEventCreateDigitizerFingerEvent &&
                g_tsyms.IOHIDEventAppendEvent && g_tsyms.CFRelease) ? 0 : -1;
    }
    g_tsyms.inject_resolved = 1;

    g_tsyms.objc_getClass = (void *(*)(const char *))lsk_sign_fn(touch_dlsym("objc_getClass"));
    g_tsyms.sel_registerName = (void *(*)(const char *))lsk_sign_fn(touch_dlsym("sel_registerName"));
    g_tsyms.objc_msgSend = lsk_sign_fn(touch_dlsym("objc_msgSend"));
    g_tsyms.objc_autoreleasePoolPush =
        (void *(*)(void))lsk_sign_fn(touch_dlsym("objc_autoreleasePoolPush"));
    g_tsyms.objc_autoreleasePoolPop =
        (void (*)(void *))lsk_sign_fn(touch_dlsym("objc_autoreleasePoolPop"));

    g_tsyms.object_getClass = (void *(*)(void *))lsk_sign_fn(touch_dlsym("object_getClass"));
    g_tsyms.class_getInstanceMethod = (void *(*)(void *, void *))lsk_sign_fn(touch_dlsym("class_getInstanceMethod"));
    g_tsyms.method_setImplementation = (void *(*)(void *, void *))lsk_sign_fn(touch_dlsym("method_setImplementation"));
    g_tsyms.class_getInstanceVariable = (void *(*)(void *, const char *))lsk_sign_fn(touch_dlsym("class_getInstanceVariable"));
    g_tsyms.ivar_getOffset = (long (*)(void *))lsk_sign_fn(touch_dlsym("ivar_getOffset"));
    g_tsyms.object_setIvar = (void *(*)(void *, void *, void *))lsk_sign_fn(touch_dlsym("object_setIvar"));

    g_tsyms.CFRunLoopSourceSignal = (void (*)(void *))lsk_sign_fn(touch_dlsym("CFRunLoopSourceSignal"));
    g_tsyms.CFRunLoopWakeUp = (void (*)(void *))lsk_sign_fn(touch_dlsym("CFRunLoopWakeUp"));
    g_tsyms.CFRunLoopGetMain = (void *(*)(void))lsk_sign_fn(touch_dlsym("CFRunLoopGetMain"));

#ifndef TOUCH_HOST_TEST
    g_tsyms.receive_internal = (void (*)(void *, void *))lsk_sign_fn((void *)TOUCH_RECEIVE_INTERNAL_ADDR);
    g_tsyms.drain_events = (void (*)(void *, void *))lsk_sign_fn((void *)TOUCH_DRAIN_EVENTS_ADDR);
    g_tsyms.process_event_queue = (void (*)(void *, uint32_t))lsk_sign_fn((void *)TOUCH_PROCESS_EVENT_QUEUE_ADDR);
#else
    if (g_test_ops) {
        if (g_test_ops->receive_internal_fn) g_tsyms.receive_internal = g_test_ops->receive_internal_fn;
        if (g_test_ops->drain_events_fn) g_tsyms.drain_events = g_test_ops->drain_events_fn;
        if (g_test_ops->process_event_queue_fn) g_tsyms.process_event_queue = g_test_ops->process_event_queue_fn;
    }
#endif

    void *cf_rel = touch_dlsym("CFRelease");
    if (!cf_rel) {
        touch_dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
        cf_rel = touch_dlsym("CFRelease");
    }
    g_tsyms.CFRelease = (void (*)(void *))lsk_sign_fn(cf_rel);

    void *create_dig = touch_dlsym("IOHIDEventCreateDigitizerEvent");
    if (!create_dig) {
        touch_dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit");
        create_dig = touch_dlsym("IOHIDEventCreateDigitizerEvent");
        if (!create_dig) {
            touch_dlopen("/System/Library/Frameworks/IOKit.framework/IOKit");
            create_dig = touch_dlsym("IOHIDEventCreateDigitizerEvent");
        }
    }
    g_tsyms.IOHIDEventCreateDigitizerEvent =
        (void *(*)(void *, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                   double, double, double, double, double, uint32_t, uint32_t, uint32_t))
        lsk_sign_fn(create_dig);
    g_tsyms.IOHIDEventCreateDigitizerFingerEvent =
        (void *(*)(void *, uint64_t, uint32_t, uint32_t, uint32_t,
                   double, double, double, double, double,
                   uint32_t, uint32_t, uint32_t))
        lsk_sign_fn(touch_dlsym("IOHIDEventCreateDigitizerFingerEvent"));
    g_tsyms.IOHIDEventAppendEvent =
        (void (*)(void *, void *, uint32_t))lsk_sign_fn(touch_dlsym("IOHIDEventAppendEvent"));
    g_tsyms.IOHIDEventSetIntegerValue =
        (void (*)(void *, uint32_t, int64_t))lsk_sign_fn(touch_dlsym("IOHIDEventSetIntegerValue"));
    g_tsyms.IOHIDEventSetSenderID =
        (void (*)(void *, uint64_t))lsk_sign_fn(touch_dlsym("IOHIDEventSetSenderID"));

    /* BackBoardServices digitizer info: the real context-id
     * setter. dlopen fallback mirrors 052; on host test the mock dlsym
     * answers directly. */
    void *bks_info = touch_dlsym("BKSHIDEventSetDigitizerInfo");
    if (!bks_info) {
        touch_dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices");
        bks_info = touch_dlsym("BKSHIDEventSetDigitizerInfo");
    }
    g_tsyms.BKSHIDEventSetDigitizerInfo =
        (void (*)(void *, uint32_t, uint8_t, uint8_t, void *, double, float))
        lsk_sign_fn(bks_info);

    if (g_tsyms.sel_registerName) {
        g_tsyms.sel_respondsToSelector = g_tsyms.sel_registerName("respondsToSelector:");
        g_tsyms.sel_sharedApplication = g_tsyms.sel_registerName("sharedApplication");
        g_tsyms.sel_eventFetcher = g_tsyms.sel_registerName("_eventFetcher");
        g_tsyms.sel_receiveHIDEvent = g_tsyms.sel_registerName("_receiveHIDEvent:");
        g_tsyms.sel_keyWindow = g_tsyms.sel_registerName("keyWindow");
        g_tsyms.sel_windows = g_tsyms.sel_registerName("windows");
        g_tsyms.sel_count = g_tsyms.sel_registerName("count");
        g_tsyms.sel_objectAtIndex = g_tsyms.sel_registerName("objectAtIndex:");
        g_tsyms.sel_connectedScenes = g_tsyms.sel_registerName("connectedScenes");
        g_tsyms.sel_allObjects = g_tsyms.sel_registerName("allObjects");
        g_tsyms.sel_rootViewController = g_tsyms.sel_registerName("rootViewController");
        g_tsyms.sel_tabBarController = g_tsyms.sel_registerName("tabBarController");
        g_tsyms.sel_selectedIndex = g_tsyms.sel_registerName("selectedIndex");
        g_tsyms.sel_viewControllers = g_tsyms.sel_registerName("viewControllers");
        g_tsyms.sel_childViewControllers = g_tsyms.sel_childViewControllers ? g_tsyms.sel_childViewControllers : g_tsyms.sel_registerName("childViewControllers");
        g_tsyms.sel_allTouches = g_tsyms.sel_registerName("allTouches");
        g_tsyms.sel_sendEvent = g_tsyms.sel_registerName("sendEvent:");
        g_tsyms.sel_contextId = g_tsyms.sel_registerName("_contextId");
        g_tsyms.sel_layer = g_tsyms.sel_registerName("layer");
        g_tsyms.sel_context = g_tsyms.sel_registerName("context");
        g_tsyms.sel_layerContextId = g_tsyms.sel_registerName("contextId");
        g_tsyms.sel_window = g_tsyms.sel_registerName("window");
        g_tsyms.sel_view = g_tsyms.sel_registerName("view");
        g_tsyms.sel_hitTest = g_tsyms.sel_registerName("hitTest:withEvent:");
        g_tsyms.sel_setWindow = g_tsyms.sel_registerName("setWindow:");
        g_tsyms.sel_setView = g_tsyms.sel_registerName("setView:");
        g_tsyms.sel_setPhase = g_tsyms.sel_registerName("setPhase:");
        g_tsyms.sel_setTimestamp = g_tsyms.sel_registerName("setTimestamp:");
        g_tsyms.sel_setTapCount = g_tsyms.sel_registerName("setTapCount:");
        g_tsyms.sel_setLocationInWindow = g_tsyms.sel_registerName("_setLocationInWindow:resetPrevious:");
        g_tsyms.sel_setHidEvent = g_tsyms.sel_registerName("_setHidEvent:");
        g_tsyms.sel_setIsFirstTouchForView = g_tsyms.sel_registerName("_setIsFirstTouchForView:");
        g_tsyms.sel_setInitialTouchTimestamp = g_tsyms.sel_registerName("_setInitialTouchTimestamp:");
        g_tsyms.sel_setTouchesPriv = g_tsyms.sel_registerName("_setTouches:");
        g_tsyms.sel_setTouches = g_tsyms.sel_registerName("setTouches:");
        g_tsyms.sel_setWithObject = g_tsyms.sel_registerName("setWithObject:");
        g_tsyms.sel_alloc = g_tsyms.sel_registerName("alloc");
        g_tsyms.sel_init = g_tsyms.sel_registerName("init");
        g_tsyms.sel_drainEvents = g_tsyms.sel_registerName("drainEventsIntoEnvironment:");
        g_tsyms.sel_dispatcherReceived = g_tsyms.sel_registerName("eventFetcherDidReceiveEvents:");
        g_tsyms.sel_setEventTimestamp = g_tsyms.sel_registerName("_setTimestamp:");
    }

    return (g_tsyms.objc_getClass && g_tsyms.sel_registerName && g_tsyms.objc_msgSend &&
            g_tsyms.IOHIDEventCreateDigitizerEvent &&
            g_tsyms.IOHIDEventCreateDigitizerFingerEvent &&
            g_tsyms.IOHIDEventAppendEvent && g_tsyms.CFRelease) ? 0 : -1;
}

static inline uint8_t objc_responds(void *obj, void *sel) {
    if (!obj || !sel || !g_tsyms.objc_msgSend || !g_tsyms.sel_respondsToSelector) {
        return 0;
    }
    uint8_t (*fn)(void *, void *, void *) =
        (uint8_t (*)(void *, void *, void *))g_tsyms.objc_msgSend;
    return fn(obj, g_tsyms.sel_respondsToSelector, sel) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Sender ID, timestamps, queue probes                                */
/* ------------------------------------------------------------------ */

#ifndef TOUCH_HOST_TEST
struct touch_mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
extern int touch_lsk_mach_timebase_info(struct touch_mach_timebase_info *info)
    __asm__("_mach_timebase_info");
#endif

/* Parse an IOSWINE_TOUCH_SENDER_ID value: "magic" for the legacy
 * constant, "0" for undefined, "0x..." hex, or decimal. Returns 0 on
 * success, -1 on anything else. No libc. */
static int touch_parse_u64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    int base = 10;
    size_t i = 0;
    if (!s || !out || !s[0]) return -1;
    if (s[0] == 'm') {
        if (s[1] == 'a' && s[2] == 'g' && s[3] == 'i' && s[4] == 'c' && s[5] == '\0') {
            *out = TOUCH_SENDER_ID_MAGIC;
            return 0;
        }
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
        if (!s[2]) return -1;
    }
    for (; s[i]; i++) {
        uint64_t d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else return -1;
        if (d >= (uint64_t)base) return -1;
        if (v > (UINT64_MAX - d) / (uint64_t)base) return -1;
        v = v * (uint64_t)base + d;
    }
    *out = v;
    return 0;
}

/* mach_absolute_time ticks converted to seconds for UITouch timestamps.
 * Uses mach_timebase_info when available; falls back to 1 tick = 1 ns. */
static double touch_timestamp_seconds(uint64_t now) {
    uint32_t numer = 1;
    uint32_t denom = 1;
#ifdef TOUCH_HOST_TEST
    if (g_test_ops && g_test_ops->timebase_fn) {
        uint32_t n = 0;
        uint32_t d = 0;
        if (g_test_ops->timebase_fn(&n, &d) == 0 && d != 0) {
            numer = n;
            denom = d;
        }
    }
#else
    {
        struct touch_mach_timebase_info tb;
        tb.numer = 0;
        tb.denom = 0;
        if (touch_lsk_mach_timebase_info(&tb) == 0 && tb.denom != 0 && tb.numer != 0) {
            numer = tb.numer;
            denom = tb.denom;
        }
    }
#endif
    return (double)now * (double)numer / (double)denom / 1e9;
}

/* Sender ID for the HID event. The legacy magic constant is a
 * per-device IORegistry entry ID from another device; it cannot be this
 * device's digitizer ID, so it is only used when the env override says
 * "magic". Default is the key window's _contextId, the only real
 * in-process routing value; 0 means undefined.
 * Returns the chosen value; logs it in trace mode. */
static uint64_t touch_choose_sender_id(uint32_t ctx_id) {
    uint64_t id;
    if (g_sender_override) {
        id = g_sender_override_value;
    } else if (ctx_id != 0) {
        id = (uint64_t)ctx_id;
    } else {
        id = 0;
    }
    if (g_touch_trace) {
        char buf[96];
        size_t pos = 0;
        const char *p = "[touch] sender id ";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
        pos += fmt_u64(buf + pos, id);
        const char *q = " ctx=";
        for (size_t i = 0; q[i]; i++) buf[pos++] = q[i];
        pos += fmt_u64(buf + pos, (uint64_t)ctx_id);
        buf[pos++] = '\n';
        touch_log_write(buf, pos);
    }
    return id;
}

/* Resolve the context id stamped on the HID event.
 *
 * Device evidence: -[UIWindow _contextId] returned
 * 2660229795 (0x9e8fe2a3) in the selftest run and 3695415077
 * (0xdc438f25) in the interactive run. A real CAContext contextId is a
 * small render-server-assigned integer, stable across runs for the same
 * window; these values are large, high-bit-set, and differ per run, so
 * _contextId is returning garbage here (uninitialized memory or a
 * pointer fragment, not a routing id). Stamping that garbage as the event's context id meant the touch
 * never reached -[UIWindow sendEvent:]: the touch had
 * no window, because the dispatcher resolves the window from the
 * event's context id against the window's layer CAContext [guess on
 * the exact lookup; the drop stage itself is device-observed].
 *
 * Candidates, in priority order:
 *   1. IOSWINE_TOUCH_CTX_ID env override (decimal or 0x-hex).
 *   2. [[win layer] context].contextId: CALayer.context and
 *      CAContext.contextId are public API, and the layer's context is
 *      the authoritative owner of the id.
 *   3. [win _contextId] (kept as fallback only).
 *   4. 0 (no context id; the event is stamped with none).
 * The two _contextId reads are compared in trace mode: if they differ,
 * _contextId is non-deterministic on this device and must not be
 * trusted. The resolution is trace-logged the first time and whenever
 * the chosen value changes. */
static uint32_t touch_resolve_context_id(void *win) {
    uint32_t win_ctx = 0;
    uint32_t win_ctx2 = 0;
    uint32_t layer_ctx = 0;
    uint32_t use = 0;
    if (win) {
        if (g_tsyms.sel_contextId && objc_responds(win, g_tsyms.sel_contextId)) {
            win_ctx = (uint32_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                win, g_tsyms.sel_contextId);
            win_ctx2 = win_ctx;
            if (g_touch_trace) {
                win_ctx2 = (uint32_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                    win, g_tsyms.sel_contextId);
            }
        }
        if (g_tsyms.sel_layer && objc_responds(win, g_tsyms.sel_layer)) {
            void *layer = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                win, g_tsyms.sel_layer);
            if (layer && g_tsyms.sel_context && objc_responds(layer, g_tsyms.sel_context)) {
                void *cactx = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                    layer, g_tsyms.sel_context);
                if (cactx && g_tsyms.sel_layerContextId &&
                    objc_responds(cactx, g_tsyms.sel_layerContextId)) {
                    layer_ctx = (uint32_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                        cactx, g_tsyms.sel_layerContextId);
                }
            }
        }
    }
    if (g_ctx_override) {
        use = (uint32_t)g_ctx_override_value;
    } else if (layer_ctx != 0) {
        use = layer_ctx;
    } else {
        use = win_ctx;
    }
    if (g_touch_trace && (!g_ctx_logged || use != g_ctx_last_logged)) {
        char buf[128];
        size_t pos = 0;
        const char *p = "[touch] ctx win=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
        pos += fmt_u64(buf + pos, (uint64_t)win_ctx);
        p = " win2=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
        pos += fmt_u64(buf + pos, (uint64_t)win_ctx2);
        p = " layer=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
        pos += fmt_u64(buf + pos, (uint64_t)layer_ctx);
        p = " use=";
        for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
        pos += fmt_u64(buf + pos, (uint64_t)use);
        buf[pos++] = '\n';
        touch_log_write(buf, pos);
        g_ctx_logged = 1;
        g_ctx_last_logged = use;
    }
    return use;
}

/* Count of an NSArray-like ivar on an object, or -1 when the ivar is
 * absent, nil, or does not respond to -count. */
static void *touch_ivar(void *obj, const char *name);

static long touch_ivar_count(void *obj, const char *name) {
    void *arr;
    if (!obj || !name) return -1;
    arr = touch_ivar(obj, name);
    if (!arr) return -1;
    if (!g_tsyms.sel_count || !objc_responds(arr, g_tsyms.sel_count)) return -1;
    return (long)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
        arr, g_tsyms.sel_count);
}

/* Trace probe: how many HID events are sitting in the fetcher queues.
 * After _receiveHIDEventInternal: returns, filtered must have grown by
 * one per event; if it stays 0, Internal dropped the event. Logged only
 * in trace mode. */
static void touch_log_queue_counts(void *fetcher) {
    long filtered;
    long raw;
    char buf[80];
    size_t pos = 0;
    const char *p;
    if (!g_touch_trace || !fetcher) return;
    filtered = touch_ivar_count(fetcher, "_incomingHIDEventsFiltered");
    raw = touch_ivar_count(fetcher, "_incomingHIDEvents");
    p = "[touch] queue filtered=";
    for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    if (filtered < 0) {
        buf[pos++] = '-';
    } else {
        pos += fmt_u64(buf + pos, (uint64_t)filtered);
    }
    p = " raw=";
    for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    if (raw < 0) {
        buf[pos++] = '-';
    } else {
        pos += fmt_u64(buf + pos, (uint64_t)raw);
    }
    buf[pos++] = '\n';
    touch_log_write(buf, pos);
}

/* ------------------------------------------------------------------ */
/* Diagnostic sendEvent: tracing                                      */
/* ------------------------------------------------------------------ */

static void sendEvent_hook(void *self, void *cmd, void *event) {
    /* Count every delivery, whether or not tracing is on. This is the
     * end-to-end signal the selftest reports (F11). */
    g_sendevent_count++;
#if defined(__aarch64__) && !defined(TOUCH_HOST_TEST)
    if (sys0(SYS_gettid) != sys0(SYS_getpid))
        touch_log_str("[touch] SENDEVENT OFF MAIN THREAD\n");
#endif
    if (g_touch_trace) {
        uint64_t count = 0;
        if (event && g_tsyms.sel_allTouches && objc_responds(event, g_tsyms.sel_allTouches)) {
            void *touches = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(event, g_tsyms.sel_allTouches);
            if (touches && g_tsyms.sel_count && objc_responds(touches, g_tsyms.sel_count)) {
                count = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(touches, g_tsyms.sel_count);
            }
        }
        /* Log every call, including count 0: a sendEvent: with an empty
         * event means hit-testing found no view, which is itself the
         * diagnosis. The old code only logged count > 0 and hid that. */
        {
            char buf[64];
            size_t pos = 0;
            const char *p = "[touch] sendEvent: reached with touch count ";
            for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
            pos += fmt_u64(buf + pos, count);
            buf[pos++] = '\n';
            touch_log_write(buf, pos);
        }
    }
    if (g_sendEvent_orig) {
        g_sendEvent_orig(self, cmd, event);
    }
}

/* Trace hook for -[UIEventFetcher drainEventsIntoEnvironment:]. Logs the
 * drain and the queue count at drain time, then chains to the original. */
static void drain_hook(void *self, void *cmd, void *env) {
    if (g_touch_trace) {
        touch_log_str("[touch] drain events\n");
        touch_log_queue_counts(self);
    }
    if (g_drain_orig) {
        g_drain_orig(self, cmd, env);
    }
}

/* Trace hook for -[UIEventDispatcher eventFetcherDidReceiveEvents:].
 * Its firing proves _receiveHIDEventInternal: signalled the dispatcher. */
static void dispatcher_received_hook(void *self, void *cmd, void *arg) {
    if (g_touch_trace) {
        touch_log_str("[touch] dispatcher received events\n");
    }
    if (g_dispatcher_received_orig) {
        g_dispatcher_received_orig(self, cmd, arg);
    }
}

/* Trace hook for -[UIWindow sendEvent:]. Its firing proves the event got
 * past UIApplication sendEvent: to the window. */
static void window_sendEvent_hook(void *self, void *cmd, void *event) {
    if (g_touch_trace) {
        touch_log_str("[touch] window sendEvent:\n");
    }
    if (g_window_sendEvent_orig) {
        g_window_sendEvent_orig(self, cmd, event);
    }
}

static void install_sendevent_hook(void *app) {
    /* Installed on the first injected event in every mode, not only
     * trace/selftest. The sendEvent: count is the delivery signal the
     * combined route's fallback reads (no delivery observed for this
     * phase means the HID chain dropped it), so the hook must be in
     * place for ordinary taps too, not just for traced or selftest
     * runs. The hook itself is one atomic increment plus a chain to
     * the original; it logs only in trace mode. */
    if (g_sendevent_hooked || !app) return;
    g_sendevent_hooked = 1;
    if (!g_tsyms.class_getInstanceMethod || !g_tsyms.method_setImplementation ||
        !g_tsyms.object_getClass || !g_tsyms.sel_sendEvent) return;
    void *cls = g_tsyms.object_getClass(app);
    void *m = g_tsyms.class_getInstanceMethod(cls, g_tsyms.sel_sendEvent);
    if (!m) {
        void *app_cls = g_tsyms.objc_getClass("UIApplication");
        if (app_cls) m = g_tsyms.class_getInstanceMethod(app_cls, g_tsyms.sel_sendEvent);
    }
    if (m) {
        void *orig = g_tsyms.method_setImplementation(m, (void *)lsk_sign_fn(sendEvent_hook));
        g_sendEvent_orig = (send_event_fn)lsk_sign_fn(orig);
    }
}

/* Install the drain / dispatcher / window trace hooks once, in trace
 * mode only. Each hook is best-effort: a missing class or method skips
 * that hook silently and the rest still install. */
static void install_pipeline_hooks(void *fetcher, void *disp) {
    void *m;
    void *orig;
    if (g_pipeline_hooks_installed || !g_touch_trace) return;
    g_pipeline_hooks_installed = 1;
    if (!g_tsyms.class_getInstanceMethod || !g_tsyms.method_setImplementation ||
        !g_tsyms.object_getClass) return;
    if (fetcher && g_tsyms.sel_drainEvents) {
        m = g_tsyms.class_getInstanceMethod(g_tsyms.object_getClass(fetcher),
                                            g_tsyms.sel_drainEvents);
        if (m) {
            orig = g_tsyms.method_setImplementation(m, (void *)lsk_sign_fn(drain_hook));
            g_drain_orig = (drain_fn)lsk_sign_fn(orig);
        }
    }
    if (disp && g_tsyms.sel_dispatcherReceived) {
        m = g_tsyms.class_getInstanceMethod(g_tsyms.object_getClass(disp),
                                            g_tsyms.sel_dispatcherReceived);
        if (m) {
            orig = g_tsyms.method_setImplementation(m, (void *)lsk_sign_fn(dispatcher_received_hook));
            g_dispatcher_received_orig = (dispatcher_received_fn)lsk_sign_fn(orig);
        }
    } else if (g_tsyms.sel_dispatcherReceived) {
        void *disp_cls = g_tsyms.objc_getClass ? g_tsyms.objc_getClass("UIEventDispatcher") : NULL;
        if (disp_cls) {
            m = g_tsyms.class_getInstanceMethod(disp_cls, g_tsyms.sel_dispatcherReceived);
            if (m) {
                orig = g_tsyms.method_setImplementation(m, (void *)lsk_sign_fn(dispatcher_received_hook));
                g_dispatcher_received_orig = (dispatcher_received_fn)lsk_sign_fn(orig);
            }
        }
    }
    if (g_tsyms.sel_sendEvent) {
        void *win_cls = g_tsyms.objc_getClass ? g_tsyms.objc_getClass("UIWindow") : NULL;
        if (win_cls) {
            m = g_tsyms.class_getInstanceMethod(win_cls, g_tsyms.sel_sendEvent);
            if (m) {
                orig = g_tsyms.method_setImplementation(m, (void *)lsk_sign_fn(window_sendEvent_hook));
                g_window_sendEvent_orig = (send_event_fn)lsk_sign_fn(orig);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Coordinate mapping and window size                                 */
/* ------------------------------------------------------------------ */

void touch_set_window_size(double width_pts, double height_pts) {
    if (!is_finite_f64(width_pts) || !is_finite_f64(height_pts)) return;
    if (width_pts <= 0.0 || height_pts <= 0.0) return;
    if (width_pts > 16384.0 || height_pts > 16384.0) return;
    win_lock_acquire();
    g_win_w = width_pts;
    g_win_h = height_pts;
    g_win_size_set = 1;
    win_lock_release();
}

static void get_window_size(double *w_out, double *h_out) {
    win_lock_acquire();
    if (g_win_size_set) {
        *w_out = g_win_w;
        *h_out = g_win_h;
    } else {
        *w_out = TOUCH_DEFAULT_WIN_W;
        *h_out = TOUCH_DEFAULT_WIN_H;
    }
    win_lock_release();
}

int touch_map_coords(const isws_touch_t *t, uint32_t client_w, uint32_t client_h,
                     double win_w, double win_h,
                     double *norm_x_out, double *norm_y_out,
                     double *win_x_out, double *win_y_out) {
    double nx;
    double ny;
    if (!t || client_w == 0 || client_h == 0) return -1;
    if (!is_finite_f32(t->x) || !is_finite_f32(t->y)) return -1;
    if (!is_finite_f64(win_w) || !is_finite_f64(win_h) || win_w <= 0.0 || win_h <= 0.0) {
        return -1;
    }
    nx = (double)t->x / (double)client_w;
    ny = (double)t->y / (double)client_h;
    if (nx < 0.0) nx = 0.0;
    if (nx > 1.0) nx = 1.0;
    if (ny < 0.0) ny = 0.0;
    if (ny > 1.0) ny = 1.0;
    if (norm_x_out) *norm_x_out = nx;
    if (norm_y_out) *norm_y_out = ny;
    if (win_x_out) *win_x_out = nx * win_w;
    if (win_y_out) *win_y_out = ny * win_h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* IOHIDEvent construction & injection (main queue)                   */
/* ------------------------------------------------------------------ */

static void *touch_ivar(void *obj, const char *name) {
    void *iv;
    if (!obj || !name) return NULL;
    if (!g_tsyms.object_getClass || !g_tsyms.class_getInstanceVariable || !g_tsyms.ivar_getOffset) {
        return NULL;
    }
    iv = g_tsyms.class_getInstanceVariable(g_tsyms.object_getClass(obj), name);
    if (!iv) return NULL;
    return *(void **)((char *)obj + g_tsyms.ivar_getOffset(iv));
}

static void *touch_find_fetcher(void *app) {
    void *f;
    if (!app) return NULL;
    if (objc_responds(app, g_tsyms.sel_eventFetcher)) {
        f = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app, g_tsyms.sel_eventFetcher);
        if (f) return f;
    }
    f = touch_ivar(app, "_eventFetcher");
    if (f) return f;
    return touch_ivar(touch_ivar(app, "_eventDispatcher"), "_eventFetcher");
}

static void *touch_find_window(void *app) {
    if (!app) return NULL;
    /* 1. Try [app keyWindow] */
    if (g_tsyms.sel_keyWindow && objc_responds(app, g_tsyms.sel_keyWindow)) {
        void *w = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app, g_tsyms.sel_keyWindow);
        if (w) return w;
    }
    void *kw = touch_ivar(app, "_keyWindow");
    if (kw) return kw;
    /* 2. Try [app windows] */
    if (g_tsyms.sel_windows && objc_responds(app, g_tsyms.sel_windows)) {
        void *wins = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app, g_tsyms.sel_windows);
        if (wins && g_tsyms.sel_count && objc_responds(wins, g_tsyms.sel_count)) {
            uint64_t cnt = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(wins, g_tsyms.sel_count);
            if (cnt > 0 && g_tsyms.sel_objectAtIndex) {
                void *w = ((void *(*)(void *, void *, uint64_t))g_tsyms.objc_msgSend)(wins, g_tsyms.sel_objectAtIndex, 0);
                if (w) return w;
            }
        }
    }
    /* 3. Try [app connectedScenes] */
    if (g_tsyms.sel_connectedScenes && objc_responds(app, g_tsyms.sel_connectedScenes)) {
        void *scenes = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app, g_tsyms.sel_connectedScenes);
        if (scenes && g_tsyms.sel_allObjects && objc_responds(scenes, g_tsyms.sel_allObjects)) {
            void *arr = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(scenes, g_tsyms.sel_allObjects);
            if (arr && g_tsyms.sel_count && objc_responds(arr, g_tsyms.sel_count)) {
                uint64_t scnt = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(arr, g_tsyms.sel_count);
                for (uint64_t i = 0; i < scnt; i++) {
                    void *scene = ((void *(*)(void *, void *, uint64_t))g_tsyms.objc_msgSend)(arr, g_tsyms.sel_objectAtIndex, i);
                    if (scene && g_tsyms.sel_windows && objc_responds(scene, g_tsyms.sel_windows)) {
                        void *sw = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(scene, g_tsyms.sel_windows);
                        if (sw && g_tsyms.sel_count && objc_responds(sw, g_tsyms.sel_count)) {
                            uint64_t wcnt = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(sw, g_tsyms.sel_count);
                            if (wcnt > 0 && g_tsyms.sel_objectAtIndex) {
                                void *w = ((void *(*)(void *, void *, uint64_t))g_tsyms.objc_msgSend)(sw, g_tsyms.sel_objectAtIndex, 0);
                                if (w) return w;
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static void *touch_find_tab_bar_controller(void *app) {
    void *win = touch_find_window(app);
    if (!win) return NULL;
    void *root_vc = NULL;
    if (g_tsyms.sel_rootViewController && objc_responds(win, g_tsyms.sel_rootViewController)) {
        root_vc = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(win, g_tsyms.sel_rootViewController);
    }
    if (!root_vc) return NULL;

    if (g_tsyms.sel_selectedIndex && objc_responds(root_vc, g_tsyms.sel_selectedIndex)) {
        return root_vc;
    }
    if (g_tsyms.sel_tabBarController && objc_responds(root_vc, g_tsyms.sel_tabBarController)) {
        void *tbc = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(root_vc, g_tsyms.sel_tabBarController);
        if (tbc && g_tsyms.sel_selectedIndex && objc_responds(tbc, g_tsyms.sel_selectedIndex)) {
            return tbc;
        }
    }
    if (g_tsyms.sel_childViewControllers && objc_responds(root_vc, g_tsyms.sel_childViewControllers)) {
        void *cvcs = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(root_vc, g_tsyms.sel_childViewControllers);
        if (cvcs && g_tsyms.sel_count && objc_responds(cvcs, g_tsyms.sel_count)) {
            uint64_t cnt = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(cvcs, g_tsyms.sel_count);
            for (uint64_t i = 0; i < cnt; i++) {
                void *c = ((void *(*)(void *, void *, uint64_t))g_tsyms.objc_msgSend)(cvcs, g_tsyms.sel_objectAtIndex, i);
                if (c && g_tsyms.sel_selectedIndex && objc_responds(c, g_tsyms.sel_selectedIndex)) {
                    return c;
                }
            }
        }
    }
    if (g_tsyms.sel_viewControllers && objc_responds(root_vc, g_tsyms.sel_viewControllers)) {
        void *vcs = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(root_vc, g_tsyms.sel_viewControllers);
        if (vcs && g_tsyms.sel_count && objc_responds(vcs, g_tsyms.sel_count)) {
            uint64_t cnt = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(vcs, g_tsyms.sel_count);
            for (uint64_t i = 0; i < cnt; i++) {
                void *c = ((void *(*)(void *, void *, uint64_t))g_tsyms.objc_msgSend)(vcs, g_tsyms.sel_objectAtIndex, i);
                if (c && g_tsyms.sel_selectedIndex && objc_responds(c, g_tsyms.sel_selectedIndex)) {
                    return c;
                }
            }
        }
    }
    return NULL;
}

static uint64_t touch_get_tab_index(void) {
    void *pool = NULL;
    if (ensure_inject_syms() != 0) return 0;
    if (g_tsyms.objc_autoreleasePoolPush) {
        pool = g_tsyms.objc_autoreleasePoolPush();
    }
    void *app_cls = g_tsyms.objc_getClass("UIApplication");
    if (!app_cls || !g_tsyms.sel_sharedApplication || !objc_responds(app_cls, g_tsyms.sel_sharedApplication)) {
        if (pool && g_tsyms.objc_autoreleasePoolPop) g_tsyms.objc_autoreleasePoolPop(pool);
        return 0;
    }
    void *app = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app_cls, g_tsyms.sel_sharedApplication);
    if (!app) {
        if (pool && g_tsyms.objc_autoreleasePoolPop) g_tsyms.objc_autoreleasePoolPop(pool);
        return 0;
    }
    void *tbc = touch_find_tab_bar_controller(app);
    if (!tbc || !g_tsyms.sel_selectedIndex || !objc_responds(tbc, g_tsyms.sel_selectedIndex)) {
        if (pool && g_tsyms.objc_autoreleasePoolPop) g_tsyms.objc_autoreleasePoolPop(pool);
        return 0;
    }
    uint64_t idx = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_tsyms.objc_msgSend)(tbc, g_tsyms.sel_selectedIndex);
    if (pool && g_tsyms.objc_autoreleasePoolPop) g_tsyms.objc_autoreleasePoolPop(pool);
    return idx;
}

/* A 2D point passed by value through objc_msgSend (CGPoint layout:
 * two doubles). */
typedef struct {
    double x;
    double y;
} touch_point_t;

/* Write a scalar value into a UITouch ivar by name.
 * Fallback when the matching setter does not respond on this build:
 * the touch state (phase, location, timestamp, tap count) must still
 * reach sendEvent:, because -[UIWindow sendEvent:] re-hit-tests Began
 * touches from locationInWindow and delivers touchesBegan: by phase.
 * [guess] on the ivar names; a wrong name costs nothing (the lookup
 * returns NULL and nothing is written). Returns 1 when written. */
static int touch_set_touch_scalar_ivar(void *touch_cls, void *touch,
                                       const char *name,
                                       const void *val, size_t len) {
    void *iv;
    long off;
    size_t i;
    if (!touch_cls || !touch || !name || !val || len == 0 ||
        !g_tsyms.class_getInstanceVariable || !g_tsyms.ivar_getOffset) {
        return 0;
    }
    iv = g_tsyms.class_getInstanceVariable(touch_cls, name);
    if (!iv) return 0;
    off = g_tsyms.ivar_getOffset(iv);
    if (off < 0) return 0;
    for (i = 0; i < len; i++) {
        ((uint8_t *)touch)[(size_t)off + i] = ((const uint8_t *)val)[i];
    }
    return 1;
}

/* Trace line for one direct-path phase, shared by the KIF and the
 * fallback delivery below so both log the same format. */
static void touch_log_direct_setters(long phase, int applied, int tried) {
    char buf[64];
    size_t pos = 0;
    const char *p = "[touch] direct phase=";
    for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    pos += fmt_u64(buf + pos, (uint64_t)phase);
    p = " setters=";
    for (size_t i = 0; p[i]; i++) buf[pos++] = p[i];
    pos += fmt_u64(buf + pos, (uint64_t)applied);
    buf[pos++] = '/';
    pos += fmt_u64(buf + pos, (uint64_t)tried);
    buf[pos++] = '\n';
    touch_log_write(buf, pos);
}

/* Route C: synthesize a UITouchesEvent from UITouch objects and call
 * -[UIApplication sendEvent:] directly, bypassing the HID layer and the
 * _UIEventDeferringManager entirely (finding 4). This is the KIF /
 * PTFakeTouch shape: UITouch setters plus _setHidEvent:, an NSSet of
 * touches attached to a UITouchesEvent, then sendEvent:. Every private
 * selector is guarded by respondsToSelector, and the exact path taken is
 * trace-logged, because the selectors are [guess] until a device run
 * confirms them. When a
 * setter does not respond, the same state is written straight into the
 * UITouch ivar (_phase, _locationInWindow, _previousLocationInWindow,
 * _timestamp, _tapCount), because -[UIWindow sendEvent:] re-hit-tests
 * Began touches from locationInWindow and delivers by phase; a touch
 * with a wrong location or a stuck phase never completes UIControl
 * tracking. The same UITouch
 * object is reused across DOWN/MOVE/UP for a pointer id: UIKit tracks a
 * touch by object identity, so a fresh object per message would look like
 * a new touch that was never began. The view is likewise sticky: the
 * Began hit-test result is cached per pointer (g_direct_views) and
 * written into the touch on every phase, so a drag-out keeps
 * delivering to the control that began tracking. Returns 0 on success, -1 when the
 * direct path could not be built (caller keeps the HID event built; the
 * HID route is not retried here, the env flag selects exactly one route).
 */
static int touch_direct_inject(void *app, void *win, uint32_t action,
                               uint32_t id, double wx, double wy,
                               uint64_t now, void *hid_event) {
    void *touch_cls;
    void *event_cls;
    void *nsset_cls;
    void *touch;
    void *event;
    void *view = NULL;
    void *set = NULL;
    long phase;
    double ts;
    touch_point_t pt;
    int setters_applied = 0;
    int setters_tried = 0;
    int window_ok = 0;
    int view_ok = 0;
    /* Per-setter flags, so the ivar fallback below knows
     * exactly which piece of touch state still needs writing. */
    int phase_ok = 0;
    int loc_ok = 0;
    int ts_ok = 0;
    int tap_ok = 0;
    int ivar_any = 0;

    if (!app || !win || id >= ISWS_MAX_POINTERS) return -1;

    if (action == ISWS_TOUCH_DOWN) phase = TOUCH_PHASE_BEGAN;
    else if (action == ISWS_TOUCH_MOVE) phase = TOUCH_PHASE_MOVED;
    else if (action == ISWS_TOUCH_UP) phase = TOUCH_PHASE_ENDED;
    else phase = TOUCH_PHASE_CANCELLED;

    touch_cls = g_tsyms.objc_getClass ? g_tsyms.objc_getClass("UITouch") : NULL;
    event_cls = g_tsyms.objc_getClass ? g_tsyms.objc_getClass("UITouchesEvent") : NULL;
    nsset_cls = g_tsyms.objc_getClass ? g_tsyms.objc_getClass("NSSet") : NULL;
    if (!touch_cls || !event_cls || !nsset_cls) {
        if (g_touch_trace) {
            touch_log_str("[touch] direct unavailable: missing UITouch/UITouchesEvent/NSSet\n");
        }
        return -1;
    }

    pt.x = wx;
    pt.y = wy;

    /* Hit-test the view ourselves: UIKit's own hit test, in window
     * points (correct here: hitTest:withEvent: takes points). Runs
     * every phase, but only the Began result is written into the
     * touch (see g_direct_views): later phases keep the Began view. */
    if (g_tsyms.sel_hitTest && objc_responds(win, g_tsyms.sel_hitTest)) {
        view = ((void *(*)(void *, void *, touch_point_t, void *))g_tsyms.objc_msgSend)(
            win, g_tsyms.sel_hitTest, pt, NULL);
    }
    if (!view) view = win;
    if (action == ISWS_TOUCH_DOWN) {
        g_direct_views[id] = view;
    } else {
        view = g_direct_views[id] ? g_direct_views[id] : win;
    }

    if (action == ISWS_TOUCH_DOWN) {
        void *alloced = NULL;
        void *inited = NULL;
        if (g_tsyms.sel_alloc && g_tsyms.sel_init) {
            alloced = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                touch_cls, g_tsyms.sel_alloc);
            if (alloced) {
                inited = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                    alloced, g_tsyms.sel_init);
            }
        }
        if (!inited) {
            if (g_touch_trace) touch_log_str("[touch] direct unavailable: UITouch alloc/init failed\n");
            return -1;
        }
        touch = inited;
        g_direct_touches[id] = touch;
        g_direct_touch_count++;
    } else {
        touch = g_direct_touches[id];
        if (!touch) {
            if (g_touch_trace) touch_log_str("[touch] direct: no touch for pointer\n");
            return -1;
        }
    }

    ts = touch_timestamp_seconds(now);

    /* Apply the UITouch setters that exist. Each is guarded: on iOS 26
     * most of these were promoted to public (mob 2026-04-21 runtime
     * enumeration), but _setLocationInWindow:resetPrevious: and
     * _setHidEvent: stay private. */
    /* tried counts every registered selector, applied counts the ones
     * the touch actually responds to, so the trace line shows gaps. */
    if (g_tsyms.sel_setWindow) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setWindow)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setWindow, win);
            setters_applied++;
            window_ok = 1;
        }
    }
    if (g_tsyms.sel_setView) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setView)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setView, view);
            setters_applied++;
            view_ok = 1;
        }
    }
    if (g_tsyms.sel_setPhase) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setPhase)) {
            ((void (*)(void *, void *, long))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setPhase, phase);
            setters_applied++;
            phase_ok = 1;
        }
    }
    if (g_tsyms.sel_setTimestamp) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setTimestamp)) {
            ((void (*)(void *, void *, double))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setTimestamp, ts);
            setters_applied++;
            ts_ok = 1;
        }
    }
    if (g_tsyms.sel_setTapCount) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setTapCount)) {
            ((void (*)(void *, void *, unsigned long))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setTapCount, 1UL);
            setters_applied++;
            tap_ok = 1;
        }
    }
    if (g_tsyms.sel_setLocationInWindow) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setLocationInWindow)) {
            ((void (*)(void *, void *, touch_point_t, int))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setLocationInWindow, pt,
                action == ISWS_TOUCH_DOWN ? 1 : 0);
            setters_applied++;
            loc_ok = 1;
        }
    }
    if (hid_event && g_tsyms.sel_setHidEvent) {
        setters_tried++;
        if (objc_responds(touch, g_tsyms.sel_setHidEvent)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_setHidEvent, hid_event);
            setters_applied++;
        }
    }
    /* The two setters below are
     * DOWN-only (the initial timestamp must not move on MOVE/UP).
     * [guess] on both selector names until a device run confirms them. */
    if (action == ISWS_TOUCH_DOWN) {
        if (g_tsyms.sel_setIsFirstTouchForView) {
            setters_tried++;
            if (objc_responds(touch, g_tsyms.sel_setIsFirstTouchForView)) {
                ((void (*)(void *, void *, uint8_t))g_tsyms.objc_msgSend)(
                    touch, g_tsyms.sel_setIsFirstTouchForView, 1);
                setters_applied++;
            }
        }
        if (g_tsyms.sel_setInitialTouchTimestamp) {
            setters_tried++;
            if (objc_responds(touch, g_tsyms.sel_setInitialTouchTimestamp)) {
                ((void (*)(void *, void *, double))g_tsyms.objc_msgSend)(
                    touch, g_tsyms.sel_setInitialTouchTimestamp, ts);
                setters_applied++;
            }
        }
    }
    /* If setWindow:/setView: do not exist on this build,
     * set the _window/_view ivars directly. [guess] on the ivar names;
     * the fallback only runs when the setter did not respond, so a
     * wrong guess costs nothing on builds where the setters exist. A
     * touch with no window is dropped silently by sendEvent:,
     * so this is the last line of defence for route C. The
     * ivar sets do not count toward setters_applied: the "no UITouch
     * setters resolved" failure below still fires when the core setters
     * are all missing, exactly as before. */
    if ((!window_ok || !view_ok) && g_tsyms.class_getInstanceVariable &&
        g_tsyms.object_setIvar && g_tsyms.objc_getClass) {
        void *touch_cls = g_tsyms.objc_getClass("UITouch");
        if (touch_cls) {
            if (!window_ok) {
                void *iv = g_tsyms.class_getInstanceVariable(touch_cls, "_window");
                if (iv) {
                    g_tsyms.object_setIvar(touch, iv, win);
                    window_ok = 1;
                }
            }
            if (!view_ok) {
                void *iv = g_tsyms.class_getInstanceVariable(touch_cls, "_view");
                if (iv) {
                    g_tsyms.object_setIvar(touch, iv, view);
                    view_ok = 1;
                }
            }
        }
    }
    /* Scalar ivar fallbacks. When a setter above did not
     * respond, write the same state straight into the ivar so the
     * touch still carries the right phase/location at sendEvent:
     * time. The window re-hit-tests Began touches from
     * locationInWindow, so a touch whose location setter never ran
     * is hit-tested at (0,0) and its touchesBegan: goes to the wrong
     * view; a touch whose phase setter never ran reads Began on
     * every phase, so endTracking: never runs. Ivar writes do NOT
     * count toward setters_applied (that line counts ObjC setter
     * calls); the trace line below names them separately. The
     * _setIsFirstTouchForView: / _setInitialTouchTimestamp: setters
     * have no fallback: they feed gesture timing, not UIControl
     * tracking. */
    {
        int ivar_phase = 0;
        int ivar_loc = 0;
        int ivar_ts = 0;
        int ivar_tap = 0;
        unsigned long tap_ul = 1UL;
        if (!phase_ok) {
            ivar_phase = touch_set_touch_scalar_ivar(touch_cls, touch, "_phase", &phase, sizeof(phase));
        }
        if (!loc_ok) {
            touch_point_t prev_pt;
            if (action == ISWS_TOUCH_DOWN) {
                prev_pt = pt;
            } else {
                prev_pt.x = g_direct_prev_x[id];
                prev_pt.y = g_direct_prev_y[id];
            }
            ivar_loc = touch_set_touch_scalar_ivar(touch_cls, touch, "_locationInWindow", &pt, sizeof(pt));
            if (ivar_loc) {
                touch_set_touch_scalar_ivar(touch_cls, touch, "_previousLocationInWindow", &prev_pt, sizeof(prev_pt));
            }
        }
        if (!ts_ok) {
            ivar_ts = touch_set_touch_scalar_ivar(touch_cls, touch, "_timestamp", &ts, sizeof(ts));
        }
        if (!tap_ok) {
            ivar_tap = touch_set_touch_scalar_ivar(touch_cls, touch, "_tapCount", &tap_ul, sizeof(tap_ul));
        }
        ivar_any = ivar_phase || ivar_loc || ivar_ts || ivar_tap;
        if (ivar_any && g_touch_trace) {
            char ibuf[64];
            size_t ipos = 0;
            const char *ip = "[touch] direct ivar phase=";
            for (size_t i = 0; ip[i]; i++) ibuf[ipos++] = ip[i];
            ibuf[ipos++] = ivar_phase ? '1' : '0';
            ip = " loc=";
            for (size_t i = 0; ip[i]; i++) ibuf[ipos++] = ip[i];
            ibuf[ipos++] = ivar_loc ? '1' : '0';
            ip = " ts=";
            for (size_t i = 0; ip[i]; i++) ibuf[ipos++] = ip[i];
            ibuf[ipos++] = ivar_ts ? '1' : '0';
            ip = " tap=";
            for (size_t i = 0; ip[i]; i++) ibuf[ipos++] = ip[i];
            ibuf[ipos++] = ivar_tap ? '1' : '0';
            ibuf[ipos++] = '\n';
            touch_log_write(ibuf, ipos);
        }
        /* Remember this phase's location for the next phase's previous. */
        g_direct_prev_x[id] = pt.x;
        g_direct_prev_y[id] = pt.y;
    }
    /* Read the window/view back through the public getters
     * and trace-log the result. sendEvent: routes a UITouchesEvent to
     * touch.window; win=0 here means the event will be dropped,
     * and view=win means hit-testing found no subview (the tab tap
     * would miss the tab bar). */
    if (g_touch_trace) {
        void *rb_win = NULL;
        void *rb_view = NULL;
        if (g_tsyms.sel_window && objc_responds(touch, g_tsyms.sel_window)) {
            rb_win = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_window);
        }
        if (g_tsyms.sel_view && objc_responds(touch, g_tsyms.sel_view)) {
            rb_view = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
                touch, g_tsyms.sel_view);
        }
        {
            char vbuf[64];
            size_t vpos = 0;
            const char *vp = "[touch] direct win=";
            for (size_t i = 0; vp[i]; i++) vbuf[vpos++] = vp[i];
            vbuf[vpos++] = rb_win ? '1' : '0';
            vp = " view=";
            for (size_t i = 0; vp[i]; i++) vbuf[vpos++] = vp[i];
            if (!rb_view) {
                vp = "nil";
            } else if (rb_view == win) {
                vp = "win";
            } else {
                vp = "subview";
            }
            for (size_t i = 0; vp[i]; i++) vbuf[vpos++] = vp[i];
            vbuf[vpos++] = '\n';
            touch_log_write(vbuf, vpos);
        }
    }
    /* Read the touch state back through the ivar offsets
     * and log it, so the next device run proves what UIKit actually
     * received (phase, location in milli-points). A mismatch against
     * the intended phase/location names the setter that lied.
     * Skipped silently when the ivar lookup fails. */
    if (g_touch_trace && g_tsyms.class_getInstanceVariable && g_tsyms.ivar_getOffset) {
        void *ivp = g_tsyms.class_getInstanceVariable(touch_cls, "_phase");
        void *ivl = g_tsyms.class_getInstanceVariable(touch_cls, "_locationInWindow");
        if (ivp && ivl) {
            long offp = g_tsyms.ivar_getOffset(ivp);
            long offl = g_tsyms.ivar_getOffset(ivl);
            if (offp >= 0 && offl >= 0) {
                long rb_phase = *(long *)((char *)touch + offp);
                touch_point_t rb_loc = *(touch_point_t *)((char *)touch + offl);
                char rbuf[96];
                size_t rpos = 0;
                const char *rp = "[touch] direct readback phase=";
                for (size_t i = 0; rp[i]; i++) rbuf[rpos++] = rp[i];
                rpos += fmt_u64(rbuf + rpos, (uint64_t)rb_phase);
                rp = " loc=";
                for (size_t i = 0; rp[i]; i++) rbuf[rpos++] = rp[i];
                rpos += fmt_milli(rbuf + rpos, rb_loc.x);
                rbuf[rpos++] = ',';
                rpos += fmt_milli(rbuf + rpos, rb_loc.y);
                rbuf[rpos++] = '\n';
                touch_log_write(rbuf, rpos);
            }
        }
    }
    if (setters_applied == 0 && !ivar_any) {
        if (g_touch_trace) touch_log_str("[touch] direct unavailable: no UITouch setters or ivars resolved\n");
        return -1;
    }

    /* KIF route: reuse the app's shared UITouchesEvent, clear it, attach the
     * HID event and add the touch, then sendEvent:. */
    {
        void *s_te = g_tsyms.sel_registerName("_touchesEvent");
        void *s_clr = g_tsyms.sel_registerName("_clearTouches");
        void *s_add = g_tsyms.sel_registerName("_addTouch:forDelayedDelivery:");
        void *s_hid = g_tsyms.sel_registerName("_setHIDEvent:");
        void *kev = NULL;
        if (objc_responds(app, s_te)) {
            kev = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(app, s_te);
        }
        if (kev && objc_responds(kev, s_add)) {
            if (objc_responds(kev, s_clr)) {
                ((void (*)(void *, void *))g_tsyms.objc_msgSend)(kev, s_clr);
            }
            {   /* The two CFRetain calls previously here leaked two
                 * retains per phase with no matching release (the parent
                 * is CFReleased once per inject in touch_inject). The
                 * parent is owned by the event through _setHidEvent:
                 * (a strong property, which retains) plus the
                 * create/release pair in touch_inject, so the extra
                 * retains are not needed. Removed. */
                /* Retain once, on DOWN only. Retaining on
                 * every phase leaked one retain per phase; the release
                 * below (on UP/CANCEL) balances this one. */
                if (touch && action == ISWS_TOUCH_DOWN) {
                    void *s_ret = g_tsyms.sel_registerName("retain");
                    ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(touch, s_ret);
                }
            }
            if (hid_event && objc_responds(kev, s_hid)) {
                ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(kev, s_hid, hid_event);
            }
            ((void (*)(void *, void *, void *, int))g_tsyms.objc_msgSend)(kev, s_add, touch, 0);
            if (g_touch_trace) {
                static const char *(*cls_name)(void *);
                if (!cls_name) cls_name = (const char *(*)(void *))lsk_sign_fn(touch_dlsym("object_getClassName"));
                touch_log_str("[touch] direct kif sendEvent view=");
                if (cls_name && view) touch_log_str(cls_name(view));
                touch_log_str(" phase=");
                touch_log_str(phase == TOUCH_PHASE_BEGAN ? "began" : phase == TOUCH_PHASE_ENDED ? "ended" : "other");
                touch_log_str("\n");
            }
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(app, g_tsyms.sel_sendEvent, kev);
            /* Same setters line as the fallback path below, so the KIF
             * path (the one that runs on device) is observable too. */
            if (g_touch_trace) touch_log_direct_setters(phase, setters_applied, setters_tried);
            if (phase == TOUCH_PHASE_ENDED && view && g_touch_tap_hack) {
                /* The interim TouchUpInside hack, now
                 * behind IOSWINE_TOUCH_TAP_HACK=1 (default off). Real
                 * UIControl tracking fires the action itself; leaving
                 * the hack on would double-fire every tap. Re-enable
                 * it only if a device run shows tracking still does
                 * not complete. */
                void *ctl_cls = g_tsyms.objc_getClass("UIControl");
                void *s_kind = g_tsyms.sel_registerName("isKindOfClass:");
                void *s_sup = g_tsyms.sel_registerName("superview");
                void *s_fire = g_tsyms.sel_registerName("sendActionsForControlEvents:");
                void *v = view;
                for (int depth = 0; v && depth < 8; depth++) {
                    if (((int (*)(void *, void *, void *))g_tsyms.objc_msgSend)(v, s_kind, ctl_cls) & 1) {
                        if (g_touch_trace) touch_log_str("[touch] direct fire TouchUpInside\n");
                        ((void (*)(void *, void *, unsigned long))g_tsyms.objc_msgSend)(v, s_fire, 1ul << 6);
                        break;
                    }
                    v = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(v, s_sup);
                }
            }
            if (action == ISWS_TOUCH_UP || action == ISWS_TOUCH_CANCEL) {
                /* Balance both retains, the DOWN retain and
                 * the alloc/init +1. The alloc retain was never released,
                 * so every touch was immortal. On UP the touch is still
                 * held by the shared event's set (added via _addTouch:),
                 * so both releases are safe; the next phase's
                 * _clearTouches frees it. */
                if (touch) {
                    void *s_rel = g_tsyms.sel_registerName("release");
                    ((void (*)(void *, void *))g_tsyms.objc_msgSend)(touch, s_rel);
                    ((void (*)(void *, void *))g_tsyms.objc_msgSend)(touch, s_rel);
                }
                g_direct_touches[id] = NULL;
                g_direct_views[id] = NULL;
            }
            return 0;
        }
        if (g_touch_trace) touch_log_str("[touch] direct kif unavailable\n");
    }

    if (!g_tsyms.sel_alloc || !g_tsyms.sel_init) return -1;
    event = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(event_cls, g_tsyms.sel_alloc);
    if (event) {
        event = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(event, g_tsyms.sel_init);
    }
    if (!event) {
        if (g_touch_trace) touch_log_str("[touch] direct unavailable: UITouchesEvent alloc/init failed\n");
        return -1;
    }

    if (g_tsyms.sel_setWithObject) {
        set = ((void *(*)(void *, void *, void *))g_tsyms.objc_msgSend)(
            nsset_cls, g_tsyms.sel_setWithObject, touch);
    }
    if (!set) {
        if (g_touch_trace) touch_log_str("[touch] direct unavailable: NSSet setWithObject: failed\n");
        return -1;
    }

    /* Attach the touches set to the event. Prefer the _touches ivar,
     * then _setTouches:, then setTouches:. [guess] on all three names. */
    {
        int attached = 0;
        if (g_tsyms.class_getInstanceVariable && g_tsyms.object_setIvar) {
            void *iv = g_tsyms.class_getInstanceVariable(event_cls, "_touches");
            if (iv) {
                g_tsyms.object_setIvar(event, iv, set);
                /* object_setIvar does not retain. The set
                 * is autoreleased and touch_inject pops its pool after
                 * delivery, which freed the set and left event._touches
                 * dangling. Retain it so it outlives the pool; the event
                 * is leaked and never freed, so this retain matches the
                 * event's lifetime. */
                {
                    void *s_ret = g_tsyms.sel_registerName("retain");
                    ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(set, s_ret);
                }
                attached = 1;
            }
        }
        if (!attached && g_tsyms.sel_setTouchesPriv &&
            objc_responds(event, g_tsyms.sel_setTouchesPriv)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                event, g_tsyms.sel_setTouchesPriv, set);
            attached = 1;
        }
        if (!attached && g_tsyms.sel_setTouches &&
            objc_responds(event, g_tsyms.sel_setTouches)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                event, g_tsyms.sel_setTouches, set);
            attached = 1;
        }
        if (!attached) {
            if (g_touch_trace) touch_log_str("[touch] direct unavailable: no way to attach touches to event\n");
            return -1;
        }
    }

    if (g_tsyms.sel_setEventTimestamp &&
        objc_responds(event, g_tsyms.sel_setEventTimestamp)) {
        ((void (*)(void *, void *, double))g_tsyms.objc_msgSend)(
            event, g_tsyms.sel_setEventTimestamp, ts);
    }

    if (g_tsyms.sel_sendEvent) {
        ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
            app, g_tsyms.sel_sendEvent, event);
    }

    if (g_touch_trace) touch_log_direct_setters(phase, setters_applied, setters_tried);

    if (action == ISWS_TOUCH_UP || action == ISWS_TOUCH_CANCEL) {
        /* Balance the alloc/init +1, which was never
         * released, so every fallback touch was immortal. The touch is
         * still held by the NSSet attached to the event (which retains
         * it), so this release is safe. */
        if (touch) {
            void *s_rel = g_tsyms.sel_registerName("release");
            ((void (*)(void *, void *))g_tsyms.objc_msgSend)(touch, s_rel);
        }
        g_direct_touches[id] = NULL;
        g_direct_views[id] = NULL;
    }
    return 0;
}

/* Route D (default): the combined delivery path.
 *
 * Why this exists: route A is dead on device. The
 * wrapper _receiveHIDEvent: only enqueues onto the fetch thread; the
 * block that would run _receiveHIDEventInternal: never executes here
 * (no HID source feeds the fetch thread's runloop), so the event sits
 * in _incomingHIDEventsFiltered forever (trace: "queue filtered=0
 * raw=-") and nothing ever reaches sendEvent:. Calling
 * _receiveHIDEventInternal: directly does reach sendEvent: (36x,
 * touch count 1), so this path does the same, synchronously on the
 * main queue where touch_inject already runs, then drives the rest of
 * the chain by hand:
 *   1. _receiveHIDEventInternal: (pinned address) on the main queue.
 *      This is the call that enqueues the event into the fetcher's
 *      filtered queue and notifies the dispatcher; the wrapper differs
 *      only in that it performs this call asynchronously on the fetch
 *      thread instead of here.
 *   2. Signal the dispatcher's _mainRunLoopCFSource and wake the main
 *      runloop, so the main runloop source that would normally drain
 *      the queue is serviced even though nothing else signals it.
 *   3. drainEventsIntoEnvironment: directly, moving the filtered
 *      events into the UIEventEnvironment.
 *   4. processEventQueue(env, 4) directly. This is the step old route
 *      B was missing: the drain only moves events into the
 *      environment's queue; processEventQueue is what pulls them out
 *      and dispatches them to -[UIApplication sendEvent:].
 *   5. If the sendEvent: hook counted no delivery for this phase, run
 *      route C (direct UITouchesEvent) for the same phase. The count
 *      is read through the hook installed by install_sendevent_hook,
 *      which runs in every mode for exactly this reason.
 *
 * What this does NOT do: it does not assign the touch a window. If
 * sendEvent: fires but -[UIWindow sendEvent:] never runs and the tab
 * does not switch, the event died downstream (hit-test / context id);
 * the fallback deliberately does not
 * trigger then, because sendEvent: DID fire.
 */
static void touch_deliver_combined(void *app, void *fetcher, void *win,
                                   void *parent, uint32_t action, uint32_t id,
                                   double wx, double wy, uint64_t now) {
    uint64_t seq_before = __atomic_load_n(&g_sendevent_count, __ATOMIC_RELAXED);
    void *disp;
    void *env;

    if (g_tsyms.receive_internal) {
        g_tsyms.receive_internal(fetcher, parent);
        if (g_touch_trace) {
            touch_log_str("[touch] internal receive\n");
            touch_log_queue_counts(fetcher);
        }
    } else if (g_tsyms.sel_receiveHIDEvent &&
               objc_responds(fetcher, g_tsyms.sel_receiveHIDEvent)) {
        ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
            fetcher, g_tsyms.sel_receiveHIDEvent, parent);
        if (g_touch_trace) {
            touch_log_str("[touch] internal receive (direct fn absent)\n");
            touch_log_queue_counts(fetcher);
        }
    }

    disp = touch_ivar(app, "_eventDispatcher");
    if (!disp) disp = touch_ivar(fetcher, "_eventDispatcher");
#ifndef TOUCH_HOST_TEST
    if (!disp && fetcher) disp = *(void **)((char *)fetcher + 24);
#endif
    if (disp) {
        void *src = touch_ivar(disp, "_mainRunLoopCFSource");
        void *rl = touch_ivar(disp, "_mainRunLoop");
#ifndef TOUCH_HOST_TEST
        /* Unverified fallback offsets (finding 6); trace-log when used. */
        if (!src) {
            src = *(void **)((char *)disp + 32);
            if (src && g_touch_trace) touch_log_str("[touch] src via offset +32\n");
        }
        if (!rl) {
            rl = *(void **)((char *)disp + 40);
            if (rl && g_touch_trace) touch_log_str("[touch] rl via offset +40\n");
        }
#endif
        if (!rl && g_tsyms.CFRunLoopGetMain) {
            rl = g_tsyms.CFRunLoopGetMain();
        }
        if (src && g_tsyms.CFRunLoopSourceSignal) {
            g_tsyms.CFRunLoopSourceSignal(src);
            if (rl && g_tsyms.CFRunLoopWakeUp) {
                g_tsyms.CFRunLoopWakeUp(rl);
            }
            if (g_touch_trace) {
                touch_log_str("[touch] source signalled\n");
            }
        }
        env = touch_ivar(disp, "_environment");
#ifndef TOUCH_HOST_TEST
        if (!env) {
            env = *(void **)((char *)disp + 8);
            if (env && g_touch_trace) touch_log_str("[touch] env via offset +8\n");
        }
#endif
        if (env && g_tsyms.drain_events) {
            g_tsyms.drain_events(fetcher, env);
        }
        if (env && g_tsyms.process_event_queue) {
            g_tsyms.process_event_queue(env, 4);
            if (g_touch_trace) {
                touch_log_str("[touch] processEventQueue\n");
            }
        }
    }

    /* Fallback to route C when the HID chain dropped the event: no
     * sendEvent: delivery was observed for this phase. When sendEvent:
     * fired, the event is UIKit's problem from here on and
     * no fallback runs, even if the tab does not switch. */
    if (__atomic_load_n(&g_sendevent_count, __ATOMIC_RELAXED) == seq_before && win) {
        if (g_touch_trace) {
            touch_log_str("[touch] combined: no sendEvent, falling back to direct\n");
        }
        touch_direct_inject(app, win, action, id, wx, wy, now, parent);
    }
}

void touch_inject(void *ctx) {
    touch_slot_t *slot = (touch_slot_t *)ctx;
    touch_slot_t local;
    double win_w;
    double win_h;
    double wx;
    double wy;
    uint32_t id;
    uint32_t action;
    uint32_t named_mask = 0;
    uint32_t named_range = 0;
    uint32_t named_touch = 0;
    double named_pressure = 0.0;
    uint32_t parent_mask = 0;
    uint32_t any_touch = 0;
    int leaving = 0;
    void *pool = NULL;
    void *app_cls;
    void *app;
    void *fetcher;
    void *win;
    uint32_t ctx_id = 0;
    uint64_t now;
    void *parent;

    if (!slot) return;
    local = *slot;
    __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);

    id = local.pointer_id;
    action = local.action;
    if (id >= ISWS_MAX_POINTERS || action > ISWS_TOUCH_CANCEL) return;

    if (ensure_inject_syms() != 0) {
        if (!g_logged_missing_syms) {
            g_logged_missing_syms = 1;
            touch_log_str("[touch] inject unavailable: missing IOKit/ObjC symbols\n");
        }
        return;
    }

    get_window_size(&win_w, &win_h);
    wx = local.norm_x * win_w;
    wy = local.norm_y * win_h;

    if (action == ISWS_TOUCH_DOWN) {
        g_fingers[id].active = 1;
        g_fingers[id].wx = wx;
        g_fingers[id].wy = wy;
        named_mask = TOUCH_MASK_RANGE | TOUCH_MASK_TOUCH | TOUCH_MASK_POSITION;
        named_range = 1;
        named_touch = 1;
        named_pressure = 1.0;
    } else if (action == ISWS_TOUCH_MOVE) {
        if (!g_fingers[id].active) return;
        if (g_fingers[id].wx == wx && g_fingers[id].wy == wy) return;
        g_fingers[id].wx = wx;
        g_fingers[id].wy = wy;
        named_mask = TOUCH_MASK_POSITION;
        named_range = 1;
        named_touch = 1;
        named_pressure = 1.0;
    } else if (action == ISWS_TOUCH_UP) {
        if (!g_fingers[id].active) return;
        g_fingers[id].wx = wx;
        g_fingers[id].wy = wy;
        named_mask = TOUCH_MASK_RANGE | TOUCH_MASK_TOUCH;
        named_range = 0;
        named_touch = 0;
        named_pressure = 0.0;
        leaving = 1;
    } else { /* ISWS_TOUCH_CANCEL */
        if (!g_fingers[id].active) return;
        g_fingers[id].wx = wx;
        g_fingers[id].wy = wy;
        named_mask = TOUCH_MASK_CANCEL | TOUCH_MASK_RANGE | TOUCH_MASK_TOUCH;
        named_range = 0;
        named_touch = 0;
        named_pressure = 0.0;
        leaving = 1;
    }

    /* The slot carries normalized coordinates; keep the window-point
     * copies for the log line and the direct path's hit test, and the
     * normalized copies for the HID event constructors (finding 1). */
    g_fingers[id].nx = local.norm_x;
    g_fingers[id].ny = local.norm_y;

    for (uint32_t i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (!g_fingers[i].active) continue;
        if (i == id) {
            parent_mask |= named_mask;
            if (named_touch) any_touch = 1;
        } else {
            parent_mask |= (TOUCH_MASK_RANGE | TOUCH_MASK_TOUCH);
            any_touch = 1;
        }
    }

    if (g_tsyms.objc_autoreleasePoolPush) {
        pool = g_tsyms.objc_autoreleasePoolPush();
    }

    app_cls = g_tsyms.objc_getClass("UIApplication");
    if (!app_cls || !objc_responds(app_cls, g_tsyms.sel_sharedApplication)) {
        goto out_clear;
    }
    app = ((void *(*)(void *, void *))g_tsyms.objc_msgSend)(
        app_cls, g_tsyms.sel_sharedApplication);
    if (!app) goto out_clear;

    install_sendevent_hook(app);

    fetcher = touch_find_fetcher(app);
    if (!fetcher) {
        if (!g_logged_missing_fetcher) {
            g_logged_missing_fetcher = 1;
            touch_log_str("[touch] UIEventFetcher unavailable\n");
        }
        goto out_clear;
    }

    win = touch_find_window(app);
    /* Resolve the context id from the layer CAContext
     * first (see touch_resolve_context_id); [win _contextId] alone
     * returned per-run garbage on device (0x9e8fe2a3 / 0xdc438f25). */
    ctx_id = touch_resolve_context_id(win);

    now = touch_mach_abs_time();
    /* Coordinates are NORMALIZED [0,1] (finding 1): UIKit multiplies by
     * the screen size when it builds a UITouch. The old code passed window
     * points (107, 791) here, which put the touch far outside every view. */
    parent = g_tsyms.IOHIDEventCreateDigitizerEvent(
        NULL, now, TOUCH_TRANSDUCER_HAND, 0, 0,
        parent_mask, 0,
        local.norm_x, local.norm_y, 0.0, 0.0, 0.0,
        1, any_touch,
        TOUCH_OPT_COLLECTION | TOUCH_OPT_DISP_INTEG);
    if (!parent) goto out_clear;

    if (g_tsyms.IOHIDEventSetIntegerValue) {
        g_tsyms.IOHIDEventSetIntegerValue(parent, TOUCH_FIELD_DISP_INTEG, 1);
        /* The 0xb0005 context-id field is an unconfirmed [guess] that
         * collides with the transducerType slot; an earlier version wrote
         * it unconditionally, which may corrupt the event. Only write it
         * when IOSWINE_TOUCH_CTX_FIELD=1. The context id itself goes
         * through BKSHIDEventSetDigitizerInfo below (the WebKit/Chromium shape). */
        if (g_ctx_field && ctx_id != 0) {
            g_tsyms.IOHIDEventSetIntegerValue(parent, TOUCH_FIELD_CONTEXT_ID, (int64_t)ctx_id);
        }
    }
    /* Stamp the context id the way backboardd does. WebKit ASSERTs the id is non-zero; pass 0 through
     * only by not calling when ctx_id is 0. */
    if (ctx_id != 0 && g_tsyms.BKSHIDEventSetDigitizerInfo) {
        g_tsyms.BKSHIDEventSetDigitizerInfo(parent, ctx_id, 0, 0, NULL,
                                            touch_timestamp_seconds(now), 1.0f);
    }
    if (g_tsyms.IOHIDEventSetSenderID) {
        g_tsyms.IOHIDEventSetSenderID(parent, touch_choose_sender_id(ctx_id));
    }

    for (uint32_t i = 0; i < ISWS_MAX_POINTERS; i++) {
        uint32_t fmask;
        uint32_t frange;
        uint32_t ftouch;
        double fpressure;
        double fx;
        double fy;
        void *child;

        if (!g_fingers[i].active) continue;
        if (i == id) {
            fmask = named_mask;
            frange = named_range;
            ftouch = named_touch;
            fpressure = named_pressure;
            fx = local.norm_x;
            fy = local.norm_y;
        } else {
            fmask = TOUCH_MASK_RANGE | TOUCH_MASK_TOUCH;
            frange = 1;
            ftouch = 1;
            fpressure = 1.0;
            fx = g_fingers[i].nx;
            fy = g_fingers[i].ny;
        }

        child = g_tsyms.IOHIDEventCreateDigitizerFingerEvent(
            NULL, now, i, 2 /* Finger */, fmask,
            fx, fy, 0.0, fpressure, 0.0,
            frange, ftouch, TOUCH_OPT_DISP_INTEG);
        if (child) {
            if (g_tsyms.IOHIDEventSetIntegerValue) {
                g_tsyms.IOHIDEventSetIntegerValue(child, TOUCH_FIELD_DISP_INTEG, 1);
                if (g_ctx_field && ctx_id != 0) {
                    g_tsyms.IOHIDEventSetIntegerValue(child, TOUCH_FIELD_CONTEXT_ID, (int64_t)ctx_id);
                }
            }
            g_tsyms.IOHIDEventAppendEvent(parent, child, 0);
            g_tsyms.CFRelease(child);
        }
    }

    if (leaving) {
        g_fingers[id].active = 0;
        g_fingers[id].wx = 0.0;
        g_fingers[id].wy = 0.0;
        g_fingers[id].nx = 0.0;
        g_fingers[id].ny = 0.0;
    }

    /* 1. Diagnostic stage: fetcher thread presence */
    void *fetch_rl = touch_ivar(fetcher, "_cfRunLoop");
    int thread_present = (fetch_rl != NULL);
    if (g_touch_trace) {
        char tbuf[64];
        size_t tpos = 0;
        const char *tp = "[touch] fetcher thread present? ";
        for (size_t i = 0; tp[i]; i++) tbuf[tpos++] = tp[i];
        tbuf[tpos++] = thread_present ? '1' : '0';
        tbuf[tpos++] = '\n';
        touch_log_write(tbuf, tpos);
    }

    touch_log_event(action, id, wx, wy);
    if (g_touch_trace) {
        touch_log_detail(action, id, local.norm_x, local.norm_y,
                         local.timestamp_ns);
    }

    /* Install trace hooks (trace mode only) before delivery so the
     * dispatcher / drain / sendEvent stages are observable on device. */
    {
        void *disp0 = touch_ivar(app, "_eventDispatcher");
        if (!disp0) disp0 = touch_ivar(fetcher, "_eventDispatcher");
        install_pipeline_hooks(fetcher, disp0);
    }

    if (g_touch_direct) {
        /* Route C: direct UITouchesEvent + sendEvent:, bypassing the HID
         * layer and the _UIEventDeferringManager entirely. */
        touch_direct_inject(app, win, action, id, wx, wy, now, parent);
    } else if (g_touch_queue) {
        /* Route A (IOSWINE_TOUCH_QUEUE=1): the thread-safe
         * _receiveHIDEvent: wrapper. It retains the event and performs
         * the block onto the fetch thread, which runs
         * _receiveHIDEventInternal:, signals the dispatcher, and the
         * main runloop source drains into the environment. No manual
         * signal or drain here: they would run before the fetch-thread
         * block executes and drain an empty queue (finding 2). Kept
         * only for A/B comparison: DEAD ON DEVICE, every
         * event queued and nothing ever reached sendEvent:. */
        if (g_tsyms.sel_receiveHIDEvent &&
            objc_responds(fetcher, g_tsyms.sel_receiveHIDEvent)) {
            ((void (*)(void *, void *, void *))g_tsyms.objc_msgSend)(
                fetcher, g_tsyms.sel_receiveHIDEvent, parent);
            if (g_touch_trace) {
                touch_log_str("[touch] queued via _receiveHIDEvent:\n");
                touch_log_queue_counts(fetcher);
            }
        } else if (g_tsyms.receive_internal) {
            g_tsyms.receive_internal(fetcher, parent);
            if (g_touch_trace) {
                touch_log_str("[touch] internal receive (wrapper absent)\n");
                touch_log_queue_counts(fetcher);
            }
        }
    } else {
        /* Route D (default): the combined path.
         * IOSWINE_TOUCH_INTERNAL=1 also selects this path (old route B
         * name kept as an alias); route B was this same sequence minus
         * processEventQueue and minus the sendEvent fallback. */
        touch_deliver_combined(app, fetcher, win, parent, action, id, wx, wy, now);
    }

    g_tsyms.CFRelease(parent);

    if (pool && g_tsyms.objc_autoreleasePoolPop) {
        g_tsyms.objc_autoreleasePoolPop(pool);
    }
    return;

out_clear:
    if (leaving) {
        g_fingers[id].active = 0;
        g_fingers[id].wx = 0.0;
        g_fingers[id].wy = 0.0;
        g_fingers[id].nx = 0.0;
        g_fingers[id].ny = 0.0;
    }
    if (pool && g_tsyms.objc_autoreleasePoolPop) {
        g_tsyms.objc_autoreleasePoolPop(pool);
    }
}

/* ------------------------------------------------------------------ */
/* Queue posting & background socket reader thread                    */
/* ------------------------------------------------------------------ */

static int enqueue_touch_for_main(const isws_touch_t *t, uint32_t client_w, uint32_t client_h) {
    double nx;
    double ny;
    touch_slot_t *slot;
    if (touch_map_coords(t, client_w, client_h, 1.0, 1.0, &nx, &ny, NULL, NULL) != 0) {
        g_drop_invalid++;
        return -1;
    }
    /* Claim the slot atomically. g_slot_next was a
     * plain counter incremented non-atomically, so two concurrent
     * enqueue_touch_for_main callers (the repro test's two producer
     * threads) could read the same value, write the same slot, and
     * one phase overwrote the other: the overwritten phase was lost
     * (its block read the overwriter's data, and the duplicate
     * delivery was suppressed by touch_inject's identical-MOVE /
     * inactive-finger skip), so the storm lost 1-4 phases with no
     * counter incremented. On device the enqueue runs on a single
     * reader thread, so this is test-visible only, but the race is
     * real. Each caller now claims a unique slot with an atomic
     * fetch-add; the in_use check below still applies. */
    slot = &g_slots[__atomic_fetch_add(&g_slot_next, 1u, __ATOMIC_RELAXED) % TOUCH_RING_LEN];
    if (__atomic_load_n(&slot->in_use, __ATOMIC_ACQUIRE) != 0) {
        /* Ring full: the main queue has not drained the slot yet.
         * Count it, and log the first drop and then every 256th when
         * tracing, so a MOVE burst does not flood the log (F9). */
        g_drop_ring_full++;
        if (g_touch_trace && (g_drop_ring_full & 0xffu) == 1) {
            touch_log_str("[touch] ring full, event dropped\n");
        }
        return -1; /* ring full */
    }
    slot->action = t->action;
    slot->pointer_id = t->pointer_id;
    slot->pointer_count = t->pointer_count;
    slot->norm_x = nx;
    slot->norm_y = ny;
    slot->timestamp_ns = t->timestamp_ns;
    __atomic_store_n(&slot->in_use, 1, __ATOMIC_RELEASE);
    /* g_slot_next is advanced by the atomic
     * fetch-add at the claim above, not here. */

    if (g_tsyms.dispatch_async_f && g_tsyms.dispatch_main_q) {
        g_tsyms.dispatch_async_f(g_tsyms.dispatch_main_q, slot, touch_inject);
    } else {
        touch_inject(slot);
    }
    return 0;
}

/* Cancel every finger the table thinks is down, by enqueueing one
 * CANCEL per active finger at its last position (F7). Used when the
 * app loses focus and when the presenter disconnects: in both cases
 * the finger is gone for real, and leaving it marked down would make
 * the next tap for that pointer id arrive as a MOVE for a finger that
 * never went down. The CANCELs go through the ring like any other
 * event, so they are delivered on the main queue in order. */
static void touch_cancel_all(void) {
    double win_w;
    double win_h;
    isws_info_t info;
    uint32_t cw;
    uint32_t ch;
    uint32_t i;

    get_window_size(&win_w, &win_h);
    info = present_last_info();
    cw = info.client_w ? info.client_w : (uint32_t)(win_w + 0.5);
    ch = info.client_h ? info.client_h : (uint32_t)(win_h + 0.5);
    if (cw == 0) cw = (uint32_t)TOUCH_DEFAULT_WIN_W;
    if (ch == 0) ch = (uint32_t)TOUCH_DEFAULT_WIN_H;

    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        isws_touch_t t;
        if (!g_fingers[i].active) continue;
        t.action = ISWS_TOUCH_CANCEL;
        t.pointer_id = i;
        t.pointer_count = 1;
        t.x = (float)(g_fingers[i].wx / win_w * (double)cw);
        t.y = (float)(g_fingers[i].wy / win_h * (double)ch);
        t.timestamp_ns = 0;
        if (enqueue_touch_for_main(&t, cw, ch) == 0) {
            g_cancel_all_sent++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard handoff to the main queue                    */
/* ------------------------------------------------------------------ */

/* One-slot handoff for TEXT/KEY events: the reader thread copies the
 * event here and dispatch_async_f runs touch_keyboard_on_main on
 * the main queue, where keyboard_handle_event() calls UIKit. The
 * slot is single because text/key events are rare; while it is busy
 * (main has not drained it yet) the next one is dropped and
 * counted. isws_text_t.utf8 is borrowed from present.c's receive
 * buffer, so TEXT bytes are copied into g_kb_utf8 first: the next
 * pump would overwrite that buffer before the main queue runs.
 * Oversize text (over TOUCH_KB_UTF8_MAX bytes) is dropped rather
 * than truncated mid-sequence; a commit that long never occurs from
 * a soft keyboard. */
#define TOUCH_KB_UTF8_MAX 4096u
static present_event_t g_kb_slot;
static uint8_t g_kb_utf8[TOUCH_KB_UTF8_MAX];
static int g_kb_slot_busy;
static uint32_t g_drop_kb_busy;
static uint32_t g_drop_kb_oversize;

static void touch_keyboard_on_main(void *ctx) {
    const present_event_t *ev = (const present_event_t *)ctx;
    g_kb_slot_busy = 0;
    if (ev) {
        (void)keyboard_handle_event(ev);
    }
}

static int touch_enqueue_keyboard_for_main(const present_event_t *ev) {
    if (!ev) return -1;
    if (g_kb_slot_busy) {
        g_drop_kb_busy++;
        if (g_touch_trace) {
            touch_log_str("[touch] keyboard event dropped (slot busy)\n");
        }
        return -1;
    }
    g_kb_slot = *ev;
    if (ev->kind == PRESENT_EV_TEXT) {
        size_t len = ev->u.text.utf8_len;
        size_t i;
        if (len > TOUCH_KB_UTF8_MAX) {
            g_drop_kb_oversize++;
            if (g_touch_trace) {
                touch_log_str("[touch] keyboard text dropped (oversize)\n");
            }
            return -1;
        }
        for (i = 0; i < len; i++) {
            g_kb_utf8[i] = ev->u.text.utf8[i];
        }
        g_kb_slot.u.text.utf8 = g_kb_utf8;
    }
    g_kb_slot_busy = 1;
    if (g_tsyms.dispatch_async_f && g_tsyms.dispatch_main_q) {
        g_tsyms.dispatch_async_f(g_tsyms.dispatch_main_q, &g_kb_slot,
                                 touch_keyboard_on_main);
    } else {
        touch_keyboard_on_main(&g_kb_slot);
    }
    return 0;
}

#ifdef TOUCH_HOST_TEST
/* Host-test stubs for keyboard.c: touch_test links touch.c without
 * keyboard.c, so the real symbols are absent. The stubs record
 * calls so touch_test can assert the TEXT/KEY routing; the device
 * build never compiles this branch and links the real keyboard.c. */
static int g_kb_stub_init_calls;
static int g_kb_stub_handle_calls;
static int g_kb_stub_last_kind;
static int g_kb_stub_visible;
static int g_kb_stub_hide_calls;
static char g_kb_stub_last_text[64];
static size_t g_kb_stub_last_text_len;

void keyboard_init(void) { g_kb_stub_init_calls++; }

int keyboard_handle_event(const present_event_t *ev) {
    size_t len;
    size_t i;
    if (!ev) return -1;
    g_kb_stub_handle_calls++;
    g_kb_stub_last_kind = (int)ev->kind;
    if (ev->kind == PRESENT_EV_TEXT) {
        len = ev->u.text.utf8_len;
        if (len > sizeof(g_kb_stub_last_text) - 1) {
            len = sizeof(g_kb_stub_last_text) - 1;
        }
        for (i = 0; i < len; i++) {
            g_kb_stub_last_text[i] = (char)ev->u.text.utf8[i];
        }
        g_kb_stub_last_text[len] = '\0';
        g_kb_stub_last_text_len = ev->u.text.utf8_len;
    }
    return 0;
}

int keyboard_request_show(int show) {
    (void)show;
    g_kb_stub_hide_calls++;
    return 0;
}

int keyboard_is_visible(void) { return g_kb_stub_visible; }

int touch_test_kb_init_calls(void) { return g_kb_stub_init_calls; }
int touch_test_kb_calls(void) { return g_kb_stub_handle_calls; }
int touch_test_kb_last_kind(void) { return g_kb_stub_last_kind; }
int touch_test_kb_hide_calls(void) { return g_kb_stub_hide_calls; }
const char *touch_test_kb_last_text(void) { return g_kb_stub_last_text; }
void touch_test_kb_set_visible(int v) { g_kb_stub_visible = v ? 1 : 0; }
#endif /* TOUCH_HOST_TEST */

/* Handle one event dequeued from the presenter. This is the whole
 * reader-side dispatch, factored out of the reader thread so host
 * tests can drive it without a socket (F7, F8).
 *
 * TOUCH: geometry comes from the latest INFO, exactly as the reader
 * thread used to compute it. The wire timestamp is checked for
 * staleness per pointer: an event more than 1 s older than the last
 * accepted timestamp for that pointer is dropped and counted. A
 * timestamp of 0 means unknown (synthetic events) and is always
 * accepted. The timestamp is NOT used as the IOHIDEvent timestamp:
 * the wire clock is Linux CLOCK_MONOTONIC, the wrong epoch for iOS,
 * so touch_inject() uses mach_absolute_time() for the event itself.
 *
 * TEXT / KEY: routed to keyboard.c on the main queue
 * via touch_enqueue_keyboard_for_main(). The text bytes are copied
 * out of the borrowed present.c buffer first: dispatch_async_f is
 * asynchronous and the next pump would overwrite that buffer
 * before the main queue runs.
 *
 * FOCUS lost: cancel every active finger (F7) and hide the soft
 * keyboard if one is showing. FOCUS
 * gained and INFO: nothing to do here; INFO is already reflected
 * in present_last_info().
 *
 * Returns 0 when the event was accepted (or was a no-op), -1 when it
 * was dropped. */
int touch_handle_present_event(const present_event_t *ev) {
    if (!ev) return -1;
    if (ev->kind == PRESENT_EV_TOUCH) {
        isws_info_t info = present_last_info();
        uint32_t cw = info.client_w ? info.client_w : (present_stride() / 4u);
        uint32_t ch = info.client_h ? info.client_h : present_height();
        uint32_t id = ev->u.touch.pointer_id;
        int64_t ts = ev->u.touch.timestamp_ns;
        int rc;
        if (cw == 0 || ch == 0) return -1;
        if (id < ISWS_MAX_POINTERS && ts != 0 && g_wire_ts_set[id] &&
            ts < g_last_wire_ts[id] - 1000000000LL) {
            g_drop_stale++;
            if (g_touch_trace) {
                touch_log_str("[touch] stale event dropped\n");
            }
            return -1;
        }
        rc = enqueue_touch_for_main(&ev->u.touch, cw, ch);
        if (rc == 0 && id < ISWS_MAX_POINTERS && ts != 0) {
            g_last_wire_ts[id] = ts;
            g_wire_ts_set[id] = 1;
        }
        return rc;
    }
    if (ev->kind == PRESENT_EV_TEXT || ev->kind == PRESENT_EV_KEY) {
        return touch_enqueue_keyboard_for_main(ev);
    }
    if (ev->kind == PRESENT_EV_FOCUS) {
        if (ev->u.focus.focused == 0) {
            touch_cancel_all();
            if (keyboard_is_visible()) {
                (void)keyboard_request_show(0);
            }
        }
        return 0;
    }
    return 0;
}

#ifndef TOUCH_HOST_TEST
static void *touch_reader_thread(void *arg) {
    (void)arg;
    for (;;) {
        int wr;
        present_event_t ev;

        if (!present_connected()) {
            if (g_reader_was_connected) {
                /* The presenter went away with fingers possibly down:
                 * cancel them so the next session starts clean (F7). */
                touch_cancel_all();
                g_reader_was_connected = 0;
            }
            sleep_ms_raw(TOUCH_POLL_TIMEOUT_MS);
            continue;
        }
        g_reader_was_connected = 1;

        wr = present_wait_readable(TOUCH_POLL_TIMEOUT_MS);
        if (wr < 0) {
            sleep_ms_raw(TOUCH_POLL_TIMEOUT_MS);
            continue;
        }

        if (wr > 0 && present_pump() != 0) {
            sleep_ms_raw(TOUCH_POLL_TIMEOUT_MS);
            continue;
        }

        while (present_poll_event(&ev)) {
            touch_handle_present_event(&ev);
        }
    }
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Self-test (IOSWINE_TOUCH_SELFTEST=1)                               */
/* ------------------------------------------------------------------ */

static void *touch_selftest_worker(void *arg) {
    (void)arg;
    uint64_t before = touch_get_tab_index();

    double win_w, win_h;
    get_window_size(&win_w, &win_h);
    /* Client pixels, not window points (F6). The tap point is a
     * window point (107, 791); convert it with the latest INFO
     * mapping when one has arrived, and fall back to treating the
     * window size as the client size (the old behaviour) when no INFO
     * has arrived yet. Passing the window size as the client size and
     * the window point as client pixels only gave the right answer by
     * accident. */
    {
        isws_info_t info = present_last_info();
        uint32_t cw = info.client_w ? info.client_w : (uint32_t)(win_w + 0.5);
        uint32_t ch = info.client_h ? info.client_h : (uint32_t)(win_h + 0.5);
        if (cw == 0) cw = (uint32_t)TOUCH_DEFAULT_WIN_W;
        if (ch == 0) ch = (uint32_t)TOUCH_DEFAULT_WIN_H;

        /* Synthesize tap DOWN at window point (107, 791) */
        isws_touch_t down_t;
        down_t.action = ISWS_TOUCH_DOWN;
        down_t.pointer_id = 0;
        down_t.pointer_count = 1;
        down_t.x = (float)(107.0 * (double)cw / win_w);
        down_t.y = (float)(791.0 * (double)ch / win_h);
        down_t.timestamp_ns = 0;
        enqueue_touch_for_main(&down_t, cw, ch);

        sleep_ms_raw(60);

        /* Synthesize tap UP at window point (107, 791) */
        isws_touch_t up_t;
        up_t.action = ISWS_TOUCH_UP;
        up_t.pointer_id = 0;
        up_t.pointer_count = 1;
        up_t.x = (float)(107.0 * (double)cw / win_w);
        up_t.y = (float)(791.0 * (double)ch / win_h);
        up_t.timestamp_ns = 0;
        enqueue_touch_for_main(&up_t, cw, ch);
    }

    /* Poll for tab index update (1 s total; the default route is
     * synchronous on the main queue, so the tap is delivered before
     * this poll starts, but keep the full second: a fallback to route
     * C or a slow main queue must not flake the verdict). */
    uint64_t after = before;
    for (int iter = 0; iter < 20; iter++) {
        sleep_ms_raw(50);
        after = touch_get_tab_index();
        if (after != before) break;
    }

    char buf[64];
    size_t pos = 0;
    const char *p1 = "[touch] selftest tab before=";
    for (size_t i = 0; p1[i]; i++) buf[pos++] = p1[i];
    pos += fmt_u64(buf + pos, before);
    const char *p2 = " after=";
    for (size_t i = 0; p2[i]; i++) buf[pos++] = p2[i];
    pos += fmt_u64(buf + pos, after);
    buf[pos++] = '\n';
    touch_log_write(buf, pos);

    /* End-to-end delivery count, on its own line so the tab line above
     * stays byte-identical (F11). Pass on device is after=1 AND
     * sendevent>=1: the tab switch proves the app reacted, the count
     * proves the tap reached -[UIApplication sendEvent:] at all. When
     * the count is 0 but the tab still switched, the delivery went
     * through a path the hook does not see; when the count is >=1 but
     * after stays 0, the event reached sendEvent: and died downstream
     * (hit-testing, gesture environment). */
    {
        char sbuf[64];
        size_t spos = 0;
        const char *sp = "[touch] selftest sendevent=";
        for (size_t i = 0; sp[i]; i++) sbuf[spos++] = sp[i];
        spos += fmt_u64(sbuf + spos, g_sendevent_count);
        sbuf[spos++] = '\n';
        touch_log_write(sbuf, spos);
    }

    return NULL;
}

void touch_on_frame(uint64_t frame_no) {
    if (!g_touch_selftest) return;
    if (frame_no != 20) return;
    if (__atomic_exchange_n(&g_selftest_done, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    ensure_dispatch_syms();
    if (g_tsyms.pthread_create) {
        uint64_t th = 0;
        g_tsyms.pthread_create(&th, NULL, touch_selftest_worker, NULL);
    } else {
        touch_selftest_worker(NULL);
    }
}

void touch_init(void) {
    const char *en = touch_getenv("IOSWINE_TOUCH");
    const char *st = touch_getenv("IOSWINE_TOUCH_SELFTEST");
    int enabled = (en && en[0] == '1' && en[1] == '\0') ||
                  (st && st[0] == '1' && st[1] == '\0');
    if (!enabled) {
        return;
    }

    const char *tr = touch_getenv("IOSWINE_TOUCH_TRACE");
    g_touch_trace = (tr && tr[0] == '1' && tr[1] == '\0');
    g_touch_selftest = (st && st[0] == '1' && st[1] == '\0');

    /* Route selection. Default is route D (the combined
     * path: Internal + signal + drain + processEventQueue + sendEvent
     * fallback). IOSWINE_TOUCH_DIRECT=1 selects route C (UITouchesEvent
     * fallback); IOSWINE_TOUCH_QUEUE=1 selects route A (the old
     * wrapper default, kept only for A/B); IOSWINE_TOUCH_INTERNAL=1
     * selects route D as well (the old route B name is an alias).
     * DIRECT wins if several are set, then QUEUE, then INTERNAL. */
    const char *di = touch_getenv("IOSWINE_TOUCH_DIRECT");
    const char *qu = touch_getenv("IOSWINE_TOUCH_QUEUE");
    const char *in = touch_getenv("IOSWINE_TOUCH_INTERNAL");
    g_touch_direct = (di && di[0] == '1' && di[1] == '\0');
    g_touch_queue = (!g_touch_direct) && (qu && qu[0] == '1' && qu[1] == '\0');
    g_touch_internal = (!g_touch_direct && !g_touch_queue) &&
                       (in && in[0] == '1' && in[1] == '\0');

    /* Sender ID override (finding 3): "magic", "0", hex, or decimal. */
    const char *sid = touch_getenv("IOSWINE_TOUCH_SENDER_ID");
    g_sender_override = 0;
    g_sender_override_value = 0;
    if (sid && sid[0] != '\0') {
        uint64_t parsed = 0;
        if (touch_parse_u64(sid, &parsed) == 0) {
            g_sender_override = 1;
            g_sender_override_value = parsed;
        } else if (g_touch_trace) {
            touch_log_str("[touch] bad IOSWINE_TOUCH_SENDER_ID, ignored\n");
        }
    }

    /* Context-id override: forces the context id stamped
     * on the HID event. Must fit in 32 bits; anything else is ignored. */
    const char *cid = touch_getenv("IOSWINE_TOUCH_CTX_ID");
    g_ctx_override = 0;
    g_ctx_override_value = 0;
    if (cid && cid[0] != '\0') {
        uint64_t parsed = 0;
        if (touch_parse_u64(cid, &parsed) == 0 && parsed <= 0xFFFFFFFFu) {
            g_ctx_override = 1;
            g_ctx_override_value = parsed;
        } else if (g_touch_trace) {
            touch_log_str("[touch] bad IOSWINE_TOUCH_CTX_ID, ignored\n");
        }
    }

    /* Guessed 0xb0005 context-id field: off by default,
     * only written when IOSWINE_TOUCH_CTX_FIELD=1. */
    const char *cf = touch_getenv("IOSWINE_TOUCH_CTX_FIELD");
    g_ctx_field = (cf && cf[0] == '1' && cf[1] == '\0');

    /* The interim TouchUpInside hack. Off by default:
     * real UIControl tracking fires the action itself, and the hack
     * would double-fire. Set IOSWINE_TOUCH_TAP_HACK=1 only if a
     * device run shows tracking still does not complete. */
    const char *th = touch_getenv("IOSWINE_TOUCH_TAP_HACK");
    g_touch_tap_hack = (th && th[0] == '1' && th[1] == '\0');

    if (g_started) return;
    g_started = 1;

    if (ensure_dispatch_syms() != 0) {
        touch_log_str("[touch] init failed: missing libdispatch/pthread symbols\n");
        return;
    }

#ifndef TOUCH_HOST_TEST
    {
        uint64_t th = 0;
        if (g_tsyms.pthread_create(&th, NULL, touch_reader_thread, NULL) == 0) {
            touch_log_str("[touch] reader started\n");
        } else {
            touch_log_str("[touch] init failed: pthread_create failed\n");
        }
    }
#else
    touch_log_str("[touch] reader started\n");
#endif

    /* Keyboard subsystem: no-op unless
     * IOSWINE_KEYBOARD=1. Started here because the reader thread
     * above is what feeds it TEXT/KEY events. */
    keyboard_init();
}

/* ------------------------------------------------------------------ */
/* Host test helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef TOUCH_HOST_TEST
void touch_test_configure(const touch_test_ops_t *ops) {
    g_test_ops = ops;
    if (ops) {
        if (ops->receive_internal_fn) {
            g_tsyms.receive_internal = ops->receive_internal_fn;
        }
        if (ops->drain_events_fn) {
            g_tsyms.drain_events = ops->drain_events_fn;
        }
        if (ops->process_event_queue_fn) {
            g_tsyms.process_event_queue = ops->process_event_queue_fn;
        }
    }
}

void touch_test_reset(void) {
    /* Clear the test ops first: touch_test_configure stores a pointer
     * to the caller's stack-local ops struct, so a stale pointer here
     * is a use-after-scope read the next time touch_getenv runs
     * (ASan catches it when the stack layout shifts). Every test
     * calls touch_test_configure after reset, so nothing depends on
     * the old pointer surviving. */
    g_test_ops = NULL;
    touch_memset(&g_tsyms, 0, sizeof(g_tsyms));
    touch_memset(g_slots, 0, sizeof(g_slots));
    touch_memset(g_fingers, 0, sizeof(g_fingers));
    g_slot_next = 0;
    g_started = 0;
    g_logged_missing_fetcher = 0;
    g_logged_missing_syms = 0;
    g_touch_trace = 0;
    g_touch_selftest = 0;
    g_touch_direct = 0;
    g_touch_tap_hack = 0;
    g_touch_internal = 0;
    g_touch_queue = 0;
    g_sender_override = 0;
    g_sender_override_value = 0;
    g_ctx_override = 0;
    g_ctx_override_value = 0;
    g_ctx_field = 0;
    g_ctx_logged = 0;
    g_ctx_last_logged = 0;
    g_sendevent_hooked = 0;
    g_pipeline_hooks_installed = 0;
    g_selftest_done = 0;
    g_sendEvent_orig = NULL;
    g_sendevent_count = 0;
    g_move_log_n = 0;
    g_drop_ring_full = 0;
    g_drop_invalid = 0;
    g_drop_stale = 0;
    g_cancel_all_sent = 0;
    touch_memset(g_last_wire_ts, 0, sizeof(g_last_wire_ts));
    touch_memset(g_wire_ts_set, 0, sizeof(g_wire_ts_set));
    g_reader_was_connected = 0;
    g_drain_orig = NULL;
    g_dispatcher_received_orig = NULL;
    g_window_sendEvent_orig = NULL;
    touch_memset(g_direct_touches, 0, sizeof(g_direct_touches));
    touch_memset(g_direct_views, 0, sizeof(g_direct_views));
    touch_memset(g_direct_prev_x, 0, sizeof(g_direct_prev_x));
    touch_memset(g_direct_prev_y, 0, sizeof(g_direct_prev_y));
    g_direct_touch_count = 0;
    g_kb_slot_busy = 0;
    g_drop_kb_busy = 0;
    g_drop_kb_oversize = 0;
    g_kb_stub_init_calls = 0;
    g_kb_stub_handle_calls = 0;
    g_kb_stub_last_kind = 0;
    g_kb_stub_visible = 0;
    g_kb_stub_hide_calls = 0;
    g_kb_stub_last_text[0] = '\0';
    g_kb_stub_last_text_len = 0;
    win_lock_acquire();
    g_win_size_set = 0;
    g_win_w = 0.0;
    g_win_h = 0.0;
    win_lock_release();
}

uint64_t touch_test_sendevent_count(void) {
    return g_sendevent_count;
}

void touch_test_drop_counts(uint64_t *ring_full, uint64_t *invalid,
                            uint64_t *stale, uint64_t *cancel_all) {
    if (ring_full) *ring_full = g_drop_ring_full;
    if (invalid) *invalid = g_drop_invalid;
    if (stale) *stale = g_drop_stale;
    if (cancel_all) *cancel_all = g_cancel_all_sent;
}

int touch_test_process_touch(const isws_touch_t *t, uint32_t client_w, uint32_t client_h) {
    double nx;
    double ny;
    touch_slot_t slot;
    if (g_tsyms.dispatch_async_f && g_tsyms.dispatch_main_q) {
        return enqueue_touch_for_main(t, client_w, client_h);
    }
    if (touch_map_coords(t, client_w, client_h, 1.0, 1.0, &nx, &ny, NULL, NULL) != 0) {
        return -1;
    }
    touch_memset(&slot, 0, sizeof(slot));
    slot.in_use = 1;
    slot.action = t->action;
    slot.pointer_id = t->pointer_id;
    slot.pointer_count = t->pointer_count;
    slot.norm_x = nx;
    slot.norm_y = ny;
    slot.timestamp_ns = t->timestamp_ns;
    touch_inject(&slot);
    return 0;
}

uint32_t touch_test_fingers_down(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (g_fingers[i].active) n++;
    }
    return n;
}
#endif
