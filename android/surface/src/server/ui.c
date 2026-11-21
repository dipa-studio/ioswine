/*
 * ui.c - Pure-C RGBA8888 launcher screen & running stats overlay renderer.
 */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "font8x16.h"

#include <stdio.h>
#include <string.h>

#define UI_RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define UI_RGB(r, g, b) UI_RGBA((r), (g), (b), 255u)

/* Palette: near-black background, soft cards, one accent. */
#define COL_BG          UI_RGB(11, 12, 16)
#define COL_HDR_BG      UI_RGB(11, 12, 16)
#define COL_CARD_BG     UI_RGB(24, 26, 33)
#define COL_CARD_BDR    UI_RGB(38, 41, 51)
#define COL_TEXT        UI_RGB(238, 240, 245)
#define COL_MUTED       UI_RGB(140, 146, 160)
#define COL_ACCENT      UI_RGB(124, 156, 255)
#define COL_OK          UI_RGB(74, 214, 140)
#define COL_WARN        UI_RGB(245, 190, 80)
#define COL_ERR         UI_RGB(255, 105, 105)
#define COL_BTN_LAUNCH  UI_RGB(92, 124, 250)
#define COL_BTN_STOP    UI_RGB(214, 64, 72)
#define COL_BTN_SEC     UI_RGB(32, 35, 44)
#define COL_BTN_BDR     UI_RGB(32, 35, 44)

static int32_t choose_scale(int32_t w, int32_t h) {
    int32_t m = (w < h) ? w : h;
    if (m >= 1200) return 3;
    if (m >= 700) return 2;
    return 1;
}

static uint32_t blend_px(uint32_t bg, uint32_t fg) {
    uint32_t a = (fg >> 24) & 0xffu;
    if (a == 255u) return fg;
    if (a == 0u) return bg;
    uint32_t inv = 255u - a;
    uint32_t r = ((fg & 0xffu) * a + (bg & 0xffu) * inv) / 255u;
    uint32_t g = (((fg >> 8) & 0xffu) * a + ((bg >> 8) & 0xffu) * inv) / 255u;
    uint32_t b = (((fg >> 16) & 0xffu) * a + ((bg >> 16) & 0xffu) * inv) / 255u;
    return UI_RGB(r, g, b);
}

static void fill_rect(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                      int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                      uint32_t color) {
    if (!dst || stride_px <= 0 || rw <= 0 || rh <= 0) return;
    int32_t x0 = rx < 0 ? 0 : rx;
    int32_t y0 = ry < 0 ? 0 : ry;
    int32_t x1 = (rx + rw > w) ? w : (rx + rw);
    int32_t y1 = (ry + rh > h) ? h : (ry + rh);
    if (x1 > stride_px) x1 = stride_px;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t a = (color >> 24) & 0xffu;
    if (a == 255u) {
        for (int32_t y = y0; y < y1; y++) {
            uint32_t *row = dst + (size_t)y * (size_t)stride_px;
            for (int32_t x = x0; x < x1; x++) row[x] = color;
        }
    } else if (a > 0u) {
        for (int32_t y = y0; y < y1; y++) {
            uint32_t *row = dst + (size_t)y * (size_t)stride_px;
            for (int32_t x = x0; x < x1; x++) row[x] = blend_px(row[x], color);
        }
    }
}

static void stroke_rect(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                        int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                        int32_t thick, uint32_t color) {
    if (thick <= 0) thick = 1;
    fill_rect(dst, w, h, stride_px, rx, ry, rw, thick, color);
    fill_rect(dst, w, h, stride_px, rx, ry + rh - thick, rw, thick, color);
    fill_rect(dst, w, h, stride_px, rx, ry, thick, rh, color);
    fill_rect(dst, w, h, stride_px, rx + rw - thick, ry, thick, rh, color);
}

/* Filled rectangle with rounded corners (radius in pixels). */
static void fill_round(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                       int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                       int32_t rad, uint32_t color) {
    if (rw <= 0 || rh <= 0) return;
    if (rad * 2 > rw) rad = rw / 2;
    if (rad * 2 > rh) rad = rh / 2;
    if (rad <= 0) { fill_rect(dst, w, h, stride_px, rx, ry, rw, rh, color); return; }
    fill_rect(dst, w, h, stride_px, rx, ry + rad, rw, rh - 2 * rad, color);
    for (int32_t i = 0; i < rad; i++) {
        /* inset for row i from the top edge of the corner circle */
        int32_t dy = rad - i;
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++;
        int32_t inset = rad - dx;
        fill_rect(dst, w, h, stride_px, rx + inset, ry + i, rw - 2 * inset, 1, color);
        fill_rect(dst, w, h, stride_px, rx + inset, ry + rh - 1 - i, rw - 2 * inset, 1, color);
    }
}

