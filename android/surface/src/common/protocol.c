/*
 * ioswine-surface protocol implementation.
 * C11, no platform calls at all: safe on host and on Android.
 */
#include "protocol.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                               */
/* ------------------------------------------------------------------ */

uint16_t isws_rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t isws_rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t isws_rd_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

int64_t isws_rd_i64le(const uint8_t *p) {
    uint64_t v = isws_rd_u64le(p);
    int64_t out;
    memcpy(&out, &v, sizeof(out));
    return out;
}

float isws_rd_f32le(const uint8_t *p) {
    uint32_t v = isws_rd_u32le(p);
    float out;
    memcpy(&out, &v, sizeof(out));
    return out;
}

void isws_wr_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

void isws_wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

void isws_wr_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

void isws_wr_i64le(uint8_t *p, int64_t v) {
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    isws_wr_u64le(p, u);
}

void isws_wr_f32le(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    isws_wr_u32le(p, u);
}

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

size_t isws_encode_header(uint8_t *out, uint16_t type, uint16_t flags,
                          uint32_t length) {
    isws_wr_u32le(out + 0, ISWS_MAGIC);
    isws_wr_u16le(out + 4, type);
    isws_wr_u16le(out + 6, flags);
    isws_wr_u32le(out + 8, length);
    return ISWS_HEADER_SIZE;
}

