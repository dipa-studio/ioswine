/*
 * fakeboard.c: in-process fake FrontBoard workspace service for ioswine.
 *
 * Answers the workspace service endpoints and drives the scene
 * lifecycle from the server side. See fakeboard.h for the endpoint
 * discovery and trigger decisions; this file implements them.
 *
 * Dispatch is keyed by bsxpc_SEL (via
 * bsxpc_message_parse), NOT by the old "message" key. The old
 * envelope keys "message"/"arguments"/"reply" were WRONG; the fake
 * answered error=1 (unknown) for every real message. The real keys
 * are "bsxpc", "bsxpc_context", "bsxpc_CID", "bsxpc_CIDr" and
 * "bsxpc_SEL" (device 2026-11-12). Replies are completion replies:
 * same CID, CIDr=true, plus the result fields. The scene-create and
 * settings-update callout envelopes use the same real keys. The
 * lifecycle-action callouts (activate / destroy) stay bare BSAction
 * dicts: whether real lifecycle actions travel bare or
 * enveloped is still [guess].
 *
 * THE "connect" REPLY RULE (working default; see
 * fakeboard.h). The incoming msgh_id is the reply-expectation
 * signal: 0x40000000 means a reply is expected, 0x10000000 means
 * the message is one-way and NO reply may be sent. This is enforced
 * in fakeboard_process_msg, which sees the Mach header. The svchost
 * adapter handler cannot enforce it (it never sees the header).
 *
 * Entry points: fakeboard_init registers the four service names with
 * svchost_register (the svchost adapter handler feeds the same
 * dispatch core as fakeboard_process_msg, but without the raw Mach
 * header, so it can only learn the endpoint from a mach_send value
 * in the message). fakeboard_process_msg is the full kobject entry
 * for a future ipc.c branch and for the host simulator.
 *
 * Every wire string comes from fakeboard_names.h. This file contains
 * no wire-string literals (checked by grep); the only string-like
 * tokens are macro names from that header.
 *
 * Style: freestanding C, same as launchd.c / bks.c / svchost.c. No
 * libc, no unaligned reads (all multi-byte wire access goes through
 * xpcwire), every read bounds-checked by the reader. No em dashes.
 */
#include "fakeboard.h"
#include "fakeboard_names.h"
#include "bsxpc.h"
#include "linux.h"

/* ------------------------------------------------------------------ */
/* Small freestanding helpers (fb_ prefixed, like ld_ / sh_ elsewhere). */
/* ------------------------------------------------------------------ */

static uint32_t fb_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int fb_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int fb_streq(const char *a, const char *b) {
    return fb_strcmp(a, b) == 0;
}