static void draw_glyph(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                       int32_t px, int32_t py, int32_t scale,
                       char ch, uint32_t color) {
    const uint8_t *g = font8x16_glyph(ch);
    for (int row = 0; row < FONT8X16_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        int32_t gy = py + row * scale;
        if (gy + scale <= 0 || gy >= h) continue;
        for (int col = 0; col < FONT8X16_W; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int32_t gx = px + col * scale;
            if (gx + scale <= 0 || gx >= w) continue;
            fill_rect(dst, w, h, stride_px, gx, gy, scale, scale, color);
        }
    }
}

static void draw_text_n(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                        int32_t x, int32_t y, int32_t scale,
                        const char *s, size_t max_chars, uint32_t color) {
    if (!s) return;
    int32_t cw = FONT8X16_W * scale;
    for (size_t i = 0; s[i] && i < max_chars; i++) {
        if (s[i] == '\n' || s[i] == '\r') break;
        draw_glyph(dst, w, h, stride_px, x + (int32_t)i * cw, y, scale, s[i], color);
    }
}

static void draw_text_centered(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                               ui_rect_t r, int32_t scale, const char *s, uint32_t color) {
    if (!s) return;
    size_t len = strlen(s);
    int32_t cw = FONT8X16_W * scale;
    int32_t ch = FONT8X16_H * scale;
    int32_t max_c = (r.w > cw) ? (r.w / cw) : 1;
    if ((int32_t)len > max_c) len = (size_t)max_c;
    int32_t tw = (int32_t)len * cw;
    int32_t tx = r.x + (r.w - tw) / 2;
    int32_t ty = r.y + (r.h - ch) / 2;
    draw_text_n(dst, w, h, stride_px, tx, ty, scale, s, len, color);
}

static void draw_button(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                        ui_rect_t r, int32_t scale, uint32_t bg, uint32_t bdr,
                        const char *label, uint32_t fg) {
    (void)bdr;
    fill_round(dst, w, h, stride_px, r.x, r.y, r.w, r.h, r.h / 2, bg);
    draw_text_centered(dst, w, h, stride_px, r, scale, label, fg);
}

