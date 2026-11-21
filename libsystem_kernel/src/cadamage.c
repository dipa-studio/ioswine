/*
 * cadamage.c - Damage rectangle tracking for the carender engine.
 *
 * Freestanding C11, no libc, no syscalls, no static state.
 * Merge rule: two rects merge when they overlap or touch on any
 * edge (touching rects merge because a single PRESENT rect covering
 * both costs the same as two adjacent ones and keeps the list short).
 * Merge repeats until no pair merges, so the list stays minimal.
 *
 * No em dashes are used in this file.
 */
#include "cadamage.h"

static int rects_touch(const cadamage_rect_t *a, const cadamage_rect_t *b) {
    uint32_t ax2 = a->x + a->w;
    uint32_t ay2 = a->y + a->h;
    uint32_t bx2 = b->x + b->w;
    uint32_t by2 = b->y + b->h;
    if (ax2 < b->x || bx2 < a->x) return 0;
    if (ay2 < b->y || by2 < a->y) return 0;
    return 1;
}

static void rect_union_into(cadamage_rect_t *dst, const cadamage_rect_t *src) {
    uint32_t x2 = dst->x + dst->w;
    uint32_t y2 = dst->y + dst->h;
    uint32_t sx2 = src->x + src->w;
    uint32_t sy2 = src->y + src->h;
    if (src->x < dst->x) dst->x = src->x;
    if (src->y < dst->y) dst->y = src->y;
    if (sx2 > x2) x2 = sx2;
    if (sy2 > y2) y2 = sy2;
    dst->w = x2 - dst->x;
    dst->h = y2 - dst->y;
}

static void merge_all(cadamage_t *d) {
    int changed = 1;
    while (changed) {
        uint32_t i;
        uint32_t j;
        changed = 0;
        for (i = 0; i < d->count && !changed; i++) {
            for (j = i + 1; j < d->count; j++) {
                if (rects_touch(&d->rects[i], &d->rects[j])) {
                    uint32_t k;
                    rect_union_into(&d->rects[i], &d->rects[j]);
                    for (k = j + 1; k < d->count; k++) {
                        d->rects[k - 1] = d->rects[k];
                    }
                    d->count--;
                    changed = 1;
                    break;
                }
            }
        }
    }
}

int cadamage_init(cadamage_t *d, uint32_t frame_w, uint32_t frame_h) {
    if (d == NULL || frame_w == 0 || frame_h == 0) return -1;
    d->frame_w = frame_w;
    d->frame_h = frame_h;
    d->count = 0;
    d->full_frame = 0;
    return 0;
}

void cadamage_reset(cadamage_t *d) {
    if (d == NULL) return;
    d->count = 0;
    d->full_frame = 0;
}

int cadamage_empty(const cadamage_t *d) {
    if (d == NULL) return 1;
    return (d->count == 0 && !d->full_frame) ? 1 : 0;
}

int cadamage_is_full(const cadamage_t *d) {
    if (d == NULL) return 0;
    return d->full_frame ? 1 : 0;
}

void cadamage_add_full(cadamage_t *d) {
    if (d == NULL) return;
    d->count = 0;
    d->full_frame = 1;
}

void cadamage_add(cadamage_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t x2;
    uint32_t y2;
    cadamage_rect_t r;
    if (d == NULL || w == 0 || h == 0) return;
    if (d->full_frame) return;
    if (x >= d->frame_w || y >= d->frame_h) return;
    x2 = x + w;
    y2 = y + h;
    if (x2 > d->frame_w) x2 = d->frame_w;
    if (y2 > d->frame_h) y2 = d->frame_h;
    if (x2 <= x || y2 <= y) return;
    r.x = x;
    r.y = y;
    r.w = x2 - x;
    r.h = y2 - y;
    if (d->count < CADAMAGE_MAX_RECTS) {
        d->rects[d->count] = r;
        d->count++;
        merge_all(d);
    } else {
        /* No room for another distinct rect: degrade to full frame
         * rather than silently dropping damage. */
        cadamage_add_full(d);
    }
}

void cadamage_add_points(cadamage_t *d, double x, double y, double w, double h,
                         double scale) {
    uint32_t px;
    uint32_t py;
    uint32_t pw;
    uint32_t ph;
    double x2;
    double y2;
    if (d == NULL || scale <= 0.0 || w <= 0.0 || h <= 0.0) return;
    /* Round outward: floor the origin, ceil the far edge. */
    px = (uint32_t)(x * scale);
    if ((double)px > x * scale) px--;
    py = (uint32_t)(y * scale);
    if ((double)py > y * scale) py--;
    x2 = (x + w) * scale;
    y2 = (y + h) * scale;
    pw = (uint32_t)(x2 + 0.999999) - px;
    ph = (uint32_t)(y2 + 0.999999) - py;
    cadamage_add(d, px, py, pw, ph);
}

int cadamage_get(const cadamage_t *d, cadamage_rect_t *out, uint32_t max_out) {
    uint32_t i;
    uint32_t n;
    if (d == NULL) return -1;
    if (d->full_frame) return -1;
    if (out == NULL || max_out == 0) return 0;
    n = d->count < max_out ? d->count : max_out;
    for (i = 0; i < n; i++) out[i] = d->rects[i];
    return (int)n;
}

int cadamage_bounding(const cadamage_t *d, cadamage_rect_t *out) {
    uint32_t i;
    uint32_t x2;
    uint32_t y2;
    if (d == NULL || out == NULL) return -1;
    if (d->full_frame) {
        out->x = 0;
        out->y = 0;
        out->w = d->frame_w;
        out->h = d->frame_h;
        return 0;
    }
    if (d->count == 0) return -1;
    *out = d->rects[0];
    x2 = out->x + out->w;
    y2 = out->y + out->h;
    for (i = 1; i < d->count; i++) {
        uint32_t rx2 = d->rects[i].x + d->rects[i].w;
        uint32_t ry2 = d->rects[i].y + d->rects[i].h;
        if (d->rects[i].x < out->x) out->x = d->rects[i].x;
        if (d->rects[i].y < out->y) out->y = d->rects[i].y;
        if (rx2 > x2) x2 = rx2;
        if (ry2 > y2) y2 = ry2;
    }
    out->w = x2 - out->x;
    out->h = y2 - out->y;
    return 0;
}