static int fb_strncmp(const char *a, const char *b, uint32_t n) {
    uint32_t i = 0;
    while (i < n && a[i] && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

static void fb_memcpy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

static void fb_memset(uint8_t *dst, uint8_t v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = v;
}

/* Bounded copy, always NUL terminated. Truncates silently. */
static void fb_copy_str(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Map a builder/reader XW_ERR_* to a KERN_* code for the public API.
 * Positive KERN_* codes pass through unchanged; next_callout keeps
 * the XW_ERR_* negatives. */
static int fb_xw_to_kern(int r) {
    if (r == XW_OK) return KERN_SUCCESS;
    if (r > 0) return r;
    if (r == XW_ERR_NOSPACE) return KERN_NO_SPACE;
    return KERN_FAILURE;
}

/* ------------------------------------------------------------------ */
/* BSXPC key tables, built from fakeboard_names.h only.                */
/* ------------------------------------------------------------------ */

/* Real envelope keys (device 2026-11-12). Designated
 * initializers, so the field order in bsxpc_envelope_keys
 * does not matter, only the field names. */
static const bsxpc_envelope_keys FB_ENV_KEYS = {
    .key_bsxpc = FB_BSXPC_KEY_BSXPC,
    .key_context = FB_BSXPC_KEY_CONTEXT,
    .key_cid = FB_BSXPC_KEY_CID,
    .key_cidr = FB_BSXPC_KEY_CIDR,
    .key_sel = FB_BSXPC_KEY_SEL
};
static const bsxpc_action_keys FB_ACTION_KEYS = {
    FB_BSXPC_KEY_ACTION_KIND, FB_BSXPC_KEY_ACTION_INFO,
    FB_BSXPC_KEY_ACTION_TARGET, FB_BSXPC_KEY_ACTION_TIMEOUT,
    FB_BSXPC_KEY_ACTION_RESPONSE
};
static const bsxpc_handle_keys FB_HANDLE_KEYS = {
    FB_BSXPC_KEY_HANDLE_PID, FB_BSXPC_KEY_HANDLE_BUNDLE_ID,
    FB_BSXPC_KEY_HANDLE_AUDIT_TOKEN, FB_BSXPC_KEY_HANDLE_IS_RUNNING,
    FB_BSXPC_KEY_HANDLE_IS_FOREGROUND, FB_BSXPC_KEY_HANDLE_IS_VALID,
    FB_BSXPC_KEY_HANDLE_IS_APPLICATION
};

/* ------------------------------------------------------------------ */
/* State.                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int index;
    mach_port_name_t port;
} fb_service_t;

static uint32_t g_fb_sas_endpoint; /* latest systemappservices connection endpoint (w00t) */
static fb_service_t g_fb_services[FB_NUM_SERVICES] = {
    { FB_SVC_WORKSPACE_SERVICE, 0, MACH_PORT_NULL },
    { FB_SVC_WORKSPACE_COUPLER, 1, MACH_PORT_NULL },
    { FB_SVC_SYSTEMAPP_SERVICES, 2, MACH_PORT_NULL },
    { FB_SVC_SYSTEM_SERVICE, 3, MACH_PORT_NULL }
};

static int g_fb_inited;
static int g_fb_state = FB_SCENE_NONE;
static uint32_t g_fb_self_pid;
static int g_fb_pid_set;
static char g_fb_bundle_id[128];
static char g_fb_scene_id[192];
static char g_fb_persistence_id[48];
static uint32_t g_fb_uuid_counter = 1;
/* Client callout endpoint: a send right to the client's connection
 * port. 0 means unknown. one_shot means it came from a reply port
 * and must be cleared after one delivered callout. */
static uint32_t g_fb_client_endpoint;
static int g_fb_endpoint_one_shot;
static int g_fb_handshake_done[FB_NUM_SERVICES];
/* Client-chosen child connection CID from the workspace-service "connect"
 * handshake (BSXPCServiceConnectionRootContext childContextWithRemoteIdentifier:).
 * Server->client callouts must echo this CID with CIDr=true [src: 0x19ba9ccd0]. */
static uint64_t g_fb_workspace_cid = 1;

/* Outbound callout queue: complete XPC bodies plus their msgh_id.
 * The Mach header is filled at dequeue time, when the endpoint is
 * known, so queued callouts survive an endpoint arriving late. */
typedef struct {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len;
    uint32_t msgh_id;
} fb_callout_t;

static fb_callout_t g_fb_queue[FB_CALLOUT_QUEUE_LEN];
static uint32_t g_fb_q_head;
static uint32_t g_fb_q_tail;
static uint32_t g_fb_q_count;

static int fb_queue_callout(const uint8_t *body, uint32_t body_len,
                            uint32_t msgh_id) {
    fb_callout_t *slot;
    if (g_fb_q_count >= FB_CALLOUT_QUEUE_LEN) return XW_ERR_NOSPACE;
    if (body_len > FB_CALLOUT_MAX_BODY) return XW_ERR_NOSPACE;
    slot = &g_fb_queue[g_fb_q_tail];
    fb_memcpy(slot->body, body, body_len);
    slot->body_len = body_len;
    slot->msgh_id = msgh_id;
    g_fb_q_tail = (g_fb_q_tail + 1) % FB_CALLOUT_QUEUE_LEN;
    g_fb_q_count++;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Identity, UUID, process handle.                                     */
/* ------------------------------------------------------------------ */

/* g_fb_scene_id = "sceneID:" + bundle_id + "-default". Returns 0 or
 * -1 when the bundle id is too long for the buffer. */
static int fb_build_scene_id(const char *bundle_id) {
    const char *parts[3];
    uint32_t pos = 0;
    int p;
    uint32_t i;
    parts[0] = FB_SCENE_ID_PREFIX;
    parts[1] = bundle_id;
    parts[2] = FB_SCENE_ID_SUFFIX;
    for (p = 0; p < 3; p++) {
        for (i = 0; parts[p][i]; i++) {
            if (pos + 1 >= sizeof(g_fb_scene_id)) {
                g_fb_scene_id[0] = 0;
                return -1;
            }
            g_fb_scene_id[pos++] = parts[p][i];
        }
    }
    g_fb_scene_id[pos] = 0;
    return 0;
}

/* Fresh persistence identifier per scene: fixed prefix, the counter
 * in the first 8 hex digits, fixed tail. Format is [guess]; see
 * fakeboard_names.h. */
static void fb_uuid_next(char *out, uint32_t cap) {
    uint32_t n = g_fb_uuid_counter++;
    uint32_t pos = 0;
    int i;
    const char *prefix = FB_UUID_PREFIX;
    const char *tail = FB_UUID_TAIL;
    const char *hex = FB_UUID_HEX_DIGITS;
    fb_memset((uint8_t *)out, 0, cap);
    if (cap == 0) return;
    while (prefix[pos] && pos + 1 < cap) {
        out[pos] = prefix[pos];
        pos++;
    }
    for (i = 7; i >= 0; i--) {
        if (pos + 1 >= cap) return;
        out[pos++] = hex[(n >> (i * 4)) & 0xf];
    }
    i = 0;
    while (tail[i] && pos + 1 < cap) {
        out[pos++] = tail[i++];
    }
    out[pos] = 0;
}

static void fb_self_handle(bsxpc_process_handle *h) {
    h->pid = g_fb_self_pid;
    h->bundle_id = (g_fb_bundle_id[0] != 0) ? g_fb_bundle_id
                                            : FB_DEFAULT_BUNDLE_ID;
    bsxpc_audit_token_forge(&h->audit, g_fb_self_pid);
    h->is_running = 1;
    h->is_foreground = (g_fb_state == FB_SCENE_FOREGROUND) ? 1 : 0;
    h->is_valid = 1;
    h->is_application = 1;
}

/* ------------------------------------------------------------------ */
/* Reply opening.                                                      */
/* ------------------------------------------------------------------ */

/* Open a reply dict for a parsed request. When the request carried a
 * CID, open a completion reply (same CID, CIDr=true) via
 * bsxpc_reply_build; otherwise open a plain dict. The caller adds
 * the result fields and closes with xw_dict_end. Returns XW_OK or a
 * negative XW_ERR_* (via bsxpc_reply_build or xw_dict_begin). */
static int fb_reply_begin(xw_builder *b, const bsxpc_message *m) {
    if (m != NULL && m->has_cid)
        return bsxpc_reply_build(b, &FB_ENV_KEYS, m->cid);
    return xw_dict_begin(b);
}

/* ------------------------------------------------------------------ */
/* Verified BaseBoard / FrontBoard XPC object builders.               */
/* Every schema matches iOS 26.6.2 (23G90) dsc decoders                */
/*. Zero string literals.    */
/* ------------------------------------------------------------------ */

static int fb_add_double_arr2(xw_builder *b, const char *key,
                              double a, double c) {
    int r = xw_dict_add_array_begin(b, key);
    if (r == XW_OK) r = xw_add_double(b, a);
    if (r == XW_OK) r = xw_add_double(b, c);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

static int fb_add_double_arr4(xw_builder *b, const char *key,
                              double a, double b1, double c, double d) {
    int r = xw_dict_add_array_begin(b, key);
    if (r == XW_OK) r = xw_add_double(b, a);
    if (r == XW_OK) r = xw_add_double(b, b1);
    if (r == XW_OK) r = xw_add_double(b, c);
    if (r == XW_OK) r = xw_add_double(b, d);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

/* BSSettings "flags" entry: [uint64 setting_id, bool value]. */
static int fb_add_flag_entry(xw_builder *b, uint64_t setting_id, int val) {
    int r = xw_array_begin(b);
    if (r == XW_OK) r = xw_add_uint64(b, setting_id);
    if (r == XW_OK) r = xw_add_bool(b, val ? 1 : 0);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

/* Open a BSSettings "objects" entry tuple with coder_type = 1 (BSXPCCoder):
 *   [uint64 setting_id, {"__BSXPCSecureClass": cls, "__BSXPCSecureValue": ...
 * Caller adds "__BSXPCSecureValue" and calls fb_obj_entry_end(). */
static int fb_obj_entry_begin(xw_builder *b, uint64_t setting_id,
                              const char *cls) {
    int r = xw_array_begin(b);
    if (r == XW_OK) r = xw_add_uint64(b, setting_id);
    if (r == XW_OK) r = xw_dict_begin(b);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_SEC_CLASS, cls);
    return r;
}

static int fb_obj_entry_end(xw_builder *b) {
    int r = xw_dict_end(b);
    if (r == XW_OK) r = xw_add_int64(b, FB_BSX_CODER_TYPE_SECURE);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

static int fb_obj_num_int64(xw_builder *b, uint64_t setting_id, int64_t v) {
    int r = fb_obj_entry_begin(b, setting_id, FB_CLS_NSNUMBER);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_BSX_KEY_SEC_VALUE, v);
    if (r == XW_OK) r = fb_obj_entry_end(b);
    return r;
}

static int fb_obj_num_double(xw_builder *b, uint64_t setting_id, double v) {
    int r = fb_obj_entry_begin(b, setting_id, FB_CLS_NSNUMBER);
    if (r == XW_OK) r = xw_dict_add_double(b, FB_BSX_KEY_SEC_VALUE, v);
    if (r == XW_OK) r = fb_obj_entry_end(b);
    return r;
}

/* NSValue(CGRect) encoded as {"data": <32B IEEE754 double[4]>, "type": "..."}. */
static int fb_obj_cgrect(xw_builder *b, uint64_t setting_id,
                         double x, double y, double w, double h) {
    double rect[4];
    int r;
    rect[0] = x;
    rect[1] = y;
    rect[2] = w;
    rect[3] = h;
    r = fb_obj_entry_begin(b, setting_id, FB_CLS_NSVALUE);
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_BSX_KEY_SEC_VALUE);
    /* Device 2026-11-15: without bsx_class the dict decodes as __NSDictionaryI
     * ("does not match expected class NSValue", BSServiceConnectionErrorDomain 4). */
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS, FB_CLS_NSVALUE);
    if (r == XW_OK) r = xw_dict_add_data(b, FB_NSVALUE_DATA, rect,
                                         (uint32_t)sizeof(rect));
    if (r == XW_OK) r = xw_dict_add_string(b, FB_NSVALUE_TYPE,
                                           FB_OBJC_TYPE_CGRECT);
    if (r == XW_OK) r = xw_dict_end(b);
    if (r == XW_OK) r = fb_obj_entry_end(b);
    return r;
}

static int fb_display_identity(xw_builder *b, const char *key) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_DISPLAY_IDENTITY);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPID_TYPE, 0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPID_CONN_TYPE, 0);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISPID_MAIN_LIKE, 1);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISPID_DISPLAY_ID, 1);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISPID_CONN_SEED, 1);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPID_PID, 0);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISPID_EXTERNAL, 0);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_DISPID_UNIQUE_ID,
                                           FB_DISPLAY_HW_ID);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISPID_SECURE, 1);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

static int fb_display_mode(xw_builder *b, const char *key) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_DISPLAY_MODE);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISPMODE_WIDTH,
                                            (uint64_t)FB_DISPLAY_WIDTH_PX);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISPMODE_HEIGHT,
                                            (uint64_t)FB_DISPLAY_HEIGHT_PX);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISPMODE_PREF_SCALE,
                                            (uint64_t)FB_DISPLAY_SCALE);
    if (r == XW_OK) r = xw_dict_add_double(b, FB_DISPMODE_SCALE_OVERRIDE, 0.0);
    if (r == XW_OK) r = xw_dict_add_double(b, FB_DISP_REFRESH_RATE, 60.0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPMODE_GAMUT, 0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPMODE_HDR, 0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISPMODE_ROTATION, 0);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISPMODE_VIRTUAL, 0);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

