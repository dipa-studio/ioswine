/*
 * ioswine-surface protocol, version 1.
 *
 * Shared by the Android presenter (server) and the iOS-side client.
 * Single header, C11, no bionic-only calls, usable on a plain Linux host
 * (for the unit test) and under the NDK.
 *
 * Wire format summary (see PROTOCOL.md for the authoritative spec):
 *   - Transport: AF_UNIX abstract socket "@ioswine-surface" (SOCK_STREAM).
 *   - Byte order: little-endian everywhere, including floats (IEEE-754).
 *   - Every message: 12-byte header + payload.
 *       u32 magic    = 0x49535753 ("ISWS")
 *       u16 type
 *       u16 flags    (per-message, see below; 0 unless stated)
 *       u32 length   = payload bytes after the header (0..65535)
 *   - The HELLO message is accompanied by one SCM_RIGHTS fd (a memfd
 *     holding both frame buffers). No other message carries an fd.
 *   - All structs in this header are *host-side API structs*, not wire
 *     layout: encode/decode functions in protocol.c serialise field by
 *     field, so host struct padding never leaks onto the wire.
 */
#ifndef IOSWINE_PROTOCOL_H
#define IOSWINE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ISWS_MAGIC          0x49535753u /* "ISWS" */
#define ISWS_VERSION        1u
#define ISWS_HEADER_SIZE    12u
#define ISWS_MAX_PAYLOAD    65535u

/* Pixel format. v1 supports exactly one. */
#define ISWS_FORMAT_BGRA8888_PREMUL 1u

/* Limits (see PROTOCOL.md). */
#define ISWS_MAX_WIDTH      8192u
#define ISWS_MAX_HEIGHT     8192u
#define ISWS_MAX_STRIDE     (ISWS_MAX_WIDTH * 4u)
#define ISWS_MAX_DAMAGE     16u
#define ISWS_MAX_POINTERS   10u
#define ISWS_NUM_BUFFERS    2u
#define ISWS_MAX_IN_FLIGHT  2u /* == ISWS_NUM_BUFFERS */

/* Abstract socket name (without the leading NUL byte). */
#define ISWS_SOCKET_NAME    "ioswine-surface"

/* ------------------------------------------------------------------ */
/* Message types                                                       */
/* ------------------------------------------------------------------ */

/* Client -> server */
#define ISWS_MSG_HELLO      1u
#define ISWS_MSG_PRESENT    2u
#define ISWS_MSG_GOODBYE    3u
#define ISWS_MSG_KEYBOARD   4u /* client asks server to show/hide the soft keyboard */
/* Server -> client */
#define ISWS_MSG_READY      0x81u
#define ISWS_MSG_FRAME_DONE 0x82u
#define ISWS_MSG_TOUCH      0x83u
#define ISWS_MSG_KEY        0x84u
#define ISWS_MSG_INFO       0x85u /* RESIZE / ROTATION / mapping update */
#define ISWS_MSG_FOCUS      0x86u
#define ISWS_MSG_ERROR      0x87u
#define ISWS_MSG_TEXT       0x88u /* committed / marked text, or delete */

/* ------------------------------------------------------------------ */
/* Payload structs (host-side API)                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t  x;      /* client pixels, may be negative before clipping */
    int32_t  y;
    uint32_t w;
    uint32_t h;
} isws_rect_t;

typedef struct {
    uint32_t version;   /* must be ISWS_VERSION */
    uint32_t width;     /* client frame width, pixels */
    uint32_t height;    /* client frame height, pixels */
    uint32_t stride;    /* bytes per row of one buffer, multiple of 4 */
    uint32_t format;    /* ISWS_FORMAT_* */
    uint32_t flags;     /* reserved, must be 0 */
} isws_hello_t;

typedef struct {
    uint64_t frame_id;              /* client-chosen, monotonically increasing */
    uint32_t buffer_index;          /* 0 or 1 */
    uint32_t damage_count;          /* 0..ISWS_MAX_DAMAGE; 0 = whole frame */
    isws_rect_t damage[ISWS_MAX_DAMAGE];
} isws_present_t;

typedef struct {
    uint32_t version;       /* server protocol version */
    uint32_t flags;         /* reserved */
    uint32_t max_in_flight; /* == ISWS_NUM_BUFFERS */
} isws_ready_t;

