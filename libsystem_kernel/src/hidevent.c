/*
 * hidevent.c - build IOHIDEvent serializations from presenter touch
 * messages.
 *
 * Implements hidevent.h exactly. One call takes one decoded TOUCH
 * message, updates the finger table, and emits one complete flat
 * IOHIDEvent serialization: one parent IOHIDDigitizerEventData
 * followed by one child IOHIDDigitizerEventData per finger that must
 * appear in this event.
 *
 * Byte format summary, all fields little-endian:
 *   - Every event struct is 104 bytes (sizeof(IOHIDDigitizerEventData)).
 *   - Common header (16 bytes): u32 size, u32 type (11 = Digitizer),
 *     u32 options, u8 depth (0 parent, 1 child), u8 reserved[3].
 *   - Parent size field is the TOTAL flat length (104 * (1 + children));
 *     each child size field is 104.
 *   - Coordinates are normalized [0,1] IOFixed (16.16); the CALLER
 *     must divide presenter client pixels by the client size before
 *     calling. hidevent.c only clamps to [0,1].
 *   - No timestamp and no sender ID live in the blob; the consumer
 *     supplies both out of band.
 *
 * Freestanding: C11, no libc. Uses only the little-endian helpers
 * from protocol.h. No function-pointer tables in static data.
 */
#include "hidevent.h"

#include <float.h>

/* ------------------------------------------------------------------ */
/* Format constants                    */
/* ------------------------------------------------------------------ */

#define HID_DIGITIZER_SIZE 104u
#define HID_TYPE_DIGITIZER 11u

/* Transducer types (IOHIDEventTypes.h). */
#define HID_TRANSDUCER_FINGER 2u
#define HID_TRANSDUCER_HAND 3u

/* Orientation type Quality (IOHIDEventTypes.h). */
#define HID_ORIENTATION_QUALITY 2u

/* Options base words. The range (0x10000) and
 * touch (0x20000) bits are NOT in these constants; write_digitizer
 * ORs them in from the range/touch arguments, so a caller cannot
 * pass an inconsistent combination. */
#define HID_OPT_PARENT_BASE 0x80003u /* Absolute|Collection|displayIntegrated */
#define HID_OPT_CHILD_BASE  0x80001u /* Absolute|displayIntegrated */

/* Event masks. */
#define HID_MASK_RANGE_TOUCH 0x3u /* Range | Touch */
#define HID_MASK_POSITION    0x4u /* Position */
#define HID_MASK_CANCEL      0x83u /* Cancel | Range | Touch */

/* IOFixed constants (16.16). */
#define HID_FIXED_ONE   65536
#define HID_FIXED_THREE (3 * 65536)
#define HID_FIXED_FIVE  (5 * 65536)

/* ------------------------------------------------------------------ */
/* Finger table                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t active;
    float x; /* normalized [0,1], as last emitted */
    float y;
} hid_finger_t;

static hid_finger_t g_fingers[ISWS_MAX_POINTERS];

/* ------------------------------------------------------------------ */
/* Small helpers (no libc)                                              */
/* ------------------------------------------------------------------ */

static void wr_i32le(uint8_t *p, int32_t v)
{
    isws_wr_u32le(p, (uint32_t)v);
}

/* Convert a clamped [0,1] float to IOFixed (16.16), round to nearest.
 * Input must already be clamped; NaN must have been rejected. */
static int32_t fixed_from_unit(float v)
{
    double d = (double)v * 65536.0 + 0.5;
    return (int32_t)d;
}

