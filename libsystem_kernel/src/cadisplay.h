/*
 * cadisplay.h - CoreAnimation local display layer for ioswine.
 *
 * Bridges QuartzCore's local render path to the Android presenter
 * client (present.c) and the IOSurface emulation
 * (iosurface.c). CA renders a frame into an IOSurface;
 * cadisplay finds that surface in the IOSurface table and forwards
 * it to present.c, zero-copy when the surface memfd matches a
 * registered present buffer fd and the format/stride match,
 * otherwise by copying the pixels into a present buffer.
 *
 * Display timing comes from present.c FRAME_DONE (the vsync source
 * that feeds CADisplayLink): the launcher calls
 * present_wait_vsync() once per refresh and then
 * cadisplay_on_vsync() with the timestamp it returns.
 *
 * Freestanding: C11, no libc. Includes present.h and iosurface.h.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "present.h"
#include "iosurface.h"

/* Default display refresh rate, Hz. 60 [guess]. The display is
 * 1440x3200 @ scale 4, 60 Hz [guess]; geometry comes from
 * present_last_info() at runtime and is passed to cadisplay_init
 * explicitly. Pixel format BGRA8888 (0x41524742,
 * kCVPixelFormatType_32BGRA), bytesPerRow 5760 before the 64-byte
 * alignment iosurface.c applies (5760 is already 64-aligned). */
#define CADISPLAY_REFRESH_HZ 60U

/* Largest surface id the find_display_surface heuristic scans.
 * Surface ids are assigned monotonically from 1, so the scan stops
 * early once every live surface has been seen; the cap is only a
 * safety net against a pathological id space. */
#define CADISPLAY_MAX_SURFACE_SCAN 65536U

typedef struct {
    uint32_t width, height;      /* 1440, 3200 */
    uint32_t scale;              /* 4 */
    uint32_t refresh_hz;         /* 60 [guess] */
    int64_t  last_vsync_ns;      /* CLOCK_MONOTONIC ns of last FRAME_DONE */
    uint64_t last_frame_id;
    uint32_t current_surface_id; /* 0 = none */
    uint32_t frames_presented;
    uint32_t frames_zero_copy;
    uint32_t frames_copied;
} cadisplay_info_t;

/* Initialise the display layer with explicit geometry. Returns 0
 * on success, -1 on a bad argument (zero width, height or scale).
 * Re-initialising resets all state (counters, vsync clock, current
 * surface, registered fds, in-flight queue). Does not touch
 * present.c; the launcher owns the presenter lifecycle. */
int cadisplay_init(uint32_t width, uint32_t height, uint32_t scale);

/* Reset all state. Safe to call when not initialised. */
void cadisplay_shutdown(void);

/* 1 if the display is initialised and the presenter is connected,
 * 0 otherwise. */
int cadisplay_connected(void);

/* Called once per FRAME_DONE (from present_wait_vsync). Records the
 * vsync timestamp, advances the CADisplayLink clock, and frees the
 * oldest in-flight present buffer (each FRAME_DONE acks exactly
 * one PRESENT, including a dropped one, per present.c). Returns 0,
 * or -1 if the display is not initialised. */
int cadisplay_on_vsync(int64_t vsync_ns);

/* CLOCK_MONOTONIC ns of the next expected vsync (last + period),
 * or -1 if no vsync has arrived yet (or the display is not
 * initialised). Period = 1e9 / refresh_hz. */
int64_t cadisplay_next_vsync_ns(void);

/* CA finished rendering a frame into surface `surface_id`. Looks
 * the surface up in the IOSurface table, then forwards it to
 * present.c: zero-copy (present_present on the matching buffer
 * index) when the surface memfd matches a registered present
 * buffer fd AND the format/stride match; otherwise copies the
 * pixels into present_framebuffer(next) and presents that.
 * Returns 0 on success, -1 if the surface does not exist, is
 * locked (CA still holds it), has no backing, or present fails
 * (including both present buffers still being in flight). */
int cadisplay_on_frame(uint32_t surface_id);

/* Same as cadisplay_on_frame, but the PRESENT carries damage rects
 * (damage_count 0 means the whole frame; at most ISWS_MAX_DAMAGE
 * rects, in framebuffer pixel coordinates). The engine passes the damage
 * it tracked from -[CALayer setNeedsDisplayInRect:] when
 * IOSWINE_CARENDER_DAMAGE=1, and the whole frame otherwise. The presenter
 * uploads only the listed rects, so a wrong rect leaves stale pixels on
 * screen. Returns -1 on a
 * bad damage_count or a NULL damage pointer with a nonzero count. */
int cadisplay_on_frame_damage(uint32_t surface_id, const isws_rect_t *damage,
                              uint32_t damage_count);

