#include "bks.h"
#include "linux.h"
#include <stdint.h>
#include <stddef.h>

extern int g_ipc_trace;

/* Freestanding string helpers */
static size_t bks_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void bks_print_str(const char *s) {
    if (!s) return;
    sys_write(2, s, bks_strlen(s));
}

static void bks_print_uint(uint64_t val) {
    char buf[32];
    int idx = 31;
    buf[idx--] = '\0';
    if (val == 0) {
        buf[idx--] = '0';
    } else {
        while (val > 0) {
            buf[idx--] = '0' + (val % 10);
            val /= 10;
        }
    }
    bks_print_str(&buf[idx + 1]);
}

/* Freestanding float parser */
static float bks_parse_float(const char **str) {
    const char *p = *str;
    float val = 0.0f;
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0f + (float)(*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        float factor = 0.1f;
        while (*p >= '0' && *p <= '9') {
            val += (float)(*p - '0') * factor;
            factor *= 0.1f;
            p++;
        }
    }
    *str = p;
    return val;
}

/* Parse screen configuration: default 1440x3200 @ 4.0 scale (019).
 * The device is a Xiaomi 12 Pro, 1440x3200 physical pixels, and the
 * window is 360x800 pt, so 4.0 is the true scale: it makes UIScreen
 * 360x800 pt @4, the snapshot frame 1440x3200, and the presenter
 * mapping 1:1. The old default 1080x2400 @3.0 reported a smaller
 * screen than the phone has, so every frame was upscaled 4/3x by
 * the presenter and any 1 px error became visible. Override with
 * IOSWINE_SCREEN=WxH@S (e.g. 1080x2400@3 restores the old geometry
 * for an A/B device check). */
static void bks_get_screen_info(float *out_width, float *out_height, float *out_scale, float *out_rot) {
    float width = 1440.0f;
    float height = 3200.0f;
    float scale = 4.0f;
    float rot = 0.0f;

    const char *env = lsk_getenv("IOSWINE_SCREEN");
    if (env && *env) {
        const char *p = env;
        float w = bks_parse_float(&p);
        if (*p == 'x' || *p == 'X') {
            p++;
            float h = bks_parse_float(&p);
            float s = 0.0f;
            if (*p == '@') {
                p++;
                s = bks_parse_float(&p);
            }
            if (w > 0.0f && h > 0.0f) {
                width = w;
                height = h;
            }
            if (s > 0.0f) {
                scale = s;
            }
        }
    }

    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
    if (out_scale) *out_scale = scale;
    if (out_rot) *out_rot = rot;
}

/* 60-byte MIG reply structure for BKSDisplayGetMainScreenInfo */
typedef struct {
    mach_msg_header_t Head;      /* 24 bytes */
    NDR_record_t      NDR;       /* 8 bytes */
    kern_return_t     RetCode;   /* 4 bytes (offset 32) */
    float             width;     /* 4 bytes (offset 36) */
    float             height;    /* 4 bytes (offset 40) */
    float             scale;     /* 4 bytes (offset 44) */
    float             rotation;  /* 4 bytes (offset 48) */
    uint32_t          flagE;     /* 4 bytes (offset 52) */
    uint32_t          flagF;     /* 4 bytes (offset 56) */
} bks_display_reply_t;

/* Process incoming message on com.apple.backboard.display.services kobject port */
int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size) {
    if (!hdr || send_size < sizeof(mach_msg_header_t)) return 0;

    if (hdr->msgh_id == BKS_DISPLAY_MIG_GET_MAIN_SCREEN_INFO) {
        if (max_reply_size < sizeof(bks_display_reply_t)) return 0;
        bks_display_reply_t *rep = (bks_display_reply_t *)reply_buf;

        rep->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep->Head.msgh_size = sizeof(bks_display_reply_t);
        rep->Head.msgh_remote_port = hdr->msgh_local_port;
        rep->Head.msgh_local_port = MACH_PORT_NULL;
        rep->Head.msgh_voucher_port = MACH_PORT_NULL;
        rep->Head.msgh_id = BKS_DISPLAY_MIG_REPLY_BASE;

        rep->NDR.mig_vers = 0;
        rep->NDR.if_vers = 0;
        rep->NDR.reserved1 = 0;
        rep->NDR.mig_encoding = 0;
        rep->NDR.int_rep = 1; /* NDR little-endian */
        rep->NDR.char_rep = 0;
        rep->NDR.float_rep = 0;
        rep->NDR.reserved2 = 0;

        rep->RetCode = 0;
        bks_get_screen_info(&rep->width, &rep->height, &rep->scale, &rep->rotation);
        rep->flagE = 0;
        rep->flagF = 0;

        if (g_ipc_trace) {
            bks_print_str("[bks] display_services msg id=6001012 width=");
            bks_print_uint((uint64_t)rep->width);
            bks_print_str(" height=");
            bks_print_uint((uint64_t)rep->height);
            bks_print_str(" scale=");
            bks_print_uint((uint64_t)rep->scale);
            bks_print_str(" -> reply size=60\n");
        }

        *out_reply_size = sizeof(bks_display_reply_t);
        return 1;
    } else {
        /* Unknown MIG id: reply MIG_BAD_ID (-301) like a real MIG server */
        if (max_reply_size < sizeof(mig_reply_error_t)) return 0;
        mig_reply_error_t *rep = (mig_reply_error_t *)reply_buf;

        rep->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep->Head.msgh_size = sizeof(mig_reply_error_t);
        rep->Head.msgh_remote_port = hdr->msgh_local_port;
        rep->Head.msgh_local_port = MACH_PORT_NULL;
        rep->Head.msgh_voucher_port = MACH_PORT_NULL;
        rep->Head.msgh_id = hdr->msgh_id + 100;

        rep->NDR.mig_vers = 0;
        rep->NDR.if_vers = 0;
        rep->NDR.reserved1 = 0;
        rep->NDR.mig_encoding = 0;
        rep->NDR.int_rep = 1;
        rep->NDR.char_rep = 0;
        rep->NDR.float_rep = 0;
        rep->NDR.reserved2 = 0;

        rep->RetCode = -301; /* MIG_BAD_ID */

        if (g_ipc_trace) {
            bks_print_str("[bks] unknown msg id=");
            bks_print_uint(hdr->msgh_id);
            bks_print_str(" -> MIG_BAD_ID (-301)\n");
        }

        *out_reply_size = sizeof(mig_reply_error_t);
        return 1;
    }
}
