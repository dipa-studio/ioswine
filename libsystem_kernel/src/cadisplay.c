/*
 * cadisplay.c - CoreAnimation local display layer for ioswine.
 *
 * See cadisplay.h. Freestanding C11, no libc. Only
 * the present and iosurface functions are called;
 * called; no socket/memfd/mmap syscalls here (present.c and
 * iosurface.c own those). No function-pointer tables in static
 * data. memcpy is declared manually, following present.c.
 *
 * Zero-copy note (real display): for a 1080-wide surface
 * iosurface.c stores bytes_per_row aligned up to 64 bytes (4352),
 * while present_stride() is width * 4 (4320), so
 * cadisplay_is_zero_copy() is 0 and every frame takes the copy
 * path. That is correct behaviour for display correctness: the
 * presenter reads its memfd with stride 4320, so presenting a
 * 4352-stride surface memfd directly would shear the image. The
 * copy path handles the mismatch row by row. Zero-copy becomes
 * reachable for the real display only if iosurface.c stops
 * aligning the reported bytesPerRow (or present.c accepts a
 * stride override); that would be an external decision, not a
 * cadisplay.c change. The host test proves the zero-copy path
 * with a surface whose bytes_per_row equals the stride.
 *
 * Present-buffer fd note: present.c uses ONE memfd for both
 * buffers (buffer 1 starts at offset stride*height in the same
 * memfd), so a present_buffer_fd() getter would return the same
 * fd for both indices. cadisplay_set_present_fd() therefore
 * cannot distinguish the two buffers by fd alone; the fd match
 * picks the lowest matching index that is not in flight. Distinct
 * per-buffer fds (separate memfds in present.c) would make the
 * match exact; until then the copy path is the reliable one.
 *
 * No em dashes are used in this file.
 */
#include "cadisplay.h"
#include "attributegraph.h"

#if !defined(CADISPLAYLINK_HOST_TEST) && defined(__aarch64__)
#include "linux.h"
extern const char *lsk_getenv(const char *name);
extern uint64_t lsk_mach_absolute_time(void) __asm__("_mach_absolute_time");
extern int lsk_mach_timebase_info(void *info) __asm__("_mach_timebase_info");
extern int lsk_clock_gettime(int clk_id, void *tp) __asm__("_clock_gettime");
#endif

/* Forward declaration: the display-link registry (defined at the
 * end of this file) is cleared whenever the
 * display layer resets, so stale link records never outlive a
 * re-init or shutdown. */
static void cdl_reset_all(void);

/* memcpy, declared manually (freestanding; same pattern as
 * present.c). The copy path is the only user. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static int g_inited;
static uint32_t g_width;
static uint32_t g_height;
static uint32_t g_scale;
static uint32_t g_refresh_hz;
static int64_t g_last_vsync_ns;
static uint64_t g_last_frame_id;
static uint32_t g_current_surface_id;
static uint32_t g_frames_presented;
static uint32_t g_frames_zero_copy;
static uint32_t g_frames_copied;
static int g_next_index;          /* preferred copy-path buffer, 0/1 */
static int g_present_fd[2];       /* registered present buffer fds, -1 = unset */

/* FIFO of buffer indices with a PRESENT sent and no FRAME_DONE yet.
 * At most 2 entries (ISWS_NUM_BUFFERS). Each FRAME_DONE acks
 * exactly one PRESENT, in order, so cadisplay_on_vsync frees the
 * head. A buffer is never reused while it is in this queue. */
static int g_flight_q[2];
static int g_flight_len;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int ca_in_flight(int index) {
    int i;
    for (i = 0; i < g_flight_len; i++) {
        if (g_flight_q[i] == index) return 1;
    }
    return 0;
}

static void ca_mark_presented(int index) {
    if (g_flight_len < 2) {
        g_flight_q[g_flight_len] = index;
        g_flight_len++;
    }
}

static void ca_mark_acked(void) {
    int i;
    if (g_flight_len <= 0) return;
    for (i = 1; i < g_flight_len; i++) g_flight_q[i - 1] = g_flight_q[i];
    g_flight_len--;
}

/* Copy `rows` rows of `row_bytes` bytes from a surface (row stride
 * `src_stride`) into a present buffer (row stride `dst_stride`).
 * Row-by-row so a surface whose bytes_per_row differs from the
 * present stride (the 64-byte alignment case) still copies the
 * correct pixels. */
static void ca_copy_rows(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t rows, uint32_t row_bytes) {
    uint32_t r;
    for (r = 0; r < rows; r++) {
        memcpy(dst + (size_t)r * dst_stride,
               src + (size_t)r * src_stride, row_bytes);
    }
}