static int32_t draw_multiline(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                              int32_t x, int32_t y, int32_t max_y, int32_t scale,
                              int32_t max_cols, int32_t line_h,
                              const char *text, uint32_t color) {
    if (!text || !*text || max_cols <= 0) return y;
    const char *p = text;
    while (*p && y + FONT8X16_H * scale <= max_y) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        while (seg > 0 && p[seg - 1] == '\r') seg--;
        if (seg == 0) {
            y += line_h / 2;
        } else {
            size_t off = 0;
            while (off < seg && y + FONT8X16_H * scale <= max_y) {
                size_t chunk = seg - off;
                if (chunk > (size_t)max_cols) chunk = (size_t)max_cols;
                draw_text_n(dst, w, h, stride_px, x, y, scale, p + off, chunk, color);
                y += line_h;
                off += chunk;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return y;
}

static void draw_log_view(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                          const ui_launcher_state_t *st, ui_layout_t *layout) {
    int32_t scale = choose_scale(w, h);
    int32_t cw = FONT8X16_W * scale;
    int32_t lh = FONT8X16_H * scale + 3 * scale;
    int32_t pad = 10 * scale;
    int32_t top_safe = 24 * scale;
    int32_t bot_safe = 16 * scale;

    fill_rect(dst, w, h, stride_px, 0, 0, w, h, COL_BG);

    int32_t hdr_h = lh + 10 * scale;
    fill_rect(dst, w, h, stride_px, 0, 0, w, top_safe + hdr_h, COL_HDR_BG);
    draw_text_n(dst, w, h, stride_px, pad, top_safe + 4 * scale, scale,
                "Last run log",
                (size_t)((w - 2 * pad) / cw), COL_ACCENT);

    int32_t btn_h = 30 * scale;
    int32_t btn_y = h - bot_safe - btn_h;
    int32_t gap = 8 * scale;
    int32_t half_w = (w - 2 * pad - gap) / 2;

    layout->btn_log_back = (ui_rect_t){ pad, btn_y, half_w, btn_h };
    layout->btn_refresh  = (ui_rect_t){ pad + half_w + gap, btn_y, half_w, btn_h };
    draw_button(dst, w, h, stride_px, layout->btn_log_back, scale,
                COL_BTN_SEC, COL_BTN_BDR, "Back", COL_TEXT);
    draw_button(dst, w, h, stride_px, layout->btn_refresh, scale,
                COL_BTN_SEC, COL_BTN_BDR, "Refresh", COL_TEXT);

    int32_t card_y = top_safe + hdr_h + 6 * scale;
    int32_t card_h = btn_y - 8 * scale - card_y;
    if (card_h > lh) {
        fill_rect(dst, w, h, stride_px, pad, card_y, w - 2 * pad, card_h, COL_CARD_BG);
        stroke_rect(dst, w, h, stride_px, pad, card_y, w - 2 * pad, card_h,
                    scale > 1 ? 2 : 1, COL_CARD_BDR);
        const char *txt = (st->sys && st->sys->log_tail[0])
                              ? st->sys->log_tail
                              : "No log yet. Launch the app first (needs root).";
        int32_t max_cols = (w - 2 * pad - 8 * scale) / cw;
        draw_multiline(dst, w, h, stride_px, pad + 4 * scale, card_y + 4 * scale,
                       card_y + card_h - 4 * scale, scale, max_cols, lh, txt, COL_TEXT);
    }
}

static void card(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                 int32_t x, int32_t y, int32_t cw_, int32_t ch_, int32_t scale) {
    fill_round(dst, w, h, stride_px, x, y, cw_, ch_, 10 * scale, COL_CARD_BG);
}

/* "label ........ value" row inside a card. */
static void kv_row(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                   int32_t x, int32_t y, int32_t inner, int32_t scale,
                   const char *k, const char *v, uint32_t vcol) {
    int32_t cw = FONT8X16_W * scale;
    int32_t cols = inner / cw;
    draw_text_n(dst, w, h, stride_px, x, y, scale, k, (size_t)cols, COL_MUTED);
    size_t vl = strlen(v);
    int32_t klen = (int32_t)strlen(k) + 1;
    if ((int32_t)vl > cols - klen) vl = (size_t)(cols - klen > 0 ? cols - klen : 0);
    draw_text_n(dst, w, h, stride_px, x + inner - (int32_t)vl * cw, y, scale, v, vl, vcol);
}

void ui_draw_launcher(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                      const ui_launcher_state_t *st, ui_layout_t *out_layout) {
    ui_layout_t local;
    memset(&local, 0, sizeof(local));
    if (!dst || w <= 0 || h <= 0 || stride_px <= 0 || !st) {
        if (out_layout) *out_layout = local;
        return;
    }

    if (st->view == UI_VIEW_LOG) {
        draw_log_view(dst, w, h, stride_px, st, &local);
        if (out_layout) *out_layout = local;
        return;
    }

    const isws_sysinfo_t *sys = st->sys;
    int32_t scale = choose_scale(w, h);
    int32_t big = scale * 2;
    int32_t cw = FONT8X16_W * scale;
    int32_t lh = FONT8X16_H * scale + 6 * scale;
    int32_t pad = 14 * scale;
    int32_t top_safe = 30 * scale;
    int32_t bot_safe = 18 * scale;
    int32_t inner_w = w - 2 * pad;
    int32_t cpad = 10 * scale;
    int32_t cinner = inner_w - 2 * cpad;
    char line[256];

    fill_rect(dst, w, h, stride_px, 0, 0, w, h, COL_BG);

    /* Title */
    int32_t y = top_safe;
    draw_text_n(dst, w, h, stride_px, pad, y, big, "ioswine",
                (size_t)(inner_w / (FONT8X16_W * big)), COL_TEXT);
    y += FONT8X16_H * big + 4 * scale;
    draw_text_n(dst, w, h, stride_px, pad, y, scale, "iOS apps on Android",
                (size_t)(inner_w / cw), COL_MUTED);
    y += lh + 12 * scale;

    /* Status hero card */
    bool running = sys && sys->loader_running;
    const char *state_txt;
    uint32_t state_col;
    if (st->stopping)       { state_txt = "Stopping";  state_col = COL_WARN; }
    else if (st->launching) { state_txt = "Starting";  state_col = COL_WARN; }
    else if (running)       { state_txt = "Running";   state_col = COL_OK; }
    else if (sys && sys->has_last_run && sys->last_crash[0])
                            { state_txt = "Crashed";   state_col = COL_ERR; }
    else                    { state_txt = "Ready";     state_col = COL_ACCENT; }

    int32_t hero_h = FONT8X16_H * big + lh * 2 + 2 * cpad;
    card(dst, w, h, stride_px, pad, y, inner_w, hero_h, scale);
    {
        int32_t ty = y + cpad;
        draw_text_n(dst, w, h, stride_px, pad + cpad, ty, scale, "Aidoku",
                    (size_t)(cinner / cw), COL_MUTED);
        ty += lh;
        int32_t dot = 5 * scale;
        fill_round(dst, w, h, stride_px, pad + cpad, ty + (FONT8X16_H * big - dot * 2) / 2,
                   dot * 2, dot * 2, dot, state_col);
        draw_text_n(dst, w, h, stride_px, pad + cpad + dot * 2 + 8 * scale, ty, big,
                    state_txt, 16, COL_TEXT);
        ty += FONT8X16_H * big + 6 * scale;
        if (running) {
            snprintf(line, sizeof(line), "pid %d", (int)sys->loader_pid);
        } else if (st->status_msg && st->status_msg[0]) {
            snprintf(line, sizeof(line), "%s", st->status_msg);
        } else if (sys && sys->probed && !sys->root_available) {
            snprintf(line, sizeof(line), "Needs root access");
        } else {
            snprintf(line, sizeof(line), "Tap Launch to open the app");
        }
        draw_text_n(dst, w, h, stride_px, pad + cpad, ty, scale, line,
                    (size_t)(cinner / cw), COL_MUTED);
    }
    y += hero_h + 10 * scale;

    /* Bottom buttons */
    int32_t sec_h = 34 * scale;
    int32_t pri_h = 44 * scale;
    int32_t gap = 10 * scale;
    int32_t sec_y = h - bot_safe - sec_h;
    int32_t pri_y = sec_y - gap - pri_h;
    int32_t half_w = (inner_w - gap) / 2;
    bool busy = st->launching || running;
    local.primary_is_stop = busy;
    local.btn_primary = (ui_rect_t){ pad, pri_y, inner_w, pri_h };
    local.btn_refresh = (ui_rect_t){ pad, sec_y, half_w, sec_h };
    local.btn_log     = (ui_rect_t){ pad + half_w + gap, sec_y, half_w, sec_h };
    draw_button(dst, w, h, stride_px, local.btn_primary, scale,
                busy ? COL_BTN_STOP : COL_BTN_LAUNCH, 0,
                busy ? (st->stopping ? "Stopping..." : "Stop") : "Launch", COL_TEXT);
    draw_button(dst, w, h, stride_px, local.btn_refresh, scale,
                COL_BTN_SEC, 0, "Refresh", COL_TEXT);
    draw_button(dst, w, h, stride_px, local.btn_log, scale,
                COL_BTN_SEC, 0, "Log", COL_TEXT);

    int32_t limit = pri_y - 10 * scale;

    /* Last run card */
    if (sys && (sys->has_last_run || st->have_session_stats) && !running) {
        int32_t ch_ = lh * 4 + 2 * cpad;
        if (y + ch_ <= limit) {
            card(dst, w, h, stride_px, pad, y, inner_w, ch_, scale);
            int32_t ty = y + cpad;
            draw_text_n(dst, w, h, stride_px, pad + cpad, ty, scale, "Last run",
                        (size_t)(cinner / cw), COL_TEXT);
            ty += lh;
            char dur[24], fr[24];
            int64_t dur_s = sys->last_duration_s >= 0 ? sys->last_duration_s
                          : (st->have_session_stats ? st->last_session_duration_s : -1);
            isws_fmt_duration(dur_s, dur, sizeof(dur));
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Ran for", dur, COL_TEXT);
            ty += lh;
            if (st->have_session_stats)
                snprintf(fr, sizeof(fr), "%llu", (unsigned long long)st->last_session_frames);
            else if (sys->last_frames >= 0)
                snprintf(fr, sizeof(fr), "%lld", (long long)sys->last_frames);
            else
                snprintf(fr, sizeof(fr), "-");
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Frames", fr, COL_TEXT);
            ty += lh;
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Crash",
                   sys->last_crash[0] ? sys->last_crash : "none",
                   sys->last_crash[0] ? COL_ERR : COL_OK);
            y += ch_ + 10 * scale;
        }
    }

    /* Device card */
    {
        int32_t ch_ = lh * 5 + 2 * cpad;
        if (y + ch_ <= limit) {
            card(dst, w, h, stride_px, pad, y, inner_w, ch_, scale);
            int32_t ty = y + cpad;
            draw_text_n(dst, w, h, stride_px, pad + cpad, ty, scale, "Device",
                        (size_t)(cinner / cw), COL_TEXT);
            ty += lh;
            bool has_root = sys && sys->root_available;
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Root",
                   (sys && sys->probed) ? (has_root ? "yes" : "no") : "...",
                   has_root ? COL_OK : COL_WARN);
            ty += lh;
            const char *se = (sys && sys->selinux[0]) ? sys->selinux : "unknown";
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "SELinux", se,
                   strcmp(se, "Enforcing") == 0 ? COL_OK : COL_WARN);
            ty += lh;
            char a1[24], a2[24];
            isws_fmt_kb(sys ? sys->data_avail_kb : -1, a1, sizeof(a1));
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Free space", a1, COL_TEXT);
            ty += lh;
            isws_fmt_kb(sys ? sys->du_total_kb : -1, a2, sizeof(a2));
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "ioswine uses", a2, COL_TEXT);
            y += ch_ + 10 * scale;
        }
    }

    /* Build card */
    {
        int32_t ch_ = lh * 3 + 2 * cpad;
        if (y + ch_ <= limit) {
            card(dst, w, h, stride_px, pad, y, inner_w, ch_, scale);
            int32_t ty = y + cpad;
            draw_text_n(dst, w, h, stride_px, pad + cpad, ty, scale, "Build",
                        (size_t)(cinner / cw), COL_TEXT);
            ty += lh;
            char ver[64] = "unknown";
            if (sys && sys->release_txt[0]) {
                const char *v = strstr(sys->release_txt, "version:");
                if (v) {
                    v += 8; while (*v == ' ') v++;
                    size_t n = strcspn(v, "\r\n");
                    if (n >= sizeof(ver)) n = sizeof(ver) - 1;
                    memcpy(ver, v, n); ver[n] = 0;
                }
            }
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Version", ver, COL_TEXT);
            ty += lh;
            bool ok = sys && sys->loader_info.exists && sys->dylib_info.exists;
            kv_row(dst, w, h, stride_px, pad + cpad, ty, cinner, scale, "Runtime",
                   ok ? "installed" : "missing", ok ? COL_OK : COL_WARN);
            (void)line;
        }
    }

    if (out_layout) *out_layout = local;
}

