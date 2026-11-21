/*
 * present_api.c - exported ioswine_present_frame() helper for lsk.
 *
 * Freestanding C11, no libc (only <stdint.h> / <stddef.h> + present.h).
 * Lazily connects to the Android presenter (@ioswine-surface) on first
 * call, copies/clips BGRA8888 rows into the active memfd double buffer,
 * and submits PRESENT. Never crashes and never blocks > 100 ms.
 *
 * Pacing: the in-flight count is computed exactly from present.c's own
 * counters (next frame id minus last acked frame id), never from a
 * local counter. A local counter drifts the moment any other consumer
 * (cadisplay_on_frame) presents directly, and a drifted count either
 * waits forever or reuses a buffer the presenter has not acked yet.
 * FRAME_DONEs ack PRESENTs in order, one per PRESENT, including
 * dropped ones, so next - (last + 1) is the exact unacked count.
 *
 * Geometry: the session geometry is fixed by the first HELLO, and
 * later frames are clipped to it (smaller frames zero the outer
 * area, larger frames clip), exactly as the pinned present_test
 * expects. A resize is a deliberate act, not a side effect: call
 * ioswine_present_shutdown() first, then present at the new size
 * and a fresh session starts at that geometry. An implicit
 * re-init on every mismatched frame would break the clip contract
 * and would tear the session down under a caller that simply
 * presents a smaller overlay.
 */
#include "present.h"

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Bound READY wait and vsync wait so no call ever blocks > 100 ms. */
#define PRESENT_API_READY_TIMEOUT_MS 80
#define PRESENT_API_VSYNC_TIMEOUT_MS 16
/* Flush budget used by ioswine_present_shutdown (ms). */
#define PRESENT_API_SHUTDOWN_FLUSH_MS 100

static uint32_t g_api_w;
static uint32_t g_api_h;
static int g_api_buf;

static void api_reset(void) {
    g_api_w = 0;
    g_api_h = 0;
    g_api_buf = 0;
}

/* Exact number of PRESENTs sent and not yet acked by a FRAME_DONE.
 * Counts every consumer's PRESENTs, not just this file's, because
 * it reads present.c's counters. */
static uint64_t api_unacked(void) {
    uint64_t next = present_next_frame_id();
    uint64_t last = present_last_frame_id();
    if (last == (uint64_t)-1) return next;
    if (next <= last + 1) return 0;
    return next - (last + 1);
}

/* Clip damage rects to the copied region [0, copy_w) x [0, copy_h).
 * Rects wholly outside are dropped; a rect partly outside is
 * shrunk. Returns the kept count (0 means whole frame). */
static uint32_t api_clip_damage(const isws_rect_t *damage,
                                uint32_t damage_count, uint32_t copy_w,
                                uint32_t copy_h, isws_rect_t *out) {
    uint32_t kept = 0;
    uint32_t i;
    if (damage == NULL || damage_count == 0) return 0;
    for (i = 0; i < damage_count && kept < ISWS_MAX_DAMAGE; i++) {
        int64_t x0 = damage[i].x;
        int64_t y0 = damage[i].y;
        int64_t x1 = x0 + (int64_t)damage[i].w;
        int64_t y1 = y0 + (int64_t)damage[i].h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int64_t)copy_w) x1 = (int64_t)copy_w;
        if (y1 > (int64_t)copy_h) y1 = (int64_t)copy_h;
        if (x1 <= x0 || y1 <= y0) continue;
        out[kept].x = (int32_t)x0;
        out[kept].y = (int32_t)y0;
        out[kept].w = (uint32_t)(x1 - x0);
        out[kept].h = (uint32_t)(y1 - y0);
        kept++;
    }
    return kept;
}