/* Pure zero-copy eligibility check, directly testable. Returns 1
 * iff the surface exists, is not locked, pixel_format == BGRA,
 * and bytes_per_row == present_stride(). The fd match is checked
 * by the caller of cadisplay_on_frame against the registered
 * present fds. Note: for the real 1440x3200 display the stride
 * check passes (5760 == 5760, already 64-aligned); the fd match
 * is still the blocker, because the surface memfd is not the
 * present memfd. (For the old 1080x2400 geometry the stride check
 * failed: iosurface.c aligned bytes_per_row up to 4352 while
 * present_stride() was 4320.) */
int cadisplay_is_zero_copy(uint32_t surface_id);

/* Register the memfd backing present buffer `index` (0 or 1), so
 * cadisplay_on_frame can do the memfd-match check. The launcher
 * fills this after present_init; the host test fills it directly.
 * present.h does not expose the fd, so the deliverable ships an
 * OPTIONAL additive patch (present-additive.patch) adding a
 * present_buffer_fd() getter to present.{c,h}; without it, leave
 * the entries at -1 and cadisplay always takes the copy path.
 * Returns 0 on success, -1 on bad index. */
int cadisplay_set_present_fd(int index, int fd);

/* Which surface CA is currently rendering into. Set explicitly by
 * the CA hook (cadisplay_set_current_surface) or found by the
 * heuristic (cadisplay_find_display_surface). 0 = none. */
uint32_t cadisplay_current_surface(void);
void cadisplay_set_current_surface(uint32_t surface_id);

/* Heuristic fallback: scan the IOSurface table for the surface CA
 * is most likely rendering into. Among surfaces matching
 * width/height/BGRA, prefer the one whose seed most recently
 * incremented (highest seed; the seed increments on every unlock,
 * so it is the frame-finished signal), falling back to
 * most-recently-created (highest id) on a seed tie. Returns the
 * surface id, or 0 if none matches (or the display is not
 * initialised). */
uint32_t cadisplay_find_display_surface(void);

/* Surface fd / seed helpers for the future presenter protocol
 * extension: the launcher uses
 * these to hand the surface memfd to the presenter directly.
 * Returns 0 on success, -1 if the surface does not exist (or the
 * out pointer is NULL). */
int cadisplay_surface_fd(uint32_t surface_id, int *fd_out);
int cadisplay_surface_seed(uint32_t surface_id, uint32_t *seed_out);

/* Fill *out with the current display state. Returns 0, or -1 if
 * out is NULL or the display is not initialised. */
int cadisplay_info(cadisplay_info_t *out);

/* IOKit services QuartzCore queries for its display
 * (IOMobileFramebuffer, IODisplay, ...). Leaves the table empty
 * (count 0, returns NULL). */
typedef struct {
    const char *service_name; /* e.g. "IOMobileFramebuffer" */
    const char *class_name;   /* e.g. "IOMobileFramebuffer" */
    const char *properties;   /* "key=value;key=value" or NULL */
} cadisplay_iokit_service_t;
const cadisplay_iokit_service_t *cadisplay_iokit_services(uint32_t *count_out);

/* ------------------------------------------------------------------ */
/* CADisplayLink                                  */
/*                                                                      */
/* The entry points are the lsk_cadisplay_* functions below;            */
/* lsk_cadisplay_vsync_tick is called once per acted self-clocked engine */
/* tick (FRAME_DONE is only ever a PRESENT ack and never arrives        */
/* unsolicited; see carender.c).                                        */
/*                                                                      */
/* Crash (B) this fixes: after a tap on a SwiftUI cell, SwiftUI        */
/* creates a display link through                                       */
/* +[CADisplayLink(CADisplayLinkPrivate)                                */
/* displayLinkWithDisplay:target:selector:] and dies with SIGSEGV      */
/* at +84 (0x183f96b9c), x8=0. The display object exists (UIScreen     */
/* works through it) but its backing CA::Display* is NULL: there is    */
/* no render server, so CA::Display::ensure_displays never populated   */
/* it. The factory dereferences that NULL internal. The fix never      */
/* lets the real factory run: the two factory methods and every        */
/* instance method the app can call on the link are swizzled to a      */
/* registry in cadisplay.c. Links are plain CADisplayLink instances    */
/* (class_createInstance, no init call, so no QuartzCore code ever     */
/* touches the broken internal) whose callbacks fire from              */
/* lsk_cadisplay_vsync_tick on the main queue.                          */
/*                                                                      */
/* The link timestamp is in seconds on the mach_absolute_time clock    */
/* (the CACurrentMediaTime clock), derived from the FRAME_DONE          */
/* CLOCK_MONOTONIC ns plus a once-measured offset between the two      */
/* clocks (both count from boot; the offset is constant).               */
/* targetTimestamp - timestamp is the effective frame period            */
/* (divisor * 1/60 s). duration is always 1/60 s (the maximum rate).   */
/* ------------------------------------------------------------------ */