static float clamp_unit(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/* ------------------------------------------------------------------ */
/* Event struct writer                                                  */
/* ------------------------------------------------------------------ */

/*
 * Write one IOHIDDigitizerEventData at p (104 bytes). All fields per
 * hid-touch.md §1.3. `is_parent` selects the Hand collection parent
 * (position 0, identity 0, radii 3.0, quality fields 0); a child gets
 * the Finger values (identity 2, radii 5.0, quality/density/
 * irregularity 1.0). `total_size` is the size field value: the total
 * flat length for the parent, 104 for a child. `mask` goes into both
 * eventMask and (parent only) childEventMask; children get
 * childEventMask 0.
 */
static void write_digitizer(uint8_t *p, uint32_t total_size,
                            uint32_t options, uint8_t depth,
                            float x, float y,
                            uint32_t transducer_index,
                            uint32_t transducer_type,
                            uint32_t identity,
                            uint32_t event_mask,
                            uint32_t child_event_mask,
                            uint32_t range, uint32_t touch,
                            int32_t pressure)
{
    int32_t major_radius;
    int32_t minor_radius;
    int32_t quality;
    int32_t density;
    int32_t irregularity;
    uint32_t opt;

    if (transducer_type == HID_TRANSDUCER_FINGER) {
        major_radius = HID_FIXED_FIVE;
        minor_radius = HID_FIXED_FIVE;
        quality = HID_FIXED_ONE;
        density = HID_FIXED_ONE;
        irregularity = HID_FIXED_ONE;
    } else {
        major_radius = HID_FIXED_THREE;
        minor_radius = HID_FIXED_THREE;
        quality = 0;
        density = 0;
        irregularity = 0;
    }

    /* Rebuild the options word from the range/touch bits so the
     * caller cannot pass an inconsistent combination. The base
     * options already carry Absolute (+Collection for the parent)
     * and displayIntegrated; range/touch are ORed in here. */
    opt = options;
    if (range != 0)
        opt |= 0x10000u;
    if (touch != 0)
        opt |= 0x20000u;

    isws_wr_u32le(p + 0, total_size);
    isws_wr_u32le(p + 4, HID_TYPE_DIGITIZER);
    isws_wr_u32le(p + 8, opt);
    p[12] = depth;
    p[13] = 0;
    p[14] = 0;
    p[15] = 0;
    wr_i32le(p + 16, fixed_from_unit(x));
    wr_i32le(p + 20, fixed_from_unit(y));
    wr_i32le(p + 24, 0); /* position.z */
    isws_wr_u32le(p + 28, transducer_index);
    isws_wr_u32le(p + 32, transducer_type);
    isws_wr_u32le(p + 36, identity);
    isws_wr_u32le(p + 40, event_mask);
    isws_wr_u32le(p + 44, child_event_mask);
    isws_wr_u32le(p + 48, 0); /* buttonMask */
    wr_i32le(p + 52, pressure);
    wr_i32le(p + 56, 0); /* auxPressure */
    wr_i32le(p + 60, 0); /* twist */
    isws_wr_u32le(p + 64, HID_ORIENTATION_QUALITY);
    /* orientation.quality union: quality, density, irregularity,
     * majorRadius, minorRadius, accuracy. */
    wr_i32le(p + 68, quality);
    wr_i32le(p + 72, density);
    wr_i32le(p + 76, irregularity);
    wr_i32le(p + 80, major_radius);
    wr_i32le(p + 84, minor_radius);
    wr_i32le(p + 88, 0); /* accuracy */
    isws_wr_u32le(p + 92, 0); /* generationCount */
    isws_wr_u32le(p + 96, 0); /* willUpdateMask */
    isws_wr_u32le(p + 100, 0); /* didUpdateMask */
}

/* ------------------------------------------------------------------ */
/* hidevent.h implementation                                            */
/* ------------------------------------------------------------------ */

int hidevent_touch(const isws_touch_t *t, uint8_t *out, size_t cap)
{
    uint32_t id;
    uint32_t action;
    float x;
    float y;
    uint32_t i;
    uint32_t nchildren;
    uint32_t total_size;
    uint32_t mask_or;
    uint32_t named_mask;
    uint32_t named_range;
    uint32_t named_touch;
    int32_t named_pressure;
    int state_changed;

    if (t == NULL || out == NULL)
        return -1;

    action = t->action;
    id = t->pointer_id;
    if (action > ISWS_TOUCH_CANCEL)
        return -1;
    if (id >= ISWS_MAX_POINTERS)
        return -1;
    if (t->pointer_count == 0)
        return -1;

    /* Non-finite coordinates are rejected: NaN cannot be converted
     * to IOFixed (UB) and cannot be clamped (every comparison with
     * NaN is false), and infinities have no meaningful position.
     * FLT_MAX is the largest finite float, so anything beyond it in
     * either direction is an infinity. */
    if (t->x != t->x || t->y != t->y)
        return -1;
    if (t->x > FLT_MAX || t->x < -FLT_MAX)
        return -1;
    if (t->y > FLT_MAX || t->y < -FLT_MAX)
        return -1;

    x = clamp_unit(t->x);
    y = clamp_unit(t->y);

    /* Decide what this message does to the table, and the named
     * finger's mask/flags, without mutating anything yet. */
    state_changed = 0;
    named_mask = 0;
    named_range = 0;
    named_touch = 0;
    named_pressure = 0;
    if (action == ISWS_TOUCH_DOWN) {
        if (!g_fingers[id].active) {
            state_changed = 1;
        } else if (g_fingers[id].x != x || g_fingers[id].y != y) {
            state_changed = 1;
        }
        named_mask = HID_MASK_RANGE_TOUCH;
        named_range = 1;
        named_touch = 1;
        named_pressure = HID_FIXED_ONE;
    } else if (action == ISWS_TOUCH_MOVE) {
        if (!g_fingers[id].active)
            return 0; /* move for a finger that is not down: ignore */
        if (g_fingers[id].x == x && g_fingers[id].y == y)
            return 0; /* duplicate move: no state change, no event */
        state_changed = 1;
        named_mask = HID_MASK_POSITION;
        named_range = 1;
        named_touch = 1;
        named_pressure = HID_FIXED_ONE;
    } else if (action == ISWS_TOUCH_UP) {
        if (!g_fingers[id].active)
            return 0; /* up for a finger that is not down: ignore */
        state_changed = 1;
        named_mask = HID_MASK_RANGE_TOUCH;
        named_range = 0;
        named_touch = 0;
        named_pressure = 0;
    } else { /* ISWS_TOUCH_CANCEL */
        if (!g_fingers[id].active)
            return 0; /* cancel for a finger that is not down: ignore */
        state_changed = 1;
        named_mask = HID_MASK_CANCEL;
        named_range = 0;
        named_touch = 0;
        named_pressure = 0;
    }

    if (!state_changed)
        return 0;

    /* Count the children this event will carry: every active finger
     * after the update, plus the named finger when it is leaving
     * (UP/CANCEL), which is still emitted once as an up/cancel child.
     * For DOWN/MOVE the named finger is active after the update. */
    nchildren = 0;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (g_fingers[i].active)
            nchildren++;
    }
    if (action == ISWS_TOUCH_DOWN && !g_fingers[id].active)
        nchildren++;
    /* UP/CANCEL: the named finger is active now and counted above;
     * it stays in the count as the leaving child. DOWN/MOVE on an
     * already-active finger: already counted. */

    total_size = HID_DIGITIZER_SIZE * (1u + nchildren);
    if (cap < total_size)
        return -1;

    /* OR of all child masks, for the parent eventMask/childEventMask. */
    mask_or = 0;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (!g_fingers[i].active)
            continue;
        if (i == id) {
            mask_or |= named_mask;
        } else {
            mask_or |= HID_MASK_RANGE_TOUCH;
        }
    }
    if (action == ISWS_TOUCH_DOWN && !g_fingers[id].active)
        mask_or |= named_mask;

    /* Apply the table update now that the size check has passed. */
    if (action == ISWS_TOUCH_DOWN || action == ISWS_TOUCH_MOVE) {
        g_fingers[id].active = 1;
        g_fingers[id].x = x;
        g_fingers[id].y = y;
    }

    /* Parent: Hand collection, position (0,0), identity 0. The
     * parent is always in range (range 1, touch 0), giving the
     * options word 0x90003 that matches a real hardware event
     *. */
    write_digitizer(out, total_size, HID_OPT_PARENT_BASE, 0,
                    0.0f, 0.0f, 0, HID_TRANSDUCER_HAND, 0,
                    mask_or, mask_or, 1, 0, 0);

    /* Children in ascending pointer_id order. */
    {
        uint32_t child_index = 0;
        uint8_t *dst;
        for (i = 0; i < ISWS_MAX_POINTERS; i++) {
            uint32_t cmask;
            uint32_t crange;
            uint32_t ctouch;
            int32_t cpressure;
            float cx;
            float cy;
            uint32_t copt;

            if (i == id) {
                if (action == ISWS_TOUCH_UP ||
                    action == ISWS_TOUCH_CANCEL) {
                    /* Leaving finger: emitted once at the message
                     * position, then removed below. */
                    cmask = named_mask;
                    crange = named_range;
                    ctouch = named_touch;
                    cpressure = named_pressure;
                    cx = x;
                    cy = y;
                    copt = HID_OPT_CHILD_BASE;
                } else {
                    cmask = named_mask;
                    crange = named_range;
                    ctouch = named_touch;
                    cpressure = named_pressure;
                    cx = x;
                    cy = y;
                    copt = HID_OPT_CHILD_BASE;
                }
            } else {
                if (!g_fingers[i].active)
                    continue;
                /* Held finger: stationary, Range|Touch, flags 1. */
                cmask = HID_MASK_RANGE_TOUCH;
                crange = 1;
                ctouch = 1;
                cpressure = HID_FIXED_ONE;
                cx = g_fingers[i].x;
                cy = g_fingers[i].y;
                copt = HID_OPT_CHILD_BASE;
            }

            dst = out + HID_DIGITIZER_SIZE * (1u + child_index);
            write_digitizer(dst, HID_DIGITIZER_SIZE, copt, 1,
                            cx, cy, i, HID_TRANSDUCER_FINGER, 2,
                            cmask, 0, crange, ctouch, cpressure);
            child_index++;
        }
    }

    if (action == ISWS_TOUCH_UP || action == ISWS_TOUCH_CANCEL) {
        g_fingers[id].active = 0;
        g_fingers[id].x = 0.0f;
        g_fingers[id].y = 0.0f;
    }

    return (int)total_size;
}

void hidevent_reset(void)
{
    uint32_t i;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        g_fingers[i].active = 0;
        g_fingers[i].x = 0.0f;
        g_fingers[i].y = 0.0f;
    }
}

uint32_t hidevent_fingers_down(void)
{
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < ISWS_MAX_POINTERS; i++) {
        if (g_fingers[i].active)
            n++;
    }
    return n;
}
