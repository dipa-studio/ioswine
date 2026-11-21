/*
 * cadamage.h - Damage rectangle tracking for the carender engine.
 *
 * A damage rect is a pixel rectangle in framebuffer coordinates
 * (origin top-left, x right, y down, width/height in pixels).
 * The tracker unions added rects, clamps them to the frame bounds,
 * and hands back at most CADAMAGE_MAX_RECTS rects for one PRESENT.
 * When the rect list would overflow, or a full-frame invalidation
 * is recorded, the tracker reports "full frame" and the caller
 * presents with damage_count 0 (the protocol's whole-frame rule).
 *
 * Pure logic, no syscalls, no libc, no static state: the caller
 * owns the cadamage_t struct. Freestanding C11.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Must match the presenter protocol limit (ISWS_MAX_DAMAGE, 16).
 * The tracker never returns more rects than this. */
#define CADAMAGE_MAX_RECTS 16u

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} cadamage_rect_t;

typedef struct {
    uint32_t frame_w;
    uint32_t frame_h;
    cadamage_rect_t rects[CADAMAGE_MAX_RECTS];
    uint32_t count;
    int full_frame; /* 1 when the whole frame is dirty */
} cadamage_t;

/* Initialise the tracker for a frame of frame_w x frame_h pixels.
 * Returns 0 on success, -1 on a zero dimension. */
int cadamage_init(cadamage_t *d, uint32_t frame_w, uint32_t frame_h);

/* Reset the tracker to empty (no damage, not full frame). The frame
 * size is kept. */
void cadamage_reset(cadamage_t *d);

/* 1 if nothing is dirty, 0 otherwise. */
int cadamage_empty(const cadamage_t *d);

/* 1 if the whole frame is dirty, 0 otherwise. */
int cadamage_is_full(const cadamage_t *d);

/* Mark the whole frame dirty. */
void cadamage_add_full(cadamage_t *d);

/* Add one rect (pixel coordinates, top-left origin). Rects fully
 * outside the frame are ignored. Rects partly outside are clamped.
 * Zero-area rects are ignored. Overlapping or adjacent rects are
 * merged. If the list would exceed CADAMAGE_MAX_RECTS distinct
 * rects, the tracker degrades to full frame rather than dropping
 * damage (dropping damage would show stale pixels). */
void cadamage_add(cadamage_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Add one rect in UIKit point coordinates, converting with the
 * given content scale (points * scale = pixels, rounded outward
 * so the rect covers every touched pixel). */
void cadamage_add_points(cadamage_t *d, double x, double y, double w, double h,
                         double scale);

/* Copy the current rect list into out (capacity max_out). Returns
 * the number of rects copied, or -1 when the tracker is full frame
 * (the caller must present the whole frame). Returns 0 when empty. */
int cadamage_get(const cadamage_t *d, cadamage_rect_t *out, uint32_t max_out);

/* Merge all rects into one bounding rect. Returns 0 and fills *out
 * when there is any damage, -1 when empty. When the tracker is full
 * frame, *out is the whole frame. */
int cadamage_bounding(const cadamage_t *d, cadamage_rect_t *out);