/* Max concurrent display links. SwiftUI typically holds a handful;
 * creation past this returns nil. */
#define CADISPLAYLINK_MAX_LINKS 16u

/* CAFrameRateRange (QuartzCore): minimum, maximum, preferred rates
 * in Hz. Default {30, 60, 60} [guess: 60 Hz panel, Apple default]. */
typedef struct {
    float minimum;
    float maximum;
    float preferred;
} ca_frame_rate_range_t;

/* Called once per acted engine tick by the self-clocked
 * engine, on EVERY tick including idle ones: a display link that
 * only fires when a frame was produced starves exactly when the
 * app is idle and waiting for an update. now_ns is CLOCK_MONOTONIC
 * ns of the engine tick (the FRAME_DONE ack timestamp when one
 * rode along, else the local timer boundary). Safe to call before
 * cadisplay_init and before any hook is installed (no-op when no
 * link is live). Fires each live, added, unpaused link whose
 * divisor divides its tick count, by dispatching the target /
 * selector invocation to the main queue. Never blocks. */
void lsk_cadisplay_vsync_tick(uint64_t now_ns);

/* 1 when the ioswine main display exists (cadisplay_init ran with
 * geometry), 0 otherwise. This reports OUR display state, not
 * QuartzCore's +[CADisplay mainDisplay]: that call is not probed
 * here because it can block in the CA::Display::query_displays
 * 5 s retry loop (MIG 40231) when the render-server thread is
 * absent, and the link machinery does not depend on it. */
int lsk_cadisplay_has_display(void);

/* Install the CADisplayLink hooks (factory methods + instance
 * methods). Called once from __libkernel_init_late. All-or-nothing:
 * if any runtime symbol is missing the install does nothing and
 * returns -1 (the app keeps the old behaviour, no partial hook).
 * Skipped entirely when IOSWINE_NO_CADISPLAYLINK=1 (kill switch).
 * Returns 0 on success, -1 when the class is absent, a symbol is
 * missing, or the install already ran. Safe to call twice (the
 * second call is a no-op returning 0). */
int cadisplay_link_install_hooks(void);

/* Display-link statistics for the device check. faults_avoided
 * counts factory calls that bypassed the real implementation with
 * a nil or absent display: the private entry with display == NULL,
 * and every public-entry call (it passes no display at all, and
 * the real public entry funnels into the same broken display).
 * On the crashing run this is the number of times crash (B) would
 * have fired; it is a lower bound, because a non-nil display with
 * a NULL backing faults the same way but cannot be told apart from
 * a healthy display here. Returns 0, or -1 if out is NULL. */
typedef struct {
    uint32_t links_created;
    uint32_t links_live;
    uint64_t ticks;
    uint64_t fired;
    uint64_t dropped;
    uint32_t faults_avoided;
} cadisplay_link_stats_t;
int cadisplay_link_stats(cadisplay_link_stats_t *out);

#ifdef CADISPLAYLINK_HOST_TEST
/* Host-test backend operations. The host test supplies fakes
 * through this struct (cadisplay_link_test_configure); on device
 * the link code resolves the same symbols with dlsym +
 * lsk_sign_fn, following snapshot.c and carender.c. dlsym_fn is
 * consulted for every ObjC/dispatch symbol by name
 * (objc_getClass, sel_registerName, sel_getName,
 * class_getClassMethod, class_getInstanceMethod,
 * method_setImplementation, class_createInstance, objc_msgSend,
 * dispatch_get_main_queue, dispatch_async_f). clock_mono_ns_fn
 * returns CLOCK_MONOTONIC ns, mach_ticks_fn returns
 * mach_absolute_time ticks, mach_timebase_fn fills numer/denom
 * (ticks * numer / denom = ns). getenv_fn answers the kill-switch
 * env var (NULL = unset). log_fn receives log bytes. */
typedef struct {
    void *(*dlsym_fn)(const char *name);
    int64_t (*clock_mono_ns_fn)(void);
    uint64_t (*mach_ticks_fn)(void);
    void (*mach_timebase_fn)(uint32_t *numer, uint32_t *denom);
    const char *(*getenv_fn)(const char *name);
    void (*log_fn)(const char *msg, size_t len);
} cdl_ops_t;

/* Host test only: install the ops table and reset all state. */
void cadisplay_link_test_configure(const cdl_ops_t *ops);
void cadisplay_link_test_reset(void);
/* Host test only: the currently installed IMP for a CADisplayLink
 * method (class method when is_class is nonzero), or NULL when the
 * class/method was never registered or the hooks are not installed.
 * Lets the test invoke the hook exactly as the runtime would. */
void *cadisplay_link_test_imp(const char *sel_name, int is_class);
#endif