static int fb_obj_display_configuration(xw_builder *b, uint64_t setting_id) {
    int r = fb_obj_entry_begin(b, setting_id, FB_CLS_DISPLAY_CONFIG);
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_BSX_KEY_SEC_VALUE);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_DISPLAY_CONFIG);
    if (r == XW_OK) r = fb_display_identity(b, FB_DISP_IDENTITY);
    if (r == XW_OK) r = fb_display_mode(b, FB_DISP_CURRENT_MODE);
    if (r == XW_OK) r = fb_display_mode(b, FB_DISP_PREFERRED_MODE);
    if (r == XW_OK) r = fb_add_double_arr4(b, FB_DISP_BOUNDS, 0.0, 0.0,
                                           FB_DISPLAY_WIDTH_PT,
                                           FB_DISPLAY_HEIGHT_PT);
    if (r == XW_OK) r = fb_add_double_arr4(b, FB_DISP_NATIVE_BOUNDS, 0.0, 0.0,
                                           FB_DISPLAY_WIDTH_PX,
                                           FB_DISPLAY_HEIGHT_PX);
    if (r == XW_OK) r = fb_add_double_arr2(b, FB_DISP_PIXEL_SIZE,
                                           FB_DISPLAY_WIDTH_PX,
                                           FB_DISPLAY_HEIGHT_PX);
    if (r == XW_OK) r = fb_add_double_arr2(b, FB_DISP_SAFE_OVERSCAN_RATIO,
                                           1.0, 1.0);
    if (r == XW_OK) r = xw_dict_add_double(b, FB_DISP_LATENCY, 0.0);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_DISP_HARDWARE_ID,
                                           FB_DISPLAY_HW_ID);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_DISP_NAME, FB_DISPLAY_NAME);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_DISP_DEVICE_NAME,
                                           FB_DISPLAY_NAME);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_DISP_SEED, 1);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISP_NOT_COMPARABLE, 0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISP_TAGS, 0);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISP_CLONING_SUPPORTED, 0);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_DISP_OVERSCANNED, 0);
    if (r == XW_OK) r = xw_dict_add_int64(b, FB_DISP_OVERSCAN_COMP, 0);
    if (r == XW_OK) r = xw_dict_end(b); /* __BSXPCSecureValue */
    if (r == XW_OK) r = fb_obj_entry_end(b);
    return r;
}

/* Build FBSSceneSettingsDiff under `key`: mainDiff (BSKeyedSettingsDiff for
 * FBSSceneSettingsCore keys) + otherDiff (BSSettingsDiff for legacy
 * UIApplicationSceneSettings numeric IDs). */
static int fb_build_scene_settings_diff(xw_builder *b, const char *key,
                                        int foreground) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_SCENE_SETTINGS_DIFF);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_DIFF_SETTINGS_CLASS,
                                           FB_CLS_UI_SCENE_SETTINGS);
    /* mainDiff -> BSKeyedSettingsDiff -> _changes -> BSKeyedSettings */
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_DIFF_MAIN);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_KEYED_SETTINGS_DIFF);
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_DIFF_CHANGES);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_KEYED_SETTINGS);
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FB_SETTINGS_BSKEYS);
    if (r == XW_OK) r = xw_add_string(b, FB_SET_FOREGROUND);
    if (r == XW_OK) r = xw_add_string(b, FB_SET_FRAME);
    if (r == XW_OK) r = xw_add_string(b, FB_SET_DISPLAY_CONFIGURATION);
    if (r == XW_OK) r = xw_add_string(b, FB_SET_LEVEL);
    if (r == XW_OK) r = xw_add_string(b, FB_SET_INTERFACE_ORIENTATION);
    if (r == XW_OK) r = xw_array_end(b); /* BSKeys */
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FB_SETTINGS_FLAGS);
    if (r == XW_OK) r = fb_add_flag_entry(b, FB_CORE_HASH_FOREGROUND,
                                          foreground ? 1 : 0);
    if (r == XW_OK) r = xw_array_end(b); /* flags */
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FB_SETTINGS_OBJECTS);
    if (r == XW_OK) r = fb_obj_cgrect(b, FB_CORE_HASH_FRAME, 0.0, 0.0,
                                      FB_DISPLAY_WIDTH_PT,
                                      FB_DISPLAY_HEIGHT_PT);
    if (r == XW_OK) r = fb_obj_display_configuration(
        b, FB_CORE_HASH_DISPLAY_CONFIG);
    if (r == XW_OK) r = fb_obj_num_double(b, FB_CORE_HASH_LEVEL,
                                          FB_SETTINGS_LEVEL);
    if (r == XW_OK) r = fb_obj_num_int64(b, FB_CORE_HASH_INTERFACE_ORIENTATION,
                                         FB_ORIENTATION_PORTRAIT);
    if (r == XW_OK) r = xw_array_end(b); /* objects */
    if (r == XW_OK) r = xw_dict_end(b);   /* _changes */
    if (r == XW_OK) r = xw_dict_end(b);   /* mainDiff */
    /* otherDiff -> BSSettingsDiff -> _changes -> BSSettings */
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_DIFF_OTHER);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_SETTINGS_DIFF);
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FB_DIFF_CHANGES);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_SETTINGS);
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FB_SETTINGS_FLAGS);
    if (r == XW_OK) r = fb_add_flag_entry(b, FB_UI_SET_UNDER_LOCK, 0);
    if (r == XW_OK) r = fb_add_flag_entry(b, FB_UI_SET_DEV_ORIENT_EVENTS, 1);
    if (r == XW_OK) r = fb_add_flag_entry(b, FB_UI_SET_STATUS_BAR_DISABLED, 0);
    if (r == XW_OK) r = fb_add_flag_entry(b, FB_UI_SET_ENHANCED_WINDOWING, 0);
    if (r == XW_OK) r = xw_array_end(b); /* flags */
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FB_SETTINGS_OBJECTS);
    if (r == XW_OK) r = fb_obj_num_int64(b, FB_UI_SET_DEACTIVATION_REASONS, 0);
    if (r == XW_OK) r = fb_obj_num_int64(b, FB_UI_SET_USER_INTERFACE_STYLE, 1);
    if (r == XW_OK) r = fb_obj_num_int64(b, FB_UI_SET_DEVICE_ORIENTATION,
                                         FB_ORIENTATION_PORTRAIT);
    if (r == XW_OK) r = fb_obj_num_int64(b, FB_UI_SET_INTERFACE_ORIENT_MODE, 1);
    if (r == XW_OK) r = xw_array_end(b); /* objects */
    if (r == XW_OK) r = xw_dict_end(b);   /* _changes */
    if (r == XW_OK) r = xw_dict_end(b);   /* otherDiff */
    if (r == XW_OK) r = xw_dict_end(b);   /* key (FBSSceneSettingsDiff) */
    return r;
}

static int fb_build_scene_identity_arg(xw_builder *b, const char *key) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_SCENE_IDENTITY);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_ID_KEY_I, g_fb_scene_id);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_ID_KEY_W,
                                           FB_DEFAULT_WORKSPACE_NAME);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_ID_KEY_IW, g_fb_scene_id);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

static int fb_build_transition_ctx_arg(xw_builder *b, const char *key) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSX_KEY_CLASS,
                                           FB_CLS_TRANSITION_CONTEXT);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

/* Minimal {"error": code} reply body. 40 bytes, same shape as the
 * svchost error dict. */