typedef struct {
    uint64_t frame_id;
    int64_t  vsync_ns;  /* CLOCK_MONOTONIC ns of the vsync the frame was posted on */
    uint32_t status;    /* 0 = presented, 1 = dropped (buffer still in flight) */
    uint32_t reserved;
} isws_frame_done_t;

/* Touch actions */
#define ISWS_TOUCH_DOWN     0u
#define ISWS_TOUCH_MOVE     1u
#define ISWS_TOUCH_UP       2u
#define ISWS_TOUCH_CANCEL   3u

typedef struct {
    uint32_t action;        /* ISWS_TOUCH_* */
    uint32_t pointer_id;    /* stable per contact, 0..ISWS_MAX_POINTERS-1 */
    uint32_t pointer_count; /* pointers currently down, including this one */
    float    x;             /* client pixels (after scale/letterbox mapping) */
    float    y;
    int64_t  timestamp_ns;  /* CLOCK_MONOTONIC */
} isws_touch_t;

/* Key codes: the v1 back key plus the hardware keys the keyboard
 * skeleton consumes. Values are Android AKEYCODE
 * numbers, so the presenter forwards them unchanged. Printable
 * keys normally arrive as ISWS_MSG_TEXT, not as KEY events: the
 * Android IME delivers them through commitText, and only hardware
 * keyboards and the system back key produce KEY events. */
#define ISWS_KEY_BACK       4u
#define ISWS_KEY_DPAD_UP    19u
#define ISWS_KEY_DPAD_DOWN  20u
#define ISWS_KEY_DPAD_LEFT  21u
#define ISWS_KEY_DPAD_RIGHT 22u
#define ISWS_KEY_TAB        61u
#define ISWS_KEY_ENTER      66u
#define ISWS_KEY_DEL        67u
#define ISWS_KEY_ESC        111u
#define ISWS_KEY_ACTION_DOWN 0u
#define ISWS_KEY_ACTION_UP   1u

typedef struct {
    uint32_t keycode;       /* ISWS_KEY_* */
    uint32_t action;        /* ISWS_KEY_ACTION_* */
    int64_t  timestamp_ns;
} isws_key_t;

/* Text input (ISWS_MSG_TEXT, server -> client).
 * Wire payload: u32 kind, u32 count, then utf8_len = payload - 8
 * bytes of UTF-8 text (no NUL terminator). kind 0 = commit
 * (insertText:), 1 = marked composition (setMarkedText:), 2 =
 * unmark (count ignored, utf8 empty), 3 = delete (count =
 * characters to delete, utf8 empty). For kind 0/1, count is the
 * UTF-8 byte length and must equal payload - 8. The decoded utf8
 * pointer borrows the caller's buffer: copy it before the buffer
 * is reused. UTF-8 validity is checked by the consumer
 * (keyboard_validate_utf8), not by the decoder. */
#define ISWS_TEXT_COMMIT    0u
#define ISWS_TEXT_MARKED    1u
#define ISWS_TEXT_UNMARK    2u
#define ISWS_TEXT_DELETE    3u

typedef struct {
    uint32_t kind;          /* ISWS_TEXT_* */
    uint32_t count;         /* delete count (kind 3) or byte length (kind 0/1) */
    const uint8_t *utf8;    /* borrowed; NULL when utf8_len == 0 */
    size_t utf8_len;        /* payload bytes after the 8-byte fixed part */
} isws_text_t;

/* Keyboard visibility request (ISWS_MSG_KEYBOARD, client -> server).
 * The client sends it when a text field becomes or
 * resigns first responder. traits is the KB_TRAIT_* bitfield (see
 * keyboard.h): bit 0 secure, bit 1 no autocorrect, bits 8..15
 * keyboard type, bits 16..23 return key type. frame_h_pts is the
 * keyboard height in window points, or 0 when unknown (v1 always
 * sends 0: the client does not know the Android IME height). */
typedef struct {
    uint32_t show;          /* 0 = hide, 1 = show */
    uint32_t traits;        /* KB_TRAIT_* bitfield */
    float    frame_h_pts;   /* 0 = unknown */
    uint32_t reserved;      /* must be 0 */
} isws_keyboard_req_t;