int ioswine_present_frame_damage(const void *bgra, uint32_t w, uint32_t h,
                                 uint32_t stride,
                                 const isws_rect_t *damage,
                                 uint32_t damage_count) {
    uint8_t *dst;
    uint32_t dst_stride;
    uint32_t copy_w;
    uint32_t copy_h;
    uint32_t row_bytes;
    uint32_t y;
    isws_rect_t clipped[ISWS_MAX_DAMAGE];
    uint32_t kept_damage;

    if (bgra == NULL || w == 0 || h == 0) return -1;
    if (w > ISWS_MAX_WIDTH || h > ISWS_MAX_HEIGHT) return -1;
    if (stride < present_stride_for_width(w)) return -1;
    if (damage_count > 0 && damage == NULL) return -1;

    if (!present_connected()) {
        api_reset();
        if (present_init_timeout(w, h, PRESENT_API_READY_TIMEOUT_MS) != 0) {
            return -1;
        }
        g_api_w = w;
        g_api_h = h;
    } else if (g_api_w == 0 || g_api_h == 0) {
        g_api_w = present_stride() / 4u;
        g_api_h = present_height();
        if (g_api_w == 0 || g_api_h == 0) return -1;
    }
    /* A frame of a different size than the session clips to the
     * session geometry (see the header comment); it never
     * re-initialises the session implicitly. */

    if (present_pump() != 0) {
        api_reset();
        return -1;
    }

    if (api_unacked() >= ISWS_NUM_BUFFERS) {
        int64_t vsync_ns = 0;
        int wr = present_wait_vsync(&vsync_ns, PRESENT_API_VSYNC_TIMEOUT_MS);
        if (wr < 0) {
            api_reset();
            return -1;
        }
        /* wr == 1 (timeout): present on the other buffer anyway,
         * per the present.h pacing contract. The server copies
         * damage rects on PRESENT, so overwriting a buffer whose
         * FRAME_DONE is one period late is the sanctioned
         * fallback, not a silent corruption. */
    }

    dst = present_framebuffer(g_api_buf);
    dst_stride = present_stride();
    if (dst == NULL || dst_stride == 0) {
        api_reset();
        return -1;
    }

    copy_w = w < g_api_w ? w : g_api_w;
    copy_h = h < g_api_h ? h : g_api_h;
    row_bytes = copy_w * 4u;

    if (copy_w < g_api_w || copy_h < g_api_h) {
        memset(dst, 0, (size_t)dst_stride * g_api_h);
    }

    for (y = 0; y < copy_h; y++) {
        memcpy(dst + (size_t)y * dst_stride,
               (const uint8_t *)bgra + (size_t)y * stride,
               row_bytes);
    }

    kept_damage = api_clip_damage(damage, damage_count, copy_w, copy_h,
                                  clipped);

    if (present_present(g_api_buf, kept_damage > 0 ? clipped : NULL,
                        kept_damage) != 0) {
        api_reset();
        return -1;
    }

    g_api_buf ^= 1;
    return 0;
}

int ioswine_present_frame(const void *bgra, uint32_t w, uint32_t h,
                          uint32_t stride) {
    return ioswine_present_frame_damage(bgra, w, h, stride, NULL, 0);
}

int ioswine_present_flush(int timeout_ms) {
    int64_t deadline = 0;
    if (timeout_ms >= 0) {
        deadline = present_now_ns() + (int64_t)timeout_ms * 1000000LL;
    }
    for (;;) {
        if (!present_connected()) return -1;
        if (api_unacked() == 0) return 0;
        if (present_pump() != 0) return -1;
        if (api_unacked() == 0) return 0;
        {
            int64_t vsync_ns = 0;
            int remain_ms = -1;
            int wr;
            if (timeout_ms >= 0) {
                int64_t remain = deadline - present_now_ns();
                if (remain <= 0) return 1;
                remain_ms = (int)(remain / 1000000LL);
                if (remain_ms > 10) remain_ms = 10;
                if (remain_ms < 0) remain_ms = 0;
            }
            wr = present_wait_vsync(&vsync_ns,
                                    timeout_ms < 0 ? 10 : remain_ms);
            if (wr < 0) return -1;
            if (wr == 1 && timeout_ms >= 0 &&
                present_now_ns() >= deadline) {
                return 1;
            }
        }
    }
}

void ioswine_present_shutdown(void) {
    (void)ioswine_present_flush(PRESENT_API_SHUTDOWN_FLUSH_MS);
    present_shutdown();
    api_reset();
}