int isws_decode_header(const uint8_t *buf, size_t len,
                       uint16_t *type, uint16_t *flags, uint32_t *length) {
    if (len < ISWS_HEADER_SIZE) return -1;
    if (isws_rd_u32le(buf + 0) != ISWS_MAGIC) return -1;
    uint32_t l = isws_rd_u32le(buf + 8);
    if (l > ISWS_MAX_PAYLOAD) return -2;
    if (type) *type = isws_rd_u16le(buf + 4);
    if (flags) *flags = isws_rd_u16le(buf + 6);
    if (length) *length = l;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-message encode/decode                                           */
/* ------------------------------------------------------------------ */

size_t isws_encode_hello(uint8_t *out, size_t cap, const isws_hello_t *m) {
    if (cap < ISWS_HELLO_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->version);
    isws_wr_u32le(out + 4, m->width);
    isws_wr_u32le(out + 8, m->height);
    isws_wr_u32le(out + 12, m->stride);
    isws_wr_u32le(out + 16, m->format);
    isws_wr_u32le(out + 20, m->flags);
    return ISWS_HELLO_PAYLOAD_SIZE;
}

int isws_decode_hello(const uint8_t *buf, size_t len, isws_hello_t *m) {
    if (len != ISWS_HELLO_PAYLOAD_SIZE) return -1;
    m->version = isws_rd_u32le(buf + 0);
    m->width   = isws_rd_u32le(buf + 4);
    m->height  = isws_rd_u32le(buf + 8);
    m->stride  = isws_rd_u32le(buf + 12);
    m->format  = isws_rd_u32le(buf + 16);
    m->flags   = isws_rd_u32le(buf + 20);
    return 0;
}

size_t isws_encode_present(uint8_t *out, size_t cap, const isws_present_t *m) {
    if (m->damage_count > ISWS_MAX_DAMAGE) return 0;
    size_t need = 16u + 16u * m->damage_count;
    if (cap < need) return 0;
    isws_wr_u64le(out + 0, m->frame_id);
    isws_wr_u32le(out + 8, m->buffer_index);
    isws_wr_u32le(out + 12, m->damage_count);
    for (uint32_t i = 0; i < m->damage_count; i++) {
        uint8_t *r = out + 16 + 16 * i;
        isws_wr_u32le(r + 0, (uint32_t)m->damage[i].x);
        isws_wr_u32le(r + 4, (uint32_t)m->damage[i].y);
        isws_wr_u32le(r + 8, m->damage[i].w);
        isws_wr_u32le(r + 12, m->damage[i].h);
    }
    return need;
}

int isws_decode_present(const uint8_t *buf, size_t len, isws_present_t *m) {
    if (len < 16) return -1;
    m->frame_id = isws_rd_u64le(buf + 0);
    m->buffer_index = isws_rd_u32le(buf + 8);
    m->damage_count = isws_rd_u32le(buf + 12);
    if (m->buffer_index >= ISWS_NUM_BUFFERS) return -1;
    if (m->damage_count > ISWS_MAX_DAMAGE) return -1;
    if (len != 16u + 16u * m->damage_count) return -1;
    for (uint32_t i = 0; i < m->damage_count; i++) {
        const uint8_t *r = buf + 16 + 16 * i;
        m->damage[i].x = (int32_t)isws_rd_u32le(r + 0);
        m->damage[i].y = (int32_t)isws_rd_u32le(r + 4);
        m->damage[i].w = isws_rd_u32le(r + 8);
        m->damage[i].h = isws_rd_u32le(r + 12);
    }
    return 0;
}

size_t isws_encode_ready(uint8_t *out, size_t cap, const isws_ready_t *m) {
    if (cap < ISWS_READY_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->version);
    isws_wr_u32le(out + 4, m->flags);
    isws_wr_u32le(out + 8, m->max_in_flight);
    return ISWS_READY_PAYLOAD_SIZE;
}

int isws_decode_ready(const uint8_t *buf, size_t len, isws_ready_t *m) {
    if (len != ISWS_READY_PAYLOAD_SIZE) return -1;
    m->version = isws_rd_u32le(buf + 0);
    m->flags = isws_rd_u32le(buf + 4);
    m->max_in_flight = isws_rd_u32le(buf + 8);
    return 0;
}

size_t isws_encode_frame_done(uint8_t *out, size_t cap, const isws_frame_done_t *m) {
    if (cap < ISWS_FRAME_DONE_PAYLOAD_SIZE) return 0;
    isws_wr_u64le(out + 0, m->frame_id);
    isws_wr_i64le(out + 8, m->vsync_ns);
    isws_wr_u32le(out + 16, m->status);
    isws_wr_u32le(out + 20, m->reserved);
    return ISWS_FRAME_DONE_PAYLOAD_SIZE;
}

int isws_decode_frame_done(const uint8_t *buf, size_t len, isws_frame_done_t *m) {
    if (len != ISWS_FRAME_DONE_PAYLOAD_SIZE) return -1;
    m->frame_id = isws_rd_u64le(buf + 0);
    m->vsync_ns = isws_rd_i64le(buf + 8);
    m->status = isws_rd_u32le(buf + 16);
    m->reserved = isws_rd_u32le(buf + 20);
    if (m->status > 1) return -1;
    return 0;
}

size_t isws_encode_touch(uint8_t *out, size_t cap, const isws_touch_t *m) {
    if (cap < ISWS_TOUCH_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->action);
    isws_wr_u32le(out + 4, m->pointer_id);
    isws_wr_u32le(out + 8, m->pointer_count);
    isws_wr_f32le(out + 12, m->x);
    isws_wr_f32le(out + 16, m->y);
    isws_wr_i64le(out + 20, m->timestamp_ns);
    return ISWS_TOUCH_PAYLOAD_SIZE;
}

int isws_decode_touch(const uint8_t *buf, size_t len, isws_touch_t *m) {
    if (len != ISWS_TOUCH_PAYLOAD_SIZE) return -1;
    m->action = isws_rd_u32le(buf + 0);
    m->pointer_id = isws_rd_u32le(buf + 4);
    m->pointer_count = isws_rd_u32le(buf + 8);
    m->x = isws_rd_f32le(buf + 12);
    m->y = isws_rd_f32le(buf + 16);
    m->timestamp_ns = isws_rd_i64le(buf + 20);
    if (m->action > ISWS_TOUCH_CANCEL) return -1;
    if (m->pointer_id >= ISWS_MAX_POINTERS) return -1;
    if (m->pointer_count == 0 || m->pointer_count > ISWS_MAX_POINTERS) return -1;
    return 0;
}

size_t isws_encode_key(uint8_t *out, size_t cap, const isws_key_t *m) {
    if (cap < ISWS_KEY_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->keycode);
    isws_wr_u32le(out + 4, m->action);
    isws_wr_i64le(out + 8, m->timestamp_ns);
    return ISWS_KEY_PAYLOAD_SIZE;
}

int isws_decode_key(const uint8_t *buf, size_t len, isws_key_t *m) {
    if (len != ISWS_KEY_PAYLOAD_SIZE) return -1;
    m->keycode = isws_rd_u32le(buf + 0);
    m->action = isws_rd_u32le(buf + 4);
    m->timestamp_ns = isws_rd_i64le(buf + 8);
    if (m->action > ISWS_KEY_ACTION_UP) return -1;
    return 0;
}

size_t isws_encode_text(uint8_t *out, size_t cap, const isws_text_t *m) {
    size_t total;
    size_t i;
    if (!out || !m) return 0;
    if (m->kind > ISWS_TEXT_DELETE) return 0;
    if (m->kind == ISWS_TEXT_UNMARK || m->kind == ISWS_TEXT_DELETE) {
        if (m->utf8_len != 0) return 0;
    } else {
        if (m->utf8_len == 0 || !m->utf8) return 0;
        if (m->count != (uint32_t)m->utf8_len) return 0;
    }
    total = ISWS_TEXT_PAYLOAD_MIN + m->utf8_len;
    if (cap < total || total > ISWS_MAX_PAYLOAD) return 0;
    isws_wr_u32le(out + 0, m->kind);
    isws_wr_u32le(out + 4, m->count);
    for (i = 0; i < m->utf8_len; i++) out[8 + i] = m->utf8[i];
    return total;
}

int isws_decode_text(const uint8_t *buf, size_t len, isws_text_t *m) {
    if (!buf || !m) return -1;
    if (len < ISWS_TEXT_PAYLOAD_MIN) return -1;
    m->kind = isws_rd_u32le(buf + 0);
    m->count = isws_rd_u32le(buf + 4);
    m->utf8_len = len - ISWS_TEXT_PAYLOAD_MIN;
    if (m->kind > ISWS_TEXT_DELETE) return -1;
    if (m->kind == ISWS_TEXT_UNMARK || m->kind == ISWS_TEXT_DELETE) {
        if (m->utf8_len != 0) return -1;
        m->utf8 = NULL;
        return 0;
    }
    if (m->utf8_len == 0) return -1;
    if (m->count != (uint32_t)m->utf8_len) return -1;
    m->utf8 = buf + ISWS_TEXT_PAYLOAD_MIN;
    return 0;
}

size_t isws_encode_keyboard_req(uint8_t *out, size_t cap, const isws_keyboard_req_t *m) {
    if (!out || !m) return 0;
    if (cap < ISWS_KEYBOARD_PAYLOAD_SIZE) return 0;
    if (m->show > 1) return 0;
    isws_wr_u32le(out + 0, m->show);
    isws_wr_u32le(out + 4, m->traits);
    isws_wr_f32le(out + 8, m->frame_h_pts);
    isws_wr_u32le(out + 12, m->reserved);
    return ISWS_KEYBOARD_PAYLOAD_SIZE;
}

int isws_decode_keyboard_req(const uint8_t *buf, size_t len, isws_keyboard_req_t *m) {
    if (!buf || !m) return -1;
    if (len != ISWS_KEYBOARD_PAYLOAD_SIZE) return -1;
    m->show = isws_rd_u32le(buf + 0);
    m->traits = isws_rd_u32le(buf + 4);
    m->frame_h_pts = isws_rd_f32le(buf + 8);
    m->reserved = isws_rd_u32le(buf + 12);
    if (m->show > 1) return -1;
    return 0;
}

size_t isws_encode_info(uint8_t *out, size_t cap, const isws_info_t *m) {
    if (cap < ISWS_INFO_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->screen_w);
    isws_wr_u32le(out + 4, m->screen_h);
    isws_wr_u32le(out + 8, m->client_w);
    isws_wr_u32le(out + 12, m->client_h);
    isws_wr_f32le(out + 16, m->scale);
    isws_wr_f32le(out + 20, m->offset_x);
    isws_wr_f32le(out + 24, m->offset_y);
    isws_wr_u32le(out + 28, m->rotation);
    return ISWS_INFO_PAYLOAD_SIZE;
}

int isws_decode_info(const uint8_t *buf, size_t len, isws_info_t *m) {
    if (len != ISWS_INFO_PAYLOAD_SIZE) return -1;
    m->screen_w = isws_rd_u32le(buf + 0);
    m->screen_h = isws_rd_u32le(buf + 4);
    m->client_w = isws_rd_u32le(buf + 8);
    m->client_h = isws_rd_u32le(buf + 12);
    m->scale = isws_rd_f32le(buf + 16);
    m->offset_x = isws_rd_f32le(buf + 20);
    m->offset_y = isws_rd_f32le(buf + 24);
    m->rotation = isws_rd_u32le(buf + 28);
    if (m->screen_w == 0 || m->screen_h == 0 ||
        m->client_w == 0 || m->client_h == 0) return -1;
    if (!(m->scale > 0.0f)) return -1;
    if (m->rotation != 0 && m->rotation != 90 &&
        m->rotation != 180 && m->rotation != 270) return -1;
    return 0;
}

size_t isws_encode_focus(uint8_t *out, size_t cap, const isws_focus_t *m) {
    if (cap < ISWS_FOCUS_PAYLOAD_SIZE) return 0;
    isws_wr_u32le(out + 0, m->focused);
    isws_wr_u32le(out + 4, m->reserved);
    return ISWS_FOCUS_PAYLOAD_SIZE;
}

int isws_decode_focus(const uint8_t *buf, size_t len, isws_focus_t *m) {
    if (len != ISWS_FOCUS_PAYLOAD_SIZE) return -1;
    m->focused = isws_rd_u32le(buf + 0);
    m->reserved = isws_rd_u32le(buf + 4);
    if (m->focused > 1) return -1;
    return 0;
}

size_t isws_encode_error(uint8_t *out, size_t cap, uint32_t code,
                         const char *msg, size_t msg_len) {
    if (cap < 4) return 0;
    if (msg_len > cap - 4) msg_len = cap - 4;
    isws_wr_u32le(out + 0, code);
    if (msg_len) memcpy(out + 4, msg, msg_len);
    return 4 + msg_len;
}

int isws_decode_error(const uint8_t *buf, size_t len, uint32_t *code,
                      const uint8_t **msg, size_t *msg_len) {
    if (len < 4) return -1;
    if (code) *code = isws_rd_u32le(buf + 0);
    if (msg) *msg = buf + 4;
    if (msg_len) *msg_len = len - 4;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

uint32_t isws_validate_hello(const isws_hello_t *m) {
    if (m->version != ISWS_VERSION) return ISWS_ERR_VERSION;
    if (m->width == 0 || m->width > ISWS_MAX_WIDTH) return ISWS_ERR_BAD_HELLO;
    if (m->height == 0 || m->height > ISWS_MAX_HEIGHT) return ISWS_ERR_BAD_HELLO;
    if (m->stride < m->width * 4u) return ISWS_ERR_BAD_HELLO;
    if (m->stride > ISWS_MAX_STRIDE) return ISWS_ERR_BAD_HELLO;
    if (m->stride % 4u != 0) return ISWS_ERR_BAD_HELLO;
    if (m->format != ISWS_FORMAT_BGRA8888_PREMUL) return ISWS_ERR_BAD_HELLO;
    if (m->flags != 0) return ISWS_ERR_BAD_HELLO;
    return ISWS_ERR_NONE;
}

uint64_t isws_memfd_size(const isws_hello_t *m) {
    if (isws_validate_hello(m) != ISWS_ERR_NONE) return 0;
    uint64_t one = (uint64_t)m->stride * (uint64_t)m->height;
    uint64_t total = one * ISWS_NUM_BUFFERS;
    if (total / ISWS_NUM_BUFFERS != one) return 0; /* overflow guard */
    return total;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* Round a non-negative double to the nearest integer, halves away
 * from zero, without libm (protocol.c must stay libm-free: the host
 * tests link it without -lm). Values here are pixel counts, far
 * below the int64 range. Negative inputs clamp to 0. */
static int64_t isws_round_nonneg(double v) {
    if (!(v > 0.0)) return 0;
    return (int64_t)(v + 0.5);
}

int isws_compute_mapping(uint32_t screen_w, uint32_t screen_h,
                         uint32_t client_w, uint32_t client_h,
                         uint32_t rotation, isws_info_t *out) {
    double sx, sy, scale, dw, dh;
    if (screen_w == 0 || screen_h == 0 || client_w == 0 || client_h == 0)
        return -1;
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
        return -1;
    if (out == NULL) return -1;
    /* Double precision: the old float computation rounded
     * client_w * scale down by a fraction of a pixel, which the
     * presenter's (int32_t) truncation then turned into a missing
     * last column/row on screen. */
    sx = (double)screen_w / (double)client_w;
    sy = (double)screen_h / (double)client_h;
    scale = sx < sy ? sx : sy;
    if (!(scale > 0.0)) return -1;
    dw = (double)client_w * scale;
    dh = (double)client_h * scale;
    if (dw > (double)screen_w) dw = (double)screen_w;
    if (dh > (double)screen_h) dh = (double)screen_h;
    out->screen_w = screen_w;
    out->screen_h = screen_h;
    out->client_w = client_w;
    out->client_h = client_h;
    out->scale = (float)scale;
    out->offset_x = (float)(((double)screen_w - dw) * 0.5);
    out->offset_y = (float)(((double)screen_h - dh) * 0.5);
    out->rotation = rotation;
    return 0;
}

void isws_dest_rect(const isws_info_t *map, int32_t *x0, int32_t *y0,
                    int32_t *x1, int32_t *y1) {
    int64_t dw, dh, ox, oy, ex, ey;
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = 0;
    if (y1) *y1 = 0;
    if (map == NULL) return;    /* Destination size from the stored float scale, rounded the same
     * way compute_mapping rounds the double one. A 1 px disagreement
     * between the two roundings is absorbed by the clamp below, never
     * by a gap: the rect stays inside the screen. */
    dw = isws_round_nonneg((double)map->client_w * (double)map->scale);
    dh = isws_round_nonneg((double)map->client_h * (double)map->scale);
    if (dw > (int64_t)map->screen_w) dw = (int64_t)map->screen_w;
    if (dh > (int64_t)map->screen_h) dh = (int64_t)map->screen_h;
    /* A destination of 0 px in either axis is useless (extreme
     * downscale: client far larger than the screen in one axis);
     * the frame is at least 1 px wide and tall so the blit loop
     * and the nearest-neighbour math always have a range. */
    if (dw <= 0 && map->screen_w > 0) dw = 1;
    if (dh <= 0 && map->screen_h > 0) dh = 1;
    ox = isws_round_nonneg((double)map->offset_x);
    oy = isws_round_nonneg((double)map->offset_y);
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    ex = ox + dw;
    ey = oy + dh;
    if (ex > (int64_t)map->screen_w) ex = (int64_t)map->screen_w;
    if (ey > (int64_t)map->screen_h) ey = (int64_t)map->screen_h;
    if (x0) *x0 = (int32_t)ox;
    if (y0) *y0 = (int32_t)oy;
    if (x1) *x1 = (int32_t)ex;
    if (y1) *y1 = (int32_t)ey;
}

int isws_effective_mapping(const isws_info_t *map, uint32_t buf_w,
                           uint32_t buf_h, isws_info_t *out) {
    isws_info_t fresh;
    if (map == NULL || out == NULL) return -1;
    if (buf_w == 0 || buf_h == 0) return -1;
    if (map->screen_w == buf_w && map->screen_h == buf_h) {
        *out = *map;
        return 0;
    }
    if (isws_compute_mapping(buf_w, buf_h, map->client_w, map->client_h,
                             map->rotation, &fresh) != 0)
        return -1;
    *out = fresh;
    return 0;
}

int32_t isws_nn_index(int32_t dst, int32_t dst0, int32_t dst_len,
                      int32_t src0, int32_t src_len) {
    int64_t off;
    if (dst_len <= 0 || src_len <= 0) return src0;
    off = (int64_t)(dst - dst0) * (int64_t)src_len / (int64_t)dst_len;
    if (off < 0) off = 0;
    if (off > (int64_t)src_len - 1) off = (int64_t)src_len - 1;
    return src0 + (int32_t)off;
}

uint32_t isws_bgra_to_rgba_opaque(uint32_t bgra) {
    uint32_t b = bgra & 0xffu;
    uint32_t g = (bgra >> 8) & 0xffu;
    uint32_t r = (bgra >> 16) & 0xffu;
    uint32_t a = (bgra >> 24) & 0xffu;
    uint32_t inv = 255u - a;
    /* Premultiplied source composited over opaque white: each stored
     * channel is already multiplied by alpha, so the composited
     * channel is channel + (255 - alpha). The sum cannot exceed 255
     * for a premultiplied input (channel <= alpha); the clamp is
     * defensive only, for a non-premultiplied pixel. */
    r += inv;
    g += inv;
    b += inv;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return r | (g << 8) | (b << 16) | (255u << 24);
}

/* Screen (window) px -> client px, through the integer destination
 * rect: the rect is the one isws_dest_rect returns
 * for this mapping, the same rect blit_rect copies the frame into
 * and fill_bars clears around, so a tap on a displayed frame pixel
 * maps to that client pixel and a tap on a bar pixel maps outside,
 * exactly, with no 1 px disagreement at the edge. The old version
 * divided by the float scale and subtracted the float offset, so
 * for a mapping whose offset is fractional (client * scale not a
 * whole pixel) the two disagreed by up to one screen pixel: a tap
 * just outside the dest rect mapped inside (phantom app-edge tap)
 * and a tap on the last frame pixel column mapped outside (dropped
 * DOWN). Returns 0 if inside the client frame, -1 if the point
 * falls in a letterbox bar (out still clamped into the client
 * frame). A degenerate map (empty dest rect or zero client size)
 * returns -1 with the outputs clamped to 0. */
int isws_screen_to_client(const isws_info_t *map, float sx, float sy,
                          float *cx, float *cy) {
    int32_t x0, y0, x1, y1;
    int32_t dw, dh;
    float x, y;
    int inside;
    if (!map || !cx || !cy) return -1;
    isws_dest_rect(map, &x0, &y0, &x1, &y1);
    dw = x1 - x0;
    dh = y1 - y0;
    if (dw <= 0 || dh <= 0 || map->client_w == 0 || map->client_h == 0) {
        *cx = 0.0f;
        *cy = 0.0f;
        return -1;
    }
    inside = (sx >= (float)x0 && sx < (float)x1 &&
              sy >= (float)y0 && sy < (float)y1);
    x = (sx - (float)x0) * (float)map->client_w / (float)dw;
    y = (sy - (float)y0) * (float)map->client_h / (float)dh;
    if (x < 0.0f) x = 0.0f;
    if (x > (float)map->client_w - 1.0f) x = (float)map->client_w - 1.0f;
    if (y < 0.0f) y = 0.0f;
    if (y > (float)map->client_h - 1.0f) y = (float)map->client_h - 1.0f;
    *cx = x;
    *cy = y;
    return inside ? 0 : -1;
}

/* Client px -> screen px, the exact inverse of
 * isws_screen_to_client through the same destination rect: a client
 * point maps to the screen pixel the blit paints it on. */
void isws_client_to_screen(const isws_info_t *map, float cx, float cy,
                           float *sx, float *sy) {
    int32_t x0, y0, x1, y1;
    if (!map || !sx || !sy) return;
    if (map->client_w == 0 || map->client_h == 0) {
        *sx = 0.0f;
        *sy = 0.0f;
        return;
    }
    isws_dest_rect(map, &x0, &y0, &x1, &y1);
    *sx = (float)x0 + cx * (float)(x1 - x0) / (float)map->client_w;
    *sy = (float)y0 + cy * (float)(y1 - y0) / (float)map->client_h;
}

int isws_clip_damage(const isws_rect_t *in, uint32_t w, uint32_t h,
                     isws_rect_t *out) {
    int64_t x0 = in->x;
    int64_t y0 = in->y;
    int64_t x1 = (int64_t)in->x + (int64_t)in->w;
    int64_t y1 = (int64_t)in->y + (int64_t)in->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)w) x1 = (int64_t)w;
    if (y1 > (int64_t)h) y1 = (int64_t)h;
    if (x1 <= x0 || y1 <= y0) return -1;
    out->x = (int32_t)x0;
    out->y = (int32_t)y0;
    out->w = (uint32_t)(x1 - x0);
    out->h = (uint32_t)(y1 - y0);
    return 0;
}

size_t isws_expand_damage(const isws_present_t *p, uint32_t w, uint32_t h,
                          isws_rect_t *out) {
    if (p->damage_count == 0) {
        out[0].x = 0;
        out[0].y = 0;
        out[0].w = w;
        out[0].h = h;
        return 1;
    }
    size_t n = 0;
    for (uint32_t i = 0; i < p->damage_count; i++) {
        if (isws_clip_damage(&p->damage[i], w, h, &out[n]) == 0) n++;
    }
    return n;
}