/* Screen <-> client mapping, sent in INFO and computed locally. */
typedef struct {
    uint32_t screen_w;  /* window size, physical pixels */
    uint32_t screen_h;
    uint32_t client_w;
    uint32_t client_h;
    float    scale;     /* aspect-fit scale, > 0 */
    float    offset_x;  /* screen px of the client frame's top-left corner */
    float    offset_y;
    uint32_t rotation;  /* 0, 90, 180, 270 degrees */
} isws_info_t;

typedef struct {
    uint32_t focused;   /* 0 = lost focus, 1 = gained focus */
    uint32_t reserved;
} isws_focus_t;

typedef struct {
    uint32_t code;      /* ISWS_ERR_* */
    /* message text follows the fixed part as raw bytes (not NUL-terminated
     * unless the sender includes it); length = payload - 4. */
} isws_error_t;

/* Error codes */
#define ISWS_ERR_NONE           0u
#define ISWS_ERR_VERSION        1u /* unsupported protocol version */
#define ISWS_ERR_BAD_HELLO      2u /* invalid width/height/stride/format */
#define ISWS_ERR_BAD_MESSAGE    3u /* unknown type / bad magic / bad length */
#define ISWS_ERR_BAD_PRESENT    4u /* bad buffer index / damage / no HELLO yet */
#define ISWS_ERR_NO_FD          5u /* HELLO arrived without the memfd */
#define ISWS_ERR_INTERNAL       6u

/* ------------------------------------------------------------------ */
/* Little-endian helpers (unaligned-safe, host-independent)            */
/* ------------------------------------------------------------------ */

uint16_t isws_rd_u16le(const uint8_t *p);
uint32_t isws_rd_u32le(const uint8_t *p);
uint64_t isws_rd_u64le(const uint8_t *p);
int64_t  isws_rd_i64le(const uint8_t *p);
float    isws_rd_f32le(const uint8_t *p);
void isws_wr_u16le(uint8_t *p, uint16_t v);
void isws_wr_u32le(uint8_t *p, uint32_t v);
void isws_wr_u64le(uint8_t *p, uint64_t v);
void isws_wr_i64le(uint8_t *p, int64_t v);
void isws_wr_f32le(uint8_t *p, float v);

/* ------------------------------------------------------------------ */
/* Header encode/decode                                                */
/*                                                                     */
/* isws_encode_header: writes 12 bytes. Returns ISWS_HEADER_SIZE.      */
/* isws_decode_header: validates magic + length bounds. Returns 0 on   */
/*   success, -1 on bad magic, -2 on length > ISWS_MAX_PAYLOAD.        */
/*   On success fills *type, *flags, *length.                          */
/* ------------------------------------------------------------------ */

size_t isws_encode_header(uint8_t *out, uint16_t type, uint16_t flags,
                          uint32_t length);
int isws_decode_header(const uint8_t *buf, size_t len,
                       uint16_t *type, uint16_t *flags, uint32_t *length);

/* ------------------------------------------------------------------ */
/* Per-message encode/decode.                                          */
/* Encode returns payload bytes written, or 0 if the output buffer is  */
/* too small. Decode returns 0 on success, -1 on bad length/content.   */
/* Payload sizes (fixed part):                                         */
/*   HELLO 24, PRESENT 16 + 16*damage_count, READY 12, FRAME_DONE 24,  */
/*   TOUCH 28, KEY 16, INFO 32, FOCUS 8, ERROR 4 + message bytes,
 *   TEXT 8 + utf8 bytes, KEYBOARD 16.      */
/* ------------------------------------------------------------------ */

#define ISWS_HELLO_PAYLOAD_SIZE       24u
#define ISWS_READY_PAYLOAD_SIZE       12u
#define ISWS_FRAME_DONE_PAYLOAD_SIZE  24u
#define ISWS_TOUCH_PAYLOAD_SIZE       28u
#define ISWS_KEY_PAYLOAD_SIZE         16u
#define ISWS_INFO_PAYLOAD_SIZE        32u
#define ISWS_FOCUS_PAYLOAD_SIZE       8u
#define ISWS_TEXT_PAYLOAD_MIN         8u
#define ISWS_KEYBOARD_PAYLOAD_SIZE    16u