void ui_overlay_bounds(int32_t w, int32_t h, ui_rect_t *out_box) {
    if (!out_box) return;
    int32_t scale = choose_scale(w, h);
    int32_t cw = FONT8X16_W * scale;
    int32_t lh = FONT8X16_H * scale + 2 * scale;
    int32_t pad = 6 * scale;
    int32_t ox = 8 * scale;
    int32_t oy = 24 * scale; /* below rounded corner / status bar */
    int32_t bw = 27 * cw + 2 * pad;
    int32_t btn_h = 20 * scale;
    int32_t bh = 3 * lh + btn_h + 3 * pad;
    if (ox + bw > w) bw = w - ox;
    if (oy + bh > h) bh = h - oy;
    if (bw < 0) bw = 0;
    if (bh < 0) bh = 0;
    *out_box = (ui_rect_t){ ox, oy, bw, bh };
}

void ui_draw_overlay(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                     const ui_overlay_state_t *st, ui_layout_t *out_layout) {
    ui_layout_t local;
    memset(&local, 0, sizeof(local));
    if (!dst || w <= 0 || h <= 0 || stride_px <= 0 || !st) {
        if (out_layout) *out_layout = local;
        return;
    }

    int32_t scale = choose_scale(w, h);
    int32_t cw = FONT8X16_W * scale;
    int32_t lh = FONT8X16_H * scale + 2 * scale;
    int32_t pad = 6 * scale;
    ui_rect_t box;
    ui_overlay_bounds(w, h, &box);
    local.overlay_box = box;

    if (box.w < 40 || box.h < 40) {
        if (out_layout) *out_layout = local;
        return;
    }

    fill_rect(dst, w, h, stride_px, box.x, box.y, box.w, box.h,
              UI_RGBA(12, 16, 24, 200));
    stroke_rect(dst, w, h, stride_px, box.x, box.y, box.w, box.h,
                1, UI_RGB(96, 178, 255));

    int32_t tx = box.x + pad;
    int32_t ty = box.y + pad;
    int32_t max_cols = (box.w - 2 * pad) / cw;
    if (max_cols < 1) max_cols = 1;

    char line[64], up[24];
    isws_fmt_duration(st->uptime_s, up, sizeof(up));
    snprintf(line, sizeof(line), "FPS:%5.1f  DROP:%llu",
             st->fps, (unsigned long long)st->frames_dropped);
    draw_text_n(dst, w, h, stride_px, tx, ty, scale, line, (size_t)max_cols, COL_TEXT);
    ty += lh;

    snprintf(line, sizeof(line), "TOUCH:%-5llu UP:%s",
             (unsigned long long)st->touches_sent, up);
    draw_text_n(dst, w, h, stride_px, tx, ty, scale, line, (size_t)max_cols, COL_TEXT);
    ty += lh;

    snprintf(line, sizeof(line), "PID:%d  PRES:%llu",
             (int)st->client_pid, (unsigned long long)st->frames_presented);
    draw_text_n(dst, w, h, stride_px, tx, ty, scale, line, (size_t)max_cols, COL_MUTED);
    ty += lh + pad / 2;

    int32_t btn_h = 20 * scale;
    int32_t btn_gap = 6 * scale;
    int32_t btn_w = (box.w - 2 * pad - btn_gap) / 2;
    if (ty + btn_h <= box.y + box.h && btn_w > 0) {
        local.btn_overlay_stop = (ui_rect_t){ tx, ty, btn_w, btn_h };
        local.btn_overlay_hide = (ui_rect_t){ tx + btn_w + btn_gap, ty, btn_w, btn_h };
        draw_button(dst, w, h, stride_px, local.btn_overlay_stop, scale,
                    COL_BTN_STOP, UI_RGB(240, 120, 120), "Stop", COL_TEXT);
        draw_button(dst, w, h, stride_px, local.btn_overlay_hide, scale,
                    COL_BTN_SEC, COL_BTN_BDR, "Hide", COL_TEXT);
    }

    if (out_layout) *out_layout = local;
}