static void ca_reset(void) {
    g_inited = 0;
    g_width = 0;
    g_height = 0;
    g_scale = 0;
    g_refresh_hz = CADISPLAY_REFRESH_HZ;
    g_last_vsync_ns = -1;
    g_last_frame_id = (uint64_t)-1;
    g_current_surface_id = 0;
    g_frames_presented = 0;
    g_frames_zero_copy = 0;
    g_frames_copied = 0;
    g_next_index = 0;
    g_present_fd[0] = -1;
    g_present_fd[1] = -1;
    g_flight_q[0] = 0;
    g_flight_q[1] = 0;
    g_flight_len = 0;
    cdl_reset_all();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

int cadisplay_init(uint32_t width, uint32_t height, uint32_t scale) {
    if (width == 0 || height == 0 || scale == 0) return -1;
    ca_reset();
    g_width = width;
    g_height = height;
    g_scale = scale;
    g_inited = 1;
    return 0;
}

void cadisplay_shutdown(void) {
    ca_reset();
}

int cadisplay_connected(void) {
    return (g_inited && present_connected()) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Vsync                                                                */
/* ------------------------------------------------------------------ */

int cadisplay_on_vsync(int64_t vsync_ns) {
    uint64_t fid;
    if (!g_inited) return -1;
    g_last_vsync_ns = vsync_ns;
    fid = present_last_frame_id();
    if (fid != (uint64_t)-1) g_last_frame_id = fid;
    ca_mark_acked();
    return 0;
}

int64_t cadisplay_next_vsync_ns(void) {
    int64_t period;
    if (!g_inited || g_last_vsync_ns < 0) return -1;
    period = 1000000000LL / (int64_t)g_refresh_hz;
    return g_last_vsync_ns + period;
}

/* ------------------------------------------------------------------ */
/* Zero-copy eligibility                                                */
/* ------------------------------------------------------------------ */

int cadisplay_is_zero_copy(uint32_t surface_id) {
    iosurface_surface s;
    if (iosurface_get_surface(surface_id, &s) != KERN_SUCCESS) return 0;
    if (s.lock_count != 0) return 0;
    if (s.pixel_format != IOSURFACE_PIXEL_FORMAT_BGRA) return 0;
    if (s.bytes_per_row != present_stride()) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Frame forwarding                                                     */
/* ------------------------------------------------------------------ */

int cadisplay_on_frame(uint32_t surface_id) {
    return cadisplay_on_frame_damage(surface_id, NULL, 0);
}

int cadisplay_on_frame_damage(uint32_t surface_id, const isws_rect_t *damage,
                              uint32_t damage_count) {
    iosurface_surface s;
    int fd;
    int i;
    int idx;
    uint8_t *fb;
    uint32_t rows;
    uint32_t row_bytes;
    uint32_t stride;

    if (!g_inited) return -1;
    if (damage_count > ISWS_MAX_DAMAGE) return -1;
    if (damage_count > 0 && damage == NULL) return -1;
    if (iosurface_get_surface(surface_id, &s) != KERN_SUCCESS) return -1;
    if (s.lock_count != 0) return -1;
    if (s.addr == NULL) return -1;

    /* Zero-copy: format/stride match and the surface memfd matches
     * a registered present buffer fd. Pick the lowest matching
     * index that is not in flight. */
    if (cadisplay_is_zero_copy(surface_id) &&
        iosurface_fd(surface_id, &fd) == KERN_SUCCESS && fd >= 0) {
        for (i = 0; i < 2; i++) {
            if (g_present_fd[i] != fd) continue;
            if (ca_in_flight(i)) continue;
            if (present_present(i, damage, damage_count) != 0) return -1;
            ca_mark_presented(i);
            g_frames_presented++;
            g_frames_zero_copy++;
            g_next_index = 1 - i;
            return 0;
        }
        /* No matching index free: fall through to the copy path. */
    }

    /* Copy path: prefer the alternating next index, take the other
     * one if it is busy, give up if both are in flight. */
    if (!ca_in_flight(g_next_index)) {
        idx = g_next_index;
    } else if (!ca_in_flight(1 - g_next_index)) {
        idx = 1 - g_next_index;
    } else {
        return -1;
    }
    fb = present_framebuffer(idx);
    if (fb == NULL) return -1;
    stride = present_stride();
    if (stride == 0) return -1;
    rows = s.height < g_height ? s.height : g_height;
    row_bytes = s.bytes_per_row < stride ? s.bytes_per_row : stride;
    /* Partial copy: the surface is shorter than the display, or its
     * rows are narrower than the present stride. The rows and the
     * right-hand strip the copy does not cover would otherwise keep
     * the previous frame's pixels (stale garbage on screen), so
     * clear the whole destination buffer first, then copy. The
     * full-size case skips the clear: every byte is overwritten. */
    if (rows < g_height || row_bytes < stride) {
        memset(fb, 0, (size_t)stride * g_height);
    }
    ca_copy_rows(fb, stride, (const uint8_t *)s.addr, s.bytes_per_row,
                 rows, row_bytes);
    if (present_present(idx, damage, damage_count) != 0) return -1;
    ca_mark_presented(idx);
    g_frames_presented++;
    g_frames_copied++;
    g_next_index = 1 - idx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Present fds and current surface                                      */
/* ------------------------------------------------------------------ */

int cadisplay_set_present_fd(int index, int fd) {
    if (index < 0 || index > 1) return -1;
    g_present_fd[index] = fd;
    return 0;
}

uint32_t cadisplay_current_surface(void) {
    return g_current_surface_id;
}

void cadisplay_set_current_surface(uint32_t surface_id) {
    g_current_surface_id = surface_id;
}

uint32_t cadisplay_find_display_surface(void) {
    uint32_t count;
    uint32_t best = 0;
    uint32_t best_seed = 0;
    uint32_t found = 0;
    uint32_t id;
    iosurface_surface s;

    if (!g_inited) return 0;
    count = iosurface_surface_count();
    if (count == 0) return 0;
    /* Seed-aware preference: the
     * seed increments on every unlock, so the highest seed is the
     * surface whose frame most recently finished. Prefer it; on a
     * seed tie fall back to most-recently-created (highest id,
     * since ids increase with creation time). Stop once every live
     * surface has been seen. */
    for (id = 1; id <= CADISPLAY_MAX_SURFACE_SCAN && found < count; id++) {
        if (iosurface_get_surface(id, &s) != KERN_SUCCESS) continue;
        found++;
        if (s.width == g_width && s.height == g_height &&
            s.pixel_format == IOSURFACE_PIXEL_FORMAT_BGRA) {
            if (best == 0 || s.seed > best_seed ||
                (s.seed == best_seed && id > best)) {
                best = id;
                best_seed = s.seed;
            }
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Surface fd / seed helpers                                            */
/* ------------------------------------------------------------------ */

int cadisplay_surface_fd(uint32_t surface_id, int *fd_out) {
    if (fd_out == NULL) return -1;
    if (iosurface_fd(surface_id, fd_out) != KERN_SUCCESS) return -1;
    return 0;
}

int cadisplay_surface_seed(uint32_t surface_id, uint32_t *seed_out) {
    iosurface_surface s;
    if (seed_out == NULL) return -1;
    if (iosurface_get_surface(surface_id, &s) != KERN_SUCCESS) return -1;
    *seed_out = s.seed;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Info                                                                 */
/* ------------------------------------------------------------------ */

int cadisplay_info(cadisplay_info_t *out) {
    if (out == NULL || !g_inited) return -1;
    out->width = g_width;
    out->height = g_height;
    out->scale = g_scale;
    out->refresh_hz = g_refresh_hz;
    out->last_vsync_ns = g_last_vsync_ns;
    out->last_frame_id = g_last_frame_id;
    out->current_surface_id = g_current_surface_id;
    out->frames_presented = g_frames_presented;
    out->frames_zero_copy = g_frames_zero_copy;
    out->frames_copied = g_frames_copied;
    return 0;
}

/* ------------------------------------------------------------------ */
/* IOKit table. Strings only,  */
/* no function pointers. Trace-gating rule (that doc section 4.2):    */
/* register only IOSurfaceRoot first; register a display service      */
/* only after the trace shows a match for its class, or the full      */
/* table if CA's local display fails with only IOSurfaceRoot. All     */
/* six use iokit_register_service with a hand-built blob (not         */
/* _simple), kind GENERIC. The device-id / num_cores values are       */
/* placeholders (0 and 1); the real Adreno 730 values are unknown.   */
/* ------------------------------------------------------------------ */

static const cadisplay_iokit_service_t cadisplay_iokit_services_table[] = {
    { "IOMobileFramebuffer", "IOMobileFramebuffer",
      "IOName=IOMobileFramebuffer;IOFBTransform=0;IOFBDependentIndex=0;IOFBDependentID=0;IOFBMemorySize=18432000;IODisplayParameters={brightness={value=65536;max=65536}}" },
    { "AppleCLCD", "AppleCLCD",
      "IOName=AppleCLCD;IOFBTransform=0;IOFBDependentIndex=0;IOFBDependentID=0;IOFBMemorySize=18432000;IODisplayParameters={brightness={value=65536;max=65536}}" },
    { "IODisplay", "IODisplay",
      "IOName=IODisplay;DisplayVendorID=1552;DisplayProductName=Color LCD;DisplayPixelDimensions=1440x3200;IODisplayParameters={brightness={value=65536;max=65536}}" },
    { "IOAccelerator", "IOAccelerator",
      "IOName=IOAccelerator;model=Adreno 730;vendor-id=0x17CB;device-id=0;VRAM,totalMB=8192;GPUConfigurationVariable={num_cores=1}" },
    { "AGXAccelerator", "AGXAccelerator",
      "IOName=AGXAccelerator;model=Adreno 730;vendor-id=0x17CB;device-id=0;VRAM,totalMB=8192;GPUConfigurationVariable={num_cores=1}" },
    { "AppleARMBacklight", "AppleARMBacklight",
      "IOName=AppleARMBacklight;IODisplayParameters={brightness={value=65536;max=65536}}" },
};

const cadisplay_iokit_service_t *cadisplay_iokit_services(uint32_t *count_out) {
    if (count_out != NULL) *count_out = 6;
    return cadisplay_iokit_services_table;
}

/* ------------------------------------------------------------------ */
/* CADisplayLink                                  */
/*                                                                      */
/* Crash (B): +[CADisplayLink(CADisplayLinkPrivate)                     */
/* displayLinkWithDisplay:target:selector:] faults at +84               */
/* (0x183f96b9c) with x8=0. The display object exists (UIScreen        */
/* works through it: -[UIScreen _mainDisplayInit] calls                */
/* +[CADisplay mainDisplay], and the app has windows), but its         */
/* backing CA::Display* is NULL: with no render server,                */
/* CA::Display::ensure_displays never populated it (the display        */
/* query, MIG 40231 _CASGetDisplays, has no thread answering it).      */
/* The factory loads that NULL internal and dereferences it.           */
/* The same fault fires for a nil display argument. The fix never      */
/* lets the real factory run: both factory methods and every           */
/* instance method the app can call on the link are swizzled to a      */
/* registry here. A link is a genuine CADisplayLink instance           */
/* (class_createInstance, no init call, so no QuartzCore code ever     */
/* touches the broken internal); its callbacks fire from               */
/* lsk_cadisplay_vsync_tick, dispatched to the main queue.             */
/*                                                                      */
/* Threading: the tick runs on the presenter's FRAME_DONE thread;     */
/* creation / invalidation / property sets run on                      */
/* the app (main) thread; the fire runs on the main queue. The         */
/* cross-thread fields are volatile; the async fire carries the        */
/* slot index plus a generation counter, so a fire posted before an    */
/* invalidate-and-reuse is dropped instead of firing the wrong         */
/* target. The registry never frees a slot while a fire may be in     */
/* flight: slots are only marked invalid, and reuse bumps the          */
/* generation.                                                          */
/*                                                                      */
/* No function-pointer tables in static data: the runtime functions    */
/* are resolved once at install time into individual statics, the      */
/* same pattern snapshot.c (g_syms) and carender.c use.                */
/* ------------------------------------------------------------------ */

/* dyld4::gAPIs in libdyld (__TPRO_CONST), pinned 23G90 cache.
 * Same addresses snapshot.c and carender.c use. */
#define CDL_GAPIS_ADDR         0x1ec954040ULL
#define CDL_GAPIS_OBJ_DISC     0x63faULL
#define CDL_SLOT_DLSYM         16ULL
#define CDL_DIV_DLSYM          0x839dULL

/* One display-link record. target is NOT retained: CADisplayLink
 * does not retain its target (documented), so a deallocated target
 * is the app's bug, not ours; the fire checks target != NULL only. */
typedef struct {
    void *link;
    void *target;
    void *selector;
    void *display;
    volatile int valid;
    volatile int invalidated;
    volatile int paused;
    volatile int added;
    int preferred_fps;        /* 0 = maximum rate */
    uint32_t divisor;         /* ticks per fire, from preferred_fps */
    uint64_t tick_count;
    uint64_t fire_count;
    volatile int64_t timestamp_ns;        /* mach clock, last fire */
    volatile int64_t target_timestamp_ns; /* mach clock, next deadline */
    int64_t duration_ns;      /* 1/60 s */
    float rate_min, rate_max, rate_pref;
    uint32_t generation;
} cdl_rec_t;

static cdl_rec_t g_cdl_links[CADISPLAYLINK_MAX_LINKS];
static uint32_t g_cdl_generation;
static uint64_t g_cdl_ticks;
static uint64_t g_cdl_fired;
static uint64_t g_cdl_dropped;
static uint32_t g_cdl_created;
static uint32_t g_cdl_faults_avoided;
static int g_cdl_hooks_installed;
static int64_t g_cdl_clock_offset_ns;
static int g_cdl_clock_offset_valid;
static uint64_t g_cdl_summary_at;
static void *g_cdl_link_class;

/* Runtime functions, resolved once at install time. */
static void *(*cdl_p_objc_getClass)(const char *);
static void *(*cdl_p_sel_registerName)(const char *);
static const char *(*cdl_p_sel_getName)(void *);
static void *(*cdl_p_class_getClassMethod)(void *, void *);
static void *(*cdl_p_class_getInstanceMethod)(void *, void *);
static void *(*cdl_p_method_setImplementation)(void *, void *);
static void *(*cdl_p_class_createInstance)(void *, size_t);
static void *(*cdl_p_dispatch_get_main_queue)(void);
static void (*cdl_p_dispatch_async_f)(void *, void *, void (*)(void *));
static void (*cdl_p_msgsend3)(void *, void *, void *);
static void *(*cdl_p_uiscreen_orig_disp_cfg)(void *, void *);
static void *g_cdl_uiscreen_fallback_cfg;
static void *g_cdl_main_queue;

#ifdef CADISPLAYLINK_HOST_TEST
static const cdl_ops_t *g_cdl_ops;
#endif

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static size_t cdl_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void cdl_log(const char *s) {
    size_t n;
    if (!s) return;
    n = cdl_strlen(s);
    if (n == 0) return;
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->log_fn) g_cdl_ops->log_fn(s, n);
#elif defined(__aarch64__)
    (void)sys_write(2, s, n);
#else
    /* Plain host build (cadisplay_test, cadisplay_stale_test): no
     * sys_write here; the link code is never exercised on host. */
#endif
}

static void cdl_log_u64(const char *prefix, uint64_t v) {
    char buf[128];
    char num[24];
    size_t pos = 0;
    size_t n = 0;
    uint64_t t = v;
    size_t i;
    if (v == 0) {
        num[n++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (t > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (rn > 0) num[n++] = rev[--rn];
    }
    while (prefix[pos] && pos < sizeof(buf) - 32) {
        buf[pos] = prefix[pos];
        pos++;
    }
    for (i = 0; i < n && pos < sizeof(buf) - 2; i++) buf[pos++] = num[i];
    buf[pos++] = '\n';
    buf[pos] = '\0';
    cdl_log(buf);
}

static void cdl_log_ptr(const char *prefix, const void *p) {
    char buf[128];
    char num[24];
    size_t pos = 0;
    size_t n = 0;
    uint64_t v = (uint64_t)(uintptr_t)p;
    size_t i;
    int started = 0;
    int shift;
    if (v == 0) {
        num[n++] = '0';
    } else {
        for (shift = 60; shift >= 0; shift -= 4) {
            int d = (int)((v >> shift) & 0xfULL);
            if (d == 0 && !started) continue;
            started = 1;
            num[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        }
    }
    while (prefix[pos] && pos < sizeof(buf) - 32) {
        buf[pos] = prefix[pos];
        pos++;
    }
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (i = 0; i < n && pos < sizeof(buf) - 2; i++) buf[pos++] = num[i];
    buf[pos++] = '\n';
    buf[pos] = '\0';
    cdl_log(buf);
}

/* ------------------------------------------------------------------ */
/* dlsym / PAC signing (snapshot.c / carender.c pattern)                */
/* ------------------------------------------------------------------ */

static void *cdl_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(CADISPLAYLINK_HOST_TEST)
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

#if defined(__aarch64__) && !defined(CADISPLAYLINK_HOST_TEST)
static __attribute__((noinline)) void *cdl_gapis_call2(uint64_t slot, uint64_t div,
                                                       uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)CDL_GAPIS_ADDR;
    uint64_t vt;
    uint64_t obj_div;
    uint64_t slot_addr;
    uint64_t fn;
    uint64_t fn_div;
    if (!obj) return NULL;
    vt = *(volatile uint64_t *)obj;
    obj_div = (obj & 0x0000ffffffffffffULL) | (CDL_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    slot_addr = vt + slot * 8ULL;
    fn = *(volatile uint64_t *)slot_addr;
    fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

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

static void *cdl_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->dlsym_fn) return g_cdl_ops->dlsym_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return cdl_gapis_call2(CDL_SLOT_DLSYM, CDL_DIV_DLSYM,
                           (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

static const char *cdl_getenv(const char *name) {
    if (!name) return NULL;
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->getenv_fn) return g_cdl_ops->getenv_fn(name);
    return NULL;
#elif defined(__aarch64__)
    return lsk_getenv(name);
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Clocks: FRAME_DONE ns (CLOCK_MONOTONIC) to mach_absolute_time ns     */
/* Both clocks count from boot; the offset between them is constant,    */
/* so it is measured once, lazily, on the first tick.                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} cdl_timespec_t;

typedef struct {
    uint32_t numer;
    uint32_t denom;
} cdl_timebase_t;

static int64_t cdl_mono_ns(void) {
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->clock_mono_ns_fn) return g_cdl_ops->clock_mono_ns_fn();
    return 0;
#elif defined(__aarch64__)
    cdl_timespec_t ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    if (lsk_clock_gettime(1 /* CLOCK_MONOTONIC */, &ts) != 0) return 0;
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
#else
    return 0;
#endif
}

static uint64_t cdl_mach_ticks(void) {
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->mach_ticks_fn) return g_cdl_ops->mach_ticks_fn();
    return 0;
#elif defined(__aarch64__)
    return lsk_mach_absolute_time();
#else
    return 0;
#endif
}

static void cdl_mach_timebase(uint32_t *numer, uint32_t *denom) {
#ifdef CADISPLAYLINK_HOST_TEST
    if (g_cdl_ops && g_cdl_ops->mach_timebase_fn) {
        g_cdl_ops->mach_timebase_fn(numer, denom);
        return;
    }
    if (numer) *numer = 0;
    if (denom) *denom = 0;
#elif defined(__aarch64__)
    cdl_timebase_t tb;
    tb.numer = 0;
    tb.denom = 0;
    if (lsk_mach_timebase_info(&tb) != 0) {
        if (numer) *numer = 0;
        if (denom) *denom = 0;
        return;
    }
    if (numer) *numer = tb.numer;
    if (denom) *denom = tb.denom;
#else
    if (numer) *numer = 0;
    if (denom) *denom = 0;
#endif
}

static int64_t cdl_clock_offset_ns(void) {
    uint32_t numer = 0, denom = 0;
    uint64_t ticks;
    int64_t mach_ns, mono_ns;
    if (g_cdl_clock_offset_valid) return g_cdl_clock_offset_ns;
    cdl_mach_timebase(&numer, &denom);
    if (numer == 0 || denom == 0) {
        g_cdl_clock_offset_ns = 0;
        g_cdl_clock_offset_valid = 1;
        return 0;
    }
    ticks = cdl_mach_ticks();
    mach_ns = (int64_t)((ticks * (uint64_t)numer) / denom);
    mono_ns = cdl_mono_ns();
    g_cdl_clock_offset_ns = mach_ns - mono_ns;
    g_cdl_clock_offset_valid = 1;
    return g_cdl_clock_offset_ns;
}

static uint32_t cdl_refresh_hz(void) {
    return g_inited ? g_refresh_hz : CADISPLAY_REFRESH_HZ;
}

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

static void cdl_reset_all(void) {
    uint32_t i;
    for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
        g_cdl_links[i].valid = 0;
        g_cdl_links[i].invalidated = 0;
        g_cdl_links[i].paused = 0;
        g_cdl_links[i].added = 0;
        g_cdl_links[i].link = NULL;
        g_cdl_links[i].target = NULL;
        g_cdl_links[i].selector = NULL;
        g_cdl_links[i].display = NULL;
    }
}

static int cdl_find_free(void) {
    uint32_t i;
    for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
        if (!g_cdl_links[i].valid) return (int)i;
    }
    return -1;
}

static int cdl_find_by_link(void *link) {
    uint32_t i;
    if (!link) return -1;
    for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
        if (g_cdl_links[i].valid && g_cdl_links[i].link == link) return (int)i;
    }
    return -1;
}

/* Rate divisor for a preferred rate: the actual rate is always a
 * factor of the 60 Hz maximum (60 -> 1, 30 -> 2, 20 -> 3, 15 -> 4,
 * 10 -> 6). fps <= 0 means maximum rate. Non-factor rates round
 * up to the next divisor (24 fps -> divisor 3, i.e. 20 Hz). */
static uint32_t cdl_divisor_for_fps(long fps) {
    uint32_t d;
    if (fps <= 0) return 1;
    if (fps >= 60) return 1;
    d = (uint32_t)((60 + fps - 1) / fps);
    if (d < 1) d = 1;
    if (d > 60) d = 60;
    return d;
}

static void *cdl_alloc_link(void) {
    if (!g_cdl_link_class || !cdl_p_class_createInstance) return NULL;
    return cdl_p_class_createInstance(g_cdl_link_class, 0);
}

static void *cdl_create_link(void *display, void *target, void *selector) {
    int idx;
    void *link;
    cdl_rec_t *r;
    if (!g_cdl_hooks_installed) return NULL;
    idx = cdl_find_free();
    if (idx < 0) {
        cdl_log("[cadisplay] link table full, returning nil\n");
        return NULL;
    }
    link = cdl_alloc_link();
    if (!link) {
        cdl_log("[cadisplay] link alloc failed, returning nil\n");
        return NULL;
    }
    r = &g_cdl_links[idx];
    memset(r, 0, sizeof(*r));
    r->link = link;
    r->target = target;
    r->selector = selector;
    r->display = display;
    r->valid = 1;
    r->preferred_fps = 0;
    r->divisor = 1;
    r->duration_ns = 1000000000LL / 60;
    r->rate_min = 30.0f;
    r->rate_max = 60.0f;
    r->rate_pref = 60.0f;
    r->generation = ++g_cdl_generation;
    g_cdl_created++;
    if (cdl_p_sel_getName && selector) {
        const char *sn = cdl_p_sel_getName(selector);
        if (sn) {
            cdl_log("[cadisplay] link selector: ");
            cdl_log(sn);
            cdl_log("\n");
        }
    }
    if (!display) {
        /* The provably-crashing case: a nil display is what the
         * real factory dereferences at +84. A non-nil display with
         * a NULL backing is the same fault but cannot be told apart
         * from a healthy display here, so this counter is a lower
         * bound on the faults avoided. */
        g_cdl_faults_avoided++;
        cdl_log_ptr("[cadisplay] link created, display=nil (fault avoided) target=", target);
    } else {
        cdl_log_ptr("[cadisplay] link created target=", target);
    }
    return link;
}

/* ------------------------------------------------------------------ */
/* Firing                                                               */
/* ------------------------------------------------------------------ */

static void cdl_fire_async(void *ctx) {
    uintptr_t v = (uintptr_t)ctx;
    int index = (int)(v & 0xffu);
    uint32_t gen = (uint32_t)(v >> 8);
    cdl_rec_t *r;
    if (index < 0 || index >= (int)CADISPLAYLINK_MAX_LINKS) {
        g_cdl_dropped++;
        return;
    }
    r = &g_cdl_links[index];
    if (!r->valid || r->generation != gen || r->invalidated) {
        /* Posted before an invalidate (or a slot reuse): drop,
         * never fire the wrong target. */
        g_cdl_dropped++;
        return;
    }
    if (!r->target || !r->selector || !cdl_p_msgsend3) {
        g_cdl_dropped++;
        return;
    }
    cdl_p_msgsend3(r->target, r->selector, r->link);
    r->fire_count++;
    g_cdl_fired++;
}

static void cdl_post_fire(cdl_rec_t *r, int index) {
    void *ctx;
    if (!cdl_p_dispatch_async_f || !g_cdl_main_queue) {
        g_cdl_dropped++;
        return;
    }
    ctx = (void *)(uintptr_t)(((uint64_t)r->generation << 8) | (uint64_t)index);
    cdl_p_dispatch_async_f(g_cdl_main_queue, ctx, cdl_fire_async);
}

void lsk_cadisplay_vsync_tick(uint64_t now_ns) {
    int64_t period_ns;
    int64_t ts_ns;
    uint32_t i;
    g_cdl_ticks++;
    period_ns = 1000000000LL / (int64_t)cdl_refresh_hz();
    ts_ns = (int64_t)now_ns + cdl_clock_offset_ns();
    /* This is the display-link
     * fire path, so the tick advances SwiftUI's host time. The
     * snapshot pump must never call ag_note_display_tick; only
     * this path does. ts_ns is on the mach_absolute_time scale
     * the guard documents. */
    ag_note_display_tick(ts_ns);
    for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
        cdl_rec_t *r = &g_cdl_links[i];
        if (!r->valid || r->invalidated || r->paused || !r->added) continue;
        r->tick_count++;
        if ((r->tick_count % r->divisor) != 0) continue;
        r->timestamp_ns = ts_ns;
        r->target_timestamp_ns = ts_ns + (int64_t)r->divisor * period_ns;
        cdl_post_fire(r, (int)i);
    }
    if (g_cdl_ticks - g_cdl_summary_at >= 600) {
        uint32_t live = 0;
        for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
            if (g_cdl_links[i].valid && !g_cdl_links[i].invalidated) live++;
        }
        g_cdl_summary_at = g_cdl_ticks;
        cdl_log_u64("[cadisplay] links live=", live);
        cdl_log_u64("[cadisplay] links fired=", g_cdl_fired);
        cdl_log_u64("[cadisplay] links dropped=", g_cdl_dropped);
    }
}

int lsk_cadisplay_has_display(void) {
    return g_inited ? 1 : 0;
}

int cadisplay_link_stats(cadisplay_link_stats_t *out) {
    uint32_t i;
    uint32_t live = 0;
    if (!out) return -1;
    for (i = 0; i < CADISPLAYLINK_MAX_LINKS; i++) {
        if (g_cdl_links[i].valid && !g_cdl_links[i].invalidated) live++;
    }
    out->links_created = g_cdl_created;
    out->links_live = live;
    out->ticks = g_cdl_ticks;
    out->fired = g_cdl_fired;
    out->dropped = g_cdl_dropped;
    out->faults_avoided = g_cdl_faults_avoided;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hook implementations (installed on CADisplayLink)                    */
/* ------------------------------------------------------------------ */

/* +displayLinkWithDisplay:target:selector: - the crashing entry.
 * The real implementation dereferences the display's NULL backing
 * at +84; this one never touches the display at all. */
static void *cdl_hook_factory_display(void *cls, void *sel, void *display,
                                      void *target, void *selector) {
    (void)cls;
    (void)sel;
    return cdl_create_link(display, target, selector);
}

/* +displayLinkWithTarget:selector: - the public entry. */
static void *cdl_hook_factory(void *cls, void *sel, void *target, void *selector) {
    (void)cls;
    (void)sel;
    return cdl_create_link(NULL, target, selector);
}

/* -init: never call the original (it may consult the display).
 * The object is already allocated; return it as-is. It has no
 * target, so it never fires; addToRunLoop on it is a no-op. */
static void *cdl_hook_init(void *self, void *sel) {
    (void)sel;
    return self;
}

static void cdl_hook_add_to_runloop(void *self, void *sel, void *runloop, void *mode) {
    int idx;
    (void)sel;
    (void)runloop;
    (void)mode;
    /* Modes are recorded nowhere: every link fires on the main
     * queue, which is the runloop SwiftUI and UIKit use. A link
     * added to a non-main runloop still fires on main; documented
     * in cadisplay-021.md. */
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    g_cdl_links[idx].added = 1;
}

static void cdl_hook_remove_from_runloop(void *self, void *sel, void *runloop, void *mode) {
    int idx;
    (void)sel;
    (void)runloop;
    (void)mode;
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    g_cdl_links[idx].added = 0;
}

static void cdl_hook_invalidate(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    /* Free the slot as well as marking it invalid: an invalidated
     * link must not pin its slot forever, or the 16-slot table
     * fills with dead links. A fire already posted for this slot
     * still drops safely: the async validation checks valid and
     * the generation, and reuse bumps the generation. */
    g_cdl_links[idx].invalidated = 1;
    g_cdl_links[idx].added = 0;
    g_cdl_links[idx].valid = 0;
    cdl_log("[cadisplay] link invalidated\n");
}

static double cdl_hook_timestamp(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return 0.0;
    return (double)g_cdl_links[idx].timestamp_ns * 1e-9;
}

static double cdl_hook_duration(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return 1.0 / 60.0;
    return (double)g_cdl_links[idx].duration_ns * 1e-9;
}

static double cdl_hook_target_timestamp(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return 0.0;
    return (double)g_cdl_links[idx].target_timestamp_ns * 1e-9;
}

static int cdl_hook_is_paused(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return 1;
    return g_cdl_links[idx].paused ? 1 : 0;
}

static void cdl_hook_set_paused(void *self, void *sel, int paused) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    g_cdl_links[idx].paused = paused ? 1 : 0;
}

static long cdl_hook_fps(void *self, void *sel) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return 0;
    return (long)g_cdl_links[idx].preferred_fps;
}

static void cdl_hook_set_fps(void *self, void *sel, long fps) {
    int idx;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    if (fps < 0) fps = 0;
    if (fps > 60) fps = 60;
    g_cdl_links[idx].preferred_fps = (int)fps;
    g_cdl_links[idx].divisor = cdl_divisor_for_fps(fps);
    g_cdl_links[idx].rate_pref = fps > 0 ? (float)fps : 60.0f;
}

static ca_frame_rate_range_t cdl_hook_range(void *self, void *sel) {
    ca_frame_rate_range_t r;
    int idx;
    (void)sel;
    r.minimum = 30.0f;
    r.maximum = 60.0f;
    r.preferred = 60.0f;
    idx = cdl_find_by_link(self);
    if (idx < 0) return r;
    r.minimum = g_cdl_links[idx].rate_min;
    r.maximum = g_cdl_links[idx].rate_max;
    r.preferred = g_cdl_links[idx].rate_pref;
    return r;
}

static void cdl_hook_set_range(void *self, void *sel, ca_frame_rate_range_t range) {
    int idx;
    float pref;
    (void)sel;
    idx = cdl_find_by_link(self);
    if (idx < 0) return;
    if (range.minimum < 1.0f) range.minimum = 1.0f;
    if (range.maximum > 60.0f) range.maximum = 60.0f;
    if (range.maximum < range.minimum) range.maximum = range.minimum;
    pref = range.preferred;
    if (pref < range.minimum) pref = range.minimum;
    if (pref > range.maximum) pref = range.maximum;
    g_cdl_links[idx].rate_min = range.minimum;
    g_cdl_links[idx].rate_max = range.maximum;
    g_cdl_links[idx].rate_pref = pref;
    g_cdl_links[idx].preferred_fps = (int)(pref + 0.5f);
    g_cdl_links[idx].divisor = cdl_divisor_for_fps((long)(pref + 0.5f));
}

/* Private accessors that read the (absent) _impl of our links. */
static double cdl_hook_max_refresh(void *self, void *sel) { (void)self; (void)sel; return 60.0; }
static void *cdl_hook_display(void *self, void *sel) { (void)self; (void)sel; return NULL; }
/* Generic stubs for the rest of the private surface (integer/bool/id/void
 * returns get 0, rates get 60 Hz, offsets 0 s). */
static long cdl_stub_zero(void *self, void *sel) { (void)self; (void)sel; return 0; }
static long cdl_stub_one(void *self, void *sel) { (void)self; (void)sel; return 1; }
static long cdl_stub_sixty(void *self, void *sel) { (void)self; (void)sel; return 60; }
static double cdl_stub_d60(void *self, void *sel) { (void)self; (void)sel; return 60.0; }
static double cdl_stub_d0(void *self, void *sel) { (void)self; (void)sel; return 0.0; }

static long cdl_hook_max_fps(void *self, void *sel) {
    (void)self;
    (void)sel;
    return 60;
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */

static void *cdl_sel(const char *name) {
    if (!cdl_p_sel_registerName) return NULL;
    return cdl_p_sel_registerName(name);
}

/* Install one method. Required methods must exist; optional ones
 * are skipped silently when absent. Returns 0 on success, -1 when
 * a required method is missing. */
static int cdl_install_one(void *cls, const char *sel_name, void *imp, int is_class, int required) {
    void *sel;
    void *m;
    if (!cdl_p_method_setImplementation) return required ? -1 : 0;
    sel = cdl_sel(sel_name);
    if (!sel) return required ? -1 : 0;
    if (is_class) {
        if (!cdl_p_class_getClassMethod) return required ? -1 : 0;
        m = cdl_p_class_getClassMethod(cls, sel);
    } else {
        if (!cdl_p_class_getInstanceMethod) return required ? -1 : 0;
        m = cdl_p_class_getInstanceMethod(cls, sel);
    }
    if (!m) {
        if (required) {
            cdl_log("[cadisplay] required CADisplayLink method missing: ");
            cdl_log(sel_name);
            cdl_log("\n");
            return -1;
        }
        return 0;
    }
    cdl_p_method_setImplementation(m, imp);
    return 0;
}

static double cdl_parse_float(const char **str) {
    const char *p = *str;
    double val = 0.0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0 + (double)(*p - '0');
        p++;
    }
    if (*p == '.') {
        double factor = 0.1;
        p++;
        while (*p >= '0' && *p <= '9') {
            val += (double)(*p - '0') * factor;
            factor *= 0.1;
            p++;
        }
    }
    *str = p;
    return val;
}

static void cdl_get_screen_geom(double *out_w_px, double *out_h_px, double *out_scale) {
    double w = 1440.0, h = 3200.0, s = 4.0;
    const char *env = cdl_getenv("IOSWINE_SCREEN");
    if (env && *env) {
        const char *p = env;
        double pw = cdl_parse_float(&p);
        if (*p == 'x' || *p == 'X') {
            double ph;
            double ps = 0.0;
            p++;
            ph = cdl_parse_float(&p);
            if (*p == '@') {
                p++;
                ps = cdl_parse_float(&p);
            }
            if (pw > 0.0 && ph > 0.0) { w = pw; h = ph; }
            if (ps > 0.0) s = ps;
        }
    }
    *out_w_px = w;
    *out_h_px = h;
    *out_scale = s;
}

static void *cdl_xpc_double_arr2(void *(*arr_create)(void *, size_t),
                                 void (*arr_set_double)(void *, size_t, double),
                                 double a, double b) {
    void *arr = arr_create(NULL, 0);
    if (!arr) return NULL;
    arr_set_double(arr, (size_t)-1, a);
    arr_set_double(arr, (size_t)-1, b);
    return arr;
}

static void *cdl_xpc_double_arr4(void *(*arr_create)(void *, size_t),
                                 void (*arr_set_double)(void *, size_t, double),
                                 double a, double b, double c, double d) {
    void *arr = arr_create(NULL, 0);
    if (!arr) return NULL;
    arr_set_double(arr, (size_t)-1, a);
    arr_set_double(arr, (size_t)-1, b);
    arr_set_double(arr, (size_t)-1, c);
    arr_set_double(arr, (size_t)-1, d);
    return arr;
}

/* Build a default FBSDisplayConfiguration matching bks/fakeboard geometry.
 * With no CARenderServer displays, +[UIScreen initialize] creates mainScreen
 * with displayConfiguration == nil; Solarium UIKitCore asserts non-nil. */
static void *cdl_build_default_display_config(void) {
    void *(*dict_create)(const char *const *, const void **, size_t) =
        (void *(*)(const char *const *, const void **, size_t))cdl_sign_fn(cdl_dlsym("xpc_dictionary_create"));
    void (*dict_set_val)(void *, const char *, void *) =
        (void (*)(void *, const char *, void *))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_value"));
    void (*dict_set_i64)(void *, const char *, int64_t) =
        (void (*)(void *, const char *, int64_t))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_int64"));
    void (*dict_set_u64)(void *, const char *, uint64_t) =
        (void (*)(void *, const char *, uint64_t))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_uint64"));
    void (*dict_set_bool)(void *, const char *, int) =
        (void (*)(void *, const char *, int))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_bool"));
    void (*dict_set_dbl)(void *, const char *, double) =
        (void (*)(void *, const char *, double))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_double"));
    void (*dict_set_str)(void *, const char *, const char *) =
        (void (*)(void *, const char *, const char *))cdl_sign_fn(cdl_dlsym("xpc_dictionary_set_string"));
    void *(*arr_create)(void *, size_t) =
        (void *(*)(void *, size_t))cdl_sign_fn(cdl_dlsym("xpc_array_create"));
    void (*arr_set_dbl)(void *, size_t, double) =
        (void (*)(void *, size_t, double))cdl_sign_fn(cdl_dlsym("xpc_array_set_double"));
    void (*xpc_rel)(void *) =
        (void (*)(void *))cdl_sign_fn(cdl_dlsym("xpc_release"));
    void *(*msg_id0)(void *, void *) = (void *(*)(void *, void *))cdl_sign_fn(cdl_dlsym("objc_msgSend"));
    void *(*msg_id1)(void *, void *, void *) = (void *(*)(void *, void *, void *))msg_id0;
    void *fbs_cls = cdl_p_objc_getClass ? cdl_p_objc_getClass("FBSDisplayConfiguration") : NULL;
    double w_px, h_px, scale, w_pt, h_pt;
    void *root, *ident, *mode, *bounds, *nbounds, *pxsz, *overscan, *obj, *cfg;

    if (!dict_create || !dict_set_val || !dict_set_i64 || !dict_set_u64 ||
        !dict_set_bool || !dict_set_dbl || !dict_set_str || !arr_create ||
        !arr_set_dbl || !msg_id0 || !fbs_cls) {
        cdl_log_ptr("[cadisplay] displayConfig missing sym fbs_cls=", fbs_cls);
        cdl_log_ptr("[cadisplay] displayConfig dict_create=", (void *)dict_create);
        return NULL;
    }

    cdl_get_screen_geom(&w_px, &h_px, &scale);
    w_pt = w_px / scale;
    h_pt = h_px / scale;

    ident = dict_create(NULL, NULL, 0);
    mode = dict_create(NULL, NULL, 0);
    root = dict_create(NULL, NULL, 0);
    if (!ident || !mode || !root) {
        if (xpc_rel) {
            if (ident) xpc_rel(ident);
            if (mode) xpc_rel(mode);
            if (root) xpc_rel(root);
        }
        return NULL;
    }

    dict_set_str(ident, "bsx_class", "FBSDisplayIdentity");
    dict_set_i64(ident, "type", 0);
    dict_set_i64(ident, "connectionType", 0);
    dict_set_bool(ident, "mainLike", 1);
    dict_set_u64(ident, "displayID", 1);
    dict_set_u64(ident, "connectionSeed", 1);
    dict_set_i64(ident, "pid", 0);
    dict_set_bool(ident, "external", 0);
    dict_set_str(ident, "uniqueID", "MainDisplay");
    dict_set_bool(ident, "secure", 1);

    dict_set_str(mode, "bsx_class", "FBSDisplayMode");
    dict_set_u64(mode, "width", (uint64_t)w_px);
    dict_set_u64(mode, "height", (uint64_t)h_px);
    dict_set_u64(mode, "preferredScale", (uint64_t)scale);
    dict_set_dbl(mode, "scaleOverride", 0.0);
    dict_set_dbl(mode, "refreshRate", 60.0);
    dict_set_i64(mode, "gamut", 0);
    dict_set_i64(mode, "hdr", 0);
    dict_set_i64(mode, "rotation", 0);
    dict_set_bool(mode, "virtual", 0);

    dict_set_str(root, "bsx_class", "FBSDisplayConfiguration");
    dict_set_val(root, "identity", ident);
    dict_set_val(root, "currentMode", mode);
    dict_set_val(root, "preferredMode", mode);

    bounds = cdl_xpc_double_arr4(arr_create, arr_set_dbl, 0.0, 0.0, w_pt, h_pt);
    nbounds = cdl_xpc_double_arr4(arr_create, arr_set_dbl, 0.0, 0.0, w_px, h_px);
    pxsz = cdl_xpc_double_arr2(arr_create, arr_set_dbl, w_px, h_px);
    overscan = cdl_xpc_double_arr2(arr_create, arr_set_dbl, 1.0, 1.0);
    if (bounds) dict_set_val(root, "bounds", bounds);
    if (nbounds) dict_set_val(root, "nativeBounds", nbounds);
    if (pxsz) dict_set_val(root, "pixelSize", pxsz);
    if (overscan) dict_set_val(root, "safeOverscanRatio", overscan);

    dict_set_dbl(root, "latency", 0.0);
    dict_set_str(root, "hardwareIdentifier", "MainDisplay");
    dict_set_str(root, "name", "LCD");
    dict_set_str(root, "deviceName", "LCD");
    dict_set_u64(root, "seed", 1);
    dict_set_bool(root, "notComparable", 0);
    dict_set_i64(root, "tags", 0);
    dict_set_bool(root, "cloningSupported", 0);
    dict_set_bool(root, "overscanned", 0);
    dict_set_i64(root, "overscanCompensation", 0);

    obj = msg_id0(fbs_cls, cdl_p_sel_registerName("alloc"));
    cfg = obj ? msg_id1(obj, cdl_p_sel_registerName("initWithXPCDictionary:"), root) : NULL;

    if (xpc_rel) {
        if (bounds) xpc_rel(bounds);
        if (nbounds) xpc_rel(nbounds);
        if (pxsz) xpc_rel(pxsz);
        if (overscan) xpc_rel(overscan);
        xpc_rel(ident);
        xpc_rel(mode);
        xpc_rel(root);
    }
    return cfg;
}

static void *cdl_hook_uiscreen_display_cfg(void *self, void *sel) {
    void *cfg = cdl_p_uiscreen_orig_disp_cfg ? cdl_p_uiscreen_orig_disp_cfg(self, sel) : NULL;
    if (cfg) return cfg;
    if (!g_cdl_uiscreen_fallback_cfg) {
        g_cdl_uiscreen_fallback_cfg = cdl_build_default_display_config();
        if (g_cdl_uiscreen_fallback_cfg) {
            cdl_log("[cadisplay] UIScreen displayConfiguration fallback built\n");
        } else {
            cdl_log("[cadisplay] UIScreen displayConfiguration fallback failed\n");
        }
    }
    return g_cdl_uiscreen_fallback_cfg;
}

static void cdl_install_uiscreen_hook(void) {
    void *scr_cls = cdl_p_objc_getClass ? cdl_p_objc_getClass("UIScreen") : NULL;
    void *sel = cdl_p_sel_registerName ? cdl_p_sel_registerName("displayConfiguration") : NULL;
    void *m = (scr_cls && sel && cdl_p_class_getInstanceMethod)
        ? cdl_p_class_getInstanceMethod(scr_cls, sel) : NULL;
    if (!m || !cdl_p_method_setImplementation) return;
    cdl_p_uiscreen_orig_disp_cfg =
        (void *(*)(void *, void *))cdl_sign_fn(cdl_p_method_setImplementation(m, (void *)cdl_hook_uiscreen_display_cfg));
    cdl_log("[cadisplay] UIScreen displayConfiguration hook installed\n");
}

int cadisplay_link_install_hooks(void) {
    const char *off;
    void *cls;
    cdl_log("[cadisplay] install hooks\n");
    if (g_cdl_hooks_installed) return 0;
    off = cdl_getenv("IOSWINE_NO_CADISPLAYLINK");
    if (off && off[0] == '1' && off[1] == '\0') {
        cdl_log("[cadisplay] display-link hooks disabled by IOSWINE_NO_CADISPLAYLINK=1\n");
        return -1;
    }
    /* Resolve everything first: the install is all-or-nothing. */
    cdl_p_objc_getClass = (void *(*)(const char *))cdl_sign_fn(cdl_dlsym("objc_getClass"));
    cdl_p_sel_registerName = (void *(*)(const char *))cdl_sign_fn(cdl_dlsym("sel_registerName"));
    cdl_p_sel_getName = (const char *(*)(void *))cdl_sign_fn(cdl_dlsym("sel_getName"));
    cdl_p_class_getClassMethod = (void *(*)(void *, void *))cdl_sign_fn(cdl_dlsym("class_getClassMethod"));
    cdl_p_class_getInstanceMethod = (void *(*)(void *, void *))cdl_sign_fn(cdl_dlsym("class_getInstanceMethod"));
    cdl_p_method_setImplementation = (void *(*)(void *, void *))cdl_sign_fn(cdl_dlsym("method_setImplementation"));
    cdl_p_class_createInstance = (void *(*)(void *, size_t))cdl_sign_fn(cdl_dlsym("class_createInstance"));
    cdl_p_dispatch_get_main_queue = (void *(*)(void))cdl_sign_fn(cdl_dlsym("dispatch_get_main_queue"));
    cdl_p_dispatch_async_f = (void (*)(void *, void *, void (*)(void *)))cdl_sign_fn(cdl_dlsym("dispatch_async_f"));
    cdl_p_msgsend3 = (void (*)(void *, void *, void *))cdl_sign_fn(cdl_dlsym("objc_msgSend"));
    /* dispatch_get_main_queue() is a header macro on Darwin, not an export:
     * fall back to the main queue object itself. */
    void *main_q_obj = cdl_p_dispatch_get_main_queue ? NULL : cdl_dlsym("_dispatch_main_q");
    if (!cdl_p_dispatch_get_main_queue && !main_q_obj) cdl_log("[cadisplay] no main queue symbol\n");
    if (!cdl_p_objc_getClass || !cdl_p_sel_registerName || !cdl_p_sel_getName ||
        !cdl_p_class_getClassMethod || !cdl_p_class_getInstanceMethod ||
        !cdl_p_method_setImplementation || !cdl_p_class_createInstance ||
        (!cdl_p_dispatch_get_main_queue && !main_q_obj) || !cdl_p_dispatch_async_f || !cdl_p_msgsend3) {
        cdl_log("[cadisplay] display-link hooks unavailable (missing runtime symbols)\n");
        return -1;
    }
    cls = cdl_p_objc_getClass("CADisplayLink");
    if (!cls) {
        cdl_log("[cadisplay] CADisplayLink class not found, hooks off\n");
        return -1;
    }
    g_cdl_link_class = cls;
    g_cdl_main_queue = cdl_p_dispatch_get_main_queue ? cdl_p_dispatch_get_main_queue() : main_q_obj;
    if (!g_cdl_main_queue) {
        cdl_log("[cadisplay] main queue unavailable, hooks off\n");
        return -1;
    }
    /* Factory methods (required): the crash lives here. */
    if (cdl_install_one(cls, "displayLinkWithDisplay:target:selector:",
                        (void *)cdl_hook_factory_display, 1, 1) != 0) return -1;
    if (cdl_install_one(cls, "displayLinkWithTarget:selector:",
                        (void *)cdl_hook_factory, 1, 1) != 0) return -1;
    /* Instance methods (required). */
    if (cdl_install_one(cls, "init", (void *)cdl_hook_init, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "addToRunLoop:forMode:", (void *)cdl_hook_add_to_runloop, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "removeFromRunLoop:forMode:", (void *)cdl_hook_remove_from_runloop, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "invalidate", (void *)cdl_hook_invalidate, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "timestamp", (void *)cdl_hook_timestamp, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "duration", (void *)cdl_hook_duration, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "targetTimestamp", (void *)cdl_hook_target_timestamp, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "isPaused", (void *)cdl_hook_is_paused, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "setPaused:", (void *)cdl_hook_set_paused, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "preferredFramesPerSecond", (void *)cdl_hook_fps, 0, 1) != 0) return -1;
    if (cdl_install_one(cls, "setPreferredFramesPerSecond:", (void *)cdl_hook_set_fps, 0, 1) != 0) return -1;
    cdl_install_one(cls, "maximumFramesPerSecond", (void *)cdl_hook_max_fps, 0, 0); /* absent on iOS 26 */
    cdl_install_one(cls, "maximumRefreshRate", (void *)cdl_hook_max_refresh, 0, 0);
    cdl_install_one(cls, "display", (void *)cdl_hook_display, 0, 0);
    {
        static const char *const zero[] = { "setUserInfo:", "userInfo", "setFrameInterval:",
            "highFrameRateReason", "setHighFrameRateReason:", "setHighFrameRateReasons:count:",
            "setPriority:", "priority", "setLocalEarlyWakeupOffset:", 0 };
        for (int i = 0; zero[i]; i++) cdl_install_one(cls, zero[i], (void *)cdl_stub_zero, 0, 0);
        cdl_install_one(cls, "active", (void *)cdl_stub_one, 0, 0);
        cdl_install_one(cls, "frameInterval", (void *)cdl_stub_one, 0, 0);
        cdl_install_one(cls, "minimumFrameDuration", (void *)cdl_stub_one, 0, 0);
        cdl_install_one(cls, "actualFramesPerSecond", (void *)cdl_stub_sixty, 0, 0);
        cdl_install_one(cls, "heartbeatRate", (void *)cdl_stub_d60, 0, 0);
        cdl_install_one(cls, "localEarlyWakeupOffset", (void *)cdl_stub_d0, 0, 0);
    }
    /* Optional: present on iOS 15+, absent on older runtimes. */
    cdl_install_one(cls, "preferredFrameRateRange", (void *)cdl_hook_range, 0, 0);
    cdl_install_one(cls, "setPreferredFrameRateRange:", (void *)cdl_hook_set_range, 0, 0);
    /* Defensive: if the runtime also exposes a plain `paused`
     * getter (the property declares getter=isPaused, so this is
     * normally absent), hook it the same way. */
    cdl_install_one(cls, "paused", (void *)cdl_hook_is_paused, 0, 0);
    cdl_install_uiscreen_hook();
    g_cdl_hooks_installed = 1;
    cdl_log_ptr("[cadisplay] display-link hooks installed, class=", cls);
    return 0;
}

#ifdef CADISPLAYLINK_HOST_TEST
void cadisplay_link_test_configure(const cdl_ops_t *ops) {
    g_cdl_ops = ops;
    cdl_reset_all();
    g_cdl_ticks = 0;
    g_cdl_fired = 0;
    g_cdl_dropped = 0;
    g_cdl_created = 0;
    g_cdl_faults_avoided = 0;
    g_cdl_generation = 0;
    g_cdl_hooks_installed = 0;
    g_cdl_clock_offset_ns = 0;
    g_cdl_clock_offset_valid = 0;
    g_cdl_summary_at = 0;
    g_cdl_link_class = NULL;
    g_cdl_main_queue = NULL;
    cdl_p_objc_getClass = NULL;
    cdl_p_sel_registerName = NULL;
    cdl_p_sel_getName = NULL;
    cdl_p_class_getClassMethod = NULL;
    cdl_p_class_getInstanceMethod = NULL;
    cdl_p_method_setImplementation = NULL;
    cdl_p_class_createInstance = NULL;
    cdl_p_dispatch_get_main_queue = NULL;
    cdl_p_dispatch_async_f = NULL;
    cdl_p_msgsend3 = NULL;
    cdl_p_uiscreen_orig_disp_cfg = NULL;
    g_cdl_uiscreen_fallback_cfg = NULL;
}

void cadisplay_link_test_reset(void) {
    cadisplay_link_test_configure(g_cdl_ops);
}

void *cadisplay_link_test_imp(const char *sel_name, int is_class) {
    void *sel;
    void *m;
    void *imp = NULL;
    if (!sel_name || !g_cdl_hooks_installed) return NULL;
    if (!cdl_p_sel_registerName) return NULL;
    sel = cdl_p_sel_registerName(sel_name);
    if (!sel || !g_cdl_link_class) return NULL;
    if (is_class) {
        if (!cdl_p_class_getClassMethod) return NULL;
        m = cdl_p_class_getClassMethod(g_cdl_link_class, sel);
    } else {
        if (!cdl_p_class_getInstanceMethod) return NULL;
        m = cdl_p_class_getInstanceMethod(g_cdl_link_class, sel);
    }
    if (!m) return NULL;
    /* The mock runtime exposes the current IMP through
     * method_getImplementation; on device this helper is test-only
     * and never called. Resolve it the same way as the rest. */
    {
        void *(*mgi)(void *) = (void *(*)(void *))cdl_sign_fn(cdl_dlsym("method_getImplementation"));
        if (!mgi) return NULL;
        imp = mgi(m);
    }
    return imp;
}
#endif