size_t isws_encode_hello(uint8_t *out, size_t cap, const isws_hello_t *m);
int    isws_decode_hello(const uint8_t *buf, size_t len, isws_hello_t *m);

size_t isws_encode_present(uint8_t *out, size_t cap, const isws_present_t *m);
int    isws_decode_present(const uint8_t *buf, size_t len, isws_present_t *m);

size_t isws_encode_ready(uint8_t *out, size_t cap, const isws_ready_t *m);
int    isws_decode_ready(const uint8_t *buf, size_t len, isws_ready_t *m);

size_t isws_encode_frame_done(uint8_t *out, size_t cap, const isws_frame_done_t *m);
int    isws_decode_frame_done(const uint8_t *buf, size_t len, isws_frame_done_t *m);

size_t isws_encode_touch(uint8_t *out, size_t cap, const isws_touch_t *m);
int    isws_decode_touch(const uint8_t *buf, size_t len, isws_touch_t *m);

size_t isws_encode_key(uint8_t *out, size_t cap, const isws_key_t *m);
int    isws_decode_key(const uint8_t *buf, size_t len, isws_key_t *m);

size_t isws_encode_text(uint8_t *out, size_t cap, const isws_text_t *m);
int    isws_decode_text(const uint8_t *buf, size_t len, isws_text_t *m);

size_t isws_encode_keyboard_req(uint8_t *out, size_t cap, const isws_keyboard_req_t *m);
int    isws_decode_keyboard_req(const uint8_t *buf, size_t len, isws_keyboard_req_t *m);

size_t isws_encode_info(uint8_t *out, size_t cap, const isws_info_t *m);
int    isws_decode_info(const uint8_t *buf, size_t len, isws_info_t *m);

size_t isws_encode_focus(uint8_t *out, size_t cap, const isws_focus_t *m);
int    isws_decode_focus(const uint8_t *buf, size_t len, isws_focus_t *m);

/* ERROR: message text is raw bytes; encode copies at most cap-4 of it. */
size_t isws_encode_error(uint8_t *out, size_t cap, uint32_t code,
                         const char *msg, size_t msg_len);
int    isws_decode_error(const uint8_t *buf, size_t len, uint32_t *code,
                         const uint8_t **msg, size_t *msg_len);

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

/* Validate a HELLO's geometry. Returns 0 or an ISWS_ERR_* code. */
uint32_t isws_validate_hello(const isws_hello_t *m);

/* Memfd size for a HELLO: stride * height * ISWS_NUM_BUFFERS. Returns 0
 * on overflow / invalid geometry. */
uint64_t isws_memfd_size(const isws_hello_t *m);

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                    */
/* ------------------------------------------------------------------ */

/* Aspect-fit letterbox mapping. scale = min(sw/cw, sh/ch); offsets centre
 * the client frame in the window. rotation is recorded, not applied: the
 * client frame is always presented upright in the window's current
 * orientation (the server re-sends INFO on rotation). Returns 0, or -1
 * if any dimension is 0. */
int isws_compute_mapping(uint32_t screen_w, uint32_t screen_h,
                         uint32_t client_w, uint32_t client_h,
                         uint32_t rotation, isws_info_t *out);

/* Screen (window) px -> client px. Returns 0 if inside the client frame,
 * -1 if the point falls in a letterbox bar (out still clamped into the
 * client frame). */
int isws_screen_to_client(const isws_info_t *map, float sx, float sy,
                          float *cx, float *cy);

/* Client px -> screen px. */
void isws_client_to_screen(const isws_info_t *map, float cx, float cy,
                           float *sx, float *sy);

/* Clip a damage rect to [0,0,w,h). Returns 0 if the (possibly clipped)
 * rect is non-empty, -1 if it is empty / fully outside. */
int isws_clip_damage(const isws_rect_t *in, uint32_t w, uint32_t h,
                     isws_rect_t *out);

/* Expand a PRESENT's damage list: damage_count == 0 means the whole
 * frame; otherwise clip each rect and drop empties. Returns the number
 * of rects written to out (<= ISWS_MAX_DAMAGE). */
size_t isws_expand_damage(const isws_present_t *p, uint32_t w, uint32_t h,
                          isws_rect_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_PROTOCOL_H */
