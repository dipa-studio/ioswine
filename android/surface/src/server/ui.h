/*
 * ui.h - Pure-C RGBA8888 launcher screen & running stats overlay renderer.
 */
#ifndef IOSWINE_UI_H
#define IOSWINE_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} ui_rect_t;

typedef enum {
    UI_VIEW_LAUNCHER = 0,
    UI_VIEW_LOG = 1,
} ui_view_t;

typedef enum {
    UI_ACT_NONE = 0,
    UI_ACT_LAUNCH,
    UI_ACT_STOP,
    UI_ACT_REFRESH,
    UI_ACT_VIEW_LOG,
    UI_ACT_LOG_BACK,
    UI_ACT_OVERLAY_STOP,
    UI_ACT_OVERLAY_HIDE,
} ui_action_t;

typedef struct {
    const isws_sysinfo_t *sys;
    ui_view_t view;
    bool launching;
    bool stopping;
    const char *status_msg;
    bool have_session_stats;
    uint64_t last_session_frames;
    uint64_t last_session_dropped;
    int64_t last_session_duration_s;
} ui_launcher_state_t;

typedef struct {
    double fps;
    uint64_t frames_presented;
    uint64_t frames_dropped;
    uint64_t touches_sent;
    int64_t uptime_s;
    int32_t client_pid;
} ui_overlay_state_t;

typedef struct {
    ui_rect_t btn_primary;
    bool primary_is_stop;
    ui_rect_t btn_refresh;
    ui_rect_t btn_log;
    ui_rect_t btn_log_back;
    ui_rect_t overlay_box;
    ui_rect_t btn_overlay_stop;
    ui_rect_t btn_overlay_hide;
} ui_layout_t;

/* Draw the full launcher (or log viewer) screen into an RGBA_8888 buffer. */
void ui_draw_launcher(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                      const ui_launcher_state_t *st, ui_layout_t *out_layout);

/* Compute the overlay bounding rectangle for a given window size. */
void ui_overlay_bounds(int32_t w, int32_t h, ui_rect_t *out_box);

/* Composite the semi-transparent top-left stats overlay over an existing frame. */
void ui_draw_overlay(uint32_t *dst, int32_t w, int32_t h, int32_t stride_px,
                     const ui_overlay_state_t *st, ui_layout_t *out_layout);

/* Hit-test touch coordinates (screen pixels) against launcher or overlay buttons. */
ui_action_t ui_hit_test_launcher(const ui_layout_t *layout, ui_view_t view,
                                 float x, float y);
ui_action_t ui_hit_test_overlay(const ui_layout_t *layout, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_UI_H */