static int fb_build_error_body(uint8_t *body, uint32_t cap, int64_t code,
                               uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, body, cap);
    if (r == XW_OK) r = xw_dict_begin(&b);
    if (r == XW_OK) r = xw_dict_add_int64(&b, FB_ENV_ERROR, code);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Acknowledgement dict: the completion reply shape when the request
 * carried a CID (CID echo, CIDr=true via fb_reply_begin), else a
 * plain {"error": 0}, plus the scene identifier when include_scene
 * is set and a scene id exists (it is empty before the first create,
 * in which case the key is omitted). */
static int fb_build_ack(xw_builder *reply, const bsxpc_message *m,
                        int include_scene) {
    int r = fb_reply_begin(reply, m);
    if (r == XW_OK) r = xw_dict_add_int64(reply, FB_ENV_ERROR, 0);
    if (r == XW_OK && include_scene && g_fb_scene_id[0] != 0)
        r = xw_dict_add_string(reply, FB_ENV_SCENE_IDENTIFIER, g_fb_scene_id);
    if (r == XW_OK) r = xw_dict_end(reply);
    return (r == XW_OK) ? XW_OK : SVCHOST_ERR_NO_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Outbound callout bodies (XPC body only; the Mach header is filled   */
/* at dequeue time by fakeboard_next_callout).                         */
/* ------------------------------------------------------------------ */

/* Open a server->client BSXPC invocation dict.
 * Per -[BSXPCServiceConnection _connection_handleEvent:] [0x19ba9b928]
 * and _connection_handleMessage: [0x19ba9ccd0]:
 *   - "bsxpc" MUST be omitted (any non-nil kind other than connect/
 *     invalidate/interrupt drops the message).
 *   - "bsxpc_CID" MUST match the client's child connection CID and
 *     "bsxpc_CIDr" MUST be true so it routes through
 *     childContextWithRemoteIdentifier:.
 *   - Positional args "1", "2", "3", ... live at the dict root
 *     (BSXPCServiceConnectionProxy decodeArguments [0x19ba9fe08]). */
static int fb_callout_begin(xw_builder *b, const char *sel, uint64_t cid) {
    int r = xw_dict_begin(b);
    if (r == XW_OK) r = xw_dict_add_string(b, FB_BSXPC_KEY_SEL, sel);
    if (r == XW_OK) r = xw_dict_add_uint64(b, FB_BSXPC_KEY_CID, cid);
    if (r == XW_OK) r = xw_dict_add_bool(b, FB_BSXPC_KEY_CIDR, 1);
    return r;
}

/* Scene-create callout:
 * -[FBSWorkspaceScenesClient createSceneWithIdentity:parameters:transitionContext:completion:]
 * [src: 0x1a475d6f0]. Positional args "1".."3". */
static int fb_build_create_body(uint8_t *buf, uint32_t cap,
                                uint32_t *out_len) {
    xw_builder b;
    int r = xw_builder_init(&b, buf, cap);
    if (r == XW_OK) r = fb_callout_begin(&b, FB_MSG_CREATE_SCENE,
                                          g_fb_workspace_cid);
    if (r == XW_OK) r = fb_build_scene_identity_arg(&b, FB_BSXPC_ARG_1);
    /* "2": FBSSceneParameters ("specification" is a raw xpc_string). */
    if (r == XW_OK) r = xw_dict_add_dict_begin(&b, FB_BSXPC_ARG_2);
    if (r == XW_OK) r = xw_dict_add_string(&b, FB_BSX_KEY_CLASS,
                                           FB_CLS_SCENE_PARAMETERS);
    if (r == XW_OK) r = xw_dict_add_string(&b, FB_ENV_SPECIFICATION,
                                           FB_SPEC_UIAPPLICATION_SCENE);
    if (r == XW_OK) r = fb_build_scene_settings_diff(&b, FB_ENV_SETTINGS, 1);
    if (r == XW_OK) r = xw_dict_end(&b); /* "2" */
    if (r == XW_OK) r = fb_build_transition_ctx_arg(&b, FB_BSXPC_ARG_3);
    if (r == XW_OK) r = xw_dict_end(&b); /* root */
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Settings-update callout:
 * -[FBSWorkspaceScenesClient sceneID:updateWithSettingsDiff:transitionContext:completion:]
 * [src: 0x1eca1c9c8]. Positional args "1".."3". */
static int fb_build_update_body(uint8_t *buf, uint32_t cap,
                                uint32_t *out_len, int foreground) {
    xw_builder b;
    int r = xw_builder_init(&b, buf, cap);
    if (r == XW_OK) r = fb_callout_begin(&b, FB_MSG_UPDATE_SETTINGS_DIFF,
                                          g_fb_workspace_cid);
    if (r == XW_OK) r = fb_build_scene_identity_arg(&b, FB_BSXPC_ARG_1);
    if (r == XW_OK) r = fb_build_scene_settings_diff(&b, FB_BSXPC_ARG_2,
                                                     foreground);
    if (r == XW_OK) r = fb_build_transition_ctx_arg(&b, FB_BSXPC_ARG_3);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* Lifecycle-action callout body: a bare BSAction dictionary
 * {kind, target, timeout}, built by bsxpc_action_build and closed
 * with xw_dict_end. It is bare on purpose: the real
 * bsxpc_action_build opens a root dict and leaves it open for the
 * optional info block, so it cannot be nested inside a message
 * envelope. The action kind carries the message identity instead of
 * an envelope message name. Whether real lifecycle actions travel
 * bare or enveloped is [guess]; this body is deliberately left bare. */
static int fb_build_action_body(uint8_t *buf, uint32_t cap,
                                uint32_t *out_len, const char *kind) {
    xw_builder b;
    int r = xw_builder_init(&b, buf, cap);
    if (r == XW_OK)
        r = bsxpc_action_build(&b, &FB_ACTION_KEYS, kind, g_fb_scene_id, 0);
    if (r == XW_OK) r = xw_dict_end(&b);
    if (r != XW_OK) return r;
    *out_len = xw_builder_len(&b);
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Scene lifecycle internals.                                          */
/* ------------------------------------------------------------------ */

/* Queue the create callout for bundle_id and move to CREATING. The
 * bundle id, scene id and persistence id are remembered for later
 * requests that do not carry one. Returns XW_OK, KERN_ALREADY_IN_SET
 * when a scene is already creating or foreground, or an XW_ERR_*. */
static int fb_do_create(const char *bundle_id) {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len = 0;
    int r;
    if (g_fb_state == FB_SCENE_CREATING ||
        g_fb_state == FB_SCENE_FOREGROUND) {
        return KERN_ALREADY_IN_SET;
    }
    fb_copy_str(g_fb_bundle_id, sizeof(g_fb_bundle_id), bundle_id);
    if (fb_build_scene_id(bundle_id) != 0) return XW_ERR_NOSPACE;
    fb_uuid_next(g_fb_persistence_id, sizeof(g_fb_persistence_id));
    r = fb_build_create_body(body, sizeof(body), &body_len);
    if (r != XW_OK) return r;
    r = fb_queue_callout(body, body_len, XW_MSGH_ID_XPC);
    if (r != XW_OK) return r;
    g_fb_state = FB_SCENE_CREATING;
    return XW_OK;
}

/* ------------------------------------------------------------------ */
/* Endpoint discovery.                                                 */
/* ------------------------------------------------------------------ */

/* Scan one dict value for a mach_send port to use as the client
 * callout endpoint. Preferred keys first (endpoint, connection,
 * clientEndpoint, clientPort), then any mach_send value in the dict.
 * The descriptor obj is the port name in the receiver's namespace.
 * The descriptor index from xw_as_mach_send is an index into the
 * reader's shared descs array, so r->descs is used for both the root
 * and a nested dict. Updates the endpoint whenever one is found
 * (latest wins) and clears the one-shot flag: a value carried in the
 * message is a persistent send right. Returns 1 when an endpoint
 * was recorded. */
static int fb_scan_dict_value_for_endpoint(xw_reader *r,
                                           const xw_value *dict) {
    const char *keys[4] = {
        FB_KEY_ENDPOINT, FB_KEY_CONNECTION,
        FB_KEY_CLIENT_ENDPOINT, FB_KEY_CLIENT_PORT
    };
    xw_value v;
    uint32_t desc;
    int i;
    int rc;
    for (i = 0; i < 4; i++) {
        if (xw_dict_find_v(dict, keys[i], &v) == XW_OK &&
            v.type == XW_TYPE_MACH_SEND &&
            xw_as_mach_send(&v, &desc) == XW_OK && desc < r->ndescs &&
            r->descs[desc].obj != 0) {
            g_fb_client_endpoint = (uint32_t)r->descs[desc].obj;
            g_fb_endpoint_one_shot = 0;
            return 1;
        }
    }
    {
        xw_dict_iter it;
        const char *key;
        xw_value val;
        if (xw_dict_iter_init_v(dict, &it) != XW_OK) return 0;
        while ((rc = xw_dict_next(&it, &key, &val)) == 1) {
            if (val.type == XW_TYPE_MACH_SEND &&
                xw_as_mach_send(&val, &desc) == XW_OK &&
                desc < r->ndescs && r->descs[desc].obj != 0) {
                g_fb_client_endpoint = (uint32_t)r->descs[desc].obj;
                g_fb_endpoint_one_shot = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* Endpoint rule 1: a mach_send port value in the message. Scan the
 * root dict first, then the bsxpc_context dict (the parameters moved
 * there from the root in the real envelope). Returns 1 when an
 * endpoint was recorded. */
static int fb_learn_endpoint_from_reader(xw_reader *r,
                                         const bsxpc_message *m) {
    xw_value root;
    if (xw_reader_root(r, &root) == XW_OK) {
        if (fb_scan_dict_value_for_endpoint(r, &root)) return 1;
    }
    if (m != NULL && m->has_ctx) {
        if (fb_scan_dict_value_for_endpoint(r, &m->ctx)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Incoming-message dispatch core.                                     */
/*                                                                     */
/* Returns XW_OK when the reply dict was built in the builder, else   */
/* an error code the caller puts into {"error": code}. When           */
/* ctx->no_reply is set, the return value and the builder content    */
/* are ignored: the caller sends no Mach reply at all. Never leaves   */
/* the caller without a reply dict unless no_reply is set: every     */
/* other path either builds one or returns a code.                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int service_index;   /* index into g_fb_services, -1 if unknown */
    uint32_t reply_port; /* raw header msgh_local_port, 0 if none */
    int have_reply_port; /* the raw header carried a reply port */
    uint32_t msgh_id;    /* raw header msgh_id, when have_msgh_id */
    int have_msgh_id;    /* the caller supplied a msgh_id */
    int no_reply;        /* OUT: the msgh_id rule says send no reply */
} fb_req_ctx;

/* Extract a bundle id from an identity dict value: the identifier
 * is "sceneID:<bundle>-default"; strip the prefix and suffix and
 * copy the middle into out. Returns 1 on success, 0 otherwise. */
static int fb_bundle_from_identity(const xw_value *identity, char *out,
                                   uint32_t cap) {
    xw_value idv;
    const char *ids = NULL;
    uint32_t idlen = 0;
    uint32_t pre = fb_strlen(FB_SCENE_ID_PREFIX);
    uint32_t suf = fb_strlen(FB_SCENE_ID_SUFFIX);
    uint32_t mid;
    uint32_t i;
    if (xw_dict_find_v(identity, FB_IDENTITY_IDENTIFIER, &idv) != XW_OK)
        return 0;
    if (xw_as_string(&idv, &ids, &idlen) != XW_OK || ids == NULL) return 0;
    if (fb_strncmp(ids, FB_SCENE_ID_PREFIX, pre) != 0) return 0;
    if (idlen <= pre + suf + 1) return 0;
    if (fb_strcmp(ids + idlen - suf - 1, FB_SCENE_ID_SUFFIX) != 0) return 0;
    /* Copy the middle: identifier minus prefix and suffix. idlen
     * counts the NUL, so the middle is idlen - pre - suf - 1 bytes. */
    mid = idlen - pre - suf - 1;
    if (cap == 0) return 0;
    i = 0;
    while (i < mid && i + 1 < cap) {
        out[i] = ids[pre + i];
        i++;
    }
    out[i] = 0;
    return 1;
}

/* Bundle id for a create request: "bundleID" key (in the context
 * first, then the root), else the identifier inside the "identity"
 * dict (context, then root) with the prefix and suffix stripped,
 * else the remembered bundle id, else the default. Writes into out
 * (always NUL terminated). */
static void fb_extract_bundle_id(const bsxpc_message *m, xw_reader *msg,
                                 char *out, uint32_t cap) {
    xw_value v;
    const char *s = NULL;
    uint32_t len = 0;
    if (m != NULL && m->has_ctx &&
        xw_dict_find_v(&m->ctx, FB_ENV_BUNDLE_ID, &v) == XW_OK &&
        xw_as_string(&v, &s, &len) == XW_OK && s != NULL) {
        fb_copy_str(out, cap, s);
        return;
    }
    if (xw_dict_find(msg, FB_ENV_BUNDLE_ID, &v) == XW_OK &&
        xw_as_string(&v, &s, &len) == XW_OK && s != NULL) {
        fb_copy_str(out, cap, s);
        return;
    }
    if (m != NULL && m->has_ctx &&
        xw_dict_find_v(&m->ctx, FB_ENV_IDENTITY, &v) == XW_OK &&
        fb_bundle_from_identity(&v, out, cap)) {
        return;
    }
    if (xw_dict_find(msg, FB_ENV_IDENTITY, &v) == XW_OK &&
        fb_bundle_from_identity(&v, out, cap)) {
        return;
    }
    if (g_fb_bundle_id[0] != 0) {
        fb_copy_str(out, cap, g_fb_bundle_id);
        return;
    }
    fb_copy_str(out, cap, FB_DEFAULT_BUNDLE_ID);
}

/* initializeClientWithParameters:completion: (device 2026-11-12).
 * The connection handshake method: the client passes its parameters
 * in bsxpc_context and takes a completion block, so it expects a
 * reply (the message arrives
 * with msgh_id 0x40000000). The parameters are accepted, not
 * validated, in v1; the reply is the completion (CID echo, CIDr=true
 * via fb_reply_begin, plus error 0). The parameter keys are not in
 * the names table yet, so no parameter is read here. */
static int fb_handle_initialize(const bsxpc_message *m, xw_builder *reply) {
    int r = fb_reply_begin(reply, m);
    if (r == XW_OK) r = xw_dict_add_int64(reply, FB_ENV_ERROR, 0);
    if (r == XW_OK) r = xw_dict_end(reply);
    return (r == XW_OK) ? XW_OK : SVCHOST_ERR_NO_MEMORY;
}

/* Scene-create request: queue the create callout (unless a scene is
 * already creating or foreground, in which case just ack with the
 * existing scene id) and reply {"error": 0, "sceneIdentifier": ...}
 * (with the CID echo when the request carried one). */
static int fb_handle_create_request(const bsxpc_message *m, xw_reader *msg,
                                    xw_builder *reply) {
    char bundle[128];
    int r;
    fb_extract_bundle_id(m, msg, bundle, sizeof(bundle));
    if (g_fb_state != FB_SCENE_CREATING &&
        g_fb_state != FB_SCENE_FOREGROUND) {
        r = fb_do_create(bundle);
        if (r != XW_OK) {
            if (r == KERN_ALREADY_IN_SET) return SVCHOST_ERR_UNKNOWN_MESSAGE;
            return SVCHOST_ERR_NO_MEMORY;
        }
    }
    return fb_build_ack(reply, m, 1);
}

/* FBSSystemService messages. pidForApplication and
 * processHandleForApplication are answered from the self handle;
 * openApplication / terminateApplication are not implemented in v1.
 * sel is the bsxpc_SEL string, already matched by the caller. */
static int fb_handle_system_service(const bsxpc_message *m, xw_reader *msg,
                                    xw_builder *reply, const char *sel) {
    (void)msg;
    if (fb_streq(sel, FB_MSG_PID_FOR_APPLICATION)) {
        int r = fb_reply_begin(reply, m);
        if (r == XW_OK) r = xw_dict_add_int64(reply, FB_ENV_ERROR, 0);
        if (r == XW_OK)
            r = xw_dict_add_uint64(reply, FB_ENV_PID, g_fb_self_pid);
        if (r == XW_OK) r = xw_dict_end(reply);
        return (r == XW_OK) ? XW_OK : SVCHOST_ERR_NO_MEMORY;
    }
    if (fb_streq(sel, FB_MSG_PROCESS_HANDLE_FOR_APPLICATION)) {
        /* {"error": 0, "processHandle": {...}}. The handle fields
         * are written one by one under the nested dict: the real
         * bsxpc_handle_build writes a complete standalone root
         * dict, so it cannot be nested under a key. The field
         * spellings come from the same FB_HANDLE_KEYS table it
         * uses, so the two shapes stay consistent. */
        bsxpc_process_handle h;
        int r;
        fb_self_handle(&h);
        r = fb_reply_begin(reply, m);
        if (r == XW_OK) r = xw_dict_add_int64(reply, FB_ENV_ERROR, 0);
        if (r == XW_OK)
            r = xw_dict_add_dict_begin(reply, FB_ENV_PROCESS_HANDLE);
        if (r == XW_OK)
            r = xw_dict_add_uint64(reply, FB_HANDLE_KEYS.key_pid, h.pid);
        if (r == XW_OK && h.bundle_id != NULL)
            r = xw_dict_add_string(reply, FB_HANDLE_KEYS.key_bundle_id,
                                   h.bundle_id);
        if (r == XW_OK)
            r = bsxpc_add_audit_token(reply, FB_HANDLE_KEYS.key_audit_token,
                                      &h.audit);
        if (r == XW_OK)
            r = xw_dict_add_bool(reply, FB_HANDLE_KEYS.key_is_running,
                                 h.is_running);
        if (r == XW_OK)
            r = xw_dict_add_bool(reply, FB_HANDLE_KEYS.key_is_foreground,
                                 h.is_foreground);
        if (r == XW_OK)
            r = xw_dict_add_bool(reply, FB_HANDLE_KEYS.key_is_valid,
                                 h.is_valid);
        if (r == XW_OK)
            r = xw_dict_add_bool(reply, FB_HANDLE_KEYS.key_is_application,
                                 h.is_application);
        if (r == XW_OK) r = xw_dict_end(reply); /* processHandle */
        if (r == XW_OK) r = xw_dict_end(reply); /* root */
        return (r == XW_OK) ? XW_OK : SVCHOST_ERR_NO_MEMORY;
    }
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

static int fb_handle_request(xw_reader *msg, xw_builder *reply,
                             fb_req_ctx *ctx) {
    bsxpc_message parsed;
    int pr;
    if (bsxpc_trace_enabled()) (void)bsxpc_log_dict(msg, FB_LOG_PREFIX);
    /* Parse the real envelope. A malformed dict (not a dict at all)
     * is a bad message; a well-formed dict with no BSXPC keys parses
     * fine with all has_* clear, and the shape fallback below
     * handles it. */
    pr = bsxpc_message_parse(msg, &FB_ENV_KEYS, &parsed);
    if (pr != XW_OK) return SVCHOST_ERR_BAD_MESSAGE;
    /* Endpoint rule 1 (mach_send in the message root or context),
     * then rule 2 (reply port as a one-shot channel). */
    fb_learn_endpoint_from_reader(msg, &parsed);
    if (ctx->have_reply_port && g_fb_client_endpoint == MACH_PORT_NULL) {
        g_fb_client_endpoint = ctx->reply_port;
        g_fb_endpoint_one_shot = 1;
    }
    if (ctx->service_index >= 0 && ctx->service_index < FB_NUM_SERVICES)
        g_fb_handshake_done[ctx->service_index] = 1;

    /* THE "connect" REPLY RULE (see
     * fakeboard.h). The incoming msgh_id is the reply-expectation
     * signal: 0x40000000 means a reply is expected, 0x10000000 means
     * the message is one-way and NO reply may be sent, even when it
     * carries a reply port. Replying to a one-way "connect" crashes
     * libxpc in its async-reply hook (the
     * crash follows fakeboard's reply to "connect" arriving on reply
     * port knote 0x2103). "connect" itself arrives
     * with 0x40000000, so the rule does NOT suppress its reply; the
     * rule only bites for genuinely one-way messages. The dispatch
     * below still runs when the rule fires: its side effects (e.g.
     * queueing a scene-create callout) are not a reply, so they must
     * happen. The caller sends no Mach reply when ctx->no_reply is
     * set, and ignores the built reply and the return value. Any
     * other msgh_id falls back to the reply-port rule. The svchost
     * adapter handler never sets have_msgh_id, so the rule cannot
     * fire there (see fakeboard.h). */
    if (ctx->have_msgh_id && ctx->msgh_id == XW_MSGH_ID_XPC) {
        ctx->no_reply = 1;
    }

    /* Dispatch on the bsxpc kind, then the selector. The device
     * message carries both at once: kind "connect" and SEL
     * "initializeClientWithParameters:completion:", so the kind
     * branch and the SEL branch agree on the same handler for it. */
    if (parsed.has_kind && fb_streq(parsed.kind, FB_MSG_CONNECT)) {
        int is_ws = (ctx->service_index == 0);
        if (parsed.has_ctx) {
            xw_value mv, sv;
            const char *svc = 0;
            uint32_t sl = 0;
            if (xw_dict_find_v(&parsed.ctx, FB_CTX_KEY_MESSAGE, &mv) == XW_OK &&
                xw_dict_find_v(&mv, FB_CTX_KEY_SERVICE, &sv) == XW_OK &&
                xw_as_string(&sv, &svc, &sl) == XW_OK && svc &&
                fb_streq(svc, FB_SVC_WORKSPACE_SERVICE)) {
                is_ws = 1;
            }
        }
        if (is_ws && parsed.has_cid && parsed.cid != 0) {
            g_fb_workspace_cid = parsed.cid;
        }
        if (parsed.has_sel &&
            fb_streq(parsed.sel, FB_MSG_INITIALIZE_CLIENT)) {
            return fb_handle_initialize(&parsed, reply);
        }
        /* Connect to the workspace service ("s" in bsxpc_context.message,
         * e.g. with SEL "handshakeWithRemnants:"): record workspace CID
         * and queue the initial scene-create callout. */
        if (is_ws && parsed.has_ctx) {
            xw_value mv, sv;
            const char *svc = 0;
            uint32_t sl = 0;
            if (xw_dict_find_v(&parsed.ctx, FB_CTX_KEY_MESSAGE, &mv) == XW_OK &&
                xw_dict_find_v(&mv, FB_CTX_KEY_SERVICE, &sv) == XW_OK &&
                xw_as_string(&sv, &svc, &sl) == XW_OK && svc &&
                fb_streq(svc, FB_SVC_WORKSPACE_SERVICE)) {
                char bundle[128];
                fb_extract_bundle_id(&parsed, msg, bundle, sizeof(bundle));
                {
                    int cr;
                    const char *pre = FB_TRACE_WS_CONN_PRE;
                    char m[96];
                    unsigned n = fb_strlen(pre);
                    if (n > sizeof(m) - 16) n = sizeof(m) - 16;
                    fb_memcpy((uint8_t *)m, (const uint8_t *)pre, n);
                    if (g_fb_sas_endpoint != MACH_PORT_NULL) {
                        g_fb_client_endpoint = g_fb_sas_endpoint;
                        g_fb_endpoint_one_shot = 0;
                    }
                    cr = fb_do_create(bundle);
                    {
                        int v = cr < 0 ? -cr : cr;
                        char t[12];
                        int k = 0;
                        if (cr < 0) m[n++] = '-';
                        do {
                            t[k++] = (char)('0' + v % 10);
                            v /= 10;
                        } while (v && k < 11);
                        while (k) m[n++] = t[--k];
                    }
                    m[n++] = '\n';
                    (void)sys_write(2, m, n);
                }
            }
        }
        return fb_build_ack(reply, &parsed, 0);
    }
    if (parsed.has_sel) {
        if (fb_streq(parsed.sel, FB_MSG_INITIALIZE_CLIENT)) {
            return fb_handle_initialize(&parsed, reply);
        }
        if (fb_streq(parsed.sel, FB_MSG_HANDSHAKE_WITH_REMNANTS)) {
            if (g_fb_state == FB_SCENE_NONE) {
                char bundle[128];
                fb_extract_bundle_id(&parsed, msg, bundle, sizeof(bundle));
                (void)fb_do_create(bundle);
            }
            return fb_build_ack(reply, &parsed, 0);
        }
        if (fb_streq(parsed.sel, FB_MSG_REQUEST_SCENE_CREATION) ||
            fb_streq(parsed.sel, FB_MSG_CREATE_SCENE_REQUEST) ||
            fb_streq(parsed.sel, FB_MSG_REQUEST_SCENE) ||
            fb_streq(parsed.sel, FB_MSG_REQUEST_SCENE_CREATION_FULL)) {
            return fb_handle_create_request(&parsed, msg, reply);
        }
        if (fb_streq(parsed.sel, FB_MSG_DID_CREATE_SCENE)) {
            /* Client acknowledgement of the create callout. */
            return fb_build_ack(reply, &parsed, 0);
        }
        if (fb_streq(parsed.sel, FB_MSG_UPDATE_SETTINGS)) {
            /* Client pushing its settings to us. Nothing to do but
             * acknowledge; the fake's own settings go out as
             * callouts, not replies. */
            return fb_build_ack(reply, &parsed, 0);
        }
        if (fb_streq(parsed.sel, FB_MSG_PID_FOR_APPLICATION) ||
            fb_streq(parsed.sel, FB_MSG_PROCESS_HANDLE_FOR_APPLICATION) ||
            fb_streq(parsed.sel, FB_MSG_OPEN_APPLICATION) ||
            fb_streq(parsed.sel, FB_MSG_TERMINATE_APPLICATION)) {
            return fb_handle_system_service(&parsed, msg, reply, parsed.sel);
        }
        /* activate / invalidate / deactivate arrive from the client
         * only if our direction guess is wrong; they are server to
         * client messages, so there is nothing to answer with. */
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    }
    /* No kind or SEL: shape-based fallback. A dict carrying identity
     * or parameters (in the context or the root) is a scene-create
     * request in another spelling; a dict with CIDr=true is an
     * acknowledgement of one of our callouts; a dict carrying a
     * bundle id is a handshake. */
    {
        xw_value v;
        if (parsed.has_ctx) {
            if (xw_dict_find_v(&parsed.ctx, FB_ENV_IDENTITY, &v) == XW_OK ||
                xw_dict_find_v(&parsed.ctx, FB_ENV_PARAMETERS, &v) == XW_OK) {
                return fb_handle_create_request(&parsed, msg, reply);
            }
        }
        if (xw_dict_find(msg, FB_ENV_IDENTITY, &v) == XW_OK ||
            xw_dict_find(msg, FB_ENV_PARAMETERS, &v) == XW_OK) {
            return fb_handle_create_request(&parsed, msg, reply);
        }
        if (parsed.has_cidr && parsed.cidr) {
            return fb_build_ack(reply, &parsed, 0);
        }
        if (parsed.has_ctx &&
            xw_dict_find_v(&parsed.ctx, FB_ENV_BUNDLE_ID, &v) == XW_OK) {
            return fb_build_ack(reply, &parsed, 1);
        }
        if (xw_dict_find(msg, FB_ENV_BUNDLE_ID, &v) == XW_OK) {
            return fb_build_ack(reply, &parsed, 1);
        }
    }
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

int fakeboard_init(void) {
    int i;
    if (g_fb_inited) return KERN_SUCCESS;
    if (!g_fb_pid_set) {
        long pid = sys0(SYS_getpid);
        if (pid > 0) g_fb_self_pid = (uint32_t)pid;
        g_fb_pid_set = 1;
    }
    for (i = 0; i < FB_NUM_SERVICES; i++) {
        mach_port_name_t port = MACH_PORT_NULL;
        int kr = svchost_register(g_fb_services[i].name,
                                  fakeboard_svchost_handler,
                                  &g_fb_services[i], &port);
        if (kr != KERN_SUCCESS) return kr;
        g_fb_services[i].port = port;
    }
    g_fb_inited = 1;
    return KERN_SUCCESS;
}

int fakeboard_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx) {
    fb_req_ctx rctx;
    int idx = 0;
    if (ctx != NULL) idx = ((fb_service_t *)ctx)->index;
    rctx.service_index = idx;
    rctx.reply_port = 0;
    rctx.have_reply_port = 0;
    /* No Mach header reaches this handler, so no msgh_id is known
     * and the "connect" msgh_id rule cannot fire here: every message
     * gets a reply when svchost_process_msg sees a reply port. See
     * fakeboard.h; the live-path gap is flagged in the report that
     * accompanies this file. */
    rctx.msgh_id = 0;
    rctx.have_msgh_id = 0;
    rctx.no_reply = 0;
    return fb_handle_request(msg, reply, &rctx);
}

int fakeboard_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                          uint8_t *reply_buf, uint32_t max_reply_size,
                          uint32_t *out_reply_size) {
    mach_port_name_t reply_port;
    int one_way;
    uint8_t *body;
    uint32_t body_cap;
    int idx = -1;
    int i;
    int64_t err_code = 0;
    uint32_t body_len = 0;
    if (!hdr || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        !out_reply_size) {
        return 0;
    }
    /* The smallest reply is the 40-byte error dict. */
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;
    reply_port = hdr->msgh_local_port;
    one_way = (reply_port == MACH_PORT_NULL);
    body = reply_buf + sizeof(mach_msg_header_t);
    body_cap = max_reply_size - sizeof(mach_msg_header_t);

    /* Dispatch by destination port. */
    for (i = 0; i < FB_NUM_SERVICES; i++) {
        if (g_fb_services[i].port != MACH_PORT_NULL &&
            g_fb_services[i].port == hdr->msgh_remote_port) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Not one of our ports. Same code launchd uses for an
         * unknown service. */
        err_code = 124;
    } else {
        xw_envelope env;
        xw_reader rd;
        if (xw_envelope_parse(hdr, send_size, &env) != XW_OK ||
            xw_reader_init(&rd, env.payload, env.payload_len,
                           env.descs, env.ndescs) != XW_OK) {
            err_code = SVCHOST_ERR_BAD_MESSAGE;
        } else {
            xw_builder b;
            if (xw_builder_init(&b, body, body_cap) != XW_OK) {
                err_code = SVCHOST_ERR_NO_MEMORY;
            } else {
                fb_req_ctx ctx;
                int hr;
                ctx.service_index = idx;
                ctx.reply_port = reply_port;
                ctx.have_reply_port = !one_way;
                /* Pass the msgh_id so fb_handle_request can enforce
                 * the "connect" reply rule (see there). */
                ctx.msgh_id = hdr->msgh_id;
                ctx.have_msgh_id = 1;
                ctx.no_reply = 0;
                hr = fb_handle_request(&rd, &b, &ctx);
                if (ctx.no_reply) {
                    /* The msgh_id rule fired: this message is
                     * one-way, so no Mach reply is sent at all,
                     * even though a reply port exists and a reply
                     * dict may have been built. The dispatch side
                     * effects (endpoint learning, queued callouts)
                     * stand. */
                    *out_reply_size = 0;
                    return 1;
                }
                if (hr == 0) {
                    body_len = xw_builder_len(&b);
                } else {
                    err_code = hr;
                }
            }
        }
    }
    /* One-way message (msgh_id 0x10000000): no reply at all, even
     * when the message failed to parse and the dispatch (with its
     * ctx.no_reply rule) never ran, so no error reply may be built
     * for it either. Replying to a one-way message is what crashes
     * libxpc in its async-reply hook (lsk-notes section 18). The
     * dispatch's ctx.no_reply already covers the parse-success case;
     * this guard covers the parse-failure, unknown-port and
     * builder-failure paths, which reach here without it. */
    if (hdr->msgh_id == XW_MSGH_ID_XPC) {
        *out_reply_size = 0;
        return 1;
    }
    if (err_code != 0) {
        if (fb_build_error_body(body, body_cap, err_code, &body_len) != 0) {
            return 0;
        }
    }
    if (one_way) {
        *out_reply_size = 0;
        return 1;
    }
    {
        /* Reply header: msgh_id
         * 0x20000000, bits remote MOVE_SEND_ONCE / local 0 / voucher
         * NULL, remote = the request's reply port. The trailer (CTX
         * msgh_context, seqno, audit pid 1 / euid 0) is ipc.c's
         * domain when it enqueues this reply, not fakeboard.c's. */
        mach_msg_header_t *rep = (mach_msg_header_t *)reply_buf;
        rep->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        rep->msgh_size = sizeof(mach_msg_header_t) + body_len;
        rep->msgh_remote_port = reply_port;
        rep->msgh_local_port = MACH_PORT_NULL;
        rep->msgh_voucher_port = MACH_PORT_NULL;
        rep->msgh_id = SVCHOST_REPLY_MSGH_ID;
    }
    *out_reply_size = sizeof(mach_msg_header_t) + body_len;
    return 1;
}

int fakeboard_next_callout(uint8_t *buf, uint32_t cap, uint32_t *out_len,
                           uint32_t *out_msgh_id) {
    fb_callout_t *c;
    uint32_t total;
    mach_msg_header_t *hdr;
    if (!buf || !out_len || !out_msgh_id) return XW_ERR_BAD_ENVELOPE;
    if (g_fb_q_count == 0) return 0;
    /* No endpoint yet: callouts stay queued. */
    if (g_fb_client_endpoint == MACH_PORT_NULL) {
        static int warned;
        if (!warned++) {
            const char *msg = FB_TRACE_NO_EP;
            (void)sys_write(2, msg, fb_strlen(msg));
        }
        return 0;
    }
    c = &g_fb_queue[g_fb_q_head];
    total = sizeof(mach_msg_header_t) + c->body_len;
    if (cap < total) return XW_ERR_NOSPACE;
    hdr = (mach_msg_header_t *)buf;
    hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0); /* endpoint is the client's receive right, same task */
    hdr->msgh_size = total;
    hdr->msgh_remote_port = g_fb_client_endpoint;
    hdr->msgh_local_port = MACH_PORT_NULL; /* completion reply not needed */
    hdr->msgh_voucher_port = MACH_PORT_NULL;
    hdr->msgh_id = c->msgh_id;
    fb_memcpy(buf + sizeof(mach_msg_header_t), c->body, c->body_len);
    *out_len = total;
    *out_msgh_id = c->msgh_id;
    g_fb_q_head = (g_fb_q_head + 1) % FB_CALLOUT_QUEUE_LEN;
    g_fb_q_count--;
    if (g_fb_endpoint_one_shot) {
        g_fb_client_endpoint = MACH_PORT_NULL;
        g_fb_endpoint_one_shot = 0;
    }
    return 1;
}

/* Called by ipc.c on a libxpc "w00t" checkin to one of our services: endpoint is the
 * client's MOVE_SEND reply endpoint (second port descriptor), the channel FrontBoard uses
 * for server-to-client invocations on that connection. Only the workspace service's
 * endpoint is kept (scene callouts go there). */
void fakeboard_note_w00t(uint32_t service_port, uint32_t endpoint) {
    for (int i = 0; i < FB_NUM_SERVICES; i++) {
        if (g_fb_services[i].port == service_port && g_fb_services[i].port != MACH_PORT_NULL &&
            fb_streq(g_fb_services[i].name, FB_SVC_SYSTEMAPP_SERVICES)) {
            g_fb_sas_endpoint = endpoint;
            if (g_fb_q_count > 0 && g_fb_state == FB_SCENE_CREATING) {
                g_fb_client_endpoint = endpoint;
                g_fb_endpoint_one_shot = 0;
            }
        }
        if (g_fb_services[i].port == service_port && g_fb_services[i].port != MACH_PORT_NULL &&
            fb_streq(g_fb_services[i].name, FB_SVC_WORKSPACE_SERVICE) && endpoint != MACH_PORT_NULL) {
            const char *msg = FB_TRACE_WS_EP;
            g_fb_client_endpoint = endpoint;
            g_fb_endpoint_one_shot = 0;
            (void)sys_write(2, msg, fb_strlen(msg));
        }
    }
}

int fakeboard_scene_create(const char *bundle_id) {
    char bundle[128];
    if (bundle_id != NULL && bundle_id[0] != 0) {
        fb_copy_str(bundle, sizeof(bundle), bundle_id);
    } else if (g_fb_bundle_id[0] != 0) {
        fb_copy_str(bundle, sizeof(bundle), g_fb_bundle_id);
    } else {
        fb_copy_str(bundle, sizeof(bundle), FB_DEFAULT_BUNDLE_ID);
    }
    return fb_xw_to_kern(fb_do_create(bundle));
}

int fakeboard_scene_update_settings(void) {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len = 0;
    int r;
    if (g_fb_state != FB_SCENE_CREATING &&
        g_fb_state != FB_SCENE_FOREGROUND &&
        g_fb_state != FB_SCENE_BACKGROUND) {
        return KERN_INVALID_VALUE;
    }
    r = fb_build_update_body(body, sizeof(body), &body_len, 1);
    if (r != XW_OK) return fb_xw_to_kern(r);
    r = fb_queue_callout(body, body_len, XW_MSGH_ID_XPC);
    return fb_xw_to_kern(r);
}

int fakeboard_scene_activate(void) {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len = 0;
    int r;
    if (g_fb_state == FB_SCENE_FOREGROUND) return KERN_SUCCESS;
    if (g_fb_state != FB_SCENE_CREATING &&
        g_fb_state != FB_SCENE_BACKGROUND) {
        return KERN_INVALID_VALUE;
    }
    r = fb_build_action_body(body, sizeof(body), &body_len,
                             FB_ACTION_ACTIVATE);
    if (r != XW_OK) return fb_xw_to_kern(r);
    r = fb_queue_callout(body, body_len, XW_MSGH_ID_XPC);
    if (r != XW_OK) return fb_xw_to_kern(r);
    g_fb_state = FB_SCENE_FOREGROUND;
    return KERN_SUCCESS;
}

int fakeboard_scene_background(void) {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len = 0;
    int r;
    if (g_fb_state != FB_SCENE_FOREGROUND) return KERN_INVALID_VALUE;
    r = fb_build_update_body(body, sizeof(body), &body_len, 0);
    if (r != XW_OK) return fb_xw_to_kern(r);
    r = fb_queue_callout(body, body_len, XW_MSGH_ID_XPC);
    if (r != XW_OK) return fb_xw_to_kern(r);
    g_fb_state = FB_SCENE_BACKGROUND;
    return KERN_SUCCESS;
}

int fakeboard_scene_invalidate(void) {
    uint8_t body[FB_CALLOUT_MAX_BODY];
    uint32_t body_len = 0;
    int r;
    if (g_fb_state == FB_SCENE_NONE) return KERN_INVALID_VALUE;
    if (g_fb_state == FB_SCENE_INVALIDATED) return KERN_SUCCESS;
    r = fb_build_action_body(body, sizeof(body), &body_len,
                             FB_ACTION_DESTROY);
    if (r != XW_OK) return fb_xw_to_kern(r);
    r = fb_queue_callout(body, body_len, XW_MSGH_ID_XPC);
    if (r != XW_OK) return fb_xw_to_kern(r);
    g_fb_state = FB_SCENE_INVALIDATED;
    return KERN_SUCCESS;
}

int fakeboard_scene_state(void) {
    return g_fb_state;
}

int fakeboard_is_inited(void) {
    return g_fb_inited;
}

void fakeboard_set_self_pid(uint32_t pid) {
    g_fb_self_pid = pid;
    g_fb_pid_set = 1;
}