static bool pt_in_rect(ui_rect_t r, float x, float y) {
    return r.w > 0 && r.h > 0 &&
           x >= (float)r.x && x < (float)(r.x + r.w) &&
           y >= (float)r.y && y < (float)(r.y + r.h);
}

ui_action_t ui_hit_test_launcher(const ui_layout_t *layout, ui_view_t view,
                                 float x, float y) {
    if (!layout) return UI_ACT_NONE;
    if (view == UI_VIEW_LOG) {
        if (pt_in_rect(layout->btn_log_back, x, y)) return UI_ACT_LOG_BACK;
        if (pt_in_rect(layout->btn_refresh, x, y))  return UI_ACT_REFRESH;
        return UI_ACT_NONE;
    }
    if (pt_in_rect(layout->btn_primary, x, y)) {
        return layout->primary_is_stop ? UI_ACT_STOP : UI_ACT_LAUNCH;
    }
    if (pt_in_rect(layout->btn_refresh, x, y)) return UI_ACT_REFRESH;
    if (pt_in_rect(layout->btn_log, x, y))     return UI_ACT_VIEW_LOG;
    return UI_ACT_NONE;
}

ui_action_t ui_hit_test_overlay(const ui_layout_t *layout, float x, float y) {
    if (!layout) return UI_ACT_NONE;
    if (pt_in_rect(layout->btn_overlay_stop, x, y)) return UI_ACT_OVERLAY_STOP;
    if (pt_in_rect(layout->btn_overlay_hide, x, y)) return UI_ACT_OVERLAY_HIDE;
    return UI_ACT_NONE;
}
